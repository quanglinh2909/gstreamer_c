#ifndef AI_ENGINE_DMA_HEAP_BUFFER_HPP
#define AI_ENGINE_DMA_HEAP_BUFFER_HPP

// CPU-readable RGA destination memory allocated from the kernel dma-heap.
//
// Why not plain malloc'd memory: handing RGA a raw virtual address makes the
// driver pin the pages with get_user_pages on every blit — and when the
// source pointer is itself an mmap'd dmabuf (the MPP decoder's frame),
// pinning that PFN-mapped vma intermittently yields a bogus sg entry and the
// kernel oopses in __clean_dcache_area_poc (observed 2026-06-12 on
// 5.10.160-rockchip-rk3588, thread "mppvideodec3:sr", sometimes freezing
// the whole board). Importing user memory once (importbuffer_virtualaddr)
// avoids the oops but the driver then skips per-job cache maintenance, so
// CPU reads of DMA-written data hit stale cache lines (posterised JPEGs).
//
// dma-heap fixes both: RGA attaches the fd like any other dmabuf (no page
// pinning), and DMA_BUF_IOCTL_SYNC gives the CPU a proper cache invalidate
// before it reads what the hardware wrote.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <mutex>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#include <rga/im2d.h>

#include "RgaLock.hpp"

namespace rga {

class DmaHeapBuffer {
public:
    DmaHeapBuffer() = default;
    ~DmaHeapBuffer() { release(); }

    DmaHeapBuffer(const DmaHeapBuffer&) = delete;
    DmaHeapBuffer& operator=(const DmaHeapBuffer&) = delete;

    // Ensures a buffer registered with RGA as wstride x hstride of `format`,
    // at least `bytes` long. Reallocates only when the geometry changes.
    // Returns false when dma-heap is unavailable (caller falls back).
    bool ensure(int wstride, int hstride, int format, size_t bytes) {
        if (m_ptr && m_handle && m_w == wstride && m_h == hstride &&
            m_fmt == format && m_bytes >= bytes) {
            return true;
        }
        release();

        const int heap = heapFd();
        if (heap < 0) return false;

        struct dma_heap_allocation_data alloc = {};
        alloc.len = bytes;
        alloc.fd_flags = O_RDWR | O_CLOEXEC;
        if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
            std::perror("dma-heap alloc");
            return false;
        }
        m_fd = static_cast<int>(alloc.fd);

        m_ptr = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                     m_fd, 0);
        if (m_ptr == MAP_FAILED) {
            std::perror("dma-heap mmap");
            m_ptr = nullptr;
            release();
            return false;
        }
        m_bytes = bytes;
        m_w = wstride;
        m_h = hstride;
        m_fmt = format;

        im_handle_param_t param;
        param.width = static_cast<uint32_t>(wstride);
        param.height = static_cast<uint32_t>(hstride);
        param.format = static_cast<uint32_t>(format);
        {
            std::lock_guard<std::mutex> lock(rgaMutex());
            m_handle = importbuffer_fd(m_fd, &param);
        }
        if (!m_handle) {
            std::fprintf(stderr, "dma-heap: rga import failed (%dx%d)\n",
                         wstride, hstride);
            release();
            return false;
        }
        return true;
    }

    bool valid() const { return m_ptr != nullptr && m_handle != 0; }
    uint8_t* data() { return static_cast<uint8_t*>(m_ptr); }
    const uint8_t* data() const { return static_cast<const uint8_t*>(m_ptr); }
    size_t size() const { return m_bytes; }
    // The underlying dmabuf fd. Hand this to another hardware block (e.g.
    // mppjpegenc via GstDmaBufAllocator) so it imports the buffer properly
    // instead of get_user_pages'ing our mmap — the latter faults the codec
    // IOMMU exactly like it faulted RGA. The fd stays owned here; consumers
    // must NOT close it (dup or pass DONT_CLOSE).
    int fd() const { return m_fd; }

    rga_buffer_t wrap(int width, int height) {
        return wrapbuffer_handle(m_handle, width, height, m_fmt, m_w, m_h);
    }

    // Cache handshake after a hardware write: invalidates the CPU cache for
    // this buffer so the next CPU read sees the DMA-written bytes. Safe to
    // call before every read; the data stays valid until the next blit.
    void syncForCpuRead() {
        struct dma_buf_sync s;
        s.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
        ioctl(m_fd, DMA_BUF_IOCTL_SYNC, &s);
        s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        ioctl(m_fd, DMA_BUF_IOCTL_SYNC, &s);
    }

private:
    // One shared heap fd for the process; /dev/dma_heap/system is group
    // "video" on this board, readable without root.
    static int heapFd() {
        static int fd = [] {
            int f = ::open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
            if (f < 0) std::perror("open /dev/dma_heap/system");
            return f;
        }();
        return fd;
    }

    void release() {
        if (m_handle) {
            std::lock_guard<std::mutex> lock(rgaMutex());
            releasebuffer_handle(m_handle);
            m_handle = 0;
        }
        if (m_ptr) {
            munmap(m_ptr, m_bytes);
            m_ptr = nullptr;
        }
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
        m_bytes = 0;
        m_w = m_h = m_fmt = 0;
    }

    int m_fd = -1;
    void* m_ptr = nullptr;
    size_t m_bytes = 0;
    rga_buffer_handle_t m_handle = 0;
    int m_w = 0;
    int m_h = 0;
    int m_fmt = 0;
};

}  // namespace rga

#endif  // AI_ENGINE_DMA_HEAP_BUFFER_HPP
