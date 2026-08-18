#ifndef IMAGE_INFERENCE_SERVICE_HPP
#define IMAGE_INFERENCE_SERVICE_HPP

// One-shot HTTP inference. Decodes a JPEG, runs the SAME model tree as the live
// RTSP path (StageRunner), and returns the AiResult serialized in the same JSON
// shape the Python consumer receives.
//
// Dùng chung StageRunner với AiJob là có chủ ý: /model-test phải cho ra đúng
// cái mà camera sẽ cho ra, nếu không thì thử nghiệm vô nghĩa.

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "ai/AiCatalog.hpp"
#include "ai/AiResult.hpp"
#include "ai/Config.hpp"
#include "ai/FramePrep.hpp"
#include "ai/FrameTypes.hpp"
#include "ai/MaskBits.hpp"
#include "ai/RgaConverter.hpp"
#include "ai/ResultPublisher.hpp"
#include "ai/models/AiModel.hpp"
#include "ai/pipeline/StageRunner.hpp"
#include "ai/transforms/Transform.hpp"

#include "common.h"
#include "postprocess.h"

struct ImageInferenceRequest {
    std::vector<uint8_t> jpegBytes;
    // Cùng dạng cây tầng như job chạy thật (xem AiJobDto/StageRunner).
    std::vector<cfg::AiStage> stages;
};

class ImageInferenceService {
public:
    // Returns JSON string in the same format as RTSP results.
    static std::string run(const ImageInferenceRequest& req) {
        static std::mutex s_mutex;
        std::lock_guard<std::mutex> lock(s_mutex);

        if (req.jpegBytes.empty()) throw std::runtime_error("empty image");
        if (req.stages.empty()) throw std::runtime_error("stages required");

        cv::Mat bgr = cv::imdecode(req.jpegBytes, cv::IMREAD_COLOR);
        if (bgr.empty()) throw std::runtime_error("JPEG decode failed");

        // Cả cây phải nạp TRƯỚC khi xử lý ảnh: cỡ đầu vào của tầng 0 quyết định
        // ảnh có phải thu nhỏ bằng CPU trước hay không (ngay dưới).
        StageRunner runner;
        std::string error;
        if (!runner.init(req.stages, "inference", &error)) {
            throw std::runtime_error(error);
        }
        AiModel* model1 = runner.rootModel();
        if (!model1) throw std::runtime_error("stage 0 khong nap duoc");

        // RGA chỉ THU NHỎ được tối đa 8 lần trong một lần blit. Model OCR cao
        // 48px mà người dùng tải lên ảnh màn hình 1080p là 22 lần -> blit hỏng,
        // endpoint trả "letterbox failed". Thu nhỏ trước bằng CPU (một lần,
        // INTER_AREA) cho về trong tầm 4 lần rồi mới giao cho RGA.
        //
        // Giữ nguyên tỉ lệ: frontend vẽ box theo tỉ lệ origWidth/origHeight nên
        // ảnh nhỏ đi không làm lệch khung, miễn là không bóp méo.
        const int maxW = model1->inputWidth() * 4;
        const int maxH = model1->inputHeight() * 4;
        if (bgr.cols > maxW || bgr.rows > maxH) {
            const double s = std::min(static_cast<double>(maxW) / bgr.cols,
                                      static_cast<double>(maxH) / bgr.rows);
            cv::Mat small;
            cv::resize(bgr, small,
                       cv::Size(std::max(16, static_cast<int>(bgr.cols * s)),
                                std::max(16, static_cast<int>(bgr.rows * s))),
                       0, 0, cv::INTER_AREA);
            bgr = small;
        }

        // Chiều ngược lại: ảnh NHỎ hơn hẳn cỡ đầu vào thì phóng to sẵn bằng
        // CPU (INTER_CUBIC) thay vì để RGA kéo giãn tuyến tính lúc letterbox.
        //
        // Cùng một ảnh biển số, chỉ khác cách phóng: RGA tự kéo 272px lên 640
        // đọc ra '9G99' (0,69); phóng CUBIC 2 lần trước rồi mới đưa vào thì ra
        // '9999' (0,96). Nét chữ nhỏ sống hay chết là ở khâu nội suy này.
        //
        // Chặn ở 4 lần: phóng hơn nữa chỉ là bịa thêm pixel, tốn thời gian mà
        // không thêm thông tin.
        const int minW = model1->inputWidth() / 2;
        const int minH = model1->inputHeight() / 2;
        if (bgr.cols < minW && bgr.rows < minH) {
            const double s = std::min(4.0, std::min(static_cast<double>(minW) / bgr.cols,
                                                    static_cast<double>(minH) / bgr.rows));
            cv::Mat big;
            cv::resize(bgr, big,
                       cv::Size(static_cast<int>(bgr.cols * s),
                                static_cast<int>(bgr.rows * s)),
                       0, 0, cv::INTER_CUBIC);
            bgr = big;
        }

        // RGA requires RGB888 source width stride to be 16-aligned, and NV12
        // chroma is 2x2-subsampled so height must be even. Trim the input so
        // both hold; lose at most 15 px right and 1 px bottom.
        const int origW = bgr.cols & ~15;
        const int origH = bgr.rows & ~1;
        if (origW < 16 || origH < 16) {
            throw std::runtime_error("image too small after alignment");
        }
        if (origW != bgr.cols || origH != bgr.rows) {
            bgr = bgr(cv::Rect(0, 0, origW, origH));
        }

        cv::Mat rgb;
        cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
        if (!rgb.isContinuous()) rgb = rgb.clone();

        std::vector<uint8_t> nv12Buf(static_cast<size_t>(origW) * origH * 3 / 2);
        if (!rga::rgbToNv12(rgb.data, origW, origH, nv12Buf.data())) {
            throw std::runtime_error("RGB->NV12 conversion failed");
        }

        Frame frame;
        frame.nv12 = nv12Buf.data();
        frame.width = origW;
        frame.height = origH;
        frame.yStride = origW;
        frame.uvStride = origW;
        frame.uvOffset = static_cast<size_t>(origW) * origH;
        frame.inferW = model1->inputWidth();
        frame.inferH = model1->inputHeight();

        // Model tự khai kiểu nhét ảnh vào khung (xem FramePrep.hpp), y như
        // đường RTSP giờ cũng dựng khung theo model tầng 0.
        const FramePrep prep = model1->framePrep();
        if (!rga::letterboxNv12ToRgb(frame, padColorFor(prep), prep)) {
            throw std::runtime_error("letterbox failed: anh " + std::to_string(origW) +
                                     "x" + std::to_string(origH) + " -> " +
                                     std::to_string(frame.inferW) + "x" +
                                     std::to_string(frame.inferH));
        }

        image_buffer_t img;
        std::memset(&img, 0, sizeof(img));
        img.width = frame.inferW;
        img.height = frame.inferH;
        img.width_stride = frame.inferW;
        img.height_stride = frame.inferH;
        img.format = IMAGE_FORMAT_RGB888;
        img.virt_addr = frame.rgb.data();
        img.size = static_cast<int>(frame.rgb.size());
        img.fd = -1;

        AiResult res;
        res.origWidth = origW;
        res.origHeight = origH;

        // Lọc lớp, ngưỡng điểm, cắt ảnh và chạy các tầng con: tất cả nằm trong
        // StageRunner, y hệt đường RTSP.
        runner.run(frame, img, res);

        return ResultPublisher::buildJson(res);
    }
};

#endif  // IMAGE_INFERENCE_SERVICE_HPP
