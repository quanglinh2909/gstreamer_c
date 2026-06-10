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
#include "AiComponent.hpp"

#include "oatpp-swagger/Controller.hpp"
#include "oatpp/network/Server.hpp"

#include <gst/gst.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
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

    auto cameraController = std::make_shared<CameraController>();
    router->addController(cameraController);

    auto aiJobController = std::make_shared<AiJobController>();
    router->addController(aiJobController);

    auto imageInferenceController = std::make_shared<ImageInferenceController>();
    router->addController(imageInferenceController);

    auto webSocketController = std::make_shared<WebSocketController>();
    router->addController(webSocketController);

    // Load camera streams and AI jobs from the database in the background so a
    // slow or temporarily-unavailable PostgreSQL does not block the HTTP server
    // from starting. runDbStartupStep retries until the database is reachable,
    // so the load self-heals when Postgres comes up (or comes back) later.
    // GStreamerService and AiManager are mutex-guarded, so loading concurrently
    // with live API requests is safe.
    std::thread dbStartupThread([] {
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
