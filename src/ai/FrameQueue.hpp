#ifndef AI_ENGINE_FRAME_QUEUE_HPP
#define AI_ENGINE_FRAME_QUEUE_HPP

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

// Bounded queue with drop-oldest overflow. For live AI we always want the
// freshest frame: when a slow worker can't keep up, old frames are discarded
// rather than stalling the decoder. Capacity 1 means "latest frame wins".
template <class T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity = 1)
        : m_capacity(capacity == 0 ? 1 : capacity) {}

    // Never blocks. Drops the oldest item(s) if the queue is already full.
    void push(T item) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_closed) return;
            while (m_queue.size() >= m_capacity) {
                m_queue.pop_front();
                ++m_dropped;
            }
            m_queue.push_back(std::move(item));
        }
        m_cv.notify_one();
    }

    // Blocks until an item is available or the queue is closed and drained.
    // Returns false only when closed and empty.
    bool pop(T& out) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return m_closed || !m_queue.empty(); });
        if (m_queue.empty()) return false;
        out = std::move(m_queue.front());
        m_queue.pop_front();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_closed = true;
        }
        m_cv.notify_all();
    }

    unsigned long droppedCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_dropped;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<T> m_queue;
    size_t m_capacity;
    bool m_closed = false;
    unsigned long m_dropped = 0;
};

#endif  // AI_ENGINE_FRAME_QUEUE_HPP
