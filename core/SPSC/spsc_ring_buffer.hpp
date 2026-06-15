#include <vector>

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity)
    : capacity_(capacity + 1)
    , buffer_(capacity)
    , read_pos_(0)
    , write_pos_(0) {};

    // try to put an element, if full then return false
    // if you want to keep u, pass it in directly
    // if you want to move u to the buffer, use std::move()
    template <typename U>
    bool push(U&& item) {
        // the move operation of T must be noexcept
        static_assert(std::is_nothrow_move_assignable_v<T> || !std::is_move_assignable_v<T>,
            "T must have a noexcept move assign operator or no move assign");

        // find the pos of next empty slot
        std::size_t next = (write_pos_ + 1) % capacity_;

        if (next == read_pos_) {
            return false;   // full
        }

        buffer_[write_pos_] = std::forward<U>(item);

        write_pos_ = next;
        return true;
    }

    // try to pop an element, if empty thrn return false
    bool pop(T& item) {
        if (read_pos_ == write_pos_) {
            return false;   // empty
        }

        item = buffer_[read_pos_];

        read_pos_ = (read_pos_ + 1) % capacity_;

        return true;
    }

    bool empty() const { return read_pos_ == write_pos_; }

    bool full() const { return (write_pos_ + 1) % capacity_ == read_pos_; }

    std::size_t size() const {
        if (write_pos_ >= read_pos_)
            return write_pos_ - read_pos_;
        return capacity_ - read_pos_ + write_pos_;
    }

    std::size_t capacity() const { return capacity_ - 1; }

private:
    std::size_t capacity_;
    std::vector<T> buffer_;
    std::size_t read_pos_ = 0;
    std::size_t write_pos_ = 0;
};