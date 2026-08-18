#ifndef AI_ENGINE_AI_RESULT_HPP
#define AI_ENGINE_AI_RESULT_HPP

#include <cstdint>
#include <string>
#include <vector>

// One detected object. Box coordinates are in full-resolution camera space.
struct Detection {
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    float score = 0;
    int classId = 0;

    // Pose/face keypoints, flattened as (x, y, score) triples in full-res space.
    std::vector<float> keypoints;

    // Face embedding vector (empty for non-face jobs).
    std::vector<float> embedding;

    // Readable name of classId when the model carries its own label table —
    // today only OCR (the character this box holds). Empty for every other
    // model, and then the JSON key is omitted entirely.
    std::string text;

    // Segmentation mask for THIS detection, as a MASK_GRID×MASK_GRID bitmap
    // covering exactly the detection's bbox (bit set = pixel belongs to the
    // object). Empty unless the model is a *_seg one.
    //
    // Why a coarse grid instead of the full-res mask: the raw mask is a
    // 640×640 byte image per FRAME (410 KB). Shipping that per detection at
    // ~12 fps would dwarf everything else on the wire and in the DB, and an
    // overlay drawn on a video tile can't show that detail anyway. 32×32 bits
    // = 128 bytes per object is plenty for a silhouette.
    std::vector<uint8_t> maskBits;   // MASK_GRID*MASK_GRID/8 bytes, row-major
    static constexpr int MASK_GRID = 32;

    // Sub-detections produced when a later stage is itself a detector (OCR
    // characters inside a plate crop, a plate inside a car crop...).
    // Toạ độ x1..y2 của chúng nằm trong KHÔNG GIAN ẢNH CẮT mà tầng đó nhìn
    // thấy — giữ nguyên như trước để bộ ghép biển số bên Python không phải
    // sửa gì.
    std::vector<Detection> children;

    // Hộp của detection này trong hệ toạ độ KHUNG GỐC. Tầng 0 thì trùng
    // x1..y2; với các tầng sau đây là chỗ duy nhất biết nó nằm đâu trong ảnh
    // thật, và là thứ tầng kế tiếp dùng để cắt ảnh (cắt từ khung gốc chứ
    // không cắt lại trên ảnh đã cắt — nét hơn hẳn).
    float fx1 = 0, fy1 = 0, fx2 = 0, fy2 = 0;
    bool hasFrameBox = false;

    // Tầng nào sinh ra detection này (chỉ số trong cfg::AiJob::stages).
    // Nhờ nó mà bên nhận phân biệt được "ký tự do model 3 đọc" với "chi tiết
    // xe do model 4 phân loại" khi cả hai cùng treo dưới một hộp cha.
    int stage = 0;
};

// One inference result for one frame of one AI job. Carries the structured
// metadata plus, when there is at least one detection, the hardware-encoded
// full-frame and per-detection crop JPEGs that Python may persist.
struct AiResult {
    std::string cameraId;
    std::string jobId;
    uint64_t seq = 0;
    int64_t tsUs = 0;
    int origWidth = 0;
    int origHeight = 0;

    std::vector<Detection> detections;
    std::vector<uint8_t> fullJpeg;                 // full frame; may be empty
};

#endif  // AI_ENGINE_AI_RESULT_HPP
