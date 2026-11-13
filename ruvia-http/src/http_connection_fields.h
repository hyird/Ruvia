#pragma once

#include <zlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/http/websocket_handshake.h"

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

namespace ruvia::detail {

// Borrowed field descriptors stay inline until their reserved or appended count
// needs heap overflow. Views are rebound after moves, never cached.
template <typename field_type>
class http_header_descriptor_storage final {
    static_assert(std::is_trivially_copyable_v<field_type>);

public:
    http_header_descriptor_storage() noexcept = default;

    http_header_descriptor_storage(const http_header_descriptor_storage&) = delete;
    http_header_descriptor_storage& operator=(const http_header_descriptor_storage&) = delete;
    http_header_descriptor_storage(http_header_descriptor_storage&& other) noexcept
        : overflow_(std::exchange(other.overflow_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          capacity_(std::exchange(other.capacity_, inline_capacity)) {
        if (overflow_ == nullptr) {
            std::copy_n(other.inline_.data(), size_, inline_.data());
        }
    }
    http_header_descriptor_storage& operator=(http_header_descriptor_storage&&) = delete;

    ~http_header_descriptor_storage() {
        if (overflow_ != nullptr) {
            std::allocator<field_type>{}.deallocate(overflow_, capacity_);
        }
    }

    void reserve(std::size_t requested_capacity) {
        if (requested_capacity <= capacity_) {
            return;
        }
        reallocate(requested_capacity);
    }

    void append(field_type field) {
        if (size_ == capacity_) {
            grow();
        }
        auto* destination = overflow_ != nullptr ? overflow_ : inline_.data();
        destination[size_++] = field;
    }

    void clear() noexcept {
        size_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] std::span<const field_type> view() const& noexcept {
        return {overflow_ != nullptr ? overflow_ : inline_.data(), size_};
    }
    std::span<const field_type> view() const&& = delete;

private:
    static constexpr std::size_t inline_capacity = 32;

    void grow() {
        constexpr auto maximum = std::numeric_limits<std::size_t>::max() / sizeof(field_type);
        if (capacity_ >= maximum) {
            throw std::length_error("too many HTTP header descriptors");
        }
        const auto next_capacity = capacity_ <= maximum / 2 ? capacity_ * 2 : maximum;
        reallocate(next_capacity);
    }

    void reallocate(std::size_t next_capacity) {
        constexpr auto maximum = std::numeric_limits<std::size_t>::max() / sizeof(field_type);
        if (next_capacity > maximum) {
            throw std::length_error("too many HTTP header descriptors");
        }
        std::allocator<field_type> allocator;
        auto* next = allocator.allocate(next_capacity);
        // Trivial descriptors cannot throw while copying. Publish only after
        // allocation succeeds, preserving the current fields on failure.
        const auto fields = view();
        std::uninitialized_copy_n(fields.data(), fields.size(), next);
        if (overflow_ != nullptr) {
            allocator.deallocate(overflow_, capacity_);
        }
        overflow_ = next;
        capacity_ = next_capacity;
    }

    // POD request slots stay uninitialized until appended. Every readable slot
    // is fully assigned before publication, without zeroing unused slots.
    std::array<field_type, inline_capacity> inline_;
    field_type* overflow_{nullptr};
    std::size_t size_{0};
    std::size_t capacity_{inline_capacity};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// The one Extended CONNECT WebSocket bootstrap field list for HTTP/2 (RFC
// 8441) and HTTP/3 (RFC 9220): the negotiation's Sec-WebSocket-* request
// fields, then the application headers. The descriptors borrow both.
[[nodiscard]] inline http_header_descriptor_storage<http_header_view> stage_websocket_extended_connect_fields(
    const websocket_client_negotiation& negotiation, std::span<const http_header_view> headers) {
    const auto negotiation_fields = negotiation.request_fields();
    http_header_descriptor_storage<http_header_view> staged;
    staged.reserve(negotiation_fields.size() + headers.size());
    for (const auto& field : negotiation_fields) {
        staged.append(http_header_view{field.name(), field.value()});
    }
    for (const auto& field : headers) {
        staged.append(field);
    }
    return staged;
}

}  // namespace ruvia::detail

// RFC 3986 syntax, one level below any HTTP-specific rule: which byte sequences
// form a legal URI component, userinfo, port, IPv4 / IPv6 / IPvFuture literal or
// reg-name. Nothing here knows about request targets, Host fields or origins --
// those rules live in http_request_target.h and http_serialized_origin.cpp.

namespace ruvia::detail {

[[nodiscard]] inline bool is_decimal_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] inline constexpr bool is_uri_lower_alpha(unsigned char byte) noexcept {
    return byte >= 'a' && byte <= 'z';
}

[[nodiscard]] inline constexpr bool is_uri_alpha(unsigned char byte) noexcept {
    return is_uri_lower_alpha(byte) || (byte >= 'A' && byte <= 'Z');
}

// RFC 3986 section 2.3 unreserved. Percent-encoding one of these octets is
// equivalent to spelling it literally, which host comparison relies on.
[[nodiscard]] inline constexpr bool is_unreserved_byte(unsigned char byte) noexcept {
    return (byte >= '0' && byte <= '9') || is_uri_alpha(byte) ||
           byte == '-' || byte == '.' || byte == '_' || byte == '~';
}

// Literal pchar bytes. The caller validates percent-encoded triplets separately.
[[nodiscard]] bool is_valid_uri_userinfo(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_uri_port(std::string_view value) noexcept;
[[nodiscard]] bool is_uri_pchar(unsigned char byte) noexcept;

// Split RFC 3986 host [ ":" port ] (host may be an IP-literal in brackets)
// without validating either part. False when brackets are unbalanced, text
// follows "]" without ":", or the port contains another ":".
[[nodiscard]] bool split_uri_host_port(
    std::string_view value, std::string_view& host, std::string_view& port, bool& has_port) noexcept;

// Parse a decimal port, rejecting anything that does not fit 16 bits.
[[nodiscard]] std::variant<std::uint16_t, std::errc> parse_port_value(std::string_view value) noexcept;

// Validate a percent-encoded component: unreserved / sub-delims / pct-encoded,
// plus ':' and '@', with '/' and '?' admitted only where the component allows them.
[[nodiscard]] bool is_valid_uri_component(
    std::string_view value, bool allow_slash, bool allow_question) noexcept;

// Address octets/groups in wire order; no allocation or platform IP parser.
// IPv6 and IPvFuture literals are supplied without surrounding brackets.
[[nodiscard]] std::optional<std::array<std::uint8_t, 4>> parse_ipv4_address(std::string_view value) noexcept;
[[nodiscard]] std::optional<std::array<std::uint16_t, 8>> parse_ipv6_address(std::string_view literal) noexcept;
[[nodiscard]] bool is_valid_ipv_future(std::string_view literal) noexcept;
[[nodiscard]] bool is_valid_reg_name(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_dns_domain_name(std::string_view name) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {

struct request_target_view {
    std::string_view scheme_;
    std::string_view path_;
    std::string_view query_;
    std::string_view authority_;
    http_request_target_form form_{http_request_target_form::origin};
};

// Validates the byte repertoire shared by the four HTTP request-target forms.
// Component-specific rules (for example, '[' and ']' being legal only in an
// IP-literal authority) are applied by parse_request_target /
// is_valid_origin_form_target.
[[nodiscard]] bool is_valid_request_target_bytes(std::string_view target) noexcept;

// RFC 3986 scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ).
[[nodiscard]] bool is_valid_uri_scheme(std::string_view value) noexcept;

[[nodiscard]] constexpr std::uint16_t http_scheme_default_port(http_scheme scheme) noexcept {
    return scheme == http_scheme::https ? std::uint16_t{443} : std::uint16_t{80};
}

[[nodiscard]] std::uint16_t http_uri_scheme_default_port(std::string_view scheme) noexcept;
[[nodiscard]] bool is_valid_uri_authority(std::string_view value) noexcept;

// Strict origin-form starts with '/'. Asterisk-form is a separate target form
// and is exposed explicitly so callers cannot lose its OPTIONS-only contract.
[[nodiscard]] bool is_valid_origin_form_target(std::string_view target) noexcept;
[[nodiscard]] bool is_valid_origin_or_asterisk_form_target(
    http_known_method method, std::string_view target) noexcept;

// Host field syntax. An empty field is valid when the target URI has no
// authority (RFC 9112 section 3.2); callers that require a usable HTTP origin
// must additionally require a non-empty value.
[[nodiscard]] bool is_valid_host_header(std::string_view value) noexcept;
// RFC 3986 `host` / HTTP `uri-host`, without a port. IP literals use their
// standard bracketed form; IPv6 zone identifiers are not URI syntax (RFC 9844
// reverted RFC 6874), while IPvFuture remains valid.
[[nodiscard]] bool is_valid_http_host(std::string_view value) noexcept;
[[nodiscard]] bool http_uri_host_equals(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] bool parse_request_target(
    http_known_method method, std::string_view target, request_target_view& output) noexcept;
template <http_temporary_owning_char_string target_type>
bool parse_request_target(http_known_method, target_type&&, request_target_view&) = delete;
[[nodiscard]] bool authority_matches_host(
    std::string_view authority, std::string_view host, std::uint16_t default_port) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {

// IMF-fixdate month names (RFC 9110 section 5.6.7), in calendar order.
inline constexpr std::array<std::string_view, 12> http_month_names{
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// Seconds since the epoch for UTC civil fields, or no value for an invalid
// calendar date, out-of-range clock fields or a time_t overflow. A second of 60
// is accepted as the leap second and counts into the following minute.
[[nodiscard]] std::optional<std::time_t> http_civil_to_time_t(
    int year, int month, int day, int hour, int minute, int second) noexcept;

// Full "Date: <value>\r\n" line for the current second, for HTTP/1 text response
// heads. Empty when the clock reports failure (-1) or a date outside the
// four-digit wire year range.
[[nodiscard]] std::string_view cached_date_header() noexcept;

// Bare date value (no field name, no CRLF) for HTTP/2 and HTTP/3 headers.
[[nodiscard]] std::string_view cached_date_value() noexcept;
}  // namespace ruvia::detail

namespace ruvia::detail {

// One identity for wire classification, cached lookup and descriptor tags.
// Other has no cache slot; the remaining identities map to compact slots.
enum class http_field_kind : std::uint8_t {
    other,
    accept,
    accept_encoding,
    access_control_request_headers,
    access_control_request_method,
    authorization,
    connection,
    content_encoding,
    content_length,
    content_type,
    cookie,
    expect,
    host,
    if_match,
    if_modified_since,
    if_none_match,
    if_range,
    if_unmodified_since,
    origin,
    range,
    sec_websocket_key,
    sec_websocket_protocol,
    sec_websocket_version,
    transfer_encoding,
    upgrade,
    user_agent,
    forwarded,
    x_forwarded_for,
    x_forwarded_proto,
    sec_websocket_extensions,
    te,
    trailer,
    proxy_authorization
};

inline constexpr std::size_t http_field_kind_count =
    static_cast<std::uint8_t>(http_field_kind::proxy_authorization) + 1;

[[nodiscard]] inline constexpr std::size_t http_field_kind_known_slot(
    http_field_kind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index == 0 ? http_field_kind_count : index - 1;
}

[[nodiscard]] inline constexpr std::uint32_t singleton_request_header_bit(
    http_field_kind kind) noexcept {
    switch (kind) {
        case http_field_kind::access_control_request_method:
        case http_field_kind::authorization:
        case http_field_kind::content_type:
        case http_field_kind::if_modified_since:
        case http_field_kind::if_range:
        case http_field_kind::if_unmodified_since:
        case http_field_kind::origin:
        case http_field_kind::range:
        case http_field_kind::sec_websocket_key:
        case http_field_kind::sec_websocket_version:
        case http_field_kind::user_agent:
            return 1U << static_cast<std::uint32_t>(kind);
        case http_field_kind::other:
        case http_field_kind::accept:
        case http_field_kind::accept_encoding:
        case http_field_kind::access_control_request_headers:
        case http_field_kind::connection:
        case http_field_kind::content_encoding:
        case http_field_kind::content_length:
        case http_field_kind::cookie:
        case http_field_kind::expect:
        case http_field_kind::host:
        case http_field_kind::if_match:
        case http_field_kind::if_none_match:
        case http_field_kind::sec_websocket_protocol:
        case http_field_kind::transfer_encoding:
        case http_field_kind::upgrade:
        case http_field_kind::forwarded:
        case http_field_kind::x_forwarded_for:
        case http_field_kind::x_forwarded_proto:
        case http_field_kind::sec_websocket_extensions:
        case http_field_kind::te:
        case http_field_kind::trailer:
        case http_field_kind::proxy_authorization:
            return 0;
    }
    return 0;
}

struct http_field_classification final {
    http_field_kind kind_{http_field_kind::other};
    std::uint32_t response_bit_{0};
};

[[nodiscard]] http_field_classification classify_http_field_name(std::string_view name) noexcept;
[[nodiscard]] http_field_kind classify_http_field(std::string_view name) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {
struct http_structured_item {
    enum class kind_type {
        other,
        integer,
        boolean,
        token,
        string,
        inner_list
    } kind_{kind_type::other};
    std::int64_t integer_{0};
    bool boolean_{false};
    std::string_view text_{};
    bool has_escapes_{false};
};
struct http_structured_text_input {
    std::string_view remaining_;

    [[nodiscard]] bool empty() const noexcept {
        return remaining_.empty();
    }
    [[nodiscard]] char peek() const noexcept {
        return remaining_.front();
    }
    [[nodiscard]] const char* data() const noexcept {
        return remaining_.data();
    }
    void advance() noexcept {
        remaining_.remove_prefix(1);
    }
};
template <typename iterator_type>
class http_structured_header_input final {
public:
    http_structured_header_input(
        iterator_type first, iterator_type last, std::string_view field_name) noexcept
        : next_(first),
          last_(last),
          field_name_(field_name) {
        (void)select_next(remaining_);
        normalize();
    }
    [[nodiscard]] bool empty() const noexcept {
        return remaining_.empty();
    }
    [[nodiscard]] char peek() const noexcept {
        return remaining_.front();
    }
    [[nodiscard]] const char* data() const noexcept {
        return remaining_.data();
    }
    void advance() noexcept {
        remaining_.remove_prefix(1);
        normalize();
    }

private:
    [[nodiscard]] bool select_next(std::string_view& value) noexcept {
        while (next_ != last_) {
            const auto& header_value = *next_++;
            if (http_ascii_equals_ignore_case(header_value.name(), field_name_)) {
                value = header_value.value();
                return true;
            }
        }
        return false;
    }
    void normalize() noexcept {
        while (remaining_.empty()) {
            if (separator_) {
                separator_ = false;
                remaining_ = pending_;
            } else if (select_next(pending_)) {
                remaining_ = ", ";
                separator_ = true;
            } else {
                return;
            }
        }
    }

    iterator_type next_;
    iterator_type last_;
    std::string_view field_name_;
    std::string_view remaining_;
    std::string_view pending_;
    bool separator_{false};
};

template <typename input_type>
struct http_structured_parser {
    input_type input_;

    [[nodiscard]] bool empty() const noexcept {
        return input_.empty();
    }
    bool take(char ch) {
        if (!input_.empty() && input_.peek() == ch) {
            input_.advance();
            return true;
        }
        return false;
    }
    void spaces() {
        while (take(' ')) {
        }
    }
    void ows() {
        while (!input_.empty() && is_http_ows_char(input_.peek())) {
            input_.advance();
        }
    }
    std::string_view key() {
        if (input_.empty() || !((input_.peek() >= 'a' && input_.peek() <= 'z') || input_.peek() == '*')) {
            return {};
        }
        const auto* const start = input_.data();
        std::size_t length = 1;
        input_.advance();
        while (!input_.empty()) {
            const auto ch = input_.peek();
            if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' || ch == '*')) {
                break;
            }
            ++length;
            input_.advance();
        }
        // Field-line boundaries inject commas, so a key always stays contiguous.
        return {start, length};
    }
    bool item(http_structured_item& value) {
        if (input_.empty()) {
            return false;
        }
        if (take('?')) {
            value.kind_ = http_structured_item::kind_type::boolean;
            if (take('1')) {
                value.boolean_ = true;
                return true;
            }
            return take('0');
        }
        if (take('"')) {
            const auto* const start = input_.data();
            std::size_t length = 0;
            bool has_escapes = false;
            while (!input_.empty()) {
                const auto ch = static_cast<unsigned char>(input_.peek());
                input_.advance();
                if (ch == '"') {
                    value.kind_ = http_structured_item::kind_type::string;
                    value.text_ = std::string_view(start, length);
                    value.has_escapes_ = has_escapes;
                    return true;
                }
                if (ch == '\\') {
                    if (input_.empty() || (input_.peek() != '\\' && input_.peek() != '"')) {
                        return false;
                    }
                    has_escapes = true;
                    input_.advance();
                    length += 2;
                } else if (ch < 0x20 || ch > 0x7e) {
                    return false;
                } else {
                    ++length;
                }
            }
            return false;
        }
        if (take(':')) {
            std::size_t count = 0, padding = 0;
            while (!input_.empty() && input_.peek() != ':') {
                const auto ch = input_.peek();
                input_.advance();
                if (ch == '=') {
                    if (++padding > 2) {
                        return false;
                    }
                } else if (padding || http_base64_decode_char(ch) < 0) {
                    return false;
                }
                ++count;
            }
            // RFC 8941 section 4.2.7 allows missing padding, including the
            // final '=' of a partially padded sequence. Validate the data
            // quartet and supplied padding as if missing padding were added.
            const auto data_remainder = (count - padding) % 4;
            return take(':') && data_remainder != 1 && padding <= (4 - data_remainder) % 4;
        }
        if (input_.peek() == '-' || (input_.peek() >= '0' && input_.peek() <= '9')) {
            const bool negative = take('-');
            std::size_t digits = 0;
            std::int64_t number = 0;
            while (!input_.empty() && input_.peek() >= '0' && input_.peek() <= '9') {
                if (++digits > 15) {
                    return false;
                }
                number = number * 10 + input_.peek() - '0';
                input_.advance();
            }
            if (!digits) {
                return false;
            }
            if (take('.')) {
                if (digits > 12) {
                    return false;
                }
                std::size_t fraction = 0;
                while (!input_.empty() && input_.peek() >= '0' && input_.peek() <= '9') {
                    input_.advance();
                    if (++fraction > 3) {
                        return false;
                    }
                }
                return fraction != 0;
            }
            value.kind_ = http_structured_item::kind_type::integer;
            value.integer_ = negative ? -number : number;
            return true;
        }
        const auto* const start = input_.data();
        const auto first = input_.peek();
        input_.advance();
        if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '*')) {
            return false;
        }
        std::size_t length = 1;
        // sf-token = ( ALPHA / "*" ) *( tchar / ":" / "/" )
        while (!input_.empty()) {
            const auto ch = input_.peek();
            if (!is_http_token_char(static_cast<unsigned char>(ch)) && ch != ':' && ch != '/') {
                break;
            }
            ++length;
            input_.advance();
        }
        value.kind_ = http_structured_item::kind_type::token;
        value.text_ = std::string_view(start, length);
        return true;
    }
    bool parameters() {
        while (take(';')) {
            spaces();
            if (key().empty()) {
                return false;
            }
            if (take('=')) {
                http_structured_item ignored;
                if (!item(ignored)) {
                    return false;
                }
            }
        }
        return true;
    }
    bool member(http_structured_item& value) {
        if (!take('(')) {
            return item(value) && parameters();
        }
        value.kind_ = http_structured_item::kind_type::inner_list;
        spaces();
        while (!take(')')) {
            http_structured_item ignored;
            if (!item(ignored) || !parameters()) {
                return false;
            }
            if (input_.empty() || (input_.peek() != ' ' && input_.peek() != ')')) {
                return false;
            }
            spaces();
        }
        return parameters();
    }
};
}  // namespace ruvia::detail

namespace ruvia::detail {

inline constexpr std::uint32_t response_header_content_length = 1U << 0;
inline constexpr std::uint32_t response_header_content_encoding = 1U << 1;
inline constexpr std::uint32_t response_header_content_type = 1U << 2;
inline constexpr std::uint32_t response_header_connection = 1U << 3;
inline constexpr std::uint32_t response_header_vary = 1U << 4;
inline constexpr std::uint32_t response_header_date = 1U << 5;
inline constexpr std::uint32_t response_header_server = 1U << 6;
inline constexpr std::uint32_t response_header_cache_control = 1U << 7;
inline constexpr std::uint32_t response_header_transfer_encoding = 1U << 8;
inline constexpr std::uint32_t response_header_allow = 1U << 9;
inline constexpr std::uint32_t response_header_access_control_allow_origin = 1U << 10;
inline constexpr std::uint32_t response_header_access_control_allow_credentials = 1U << 11;
inline constexpr std::uint32_t response_header_access_control_allow_methods = 1U << 12;
inline constexpr std::uint32_t response_header_access_control_allow_headers = 1U << 13;
inline constexpr std::uint32_t response_header_access_control_max_age = 1U << 14;
inline constexpr std::uint32_t response_header_access_control_expose_headers = 1U << 15;
inline constexpr std::uint32_t response_header_accept_ranges = 1U << 16;
inline constexpr std::uint32_t response_header_content_range = 1U << 17;
inline constexpr std::uint32_t response_header_etag = 1U << 18;
inline constexpr std::uint32_t response_header_last_modified = 1U << 19;
inline constexpr std::uint32_t response_header_location = 1U << 20;
inline constexpr std::uint32_t response_header_set_cookie = 1U << 21;
inline constexpr std::size_t response_known_header_count = 22;
inline constexpr std::uint32_t response_header_upgrade = 1U << 22;
inline constexpr std::uint32_t response_header_trailer = 1U << 23;
inline constexpr std::uint32_t response_header_keep_alive = 1U << 24;
inline constexpr std::uint32_t response_header_proxy_connection = 1U << 25;

[[nodiscard]] inline constexpr std::size_t response_known_header_slot(std::uint32_t bit) noexcept {
    constexpr std::uint32_t known_mask = (1U << response_known_header_count) - 1U;
    if (bit == 0 || (bit & ~known_mask) != 0 || (bit & (bit - 1U)) != 0) {
        return response_known_header_count;
    }
    return static_cast<std::size_t>(std::countr_zero(bit));
}

[[nodiscard]] inline bool response_header_append_forbidden(std::uint32_t bit) noexcept {
    return bit == response_header_content_length || bit == response_header_content_type ||
           bit == response_header_date || bit == response_header_server ||
           bit == response_header_transfer_encoding || bit == response_header_allow ||
           bit == response_header_access_control_allow_origin ||
           bit == response_header_access_control_allow_credentials ||
           bit == response_header_access_control_max_age || bit == response_header_accept_ranges ||
           bit == response_header_content_range || bit == response_header_etag ||
           bit == response_header_last_modified || bit == response_header_location;
}

[[nodiscard]] std::uint32_t classify_response_header_name(std::string_view name) noexcept;

// RFC 7541 §7.1.3 / RFC 9204 §4.2.1: credential-bearing fields use the
// never-indexed literal so an intermediary never places them in a shared dynamic table.
[[nodiscard]] inline bool http_field_never_indexed(std::string_view name) noexcept {
    const auto field = classify_http_field_name(name);
    return field.kind_ == http_field_kind::authorization ||
           field.kind_ == http_field_kind::proxy_authorization ||
           field.kind_ == http_field_kind::cookie ||
           field.response_bit_ == response_header_set_cookie;
}

[[nodiscard]] std::string_view response_known_header_lowercase_name(std::uint32_t known_bit) noexcept;

}  // namespace ruvia::detail

#pragma once

namespace ruvia::detail {

// Case-insensitive field-name set membership (RFC 9110 section 5.1).
[[nodiscard]] inline bool http_field_name_in(
    std::string_view name, std::span<const std::string_view> fields) noexcept {
    return std::ranges::any_of(
        fields, [name](std::string_view field) noexcept { return http_ascii_equals_ignore_case(name, field); });
}

// A successful CONNECT or extended-CONNECT tunnel carries no representation
// (RFC 9110 section 9.3.6, RFC 9298 section 3): its head must not declare
// framing or representation metadata.
inline constexpr std::array<std::string_view, 5> http_connect_forbidden_fields{
    "content-length", "transfer-encoding", "content-type", "content-encoding", "trailer"};

// HTTP/2 applies the connection-specific field ban. Decoded-name
// lowercase validation and the request-only TE value exception belong to the
// protocol boundary; application-owned names are matched case-insensitively.
[[nodiscard]] inline bool is_forbidden_http_binary_connection_field(std::string_view name) noexcept {
    switch (name.size()) {
        case 7:
            return http_ascii_equals_ignore_case(name, "upgrade");
        case 10:
            return http_ascii_equals_ignore_case(name, "connection") ||
                   http_ascii_equals_ignore_case(name, "keep-alive");
        case 16:
            return http_ascii_equals_ignore_case(name, "proxy-connection");
        case 17:
            return http_ascii_equals_ignore_case(name, "transfer-encoding");
        default:
            return false;
    }
}

[[nodiscard]] bool is_forbidden_http_binary_response_field(std::string_view name) noexcept;

// Spellings of every generated Connection field line. A sender of Upgrade
// lists the "Upgrade" connection option (RFC 9110 section 7.8); an HTTP/1.0
// persistent connection lists "keep-alive" (RFC 9112 section 9.3).
inline constexpr std::string_view http_connection_field_name = "Connection";
inline constexpr std::string_view http_connection_upgrade_option = "Upgrade";
inline constexpr std::string_view http_connection_keep_alive_option = "keep-alive";

enum class http_connection_option : std::uint8_t {
    close = 1U << 0,
    keep_alive = 1U << 1,
    upgrade = 1U << 2,
    te = 1U << 3
};

// A sender must not send an option naming a field intended for all recipients
// of the content (RFC 9110 section 7.6.1), nor one naming a field that frames
// (Content-Length, Transfer-Encoding) or routes the message: Host and the
// request-origin metadata Forwarded (RFC 7239), X-Forwarded-For,
// X-Forwarded-Proto and X-Forwarded-Host. A next hop honouring such an option
// strips the field before routing or client-address decisions consume it, so
// the nomination is refused as Pingora refuses it. Pseudo-header-shaped names
// are not tokens and already fail the list grammar. The recipient list
// (http_connection_option_names_protected_field) rejects the framing and Host
// nominations.
[[nodiscard]] inline bool http_connection_option_conflicts_with_managed_field(
    std::string_view option) noexcept {
    const auto field = classify_http_field_name(option);
    switch (field.kind_) {
        case http_field_kind::other:
            if (http_ascii_equals_ignore_case(option, "X-Forwarded-Host")) {
                return true;
            }
            break;
        case http_field_kind::connection:
        case http_field_kind::upgrade:
        case http_field_kind::sec_websocket_extensions:
        case http_field_kind::te:
        case http_field_kind::proxy_authorization:
            return false;
        case http_field_kind::forwarded:
        case http_field_kind::x_forwarded_for:
        case http_field_kind::x_forwarded_proto:
        case http_field_kind::accept:
        case http_field_kind::accept_encoding:
        case http_field_kind::access_control_request_headers:
        case http_field_kind::access_control_request_method:
        case http_field_kind::authorization:
        case http_field_kind::content_encoding:
        case http_field_kind::content_length:
        case http_field_kind::content_type:
        case http_field_kind::transfer_encoding:
        case http_field_kind::cookie:
        case http_field_kind::expect:
        case http_field_kind::host:
        case http_field_kind::if_match:
        case http_field_kind::if_modified_since:
        case http_field_kind::if_none_match:
        case http_field_kind::if_range:
        case http_field_kind::if_unmodified_since:
        case http_field_kind::origin:
        case http_field_kind::range:
        case http_field_kind::sec_websocket_key:
        case http_field_kind::sec_websocket_protocol:
        case http_field_kind::sec_websocket_version:
        case http_field_kind::user_agent:
        case http_field_kind::trailer:
            return true;
    }
    const auto known_bit = field.response_bit_;
    return (known_bit & ~(response_header_connection | response_header_upgrade |
                            response_header_keep_alive | response_header_proxy_connection)) != 0;
}

// Who parses a Connection field: its sender, or the recipient of a request or
// of a response, whose protected lists differ.
enum class http_connection_field_role : std::uint8_t {
    sender,
    request_recipient,
    response_recipient,
};

// The received options a recipient refuses. RFC 9110 section 7.6.1 forbids
// sending an option that names an end-to-end field, while a recipient only
// rejects options that make message framing or request target routing
// ambiguous if stripped by an intermediary:
// - every message: Content-Length and Transfer-Encoding;
// - a request also: Host.
// End-to-end fields such as Authorization, Cookie, Origin, or Set-Cookie
// remain prohibited for senders, not 400-rejected on receipt.
[[nodiscard]] inline bool http_connection_option_names_protected_field(
    std::string_view option, http_connection_field_role recipient) noexcept {
    switch (classify_http_field(option)) {
        case http_field_kind::content_length:
        case http_field_kind::transfer_encoding:
            return true;
        case http_field_kind::host:
            return recipient == http_connection_field_role::request_recipient;
        default:
            return false;
    }
}

// Incremental parser for the logical Connection field value. Repeated field
// lines extend the same state, so a caller cannot accidentally let a later
// occurrence erase an earlier close/Upgrade/TE signal. A sender's options must
// not name a field the protocol layer manages: a hop-by-hop declaration must
// not strip framing, routing or end-to-end metadata from the message. A recipient
// rejects only options naming a protected field (see
// http_connection_option_names_protected_field). Empty list members are
// malformed for a sender and ignored by a recipient (RFC 9110 section 5.6.1).
class http_connection_options final {
public:
    [[nodiscard]] http_field_list_parse_status parse_field(
        std::string_view field_value, http_connection_field_role role) noexcept {
        auto parsed_bits = state_;
        // An explicitly present but empty Connection field is useless and cannot
        // be safely extended by a later generated option; the strict writer
        // contract therefore rejects it as well as any empty member.
        const auto status = http_visit_token_list(field_value,
            role == http_connection_field_role::sender ? http_field_list_role::sender : http_field_list_role::recipient,
            [&parsed_bits, role](std::string_view option) noexcept {
                if (role == http_connection_field_role::sender
                        ? http_connection_option_conflicts_with_managed_field(option)
                        : http_connection_option_names_protected_field(option, role)) {
                    return false;
                }
                if (http_ascii_equals_ignore_case(option, "close")) {
                    parsed_bits |= bit(http_connection_option::close);
                } else if (http_ascii_equals_ignore_case(option, http_connection_keep_alive_option)) {
                    parsed_bits |= bit(http_connection_option::keep_alive);
                } else if (http_ascii_equals_ignore_case(option, http_connection_upgrade_option)) {
                    parsed_bits |= bit(http_connection_option::upgrade);
                } else if (http_ascii_equals_ignore_case(option, "TE")) {
                    parsed_bits |= bit(http_connection_option::te);
                }
                return true;
            });
        if (status == http_field_list_parse_status::ok) {
            state_ = static_cast<std::uint8_t>(parsed_bits | field_present_bit);
        }
        return status;
    }

    [[nodiscard]] bool has_field() const noexcept {
        return (state_ & field_present_bit) != 0;
    }

    [[nodiscard]] bool contains(http_connection_option option) const noexcept {
        return (state_ & bit(option)) != 0;
    }

    [[nodiscard]] bool close() const noexcept {
        return contains(http_connection_option::close);
    }

    [[nodiscard]] bool keep_alive() const noexcept {
        return contains(http_connection_option::keep_alive);
    }

    [[nodiscard]] bool upgrade() const noexcept {
        return contains(http_connection_option::upgrade);
    }

    [[nodiscard]] bool te() const noexcept {
        return contains(http_connection_option::te);
    }

private:
    static constexpr std::uint8_t field_present_bit = 1U << 7;

    [[nodiscard]] static constexpr std::uint8_t bit(http_connection_option option) noexcept {
        return static_cast<std::uint8_t>(option);
    }

    // Connection owns four recognised-token bits plus one orthogonal field
    // presence bit. Keeping them in one committed byte makes absent, present
    // empty/unknown, and present with recognised options impossible to tear.
    std::uint8_t state_{0};
};

static_assert(std::is_trivially_copyable_v<http_connection_options>);
static_assert(sizeof(http_connection_options) == 1);

struct http_upgrade_protocol final {
    std::string_view name_;
    std::string_view version_;
};

[[nodiscard]] inline bool http_parse_upgrade_protocol(
    std::string_view value, http_upgrade_protocol& output) noexcept {
    const auto slash = value.find('/');
    const auto name = slash == std::string_view::npos ? value : value.substr(0, slash);
    const auto version =
        slash == std::string_view::npos ? std::string_view{} : value.substr(slash + 1);
    if (!is_valid_http_header_name(name) ||
        (slash != std::string_view::npos &&
            (!is_valid_http_header_name(version) || (version.find('/') != std::string_view::npos)))) {
        return false;
    }
    output = http_upgrade_protocol{.name_ = name, .version_ = version};
    return true;
}

template <http_temporary_owning_char_string value_type>
bool http_parse_upgrade_protocol(value_type&&, http_upgrade_protocol&) = delete;

[[nodiscard]] inline bool http_upgrade_protocol_equals(
    const http_upgrade_protocol& left, const http_upgrade_protocol& right) noexcept {
    // RFC 9110 section 7.8: protocol-name is case-insensitive, while an
    // optional protocol-version remains an exact token.
    return http_ascii_equals_ignore_case(left.name_, right.name_) && left.version_ == right.version_;
}

// Incremental Upgrade list parser. It owns repeated-field/list syntax, while
// the visitor owns policy such as whether a selected protocol was offered.
enum class http_upgrade_field_state : std::uint8_t {
    absent,
    present_without_protocol,
    present_with_protocol,
};

class http_upgrade_protocols final {
public:
    [[nodiscard]] http_field_list_parse_status parse_field(
        std::string_view field_value, http_field_list_role role) noexcept {
        return parse_field(field_value, role, [](const http_upgrade_protocol&) noexcept { return true; });
    }

    template <typename visitor_type>
    [[nodiscard]] http_field_list_parse_status parse_field(
        std::string_view field_value, http_field_list_role role, visitor_type&& visitor) noexcept {
        bool parsed_protocol = false;
        const auto status = http_visit_list_members(field_value, role,
            [&parsed_protocol, &visitor](std::string_view member) noexcept {
                http_upgrade_protocol protocol;
                if (!http_parse_upgrade_protocol(member, protocol)) {
                    return http_field_list_parse_status::malformed;
                }
                if (!visitor(protocol)) {
                    return http_field_list_parse_status::rejected;
                }
                parsed_protocol = true;
                return http_field_list_parse_status::ok;
            });
        if (status != http_field_list_parse_status::ok) {
            return status;
        }
        if (parsed_protocol) {
            state_ = http_upgrade_field_state::present_with_protocol;
        } else if (state_ == http_upgrade_field_state::absent) {
            state_ = http_upgrade_field_state::present_without_protocol;
        }
        return http_field_list_parse_status::ok;
    }

    [[nodiscard]] bool has_field() const noexcept {
        return state_ != http_upgrade_field_state::absent;
    }

    [[nodiscard]] bool has_protocol() const noexcept {
        return state_ == http_upgrade_field_state::present_with_protocol;
    }

private:
    http_upgrade_field_state state_{http_upgrade_field_state::absent};
};

static_assert(std::is_trivially_copyable_v<http_upgrade_protocols>);
static_assert(sizeof(http_upgrade_protocols) == 1);

enum class http_response_control_field_status : std::uint8_t {
    ok,
    invalid_connection,
    invalid_upgrade,
    te_forbidden,
};

// Sender control-field grammar shared by insertion and final head analysis.
// Keep the parsed state across repeated lines, but do not check the Upgrade
// association until construction of all Connection fields has finished.
[[nodiscard]] inline http_response_control_field_status parse_http_response_control_field(
    http_field_kind kind, std::string_view value,
    http_connection_options& connection_options, http_upgrade_protocols& upgrade_protocols) noexcept {
    switch (kind) {
        case http_field_kind::connection:
            return connection_options.parse_field(value, http_connection_field_role::sender) ==
                           http_field_list_parse_status::ok
                       ? http_response_control_field_status::ok
                       : http_response_control_field_status::invalid_connection;
        case http_field_kind::upgrade:
            return upgrade_protocols.parse_field(value, http_field_list_role::sender) ==
                           http_field_list_parse_status::ok
                       ? http_response_control_field_status::ok
                       : http_response_control_field_status::invalid_upgrade;
        case http_field_kind::te:
            return http_response_control_field_status::te_forbidden;
        default:
            return http_response_control_field_status::ok;
    }
}

[[nodiscard]] inline bool http_response_upgrade_pair_valid(
    const http_connection_options& connection_options, const http_upgrade_protocols& upgrade_protocols) noexcept {
    return !upgrade_protocols.has_field() || connection_options.upgrade();
}

}  // namespace ruvia::detail

// Shared quality parsing (RFC 9110 section 12.4.2). Token fields use one
// optional weight; media ranges extract q from their parameters.

namespace ruvia::detail {

[[nodiscard]] inline int http_parse_quality_value(std::string_view value) noexcept {
    value = http_trim_ows(value);
    if (value == "1") {
        return 1000;
    }
    if (value == "0") {
        return 0;
    }
    if (value.size() >= 2 && value[1] == '.' && (value[0] == '0' || value[0] == '1')) {
        int quality = value[0] == '1' ? 1000 : 0;
        if (value[0] == '1') {
            for (std::size_t i = 2; i < value.size(); ++i) {
                if (i > 4 || value[i] != '0') {
                    return -1;
                }
            }
            return quality;
        }

        int scale = 100;
        for (std::size_t i = 2; i < value.size(); ++i) {
            if (i > 4 || value[i] < '0' || value[i] > '9') {
                return -1;
            }
            quality += (value[i] - '0') * scale;
            scale /= 10;
        }
        return quality;
    }
    return -1;
}

// Token-based Accept fields allow one optional weight, not media parameters.
// The list visitor has already trimmed surrounding OWS. Invalid weights are
// unacceptable rather than inheriting the unweighted default quality.
[[nodiscard]] inline int http_weight_parameter(std::string_view value) noexcept {
    const auto semicolon = value.find(';');
    if (semicolon == std::string_view::npos) {
        return 1000;
    }
    auto weight = http_trim_ows(value.substr(semicolon + 1));
    if (weight.size() < 3 || http_ascii_to_lower(static_cast<unsigned char>(weight[0])) != 'q' ||
        weight[1] != '=') {
        return 0;
    }
    const auto qvalue = weight.substr(2);
    if (qvalue != http_trim_ows(qvalue)) {
        return 0;
    }
    const auto parsed_value = http_parse_quality_value(qvalue);
    return parsed_value < 0 ? 0 : parsed_value;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

enum class response_field_status : std::uint8_t {
    ok,
    invalid,
    repeated_singleton,
};

// One state per generated response header section. Content-Type's single media
// type cannot be combined into a list (RFC 9110 section 5.3).
class response_field_state final {
private:
    friend response_field_status validate_response_field(http_field_kind kind, std::string_view value,
        response_field_state& state) noexcept;

    bool content_type_seen_{false};
};

// Sender-only response metadata semantics. Callers validate field-name/value bytes
// and retain protocol-specific framing, connection, and forbidden-field rules.
// A fresh state also validates one value before an http_response setter commits
// its replacement; the setter owns the separate append/replace policy.
[[nodiscard]] response_field_status validate_response_field(http_field_kind kind, std::string_view value,
    response_field_state& state) noexcept;

[[nodiscard]] bool is_forbidden_response_trailer_name(std::string_view name) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {

enum class http_interim_response_header_validation_status : std::uint8_t {
    ok,
    invalid_header,
    content_length_forbidden,
    transfer_encoding_forbidden,
    trailer_forbidden,
    repeated_singleton,
};

// Incremental sender validation for a version-neutral interim head. Recipients
// use their version's field syntax and message-boundary rules instead: 1xx
// framing advertisements do not establish content (RFC 9112 section 6.3 and
// RFC 9113 section 8.1.1).
class http_interim_response_sender_validator final {
public:
    // kind_out receives the field identity of an accepted line.
    [[nodiscard]] http_interim_response_header_validation_status validate(
        std::string_view name, std::string_view value,
        http_field_kind* kind_out = nullptr) noexcept {
        if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
            return http_interim_response_header_validation_status::invalid_header;
        }

        const auto field = classify_http_field_name(name);
        const auto known_bit = field.response_bit_;
        const auto kind = field.kind_;
        switch (kind) {
            case http_field_kind::content_length:
                return http_interim_response_header_validation_status::content_length_forbidden;
            case http_field_kind::transfer_encoding:
                return http_interim_response_header_validation_status::transfer_encoding_forbidden;
            case http_field_kind::trailer:
                return http_interim_response_header_validation_status::trailer_forbidden;
            default:
                break;
        }
        switch (validate_response_field(kind, value, fields_)) {
            case response_field_status::ok:
                break;
            case response_field_status::invalid:
                return http_interim_response_header_validation_status::invalid_header;
            case response_field_status::repeated_singleton:
                return http_interim_response_header_validation_status::repeated_singleton;
        }
        if (known_bit != 0 && known_bit != response_header_content_type && (known_bits_ & known_bit) != 0 &&
            response_header_append_forbidden(known_bit)) {
            return http_interim_response_header_validation_status::repeated_singleton;
        }
        known_bits_ |= known_bit;
        if (kind_out != nullptr) {
            *kind_out = kind;
        }
        return http_interim_response_header_validation_status::ok;
    }

private:
    response_field_state fields_;
    std::uint32_t known_bits_{0};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

[[nodiscard]] inline std::string_view http_trim_weak_etag_prefix(std::string_view value) noexcept {
    if (value.size() >= 2 && value[0] == 'W' && value[1] == '/') {
        value.remove_prefix(2);
    }
    return value;
}

template <http_temporary_owning_char_string value_type>
std::string_view http_trim_weak_etag_prefix(value_type&&) = delete;

[[nodiscard]] inline bool http_is_weak_etag(std::string_view value) noexcept {
    return value.size() >= 2 && value[0] == 'W' && value[1] == '/';
}

[[nodiscard]] inline bool http_is_strong_etag(std::string_view value) noexcept {
    return !value.empty() && value.front() == '"';
}

[[nodiscard]] inline bool http_strong_etag_equals(
    std::string_view left, std::string_view right) noexcept {
    return !http_is_weak_etag(left) && !http_is_weak_etag(right) && left == right;
}

[[nodiscard]] inline bool http_weak_etag_equals(
    std::string_view left, std::string_view right) noexcept {
    return http_trim_weak_etag_prefix(left) == http_trim_weak_etag_prefix(right);
}

struct http_etag_list_match_result final {
    bool valid_;
    bool matched_;
};

[[nodiscard]] inline http_etag_list_match_result http_parse_etag_list_matches(
    std::string_view values, std::string_view expected, bool strong) noexcept {
    bool matched = false;
    std::size_t offset = 0;

    while (offset < values.size()) {
        while (offset < values.size() && (is_http_ows_char(values[offset]) || values[offset] == ',')) {
            ++offset;
        }
        if (offset == values.size()) {
            break;
        }

        const std::size_t begin = offset;
        if (values.substr(offset).starts_with("W/")) {
            offset += 2;
        }
        if (offset == values.size() || values[offset] != '"') {
            return {false, false};
        }
        ++offset;
        while (offset < values.size() && values[offset] != '"') {
            const auto byte = static_cast<unsigned char>(values[offset]);
            if (byte != 0x21 && !(byte >= 0x23 && byte <= 0x7e) && byte < 0x80) {
                return {false, false};
            }
            ++offset;
        }
        if (offset == values.size()) {
            return {false, false};
        }
        ++offset;
        const auto entity_tag = values.substr(begin, offset - begin);
        matched = matched || (strong ? http_strong_etag_equals(entity_tag, expected)
                                     : http_weak_etag_equals(entity_tag, expected));

        while (offset < values.size() && is_http_ows_char(values[offset])) {
            ++offset;
        }
        if (offset < values.size() && values[offset] != ',') {
            return {false, false};
        }
    }
    return {true, matched};
}

}  // namespace ruvia::detail

#pragma once

namespace ruvia::detail {

[[nodiscard]] inline bool http_is_client_supported_te_transfer_coding(std::string_view coding) noexcept {
    return http_is_gzip_coding_token(coding) || http_ascii_equals_ignore_case(coding, "deflate");
}

[[nodiscard]] inline bool is_valid_http_te_field_item(
    std::string_view item, http_field_list_role role) noexcept {
    const auto first_semicolon = http_find_unquoted_delimiter(item, 0, ';');
    const auto coding = http_trim_ows(item.substr(0, first_semicolon));
    if (!is_valid_http_header_name(coding)) {
        return false;
    }
    if (http_ascii_equals_ignore_case(coding, "trailers")) {
        return first_semicolon == item.size();
    }
    // RFC 9112 section 7.4: a client MUST NOT list chunked in TE. That binds
    // the sender; a recipient treats it as an always-acceptable coding.
    if (role == http_field_list_role::sender &&
        !http_is_client_supported_te_transfer_coding(coding)) {
        return false;
    }

    // Validate transfer-parameter syntax and TE's weight in the same walk.
    // Unlike other parameters, q permits neither BWS around '=' nor a quoted
    // value. Recipients accept extension parameters; clients advertise only
    // the codings they can decode, with an optional weight.
    bool quality_seen = false;
    return http_all_parameters(item.substr(first_semicolon),
        [&quality_seen, role](std::string_view parameter) noexcept {
            const auto equals = parameter.find('=');
            if (equals == std::string_view::npos) {
                return false;
            }
            const auto raw_name = parameter.substr(0, equals);
            const auto raw_value = parameter.substr(equals + 1);
            const auto name = http_trim_ows(raw_name);
            const auto value = http_trim_ows(raw_value);
            if (http_ascii_equals_ignore_case(name, "q")) {
                if (quality_seen || raw_name != name || raw_value != value ||
                    http_parse_quality_value(value) < 0) {
                    return false;
                }
                quality_seen = true;
                return true;
            }
            return role == http_field_list_role::recipient && is_valid_http_transfer_parameter(parameter);
        });
}

[[nodiscard]] inline bool is_valid_http_te_field_value(
    std::string_view value, http_field_list_role role) noexcept {
    // RFC 9112 section 7.4 explicitly permits an empty TE field. It advertises
    // no optional transfer coding; chunked remains implicitly acceptable.
    if (http_trim_ows(value).empty()) {
        return true;
    }

    // A client generating TE is held to the sender's no-empty-member rule.
    return http_visit_list_members(value, role, [role](std::string_view item) noexcept {
        return is_valid_http_te_field_item(item, role) ? http_field_list_parse_status::ok
                                                       : http_field_list_parse_status::malformed;
    }) == http_field_list_parse_status::ok;
}

// HTTP/2 carries no transfer codings, so TE may only advertise
// "trailers" (RFC 9113 section 8.2.2). The member is an
// unprefixed ABNF literal and therefore case-insensitive (RFC 7405 section
// 2.1); list members are OWS-trimmed (RFC 9110 section 5.6.1). Recipients
// ignore empty members and accept empty lists (RFC 9110 section 10.1.4).
// This sender API requires a nonempty list without empty members.
[[nodiscard]] inline bool is_valid_binary_http_te_field_value(
    std::string_view value, http_field_list_role role) noexcept {
    bool has_member = false;
    const auto status = http_visit_list_members(value, role, [&has_member](std::string_view member) noexcept {
        has_member = true;
        return http_ascii_equals_ignore_case(member, "trailers") ? http_field_list_parse_status::ok
                                                                 : http_field_list_parse_status::malformed;
    });
    return status == http_field_list_parse_status::ok && (role == http_field_list_role::recipient || has_member);
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// The standardized Expect field member is defined once so parsers and writers
// cannot drift on its wire spelling.
inline constexpr std::string_view http_continue_expectation_token = "100-continue";

// RFC 6454 section 7.1 permits either `null` or a space-delimited list of
// serialized origins. Fetch-generated CORS requests currently send one item,
// but the HTTP protocol primitive must retain the complete field grammar.
[[nodiscard]] inline bool is_valid_http_origin_field_value(std::string_view value) noexcept {
    value = http_trim_ows(value);
    if (value == "null") {
        return true;
    }
    std::size_t offset = 0;
    for (;;) {
        const auto separator = value.find(' ', offset);
        const auto end = separator == std::string_view::npos ? value.size() : separator;
        if (!is_valid_http_serialized_origin(value.substr(offset, end - offset))) {
            return false;
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        offset = separator + 1;
    }
}

inline constexpr auto binary_field_name_char_table = [] {
    auto table_value = http_token_char_table;
    for (unsigned byte = 'A'; byte <= 'Z'; ++byte) {
        table_value[byte] = false;
    }
    return table_value;
}();

// HTTP/2 (RFC 9113 §8.2.1) and HTTP/3 (RFC 9114 §4.2) regular field names are
// nonempty lowercase tokens.
[[nodiscard]] inline bool is_valid_binary_field_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    std::size_t index = 0;
    while (name.size() - index >= 4) {
        const auto* bytes_value = reinterpret_cast<const unsigned char*>(name.data() + index);
        const bool first = binary_field_name_char_table[bytes_value[0]];
        const bool second = binary_field_name_char_table[bytes_value[1]];
        const bool third = binary_field_name_char_table[bytes_value[2]];
        const bool fourth = binary_field_name_char_table[bytes_value[3]];
        if (!(first & second & third & fourth)) {
            return false;
        }
        index += 4;
    }
    while (index < name.size()) {
        if (!binary_field_name_char_table[static_cast<unsigned char>(name[index])]) {
            return false;
        }
        ++index;
    }
    return true;
}

[[nodiscard]] inline bool is_http_request_scheme(std::string_view scheme) noexcept {
    return http_ascii_equals_ignore_case(scheme, "http") || http_ascii_equals_ignore_case(scheme, "https");
}

// http(s) target URIs carry a mandatory, non-empty authority with Host
// syntax; other schemes may carry any RFC 3986 authority.
[[nodiscard]] inline bool is_valid_binary_request_authority(
    std::string_view scheme, std::string_view authority) noexcept {
    if (is_http_request_scheme(scheme)) {
        return !authority.empty() && is_valid_host_header(authority);
    }
    return is_valid_uri_authority(authority);
}

[[nodiscard]] inline bool binary_regular_request_requires_authority(
    std::string_view scheme, std::string_view path) noexcept {
    return path != "*" && is_http_request_scheme(scheme);
}

// HTTP(S) already uses Host syntax; other schemes can contain URI userinfo.
[[nodiscard]] inline bool binary_request_synthesizes_host(bool has_host, bool has_authority,
    std::string_view scheme, std::string_view authority) noexcept {
    return !has_host && has_authority &&
           (is_http_request_scheme(scheme) || is_valid_host_header(authority));
}

// RFC 8441 §4 / RFC 9220 §3: websocket extended CONNECT requires an HTTP(S) URI
// scheme; other extended protocols retain the full RFC 3986 space.
[[nodiscard]] inline bool is_valid_extended_connect_scheme(
    std::string_view protocol, std::string_view scheme) noexcept {
    return !http_ascii_equals_ignore_case(protocol, "websocket") || is_http_request_scheme(scheme);
}

[[nodiscard]] inline bool validate_binary_request_authority_and_host(
    std::string_view scheme, std::string_view path, bool has_authority, std::string_view authority,
    bool has_host, std::string_view host) noexcept {
    if (has_authority && !is_valid_binary_request_authority(scheme, authority)) {
        return false;
    }
    if (!has_authority && binary_regular_request_requires_authority(scheme, path) &&
        (!has_host || !is_valid_binary_request_authority(scheme, host))) {
        return false;
    }
    return !has_authority || !has_host ||
           authority_matches_host(authority, host, http_uri_scheme_default_port(scheme));
}

// Joins split binary (RFC 9113 §8.2.3, RFC 9114 §4.2.1) Cookie crumbs with "; "
// within max_http_header_bytes. When `first_crumb` is non-empty and `joined` is
// still empty, `first_crumb` is materialized before appending the second crumb.
[[nodiscard]] inline bool append_binary_cookie_crumb(std::string& joined,
    std::string_view value, bool has_existing_cookie, std::string_view first_crumb = {}) {
    if (value.empty()) {
        return true;
    }
    if (value.size() > max_http_header_bytes) {
        return false;
    }
    if (!has_existing_cookie) {
        reserve_geometric(joined, value.size());
        joined.append(value.data(), value.size());
        return true;
    }
    constexpr std::string_view cookie_separator = "; ";
    const auto prefix_bytes = joined.empty() ? first_crumb.size() : joined.size();
    if (prefix_bytes > max_http_header_bytes - cookie_separator.size() ||
        value.size() > max_http_header_bytes - cookie_separator.size() - prefix_bytes) {
        return false;
    }
    reserve_geometric(joined, prefix_bytes + cookie_separator.size() + value.size());
    if (joined.empty() && !first_crumb.empty()) {
        joined.append(first_crumb.data(), first_crumb.size());
    }
    joined.append(cookie_separator.data(), cookie_separator.size());
    joined.append(value.data(), value.size());
    return true;
}

[[nodiscard]] constexpr bool is_valid_http_request_field_source(http_request_field_source source) noexcept {
    return source == http_request_field_source::generated || source == http_request_field_source::forwarded;
}

// The only version-dependent request-field semantics: HTTP/1.1 negotiates
// transfer codings through TE, while HTTP/2 permits only "trailers".
enum class request_field_framing : std::uint8_t { http1,
    binary };

enum class request_field_status : std::uint8_t {
    ok,
    // Grammar violation, or a second instance of a field that may occur once.
    invalid,
    // A client generated the field itself. Expect and Content-Length are
    // derived from the request plan on every HTTP version, so a raw field
    // would be a second source of truth for the body gate or the framing.
    managed_by_sender,
};

// Per field section: which once-only fields were seen, plus the framing whose
// TE rules apply. One instance spans exactly one request header section.
class request_field_state final {
public:
    explicit request_field_state(request_field_framing framing) noexcept
        : framing_(framing) {}

    // True once a field that may occur only once (Host and every
    // singleton_request_header_bit() kind) has been validated.
    [[nodiscard]] bool seen(http_field_kind kind) const noexcept;

private:
    friend request_field_status validate_request_field(http_field_kind kind, std::string_view value,
        http_field_list_role role, request_field_state& state, http_request_field_source source) noexcept;

    std::uint32_t seen_{0};
    request_field_framing framing_;
};

// The authoritative semantic check for one request field, shared by the
// HTTP/1 and HTTP/2 request writers (role sender) and request
// recipients (role recipient). Callers classify the name once with
// classify_http_field() and keep version-specific concerns themselves:
// framing state (Content-Length values on receipt, Transfer-Encoding,
// Connection, Upgrade), Host/:authority agreement, cookie joining, and parsing
// accepted Expect members.
[[nodiscard]] request_field_status validate_request_field(http_field_kind kind, std::string_view value,
    http_field_list_role role, request_field_state& state,
    http_request_field_source source = http_request_field_source::generated) noexcept;

[[nodiscard]] bool is_protocol_managed_request_field(
    http_field_kind kind, std::string_view name) noexcept;
}  // namespace ruvia::detail

namespace ruvia::detail {

struct http_request_access final {
    static constexpr std::size_t cached_header_slots = http_request::cached_header_slots;
    static_assert(cached_header_slots == http_field_kind_count - 1);

    [[nodiscard]] static http_request make() noexcept {
        return http_request();
    }

    [[nodiscard]] static constexpr std::size_t known_header_slot(http_field_kind name) noexcept {
        const auto slot = http_field_kind_known_slot(name);
        return slot < cached_header_slots ? slot : cached_header_slots;
    }

    [[nodiscard]] static std::string_view known_header(
        const http_request& request, http_field_kind name) noexcept {
        const auto slot = known_header_slot(name);
        if (slot >= cached_header_slots) {
            return {};
        }
        const auto index = request.cached_headers_[slot];
        return index != 0 && index <= request.headers_.size()
                   ? request.headers_[index - 1].value()
                   : std::string_view{};
    }

    [[nodiscard]] static bool has_known_header(
        const http_request& request, http_field_kind name) noexcept {
        const auto slot = known_header_slot(name);
        return slot < cached_header_slots && request.cached_headers_[slot] != 0 &&
               request.cached_headers_[slot] <= request.headers_.size();
    }

    [[nodiscard]] static bool has_repeated_known_header(
        const http_request& request, http_field_kind name) noexcept {
        const auto slot = known_header_slot(name);
        return slot < cached_header_slots && request.headers_.has_repeated_known_slot(slot);
    }

    // Visits the value of every field line of known kind `name` in wire order and
    // returns false as soon as the visitor does. A non-repeated field is served
    // from the cache; repeated fields scan only up to the cached last line.
    template <typename visitor_type>
    static bool visit_known_headers(
        const http_request& request, http_field_kind name, visitor_type&& visitor) {
        const auto slot = known_header_slot(name);
        if (slot >= cached_header_slots) {
            return true;
        }
        const auto headers = request.headers_.fields();
        const std::size_t last_index = request.cached_headers_[slot];
        if (last_index == 0 || last_index > headers.size()) {
            return true;
        }
        if (!request.headers_.has_repeated_known_slot(slot)) {
            return visitor(headers[last_index - 1].value());
        }
        for (std::size_t i = 0; i < last_index; ++i) {
            if (request.headers_.kind_at(i) == name && !visitor(headers[i].value())) {
                return false;
            }
        }
        return true;
    }

    static void set_method(http_request& request, std::string_view method) noexcept {
        request.method_ = method;
        request.known_method_ = classify_http_method(method);
    }

    static void set_target(http_request& request, std::string_view target) noexcept {
        request.target_ = target;
    }

    static void set_scheme(http_request& request, std::string_view scheme) noexcept {
        request.scheme_ = scheme;
    }

    static void set_authority(http_request& request, std::string_view authority) noexcept {
        request.authority_ = authority;
    }

    static void set_protocol(http_request& request, std::string_view protocol) noexcept {
        request.protocol_ = protocol;
    }

    static void set_target_form(http_request& request, ::ruvia::http_request_target_form form) noexcept {
        request.target_form_ = form;
    }

    static void set_path(http_request& request, std::string_view path) noexcept {
        request.path_ = path;
    }

    static void set_query_string(http_request& request, std::string_view query_string) noexcept {
        request.query_string_ = query_string;
    }

    static void set_protocol_version(
        http_request& request, http_protocol_version protocol_version) noexcept {
        request.protocol_version_ = protocol_version;
    }

    // Call once after syntax validation, with the exact semantic field count.
    static void reserve_headers(http_request& request, std::size_t count) {
        if (count > (std::numeric_limits<std::uint16_t>::max)() || !request.headers_.empty()) {
            throw std::logic_error("invalid request header block reservation");
        }
        request.headers_.reserve(count);
    }

    static bool add_header(
        http_request& request, http_header_view header_value, http_field_kind kind) {
        if (request.headers_.size() == (std::numeric_limits<std::uint16_t>::max)()) {
            return false;
        }
        const auto known_slot = known_header_slot(kind);
        request.headers_.append(
            header_value, known_slot < cached_header_slots ? kind : http_field_kind::other);
        if (known_slot < cached_header_slots) {
            if (request.cached_headers_[known_slot] != 0) {
                request.headers_.mark_repeated_known_slot(known_slot);
            }
            request.cached_headers_[known_slot] = static_cast<std::uint16_t>(request.headers_.size());
        }
        return true;
    }

    [[nodiscard]] static http_field_kind header_kind(
        const http_request& request, std::size_t index) noexcept {
        return request.headers_.kind_at(index);
    }

    static void set_body(http_request& request, std::span<const std::byte> body) noexcept {
        request.body_ = body;
    }
};

struct binary_request_materialization_spec final {
    http_protocol_version version_{http_protocol_version::http2};
    ::ruvia::http_request_target_form target_form_{::ruvia::http_request_target_form::http2};
    std::string_view method_{};
    std::string_view scheme_{};
    std::string_view authority_{};
    std::string_view host_{};
    std::string_view path_{};
    std::string_view protocol_{};
    std::string_view cookie_{};
    std::string_view body_{};
    std::size_t regular_header_count_{0};
    bool has_authority_{false};
    bool has_host_{false};
    bool has_path_{false};
    bool has_cookie_{false};
    bool standard_connect_{false};
    const char* failure_message_{"validated binary HTTP request cannot be materialized"};
};

template <typename header_accessor_type>
[[nodiscard]] inline http_request materialize_binary_http_request(
    const binary_request_materialization_spec& spec,
    header_accessor_type&& header_at) {
    auto request = http_request_access::make();
    if (spec.method_.empty()) {
        throw std::logic_error(spec.failure_message_);
    }
    const auto target = spec.standard_connect_ ? spec.authority_ : spec.path_;
    if (target.empty() && (spec.standard_connect_ || !spec.has_path_)) {
        throw std::logic_error(spec.failure_message_);
    }
    std::string_view request_path = target;
    std::string_view query_string;
    if (!spec.standard_connect_ && target != "*") {
        if (const auto query = target.find('?'); query != std::string_view::npos) {
            request_path = target.substr(0, query);
            query_string = target.substr(query + 1);
        }
    }
    const auto effective_authority = spec.has_authority_ ? spec.authority_ : spec.host_;

    http_request_access::set_method(request, spec.method_);
    http_request_access::set_protocol_version(request, spec.version_);
    http_request_access::set_target(request, target);
    http_request_access::set_scheme(request, spec.scheme_);
    http_request_access::set_authority(request, effective_authority);
    http_request_access::set_protocol(request, spec.protocol_);
    http_request_access::set_target_form(request, spec.target_form_);
    http_request_access::set_path(request, request_path);
    http_request_access::set_query_string(request, query_string);
    http_request_access::set_body(request, std::as_bytes(std::span(spec.body_.data(), spec.body_.size())));

    // A non-HTTP target can carry an authority that is not legal Host syntax;
    // never manufacture an invalid regular field from that distinct grammar.
    const bool synthesize_host = binary_request_synthesizes_host(
        spec.has_host_, spec.has_authority_, spec.scheme_, spec.authority_);
    const auto header_count = spec.regular_header_count_ + static_cast<std::size_t>(synthesize_host) +
                              static_cast<std::size_t>(spec.has_cookie_);
    if (header_count > (std::numeric_limits<std::uint16_t>::max)()) {
        throw std::logic_error(spec.failure_message_);
    }
    http_request_access::reserve_headers(request, header_count);
    for (std::size_t index = 0; index < spec.regular_header_count_; ++index) {
        const auto [field, kind] = header_at(index);
        if (!http_request_access::add_header(request, field, kind)) {
            throw std::logic_error(spec.failure_message_);
        }
    }
    if (synthesize_host &&
        !http_request_access::add_header(
            request, http_header_view{"host", spec.authority_}, http_field_kind::host)) {
        throw std::logic_error(spec.failure_message_);
    }
    if (spec.has_cookie_ &&
        !http_request_access::add_header(
            request, http_header_view{"cookie", spec.cookie_}, http_field_kind::cookie)) {
        throw std::logic_error(spec.failure_message_);
    }
    return request;
}

}  // namespace ruvia::detail
