#ifndef test_gstreamer_PlaybackDto_hpp
#define test_gstreamer_PlaybackDto_hpp

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

// Lệnh điều khiển một phiên xem lại. Trường nào bỏ trống = giữ nguyên, nên
// một lệnh có thể vừa đổi tốc độ vừa nhảy chỗ, hoặc chỉ làm một việc.
class PlaybackControlDto : public oatpp::DTO {
    DTO_INIT(PlaybackControlDto, DTO)

    // Mốc thời gian tường muốn nhảy tới (epoch ms).
    DTO_FIELD(Int64, atMs);
    // 1 = tốc độ thật; 4/8/16 = tua nhanh (từ 4 trở lên engine chỉ gửi keyframe).
    DTO_FIELD(Float64, rate);
    DTO_FIELD(Boolean, paused);
};

class PlaybackStatusDto : public oatpp::DTO {
    DTO_INIT(PlaybackStatusDto, DTO)

    DTO_FIELD(String, sessionId);
    // Vị trí đang phát, epoch ms — giao diện dùng để vẽ con trỏ trên timeline.
    DTO_FIELD(Int64, positionMs);
    DTO_FIELD(Float64, rate);
    DTO_FIELD(Boolean, paused);
    // true = đã phát hết dữ liệu phía sau mốc hiện tại.
    DTO_FIELD(Boolean, ended);
    // true = đang đợi đoạn ghi kế tiếp đóng lại (sát mép live).
    DTO_FIELD(Boolean, waiting);
    // Số thứ tự lệnh seek đã áp xong.
    DTO_FIELD(Int64, seq);
    // Số thứ tự lệnh seek vừa nhận (chỉ khác 0 trong trả lời của /control):
    // client bỏ qua mọi bản tin trạng thái có seq nhỏ hơn giá trị này.
    DTO_FIELD(Int64, seekSeq);
};

#include OATPP_CODEGEN_END(DTO)

#endif  // test_gstreamer_PlaybackDto_hpp
