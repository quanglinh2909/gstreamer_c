#ifndef test_gstreamer_SwaggerComponent_hpp
#define test_gstreamer_SwaggerComponent_hpp

#include "config/ConfigDto.hpp"

#include "oatpp-swagger/Model.hpp"
#include "oatpp-swagger/Resources.hpp"
#include "oatpp/core/macro/component.hpp"

#include <string>

class SwaggerComponent {
public:
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::swagger::DocumentInfo>, swaggerDocumentInfo)([] {
        OATPP_COMPONENT(oatpp::Object<ConfigDto>, config);

        const auto host = config->server->host;
        const auto port = std::to_string(*config->server->port);
        const auto serverUrl = std::string("http://") + host->c_str() + ":" + port;

        oatpp::swagger::DocumentInfo::Builder builder;
        builder
            .setTitle(config->swagger->title)
            .setDescription(config->swagger->description)
            .setVersion(config->swagger->version)
            .setContactName("dev")
            .setContactUrl("https://example.local/")
            .addServer(serverUrl, "Configured server");
        return builder.build();
    }());

    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::swagger::Resources>, swaggerResources)([] {
        return oatpp::swagger::Resources::loadResources(OATPP_SWAGGER_RES_PATH);
    }());
};

#endif
