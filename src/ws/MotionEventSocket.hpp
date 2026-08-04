#ifndef test_gstreamer_MotionEventSocket_hpp
#define test_gstreamer_MotionEventSocket_hpp

// WebSocket bắn sự kiện chuyển động ngay khi xảy ra.
//
//   GET /ws/motion-events        (frontend gọi qua /wsc/motion-events)
//
// Mỗi sự kiện là một JSON:
//   {"cameraId":"<uuid>","startAt":"...","endAt":"...",
//    "cells":"0:9,2:8","gridX":10,"gridY":10}
//
// Ngoài ra socket còn bắn TỪNG KHUNG có chuyển động, để lớp phủ trên video vẽ
// được ngay lúc đang động chứ không phải đợi sự kiện đóng:
//   {"type":"frame","cameraId":...,"inside":"0:1,0:2","outside":"5:9",
//    "gridX":32,"gridY":32}
// `inside` = ô nằm trong vùng đã vẽ, `outside` = ô động ngoài mọi vùng (giao
// diện tô đỏ). Khung được bắn BẤT KỂ vùng đã đủ ngưỡng hay chưa — đủ ngưỡng chỉ
// quyết định có sinh SỰ KIỆN hay không.
//
// Khung chỉ gửi cho socket ĐÃ ĐĂNG KÝ camera đó. Client đăng ký bằng cách gửi
// một dòng text là danh sách id ngăn bằng dấu phẩy (gửi lại là thay hẳn, gửi
// chuỗi rỗng là ngừng nhận khung). Không đăng ký thì chỉ nhận sự kiện — nếu bắn
// mọi camera cho mọi người thì 16 camera × 5 khung/giây đổ xuống từng trình
// duyệt, gần hết là dữ liệu người ta không xem.
//
// Vì sao đăng ký qua THÔNG ĐIỆP chứ không qua query `?camera_id=`: tường Live
// đổi camera trên các ô liên tục, mà đổi query thì phải đóng/mở lại socket.
//
// Chỉ server -> client cho phần dữ liệu. Danh sách ô đi kèm để Live View vẽ
// được đúng vùng vừa động, không phải chỉ báo "có chuyển động".
//
// Vì sao KHÔNG dùng chung ConnectionHandler với /ws/camera-state: oatpp gắn
// đúng MỘT SocketInstanceListener cho mỗi handler, mà listener chính là chỗ
// quyết định socket mới thuộc registry nào. Hai luồng dữ liệu -> hai handler.

#include "service/RecordingTypes.hpp"
#include "ws/CameraStateSocket.hpp"  // dùng lại ws::jsonEscape

#include "oatpp-websocket/ConnectionHandler.hpp"
#include "oatpp-websocket/WebSocket.hpp"

#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace ws {

class MotionEventRegistry {
public:
    void add(const oatpp::websocket::WebSocket* socket) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sockets[socket] = {};
    }

    void remove(const oatpp::websocket::WebSocket* socket) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sockets.erase(socket);
    }

    /** Danh sách camera socket này muốn nhận KHUNG. Rỗng = không nhận khung. */
    void subscribe(const oatpp::websocket::WebSocket* socket, const std::string& csv) {
        std::set<std::string> ids;
        size_t start = 0;
        while (start <= csv.size()) {
            const auto comma = csv.find(',', start);
            auto piece = csv.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start);
            // Bỏ khoảng trắng hai đầu — client nào ghép chuỗi có dấu cách vẫn chạy.
            while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.front()))) {
                piece.erase(piece.begin());
            }
            while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.back()))) {
                piece.pop_back();
            }
            if (!piece.empty()) ids.insert(piece);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_sockets.find(socket);
        if (found != m_sockets.end()) found->second = std::move(ids);
    }

    /** Có ai đang xem camera này không — engine khỏi dựng chuỗi JSON nếu không. */
    bool anyoneWatching(const std::string& cameraId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [socket, ids] : m_sockets) {
            (void)socket;
            if (ids.count(cameraId)) return true;
        }
        return false;
    }

    // Gọi từ luồng GStreamer, 5 lần/giây cho mỗi camera đang có động.
    void broadcastFrame(const recording::MotionFrameSnapshot& frame) {
        if (frame.cameraId.empty()) return;
        if (!anyoneWatching(frame.cameraId)) return;

        std::string json = "{\"type\":\"frame\",";
        json += "\"cameraId\":\"" + jsonEscape(frame.cameraId) + "\",";
        json += "\"inside\":\"" + jsonEscape(frame.insideCells) + "\",";
        json += "\"outside\":\"" + jsonEscape(frame.outsideCells) + "\",";
        json += "\"gridX\":" + std::to_string(frame.gridX) + ",";
        json += "\"gridY\":" + std::to_string(frame.gridY);
        json += "}";
        const oatpp::String message(json.c_str());

        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [socket, ids] : m_sockets) {
            if (!ids.count(frame.cameraId)) continue;
            try {
                socket->sendOneFrameText(message);
            } catch (...) {
                // Socket chết — bỏ qua, vòng đọc của nó sẽ tự gỡ đăng ký.
            }
        }
    }

    // Gọi từ luồng GStreamer (motion sink). Mutex vừa giữ tập socket vừa nối
    // tiếp các lần gửi để hai luồng không ghi cùng một socket.
    void broadcastEvent(const recording::MotionEventSnapshot& event) {
        if (event.cameraId.empty()) return;

        std::string json = "{\"type\":\"event\",";
        json += "\"cameraId\":\"" + jsonEscape(event.cameraId) + "\",";
        json += "\"startAt\":\"" + jsonEscape(event.startAt) + "\",";
        json += "\"endAt\":\"" + jsonEscape(event.endAt) + "\",";
        json += "\"cells\":\"" + jsonEscape(event.cells) + "\",";
        json += "\"gridX\":" + std::to_string(event.gridX) + ",";
        json += "\"gridY\":" + std::to_string(event.gridY);
        json += "}";
        const oatpp::String message(json.c_str());

        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [socket, ids] : m_sockets) {
            (void)ids;  // sự kiện gửi cho MỌI socket, không lọc camera
            try {
                socket->sendOneFrameText(message);
            } catch (...) {
                // Socket chết — bỏ qua, vòng đọc của nó sẽ tự gỡ đăng ký.
            }
        }
    }

private:
    std::mutex m_mutex;
    // socket -> camera nó muốn nhận KHUNG (sự kiện thì ai cũng nhận).
    std::map<const oatpp::websocket::WebSocket*, std::set<std::string>> m_sockets;
};

class MotionEventSocketListener : public oatpp::websocket::WebSocket::Listener {
public:
    explicit MotionEventSocketListener(std::shared_ptr<MotionEventRegistry> registry)
        : m_registry(std::move(registry)) {}

    void onPing(const WebSocket& socket, const oatpp::String& message) override {
        try {
            socket.sendPong(message);
        } catch (...) {
        }
    }
    void onPong(const WebSocket&, const oatpp::String&) override {}
    void onClose(const WebSocket&, v_uint16, const oatpp::String&) override {}

    // Client gửi danh sách camera muốn nhận KHUNG. oatpp gọi hàm này nhiều lần
    // cho một thông điệp dài rồi gọi lần cuối với size==0 để báo hết — phải gom
    // đủ mới xử lý, không thì danh sách dài bị cắt làm đôi.
    void readMessage(const WebSocket& socket,
                     v_uint8,
                     p_char8 data,
                     oatpp::v_io_size size) override {
        if (size == 0) {
            m_registry->subscribe(&socket, m_buffer);
            m_buffer.clear();
            return;
        }
        if (data && size > 0) {
            // Chặn client gửi chuỗi vô hạn làm phình bộ nhớ của engine.
            if (m_buffer.size() < 64 * 1024) {
                m_buffer.append(reinterpret_cast<const char*>(data),
                                static_cast<size_t>(size));
            }
        }
    }

private:
    std::shared_ptr<MotionEventRegistry> m_registry;
    std::string m_buffer;
};

class MotionEventInstanceListener
    : public oatpp::websocket::ConnectionHandler::SocketInstanceListener {
public:
    explicit MotionEventInstanceListener(std::shared_ptr<MotionEventRegistry> registry)
        : m_registry(registry),
          m_socketListener(std::make_shared<MotionEventSocketListener>(std::move(registry))) {}

    void onAfterCreate(const WebSocket& socket,
                       const std::shared_ptr<const ParameterMap>&) override {
        socket.setListener(m_socketListener);
        m_registry->add(&socket);
    }

    void onBeforeDestroy(const WebSocket& socket) override {
        m_registry->remove(&socket);
    }

private:
    std::shared_ptr<MotionEventRegistry> m_registry;
    std::shared_ptr<MotionEventSocketListener> m_socketListener;
};

}  // namespace ws

#endif
