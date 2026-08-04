#ifndef test_gstreamer_CameraDto_hpp
#define test_gstreamer_CameraDto_hpp

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

class CameraDto : public oatpp::DTO {
    DTO_INIT(CameraDto, DTO)

    DTO_FIELD_INFO(id) { info->description = "Camera id (UUID, server-generated)"; }
    DTO_FIELD(String, id);

    DTO_FIELD_INFO(name) { info->description = "Display name"; }
    DTO_FIELD(String, name);

    DTO_FIELD_INFO(rtsp) { info->description = "RTSP stream URL"; }
    DTO_FIELD(String, rtsp);

    DTO_FIELD_INFO(state) { info->description = "Camera state: online | offline | error"; }
    DTO_FIELD(String, state);

    DTO_FIELD_INFO(inputRtsp) { info->description = "Input RTSP URL used by runtime stream"; }
    DTO_FIELD(String, inputRtsp);

    DTO_FIELD_INFO(outputRtsp) { info->description = "Output RTSP URL exposed by this service"; }
    DTO_FIELD(String, outputRtsp);

    DTO_FIELD_INFO(codec) { info->description = "Detected stream codec"; }
    DTO_FIELD(String, codec);

    DTO_FIELD_INFO(hardware) { info->description = "Motion-decoder hardware preference: auto | software | vaapi | nvdec | v4l2"; }
    DTO_FIELD(String, hardware);

    DTO_FIELD_INFO(recordingEnabled) { info->description = "Whether recording is enabled for this camera"; }
    DTO_FIELD(Boolean, recordingEnabled);

    DTO_FIELD_INFO(recordingMode) { info->description = "off | always | motion"; }
    DTO_FIELD(String, recordingMode);

    DTO_FIELD_INFO(motionEnabled) { info->description = "Whether motion detection is enabled"; }
    DTO_FIELD(Boolean, motionEnabled);

    DTO_FIELD_INFO(motionSensitivity) { info->description = "motioncells sensitivity"; }
    DTO_FIELD(Float64, motionSensitivity);

    DTO_FIELD_INFO(motionThreshold) { info->description = "motioncells threshold"; }
    DTO_FIELD(Float64, motionThreshold);

    DTO_FIELD_INFO(preMotionSeconds) { info->description = "Seconds retained before motion starts"; }
    DTO_FIELD(UInt32, preMotionSeconds);

    DTO_FIELD_INFO(postMotionSeconds) { info->description = "Seconds retained after motion ends"; }
    DTO_FIELD(UInt32, postMotionSeconds);

    DTO_FIELD_INFO(segmentSeconds) { info->description = "Recording segment duration in seconds"; }
    DTO_FIELD(UInt32, segmentSeconds);

    DTO_FIELD_INFO(motionKeyframeOnly) { info->description = "Analyze motion on keyframes only (lower CPU)"; }
    DTO_FIELD(Boolean, motionKeyframeOnly);

    DTO_FIELD_INFO(motionGridX) { info->description = "Motion grid columns (8..32)"; }
    DTO_FIELD(UInt32, motionGridX);

    DTO_FIELD_INFO(motionGridY) { info->description = "Motion grid rows (8..32)"; }
    DTO_FIELD(UInt32, motionGridY);

    DTO_FIELD_INFO(motionCellLevels) {
        info->description = "One digit per cell, row-major: 0 = ignore, 1..9 = level (5 neutral)";
    }
    DTO_FIELD(String, motionCellLevels);
    DTO_FIELD_INFO(motionZones) {
        info->description =
            "Vung chuyen dong, JSON [{\"r1\",\"c1\",\"r2\",\"c2\",\"level\"}]. "
            "Toa do theo O cua luoi, ke ca hai dau. level 1..10 = can level*10% "
            "so o CUA CHINH VUNG do cung dong.";
    }
    DTO_FIELD(String, motionZones);
    DTO_FIELD_INFO(motionSaveEvents) {
        info->description =
            "Ghi su kien chuyen dong xuong DB hay chi ban WebSocket. Tat thi lop "
            "phu live van ve, chi khong con lich su de xem lai.";
    }
    DTO_FIELD(Boolean, motionSaveEvents);

    // ENGINE KHONG DUNG GIA TRI NAY. Bo don dung luong ben Python doc cot
    // cameras.retention_days: qua bay nhieu ngay thi ban ghi va su kien cua
    // camera bi xoa, ke ca khi dia con rong. 0 = khong gioi han.
    // Engine chi cho no di qua de bieu mau sua camera doc/ghi bang DUNG MOT
    // lan PUT nhu moi thiet lap khac cua camera.
    DTO_FIELD_INFO(retentionDays) {
        info->description =
            "So ngay giu du lieu cua camera nay (0 = khong gioi han). Engine "
            "khong doc; bo don cua Python thi hanh.";
    }
    DTO_FIELD(UInt32, retentionDays);

    DTO_FIELD_INFO(retryCount) { info->description = "Current reconnect retry count"; }
    DTO_FIELD(UInt32, retryCount);

    DTO_FIELD_INFO(lastError) { info->description = "Last stream error message"; }
    DTO_FIELD(String, lastError);

    DTO_FIELD_INFO(lastChangedAt) { info->description = "Last stream state change time in UTC"; }
    DTO_FIELD(String, lastChangedAt);
};

class CreateCameraDto : public oatpp::DTO {
    DTO_INIT(CreateCameraDto, DTO)

    DTO_FIELD(String, name);
    DTO_FIELD(String, rtsp);
    DTO_FIELD(String, hardware);
    DTO_FIELD(Boolean, recordingEnabled);
    DTO_FIELD(String, recordingMode);
    DTO_FIELD(Boolean, motionEnabled);
    DTO_FIELD(Float64, motionSensitivity);
    DTO_FIELD(Float64, motionThreshold);
    DTO_FIELD(UInt32, preMotionSeconds);
    DTO_FIELD(UInt32, postMotionSeconds);
    DTO_FIELD(UInt32, segmentSeconds);
    DTO_FIELD(Boolean, motionKeyframeOnly);
    DTO_FIELD(UInt32, motionGridX);
    DTO_FIELD(UInt32, motionGridY);
    DTO_FIELD(String, motionCellLevels);
    DTO_FIELD_INFO(motionZones) {
        info->description =
            "Vung chuyen dong, JSON [{\"r1\",\"c1\",\"r2\",\"c2\",\"level\"}]. "
            "Toa do theo O cua luoi, ke ca hai dau. level 1..10 = can level*10% "
            "so o CUA CHINH VUNG do cung dong.";
    }
    DTO_FIELD(String, motionZones);
    DTO_FIELD_INFO(motionSaveEvents) {
        info->description =
            "Ghi su kien chuyen dong xuong DB hay chi ban WebSocket. Tat thi lop "
            "phu live van ve, chi khong con lich su de xem lai.";
    }
    DTO_FIELD(Boolean, motionSaveEvents);

    // ENGINE KHONG DUNG GIA TRI NAY. Bo don dung luong ben Python doc cot
    // cameras.retention_days: qua bay nhieu ngay thi ban ghi va su kien cua
    // camera bi xoa, ke ca khi dia con rong. 0 = khong gioi han.
    // Engine chi cho no di qua de bieu mau sua camera doc/ghi bang DUNG MOT
    // lan PUT nhu moi thiet lap khac cua camera.
    DTO_FIELD_INFO(retentionDays) {
        info->description =
            "So ngay giu du lieu cua camera nay (0 = khong gioi han). Engine "
            "khong doc; bo don cua Python thi hanh.";
    }
    DTO_FIELD(UInt32, retentionDays);
};

#include OATPP_CODEGEN_END(DTO)

#endif
