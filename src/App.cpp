#include "AppComponent.hpp"
#include "DatabaseComponent.hpp"
#include "GStreamerComponent.hpp"
#include "SwaggerComponent.hpp"
#include "WebSocketComponent.hpp"
#include "config/ConfigComponent.hpp"
#include "controller/CameraController.hpp"
#include "controller/AiJobController.hpp"
#include "controller/ImageInferenceController.hpp"
#include "controller/WebSocketController.hpp"
#include "controller/WebRtcController.hpp"
#include "controller/PlaybackController.hpp"
#include "controller/MoqController.hpp"
#include "AiComponent.hpp"

#include "oatpp-swagger/Controller.hpp"
#include "oatpp/network/Server.hpp"

#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <algorithm>

namespace {
std::shared_ptr<oatpp::network::Server> g_server;
std::atomic<bool> g_shutdown{false};

void onSignal(int) {
    g_shutdown.store(true);
    if (g_server) g_server->stop();
}

// Đo thời lượng THẬT (ms) của một file media bằng GstDiscoverer; -1 nếu không
// đọc được. Dùng để finalize segment mồ côi: file bị cắt giữa chừng khi tắt đột
// ngột thường ngắn hơn nhiều so với ước lượng — ghi ước lượng vào DB tạo "đuôi
// ma" làm trình phát chờ dữ liệu không tồn tại (xoay vô hạn).
int32_t probeMediaDurationMs(const std::string& absPath) {
    GError* err = nullptr;
    GstDiscoverer* discoverer = gst_discoverer_new(10 * GST_SECOND, &err);
    if (!discoverer) {
        if (err) g_error_free(err);
        return -1;
    }
    int32_t ms = -1;
    if (gchar* uri = g_filename_to_uri(absPath.c_str(), nullptr, nullptr)) {
        GstDiscovererInfo* info = gst_discoverer_discover_uri(discoverer, uri, &err);
        g_free(uri);
        if (info) {
            if (gst_discoverer_info_get_result(info) == GST_DISCOVERER_OK) {
                const GstClockTime duration = gst_discoverer_info_get_duration(info);
                if (GST_CLOCK_TIME_IS_VALID(duration) && duration > 0) {
                    ms = static_cast<int32_t>(duration / GST_MSECOND);
                }
            }
            g_object_unref(info);
        }
    }
    if (err) g_error_free(err);
    g_object_unref(discoverer);
    return ms;
}

std::string resolveConfigPath(int argc, char* argv[]) {
    if (argc > 1) return argv[1];
    if (const char* env = std::getenv("CONFIG_PATH")) return env;
    return "config/config.json";
}

// Sleep for delayMs, but wake early if shutdown is requested so the process
// can exit promptly instead of blocking on a long backoff.
void interruptibleSleep(int delayMs) {
    constexpr int kSliceMs = 100;
    for (int slept = 0; slept < delayMs && !g_shutdown.load(); slept += kSliceMs) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::min(kSliceMs, delayMs - slept)));
    }
}

// Retry a startup step that depends on the database. Postgres (often a Docker
// container) may not be ready when this process starts, in which case oatpp
// throws std::runtime_error("...Can't connect.") from getConnection() -- and it
// may also restart later. We keep retrying with capped backoff until the step
// succeeds (or shutdown is requested), so the load eventually happens once the
// database is reachable instead of giving up after a fixed window. Returns true
// on success, false if we stopped because shutdown was requested.
template <typename Fn>
bool runDbStartupStep(const char* what, Fn&& step) {
    constexpr int kInitialMs = 1000;
    constexpr int kMaxMs     = 5000;
    int delayMs = kInitialMs;
    for (int attempt = 1; !g_shutdown.load(); ++attempt) {
        try {
            step();
            if (attempt > 1) {
                std::cerr << "[startup] " << what << " succeeded on attempt "
                          << attempt << "." << std::endl;
            }
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[startup] " << what << " failed (attempt " << attempt
                      << "): " << e.what() << " -- retrying in " << delayMs
                      << "ms (is PostgreSQL up?)" << std::endl;
            interruptibleSleep(delayMs);
            delayMs = std::min(delayMs * 2, kMaxMs);
        }
    }
    return false;
}
}

void run(const std::string& configPath) {
    // Zero-copy cho transcode H265->H264 (nhánh xem live của trình duyệt không
    // nhận HEVC). Mặc định mpph264enc ÉP căn vstride bội 16 (1080 -> 1088);
    // decoder lại nhả đúng 1080 nên strides KHÔNG khớp, encoder rơi vào nhánh
    // "converting to aligned NV12" và CHÉP TỪNG KHUNG bằng CPU
    // (gst_video_frame_copy). Đo tận nơi: 1080p ~5,5MB/khung x 25 khung/s đọc
    // từ vùng nhớ không cache = 21% trong tổng 32% CPU của một transcode.
    //
    // RKVENC của RK3588 KHÔNG cần vstride căn 16 — plugin có sẵn công tắc bỏ
    // ép căn, và khi bỏ thì encoder dùng thẳng bộ đệm dmabuf của decoder
    // ("using imported buffer" cho 545/545 khung, kiểm bằng GST_DEBUG=mppenc:6).
    // Đo: 32% -> 11% CPU mỗi camera, cùng ~25 khung/s, ảnh ra kiểm tra bằng mắt
    // vẫn đúng (không méo/không xanh).
    //
    // Đặt ở đây thay vì trong ecosystem/.env để chạy kiểu nào cũng có hiệu lực;
    // 0 = không ghi đè nếu người dùng đã tự đặt biến này.
    setenv("GST_MPP_ENC_UNALIGNED_VSTRIDE", "1", 0);

    gst_init(nullptr, nullptr);

    ConfigComponent   configComponents(configPath);
    AppComponent      appComponents;
    SwaggerComponent  swaggerComponents;
    DatabaseComponent databaseComponents;
    WebSocketComponent webSocketComponents;
    GStreamerComponent gstreamerComponents;
    AiComponent        aiComponents;

    OATPP_COMPONENT(std::shared_ptr<oatpp::web::server::HttpRouter>, router);
    OATPP_COMPONENT(std::shared_ptr<GStreamerService>, gstreamerService);
    OATPP_COMPONENT(std::shared_ptr<AiManager>, aiManager);

    gstreamerService->start();
    aiManager->start();

    // Nối hai chiều giữa ghi hình và AI cho việc DÒ CHUYỂN ĐỘNG. Nối ở đây,
    // bằng std::function, vì AiManager đã include service/ — để hai bên
    // include lẫn nhau là vòng include.
    //
    //   xuôi : camera bật/tắt chuyển động -> AiManager dựng/bỏ MotionDetector
    //   ngược: mỗi khung đã phân tích     -> CameraRecordingSession xét vùng
    //
    // Chuyển động giờ chạy TRÊN khung của pipeline AI (đã giải mã một lần, đã
    // qua RGA) thay vì một nhánh GStreamer riêng có videoscale bằng CPU — đo
    // được ~25% CPU mỗi camera 1080p ở nhánh cũ.
    {
        std::weak_ptr<AiManager> aiWeak = aiManager;
        gstreamerService->setMotionConfigSink(
            [aiWeak](const stream::CameraRuntimeConfig& camera, bool enabled) {
                auto ai = aiWeak.lock();
                if (!ai) return;
                cfg::Camera c;
                c.id = camera.id;
                c.name = camera.name;
                c.uri = camera.rtsp;
                ai->setCameraMotion(c, enabled,
                                    recording::clampMotionGrid(camera.motionGridX),
                                    recording::clampMotionGrid(camera.motionGridY));
            });

        gstreamerService->setMotionJpegSource(
            [aiWeak](const std::string& cameraId) -> std::vector<uint8_t> {
                auto ai = aiWeak.lock();
                return ai ? ai->grabMotionJpeg(cameraId) : std::vector<uint8_t>{};
            });

        std::weak_ptr<GStreamerService> gsWeak = gstreamerService;
        aiManager->setMotionSink(
            [gsWeak](const std::string& cameraId, const std::string& cells) {
                auto gs = gsWeak.lock();
                if (gs) gs->noteMotionCells(cameraId, cells);
            });
    }

    auto cameraController = std::make_shared<CameraController>();
    router->addController(cameraController);

    auto aiJobController = std::make_shared<AiJobController>();
    router->addController(aiJobController);

    auto imageInferenceController = std::make_shared<ImageInferenceController>();
    router->addController(imageInferenceController);

    auto webSocketController = std::make_shared<WebSocketController>();
    router->addController(webSocketController);

    auto webRtcController = std::make_shared<WebRtcController>();
    router->addController(webRtcController);

    auto playbackController = std::make_shared<PlaybackController>();
    router->addController(playbackController);

    // Duong xem thu hai: MoQ tren QUIC. Chi may chu MoQ (tien trinh Python)
    // goi vao day; trinh duyet noi thang QUIC voi no.
    auto moqController = std::make_shared<MoqController>();
    router->addController(moqController);

    // Load camera streams and AI jobs from the database in the background so a
    // slow or temporarily-unavailable PostgreSQL does not block the HTTP server
    // from starting. runDbStartupStep retries until the database is reachable,
    // so the load self-heals when Postgres comes up (or comes back) later.
    // GStreamerService and AiManager are mutex-guarded, so loading concurrently
    // with live API requests is safe.
    std::thread dbStartupThread([] {
        // Đóng các segment còn 'recording' mồ côi từ lần chạy trước (tắt đột
        // ngột giữa lúc ghi) TRƯỚC khi khởi động stream — nếu không, finalizer
        // sẽ đóng nhầm luôn đoạn mà camera vừa mới mở khi bắt đầu ghi lại.
        // Thời lượng lấy bằng cách ĐO FILE THẬT, không ước lượng (xem
        // probeMediaDurationMs); file thiếu/rỗng/hỏng thì xoá hàng.
        runDbStartupStep("finalize orphan recording segments", [] {
            OATPP_COMPONENT(std::shared_ptr<CameraDb>, cameraDb);
            auto res = cameraDb->listOrphanRecordingSegments();
            auto rows = res
                ? res->fetch<oatpp::Vector<oatpp::Object<OrphanSegmentDto>>>()
                : nullptr;
            if (!rows) return;
            for (const auto& row : *rows) {
                if (!row || !row->id || !row->path) continue;
                const std::string id = row->id->c_str();
                const std::string path = row->path->c_str();
                std::error_code fsError;
                const auto abs = std::filesystem::absolute(path, fsError);
                const bool exists = !fsError &&
                    std::filesystem::exists(abs, fsError) &&
                    std::filesystem::file_size(abs, fsError) > 0;
                const int32_t realMs =
                    exists ? probeMediaDurationMs(abs.string()) : -1;
                if (realMs > 0) {
                    cameraDb->finalizeRecordingSegmentProbed(id.c_str(), realMs);
                    std::cout << "[recording] orphan finalized (" << realMs
                              << "ms do thuc): " << path << std::endl;
                } else {
                    cameraDb->deleteRecordingSegmentById(id.c_str());
                    std::cout << "[recording] orphan bo (file thieu/hong): "
                              << path << std::endl;
                }
            }
        });

        runDbStartupStep("start camera streams from database", [] {
            CameraService startupCameraService;
            startupCameraService.startAllStreamsFromDatabase();
        });

        // Load enabled AI jobs from the database into the live AI subsystem.
        runDbStartupStep("load AI jobs from database", [] {
            AiJobService startupAiJobService;
            startupAiJobService.startAllFromDatabase();
        });
    });

    auto docEndpoints = cameraController->getEndpoints();
    docEndpoints.append(aiJobController->getEndpoints());
    docEndpoints.append(imageInferenceController->getEndpoints());
    docEndpoints.append(webRtcController->getEndpoints());
    docEndpoints.append(playbackController->getEndpoints());
    auto swaggerController = oatpp::swagger::Controller::createShared(docEndpoints);
    router->addController(swaggerController);

    OATPP_COMPONENT(std::shared_ptr<oatpp::network::ServerConnectionProvider>, provider);
    OATPP_COMPONENT(std::shared_ptr<oatpp::network::ConnectionHandler>,        handler, "http");

    g_server = oatpp::network::Server::createShared(provider, handler);

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    std::cout << "Server running on http://"
              << provider->getProperty("host").toString()->c_str()
              << ":"
              << provider->getProperty("port").toString()->c_str()
              << "  (Swagger UI: /swagger/ui)"
              << "  [config: " << configPath << "]" << std::endl;

    g_server->run();
    g_shutdown.store(true);
    if (dbStartupThread.joinable()) dbStartupThread.join();
    aiManager->stop();
    gstreamerService->cleanup();
}

int main(int argc, char* argv[]) {
    const std::string configPath = resolveConfigPath(argc, argv);
    oatpp::base::Environment::init();
    run(configPath);
    oatpp::base::Environment::destroy();
    return 0;
}
