#ifndef test_gstreamer_ImageInferenceController_hpp
#define test_gstreamer_ImageInferenceController_hpp

// One-shot HTTP inference endpoint.
// Accepts multipart/form-data with these parts:
//   image          (file)   — JPEG bytes
//   modelPath      (text)   — path to stage-1 .rknn
//   modelType      (text)   — yolov8_detect | yolov8_pose | yolov8_seg | face_recognition
//   modelPath2     (text)   — optional, stage-2 .rknn
//   modelType2     (text)   — optional, stage-2 type
//   transformData  (text)   — optional, "" | align_face | align_plate
//   primaryConf    (text)   — float, e.g. "0.25"
//   secondaryConf  (text)   — float, e.g. "0.3"
// Returns the same JSON shape the Python consumer receives over the socket.

#include <cstdlib>
#include <string>
#include <vector>

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
            "multipart/form-data: 'image' (JPEG file) + text fields "
            "modelPath, modelType, modelPath2, modelType2, transformData, "
            "primaryConf, secondaryConf. Returns the same JSON shape as the "
            "live RTSP pipeline emits to the Python consumer.";
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
        req.modelPath = getText(multipart, "modelPath");
        req.modelType = getText(multipart, "modelType");
        req.modelPath2 = getText(multipart, "modelPath2");
        req.modelType2 = getText(multipart, "modelType2");
        req.transformData = getText(multipart, "transformData");
        req.primaryConf = getFloat(multipart, "primaryConf", 0.25f);
        req.secondaryConf = getFloat(multipart, "secondaryConf", 0.0f);

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

    static float getFloat(
        const std::shared_ptr<oatpp::web::mime::multipart::PartList>& mp,
        const std::string& name, float defaultVal) {
        auto s = getText(mp, name);
        if (s.empty()) return defaultVal;
        try {
            return std::stof(s);
        } catch (...) {
            return defaultVal;
        }
    }
};

#include OATPP_CODEGEN_END(ApiController)

#endif  // test_gstreamer_ImageInferenceController_hpp
