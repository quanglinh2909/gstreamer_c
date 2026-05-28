#ifndef test_gstreamer_WebSocketController_hpp
#define test_gstreamer_WebSocketController_hpp

#include "oatpp-websocket/Handshaker.hpp"

#include "oatpp/web/server/api/ApiController.hpp"
#include "oatpp/network/ConnectionHandler.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/core/macro/component.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

// Streams camera state changes to clients over WebSocket.
//
//   GET /ws/camera-state   (HTTP Upgrade -> WebSocket)
//
// The endpoint only performs the handshake; the connection is then handed to
// the "websocket" ConnectionHandler. Not added to the Swagger docs — Swagger
// cannot describe a WebSocket endpoint.
class WebSocketController : public oatpp::web::server::api::ApiController {
public:
    explicit WebSocketController(
        OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

    ENDPOINT("GET", "/ws/camera-state", wsCameraState,
             REQUEST(std::shared_ptr<IncomingRequest>, request))
    {
        return oatpp::websocket::Handshaker::serversideHandshake(
            request->getHeaders(), m_websocketConnectionHandler);
    }

private:
    OATPP_COMPONENT(std::shared_ptr<oatpp::network::ConnectionHandler>,
                    m_websocketConnectionHandler, "websocket");
};

#include OATPP_CODEGEN_END(ApiController)

#endif
