#ifndef test_gstreamer_AiComponent_hpp
#define test_gstreamer_AiComponent_hpp

#include "ai/AiManager.hpp"
#include "service/GStreamerService.hpp"

#include "oatpp/core/macro/component.hpp"

// Registers the in-process AI subsystem so services/controllers can resolve it
// via OATPP_COMPONENT. App.cpp owns starting/stopping it.
class AiComponent {
public:
    OATPP_CREATE_COMPONENT(std::shared_ptr<AiManager>, aiManager)([] {
        auto ai = std::make_shared<AiManager>();
        // Chia sẻ sổ nguồn RTP của GStreamerService: pipeline AI bám vào kết nối
        // ghi hình / xem live sẵn có thay vì mở kết nối RTSP thứ hai tới camera.
        // Resolve GStreamerService ở đây tạo nó trước (registry rỗng), recording
        // sẽ đổ nguồn vào sau khi khởi động — AI tra lúc chạy nên thấy đầy đủ.
        OATPP_COMPONENT(std::shared_ptr<GStreamerService>, streams);
        ai->setSourceRegistry(streams->sourceRegistry());
        return ai;
    }());
};

#endif
