#ifndef AI_ENGINE_PADDLE_OCR_MODEL_HPP
#define AI_ENGINE_PADDLE_OCR_MODEL_HPP

// PaddleOCR đầy đủ trong MỘT loại model: tìm dòng chữ (det) rồi đọc từng dòng
// (rec). Chọn một cái là ra chữ, không phải ghép hai tầng bằng tay.
//
// Mỗi detection = MỘT DÒNG CHỮ, chuỗi đọc được nằm ở Detection::text. Cách nó
// đi ra ngoài: detect() giữ lại các chuỗi của chính lần chạy này rồi đặt
// cls_id = chỉ số trong danh sách đó, nên labelFor(cls_id) mà tầng trên gọi
// ngay sau detect() trả về đúng chuỗi — không phải sửa gì trong đường ống.
//
// Chọn file: trỏ vào model DET, model REC được suy ra bằng cách đổi "det"
// thành "rec" trong tên (ppocrv6_tiny_det.rknn -> ppocrv6_tiny_rec.rknn), từ
// điển là "<tên rec>.txt".
//
// Ảnh cắt cho rec lấy từ khung GỐC khi có (setSourceFrame): chữ 11px trong ảnh
// 1500px mà cắt từ bản 640x640 của det thì chỉ còn ~5px, đọc không ra.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "AiModel.hpp"
#include "CpuCrop.hpp"
#include "FrameTypes.hpp"
#include "ppocr_det.h"
#include "ppocr_rec.h"

class PaddleOcrModel : public AiModel {
public:
    ~PaddleOcrModel() override {
        release_ppocr_det_model(&m_det);
        release_ppocr_rec_model(&m_rec);
    }

    bool load(const std::string& modelPath) override {
        std::memset(&m_det, 0, sizeof(m_det));
        std::memset(&m_rec, 0, sizeof(m_rec));
        if (init_ppocr_det_model(modelPath.c_str(), &m_det) != 0) return false;

        const std::string recPath = siblingRecPath(modelPath);
        if (recPath.empty()) {
            std::fprintf(stderr,
                         "paddle_ocr: khong suy ra duoc file rec tu '%s' "
                         "(can ten kieu *_det.rknn co *_rec.rknn di kem)\n",
                         modelPath.c_str());
            return false;
        }
        if (init_ppocr_rec_model(recPath.c_str(), &m_rec) != 0) {
            std::fprintf(stderr, "paddle_ocr: khong nap duoc rec '%s'\n",
                         recPath.c_str());
            return false;
        }
        loadDictionary(recPath);
        std::fprintf(stderr, "paddle_ocr: det=%s rec=%s\n", modelPath.c_str(),
                     recPath.c_str());
        return true;
    }

    int inputWidth() const override {
        return m_det.model_width > 0 ? m_det.model_width : 640;
    }
    int inputHeight() const override {
        return m_det.model_height > 0 ? m_det.model_height : 640;
    }
    bool prefersTightCrop() const override { return true; }
    // Khung vào của det: kéo đầy, giống PaddleOcrDetModel.
    FramePrep framePrep() const override { return FramePrep::Stretch; }
    void setSourceFrame(const struct Frame* frame) override { m_frame = frame; }

    std::string labelFor(int classId) const override {
        if (classId < 0 || classId >= static_cast<int>(m_texts.size())) return {};
        return m_texts[static_cast<size_t>(classId)];
    }

    bool detect(image_buffer_t& img, object_detect_result_list& out) override {
        ppocr_box_t boxes[PPOCR_MAX_BOXES];
        int count = 0;
        if (inference_ppocr_det_model(&m_det, &img, 0.0f, boxes, &count) != 0) {
            return false;
        }

        std::memset(&out, 0, sizeof(out));
        m_texts.clear();

        const int recW = m_rec.model_width > 0 ? m_rec.model_width : 320;
        const int recH = m_rec.model_height > 0 ? m_rec.model_height : 48;
        std::vector<uint8_t> crop;

        int kept = 0;
        for (int i = 0; i < count && kept < OBJ_NUMB_MAX_SIZE; ++i) {
            if (!cropForRec(img, boxes[i], recW, recH, crop)) continue;

            image_buffer_t sub;
            std::memset(&sub, 0, sizeof(sub));
            sub.width = recW;
            sub.height = recH;
            sub.width_stride = recW;
            sub.height_stride = recH;
            sub.format = IMAGE_FORMAT_RGB888;
            sub.virt_addr = crop.data();
            sub.size = static_cast<int>(crop.size());
            sub.fd = -1;

            ppocr_char_t chars[PPOCR_MAX_CHARS];
            int nchars = 0;
            int seqLen = 0;
            if (inference_ppocr_rec_model(&m_rec, &sub, chars, &nchars, &seqLen) != 0 ||
                nchars == 0) {
                continue;  // vệt không đọc ra chữ thì không phải chữ
            }

            std::string text;
            float scoreSum = 0.0f;
            for (int c = 0; c < nchars; ++c) {
                text += labelOf(chars[c].index);
                scoreSum += chars[c].score;
            }
            if (text.empty()) continue;

            object_detect_result& d = out.results[kept];
            std::memset(&d, 0, sizeof(d));
            d.box.left = boxes[i].left;
            d.box.top = boxes[i].top;
            d.box.right = boxes[i].right;
            d.box.bottom = boxes[i].bottom;
            // Điểm của cả dòng = trung bình độ chắc chắn của các ký tự đọc ra;
            // điểm của det chỉ nói "có nét chữ ở đây", không nói đọc có nổi.
            d.prop = scoreSum / static_cast<float>(nchars);
            d.cls_id = kept;  // chỉ số vào m_texts -> labelFor() trả ra chuỗi
            m_texts.push_back(std::move(text));
            ++kept;
        }
        out.count = kept;
        return true;
    }

private:
    // Ảnh cho rec: ưu tiên cắt từ khung GỐC (nét hơn nhiều), không có thì cắt
    // ngay trên ảnh đầu vào của det.
    //
    // GIỮ NGUYÊN TỈ LỆ rồi đệm xám bên phải, không kéo đầy 48x320: đo trên 6
    // dòng biển số thật, kéo đầy đọc đúng 0/6 còn giữ tỉ lệ đúng 5/6.
    bool cropForRec(const image_buffer_t& img, const ppocr_box_t& b, int dstW,
                    int dstH, std::vector<uint8_t>& out) {
        if (m_frame != nullptr) {
            float ox1, oy1, ox2, oy2;
            m_frame->inferToOrig(static_cast<float>(b.left), static_cast<float>(b.top),
                                 &ox1, &oy1);
            m_frame->inferToOrig(static_cast<float>(b.right), static_cast<float>(b.bottom),
                                 &ox2, &oy2);
            const int x = std::max(0, static_cast<int>(ox1));
            const int y = std::max(0, static_cast<int>(oy1));
            const int w = static_cast<int>(ox2) - x;
            const int h = static_cast<int>(oy2) - y;
            if (w > 1 && h > 1) {
                const int tw = fitWidth(w, h, dstW, dstH);
                const uint8_t* nv12 = m_frame->cpuNv12();
                if (nv12 &&
                    cropNv12ToRgbCpu(nv12, m_frame->width, m_frame->height,
                                     m_frame->yStride, m_frame->uvOffset,
                                     m_frame->uvStride, x, y, w, h, tw, dstH,
                                     m_scratch)) {
                    padRight(m_scratch, tw, dstH, dstW, out);
                    return true;
                }
            }
        }
        const int tw = fitWidth(b.right - b.left, b.bottom - b.top, dstW, dstH);
        if (!cropRgbNearest(img, b, tw, dstH, m_scratch)) return false;
        padRight(m_scratch, tw, dstH, dstW, out);
        return true;
    }

    // Bề rộng của dòng chữ sau khi kéo cho cao đúng dstH, chặn ở dstW.
    static int fitWidth(int w, int h, int dstW, int dstH) {
        if (h <= 0 || w <= 0) return dstW;
        const int tw = (w * dstH + h / 2) / h;
        return std::max(1, std::min(dstW, tw));
    }

    // Dán dòng chữ sát mép trái, phần thừa để xám 128 — đúng chỗ mà
    // resize_norm_img của PaddleOCR đệm 0 sau khi chuẩn hoá về [-1,1].
    static void padRight(const std::vector<uint8_t>& src, int srcW, int h,
                         int dstW, std::vector<uint8_t>& out) {
        out.assign(static_cast<size_t>(dstW) * h * 3, kRecPad);
        for (int y = 0; y < h; ++y) {
            std::memcpy(out.data() + static_cast<size_t>(y) * dstW * 3,
                        src.data() + static_cast<size_t>(y) * srcW * 3,
                        static_cast<size_t>(srcW) * 3);
        }
    }

    // Dự phòng: cắt + phóng bằng lấy mẫu gần nhất ngay trên ảnh RGB888 của det.
    static bool cropRgbNearest(const image_buffer_t& img, const ppocr_box_t& b,
                               int dstW, int dstH, std::vector<uint8_t>& out) {
        const int srcW = img.width;
        const int srcH = img.height;
        const int stride = img.width_stride > 0 ? img.width_stride * 3 : srcW * 3;
        const int w = b.right - b.left;
        const int h = b.bottom - b.top;
        if (!img.virt_addr || w <= 1 || h <= 1 || dstW <= 0 || dstH <= 0) return false;

        out.assign(static_cast<size_t>(dstW) * dstH * 3, 0);
        for (int y = 0; y < dstH; ++y) {
            int sy = b.top + (y * h) / dstH;
            if (sy < 0) sy = 0;
            if (sy >= srcH) sy = srcH - 1;
            const uint8_t* srcRow = img.virt_addr + static_cast<size_t>(sy) * stride;
            uint8_t* dstRow = out.data() + static_cast<size_t>(y) * dstW * 3;
            for (int x = 0; x < dstW; ++x) {
                int sx = b.left + (x * w) / dstW;
                if (sx < 0) sx = 0;
                if (sx >= srcW) sx = srcW - 1;
                std::memcpy(dstRow + x * 3, srcRow + sx * 3, 3);
            }
        }
        return true;
    }

    std::string labelOf(int index) const {
        if (index < 0 || index >= static_cast<int>(m_dict.size())) return {};
        return m_dict[static_cast<size_t>(index)];
    }

    static std::string siblingRecPath(const std::string& detPath) {
        const size_t slash = detPath.find_last_of('/');
        const std::string dir =
            slash == std::string::npos ? std::string() : detPath.substr(0, slash + 1);
        std::string name =
            slash == std::string::npos ? detPath : detPath.substr(slash + 1);
        const size_t at = name.rfind("det");
        if (at == std::string::npos) return {};
        name.replace(at, 3, "rec");
        const std::string full = dir + name;
        std::ifstream probe(full, std::ios::binary);
        return probe.is_open() ? full : std::string();
    }

    void loadDictionary(const std::string& recPath) {
        m_dict.clear();
        std::string sidecar = recPath;
        const size_t dot = sidecar.find_last_of('.');
        if (dot != std::string::npos) sidecar.resize(dot);
        sidecar += ".txt";
        std::ifstream in(sidecar);
        if (!in.is_open()) {
            std::fprintf(stderr, "paddle_ocr: thieu tu dien %s\n", sidecar.c_str());
            return;
        }
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            m_dict.push_back(line);
        }
        m_dict.push_back(" ");
        std::fprintf(stderr, "paddle_ocr: tu dien %zu ky tu\n", m_dict.size());
    }

    static constexpr uint8_t kRecPad = 128;

    rknn_app_context_t m_det{};
    rknn_app_context_t m_rec{};
    // Ảnh dòng chữ trước khi đệm; giữ lại để không cấp phát mỗi hộp.
    std::vector<uint8_t> m_scratch;
    std::vector<std::string> m_dict;
    // Chuỗi của LẦN CHẠY hiện tại; cls_id là chỉ số vào đây.
    std::vector<std::string> m_texts;
    const struct Frame* m_frame = nullptr;
};

#endif  // AI_ENGINE_PADDLE_OCR_MODEL_HPP
