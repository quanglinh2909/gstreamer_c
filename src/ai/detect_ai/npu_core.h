#ifndef DETECT_AI_NPU_CORE_H_
#define DETECT_AI_NPU_CORE_H_

// NPU core selection for RK3588 (3 NPU cores).
//
// Measured on this board with yolov8m (640, int8), camera ~15fps:
//
//   mask      | 1 job          | 2 jobs (same model)
//   ----------+----------------+---------------------------
//   AUTO      | 12/s (Core0)   | 13/s + 13/s  (Core0+Core1)
//   0_1_2     | 13-14/s        | 7/s + 8/s    (serialized)
//
// The combined 0_1_2 mask splits one inference across all three cores, but
// concurrent inferences then SERIALIZE — with several jobs each one gets a
// fraction of the rate. AUTO keeps one whole core per model context, so
// jobs scale nearly linearly until all three cores are busy. Since this
// system is meant to run multiple cameras / AI jobs, AUTO is the default;
// a single-heavy-job deployment can squeeze out the last 1-2 fps with
// AI_NPU_CORE_MASK=012.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rknn_api.h"

static inline void npu_set_multicore(rknn_context ctx, const char* tag)
{
    const char* env = getenv("AI_NPU_CORE_MASK");
    if (!env || strcmp(env, "012") != 0) return;  // default: driver AUTO

    int ret = rknn_set_core_mask(ctx, RKNN_NPU_CORE_0_1_2);
    if (ret < 0) {
        // Non-fatal: chips without multiple NPU cores (or older drivers)
        // reject the mask — the model still runs on the default core.
        printf("%s: rknn_set_core_mask(0_1_2) unsupported (ret=%d), "
               "keeping default core\n", tag, ret);
    }
}

#endif  // DETECT_AI_NPU_CORE_H_
