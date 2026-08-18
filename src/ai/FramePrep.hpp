#ifndef AI_ENGINE_FRAME_PREP_HPP
#define AI_ENGINE_FRAME_PREP_HPP

// Cách nhét ảnh nguồn vào khung đầu vào của model. Model tự khai kiểu nó được
// huấn luyện, vì chọn sai kiểu là hỏng hẳn kết quả chứ không phải kém đi một
// chút.
//
// Đo trên 6 dòng biển số thật cắt từ camera Cổng PTZ (model rec biển số tự
// train, 48x320):
//     kéo đầy khung  -> 0/6 đúng  ('47714', '611755', 'D1M27', ...)
//     giữ tỉ lệ, đệm -> 5/6 đúng  ('47A79514', '61H01755', '31021', ...)
// Ngược lại PP-OCR det thì huấn luyện bằng DetResizeForTest cỡ cố định, tức là
// KÉO ĐẦY — và kéo đầy còn giữ được nhiều pixel dọc hơn (khung 16:9 mà
// letterbox vào 480x480 thì chữ chỉ còn 270px chiều cao).
//
// Đầu header riêng chứ không nằm trong FrameTypes.hpp: AiModel.hpp cố tình
// không kéo theo header GStreamer.
enum class FramePrep {
    // Giữ tỉ lệ, canh giữa, đệm quanh. Mặc định cho mọi detector.
    Letterbox,
    // Kéo đầy khung, chấp nhận méo tỉ lệ.
    Stretch,
    // Giữ tỉ lệ theo CHIỀU CAO, canh trái, đệm phần thừa bên phải — đúng cách
    // PaddleOCR resize_norm_img dựng một dòng chữ.
    FitHeight,
};

// Màu đệm đi LIỀN với kiểu dựng, nên suy ra chứ đừng bắt mỗi chỗ gọi tự nhớ:
// 128 cho dòng chữ (PaddleOCR đệm 0 sau khi chuẩn hoá về [-1,1] = xám 128),
// 114 như YOLO cho các detector.
inline int padColorFor(FramePrep prep) {
    return prep == FramePrep::FitHeight ? 128 : 114;
}

// Khung đầu vào mà model tầng 0 đòi hỏi. Camera pipeline dựng MỘT khung cho
// mỗi spec khác nhau trong đám job của nó, nên hai job cùng cỡ vẫn chỉ tốn một
// lần letterbox (trường hợp thường gặp), còn job dùng model cỡ khác vẫn được
// ăn đúng khung của nó thay vì bị ép về 640x640 như trước.
struct FrameSpec {
    int width = 640;
    int height = 640;
    FramePrep prep = FramePrep::Letterbox;

    int padColor() const { return padColorFor(prep); }

    bool operator==(const FrameSpec& o) const {
        return width == o.width && height == o.height && prep == o.prep;
    }
    bool operator!=(const FrameSpec& o) const { return !(*this == o); }
};

#endif  // AI_ENGINE_FRAME_PREP_HPP
