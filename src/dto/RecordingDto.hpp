#ifndef test_gstreamer_RecordingDto_hpp
#define test_gstreamer_RecordingDto_hpp

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

class RecordingSegmentDto : public oatpp::DTO {
    DTO_INIT(RecordingSegmentDto, DTO)

    DTO_FIELD(String, id);
    DTO_FIELD(String, cameraId);
    DTO_FIELD(String, path);
    DTO_FIELD(String, startAt);
    DTO_FIELD(String, endAt);
    DTO_FIELD(Int32, durationMs);
    DTO_FIELD(String, codec);
    DTO_FIELD(String, container);
    DTO_FIELD(String, recordingMode);
    DTO_FIELD(Boolean, hasMotion);
    DTO_FIELD(String, motionEventId);
    DTO_FIELD(String, status);
};

class RecordingSeekDto : public oatpp::DTO {
    DTO_INIT(RecordingSeekDto, DTO)

    DTO_FIELD(String, segmentId);
    DTO_FIELD(String, cameraId);
    DTO_FIELD(String, fileUrl);
    DTO_FIELD(String, segmentStartAt);
    DTO_FIELD(String, segmentEndAt);
    DTO_FIELD(Int64, offsetMs);
};

class MotionEventDto : public oatpp::DTO {
    DTO_INIT(MotionEventDto, DTO)

    DTO_FIELD(String, id);
    DTO_FIELD(String, cameraId);
    DTO_FIELD(String, startAt);
    DTO_FIELD(String, endAt);
    DTO_FIELD(Float64, maxScore);
};

#include OATPP_CODEGEN_END(DTO)

#endif
