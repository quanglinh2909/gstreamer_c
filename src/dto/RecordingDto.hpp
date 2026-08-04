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
    // Mốc phiên ghi (epoch ms): đổi giá trị giữa hai đoạn kề = PTS reset =
    // playlist phải chèn EXT-X-DISCONTINUITY.
    DTO_FIELD(Int64, sessionStartMs);
};

// Hàng 'recording' mồ côi lúc khởi động (chỉ id + path để probe file).
class OrphanSegmentDto : public oatpp::DTO {
    DTO_INIT(OrphanSegmentDto, DTO)

    DTO_FIELD(String, id);
    DTO_FIELD(String, path);
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

    // Ô đã động, dạng "hàng:cột" ngăn bằng dấu phẩy, kèm cỡ lưới lúc ghi nhận —
    // frontend cần cả hai mới vẽ lại đúng chỗ trên khung hình.
    DTO_FIELD(String, cells);
    DTO_FIELD(Int32, gridX);
    DTO_FIELD(Int32, gridY);

    // Khung hình lúc sự kiện bắt đầu, đường dẫn tương đối thư mục engine.
    // Rỗng = sự kiện này không có ảnh (nhánh ảnh chưa kịp có khung nào, hoặc
    // sự kiện được ghi bởi bản cũ). Frontend lấy ảnh qua
    // GET /motion-events/{id}/image chứ không đụng tới đường dẫn này.
    DTO_FIELD(String, imagePath);
};

/**
 * Thân của POST /cameras/{id}/ai-event — bên Python báo "camera này vừa có sự
 * kiện AI" để engine giữ đoạn ghi quanh thời điểm đó.
 *
 * KHÔNG mang cửa sổ thời gian: "ghi trước/ghi sau" là cài đặt của CAMERA
 * (preMotionSeconds/postMotionSeconds) và dùng chung cho cả chuyển động lẫn
 * mọi AI — một chỗ duy nhất để chỉnh, và luôn khớp với độ sâu bộ đệm đoạn-chờ.
 */
class AiEventDto : public oatpp::DTO {
    DTO_INIT(AiEventDto, DTO)

    // Loại AI đã sinh sự kiện ("plate_recognition", "face_mask", ...). Chỉ để
    // đọc log — engine không xử lý khác nhau theo loại.
    DTO_FIELD(String, source);
};

#include OATPP_CODEGEN_END(DTO)

#endif
