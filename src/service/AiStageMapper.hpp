#ifndef test_gstreamer_AiStageMapper_hpp
#define test_gstreamer_AiStageMapper_hpp

// Dịch mảng tầng giữa DTO (JSON của API/DB) và cfg::AiStage (cấu hình engine).
//
// Dùng chung cho CẢ HAI đường vào: job chạy trên RTSP (AiJobService) và endpoint
// thử một tấm ảnh (/inference/run). Hai đường đó phải hiểu cấu hình y hệt nhau,
// nếu không thì thử trên /model-test ra một kiểu mà chạy thật lại ra kiểu khác.

#include <string>
#include <vector>

#include "ai/Config.hpp"
#include "dto/AiJobDto.hpp"

#include "oatpp/core/Types.hpp"
#include "oatpp/core/data/mapping/ObjectMapper.hpp"

namespace ai_stage {

using StageList = oatpp::List<oatpp::Object<AiStageDto>>;

inline std::string str(const oatpp::String& v) {
    return v ? std::string(v->c_str()) : std::string();
}

// Chỉ số parent giữ NGUYÊN như gửi lên; StageRunner là chỗ bắt cây sai (parent
// trỏ tới tầng đứng sau, cây quá sâu...) và từ chối cả job, nên ở đây không
// đoán bừa giúp người dùng. Không khai parent thì hiểu là chuỗi thẳng: tầng 0
// chạy trên khung, tầng i nối vào tầng i-1.
inline std::vector<cfg::AiStage> toCfg(const StageList& stages) {
    std::vector<cfg::AiStage> out;
    if (!stages) return out;
    int index = 0;
    for (const auto& s : *stages) {
        if (!s) { ++index; continue; }
        cfg::AiStage stage;
        stage.modelPath = str(s->modelPath);
        stage.modelType = str(s->modelType);
        stage.transform = str(s->transform);
        stage.inputClasses = cfg::parseClassFilter(str(s->inputClasses));
        stage.classFilter = cfg::parseClassFilter(str(s->classFilter));
        // So sánh với nullptr chứ KHÔNG viết `s->conf ? ...`: oatpp::Boolean
        // có operator bool trả về GIÁ TRỊ chứ không phải kiểm tra null, và
        // ranh giới giữa Boolean với Int32/Float64 mong manh tới mức không
        // đáng đánh cược — `parent = 0` mà bị hiểu là "không khai" thì tầng
        // này lặng lẽ nối nhầm vào tầng khác.
        stage.conf = s->conf != nullptr ? static_cast<float>(*s->conf) : 0.25f;
        stage.parent = s->parent != nullptr ? *s->parent
                                            : (index == 0 ? -1 : index - 1);
        out.push_back(std::move(stage));
        ++index;
    }
    return out;
}

// Chuỗi JSON -> cfg. Trả về mảng rỗng khi JSON hỏng; chỗ gọi tự quyết định báo
// lỗi thế nào (400 cho người gọi, bỏ qua job cho dữ liệu DB hỏng).
inline std::vector<cfg::AiStage> fromJson(
    const std::shared_ptr<oatpp::data::mapping::ObjectMapper>& mapper,
    const oatpp::String& json) {
    if (!mapper || !json || json->size() == 0) return {};
    try {
        return toCfg(mapper->readFromString<StageList>(json));
    } catch (const std::exception&) {
        return {};
    }
}

}  // namespace ai_stage

#endif
