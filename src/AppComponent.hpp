#ifndef test_gstreamer_AppComponent_hpp
#define test_gstreamer_AppComponent_hpp

#include "config/ConfigDto.hpp"

#include "oatpp/web/server/HttpConnectionHandler.hpp"
#include "oatpp/network/tcp/server/ConnectionProvider.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/web/server/HttpRouter.hpp"
#include "oatpp/core/macro/component.hpp"

class AppComponent {
public:
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::network::ServerConnectionProvider>, serverConnectionProvider)([] {
        OATPP_COMPONENT(oatpp::Object<ConfigDto>, config);
        return oatpp::network::tcp::server::ConnectionProvider::createShared(
            { config->server->host,
              static_cast<v_uint16>(*config->server->port),
              oatpp::network::Address::IP_4 });
    }());

    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::web::server::HttpRouter>, httpRouter)([] {
        return oatpp::web::server::HttpRouter::createShared();
    }());

    // Qualified "http": a second ConnectionHandler ("websocket") exists, so an
    // unqualified lookup of this type would be ambiguous.
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::network::ConnectionHandler>, serverConnectionHandler)("http", [] {
        OATPP_COMPONENT(std::shared_ptr<oatpp::web::server::HttpRouter>, router);
        return oatpp::web::server::HttpConnectionHandler::createShared(router);
    }());

    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::data::mapping::ObjectMapper>, apiObjectMapper)([] {
        auto mapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
        mapper->getSerializer()->getConfig()->useBeautifier = true;
        return mapper;
    }());
};

#endif
