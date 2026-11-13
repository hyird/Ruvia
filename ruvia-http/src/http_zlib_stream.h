#pragma once

#include <zlib.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_datagram.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/url_encoding.h"

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

#pragma once

namespace ruvia::detail {

struct http_content_coding_field_result_access final {
    using sequence_type = http_content_coding_field_result::content_coding_sequence;

    [[nodiscard]] static constexpr http_content_coding_field_result coding(
        sequence_type values) noexcept {
        return http_content_coding_field_result(values);
    }

    [[nodiscard]] static constexpr http_content_coding_field_result unsupported(
        bool depth_exceeded = false) noexcept {
        return http_content_coding_field_result(http_unsupported_content_coding(depth_exceeded));
    }

    [[nodiscard]] static constexpr http_content_coding_field_result invalid() noexcept {
        return http_content_coding_field_result(http_invalid_content_coding_field{});
    }
};

// Shared token walk for decoding and outbound acceptance. The caller decides
// whether an unknown extension requires a decoder or only a quality check.
template <typename visitor_type>
[[nodiscard]] inline http_field_list_parse_status visit_http_content_coding_field(
    std::string_view value, http_field_list_role role, visitor_type&& visitor) {
    return http_visit_token_list(value, role, [&visitor](std::string_view token) {
        return visitor(token, http_content_coding_from_token(token));
    });
}

// Accumulates recipient list grammar across every Content-Encoding field line;
// empty list members are ignored.
class http_content_coding_field_parser final {
public:
    constexpr http_content_coding_field_parser() noexcept = default;

    void update(std::string_view value) noexcept;

    [[nodiscard]] http_content_coding_field_result finish() const noexcept;

private:
    http_content_coding_field_result_access::sequence_type codings_{};
    bool unknown_coding_{false};
    bool depth_exceeded_{false};
    bool invalid_{false};
};

// Validate list grammar without selecting decoders. Recipients/forwarders may
// retain empty members, but every nonempty member must still be a coding token.
[[nodiscard]] bool is_valid_http_content_encoding_field_value(
    std::string_view value, http_field_list_role role) noexcept;

template <typename headers_type>
[[nodiscard]] inline http_content_coding_field_result http_content_coding_from_headers(
    const headers_type& headers) noexcept {
    http_content_coding_field_parser parser;
    for (const auto& header : headers) {
        if (http_ascii_equals_ignore_case(header.name(), "Content-Encoding")) {
            parser.update(header.value());
        }
    }
    return parser.finish();
}

}  // namespace ruvia::detail

// A reusable codec slot: native compression libraries keep pointers into
// their stream, so the codec lives in one stable heap allocation.
// `codec_type` provides initialized() and reset().

namespace ruvia::detail {

// The slot's codec, created on first use. A codec that failed to initialize is
// not kept, so the next use retries; nullptr reports that failure.
template <typename codec_type, typename... args_type>
[[nodiscard]] codec_type* acquire_codec(codec_type*& slot, args_type&&... args) {
    if (slot == nullptr) {
        auto* created = new codec_type(std::forward<args_type>(args)...);
        if (!created->initialized()) {
            delete created;
            return nullptr;
        }
        slot = created;
    }
    return slot;
}

// Returns an acquired slot's codec to its initial state when the scope ends
// normally. A codec left by an exception, or one that cannot reset, is
// dropped and recreated by the next use: failed native state is never reused.
template <typename codec_type>
class codec_reset_guard final {
public:
    explicit codec_reset_guard(codec_type*& slot) noexcept
        : slot_(slot),
          uncaught_exceptions_(std::uncaught_exceptions()) {}
    codec_reset_guard(const codec_reset_guard&) = delete;
    codec_reset_guard& operator=(const codec_reset_guard&) = delete;

    ~codec_reset_guard() {
        if (std::uncaught_exceptions() != uncaught_exceptions_ || !slot_->reset()) {
            delete std::exchange(slot_, nullptr);
        }
    }

private:
    codec_type*& slot_;
    int uncaught_exceptions_;
};

}  // namespace ruvia::detail

#pragma once

namespace ruvia::detail {

enum class http_content_length_parse_status : std::uint8_t { ok,
    invalid,
    conflicting };

// Incremental Content-Length accumulator. Accepts comma-combined values with
// OWS and commits only when every repeated value agrees.
class http_content_length_state final {
public:
    [[nodiscard]] http_content_length_parse_status parse_field(std::string_view field_value) noexcept {
        auto status = http_content_length_parse_status::ok;
        bool saw_value = false;
        auto parsed_value = value_;
        http_visit_comma_separated_quoted_field_items(field_value, [&parsed_value, &status, &saw_value](
                                                                       std::string_view item) noexcept {
            status = accumulate(item, parsed_value);
            saw_value = true;
            return status == http_content_length_parse_status::ok;
        });
        if (status == http_content_length_parse_status::ok && !saw_value) {
            return http_content_length_parse_status::invalid;
        }
        if (status == http_content_length_parse_status::ok) {
            value_ = parsed_value;
        }
        return status;
    }

    [[nodiscard]] http_content_length_parse_status parse_single_value(std::string_view input) noexcept {
        auto parsed = value_;
        const auto status = accumulate(input, parsed);
        if (status == http_content_length_parse_status::ok) {
            value_ = parsed;
        }
        return status;
    }

    [[nodiscard]] std::optional<std::uint64_t> value() const noexcept {
        return value_;
    }

private:
    [[nodiscard]] static http_content_length_parse_status accumulate(
        std::string_view item, std::optional<std::uint64_t>& value) noexcept {
        if (item.empty()) {
            return http_content_length_parse_status::invalid;
        }
        std::uint64_t parsed_value = 0;
        const auto [end, ec] = std::from_chars(item.data(), item.data() + item.size(), parsed_value);
        if (ec != std::errc{} || end != item.data() + item.size()) {
            return http_content_length_parse_status::invalid;
        }
        if (value.has_value() && *value != parsed_value) {
            return http_content_length_parse_status::conflicting;
        }
        value = parsed_value;
        return http_content_length_parse_status::ok;
    }

    std::optional<std::uint64_t> value_;
};

}  // namespace ruvia::detail

// One compression library binding per coding, behind a uniform signature: decode
// or encode a whole buffer, bounded by an
// explicit output ceiling. Decode reports data errors as values; encode returns
// nullopt only for the ceiling. Both throw http_content_codec_error when the
// codec itself cannot run. Which coding a field asks for is decided elsewhere;
// this is only the machinery each one runs on. decode_http_content maps an
// empty representation to empty content before any decoder below runs.

namespace ruvia::detail {

class brotli_encoder_stream;
class zlib_deflate_stream;
class zstd_encoder_stream;

// Append decoder output while enforcing the ceiling; false means the ceiling
// would be exceeded and the decode must fail.
[[nodiscard]] inline bool append_decoded_bytes(
    std::string& output, const char* bytes_value, std::size_t size, std::size_t max_decoded_bytes) {
    if (output.size() > max_decoded_bytes || size > max_decoded_bytes - output.size()) {
        return false;
    }
    output.append(bytes_value, size);
    return true;
}

template <typename stream_type, typename operation_type>
[[nodiscard]] inline std::optional<std::string> encode_whole_content_with_stream(
    stream_type& encoder, operation_type finish_operation, std::string_view input,
    std::size_t max_encoded_bytes) {
    std::string output;
    if (!encoder.encode(input, output, finish_operation, max_encoded_bytes)) {
        return std::nullopt;
    }
    return output;
}

template <typename stream_type, typename operation_type, typename... arg_types>
[[nodiscard]] inline std::optional<std::string> encode_whole_content(
    const char* init_error, operation_type finish_operation, std::string_view input,
    std::size_t max_encoded_bytes, arg_types&&... args) {
    stream_type encoder(std::forward<arg_types>(args)...);
    if (!encoder.initialized()) {
        throw http_content_codec_error(init_error);
    }
    return encode_whole_content_with_stream(
        encoder, finish_operation, input, max_encoded_bytes);
}

[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_gzip_content(
    std::string_view input, std::size_t max_decoded_bytes);
[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_deflate_content(
    std::string_view input, std::size_t max_decoded_bytes);
[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_brotli_content(
    std::string_view input, std::size_t max_decoded_bytes);
[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_zstd_content(
    std::string_view input, std::size_t max_decoded_bytes);

[[nodiscard]] std::optional<std::string> encode_gzip_content(
    std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_deflate_content(
    std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_brotli_content(
    std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_zstd_content(
    std::string_view input, std::size_t max_encoded_bytes);

// A whole-buffer encode, one finishing pass of the stream's single encode
// loop, over caller-owned codec state that is freshly initialized or reset.
// Every path leaves the state consumed (finished or mid-stream), so an owner
// that reuses it resets it afterwards.
[[nodiscard]] std::optional<std::string> encode_zlib_content(zlib_deflate_stream& deflater,
    std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_brotli_content(brotli_encoder_stream& encoder,
    std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_zstd_content(zstd_encoder_stream& encoder,
    std::string_view input, std::size_t max_encoded_bytes);
}  // namespace ruvia::detail

// zlib inflate/deflate state with tracked working memory, including the LZ77
// window. zlib keeps back-pointers to both the z_stream and the allocator
// context, so these types are pinned: owners embed them in storage whose
// address never changes.
//
// Construction rethrows an allocation failure captured during initialization.
// Any other zlib refusal leaves the stream uninitialized so each owner maps it
// into its own error contract.
//
// reset() returns an initialized stream to its freshly initialized state while
// keeping its working memory, so the next unit of work produces the same bytes
// as a new stream. false means the native state is unusable and the owner must
// destroy the stream.

namespace ruvia::detail {

// zlib's own default; deflate memory is about (1 << (window_bits + 2)) +
// (1 << (mem_level + 9)) bytes, plus the zlib >= 1.2.12 pending buffer.
inline constexpr int zlib_default_mem_level = 8;

// zlib window parameters of the gzip (RFC 1952) and deflate (RFC 9110 zlib
// wrapper) content codings.
inline constexpr int zlib_gzip_window_bits = 15 + 16;
inline constexpr int zlib_deflate_window_bits = 15;

enum class zlib_content_format : unsigned char { gzip,
    deflate };

// Supplies the next window of `input` once zlib has consumed the previous
// one: inputs can exceed zlib's 32-bit avail_in, so they are handed over in
// windows instead of truncated. `supplied` counts the bytes handed over so far.
inline void zlib_refill_input(z_stream& stream, std::string_view input, std::size_t& supplied) noexcept {
    if (stream.avail_in != 0 || supplied == input.size()) {
        return;
    }
    const auto count = static_cast<uInt>(
        std::min<std::size_t>(input.size() - supplied, (std::numeric_limits<uInt>::max)()));
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data() + supplied));
    stream.avail_in = count;
    supplied += count;
}

// zlib keeps the caller's input and output pointers in the z_stream. The
// borrowed buffers only live for one call, so the scope clears them when that
// call ends, including by exception.
class zlib_borrow_scope final {
public:
    explicit zlib_borrow_scope(z_stream& stream) noexcept
        : stream_(stream) {}
    ~zlib_borrow_scope() {
        stream_.next_in = nullptr;
        stream_.avail_in = 0;
        stream_.next_out = nullptr;
        stream_.avail_out = 0;
    }
    zlib_borrow_scope(const zlib_borrow_scope&) = delete;
    zlib_borrow_scope& operator=(const zlib_borrow_scope&) = delete;

private:
    z_stream& stream_;
};

class zlib_inflate_stream final {
public:
    explicit zlib_inflate_stream(
        int window_bits,
        std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)());
    ~zlib_inflate_stream();
    zlib_inflate_stream(const zlib_inflate_stream&) = delete;
    zlib_inflate_stream& operator=(const zlib_inflate_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return stream_.state != nullptr;
    }
    [[nodiscard]] z_stream& native() noexcept {
        return stream_;
    }
    void rethrow_allocation_failure() const {
        allocation_.rethrow_if_failed();
    }
    [[nodiscard]] bool reset() noexcept;

private:
    http_c_library_memory allocation_;
    z_stream stream_;
};

class zlib_deflate_stream final {
public:
    zlib_deflate_stream(
        int level, int window_bits, int mem_level, int strategy,
        std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)());
    // The gzip or deflate content-coding stream with zlib's default parameters.
    explicit zlib_deflate_stream(
        zlib_content_format format,
        std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)());
    ~zlib_deflate_stream();
    zlib_deflate_stream(const zlib_deflate_stream&) = delete;
    zlib_deflate_stream& operator=(const zlib_deflate_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return stream_.state != nullptr;
    }
    [[nodiscard]] z_stream& native() noexcept {
        return stream_;
    }
    void rethrow_allocation_failure() const {
        allocation_.rethrow_if_failed();
    }
    [[nodiscard]] bool reset() noexcept;
    // The single content-coding deflate loop, shared by whole-buffer and
    // streaming coding: consumes the input and drains `flush` (Z_NO_FLUSH,
    // Z_SYNC_FLUSH or Z_FINISH) into output. False means output would exceed
    // `max_encoded_bytes` in total; no byte past that cap is appended. After
    // false or an exception, the owner must reset or discard this stream.
    [[nodiscard]] bool encode(std::string_view input, std::string& output, int flush,
        std::size_t max_encoded_bytes);

private:
    http_c_library_memory allocation_;
    z_stream stream_;
};

}  // namespace ruvia::detail

#pragma once

namespace ruvia::detail {

[[nodiscard]] inline bool http_parse_transfer_coding_syntax(
    std::string_view item, std::string_view& coding, bool& has_parameters) noexcept {
    const auto first_semicolon = http_find_unquoted_delimiter(item, 0, ';');
    coding = http_trim_ows(item.substr(0, first_semicolon));
    if (!is_valid_http_header_name(coding)) {
        return false;
    }

    has_parameters = first_semicolon < item.size();
    return http_all_parameters(item, is_valid_http_transfer_parameter);
}

template <http_temporary_owning_char_string item_type>
bool http_parse_transfer_coding_syntax(item_type&&, std::string_view&, bool&) = delete;

enum class http_transfer_encoding_parse_status : std::uint8_t { ok,
    malformed,
    unsupported };

class http_non_chunked_transfer_encoding final {
public:
    [[nodiscard]] const http_transfer_codings& transfer_codings() const noexcept {
        return transfer_codings_;
    }

private:
    friend class http_transfer_encoding_value;

    explicit http_non_chunked_transfer_encoding(http_transfer_codings transfer_codings)
        : transfer_codings_(std::move(transfer_codings)) {}

    http_transfer_codings transfer_codings_;
};

class http_final_chunked_transfer_encoding final {
public:
    [[nodiscard]] const http_transfer_codings& transfer_codings() const noexcept {
        return transfer_codings_;
    }

private:
    friend class http_transfer_encoding_value;

    explicit http_final_chunked_transfer_encoding(http_transfer_codings transfer_codings)
        : transfer_codings_(std::move(transfer_codings)) {}

    http_transfer_codings transfer_codings_;
};

class http_transfer_encoding_value final {
public:
    [[nodiscard]] const http_non_chunked_transfer_encoding* non_chunked() const& noexcept {
        return std::get_if<http_non_chunked_transfer_encoding>(&value_);
    }
    const http_non_chunked_transfer_encoding* non_chunked() const&& = delete;

    [[nodiscard]] const http_final_chunked_transfer_encoding* final_chunked() const& noexcept {
        return std::get_if<http_final_chunked_transfer_encoding>(&value_);
    }
    const http_final_chunked_transfer_encoding* final_chunked() const&& = delete;

private:
    friend class http_transfer_encoding_state;

    using value_type = std::variant<http_non_chunked_transfer_encoding, http_final_chunked_transfer_encoding>;

    [[nodiscard]] static http_transfer_encoding_value make_non_chunked(
        http_transfer_codings transfer_codings) {
        return http_transfer_encoding_value(http_non_chunked_transfer_encoding(std::move(transfer_codings)));
    }

    [[nodiscard]] static http_transfer_encoding_value make_final_chunked(
        http_transfer_codings transfer_codings) {
        return http_transfer_encoding_value(http_final_chunked_transfer_encoding(std::move(transfer_codings)));
    }

    explicit http_transfer_encoding_value(http_non_chunked_transfer_encoding value)
        : value_(std::move(value)) {}

    explicit http_transfer_encoding_value(http_final_chunked_transfer_encoding value)
        : value_(std::move(value)) {}

    value_type value_;
};

static_assert(std::is_nothrow_move_constructible_v<http_transfer_encoding_value>);

// Field lines form one ordered recipient-side list. RFC 9110 section 5.6.1.2
// requires ignoring empty members, including on repeated field lines. Unknown
// codings are remembered while later items are still checked for malformed syntax.
class http_transfer_encoding_state final {
public:
    [[nodiscard]] http_transfer_encoding_parse_status parse_field(std::string_view field_value) {
        // The published value is the sole owner of the committed coding list.
        http_transfer_codings next_codings;
        bool final_chunked = false;
        if (value_) {
            if (const auto* chunked = value_->final_chunked()) {
                next_codings = chunked->transfer_codings();
                final_chunked = true;
            } else {
                next_codings = value_->non_chunked()->transfer_codings();
            }
        }
        bool unsupported = unsupported_;
        bool malformed = false;
        http_visit_comma_separated_quoted_field_items(field_value,
            [&next_codings, &final_chunked, &unsupported, &malformed](std::string_view item) {
                if (item.empty()) {
                    return true;
                }
                std::string_view coding;
                bool has_parameters = false;
                if (final_chunked || !http_parse_transfer_coding_syntax(item, coding, has_parameters)) {
                    malformed = true;
                    return false;
                }
                if (http_ascii_equals_ignore_case(coding, "chunked")) {
                    if (has_parameters) {
                        malformed = true;
                        return false;
                    }
                    final_chunked = true;
                    return true;
                }
                const bool gzip = http_is_gzip_coding_token(coding);
                const bool deflate = http_ascii_equals_ignore_case(coding, "deflate");
                if (gzip || deflate) {
                    if (has_parameters) {
                        malformed = true;
                        return false;
                    }
                    if (next_codings.size() == max_transfer_codings) {
                        unsupported = true;
                        return true;
                    }
                    next_codings.push_back(gzip ? http_transfer_coding::gzip
                                                : http_transfer_coding::deflate);
                    return true;
                }
                unsupported = true;
                return true;
            });
        if (malformed) {
            return http_transfer_encoding_parse_status::malformed;
        }
        auto next_value = final_chunked ? http_transfer_encoding_value::make_final_chunked(std::move(next_codings))
                                        : http_transfer_encoding_value::make_non_chunked(std::move(next_codings));
        // The coding list is inline, so publication cannot fail.
        value_.emplace(std::move(next_value));
        unsupported_ = unsupported;
        return unsupported_ ? http_transfer_encoding_parse_status::unsupported
                            : http_transfer_encoding_parse_status::ok;
    }

    [[nodiscard]] const std::optional<http_transfer_encoding_value>& value() const noexcept {
        return value_;
    }
    [[nodiscard]] bool unsupported() const noexcept {
        return unsupported_;
    }

private:
    std::optional<http_transfer_encoding_value> value_;
    bool unsupported_{false};
};

}  // namespace ruvia::detail
