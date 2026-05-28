#ifndef test_gstreamer_WebSocketComponent_hpp
#define test_gstreamer_WebSocketComponent_hpp

#include "ws/CameraStateSocket.hpp"

#include "oatpp-websocket/ConnectionHandler.hpp"
#include "oatpp/network/ConnectionHandler.hpp"
#include "oatpp/core/macro/component.hpp"

// Registers the camera-state WebSocket subsystem:
//   * cameraStateRegistry      — shared live socket set + broadcaster
//   * websocketConnectionHandler — the upgrade handler (resolved by the
//     qualifier name "websocket" from WebSocketController)
//
// Must be constructed before GStreamerComponent, whose status sink resolves
// cameraStateRegistry to broadcast state changes.
class WebSocketComponent {
public:
    OATPP_CREATE_COMPONENT(std::shared_ptr<ws::CameraStateRegistry>, cameraStateRegistry)([] {
        return std::make_shared<ws::CameraStateRegistry>();
    }());

    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::network::ConnectionHandler>,
                           websocketConnectionHandler)("websocket", [] {
        OATPP_COMPONENT(std::shared_ptr<ws::CameraStateRegistry>, registry);
        auto handler = oatpp::websocket::ConnectionHandler::createShared();
        handler->setSocketInstanceListener(
            std::make_shared<ws::CameraStateInstanceListener>(registry));
        return handler;
    }());
};

#endif
