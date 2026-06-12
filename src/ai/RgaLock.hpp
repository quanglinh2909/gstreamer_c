#ifndef AI_ENGINE_RGA_LOCK_HPP
#define AI_ENGINE_RGA_LOCK_HPP

#include <mutex>

namespace rga {

// All librga calls (blit, import, release) are serialised through one mutex.
// librga's virtual-address (MMU) path is not reliable when several threads
// submit blits at once — under multiple AI streams it fails with "RGA_BLIT
// Invalid argument". RGA is a single hardware engine anyway, so serialising
// costs little. Lives in its own header so FrameTypes (which must release an
// import handle in the Frame destructor) does not have to pull in the whole
// converter.
inline std::mutex& rgaMutex() {
    static std::mutex m;
    return m;
}

}  // namespace rga

#endif  // AI_ENGINE_RGA_LOCK_HPP
