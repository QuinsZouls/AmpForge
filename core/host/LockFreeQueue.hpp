#pragma once
#include <atomic>
#include <cstddef>
#include <array>

namespace ampforge {

// Lock-free Single-Producer Single-Consumer (SPSC) ring buffer.
// Capacity must be a power of two.
template <typename T, size_t Capacity = 512>
class SpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
public:
    SpscRingBuffer() : head(0), tail(0) {}

    bool push(const T& item) {
        const size_t currentHead = head.load(std::memory_order_relaxed);
        const size_t currentTail = tail.load(std::memory_order_acquire);
        if ((currentHead - currentTail) >= Capacity) {
            return false; // Full
        }
        buffer[currentHead & (Capacity - 1)] = item;
        head.store(currentHead + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const size_t currentTail = tail.load(std::memory_order_relaxed);
        const size_t currentHead = head.load(std::memory_order_acquire);
        if (currentTail == currentHead) {
            return false; // Empty
        }
        item = buffer[currentTail & (Capacity - 1)];
        tail.store(currentTail + 1, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return tail.load(std::memory_order_relaxed) == head.load(std::memory_order_relaxed);
    }

    size_t size() const {
        const size_t h = head.load(std::memory_order_relaxed);
        const size_t t = tail.load(std::memory_order_relaxed);
        return (h >= t) ? (h - t) : 0;
    }

private:
    std::array<T, Capacity> buffer;
    alignas(64) std::atomic<size_t> head;
    alignas(64) std::atomic<size_t> tail;
};

struct PluginParamUpdate {
    uint32_t id = 0;
    float value = 0.0f;
};

} // namespace ampforge
