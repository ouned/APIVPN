#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace apivpn {

class ByteBuffer {
public:
    ByteBuffer() = default;
    explicit ByteBuffer(size_t reserve_bytes) {
        data_.reserve(reserve_bytes);
    }

    void clear() {
        data_.clear();
        read_offset_ = 0;
        releaseExcessCapacity();
    }

    void append(const void* src, size_t len) {
        if (len == 0 || !src) return;
        if (read_offset_ > 0) {
            if (read_offset_ >= data_.size()) {
                data_.clear();
                read_offset_ = 0;
            } else if (read_offset_ >= 16384 && read_offset_ * 2 >= data_.size()) {
                compact();
            }
        }
        const auto* bytes = static_cast<const uint8_t*>(src);
        data_.insert(data_.end(), bytes, bytes + len);
    }

    template <typename T>
    void appendPod(const T& pod) {
        append(&pod, sizeof(T));
    }

    const uint8_t* data() const { return data_.data() + read_offset_; }
    uint8_t* data() { return data_.data() + read_offset_; }
    size_t size() const { return data_.size() - read_offset_; }
    bool empty() const { return read_offset_ >= data_.size(); }

    void consumeFront(size_t count) {
        if (count >= size()) {
            data_.clear();
            read_offset_ = 0;
            releaseExcessCapacity();
        } else if (count > 0) {
            read_offset_ += count;
            if (read_offset_ >= 16384 && read_offset_ * 2 >= data_.size()) {
                compact();
            }
        }
    }

private:
    // Once a burst has drained, return its memory: a single spike (e.g. a peer-list storm or a
    // relay burst to a slow reader) must not pin megabytes per session for the process lifetime.
    static constexpr size_t kRetainCapacityBytes = 1024 * 1024;
    void releaseExcessCapacity() {
        if (data_.capacity() > kRetainCapacityBytes) {
            std::vector<uint8_t>().swap(data_);
        }
    }

    void compact() {
        size_t rem = size();
        if (rem > 0 && read_offset_ > 0) {
            std::memmove(data_.data(), data_.data() + read_offset_, rem);
        }
        data_.resize(rem);
        read_offset_ = 0;
    }

    std::vector<uint8_t> data_;
    size_t               read_offset_ = 0;
};

} // namespace apivpn
