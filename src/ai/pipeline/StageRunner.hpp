#ifndef AI_ENGINE_STAGE_RUNNER_HPP
#define AI_ENGINE_STAGE_RUNNER_HPP

// Chạy một CÂY model trên một khung hình.
//
// Tầng 0 chạy trên cả khung. Mỗi tầng sau gắn vào một tầng cha: với TỪNG
// detection mà tầng cha giữ lại, nó cắt ảnh (qua transform) rồi chạy model của
// mình trên ảnh đó; kết quả treo vào `children` của detection cha.
//
//     tầng 0: yolov8 (ô tô, xe máy, xe tải, biển số)
//       ├─ tầng 1: paddle_ocr        inputClasses = {biển số}
//       └─ tầng 2: phân loại chi tiết inputClasses = {ô tô, xe máy, xe tải}
//
// Nhiều tầng CÙNG một cha thì chạy song song trên cùng những hộp đó; tầng nối
// tiếp nhau (cha là tầng 1, 2, ...) thì thành chuỗi. Cùng một cấu trúc dữ liệu
// lo cả hai, nên thêm bài toán mới chỉ là thêm một phần tử vào mảng stages.
//
// VÌ SAO CẮT LUÔN TỪ KHUNG GỐC: từ tầng 3 trở đi, hộp mà tầng cha trả về nằm
// trong không gian ẢNH CẮT của nó. Cắt tiếp trên ảnh cắt là mất nét chồng lên
// nhau. Nên mỗi detection mang thêm hộp theo toạ độ KHUNG GỐC (Detection::fx1)
// và mọi tầng đều cắt từ khung gốc bằng hộp đó.
//
// Bộ chạy này là chỗ DUY NHẤT biết cách chạy nhiều tầng — cả đường RTSP
// (AiJob) lẫn endpoint thử ảnh (ImageInferenceService) đều gọi vào đây.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "AiCatalog.hpp"
#include "AiResult.hpp"
#include "Config.hpp"
#include "FrameTypes.hpp"
#include "MaskBits.hpp"
#include "models/AiModel.hpp"
#include "transforms/Transform.hpp"

#include "common.h"
#include "postprocess.h"

class StageRunner {
public:
    // Chặn cấu hình vô lý mà vẫn thoải mái cho mọi bài toán thật.
    static constexpr int kMaxStages = 8;
    static constexpr int kMaxDepth = 6;

    // Nạp model + transform của từng tầng. `tag` chỉ để in log.
    // Trả về false kèm thông báo trong `error` khi cấu hình sai.
    bool init(const std::vector<cfg::AiStage>& stages, const std::string& tag,
              std::string* error = nullptr) {
        m_stages.clear();
        if (stages.empty()) return fail(error, "khong co tang nao");
        if (static_cast<int>(stages.size()) > kMaxStages) {
            return fail(error, "qua " + std::to_string(kMaxStages) + " tang");
        }
        if (stages[0].parent >= 0) return fail(error, "tang 0 phai chay tren khung");

        m_stages.resize(stages.size());
        for (size_t i = 0; i < stages.size(); ++i) {
            const cfg::AiStage& sc = stages[i];
            Stage& st = m_stages[i];
            st.cfg = sc;

            // Cha phải là tầng ĐỨNG TRƯỚC: vừa cấm vòng lặp, vừa bảo đảm khi
            // chạy tới tầng i thì tầng cha đã có kết quả.
            if (i > 0 && (sc.parent < 0 || sc.parent >= static_cast<int>(i))) {
                return fail(error, "tang " + std::to_string(i) +
                                       " co parent khong hop le");
            }
            st.model = ai::createModel(sc.modelType);
            if (!st.model) {
                return fail(error, "tang " + std::to_string(i) +
                                       ": khong co loai model '" + sc.modelType + "'");
            }
            if (!st.model->load(sc.modelPath)) {
                return fail(error, "tang " + std::to_string(i) +
                                       ": khong nap duoc '" + sc.modelPath + "'");
            }
            if (i > 0) {
                st.transform = ai::getTransform(sc.transform);
                if (!st.transform) {
                    return fail(error, "tang " + std::to_string(i) +
                                           ": khong co transform '" + sc.transform + "'");
                }
                m_stages[static_cast<size_t>(sc.parent)].children.push_back(
                    static_cast<int>(i));
            }
        }

        if (depthOf(0) > kMaxDepth) return fail(error, "cay qua sau");
        std::fprintf(stderr, "[ai] %s: %zu tang\n", tag.c_str(), m_stages.size());
        return true;
    }

    bool ready() const { return !m_stages.empty(); }
    size_t stageCount() const { return m_stages.size(); }

    // Model tầng 0 — chỗ gọi cần nó để biết cỡ đầu vào và kiểu dựng khung
    // TRƯỚC khi tiền xử lý ảnh.
    AiModel* rootModel() const {
        return m_stages.empty() ? nullptr : m_stages[0].model.get();
    }

    // Chạy cả cây trên một khung đã tiền xử lý. `img` là ảnh đầu vào của tầng
    // 0 (đã letterbox/kéo giãn), `f` là khung gốc để cắt ảnh cho các tầng sau.
    void run(const Frame& f, image_buffer_t& img, AiResult& res) {
        if (m_stages.empty()) return;
        Stage& root = m_stages[0];

        object_detect_result_list results;
        std::memset(&results, 0, sizeof(results));
        // Model tự cắt ảnh bên trong (OCR gộp) đọc từ khung GỐC.
        root.model->setSourceFrame(&f);
        if (!root.model->detect(img, results)) return;

        for (int i = 0; i < results.count && i < OBJ_NUMB_MAX_SIZE; ++i) {
            const object_detect_result& d = results.results[i];
            if (!root.cfg.classFilter.empty() &&
                root.cfg.classFilter.count(d.cls_id) == 0) {
                continue;
            }
            if (d.prop < root.cfg.conf) continue;

            Detection det;
            f.inferToOrig(static_cast<float>(d.box.left),
                          static_cast<float>(d.box.top), &det.x1, &det.y1);
            f.inferToOrig(static_cast<float>(d.box.right),
                          static_cast<float>(d.box.bottom), &det.x2, &det.y2);
            if (det.x2 <= det.x1 || det.y2 <= det.y1) continue;
            det.score = d.prop;
            det.classId = d.cls_id;
            det.text = root.model->labelFor(d.cls_id);
            det.stage = 0;
            // Tầng 0 vốn đã ở hệ toạ độ khung gốc.
            det.fx1 = det.x1; det.fy1 = det.y1;
            det.fx2 = det.x2; det.fy2 = det.y2;

            const int kpCount = d.keypoint_count < AI_POSE_KEYPOINT_NUM
                                    ? d.keypoint_count
                                    : AI_POSE_KEYPOINT_NUM;
            for (int k = 0; k < kpCount; ++k) {
                float ox, oy;
                f.inferToOrig(d.keypoints[k].x, d.keypoints[k].y, &ox, &oy);
                det.keypoints.push_back(ox);
                det.keypoints.push_back(oy);
                det.keypoints.push_back(d.keypoints[k].score);
            }

            // Mask phân vùng: postprocess đã dựng sẵn ảnh nhãn cho CẢ KHUNG ở
            // không gian model; cắt phần trong bbox rồi hạ mẫu về lưới bit.
            if (results.seg_valid && results.seg_width > 0 && results.seg_height > 0) {
                fillMaskBits(results, d, det);
            }

            runChildren(0, f, det, 1);
            res.detections.push_back(std::move(det));
        }
    }

private:
    struct Stage {
        cfg::AiStage cfg;
        std::unique_ptr<AiModel> model;
        Transform* transform = nullptr;  // của AiCatalog (singleton dùng chung)
        std::vector<int> children;       // chỉ số các tầng nhận đầu ra của tầng này

        // Đo thời gian tích luỹ (chỉ khi AI_STAGE_TIMING=1). Tách TRANSFORM
        // với MODEL vì hai thứ đó tốn ở hai nơi khác nhau — dựng ảnh cắt ăn
        // CPU/RGA còn model ăn NPU — mà nhìn tổng thì không biết đường nào mà
        // sửa. Chỉ thread worker của job chạm vào nên không cần khoá.
        double transformMs = 0.0;
        double modelMs = 0.0;
        long calls = 0;
        long lastReport = 0;
    };

    static bool timingOn() {
        static const bool on = [] {
            const char* env = std::getenv("AI_STAGE_TIMING");
            return env && env[0] == '1';
        }();
        return on;
    }

    static double nowMs() {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // Đo một lần chạy tầng con, và cứ mỗi 100 lần thì in trung bình.
    struct StageTimer {
        Stage* st = nullptr;
        int index = 0;
        double t0 = 0.0, tTransform = 0.0;

        StageTimer(Stage& stage, int stageIdx) {
            if (!timingOn()) return;
            st = &stage;
            index = stageIdx;
            t0 = tTransform = nowMs();
        }
        void markTransform() {
            if (!st) return;
            const double now = nowMs();
            st->transformMs += now - t0;
            tTransform = now;
        }
        void markModel() {
            if (!st) return;
            st->modelMs += nowMs() - tTransform;
            ++st->calls;
            if (st->calls - st->lastReport < 100) return;
            st->lastReport = st->calls;
            std::fprintf(stderr,
                         "[ai time] tang %d (%s): %ld lan | dung anh %.1f ms/lan"
                         " | model %.1f ms/lan\n",
                         index, st->cfg.modelType.c_str(), st->calls,
                         st->transformMs / st->calls, st->modelMs / st->calls);
        }
    };

    static bool fail(std::string* error, const std::string& msg) {
        if (error) *error = msg;
        std::fprintf(stderr, "[ai] cau hinh tang sai: %s\n", msg.c_str());
        return false;
    }

    int depthOf(int stage) const {
        int deepest = 1;
        for (int c : m_stages[static_cast<size_t>(stage)].children) {
            const int d = 1 + depthOf(c);
            if (d > deepest) deepest = d;
        }
        return deepest;
    }

    // Chạy mọi tầng con của `parentStage` trên một detection đã giữ lại.
    void runChildren(int parentStage, const Frame& f, Detection& parent, int depth) {
        if (depth > kMaxDepth) return;
        for (int childIdx : m_stages[static_cast<size_t>(parentStage)].children) {
            Stage& st = m_stages[static_cast<size_t>(childIdx)];
            // Lọc ĐẦU VÀO: tầng này chỉ quan tâm vài lớp của tầng cha.
            if (!st.cfg.inputClasses.empty() &&
                st.cfg.inputClasses.count(parent.classId) == 0) {
                continue;
            }
            runStage(childIdx, f, parent, depth);
        }
    }

    void runStage(int stageIdx, const Frame& f, Detection& parent, int depth) {
        Stage& st = m_stages[static_cast<size_t>(stageIdx)];

        TransformContext ctx;
        ctx.frame = &f;
        ctx.det = &parent;
        // LUÔN cắt bằng hộp theo khung gốc — xem đầu file.
        ctx.x1 = parent.fx1; ctx.y1 = parent.fy1;
        ctx.x2 = parent.fx2; ctx.y2 = parent.fy2;
        ctx.keypoints = &parent.keypoints;
        ctx.targetW = st.model->inputWidth();
        ctx.targetH = st.model->inputHeight();
        ctx.tightCrop = st.model->prefersTightCrop();
        // Model tầng con cũng tự khai kiểu dựng ảnh như model tầng 0. Bỏ dòng
        // này là mọi ảnh cắt bị KÉO ĐẦY khung — PP-OCR rec đọc ra rác vì nó
        // được huấn luyện theo kiểu giữ tỉ lệ, đệm phải.
        ctx.prep = st.model->framePrep();

        StageTimer timer(st, stageIdx);
        std::vector<uint8_t> stageInput;
        if (!st.transform->apply(ctx, stageInput)) return;
        timer.markTransform();

        image_buffer_t img;
        std::memset(&img, 0, sizeof(img));
        img.width = ctx.targetW;
        img.height = ctx.targetH;
        img.width_stride = ctx.targetW;
        img.height_stride = ctx.targetH;
        img.format = IMAGE_FORMAT_RGB888;
        img.virt_addr = stageInput.data();
        img.size = static_cast<int>(stageInput.size());
        img.fd = -1;

        const size_t before = parent.children.size();
        const bool ran = st.model->runStage2(img, parent);
        timer.markModel();
        if (!ran) return;

        // Vùng khung gốc mà ảnh vừa chạy phủ lên. Helper nào không báo (phép
        // warp) thì lấy tạm hộp cha — đúng với mọi phép cắt sát hộp.
        float srcX = ctx.srcW > 0 ? ctx.srcX : parent.fx1;
        float srcY = ctx.srcH > 0 ? ctx.srcY : parent.fy1;
        float srcW = ctx.srcW > 0 ? ctx.srcW : parent.fx2 - parent.fx1;
        float srcH = ctx.srcH > 0 ? ctx.srcH : parent.fy2 - parent.fy1;
        const float kx = ctx.targetW > 0 ? srcW / ctx.targetW : 0.0f;
        const float ky = ctx.targetH > 0 ? srcH / ctx.targetH : 0.0f;

        for (size_t i = before; i < parent.children.size();) {
            Detection& c = parent.children[i];
            // Lọc ĐẦU RA của chính tầng này.
            const bool drop =
                c.score < st.cfg.conf ||
                (!st.cfg.classFilter.empty() &&
                 st.cfg.classFilter.count(c.classId) == 0);
            if (drop) {
                parent.children.erase(parent.children.begin() +
                                      static_cast<long>(i));
                continue;
            }
            c.stage = stageIdx;
            // x1..y2 giữ nguyên trong không gian ảnh cắt (bên Python đang dựa
            // vào đó); hộp theo khung gốc đi kèm riêng.
            c.fx1 = srcX + c.x1 * kx;
            c.fy1 = srcY + c.y1 * ky;
            c.fx2 = srcX + c.x2 * kx;
            c.fy2 = srcY + c.y2 * ky;
            c.hasFrameBox = true;
            runChildren(stageIdx, f, c, depth + 1);
            ++i;
        }
    }

    std::vector<Stage> m_stages;
};

#endif  // AI_ENGINE_STAGE_RUNNER_HPP
