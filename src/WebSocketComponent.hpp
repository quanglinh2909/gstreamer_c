#ifndef test_gstreamer_WebSocketComponent_hpp
#define test_gstreamer_WebSocketComponent_hpp

#include "ws/CameraStateSocket.hpp"
#include "ws/MotionEventSocket.hpp"

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

    // Sự kiện chuyển động: registry + handler RIÊNG. Mỗi ConnectionHandler chỉ
    // gắn được một SocketInstanceListener, mà listener là chỗ quyết định socket
    // mới vào registry nào — nên hai luồng dữ liệu phải là hai handler.
    OATPP_CREATE_COMPONENT(std::shared_ptr<ws::MotionEventRegistry>, motionEventRegistry)([] {
        return std::make_shared<ws::MotionEventRegistry>();
    }());

    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::network::ConnectionHandler>,
                           motionWebsocketConnectionHandler)("websocket-motion", [] {
        OATPP_COMPONENT(std::shared_ptr<ws::MotionEventRegistry>, registry);
        auto handler = oatpp::websocket::ConnectionHandler::createShared();
        handler->setSocketInstanceListener(
            std::make_shared<ws::MotionEventInstanceListener>(registry));
        return handler;
    }());
};

#endif
