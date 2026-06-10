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

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <algorithm>

namespace {
std::shared_ptr<oatpp::network::Server> g_server;

void onSignal(int) {
    if (g_server) g_server->stop();
}

std::string resolveConfigPath(int argc, char* argv[]) {
    if (argc > 1) return argv[1];
    if (const char* env = std::getenv("CONFIG_PATH")) return env;
    return "config/config.json";
}

// Retry a startup step that depends on the database. Postgres (often a Docker
// container) may not be ready when this process starts, in which case oatpp
// throws std::runtime_error("...Can't connect.") from getConnection(). Without
// this, the exception propagates out of run()/main, std::terminate aborts the
// process and a core dump is written. We instead wait with backoff and retry.
template <typename Fn>
void runDbStartupStep(const char* what, Fn&& step) {
    constexpr int   kMaxAttempts = 30;   // ~ up to ~2 min total with the cap below
    constexpr int   kInitialMs   = 1000;
    constexpr int   kMaxMs       = 5000;
    int delayMs = kInitialMs;
    for (int attempt = 1; ; ++attempt) {
        try {
            step();
            return;
        } catch (const std::exception& e) {
            if (attempt >= kMaxAttempts) {
                std::cerr << "[startup] " << what << " failed after " << attempt
                          << " attempts: " << e.what()
                          << " -- giving up, continuing without it." << std::endl;
                return;
            }
            std::cerr << "[startup] " << what << " failed (attempt " << attempt
                      << "): " << e.what() << " -- retrying in " << delayMs
                      << "ms (is PostgreSQL up?)" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            delayMs = std::min(delayMs * 2, kMaxMs);
        }
    }
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

    runDbStartupStep("start camera streams from database", [] {
        CameraService startupCameraService;
        startupCameraService.startAllStreamsFromDatabase();
    });

    // Load enabled AI jobs from the database into the live AI subsystem.
    runDbStartupStep("load AI jobs from database", [] {
        AiJobService startupAiJobService;
        startupAiJobService.startAllFromDatabase();
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
