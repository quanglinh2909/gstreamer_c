#ifndef AI_ENGINE_MOTION_DETECTOR_HPP
#define AI_ENGINE_MOTION_DETECTOR_HPP

// Dò chuyển động NHƯ MỘT "AI job": đọc đúng khung mà pipeline AI đã dựng sẵn
// cho các model, thay vì tự dựng một nhánh GStreamer riêng.
//
// VÌ SAO ĐỔI (đo trên 12 camera 1080p thật, xem ghi chú ở RecordingTypes.hpp):
// nhánh cũ `mppvideodec ! videoscale ! videoconvert ! motioncells` tốn ~25%
// CPU MỖI CAMERA, trong đó:
//     giải mã                    1,3%   <- lặp lại lần hai, camera đã giải mã cho AI rồi
//     videoscale + videoconvert 15,1%   <- co giãn 1080p BẰNG CPU
//     motioncells                8,8%
// Cả ba khoản đó biến mất ở đây:
//   * khung đã được giải mã MỘT lần cho cả camera (AiCameraPipeline),
//   * đã được RGA co giãn + đổi sang RGB888 (RgaConverter::letterboxNv12ToRgb)
//     — phần cứng, không tốn CPU,
//   * việc còn lại chỉ là trừ hai khung theo ô, vài trăm nghìn điểm ảnh mỗi
//     giây, rẻ hơn motioncells một bậc.
//
// KHÔNG dùng lại motioncells: phần tử đó nhận RGB nên vẫn cần một bản sao
// khác, và mặt nạ của nó chỉ đọc được 255 ô đầu (đã đo) — giới hạn từng làm
// lưới lớn dò sai trong im lặng. Ở đây không cần mặt nạ vì việc "ô nào thuộc
// vùng nào" do CameraRecordingSession quyết định, y như trước.
//
// Đầu ra giữ NGUYÊN định dạng cũ ("r:c,r:c") để đi vào đúng
// CameraRecordingSession::evaluateZones — logic vùng/ngưỡng không đổi một dòng.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "FrameTypes.hpp"

class MotionDetector {
public:
    // sink(cellsCsv): gọi MỖI khung được phân tích, kể cả khi không ô nào động
    // (chuỗi rỗng). Bên nhận cần cả tin "không có gì" để đóng sự kiện đang mở —
    // motioncells trước đây làm việc đó bằng thông điệp motion_finished.
    using CellSink = std::function<void(const std::string&)>;

    MotionDetector(std::string cameraId, uint32_t gridX, uint32_t gridY, CellSink sink)
        : m_cameraId(std::move(cameraId)),
          m_gridX(std::max<uint32_t>(1, gridX)),
          m_gridY(std::max<uint32_t>(1, gridY)),
          m_sink(std::move(sink)) {}

    /** Khung mới nhất đã phân tích, để chụp ảnh sự kiện. Có thể là nullptr. */
    FramePtr latestFrame() const {
        std::lock_guard<std::mutex> lock(m_lastMutex);
        return m_last;
    }

    void submit(const FramePtr& frame) {
        if (!frame || frame->rgb.empty() || !m_sink) return;

        // Lưới trải trên phần ẢNH THẬT của khung letterbox, không trải lên cả
        // vùng đệm đen: trải lên cả viền thì ô ở mép không bao giờ động, và
        // vùng người dùng vẽ trên khung hình thật sẽ lệch khỏi ô tương ứng.
        const int cx = frame->contentW > 0 ? frame->contentX : 0;
        const int cy = frame->contentH > 0 ? frame->contentY : 0;
        const int cw = frame->contentW > 0 ? frame->contentW : frame->inferW;
        const int ch = frame->contentH > 0 ? frame->contentH : frame->inferH;
        if (cw <= 0 || ch <= 0) return;

        // Lưới lấy mẫu cố định, KHÔNG phụ thuộc kích thước khung: đổi độ phân
        // giải giữa chừng mà đệm cũ lệch cỡ là so nhầm điểm với điểm.
        const int sw = kSampleW;
        const int sh = kSampleH;
        const size_t plane = static_cast<size_t>(sw) * sh;
        if (m_prevLuma.size() != plane) {
            m_prevLuma.assign(plane, 0);
            m_havePrev = false;
        }
        m_curLuma.resize(plane);

        const uint8_t* rgb = frame->rgb.data();
        const int stride = frame->inferW * 3;
        for (int sy = 0; sy < sh; ++sy) {
            const int y = cy + (ch * sy) / sh;
            const uint8_t* line = rgb + static_cast<size_t>(std::min(y, frame->inferH - 1)) * stride;
            uint8_t* out = m_curLuma.data() + static_cast<size_t>(sy) * sw;
            for (int sx = 0; sx < sw; ++sx) {
                const int x = std::min(cx + (cw * sx) / sw, frame->inferW - 1);
                const uint8_t* px = line + static_cast<size_t>(x) * 3;
                // Độ sáng xấp xỉ (2R + 5G + B) / 8 — số nguyên, đủ để so hai
                // khung liên tiếp.
                out[sx] = static_cast<uint8_t>((2 * px[0] + 5 * px[1] + px[2]) >> 3);
            }
        }

        std::string csv;
        if (m_havePrev) {
            const size_t cellCount = static_cast<size_t>(m_gridX) * m_gridY;
            m_moved.assign(cellCount, 0u);
            m_total.assign(cellCount, 0u);
            for (int sy = 0; sy < sh; ++sy) {
                const size_t row = static_cast<size_t>(m_gridY) * sy / sh;
                const uint8_t* cur = m_curLuma.data() + static_cast<size_t>(sy) * sw;
                const uint8_t* prv = m_prevLuma.data() + static_cast<size_t>(sy) * sw;
                for (int sx = 0; sx < sw; ++sx) {
                    const size_t col = static_cast<size_t>(m_gridX) * sx / sw;
                    const size_t idx = row * m_gridX + col;
                    ++m_total[idx];
                    const int d = static_cast<int>(cur[sx]) - static_cast<int>(prv[sx]);
                    if (d > kPixelDelta || d < -kPixelDelta) ++m_moved[idx];
                }
            }
            csv.reserve(256);
            for (size_t idx = 0; idx < cellCount; ++idx) {
                if (m_total[idx] == 0) continue;
                // Đếm ĐIỂM ẢNH đổi chứ không lấy trung bình cả ô: một người đi
                // qua chỉ chiếm một phần ô, lấy trung bình thì thay đổi bị pha
                // loãng đi và ô không bao giờ báo động. Đây cũng là cách
                // motioncells làm, nên MỨC CỦA VÙNG người dùng đã chỉnh trước
                // đây vẫn giữ nguyên ý nghĩa.
                if (m_moved[idx] * 100u < m_total[idx] * kCellPercent) continue;
                if (!csv.empty()) csv.push_back(',');
                csv += std::to_string(idx / m_gridX);
                csv.push_back(':');
                csv += std::to_string(idx % m_gridX);
            }
        }

        m_prevLuma.swap(m_curLuma);
        m_havePrev = true;
        {
            // Giữ khung MỚI NHẤT để chụp ảnh sự kiện. Chỉ giữ con trỏ chia sẻ,
            // không sao chép điểm ảnh — và chỉ mã hoá JPEG lúc sự kiện BẮT ĐẦU,
            // chứ không encode đều 1 khung/giây như nhánh cũ (đo được 5,6% CPU
            // mỗi camera cho việc encode ra rồi vứt gần hết).
            std::lock_guard<std::mutex> lock(m_lastMutex);
            m_last = frame;
        }
        m_sink(csv);
    }

private:
    // Lưới lấy mẫu: 320x320 điểm cho mỗi khung, tức mỗi ô của lưới 32x32 có
    // 100 điểm. Đo được nền nhiễu cảm biến ở mức 0,1-0,3 đơn vị độ sáng, còn
    // chuyển động thật đẩy đỉnh lên 10-55 — nên hai hằng số dưới đây nằm giữa,
    // rất xa cả hai phía.
    static constexpr int kSampleW = 320;
    static constexpr int kSampleH = 320;
    // Một ĐIỂM ẢNH đổi quá bấy nhiêu đơn vị độ sáng thì coi là đã đổi.
    static constexpr int kPixelDelta = 12;
    // Một Ô có từng ấy PHẦN TRĂM điểm ảnh đã đổi thì coi là ô động.
    static constexpr unsigned kCellPercent = 15;

    std::string m_cameraId;
    uint32_t m_gridX;
    uint32_t m_gridY;
    CellSink m_sink;

    bool m_havePrev = false;
    std::vector<uint8_t> m_prevLuma;   // độ sáng khung TRƯỚC, lưới kSampleW*H
    std::vector<uint8_t> m_curLuma;    // dùng lại mỗi khung để khỏi cấp phát
    std::vector<uint32_t> m_moved;
    std::vector<uint32_t> m_total;

    mutable std::mutex m_lastMutex;
    FramePtr m_last;
};

#endif  // AI_ENGINE_MOTION_DETECTOR_HPP
