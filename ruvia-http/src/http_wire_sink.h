#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/http/http_datagram.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_status.h"

namespace ruvia::detail {

// Routes C protocol-library (nghttp2, nghttp3, ngtcp2) and compression-codec
// (zlib, Brotli, zstd) allocations with size tracking and exception capture.
// C free() carries no size, so each block stores its total size in a
// max-aligned prefix and realloc is emulated by allocate-copy-free. C callbacks
// must never throw: an allocation exception is reported as nullptr and retained
// so the C++ owner can rethrow it after the C call returns.
//
// `native_budget` caps the payload bytes live at once; a request past it is
// refused and remembered by budget_exceeded().
class http_c_library_memory final {
public:
    explicit http_c_library_memory(
        std::size_t native_budget = (std::numeric_limits<std::size_t>::max)(),
        bool throw_on_budget_exceeded = false) noexcept;
    http_c_library_memory(const http_c_library_memory&) = delete;
    http_c_library_memory& operator=(const http_c_library_memory&) = delete;
    void rethrow_if_failed() const;
    [[noreturn]] void rethrow_allocation_failure() const;
    void clear_allocation_failure() noexcept {
        failure_ = nullptr;
        budget_exceeded_ = false;
    }
    [[nodiscard]] bool budget_exceeded() const noexcept {
        return budget_exceeded_;
    }

    static void* allocate(std::size_t size, void* context) noexcept;
    static void deallocate(void* pointer, void* context) noexcept;
    static void* allocate_zeroed(std::size_t count, std::size_t size, void* context) noexcept;
    static void* reallocate(void* pointer, std::size_t size, void* context) noexcept;

    static void* allocate_opaque_first(void* context, std::size_t size) noexcept {
        return allocate(size, context);
    }
    static void deallocate_opaque_first(void* context, void* pointer) noexcept {
        deallocate(pointer, context);
    }

private:
    std::size_t native_budget_;
    std::size_t live_bytes_{0};
    mutable std::exception_ptr failure_;
    bool budget_exceeded_{false};
    bool throw_on_budget_exceeded_{false};
};

template <typename table_type>
[[nodiscard]] constexpr table_type make_c_library_mem_table(http_c_library_memory& memory) noexcept {
    return table_type{
        &memory,
        &http_c_library_memory::allocate,
        &http_c_library_memory::deallocate,
        &http_c_library_memory::allocate_zeroed,
        &http_c_library_memory::reallocate,
    };
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// Ensures capacity for `required` elements before a mutation that must not
// reallocate. Growth at least doubles the current capacity because an exact
// reserve(n) (libc++) would turn a sequence of small appends into quadratic copying.
template <typename container_type>
void reserve_geometric(container_type& container, std::size_t required) {
    const auto capacity = container.capacity();
    if (required <= capacity) {
        return;
    }
    const auto max_size = container.max_size();
    if (required > max_size) {
        throw std::length_error("container capacity overflow");
    }
    const auto doubled = capacity > max_size / 2 ? max_size : capacity * 2;
    container.reserve((std::max)(required, doubled));
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// Address intervals compare as unsigned integers: views into unrelated buffers
// are unrelated pointers, and subtraction avoids forming an overflowing end.
[[nodiscard]] inline bool http_text_overlaps(std::string_view left, std::string_view right) noexcept {
    if (left.empty() || right.empty()) {
        return false;
    }
    const auto left_address = reinterpret_cast<std::uintptr_t>(left.data());
    const auto right_address = reinterpret_cast<std::uintptr_t>(right.data());
    return left_address >= right_address ? left_address - right_address < right.size()
                                         : right_address - left_address < left.size();
}

// Whether `text` views bytes `storage` owns, its terminator included: growing or
// rewriting `storage` would destroy that input, so the caller owns a copy first.
[[nodiscard]] inline bool http_text_aliases_string(std::string_view text, const std::string& storage) noexcept {
    return http_text_overlaps(text, std::string_view(storage.data(), storage.capacity() + 1));
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// RFC 9000 §16 variable-length integer, shared by the Capsule Protocol (RFC
// 9297 §3.2) and HTTP/3 Datagram framing (RFC 9297 §2.1).
inline constexpr std::uint64_t quic_varint_max = (std::uint64_t{1} << 62) - 1;

struct quic_varint final {
    std::uint64_t value_;
    std::size_t encoded_bytes_;
};

[[nodiscard]] constexpr std::size_t quic_varint_size(std::uint64_t value) noexcept {
    return value < (std::uint64_t{1} << 6) ? 1 : value < (std::uint64_t{1} << 14) ? 2
                                             : value < (std::uint64_t{1} << 30)   ? 4
                                                                                  : 8;
}

[[nodiscard]] inline std::variant<quic_varint, http_capsule_codec_error> decode_quic_varint(
    std::span<const char> input) noexcept {
    if (input.empty()) {
        return http_capsule_codec_error::need_more_data;
    }
    const auto first = static_cast<unsigned char>(input.front());
    const std::size_t size = std::size_t{1} << (first >> 6);
    if (input.size() < size) {
        return http_capsule_codec_error::need_more_data;
    }
    std::uint64_t value = first & 0x3f;
    for (std::size_t i = 1; i < size; ++i) {
        value = (value << 8) | static_cast<unsigned char>(input[i]);
    }
    return quic_varint{value, size};
}

[[nodiscard]] inline std::variant<std::size_t, http_capsule_codec_error> encode_quic_varint(
    std::span<char> output, std::uint64_t value) noexcept {
    if (value > quic_varint_max) {
        return http_capsule_codec_error::value_out_of_range;
    }
    const auto size = quic_varint_size(value);
    if (output.size() < size) {
        return http_capsule_codec_error::output_too_small;
    }
    for (std::size_t i = size; i != 0; --i) {
        output[i - 1] = static_cast<char>(value & 0xff);
        value >>= 8;
    }
    const unsigned tag = size == 1 ? 0 : size == 2 ? 0x40
                                     : size == 4   ? 0x80
                                                   : 0xc0;
    output[0] = static_cast<char>(static_cast<unsigned char>(output[0]) | tag);
    return size;
}

}  // namespace ruvia::detail

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

// Growing sink appending to a string.
class http_string_wire_sink final {
public:
    explicit http_string_wire_sink(std::string& output) noexcept
        : out_(&output) {}

    void append(std::string_view value) {
        out_->append(value);
    }

private:
    std::string* out_;
};

// RFC 9110 section 6.2: a server sends the highest version it conforms to
// whose major version does not exceed the request's, so every HTTP/1 response
// (including one to an HTTP/1.0 request) says HTTP/1.1. HTTP/1.0 request rules
// such as close-delimited streams and the keep-alive token stay keyed on the
// request version.
inline constexpr std::string_view http11_status_line_prefix = "HTTP/1.1 ";

[[nodiscard]] constexpr std::size_t http1_status_line_size(std::string_view reason_phrase) noexcept {
    return http11_status_line_prefix.size() + http_status_code_token_size + 1 + reason_phrase.size() + 2;
}

// The single HTTP/1 status-line emitter (RFC 9112 section 4):
// HTTP-version SP status-code SP [ reason-phrase ] CRLF. Its size is
// http1_status_line_size(reason_phrase).
template <typename sink_type>
constexpr void emit_http1_status_line(sink_type& sink, http_status_code status, std::string_view reason_phrase) {
    sink.append(http11_status_line_prefix);
    const auto status_token = http_status_code_token(status);
    sink.append(http_status_code_token_view(status_token));
    // RFC 9112 requires this SP even when the optional reason phrase is empty.
    sink.append(' ');
    sink.append(reason_phrase);
    sink.append(std::string_view("\r\n"));
}

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
