#include "AppComponent.hpp"
#include "DatabaseComponent.hpp"
#include "GStreamerComponent.hpp"
#include "SwaggerComponent.hpp"
#include "config/ConfigComponent.hpp"
#include "controller/CameraController.hpp"
#include "controller/AiJobController.hpp"
#include "AiComponent.hpp"

#include "oatpp-swagger/Controller.hpp"
#include "oatpp/network/Server.hpp"

#include <gst/gst.h>

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

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
}

void run(const std::string& configPath) {
    gst_init(nullptr, nullptr);

    ConfigComponent   configComponents(configPath);
    AppComponent      appComponents;
    SwaggerComponent  swaggerComponents;
    DatabaseComponent databaseComponents;
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

    CameraService startupCameraService;
    startupCameraService.startAllStreamsFromDatabase();

    // Load enabled AI jobs from the database into the live AI subsystem.
    AiJobService startupAiJobService;
    startupAiJobService.startAllFromDatabase();

    auto docEndpoints = cameraController->getEndpoints();
    docEndpoints.append(aiJobController->getEndpoints());
    auto swaggerController = oatpp::swagger::Controller::createShared(docEndpoints);
    router->addController(swaggerController);

    OATPP_COMPONENT(std::shared_ptr<oatpp::network::ServerConnectionProvider>, provider);
    OATPP_COMPONENT(std::shared_ptr<oatpp::network::ConnectionHandler>,        handler);

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
