#ifndef DETECT_AI_FACE_RECOGNITION_H_
#define DETECT_AI_FACE_RECOGNITION_H_

#include "yolov8.h"

#define FACE_EMBEDDING_DIM 512

typedef struct {
    int valid;
    int dim;
    float embedding[FACE_EMBEDDING_DIM];
} face_embedding_result_t;

int init_face_recognition_model(const char* model_path, rknn_app_context_t* app_ctx);
int release_face_recognition_model(rknn_app_context_t* app_ctx);
int inference_face_recognition_model(rknn_app_context_t* app_ctx,
                                     image_buffer_t* img,
                                     face_embedding_result_t* embedding_result);

#endif
