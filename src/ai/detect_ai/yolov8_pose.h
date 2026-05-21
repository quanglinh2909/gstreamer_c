#ifndef _RKNN_DEMO_YOLOV8_POSE_H_
#define _RKNN_DEMO_YOLOV8_POSE_H_

#include "yolov8.h"

int init_yolov8_pose_model(const char* model_path, rknn_app_context_t* app_ctx);
int release_yolov8_pose_model(rknn_app_context_t* app_ctx);
int inference_yolov8_pose_model(rknn_app_context_t* app_ctx, image_buffer_t* img, object_detect_result_list* od_results);

#endif
