#ifndef test_gstreamer_CameraSourceRegistry_hpp
#define test_gstreamer_CameraSourceRegistry_hpp

// Sổ đăng ký nguồn RTP dùng chung theo camera — điểm hợp nhất "một kết nối tới
// camera vật lý" cho MỌI thứ tiêu thụ luồng: xem live (WHEP) và ghi hình
// (recording). Trước đây mỗi thứ tự mở rtspsrc riêng tới camera; camera vừa
// ghi vừa được xem là hai kết nối, mà nhiều camera Dahua/Hikvision giới hạn số
// luồng đồng thời.
//
// Đếm tham chiếu bằng shared_ptr: mỗi phiên xem và mỗi phiên ghi giữ một
// shared_ptr<CameraRtpSource>. Registry chỉ giữ weak_ptr nên nguồn TỰ chết khi
// người tiêu thụ cuối buông ra — camera rảnh (không ai xem, không ghi) thì
// không có kết nối nào, engine về gần 0% CPU.
//
// Tạo LƯỜI (lúc có người tiêu thụ đầu tiên) chứ không phải mỗi camera online
// đều kéo sẵn — nếu không thì mọi camera đều bị kéo suốt dù chẳng ai dùng.

#include "service/CameraRtpSource.hpp"
#include "service/TranscodedRtpSource.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace stream {

class CameraSourceRegistry {
public:
    // Lấy nguồn của camera: tái dùng nếu còn sống và đúng codec, không thì dựng
    // mới và PLAYING. Trả nullptr nếu không khởi động được. Gọi được từ nhiều
    // thread (thread HTTP của WHEP, thread camera của recording).
    std::shared_ptr<CameraRtpSource> acquire(const std::string& cameraId,
                                             const std::string& cameraRtsp,
                                             const std::string& cameraCodec,
                                             const GStreamerConfig& config) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sources.find(cameraId);
        if (it != m_sources.end()) {
            if (auto existing = it->second.lock()) {
                // Codec đổi (h264<->h265) thì nguồn cũ vô dụng, dựng lại.
                if (existing->alive() && existing->codec() == cameraCodec) {
                    return existing;
                }
            }
        }
        auto source = std::make_shared<CameraRtpSource>(cameraId, cameraRtsp,
                                                        cameraCodec, config);
        if (!source->start()) return nullptr;
        m_sources[cameraId] = source;
        return source;
    }

    // Chỉ TRA nguồn đang sống của camera, KHÔNG dựng mới và KHÔNG cần biết codec.
    // Dùng cho consumer muốn bám vào kết nối RTSP CÓ SẴN (ghi hình / xem live)
    // thay vì tự mở kết nối thứ hai tới camera — cụ thể là pipeline AI. Trả
    // nullptr khi chưa có ai kéo camera này (khi đó AI tự mở rtspsrc như cũ).
    // Không đoán codec: nguồn do recording tạo với codec ĐÃ DÒ lúc chạy, nên
    // AI bám vào là chắc chắn đúng codec; đây cũng là lý do KHÔNG gọi acquire()
    // (acquire cần truyền codec, đoán sai sẽ huỷ+dựng lại nguồn của recording).
    std::shared_ptr<CameraRtpSource> lookup(const std::string& cameraId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sources.find(cameraId);
        if (it != m_sources.end()) {
            if (auto existing = it->second.lock()) {
                if (existing->alive()) return existing;
            }
        }
        return nullptr;
    }

    // Nguồn H264 DÙNG CHUNG cho camera H265: transcode một lần rồi phát cho mọi
    // phiên WHEP không nhận H265. Tái dùng nếu đã có và còn sống, không thì dựng
    // mới trên nền `base` (nguồn H265 gốc của chính camera đó). Trả nullptr nếu
    // không dựng được -> phiên tự transcode như trước (fallback). Xem
    // TranscodedRtpSource. Giữ weak_ptr: nguồn transcode tự chết khi người xem
    // H265-transcode cuối buông ra.
    std::shared_ptr<FrameSource> acquireTranscoded(
        const std::string& cameraId,
        const std::shared_ptr<CameraRtpSource>& base,
        const GStreamerConfig& config) {
        if (!base) return nullptr;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_transcoded.find(cameraId);
        if (it != m_transcoded.end()) {
            if (auto existing = it->second.lock()) {
                if (existing->alive()) return existing;
            }
        }
        auto source = std::make_shared<TranscodedRtpSource>(cameraId, base, config);
        if (!source->start()) return nullptr;
        m_transcoded[cameraId] = source;
        return source;
    }

private:
    std::mutex m_mutex;
    std::unordered_map<std::string, std::weak_ptr<CameraRtpSource>> m_sources;
    // Tách sổ riêng: một camera H265 có thể đồng thời có nguồn gốc (ghi hình,
    // người xem Safari nhận H265) VÀ nguồn transcode H264 (người xem Chrome).
    std::unordered_map<std::string, std::weak_ptr<TranscodedRtpSource>> m_transcoded;
};

}  // namespace stream

#endif  // test_gstreamer_CameraSourceRegistry_hpp
