#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_number_format.h"

#include "util/http_text_overlap.h"

// The single HTTP/1 wire-emission toolkit: sinks, the field-line emitter, and
// the overlap-safe writer used by every pre-measured head/trailer writer.

namespace ruvia::detail {

// Unchecked sink writing through a raw cursor. The caller measures the output
// first and guarantees capacity; constant-size appends inline to stores.
class http_raw_wire_sink final {
public:
    explicit http_raw_wire_sink(char* output) noexcept
        : out_(output) {}

    void append(std::string_view value) noexcept {
        // A default-constructed empty string_view may carry a null data pointer.
        // libc annotates memcpy arguments as nonnull even when the byte count is
        // zero, so avoid passing that representation across the C boundary.
        if (value.empty()) {
            return;
        }
        std::memcpy(out_, value.data(), value.size());
        out_ += value.size();
    }

    void append(char value) noexcept {
        *out_++ = value;
    }

    void append_unsigned(std::uint64_t value) noexcept {
        // The measured output reserves only http_unsigned_decimal_size(value)
        // bytes, so formatting in place could form a pointer past the caller's
        // buffer when the digits end it. Format locally and copy.
        std::array<char, http_max_unsigned_decimal_size> digits;
        append(http_format_unsigned(digits, value));
    }

    [[nodiscard]] char* position() const noexcept {
        return out_;
    }

private:
    char* out_;
};

// Growing sink appending to a PMR string.
class http_string_wire_sink final {
public:
    explicit http_string_wire_sink(std::pmr::string& output) noexcept
        : out_(&output) {}

    void append(std::string_view value) {
        out_->append(value);
    }

    void append(char value) {
        out_->push_back(value);
    }

    void append_unsigned(std::uint64_t value) {
        http_append_unsigned(*out_, value);
    }

private:
    std::pmr::string* out_;
};

// The single HTTP/1 field-line emitter: "name: value\r\n". Its size is
// name.size() + value.size() + http_field_line_overhead.
template <typename sink_type>
void emit_http1_field_line(sink_type& sink, std::string_view name, std::string_view value) {
    sink.append(name);
    sink.append(std::string_view(": "));
    sink.append(value);
    sink.append(std::string_view("\r\n"));
}

// Whether any borrowed input of a field-section write overlaps `output`: the
// field descriptor array itself, every field name and value, and `texts`
// (start-line pieces such as a method, target or host).
[[nodiscard]] inline bool http_wire_inputs_overlap(std::string_view output,
    std::span<const http_header_view> fields, std::initializer_list<std::string_view> texts = {}) noexcept {
    for (const auto text : texts) {
        if (http_text_overlaps(text, output)) {
            return true;
        }
    }
    if (http_text_overlaps(
            std::string_view(reinterpret_cast<const char*>(fields.data()), fields.size_bytes()), output)) {
        return true;
    }
    for (const auto& field : fields) {
        if (http_text_overlaps(field.name(), output) || http_text_overlaps(field.value(), output)) {
            return true;
        }
    }
    return false;
}

// A pre-measured encoder that ended anywhere but `expected` has already written
// past the measured bytes or left some of them unwritten; neither the memory
// nor the output can be trusted, and the noexcept writers cannot report it.
inline void check_http_wire_end(const char* end, const char* expected) noexcept {
    if (end != expected) {
        std::terminate();
    }
}

// Keep the bounded stack snapshot off the ordinary non-overlapping call path.
template <typename encode_type>
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void write_http_wire_snapshot(std::span<char> output, const encode_type& encode) noexcept {
    std::array<char, max_http_header_bytes> scratch;
    check_http_wire_end(encode(scratch.data()), scratch.data() + output.size());
    std::memcpy(output.data(), scratch.data(), output.size());
}

// The single overlap-safe writer. `encode(char*)` writes exactly output.size()
// bytes, which validation bounded to max_http_header_bytes, and returns its end
// position. When its inputs overlap `output` (see http_wire_inputs_overlap) the
// bytes are encoded into a bounded snapshot first, so every borrowed input,
// including the field descriptors, is consumed before any output byte is
// overwritten.
template <typename encode_type>
void write_http_wire(std::span<char> output, bool inputs_overlap, const encode_type& encode) noexcept {
    if (inputs_overlap) {
        write_http_wire_snapshot(output, encode);
    } else {
        check_http_wire_end(encode(output.data()), output.data() + output.size());
    }
}

}  // namespace ruvia::detail
