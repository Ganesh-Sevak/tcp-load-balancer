#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace lb {

class OutputBuffer {
public:
    explicit OutputBuffer(std::size_t compact_threshold = 256 * 1024);

    [[nodiscard]] bool empty() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t retained_capacity() const;
    [[nodiscard]] std::span<const char> readable_span() const;

    void append(std::span<const char> bytes);
    void consume(std::size_t count);
    void clear();

private:
    void compact_if_worthwhile();

    std::vector<char> storage_;
    std::size_t read_offset_{0};
    std::size_t compact_threshold_{0};
};

}  // namespace lb
