#ifndef DETECT_AI_PPOCR_REC_H_
#define DETECT_AI_PPOCR_REC_H_

// PaddleOCR text recogniser (PP-OCR "rec" branch), CTC head.
//
// Reads ONE line of text from an image already scaled to the model input
// (48x320 for every PP-OCR rec model shipped so far). The output is a
// sequence of (timestep, character index, score) — the caller maps the index
// through the model's character dictionary.
//
// The .rknn must carry the /255 normalisation (convert with
// mean_values=[[0,0,0]], std_values=[[255,255,255]]): this engine feeds raw
// 0..255 pixels, while PP-OCR is trained on 0..1. Without it the input is
// 255x too large and the model returns noise.

#include "yolov8.h"  // rknn_app_context_t, image_buffer_t

// A CTC timestep that survived blank/repeat collapsing.
typedef struct {
    int index;    // row in the character dictionary (0-based, blank removed)
    float score;  // softmax-free max logit ratio at this step, 0..1
    int step;     // which of the seq_len timesteps it came from
} ppocr_char_t;

#define PPOCR_MAX_CHARS 64

int init_ppocr_rec_model(const char* model_path, rknn_app_context_t* app_ctx);
int release_ppocr_rec_model(rknn_app_context_t* app_ctx);

// Returns 0 on success and writes at most PPOCR_MAX_CHARS entries.
// `seq_len_out` receives the model's timestep count so the caller can turn a
// step index into an x position inside the input image.
int inference_ppocr_rec_model(rknn_app_context_t* app_ctx,
                              image_buffer_t* img,
                              ppocr_char_t* chars_out,
                              int* char_count_out,
                              int* seq_len_out);

#endif  // DETECT_AI_PPOCR_REC_H_
