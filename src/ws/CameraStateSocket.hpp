#ifndef test_gstreamer_CameraStateSocket_hpp
#define test_gstreamer_CameraStateSocket_hpp

// WebSocket fan-out of camera state changes.
//
//   GET /ws/camera-state
//
// Every connected client receives a JSON message whenever a camera's runtime
// state actually changes:
//   {"id":"<uuid>","state":"online|offline|error",
//    "lastError":"...","lastChangedAt":"<iso8601>"}
//
// CameraStateRegistry holds the live socket set and is driven by the GStreamer
// status sink (see GStreamerComponent). It is server->client only.

#include "service/StreamTypes.hpp"

#include "oatpp-websocket/ConnectionHandler.hpp"
#include "oatpp-websocket/WebSocket.hpp"

#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace ws {

inline std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// Thread-safe set of connected camera-state sockets, with a broadcast helper.
class CameraStateRegistry {
public:
    void add(const oatpp::websocket::WebSocket* socket) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sockets.insert(socket);
    }

    void remove(const oatpp::websocket::WebSocket* socket) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sockets.erase(socket);
    }

    // Pushes one JSON message to every client, but only when the camera's
    // state (or its error detail) actually changed since the last push — the
    // GStreamer status sink fires on every transition, some of which map to
    // the same coarse state. Called from GStreamer threads; the mutex
    // serialises broadcasts so two threads never write one socket at once.
    // A failing socket is skipped — its read loop will unregister it.
    void broadcastState(const stream::StreamStatusSnapshot& snapshot) {
        if (snapshot.id.empty()) return;
        const std::string state = stream::toString(snapshot.state);

        std::lock_guard<std::mutex> lock(m_mutex);
        const std::string key = state + '\n' + snapshot.lastError;
        auto& last = m_lastKeyByCamera[snapshot.id];
        if (last == key) return;  // nothing meaningful changed
        last = key;

        const oatpp::String message = buildMessage(snapshot, state);
        for (const auto* socket : m_sockets) {
            try {
                socket->sendOneFrameText(message);
            } catch (...) {
                // Dead socket — ignore; onBeforeDestroy will remove it.
            }
        }
    }

private:
    static oatpp::String buildMessage(const stream::StreamStatusSnapshot& s,
                                      const std::string& state) {
        std::string json = "{";
        json += "\"id\":\"" + jsonEscape(s.id) + "\",";
        json += "\"state\":\"" + jsonEscape(state) + "\",";
        json += "\"lastError\":\"" + jsonEscape(s.lastError) + "\",";
        json += "\"lastChangedAt\":\"" + jsonEscape(s.lastChangedAt) + "\"";
        json += "}";
        return oatpp::String(json.c_str());
    }

    std::mutex m_mutex;
    std::set<const oatpp::websocket::WebSocket*> m_sockets;
    std::map<std::string, std::string> m_lastKeyByCamera;
};

// Per-socket listener. The camera-state stream is server->client only, so
// inbound frames are ignored; ping is answered to keep the link alive.
class CameraStateSocketListener : public oatpp::websocket::WebSocket::Listener {
public:
    void onPing(const WebSocket& socket, const oatpp::String& message) override {
        try {
            socket.sendPong(message);
        } catch (...) {
        }
    }
    void onPong(const WebSocket&, const oatpp::String&) override {}
    void onClose(const WebSocket&, v_uint16, const oatpp::String&) override {}
    void readMessage(const WebSocket&, v_uint8, p_char8, oatpp::v_io_size) override {}
};

// Registers/unregisters each accepted socket with the registry.
class CameraStateInstanceListener
    : public oatpp::websocket::ConnectionHandler::SocketInstanceListener {
public:
    explicit CameraStateInstanceListener(std::shared_ptr<CameraStateRegistry> registry)
        : m_registry(std::move(registry)),
          m_socketListener(std::make_shared<CameraStateSocketListener>()) {}

    void onAfterCreate(const WebSocket& socket,
                       const std::shared_ptr<const ParameterMap>&) override {
        socket.setListener(m_socketListener);
        m_registry->add(&socket);
    }

    void onBeforeDestroy(const WebSocket& socket) override {
        m_registry->remove(&socket);
    }

private:
    std::shared_ptr<CameraStateRegistry> m_registry;
    std::shared_ptr<CameraStateSocketListener> m_socketListener;
};

}  // namespace ws

#endif
