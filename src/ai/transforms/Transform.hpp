#ifndef AI_ENGINE_TRANSFORM_HPP
#define AI_ENGINE_TRANSFORM_HPP

// Stage-2 helper interface: turns one detection into the image fed to the
// stage-2 model (crop, alignment warp, etc).
//
// To add a new helper: implement this interface in src/ai/transforms/ and
// register it in AiCatalog.hpp — it then becomes selectable as
// AiJob.transformData and shows up in GET /ai-transforms automatically.
//
// Implementations must be stateless: one shared instance is used by every
// job worker thread.

#include <string>
#include <vector>

#include "AiResult.hpp"
#include "FramePrep.hpp"
#include "FrameTypes.hpp"

struct TransformContext {
    const Frame* frame;                   // full-res NV12 source
    const Detection* det;                 // detection being cropped
    // Hộp cần cắt, LUÔN theo hệ toạ độ khung gốc. Phải đọc từ đây chứ không
    // đọc det->x1: từ tầng 3 trở đi, x1..y2 của detection cha nằm trong không
    // gian ảnh cắt của tầng 2, còn cắt thì vẫn cắt trên khung gốc.
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    const std::vector<float>* keypoints;  // flat (x,y,score) triples, full-res
    int targetW;                          // model input width of this stage
    int targetH;                          // model input height of this stage
    // When true the helper crops tight to the detection box (no context
    // padding) so the object fills the model input — what DETR-style models
    // (rf_detect) expect. Set by the runner from the stage model's preference.
    bool tightCrop = false;
    // Cách nhét ảnh cắt vào khung đầu vào của model TẦNG NÀY (xem FramePrep).
    // Bộ chạy lấy từ chính model, y như tầng 0 vẫn làm — sai kiểu là hỏng hẳn
    // kết quả chứ không phải kém đi một chút: đo trên 6 dòng biển số thật,
    // kéo đầy 0/6 đúng còn giữ tỉ lệ + đệm 5/6 đúng.
    FramePrep prep = FramePrep::Letterbox;

    // --- ĐẦU RA: vùng của khung gốc mà ảnh vừa dựng phủ lên ---
    // Bộ chạy dùng nó để đưa hộp con (toạ độ trong ảnh cắt) về lại khung gốc,
    // nhờ vậy tầng sau nữa mới cắt được. Helper nào không phải phép cắt hình
    // chữ nhật (xoay/warp) thì cứ để srcW = 0, bộ chạy sẽ lấy tạm hộp cha.
    float srcX = 0, srcY = 0, srcW = 0, srcH = 0;
};

class Transform {
public:
    virtual ~Transform() = default;

    // Stable id stored in AiJob.transformData ("" = the identity/crop helper).
    virtual std::string id() const = 0;
    virtual std::string label() const = 0;
    virtual std::string description() const = 0;

    // Produces the stage input as packed RGB888 of size targetW x targetH.
    // Returns false to skip this detection (e.g. missing landmarks).
    // ctx là non-const để helper ghi lại vùng nguồn nó đã cắt (srcX..srcH).
    virtual bool apply(TransformContext& ctx, std::vector<uint8_t>& outRgb) = 0;
};

#endif  // AI_ENGINE_TRANSFORM_HPP
