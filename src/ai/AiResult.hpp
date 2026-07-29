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

    // Stage-2 sub-detections produced when model 2 is itself a detector
    // (e.g. OCR characters found inside this detection's crop). Coordinates
    // are in stage-2 crop space. Empty for single-stage / embedding jobs.
    std::vector<Detection> children;
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
