#ifndef test_gstreamer_MoqDto_hpp
#define test_gstreamer_MoqDto_hpp

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

// Yêu cầu mở một đường bơm khung cho MoQ. Do MÁY CHỦ MoQ gửi, không phải
// trình duyệt — trình duyệt chỉ nói giao thức MoQ trên QUIC.
class MoqFeedRequestDto : public oatpp::DTO {
    DTO_INIT(MoqFeedRequestDto, DTO)

    // Mã do máy chủ MoQ sinh; engine khai báo lại mã này khi nối vào unix
    // socket để bên kia biết dòng khung thuộc về người xem nào.
    DTO_FIELD(String, feed);
    DTO_FIELD(String, cameraId);
    // "live" | "playback"
    DTO_FIELD(String, mode);
    // Chỉ dùng cho playback: mốc bắt đầu (epoch ms) và tốc độ phát.
    DTO_FIELD(Int64, atMs);
    DTO_FIELD(Float64, rate);
};

class MoqFeedDto : public oatpp::DTO {
    DTO_INIT(MoqFeedDto, DTO)

    // Dùng cho DELETE /moq/feeds/{id}, và cho /playback/{id}/control khi xem lại.
    DTO_FIELD(String, sessionId);
};

#include OATPP_CODEGEN_END(DTO)

#endif  // test_gstreamer_MoqDto_hpp
