#ifndef test_gstreamer_ImageInferenceController_hpp
#define test_gstreamer_ImageInferenceController_hpp

// One-shot HTTP inference endpoint.
// Accepts multipart/form-data with these parts:
//   image   (file)  — JPEG bytes
//   stages  (text)  — chuỗi JSON, mảng tầng y hệt trường `stages` của ai-job:
//
//     [{"modelPath":"weights/yolov8.rknn","modelType":"yolov8_detect",
//       "classFilter":"7","conf":0.25},
//      {"parent":0,"modelPath":"weights/plate_det.rknn",
//       "modelType":"paddle_ocr_det","transform":"align_plate","conf":0.3},
//      {"parent":1,"modelPath":"weights/plate_rec.rknn",
//       "modelType":"paddle_ocr_rec","conf":0.3}]
//
// Returns the same JSON shape the Python consumer receives over the socket.

#include <cstdlib>
#include <string>
#include <vector>

#include "service/AiStageMapper.hpp"
#include "service/ImageInferenceService.hpp"

#include "oatpp/web/server/api/ApiController.hpp"
#include "oatpp/web/mime/multipart/PartList.hpp"
#include "oatpp/web/mime/multipart/Reader.hpp"
#include "oatpp/web/mime/multipart/InMemoryDataProvider.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/core/macro/component.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

class ImageInferenceController : public oatpp::web::server::api::ApiController {
public:
    explicit ImageInferenceController(
        OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

    ENDPOINT_INFO(run) {
        info->summary = "Run AI inference on an uploaded image (one-shot)";
        info->description =
            "multipart/form-data: 'image' (JPEG file) + 'stages' (chuoi JSON, "
            "mang tang y het truong stages cua ai-job). Returns the same JSON "
            "shape as the live RTSP pipeline emits to the Python consumer.";
        info->addConsumes<oatpp::Any>("multipart/form-data");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json");
        info->addResponse<oatpp::String>(Status::CODE_400, "text/plain");
        info->addResponse<oatpp::String>(Status::CODE_500, "text/plain");
    }
    ENDPOINT("POST", "/inference/run", run,
             REQUEST(std::shared_ptr<IncomingRequest>, request))
    {
        namespace mp = oatpp::web::mime::multipart;

        auto multipart = std::make_shared<mp::PartList>(request->getHeaders());
        mp::Reader reader(multipart.get());
        reader.setPartReader("image", mp::createInMemoryPartReader(64 * 1024 * 1024));
        reader.setDefaultPartReader(mp::createInMemoryPartReader(8 * 1024));
        request->transferBody(&reader);

        auto imagePart = multipart->getNamedPart("image");
        if (!imagePart) {
            return createResponse(Status::CODE_400, "missing 'image' part");
        }
        auto imageData = imagePart->getPayload()->getInMemoryData();
        if (!imageData || imageData->empty()) {
            return createResponse(Status::CODE_400, "empty image");
        }

        ImageInferenceRequest req;
        req.jpegBytes.assign(imageData->data(),
                             imageData->data() + imageData->size());

        const std::string stagesJson = getText(multipart, "stages");
        if (stagesJson.empty()) {
            return createResponse(Status::CODE_400, "missing 'stages' part");
        }
        req.stages = ai_stage::fromJson(getDefaultObjectMapper(),
                                        oatpp::String(stagesJson.c_str()));
        if (req.stages.empty()) {
            return createResponse(Status::CODE_400,
                                  "'stages' khong phai mang JSON hop le");
        }

        try {
            std::string json = ImageInferenceService::run(req);
            auto response = createResponse(Status::CODE_200, json.c_str());
            response->putHeader(Header::CONTENT_TYPE, "application/json");
            return response;
        } catch (const std::exception& e) {
            return createResponse(Status::CODE_500, e.what());
        }
    }

private:
    static std::string getText(
        const std::shared_ptr<oatpp::web::mime::multipart::PartList>& mp,
        const std::string& name) {
        auto part = mp->getNamedPart(name);
        if (!part || !part->getPayload()) return "";
        auto data = part->getPayload()->getInMemoryData();
        if (!data) return "";
        return std::string(data->c_str(), data->size());
    }
};

#include OATPP_CODEGEN_END(ApiController)

#endif  // test_gstreamer_ImageInferenceController_hpp
