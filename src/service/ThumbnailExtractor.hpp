#ifndef test_gstreamer_ThumbnailExtractor_hpp
#define test_gstreamer_ThumbnailExtractor_hpp

// Trích MỘT khung hình JPEG từ một file .ts đã ghi, tại một mốc thời gian trong
// file — để hiện ảnh xem trước khi rê chuột trên timeline.
//
// Cùng ý tưởng seek với PlaybackSource nhưng KHÔNG có phiên, KHÔNG có feeder:
// mở pipeline giải mã ngắn hạn, PAUSED, seek tới keyframe gần nhất TRƯỚC mốc,
// lấy đúng khung preroll rồi đóng. Vì chỉ cần một hình, dùng khung PREROLL
// (pipeline ở PAUSED tự dừng ngay sau khung đầu) nên không phải chạy PLAYING —
// nhanh và không xả cả file vào bộ giải mã.
//
// Lùi về keyframe (SNAP_BEFORE|KEY_UNIT) là đủ tốt cho ảnh xem trước: sai lệch
// tối đa bằng một GOP (~1s), mắt không phân biệt được khi rê chuột.

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <cstdint>
#include <sstream>
#include <string>

#include "service/StreamTypes.hpp"

namespace stream {

class ThumbnailExtractor {
public:
    // Trả về JPEG (bytes) hoặc chuỗi rỗng nếu thất bại. width: bề rộng ảnh ra
    // (chiều cao tự tính theo tỉ lệ, pixel vuông).
    // Khoảng lùi trước mốc để chu kỳ làm-mới-dần của smart-codec kịp hoàn tất.
    static constexpr int64_t kWarmupRunbackMs = 2000;

    static std::string extract(const std::string& path, const std::string& codec,
                               int64_t offsetMs, int width) {
        const bool h265 = (codec == "h265");
        std::ostringstream launch;
        launch
            << "filesrc location=" << quoteLaunchValue(path)
            << " ! tsdemux name=d d. ! "
            // config-interval=-1: chèn SPS/PPS trước MỖI keyframe. BẮT BUỘC khi
            // seek giữa file — không có nó avdec thiếu tham số giải mã nên phun
            // ra một tràng khung XÁM cho tới khi tình cờ gặp lại SPS/PPS.
            << (h265 ? "h265parse" : "h264parse") << " config-interval=-1"
            // output-corrupt=false là CHÌA KHOÁ chống khung xám: sau seek giữa
            // file, avdec giải mã các khung P mồ côi (thiếu tham chiếu đã bị
            // flush) thành khung XÁM và đánh dấu "corrupt". Mặc định
            // output-corrupt=true đẩy chúng ra (xám tới tận IDR kế, có khi ~40
            // khung). Đặt false thì avdec BỎ hẳn chúng, khung ra ĐẦU TIÊN đã là
            // khung giải mã thật.
            << " ! " << (h265 ? "avdec_h265" : "avdec_h264") << " output-corrupt=false"
            << " ! videoconvert ! videoscale"
            // Chỉ ghim bề rộng + pixel vuông: videoscale tự tính chiều cao giữ
            // đúng tỉ lệ khung gốc.
            << " ! video/x-raw,width=" << width << ",pixel-aspect-ratio=1/1"
            << " ! jpegenc quality=70"
            // max-buffers vừa đủ + drop=false để lấy các khung THEO THỨ TỰ: bỏ
            // khung filler xám đầu tiên, giữ khung đã giải mã thật ngay sau.
            << " ! appsink name=out sync=false max-buffers=4 drop=false";

        GError* err = nullptr;
        GstElement* pipeline = gst_parse_launch(launch.str().c_str(), &err);
        if (!pipeline || err) {
            if (err) g_error_free(err);
            if (pipeline) gst_object_unref(pipeline);
            return {};
        }

        GstElement* appsink = gst_bin_get_by_name(GST_BIN(pipeline), "out");
        if (!appsink) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            return {};
        }

        std::string jpeg;

        // PAUSED + đợi preroll: seek trên pipeline chưa có trạng thái bị bỏ qua.
        gst_element_set_state(pipeline, GST_STATE_PAUSED);
        GstState state = GST_STATE_NULL;
        GstStateChangeReturn sc =
            gst_element_get_state(pipeline, &state, nullptr, 3 * GST_SECOND);
        if (sc == GST_STATE_CHANGE_FAILURE) {
            gst_object_unref(appsink);
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            return {};
        }

        // LÙI thêm một khoảng "khởi động" trước mốc. Camera smart-codec (Dahua
        // H.265+) làm mới DẦN (gradual refresh): sau mỗi điểm phục hồi phải giải
        // mã hết một chu kỳ (~15-16 khung) mới có khung HOÀN CHỈNH; seek đúng mốc
        // rồi mốc lại rơi GIỮA chu kỳ -> chỉ kịp lấy khung xám dở dang. Lùi ~2s
        // để chu kỳ kịp hoàn tất TRƯỚC mốc, khung hoàn chỉnh sẵn sàng để chọn.
        // Sai lệch ~2s không thành vấn đề với ảnh xem trước khi rê chuột.
        const int64_t seekMs =
            offsetMs > kWarmupRunbackMs ? offsetMs - kWarmupRunbackMs : 0;
        if (seekMs > 0) {
            gst_element_seek_simple(
                pipeline, GST_FORMAT_TIME,
                static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                          GST_SEEK_FLAG_KEY_UNIT |
                                          GST_SEEK_FLAG_SNAP_BEFORE),
                static_cast<gint64>(seekMs) * GST_MSECOND);
        }
        // seekMs == 0 (mốc gần đầu đoạn): không seek, giải mã từ đầu đoạn — đoạn
        // luôn mở bằng IDR sạch nên khung đầu đã hoàn chỉnh.

        // KHÔNG lấy khung PREROLL sau seek FLUSH: preroll hay dính khung đệm.
        // Cho chạy PLAYING rồi kéo SAMPLE thật.
        gst_element_set_state(pipeline, GST_STATE_PLAYING);

        // Chọn khung: GIỮ khung có JPEG LỚN NHẤT, kéo tiếp chừng nào cỡ còn lập
        // KỶ LỤC MỚI, dừng khi cỡ tụt hẳn (đã qua khung thật).
        //
        // Vì sao KHÔNG dùng ngưỡng byte tuyệt đối như trước (>=1500 -> "khung
        // thật"): camera Dahua bật "smart codec" (H.265+) dùng LÀM MỚI DẦN
        // (gradual refresh) — sau khi seek giữa file, decoder phun ra ~15-16
        // khung mờ/xám rõ dần RỒI mới tới khung hoàn chỉnh. Các khung xám đó
        // KHÔNG nhỏ (5-13KB ở width 240, tuỳ width/cảnh) nên ngưỡng 1500 cắt
        // nhầm ngay khung xám đầu -> thumbnail XÁM. Đo thực tế: cỡ JPEG TĂNG ĐƠN
        // ĐIỆU qua các khung xám, khung thật là CỰC ĐẠI và vọt ~5x (12KB->60KB),
        // khung P NGAY SAU nó nhỏ hẳn. Nên bám dấu hiệu tương đối đó, không phụ
        // thuộc width/cảnh. Stream thường (IDR ngay khung đầu): khung 0 lớn
        // nhất, khung 1 (P) nhỏ hơn -> dừng ngay ở khung 2, nhanh như cũ.
        // Trần an toàn: đủ để đi qua khoảng lùi khởi động (~2s) + chu kỳ làm mới
        // rồi tới khung hoàn chỉnh, nhưng vẫn dừng SỚM nhờ điều kiện tụt cỡ dưới.
        constexpr int kMaxFrames = 64;
        size_t biggest = 0;
        for (int i = 0; i < kMaxFrames; ++i) {
            GstSample* sample =
                gst_app_sink_try_pull_sample(GST_APP_SINK(appsink), 3 * GST_SECOND);
            if (!sample) break;
            size_t curSize = 0;
            if (GstBuffer* buffer = gst_sample_get_buffer(sample)) {
                GstMapInfo map;
                if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                    curSize = map.size;
                    if (curSize > biggest) {
                        biggest = curSize;
                        jpeg.assign(reinterpret_cast<const char*>(map.data), map.size);
                    }
                    gst_buffer_unmap(buffer, &map);
                }
            }
            gst_sample_unref(sample);
            // Đã qua khung cực đại (khung thật) khi cỡ tụt xuống < 60% kỷ lục —
            // đủ dung sai cho dao động nhỏ trong chuỗi làm-mới-dần (các khung đó
            // sát nhau, luôn lập kỷ lục mới), nhưng khung P sau khung thật tụt
            // hẳn nên bắt được. i>0 để luôn giữ ít nhất khung đầu.
            if (i > 0 && curSize * 10 < biggest * 6) break;
        }

        gst_object_unref(appsink);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return jpeg;
    }
};

}  // namespace stream

#endif  // test_gstreamer_ThumbnailExtractor_hpp
