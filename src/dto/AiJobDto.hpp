#ifndef test_gstreamer_AiJobDto_hpp
#define test_gstreamer_AiJobDto_hpp

// Một job AI = một CÂY model, khai bằng mảng `stages` theo thứ tự chạy.
//
// KHÔNG có modelPath2/modelType2/transformData nữa: hai model chỉ là trường hợp
// mảng có hai phần tử. Thêm tầng thứ ba, thứ tư là thêm phần tử — API không đổi.
//
//   [ {"modelPath":"weights/yolov8.rknn","modelType":"yolov8_detect",
//      "classFilter":"2,3,5,7","conf":0.25},
//     {"parent":0,"modelPath":"weights/plate_det.rknn",
//      "modelType":"paddle_ocr_det","inputClasses":"7","conf":0.3},
//     {"parent":1,"modelPath":"weights/plate_rec.rknn",
//      "modelType":"paddle_ocr_rec","conf":0.3},
//     {"parent":0,"modelPath":"weights/vehicle.rknn","modelType":"yolov8_detect",
//      "inputClasses":"2,3,5","conf":0.4} ]
//
// Cỡ đầu vào của mỗi tầng KHÔNG khai ở đây — engine đọc thẳng từ file .rknn,
// nên không có chuyện khai 640x640 rồi model thật lại là 480x480.

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

class AiStageDto : public oatpp::DTO {
    DTO_INIT(AiStageDto, DTO)

    DTO_FIELD_INFO(parent) {
        info->description =
            "Chi so tang cha trong mang (bat buoc nho hon chi so cua chinh no); "
            "-1 hoac bo trong = chay tren ca khung hinh (chi tang 0)";
    }
    DTO_FIELD(Int32, parent);

    DTO_FIELD_INFO(modelPath) { info->description = ".rknn model file path"; }
    DTO_FIELD(String, modelPath);

    DTO_FIELD_INFO(modelType) {
        info->description = "Loai model da dang ky trong AiCatalog "
                            "(yolov8_detect | paddle_ocr_det | paddle_ocr_rec | ...)";
    }
    DTO_FIELD(String, modelType);

    DTO_FIELD_INFO(transform) {
        info->description = "Cach dung anh dau vao tu hop cua tang cha "
                            "('' = cat thang theo hop | align_face | align_plate). "
                            "Khong dung o tang 0.";
    }
    DTO_FIELD(String, transform);

    DTO_FIELD_INFO(inputClasses) {
        info->description = "Loc DAU VAO: chi nhan detection cua tang cha mang "
                            "lop nay ('all'/rong = nhan het). Vd '7' = chi bien so.";
    }
    DTO_FIELD(String, inputClasses);

    DTO_FIELD_INFO(classFilter) {
        info->description = "Loc DAU RA: giu lop nao trong ket qua cua chinh "
                            "tang nay ('all'/rong = giu het)";
    }
    DTO_FIELD(String, classFilter);

    DTO_FIELD_INFO(conf) { info->description = "Nguong diem cua tang nay (0..1)"; }
    DTO_FIELD(Float64, conf);
};

class AiJobDto : public oatpp::DTO {
    DTO_INIT(AiJobDto, DTO)

    DTO_FIELD_INFO(id) { info->description = "AI job id (UUID, server-generated)"; }
    DTO_FIELD(String, id);

    DTO_FIELD_INFO(name) { info->description = "Display name"; }
    DTO_FIELD(String, name);

    DTO_FIELD_INFO(cameraId) { info->description = "Camera this job runs on"; }
    DTO_FIELD(String, cameraId);

    DTO_FIELD_INFO(enabled) { info->description = "Whether the job runs"; }
    DTO_FIELD(Boolean, enabled);

    DTO_FIELD_INFO(maxFps) { info->description = "Inference fps cap (0 = unlimited)"; }
    DTO_FIELD(Int32, maxFps);

    DTO_FIELD_INFO(stages) {
        info->description = "Cay model theo thu tu chay; phan tu 0 chay tren ca khung";
    }
    DTO_FIELD(List<oatpp::Object<AiStageDto>>, stages);
};

class CreateAiJobDto : public oatpp::DTO {
    DTO_INIT(CreateAiJobDto, DTO)

    DTO_FIELD(String, name);
    DTO_FIELD(String, cameraId);
    DTO_FIELD(Boolean, enabled);
    DTO_FIELD(Int32, maxFps);
    DTO_FIELD(List<oatpp::Object<AiStageDto>>, stages);
};

// Dạng thô đúng như Postgres trả về: `stages` là chuỗi JSON (cột jsonb ép sang
// text), vì oatpp-postgresql không ánh xạ jsonb sang DTO lồng nhau. AiJobService
// là chỗ duy nhất dịch giữa dạng này và AiJobDto ở trên.
class AiJobRowDto : public oatpp::DTO {
    DTO_INIT(AiJobRowDto, DTO)

    DTO_FIELD(String, id);
    DTO_FIELD(String, name);
    DTO_FIELD(String, cameraId);
    DTO_FIELD(Boolean, enabled);
    DTO_FIELD(Int32, maxFps);
    DTO_FIELD(String, stages);
};

#include OATPP_CODEGEN_END(DTO)

#endif
