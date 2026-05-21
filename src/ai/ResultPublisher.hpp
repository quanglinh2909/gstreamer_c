#ifndef AI_ENGINE_RESULT_PUBLISHER_HPP
#define AI_ENGINE_RESULT_PUBLISHER_HPP

// Publishes AiResult messages to a Python consumer over a Unix domain socket.
// One framed message per result:
//
//   [u32 total_len][u32 json_len][json bytes][full jpeg][crop jpeg...]
//
// All integers are big-endian. The JSON carries every size, so the consumer
// slices the trailing binary blob deterministically. Detection metadata is
// small; the JPEGs are hardware-encoded and only present when there is at
// least one detection. If no consumer is connected the result is dropped.

#include <arpa/inet.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "AiResult.hpp"

class ResultPublisher {
public:
    explicit ResultPublisher(std::string socketPath)
        : m_path(std::move(socketPath)) {}

    ~ResultPublisher() { stop(); }

    ResultPublisher(const ResultPublisher&) = delete;
    ResultPublisher& operator=(const ResultPublisher&) = delete;

    bool start() {
        m_listenFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (m_listenFd < 0) {
            std::perror("ResultPublisher socket");
            return false;
        }
        ::unlink(m_path.c_str());

        sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (m_path.size() >= sizeof(addr.sun_path)) {
            std::fprintf(stderr, "ResultPublisher: socket path too long\n");
            return false;
        }
        std::memcpy(addr.sun_path, m_path.c_str(), m_path.size() + 1);

        if (::bind(m_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::perror("ResultPublisher bind");
            return false;
        }
        if (::listen(m_listenFd, 4) < 0) {
            std::perror("ResultPublisher listen");
            return false;
        }

        m_running = true;
        m_acceptThread = std::thread([this] { acceptLoop(); });
        return true;
    }

    void stop() {
        if (!m_running.exchange(false)) return;
        if (m_listenFd >= 0) {
            ::shutdown(m_listenFd, SHUT_RDWR);
            ::close(m_listenFd);
            m_listenFd = -1;
        }
        if (m_acceptThread.joinable()) m_acceptThread.join();
        std::lock_guard<std::mutex> lock(m_mutex);
        for (int fd : m_clients) ::close(fd);
        m_clients.clear();
        ::unlink(m_path.c_str());
    }

    // Thread-safe; called from every AI job worker.
    void publish(const AiResult& res) {
        std::vector<uint8_t> msg = serialize(res);
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_clients.begin(); it != m_clients.end();) {
            if (sendAll(*it, msg.data(), msg.size())) {
                ++it;
            } else {
                ::close(*it);
                it = m_clients.erase(it);
            }
        }
    }

private:
    void acceptLoop() {
        while (m_running.load()) {
            int fd = ::accept(m_listenFd, nullptr, nullptr);
            if (fd < 0) {
                if (m_running.load()) std::perror("ResultPublisher accept");
                break;
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            m_clients.push_back(fd);
            std::fprintf(stderr, "ResultPublisher: consumer connected (fd=%d)\n", fd);
        }
    }

    static bool sendAll(int fd, const uint8_t* data, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    static void appendU32(std::vector<uint8_t>& out, uint32_t v) {
        uint32_t be = htonl(v);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&be);
        out.insert(out.end(), p, p + 4);
    }

    static void jsonEscape(std::ostringstream& os, const std::string& s) {
        os << '"';
        for (char c : s) {
            switch (c) {
                case '"': os << "\\\""; break;
                case '\\': os << "\\\\"; break;
                case '\n': os << "\\n"; break;
                case '\r': os << "\\r"; break;
                case '\t': os << "\\t"; break;
                default: os << c; break;
            }
        }
        os << '"';
    }

    static std::string buildJson(const AiResult& res) {
        std::ostringstream os;
        os.setf(std::ios::fixed);
        os.precision(4);
        os << '{';
        os << "\"cameraId\":"; jsonEscape(os, res.cameraId); os << ',';
        os << "\"jobId\":"; jsonEscape(os, res.jobId); os << ',';
        os << "\"seq\":" << res.seq << ',';
        os << "\"tsUs\":" << res.tsUs << ',';
        os << "\"origWidth\":" << res.origWidth << ',';
        os << "\"origHeight\":" << res.origHeight << ',';
        os << "\"fullJpegSize\":" << res.fullJpeg.size() << ',';
        os << "\"detections\":[";
        for (size_t i = 0; i < res.detections.size(); ++i) {
            const Detection& d = res.detections[i];
            if (i) os << ',';
            os << '{';
            os << "\"x1\":" << d.x1 << ",\"y1\":" << d.y1
               << ",\"x2\":" << d.x2 << ",\"y2\":" << d.y2 << ',';
            os << "\"score\":" << d.score << ',';
            os << "\"classId\":" << d.classId << ',';
            os << "\"keypoints\":[";
            for (size_t k = 0; k < d.keypoints.size(); ++k) {
                if (k) os << ',';
                os << d.keypoints[k];
            }
            os << "],";
            os << "\"embedding\":[";
            for (size_t e = 0; e < d.embedding.size(); ++e) {
                if (e) os << ',';
                os << d.embedding[e];
            }
            os << "],";
            uint32_t cropSize = 0;
            if (d.cropJpegIndex >= 0 &&
                d.cropJpegIndex < static_cast<int>(res.cropJpegs.size())) {
                cropSize = static_cast<uint32_t>(res.cropJpegs[d.cropJpegIndex].size());
            }
            os << "\"cropJpegSize\":" << cropSize;
            os << '}';
        }
        os << "]}";
        return os.str();
    }

    static std::vector<uint8_t> serialize(const AiResult& res) {
        const std::string json = buildJson(res);

        std::vector<uint8_t> body;
        appendU32(body, static_cast<uint32_t>(json.size()));
        body.insert(body.end(), json.begin(), json.end());
        body.insert(body.end(), res.fullJpeg.begin(), res.fullJpeg.end());
        for (const Detection& d : res.detections) {
            if (d.cropJpegIndex >= 0 &&
                d.cropJpegIndex < static_cast<int>(res.cropJpegs.size())) {
                const auto& blob = res.cropJpegs[d.cropJpegIndex];
                body.insert(body.end(), blob.begin(), blob.end());
            }
        }

        std::vector<uint8_t> msg;
        appendU32(msg, static_cast<uint32_t>(body.size()));
        msg.insert(msg.end(), body.begin(), body.end());
        return msg;
    }

    std::string m_path;
    int m_listenFd = -1;
    std::atomic<bool> m_running{false};
    std::thread m_acceptThread;
    std::mutex m_mutex;
    std::vector<int> m_clients;
};

#endif  // AI_ENGINE_RESULT_PUBLISHER_HPP
