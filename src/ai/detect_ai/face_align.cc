#include "face_align.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace {

static const FacePoint2f kReferenceFacialPoints[5] = {
    {30.29459953f, 51.69630051f},
    {65.53179932f, 51.50139999f},
    {48.02519989f, 71.73660278f},
    {33.54930115f, 92.36550140f},
    {62.72990036f, 92.20410156f},
};

using Mat3 = std::array<std::array<float, 3>, 3>;
using Mat4 = std::array<std::array<float, 4>, 4>;
using Vec4 = std::array<float, 4>;

static bool invert_3x3(const Mat3& src, Mat3* dst) {
    const float a = src[0][0], b = src[0][1], c = src[0][2];
    const float d = src[1][0], e = src[1][1], f = src[1][2];
    const float g = src[2][0], h = src[2][1], i = src[2][2];

    const float A = e * i - f * h;
    const float B = -(d * i - f * g);
    const float C = d * h - e * g;
    const float D = -(b * i - c * h);
    const float E = a * i - c * g;
    const float F = -(a * h - b * g);
    const float G = b * f - c * e;
    const float H = -(a * f - c * d);
    const float I = a * e - b * d;

    const float det = a * A + b * B + c * C;
    if (std::fabs(det) < 1e-8f) {
        return false;
    }
    const float inv_det = 1.0f / det;

    (*dst)[0][0] = A * inv_det;
    (*dst)[0][1] = D * inv_det;
    (*dst)[0][2] = G * inv_det;
    (*dst)[1][0] = B * inv_det;
    (*dst)[1][1] = E * inv_det;
    (*dst)[1][2] = H * inv_det;
    (*dst)[2][0] = C * inv_det;
    (*dst)[2][1] = F * inv_det;
    (*dst)[2][2] = I * inv_det;
    return true;
}

static bool solve_4x4(Mat4 a, Vec4 b, Vec4* x) {
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        float max_abs = std::fabs(a[col][col]);
        for (int row = col + 1; row < 4; ++row) {
            float v = std::fabs(a[row][col]);
            if (v > max_abs) {
                max_abs = v;
                pivot = row;
            }
        }
        if (max_abs < 1e-8f) {
            return false;
        }
        if (pivot != col) {
            std::swap(a[pivot], a[col]);
            std::swap(b[pivot], b[col]);
        }

        const float diag = a[col][col];
        for (int j = col; j < 4; ++j) {
            a[col][j] /= diag;
        }
        b[col] /= diag;

        for (int row = 0; row < 4; ++row) {
            if (row == col) {
                continue;
            }
            const float factor = a[row][col];
            if (std::fabs(factor) < 1e-8f) {
                continue;
            }
            for (int j = col; j < 4; ++j) {
                a[row][j] -= factor * a[col][j];
            }
            b[row] -= factor * b[col];
        }
    }

    *x = b;
    return true;
}

static bool find_nonreflective_similarity(const FacePoint2f uv[5],
                                          const FacePoint2f xy[5],
                                          Mat3* transform) {
    Mat4 xtx{};
    Vec4 xtu{};

    for (int idx = 0; idx < 5; ++idx) {
        const float x = xy[idx].x;
        const float y = xy[idx].y;
        const float u = uv[idx].x;
        const float v = uv[idx].y;

        const std::array<float, 4> row_u = {x, y, 1.0f, 0.0f};
        const std::array<float, 4> row_v = {y, -x, 0.0f, 1.0f};

        for (int r = 0; r < 4; ++r) {
            xtu[r] += row_u[r] * u + row_v[r] * v;
            for (int c = 0; c < 4; ++c) {
                xtx[r][c] += row_u[r] * row_u[c] + row_v[r] * row_v[c];
            }
        }
    }

    Vec4 sol{};
    if (!solve_4x4(xtx, xtu, &sol)) {
        return false;
    }

    const float sc = sol[0];
    const float ss = sol[1];
    const float tx = sol[2];
    const float ty = sol[3];

    Mat3 tinv{};
    tinv[0] = {sc, -ss, 0.0f};
    tinv[1] = {ss, sc, 0.0f};
    tinv[2] = {tx, ty, 1.0f};

    Mat3 t{};
    if (!invert_3x3(tinv, &t)) {
        return false;
    }
    t[0][2] = 0.0f;
    t[1][2] = 0.0f;
    t[2][2] = 1.0f;
    *transform = t;
    return true;
}

static inline float bilinear_at(const uint8_t* src, int stride, int width, int height, float x, float y, int c) {
    if (x < 0.0f || y < 0.0f || x > static_cast<float>(width - 1) || y > static_cast<float>(height - 1)) {
        return 0.0f;
    }

    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const float dx = x - static_cast<float>(x0);
    const float dy = y - static_cast<float>(y0);

    const uint8_t* p00 = src + y0 * stride + x0 * 3;
    const uint8_t* p01 = src + y0 * stride + x1 * 3;
    const uint8_t* p10 = src + y1 * stride + x0 * 3;
    const uint8_t* p11 = src + y1 * stride + x1 * 3;

    const float v00 = static_cast<float>(p00[c]);
    const float v01 = static_cast<float>(p01[c]);
    const float v10 = static_cast<float>(p10[c]);
    const float v11 = static_cast<float>(p11[c]);

    const float top = v00 + (v01 - v00) * dx;
    const float bottom = v10 + (v11 - v10) * dx;
    return top + (bottom - top) * dy;
}

}  // namespace

bool get_reference_facial_points(FacePoint2f out[5], int target_width, int target_height) {
    if (!out || target_width <= 0 || target_height <= 0) {
        return false;
    }

    for (int i = 0; i < 5; ++i) {
        out[i] = kReferenceFacialPoints[i];
    }

    if (target_width == target_height) {
        const float size_diff_x = (112.0f - 96.0f) * 0.5f;
        for (int i = 0; i < 5; ++i) {
            out[i].x += size_diff_x;
        }
    }
    return true;
}

bool get_face_similarity_transform(const FacePoint2f src_pts[5],
                                   float affine[2][3],
                                   int target_width,
                                   int target_height) {
    if (!src_pts || !affine || target_width <= 0 || target_height <= 0) {
        return false;
    }

    FacePoint2f ref_pts[5];
    if (!get_reference_facial_points(ref_pts, target_width, target_height)) {
        return false;
    }

    Mat3 transform{};
    if (!find_nonreflective_similarity(src_pts, ref_pts, &transform)) {
        return false;
    }

    affine[0][0] = transform[0][0];
    affine[0][1] = transform[1][0];
    affine[0][2] = transform[2][0];
    affine[1][0] = transform[0][1];
    affine[1][1] = transform[1][1];
    affine[1][2] = transform[2][1];
    return true;
}

bool align_face_u8c3(const uint8_t* src_data,
                     int src_width,
                     int src_height,
                     int src_stride,
                     const FacePoint2f keypoints[5],
                     uint8_t* dst_data,
                     int dst_width,
                     int dst_height,
                     int dst_stride) {
    if (!src_data || !keypoints || !dst_data || src_width <= 0 || src_height <= 0 || dst_width <= 0 || dst_height <= 0) {
        return false;
    }
    if (src_stride <= 0) {
        src_stride = src_width * 3;
    }
    if (dst_stride <= 0) {
        dst_stride = dst_width * 3;
    }

    float affine[2][3];
    if (!get_face_similarity_transform(keypoints, affine, dst_width, dst_height)) {
        return false;
    }

    cv::Mat src_mat(src_height, src_width, CV_8UC3, const_cast<uint8_t*>(src_data), static_cast<size_t>(src_stride));
    cv::Mat dst_mat(dst_height, dst_width, CV_8UC3, dst_data, static_cast<size_t>(dst_stride));
    cv::Mat warp = (cv::Mat_<double>(2, 3) <<
        static_cast<double>(affine[0][0]), static_cast<double>(affine[0][1]), static_cast<double>(affine[0][2]),
        static_cast<double>(affine[1][0]), static_cast<double>(affine[1][1]), static_cast<double>(affine[1][2]));

    cv::warpAffine(src_mat,
                   dst_mat,
                   warp,
                   cv::Size(dst_width, dst_height),
                   cv::INTER_LINEAR,
                   cv::BORDER_CONSTANT,
                   cv::Scalar(0, 0, 0));
    return true;
}

bool align_face_image(const image_buffer_t& src_image,
                      const FacePoint2f keypoints[5],
                      image_buffer_t* dst_image,
                      int target_width,
                      int target_height) {
    if (!dst_image || !src_image.virt_addr || src_image.width <= 0 || src_image.height <= 0) {
        return false;
    }
    if (!dst_image->virt_addr || dst_image->size < target_width * target_height * 3) {
        return false;
    }

    dst_image->width = target_width;
    dst_image->height = target_height;
    dst_image->width_stride = target_width;
    dst_image->height_stride = target_height;
    dst_image->format = src_image.format;

    const int src_stride = src_image.width_stride > 0 ? src_image.width_stride * 3 : src_image.width * 3;
    const int dst_stride = dst_image->width_stride > 0 ? dst_image->width_stride * 3 : target_width * 3;
    return align_face_u8c3(src_image.virt_addr,
                           src_image.width,
                           src_image.height,
                           src_stride,
                           keypoints,
                           dst_image->virt_addr,
                           target_width,
                           target_height,
                           dst_stride);
}
