#ifndef AI_ENGINE_PADDLE_OCR_DET_MODEL_HPP
#define AI_ENGINE_PADDLE_OCR_DET_MODEL_HPP

// PaddleOCR text detector (PP-OCR "det"). Stage-1 partner of paddle_ocr_rec:
// it finds the lines of text, rec reads each one.
//
// Pair them on the test page as model 1 = paddle_ocr_det, model 2 =
// paddle_ocr_rec with no transform — every text box then comes back with its
// characters as children. Alone, det only tells you WHERE text is (class 0).

#include <cstring>
#include <string>

#include "AiModel.hpp"
#include "ppocr_det.h"

class PaddleOcrDetModel : public AiModel {
public:
    ~PaddleOcrDetModel() override { release_ppocr_det_model(&m_ctx); }

    bool load(const std::string& modelPath) override {
        std::memset(&m_ctx, 0, sizeof(m_ctx));
        return init_ppocr_det_model(modelPath.c_str(), &m_ctx) == 0;
    }

    int inputWidth() const override {
        return m_ctx.model_width > 0 ? m_ctx.model_width : 640;
    }
    int inputHeight() const override {
        return m_ctx.model_height > 0 ? m_ctx.model_height : 640;
    }

    // DBNet được đánh giá bằng DetResizeForTest cỡ cố định, tức là KÉO ĐẦY chứ
    // không letterbox. Kéo đầy còn giữ được nhiều pixel dọc hơn: khung 16:9
    // letterbox vào 480x480 thì nội dung chỉ cao 270px, chữ nhỏ mất nét.
    FramePrep framePrep() const override { return FramePrep::Stretch; }

    bool detect(image_buffer_t& img, object_detect_result_list& out) override {
        ppocr_box_t boxes[PPOCR_MAX_BOXES];
        int count = 0;
        // 0.0 ở đây: để tầng trên lọc bằng primaryConf của người dùng, đúng
        // như mọi detector khác trong dự án.
        if (inference_ppocr_det_model(&m_ctx, &img, 0.0f, boxes, &count) != 0) {
            return false;
        }
        std::memset(&out, 0, sizeof(out));
        int kept = 0;
        for (int i = 0; i < count && kept < OBJ_NUMB_MAX_SIZE; ++i) {
            object_detect_result& d = out.results[kept];
            std::memset(&d, 0, sizeof(d));
            d.box.left = boxes[i].left;
            d.box.top = boxes[i].top;
            d.box.right = boxes[i].right;
            d.box.bottom = boxes[i].bottom;
            d.prop = boxes[i].score;
            d.cls_id = 0;
            ++kept;
        }
        out.count = kept;
        return true;
    }

    // KHÔNG đặt nhãn cho hộp: det chỉ biết "chỗ này có chữ", chữ gì là việc
    // của rec. Đặt đại một nhãn ("chu") thì nó ghi đè lên chỗ hiển thị chữ và
    // cả trang chỉ toàn đọc ra "chu chu chu".
private:
    rknn_app_context_t m_ctx{};
};

#endif  // AI_ENGINE_PADDLE_OCR_DET_MODEL_HPP
