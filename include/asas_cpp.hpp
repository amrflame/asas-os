#ifndef ASAS_CPP_HPP
#define ASAS_CPP_HPP

#include "asas_libc.h"

namespace asas {

class HeapBuffer {
public:
    explicit HeapBuffer(UINTN size) : pointer_(asas_malloc(size)) {}
    ~HeapBuffer() { asas_free(pointer_); }

    HeapBuffer(const HeapBuffer &) = delete;
    HeapBuffer &operator=(const HeapBuffer &) = delete;

    void *data() const { return pointer_; }
    bool valid() const { return pointer_ != nullptr; }

private:
    void *pointer_;
};

template <typename T, UINTN Capacity>
class StaticVector {
public:
    StaticVector() : size_(0) {}

    bool push_back(const T &value)
    {
        if (size_ >= Capacity) {
            return false;
        }

        values_[size_++] = value;
        return true;
    }

    UINTN size() const { return size_; }
    const T &operator[](UINTN index) const { return values_[index]; }

private:
    T values_[Capacity];
    UINTN size_;
};

}

#endif

