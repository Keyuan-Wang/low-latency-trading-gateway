#include <mutex>
#include <array>
#include <optional>

template <typename T, std::size_t Capacity>
class MutexRingBuffer {
public:
    static_assert(Capacity >= 2);

    bool push(const T& value) {
        std::lock_guard<std::mutex> lock(mutex_);

        const std::size_t next_ = increment(head_);

        if (next_ == tail_)     return false;   // full

        buffer_[head_] = value;
        head_ = next_;
        return true;
    }


    std::optional<T> pop() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (head_ == tail_)     return std::nullopt;   // empty

        const T value = buffer_[tail_];

        tail_ = increment(tail_);

        return value;
    }
    
    bool empty() const { return tail_ == head_; }

    bool full() const { return increment(head_) == tail_; }

private:
    static constexpr std::size_t increment(std::size_t index) {
        return (index + 1) % Capacity;
    }


    std::array<T, Capacity> buffer_{};
    std::size_t tail_ = 0;
    std::size_t head_ = 0;
    std::mutex mutex_;
};