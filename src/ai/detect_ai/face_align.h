#ifndef DETECT_AI_FACE_ALIGN_H_
#define DETECT_AI_FACE_ALIGN_H_

#include <stdint.h>

#include "common.h"

struct FacePoint2f {
    float x;
    float y;
};

bool get_reference_facial_points(FacePoint2f out[5], int target_width = 112, int target_height = 112);
bool get_face_similarity_transform(const FacePoint2f src_pts[5],
                                   float affine[2][3],
                                   int target_width = 112,
                                   int target_height = 112);
bool align_face_u8c3(const uint8_t* src_data,
                     int src_width,
                     int src_height,
                     int src_stride,
                     const FacePoint2f keypoints[5],
                     uint8_t* dst_data,
                     int dst_width = 112,
                     int dst_height = 112,
                     int dst_stride = 0);
bool align_face_image(const image_buffer_t& src_image,
                      const FacePoint2f keypoints[5],
                      image_buffer_t* dst_image,
                      int target_width = 112,
                      int target_height = 112);

#endif
