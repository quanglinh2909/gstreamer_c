#ifndef AI_ENGINE_CONFIG_HPP
#define AI_ENGINE_CONFIG_HPP

// Plain runtime structs for the AI subsystem. Populated from the database
// (ai_jobs / cameras tables) by AiJobService — there is no config file.

#include <set>
#include <string>
#include <vector>

namespace cfg {

struct Camera {
    std::string id;
    std::string name;
    std::string uri;     // rtsp://...
    bool enabled = true;
};

// MỘT TẦNG của một job. Tầng 0 chạy trên cả khung hình; mọi tầng sau chạy
// trên ảnh cắt ra từ từng detection của tầng cha (parent).
//
// Hai bộ lọc lớp, khác nhau và đều tuỳ chọn:
//   * inputClasses — lọc ĐẦU VÀO: chỉ những detection của tầng cha mang lớp
//     này mới được đưa vào tầng này. Đây là chỗ để "model 1 tìm ô tô, xe máy,
//     xe tải, biển số; model 2 chỉ nhận biển số; model 3 chỉ nhận ba loại xe".
//   * classFilter — lọc ĐẦU RA: giữ lại lớp nào trong kết quả của chính tầng
//     này.
// Rỗng = không lọc.
struct AiStage {
    std::string modelPath;
    std::string modelType;
    // Cách dựng ảnh đầu vào từ detection của tầng cha ("" = cắt thẳng theo hộp).
    // Không dùng ở tầng 0 (tầng 0 nhận cả khung).
    std::string transform;
    std::set<int> inputClasses;
    std::set<int> classFilter;
    float conf = 0.25f;
    // Chỉ số tầng cha trong mảng stages; -1 = chạy trên khung hình.
    int parent = -1;
};

// Một job AI = một CÂY model chạy trên khung của một camera.
//
// KHÔNG còn khái niệm "model 1 / model 2": job nào cũng chỉ là một mảng tầng,
// job một model là mảng một phần tử. Muốn thêm tầng thứ ba, thứ tư (đọc chữ
// trong biển số đã cắt, phân biệt màu/hãng xe trên đúng những hộp xe) thì thêm
// phần tử vào mảng, không phải sửa engine.
struct AiJob {
    std::string jobId;
    std::string name;
    std::string cameraId;
    bool enabled = true;
    int maxFps = 0;               // 0 => run as fast as inference allows

    // Theo thứ tự chạy: phần tử 0 chạy trên cả khung, mọi phần tử sau trỏ về
    // một tầng ĐỨNG TRƯỚC nó qua `parent`. StageRunner kiểm tra lại điều này.
    std::vector<AiStage> stages;
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
