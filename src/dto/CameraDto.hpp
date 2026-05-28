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
};

#include OATPP_CODEGEN_END(DTO)

#endif
