#include "lb/output_buffer.hpp"

#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "test failed: " << message << "\n";
        std::exit(1);
    }
}

std::span<const char> bytes(const std::string& value) {
    return {value.data(), value.size()};
}

std::string readable_string(const lb::OutputBuffer& buffer) {
    const auto readable = buffer.readable_span();
    return {readable.data(), readable.size()};
}

void append_and_consume_preserves_order() {
    lb::OutputBuffer buffer;

    buffer.append(bytes("hello"));
    buffer.append(bytes(" world"));
    require(buffer.size() == 11, "buffer reports appended size");
    require(readable_string(buffer) == "hello world", "buffer preserves appended order");

    buffer.consume(6);
    require(buffer.size() == 5, "buffer reports remaining size");
    require(readable_string(buffer) == "world", "buffer exposes unread suffix");

    buffer.consume(5);
    require(buffer.empty(), "buffer empties after consuming all bytes");
    require(buffer.readable_span().empty(), "empty buffer exposes empty span");
}

void compacts_after_large_consumed_prefix() {
    lb::OutputBuffer buffer(4);

    buffer.append(bytes("abcdefghij"));
    const auto initial_capacity = buffer.retained_capacity();
    buffer.consume(6);
    require(readable_string(buffer) == "ghij", "buffer keeps unread bytes after compaction");
    require(buffer.retained_capacity() <= initial_capacity, "compaction does not grow capacity");

    buffer.append(bytes("kl"));
    require(readable_string(buffer) == "ghijkl", "append works after compaction");
}

void consume_rejects_overread() {
    lb::OutputBuffer buffer;
    buffer.append(bytes("abc"));

    bool rejected = false;
    try {
        buffer.consume(4);
    } catch (const std::out_of_range&) {
        rejected = true;
    }

    require(rejected, "over-consuming buffer is rejected");
    require(readable_string(buffer) == "abc", "failed consume leaves buffer unchanged");
}

}  // namespace

int main() {
    append_and_consume_preserves_order();
    compacts_after_large_consumed_prefix();
    consume_rejects_overread();

    std::cout << "output buffer tests passed\n";
    return 0;
}
