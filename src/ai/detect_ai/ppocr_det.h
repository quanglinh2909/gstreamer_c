#ifndef DETECT_AI_PPOCR_DET_H_
#define DETECT_AI_PPOCR_DET_H_

// PaddleOCR text detector (PP-OCR "det", a DBNet). Finds WHERE the lines of
// text are; reading them is ppocr_rec's job on each crop.
//
// The network returns one probability map the size of the input: high where a
// character stroke is. Turning that into boxes is the postprocess below —
// threshold, connected components, then "unclip" (DBNet is trained to predict
// a SHRUNK version of each text region, so the raw blob is smaller than the
// text and must be grown back).
//
// Convert with the ImageNet mean/std the model zoo uses
// (mean_values=[[123.675,116.28,103.53]], std_values=[[58.395,57.12,57.375]]),
// which is what this engine's raw 0..255 RGB input expects.

#include "yolov8.h"  // rknn_app_context_t, image_buffer_t

typedef struct {
    int left, top, right, bottom;
    float score;
} ppocr_box_t;

#define PPOCR_MAX_BOXES 64

int init_ppocr_det_model(const char* model_path, rknn_app_context_t* app_ctx);
int release_ppocr_det_model(rknn_app_context_t* app_ctx);

// `box_thresh` drops regions whose mean probability is below it (0.3 is the
// PaddleOCR default). Boxes come back in model-input pixels.
int inference_ppocr_det_model(rknn_app_context_t* app_ctx,
                              image_buffer_t* img,
                              float box_thresh,
                              ppocr_box_t* boxes_out,
                              int* box_count_out);

#endif  // DETECT_AI_PPOCR_DET_H_
