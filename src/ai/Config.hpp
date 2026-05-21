#ifndef AI_ENGINE_CONFIG_HPP
#define AI_ENGINE_CONFIG_HPP

// Plain runtime structs for the AI subsystem. Populated from the database
// (ai_jobs / cameras tables) by AiJobService — there is no config file.

#include <set>
#include <string>

namespace cfg {

struct Camera {
    std::string id;
    std::string name;
    std::string uri;     // rtsp://...
    bool enabled = true;
};

// One AI job: a model-1 detector, optionally cascaded into a model-2 stage.
// classFilter picks which model-1 classes feed model-2; transform is the
// alignment helper applied to each crop before model-2.
struct AiJob {
    std::string jobId;
    std::string name;
    std::string cameraId;
    bool enabled = true;

    std::string model1Path;
    std::string model1Type;       // yolov8_detect | yolov8_pose | yolov8_seg
    std::set<int> classFilter;    // empty => keep all classes

    std::string model2Path;       // empty => single-stage job
    std::string model2Type;       // face_recognition | yolov8_detect | ...
    std::string transform;        // "" | align_face | align_plate

    float primaryConf = 0.25f;
    float secondaryConf = 0.25f;
    int maxFps = 0;               // 0 => run as fast as inference allows

    bool hasModel2() const { return !model2Path.empty(); }
};

// Parses a class filter string ("all" or csv like "0,2,5") into a set of ids.
// An empty set means "keep all classes".
inline std::set<int> parseClassFilter(const std::string& s) {
    std::set<int> out;
    if (s.empty() || s == "all") return out;
    size_t start = 0;
    while (start < s.size()) {
        size_t comma = s.find(',', start);
        const std::string tok =
            s.substr(start, comma == std::string::npos ? std::string::npos
                                                       : comma - start);
        if (!tok.empty()) {
            try {
                out.insert(std::stoi(tok));
            } catch (...) {
            }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

}  // namespace cfg

#endif  // AI_ENGINE_CONFIG_HPP
