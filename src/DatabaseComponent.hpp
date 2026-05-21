#ifndef test_gstreamer_DatabaseComponent_hpp
#define test_gstreamer_DatabaseComponent_hpp

#include "config/ConfigDto.hpp"
#include "db/CameraDb.hpp"
#include "db/AiJobDb.hpp"

#include "oatpp-postgresql/ConnectionProvider.hpp"
#include "oatpp-postgresql/Executor.hpp"

#include "oatpp/core/macro/component.hpp"

class DatabaseComponent {
public:
    OATPP_CREATE_COMPONENT(std::shared_ptr<CameraDb>, cameraDb)([] {
        OATPP_COMPONENT(oatpp::Object<ConfigDto>, config);

        auto connectionProvider =
            std::make_shared<oatpp::postgresql::ConnectionProvider>(config->database->url);

        auto connectionPool =
            oatpp::postgresql::ConnectionPool::createShared(
                connectionProvider,
                *config->database->poolMaxConnections,
                std::chrono::seconds(*config->database->poolIdleSeconds));

        auto executor =
            std::make_shared<oatpp::postgresql::Executor>(connectionPool);

        return std::make_shared<CameraDb>(executor);
    }());

    OATPP_CREATE_COMPONENT(std::shared_ptr<AiJobDb>, aiJobDb)([] {
        OATPP_COMPONENT(oatpp::Object<ConfigDto>, config);

        auto connectionProvider =
            std::make_shared<oatpp::postgresql::ConnectionProvider>(config->database->url);

        auto connectionPool =
            oatpp::postgresql::ConnectionPool::createShared(
                connectionProvider,
                *config->database->poolMaxConnections,
                std::chrono::seconds(*config->database->poolIdleSeconds));

        auto executor =
            std::make_shared<oatpp::postgresql::Executor>(connectionPool);

        return std::make_shared<AiJobDb>(executor);
    }());
};

#endif
