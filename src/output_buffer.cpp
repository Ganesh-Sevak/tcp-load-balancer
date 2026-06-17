#include "lb/output_buffer.hpp"

#include <algorithm>
#include <stdexcept>

namespace lb {

OutputBuffer::OutputBuffer(std::size_t compact_threshold) : compact_threshold_(compact_threshold) {}

bool OutputBuffer::empty() const {
    return size() == 0;
}

std::size_t OutputBuffer::size() const {
    return storage_.size() - read_offset_;
}

std::size_t OutputBuffer::retained_capacity() const {
    return storage_.capacity();
}

std::span<const char> OutputBuffer::readable_span() const {
    if (empty()) {
        return {};
    }
    return {storage_.data() + read_offset_, size()};
}

void OutputBuffer::append(std::span<const char> bytes) {
    if (bytes.empty()) {
        return;
    }

    compact_if_worthwhile();
    storage_.insert(storage_.end(), bytes.begin(), bytes.end());
}

void OutputBuffer::consume(std::size_t count) {
    if (count > size()) {
        throw std::out_of_range("output buffer consume exceeds readable size");
    }

    read_offset_ += count;
    if (read_offset_ == storage_.size()) {
        clear();
        return;
    }

    compact_if_worthwhile();
}

void OutputBuffer::clear() {
    storage_.clear();
    read_offset_ = 0;
}

void OutputBuffer::compact_if_worthwhile() {
    if (read_offset_ < compact_threshold_ || read_offset_ < storage_.size() / 2) {
        return;
    }

    storage_.erase(storage_.begin(), storage_.begin() + static_cast<std::ptrdiff_t>(read_offset_));
    read_offset_ = 0;
}

}  // namespace lb
