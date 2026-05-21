#ifndef _RKNN_YOLOV8_DEMO_POSTPROCESS_H_
#define _RKNN_YOLOV8_DEMO_POSTPROCESS_H_

#include <stdint.h>

#include <vector>

#include "common.h"
#include "image_utils.h"
#include "rknn_api.h"

#define OBJ_NAME_MAX_SIZE 64
#define OBJ_NUMB_MAX_SIZE 128
#define OBJ_CLASS_NUM 17
#define AI_POSE_KEYPOINT_NUM 17
#define AI_SEG_MASK_WIDTH 640
#define AI_SEG_MASK_HEIGHT 640
#define AI_SEG_MASK_SIZE (AI_SEG_MASK_WIDTH * AI_SEG_MASK_HEIGHT)
#define NMS_THRESH 0.45f
#define BOX_THRESH 0.1f

typedef struct {
    float x;
    float y;
    float score;
} pose_keypoint_t;

typedef struct {
    image_rect_t box;
    float prop;
    int cls_id;
    int tracker_id;
    int keypoint_count;
    pose_keypoint_t keypoints[AI_POSE_KEYPOINT_NUM];
} object_detect_result;

typedef struct {
    int id;
    int count;
    int seg_width;
    int seg_height;
    int seg_valid;
    uint8_t seg_mask[AI_SEG_MASK_SIZE];
    object_detect_result results[OBJ_NUMB_MAX_SIZE];
} object_detect_result_list;

struct rknn_app_context_t;

int init_post_process();
void deinit_post_process();
char* coco_cls_to_name(int cls_id);

int post_process(rknn_app_context_t* app_ctx,
                 void* outputs,
                 letterbox_t* letter_box,
                 float conf_threshold,
                 float nms_threshold,
                 object_detect_result_list* od_results);

int post_process_seg(rknn_app_context_t* app_ctx,
                     rknn_output* outputs,
                     letterbox_t* letter_box,
                     float conf_threshold,
                     float nms_threshold,
                     object_detect_result_list* od_results);

int post_process_pose(rknn_app_context_t* app_ctx,
                      rknn_output* outputs,
                      letterbox_t* letter_box,
                      float conf_threshold,
                      float nms_threshold,
                      object_detect_result_list* od_results);

#endif
