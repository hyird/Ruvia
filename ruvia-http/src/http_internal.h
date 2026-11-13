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
#include <vector>

#include "ruvia/http/http_client.h"
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#endif

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

// The single HTTP/1 wire-emission toolkit: sinks, the field-line emitter, and
// the overlap-safe writer used by every pre-measured head/trailer writer.

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

// A reusable codec slot: native compression libraries keep pointers into
// their stream, so the codec lives in one stable heap allocation.
// `codec_type` provides initialized() and reset().

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

// One compression library binding per coding, behind a uniform signature: decode
// or encode a whole buffer, bounded by an
// explicit output ceiling. Decode reports data errors as values; encode returns
// nullopt only for the ceiling. Both throw http_content_codec_error when the
// codec itself cannot run. Which coding a field asks for is decided elsewhere;
// this is only the machinery each one runs on. decode_http_content maps an
// empty representation to empty content before any decoder below runs.

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

// RFC 3986 syntax, one level below any HTTP-specific rule: which byte sequences
// form a legal URI component, userinfo, port, IPv4 / IPv6 / IPvFuture literal or
// reg-name. Nothing here knows about request targets, Host fields or origins --
// those rules live in http_request.h and http_request.cpp.

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

// Shared quality parsing (RFC 9110 section 12.4.2). Token fields use one
// optional weight; media ranges extract q from their parameters.

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

namespace ruvia {

namespace detail {

// The base64-encoded SHA-1 of the client key plus the RFC 6455 GUID, as it
// appears in the Sec-websocket-Accept response field (always 28 bytes).
using websocket_accept_key_type = std::array<char, 28>;

// Throws std::length_error before inspecting the view when key + the RFC
// GUID cannot be represented by the SHA-1 64-bit bit-length field or by the
// size_t arithmetic used by the incremental implementation.
void encode_websocket_accept(websocket_accept_key_type& output, std::string_view key);

[[nodiscard]] bool append_websocket_extension_list(
    std::string_view value, bool& has_extension) noexcept;
[[nodiscard]] std::string_view choose_websocket_subprotocol(
    const websocket_subprotocol_set& offered,
    std::span<const std::string_view> supported) noexcept;
[[nodiscard]] std::string_view choose_websocket_subprotocol(
    const http_request& request, std::span<const std::string_view> supported) noexcept;

// Request-side Sec-WebSocket-* field checks. A recipient (server) skips a
// repeated offered subprotocol; a sender (client) must offer unique ones
// (RFC 6455 section 4.1).
class websocket_request_header_validator final {
public:
    explicit websocket_request_header_validator(http_field_list_role role) noexcept
        : role_(role) {}

    [[nodiscard]] bool observe_header(http_field_kind kind, std::string_view value) noexcept;
    [[nodiscard]] bool offers_valid() const noexcept;
    [[nodiscard]] bool client_offers_valid() const noexcept;
    [[nodiscard]] websocket_handshake_validation_result finish(
        websocket_handshake_validation_options options = {}) const noexcept;

private:
    websocket_subprotocol_set protocols_{};
    http_field_list_role role_;
    std::string_view version_{};
    bool version_present_{false};
    bool valid_{true};
    bool protocols_present_{false};
    bool extensions_present_{false};
    bool has_extension_{false};
};

[[nodiscard]] websocket_handshake_validation_result validate_extended_connect_websocket_headers(
    const http_request& request, websocket_handshake_validation_options options = {}) noexcept;
}  // namespace detail
}  // namespace ruvia

// The RFC 6455 section 5.2 wire vocabulary: what a frame's first two bytes mean,
// which opcode and length combinations are legal, and the byte-level encode and
// unmask primitives. Everything here is a pure function of bytes -- no buffer
// ownership, no message state.

namespace ruvia::detail {

// The fixed-size scratch a frame header is encoded into (2 bytes plus up to an
// 8-byte extended length).
using websocket_frame_header_type = std::array<char, 10>;

// Wire failures are protocol values, not exceptions. The numeric values are the
// RFC 6455 §7.4.1 Close codes the connection must send before ending transport.
enum class websocket_protocol_failure : std::uint16_t {
    protocol_error = 1002,
    invalid_payload_data = 1007,
    message_too_large = 1009,
};

[[nodiscard]] constexpr std::uint16_t websocket_protocol_failure_close_code(
    websocket_protocol_failure failure) noexcept {
    return static_cast<std::uint16_t>(failure);
}

enum class websocket_frame_kind : std::uint8_t {
    continuation = 0x0,
    text = 0x1,
    binary = 0x2,
    close = 0x8,
    ping = 0x9,
    pong = 0xA,
};

// Validated first-byte semantics. The raw continuation opcode is preserved as
// its own kind instead of being normalized to Text plus an independent boolean;
// compression is likewise admitted only where RFC 7692 permits RSV1.
class websocket_frame_start final {
public:
    [[nodiscard]] constexpr websocket_frame_kind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] constexpr bool final() const noexcept {
        return final_;
    }

    [[nodiscard]] constexpr bool compressed() const noexcept {
        return compressed_;
    }

private:
    friend std::optional<websocket_frame_start> decode_websocket_frame_start(
        unsigned char, unsigned char, bool, bool) noexcept;

    constexpr websocket_frame_start(websocket_frame_kind kind, bool final, bool compressed) noexcept
        : kind_(kind),
          final_(final),
          compressed_(compressed) {}

    websocket_frame_kind kind_;
    bool final_;
    bool compressed_;
};

[[nodiscard]] inline bool is_invalid_websocket_raw_opcode(std::uint8_t raw_opcode) noexcept {
    return (raw_opcode >= 0x3 && raw_opcode <= 0x7) || raw_opcode >= 0xB;
}

[[nodiscard]] inline bool is_websocket_control_frame_kind(websocket_frame_kind kind) noexcept {
    return static_cast<std::uint8_t>(kind) >= 0x8;
}

// allow_rsv1 enables the RSV1 (compressed) bit when permessage-deflate is
// negotiated; it is valid only on the first frame of a data message, never on a
// continuation or control frame (RFC 7692 §6.1). RSV2/RSV3 are always rejected.
[[nodiscard]] inline std::optional<websocket_frame_start> decode_websocket_frame_start(
    unsigned char first, unsigned char second, bool allow_rsv1, bool expect_masked) noexcept {
    const auto raw_opcode = static_cast<std::uint8_t>(first & 0x0FU);
    const bool rsv1 = (first & 0x40U) != 0;
    if ((first & 0x30U) != 0 || ((second & 0x80U) != 0) != expect_masked ||
        is_invalid_websocket_raw_opcode(raw_opcode)) {
        return std::nullopt;
    }
    if (rsv1 && (!allow_rsv1 || raw_opcode == 0 || raw_opcode >= 0x8)) {
        return std::nullopt;
    }
    return websocket_frame_start(
        static_cast<websocket_frame_kind>(raw_opcode), (first & 0x80U) != 0, rsv1);
}

[[nodiscard]] inline bool is_invalid_websocket_control_frame(
    const websocket_frame_start& frame, std::uint64_t payload_size) noexcept {
    return is_websocket_control_frame_kind(frame.kind()) && (!frame.final() || payload_size > 125);
}

[[nodiscard]] inline bool websocket_frame_length_exceeds_limit(
    std::uint64_t payload_size, protocol_byte_limit message_limit) noexcept {
    if (payload_size > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return true;
    }
    return message_limit.exceeds(static_cast<std::size_t>(payload_size));
}

// A control frame (Close/Ping/Pong) is capped at 125 bytes by RFC 6455 §5.5 and
// is explicitly NOT subject to the per-message size limit, so only data frames
// (Text/Binary/Continuation) are measured against max_message_bytes_. Applying the
// limit to control frames would reject a legal Ping/Pong, or a Close carrying a
// reason phrase, once max_message_bytes_ drops below 125 -- silently breaking the
// close handshake and keepalive on a small-message configuration.
[[nodiscard]] inline bool websocket_frame_exceeds_message_limit(
    websocket_frame_kind kind, std::uint64_t payload_size, protocol_byte_limit message_limit) noexcept {
    return !is_websocket_control_frame_kind(kind) &&
           websocket_frame_length_exceeds_limit(payload_size, message_limit);
}

[[nodiscard]] inline std::uint16_t read_websocket_uint16(const char* data) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(static_cast<unsigned char>(data[0])) << 8) |
        static_cast<unsigned char>(data[1]));
}

[[nodiscard]] constexpr std::variant<std::uint64_t, websocket_protocol_failure> read_websocket_uint64(
    std::span<const char, 8> data) noexcept {
    if ((static_cast<unsigned char>(data[0]) & 0x80U) != 0) {
        return websocket_protocol_failure::protocol_error;
    }
    std::uint64_t value = 0;
    for (const char byte : data) {
        value = (value << 8) | static_cast<unsigned char>(byte);
    }
    return value;
}

// `opcode_bits` is 0 for a continuation frame (RFC 6455 section 5.4).
[[nodiscard]] inline std::size_t encode_websocket_frame_header(websocket_frame_header_type& header_value,
    std::uint8_t opcode_bits, bool final, std::size_t payload_size, bool rsv1, bool masked) noexcept {
    std::size_t header_size = 0;
    header_value[header_size++] = static_cast<char>((final ? 0x80U : 0U) | (rsv1 ? 0x40U : 0U) | opcode_bits);
    if (payload_size <= 125) {
        header_value[header_size++] = static_cast<char>((masked ? 0x80U : 0U) | payload_size);
    } else if (payload_size <= 0xFFFF) {
        header_value[header_size++] = static_cast<char>((masked ? 0x80U : 0U) | 126U);
        header_value[header_size++] = static_cast<char>((payload_size >> 8) & 0xFF);
        header_value[header_size++] = static_cast<char>(payload_size & 0xFF);
    } else {
        header_value[header_size++] = static_cast<char>((masked ? 0x80U : 0U) | 127U);
        const auto size = static_cast<std::uint64_t>(payload_size);
        for (int shift = 56; shift >= 0; shift -= 8) {
            header_value[header_size++] = static_cast<char>((size >> shift) & 0xFF);
        }
    }
    return header_size;
}

// XORs `payload_value` with the 4-byte key in place, starting at key offset 0.
// The key is replicated into wider words loaded and stored through memcpy, which
// is valid at any alignment and aliasing and keeps the byte order intact on any
// endianness; the tail shorter than a word is masked bytewise.
inline void decode_masked_websocket_payload(
    char* payload_value, std::size_t payload_size, const char* mask) noexcept {
    std::array<char, 8> key_bytes;
    std::memcpy(key_bytes.data(), mask, 4);
    std::memcpy(key_bytes.data() + 4, mask, 4);
    std::uint64_t key;
    std::memcpy(&key, key_bytes.data(), sizeof(key));
    std::size_t i = 0;
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    const auto wide_key = _mm_set1_epi64x(static_cast<long long>(key));
    for (; payload_size - i >= 16; i += 16) {
        auto* const block = reinterpret_cast<__m128i*>(payload_value + i);
        _mm_storeu_si128(block, _mm_xor_si128(_mm_loadu_si128(block), wide_key));
    }
#endif
    for (; payload_size - i >= sizeof(key); i += sizeof(key)) {
        std::uint64_t word;
        std::memcpy(&word, payload_value + i, sizeof(word));
        word ^= key;
        std::memcpy(payload_value + i, &word, sizeof(word));
    }
    for (; i < payload_size; ++i) {
        payload_value[i] = static_cast<char>(payload_value[i] ^ mask[i & 3U]);
    }
}
}  // namespace ruvia::detail

#pragma once

// The one RFC 7692 whole-message deflate and inflate loop, shared by
// per-connection codecs and the shared no-context-takeover codec set. Context
// takeover is the owner's decision: these functions neither reset nor keep the
// state between messages.

namespace ruvia::detail {

// zlib cannot emit an 8-bit raw window, so an 8-bit sender window uses zlib's
// 9-bit window restricted to Huffman coding, which never refers back at all.
[[nodiscard]] constexpr int websocket_deflate_zlib_window_bits(int window_bits) noexcept {
    return -std::max(window_bits, 9);
}

[[nodiscard]] constexpr int websocket_deflate_zlib_strategy(int window_bits) noexcept {
    return window_bits == 8 ? Z_HUFFMAN_ONLY : Z_DEFAULT_STRATEGY;
}

// Compresses a message, or one fragment of it, up to a sync flush and appends
// the raw-DEFLATE blocks to `out`. At the message end the trailing
// 0x00 0x00 0xFF 0xFF flush marker is removed (RFC 7692 §7.2.1); earlier
// fragments keep it, which the receiver inflates as an empty stored block.
[[nodiscard]] inline bool websocket_deflate_message(zlib_deflate_stream& deflater, std::string_view input,
    std::string& out, bool message_end = true) {
    std::optional<std::string> owned_input;
    if (http_text_aliases_string(input, out)) {
        input = owned_input.emplace(input);
    }
    auto& stream = deflater.native();
    const zlib_borrow_scope borrow(stream);
    const auto start = out.size();
    stream.avail_in = 0;
    std::size_t supplied = 0;
    char buffer[4096];
    for (;;) {
        zlib_refill_input(stream, input, supplied);
        const auto before_input = stream.avail_in;
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        const int status = deflate(&stream, supplied == input.size() ? Z_SYNC_FLUSH : Z_NO_FLUSH);
        if (status != Z_OK && status != Z_BUF_ERROR) {
            deflater.rethrow_allocation_failure();
            return false;
        }
        const auto produced = sizeof(buffer) - stream.avail_out;
        out.append(buffer, produced);
        if (stream.avail_out != 0 && stream.avail_in == 0 && supplied == input.size()) {
            break;
        }
        if (produced == 0 && stream.avail_in == before_input &&
            !(stream.avail_in == 0 && supplied < input.size())) {
            return false;
        }
    }
    if (!message_end) {
        return true;
    }
    if (out.size() - start >= 4) {
        out.resize(out.size() - 4);
    }
    if (out.size() == start) {
        out.push_back('\0');
    }
    return true;
}

// One inflate pass over `data`; the message limit covers everything appended
// to `out` since `start`.
[[nodiscard]] inline websocket_inflate_result websocket_inflate_chunk(zlib_inflate_stream& inflater, const char* data,
    std::size_t size, std::string& out, std::size_t start, protocol_byte_limit message_limit) {
    auto& stream = inflater.native();
    const zlib_borrow_scope borrow(stream);
    stream.avail_in = 0;
    std::size_t supplied = 0;
    char buffer[8192];
    for (;;) {
        zlib_refill_input(stream, std::string_view(data, size), supplied);
        const auto before_input = stream.avail_in;
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        const int status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_STREAM_END && status != Z_OK && status != Z_BUF_ERROR) {
            inflater.rethrow_allocation_failure();
            return websocket_inflate_result::error;
        }
        const auto produced = sizeof(buffer) - stream.avail_out;
        if (message_limit.addition_exceeds(out.size() - start, produced)) {
            return websocket_inflate_result::too_large;
        }
        out.append(buffer, produced);
        // RFC 7692 permits BFINAL=1 followed by another DEFLATE stream.
        // Preserve the LZ77 window across streams, including takeover
        // history, while discarding the terminated stream's padding bits.
        if (status == Z_STREAM_END && inflateResetKeep(&stream) != Z_OK) {
            return websocket_inflate_result::error;
        }
        if (stream.avail_out != 0 && stream.avail_in == 0 && supplied == size) {
            break;
        }
        if (produced == 0 && stream.avail_in == before_input &&
            !(stream.avail_in == 0 && supplied < size)) {
            return websocket_inflate_result::error;
        }
    }
    return websocket_inflate_result::ok;
}

// Decompresses a whole message: appends the 0x00 0x00 0xFF 0xFF marker that
// the sender stripped, then raw-inflates into `out`, bounded by one explicit
// message limit across the appended bytes to defuse decompression bombs
// (RFC 7692 §7.2.2).
[[nodiscard]] inline websocket_inflate_result websocket_inflate_message(zlib_inflate_stream& inflater,
    std::string_view input, std::string& out, protocol_byte_limit message_limit) {
    static constexpr unsigned char flush_marker[4] = {0x00, 0x00, 0xFF, 0xFF};
    std::optional<std::string> owned_input;
    if (http_text_aliases_string(input, out)) {
        input = owned_input.emplace(input);
    }
    const auto start = out.size();
    if (const auto result_value = websocket_inflate_chunk(
            inflater, input.data(), input.size(), out, start, message_limit);
        result_value != websocket_inflate_result::ok) {
        return result_value;
    }
    if (const auto result_value = websocket_inflate_chunk(inflater,
            reinterpret_cast<const char*>(flush_marker), sizeof(flush_marker), out, start, message_limit);
        result_value != websocket_inflate_result::ok) {
        return result_value;
    }
    // zlib reports bit 128 only at a complete DEFLATE block boundary.
    // Consuming every input byte alone does not detect a truncated block.
    return (inflater.native().data_type & 128) != 0 ? websocket_inflate_result::ok : websocket_inflate_result::error;
}

// RFC 7692 codec state of one connection. A direction negotiated without
// context takeover starts every message from an empty dictionary, so it uses
// the borrowed shared codec set when the owner supplies one and then holds no
// codec memory. Every other direction owns a private zlib stream,
// created on its first compressed message and kept,
// with its dictionary, until the connection ends. The owner validates the
// deflater (validate_websocket_deflater_config) and windows (8-15).
class websocket_deflate final {
public:
    websocket_deflate(websocket_deflate_codecs* shared,
        websocket_deflater_config deflater, bool send_takeover, bool receive_takeover, int send_window,
        int receive_window) noexcept
        : shared_(shared),
          deflater_config_(deflater),
          send_window_(send_window),
          receive_window_(receive_window),
          send_takeover_(send_takeover),
          receive_takeover_(receive_takeover) {}

    websocket_deflate(const websocket_deflate&) = delete;
    websocket_deflate& operator=(const websocket_deflate&) = delete;

    // Appends the compressed message to `out`. false reports a codec failure;
    // allocation exceptions propagate.
    bool compress(std::string_view input, std::string& out) {
        if (!send_takeover_ && shared_ != nullptr) {
            // Reset after every call, including failures and exceptions.
            return shared_->deflate_message(input, out, deflater_config_.compression_level_, send_window_);
        }
        if (deflater_ == nullptr) {
            deflater_ = std::make_unique<zlib_deflate_stream>(
                deflater_config_.compression_level_, websocket_deflate_zlib_window_bits(send_window_),
                deflater_config_.mem_level_, websocket_deflate_zlib_strategy(send_window_));
            if (!deflater_->initialized()) {
                deflater_.reset();
                return false;
            }
        } else if (!send_takeover_ && !deflater_->reset()) {
            return false;
        }
        return websocket_deflate_message(*deflater_, input, out);
    }

    // Compresses one fragment of a message sent as several frames. The whole
    // message is one DEFLATE stream, so it uses a private codec even where
    // whole messages borrow the shared one, and starts from an empty
    // dictionary at its first fragment unless context takeover applies.
    bool compress_fragment(std::string_view input, bool first, bool final, std::string& out) {
        if (deflater_ == nullptr) {
            deflater_ = std::make_unique<zlib_deflate_stream>(
                deflater_config_.compression_level_, websocket_deflate_zlib_window_bits(send_window_),
                deflater_config_.mem_level_, websocket_deflate_zlib_strategy(send_window_));
            if (!deflater_->initialized()) {
                deflater_.reset();
                return false;
            }
        } else if (first && !send_takeover_ && !deflater_->reset()) {
            return false;
        }
        return websocket_deflate_message(*deflater_, input, out, final);
    }

    websocket_inflate_result decompress(
        std::string_view input, std::string& out, protocol_byte_limit message_limit) {
        if (!receive_takeover_ && shared_ != nullptr) {
            // The shared inflater has a 15-bit window, which also inflates
            // every smaller negotiated window.
            return shared_->inflate_message(input, out, message_limit);
        }
        if (inflater_ == nullptr) {
            inflater_ = std::make_unique<zlib_inflate_stream>(-receive_window_);
            if (!inflater_->initialized()) {
                inflater_.reset();
                return websocket_inflate_result::error;
            }
        } else if (!receive_takeover_ && !inflater_->reset()) {
            return websocket_inflate_result::error;
        }
        return websocket_inflate_message(*inflater_, input, out, message_limit);
    }

    // A trial that is not sent must not enter the peer's future dictionary.
    // Starting the next compressed message with an empty sender dictionary is
    // legal even when the receiver retains its prior dictionary (RFC 7692).
    // The shared codecs already reset after every message.
    void discard_compression() {
        if (deflater_ != nullptr && !deflater_->reset()) {
            throw std::runtime_error("failed to reset discarded WebSocket compression");
        }
    }

private:
    websocket_deflate_codecs* shared_;
    std::unique_ptr<zlib_deflate_stream> deflater_;
    std::unique_ptr<zlib_inflate_stream> inflater_;
    websocket_deflater_config deflater_config_;
    int send_window_;
    int receive_window_;
    bool send_takeover_;
    bool receive_takeover_;
};

// Decode RFC 7692 window parameters (8 through 15), including quoted tokens.
// The negotiated send and receive bounds are independent of takeover policy.
[[nodiscard]] inline std::optional<int> websocket_deflate_window_bits(
    std::string_view value) noexcept {
    value = http_trim_ows(value);
    int parsed_value = 0;
    std::size_t digits = 0;
    bool leading_zero = false;
    // RFC 7692 section 7.1.2 defines both max-window-bits parameters as
    // decimal integers without leading zeroes. Apply that grammar after
    // quoted-pair decoding as well as to tokens.
    const auto append_digit = [&](unsigned char ch) noexcept {
        if (ch < '0' || ch > '9' || ++digits > 2) {
            return false;
        }
        if (digits == 1) {
            leading_zero = ch == '0';
        } else if (leading_zero) {
            return false;
        }
        parsed_value = parsed_value * 10 + (ch - '0');
        return true;
    };
    if (!value.empty() && value.front() == '"') {
        http_quoted_string_cursor quoted(value);
        unsigned char byte = 0;
        while (quoted.next(byte)) {
            if (!append_digit(byte)) {
                return std::nullopt;
            }
        }
        if (!quoted.closed() || quoted.consumed() != value.size()) {
            return std::nullopt;
        }
    } else {
        for (const char ch : value) {
            if (!append_digit(static_cast<unsigned char>(ch))) {
                return std::nullopt;
            }
        }
    }
    return digits != 0 && parsed_value >= 8 && parsed_value <= 15 ? std::optional<int>(parsed_value) : std::nullopt;
}

// RFC 7692 section 7.1 parameters of one permessage-deflate extension element.
// client_max_window_bits may appear without a value; whether that is
// acceptable depends on the role, so its presence is kept apart from its value.
struct permessage_deflate_params final {
    bool server_no_context_takeover_{false};
    bool client_no_context_takeover_{false};
    std::optional<int> server_max_window_bits_;
    bool client_max_window_bits_present_{false};
    std::optional<int> client_max_window_bits_;
};

// Grammar shared by offers and responses: every parameter is defined, appears
// at most once and carries a value exactly where RFC 7692 defines one (an
// undefined or malformed parameter cannot be negotiated). Returns nullopt for
// another extension or a malformed element; role policy is left to callers.
[[nodiscard]] inline std::optional<permessage_deflate_params> parse_permessage_deflate_params(
    std::string_view element) noexcept {
    const auto name = http_header_token_before_parameters(element);
    if (!http_ascii_equals_ignore_case(name, "permessage-deflate")) {
        return std::nullopt;
    }

    permessage_deflate_params params;
    const bool valid = http_all_parameters(element, [&params](std::string_view parameter) noexcept {
        const auto equals = parameter.find('=');
        const bool has_value = equals != std::string_view::npos;
        const auto parameter_name = http_trim_ows(parameter.substr(0, equals));
        const auto parameter_value =
            has_value ? http_trim_ows(parameter.substr(equals + 1)) : std::string_view{};

        switch (parameter_name.size()) {
            case 22:
                if (http_ascii_equals_ignore_case(parameter_name, "server_max_window_bits")) {
                    // A missing or empty value fails window decoding.
                    if (params.server_max_window_bits_.has_value()) {
                        return false;
                    }
                    params.server_max_window_bits_ = websocket_deflate_window_bits(parameter_value);
                    return params.server_max_window_bits_.has_value();
                }
                if (http_ascii_equals_ignore_case(parameter_name, "client_max_window_bits")) {
                    if (params.client_max_window_bits_present_) {
                        return false;
                    }
                    params.client_max_window_bits_present_ = true;
                    if (has_value) {
                        params.client_max_window_bits_ = websocket_deflate_window_bits(parameter_value);
                        return params.client_max_window_bits_.has_value();
                    }
                    return true;
                }
                return false;
            case 26:
                if (http_ascii_equals_ignore_case(parameter_name, "server_no_context_takeover")) {
                    if (params.server_no_context_takeover_ || has_value) {
                        return false;
                    }
                    params.server_no_context_takeover_ = true;
                    return true;
                }
                if (http_ascii_equals_ignore_case(parameter_name, "client_no_context_takeover")) {
                    if (params.client_no_context_takeover_ || has_value) {
                        return false;
                    }
                    params.client_no_context_takeover_ = true;
                    return true;
                }
                return false;
            default:
                // Includes an empty parameter, such as a trailing ';'.
                return false;
        }
    });
    return valid ? std::optional(params) : std::nullopt;
}

// Server role: an offered client_max_window_bits, with or without a value, is
// accepted but never echoed in the response.
[[nodiscard]] inline std::optional<websocket_compression> websocket_parse_deflate_offer(
    std::string_view offer, bool context_takeover) noexcept {
    const auto params = parse_permessage_deflate_params(offer);
    if (!params.has_value()) {
        return std::nullopt;
    }
    websocket_compression accepted{.enabled_ = true, .server_max_window_bits_ = params->server_max_window_bits_};
    if (context_takeover) {
        accepted.server_no_context_takeover_ = params->server_no_context_takeover_;
        accepted.client_no_context_takeover_ = params->client_no_context_takeover_;
    }
    return accepted;
}

[[nodiscard]] inline websocket_compression websocket_scan_deflate_offers(
    std::string_view offers, bool context_takeover) noexcept {
    std::optional<websocket_compression> accepted;
    http_visit_comma_separated_quoted_field_items(offers, [&accepted, context_takeover](std::string_view offer) noexcept {
        if (offer.empty()) {
            return true;
        }
        accepted = websocket_parse_deflate_offer(offer, context_takeover);
        return !accepted.has_value();
    });
    return accepted.value_or((websocket_compression{}));
}

[[nodiscard]] inline websocket_compression websocket_negotiate_permessage_deflate(
    const http_request& request, websocket_deflate_config config) {
    validate_websocket_deflater_config(config.deflater_);
    if (!config.enabled_) {
        return (websocket_compression{});
    }
    // RFC 6455 §9.1: extension declarations may be split across multiple
    // Sec-WebSocket-Extensions field lines, which RFC 9110 §5.3 makes equivalent to
    // one comma-joined list. Scan every line in order, validating the whole
    // extension list while honoring the first acceptable permessage-deflate offer.
    bool has_extension = false;
    websocket_compression negotiated{};
    const bool valid = http_request_access::visit_known_headers(request,
        http_field_kind::sec_websocket_extensions,
        [&has_extension, &negotiated, &config](std::string_view value) noexcept {
            if (!append_websocket_extension_list(value, has_extension)) {
                return false;
            }
            if (!negotiated.enabled_) {
                negotiated = websocket_scan_deflate_offers(value, config.context_takeover_);
            }
            return true;
        });
    return valid && has_extension ? negotiated : (websocket_compression{});
}

// HTTP-version-independent result of one server-side websocket negotiation. HTTP/1
// and RFC 8441 consume this same immutable value for their response head, and the
// subsequent ws_connection consumes its exact deflate alternative. This prevents
// response metadata and frame RSV1 semantics from being configured separately.
// The selected subprotocol is copied into owned storage so this
// committed value never borrows mutable route configuration.
class websocket_server_negotiation final {
public:
    websocket_server_negotiation(const websocket_server_negotiation&) = delete;
    websocket_server_negotiation& operator=(const websocket_server_negotiation&) = delete;
    websocket_server_negotiation(websocket_server_negotiation&&) noexcept = default;
    websocket_server_negotiation& operator=(websocket_server_negotiation&&) = delete;

    [[nodiscard]] std::string_view subprotocol() const& noexcept {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] websocket_compression compression() const noexcept {
        return compression_;
    }

    [[nodiscard]] std::string_view extensions() const& noexcept {
        return extensions_.view();
    }
    std::string_view extensions() const&& = delete;

    [[nodiscard]] std::span<const http_header> response_headers() const& noexcept {
        return response_headers_;
    }
    std::span<const http_header> response_headers() const&& = delete;

    [[nodiscard]] std::string take_subprotocol() && noexcept {
        return std::move(subprotocol_);
    }

    [[nodiscard]] std::vector<http_header> take_response_headers() && noexcept {
        return std::move(response_headers_);
    }

private:
    friend websocket_server_negotiation make_websocket_server_negotiation(
        const http_request&, const websocket_server_handshake_options&);

    websocket_server_negotiation(std::string_view subprotocol, websocket_compression compression,
        std::span<const http_header_view> response_headers_value);

    std::string subprotocol_;
    websocket_compression compression_;
    websocket_compression_extension extensions_;
    std::vector<http_header> response_headers_;
};

// Copies the selected subprotocol and response fields.
[[nodiscard]] websocket_server_negotiation make_websocket_server_negotiation(
    const http_request& request, const websocket_server_handshake_options& options);

[[nodiscard]] constexpr http_response_content_semantics classify_http_response_content_semantics(
    http_known_method request_method, http_status_code status_code) noexcept {
    if (status_code == http_status::switching_protocols) {
        return http_response_content_semantics::protocol_switch;
    }
    if (status_code.is_informational()) {
        return http_response_content_semantics::informational;
    }
    if (request_method == http_known_method::connect && status_code.is_successful()) {
        return http_response_content_semantics::connect_tunnel;
    }
    if (request_method == http_known_method::head || status_code == http_status::no_content ||
        status_code == http_status::not_modified) {
        return http_response_content_semantics::without_content;
    }
    return http_response_content_semantics::with_content;
}

struct http_response_body_access final {
    [[nodiscard]] static const http_response_body& body(const http_response& response) noexcept {
        return response.body_;
    }
    [[nodiscard]] static const http_response_body& body(const http_response&& response) = delete;
};

[[nodiscard]] inline const http_response_body& response_body(const http_response& response) noexcept {
    return http_response_body_access::body(response);
}
[[nodiscard]] const http_response_body& response_body(const http_response&& response) = delete;

struct http_response_header_state_access final {
    [[nodiscard]] static std::uint32_t known_bits(const http_response_headers& headers) noexcept {
        return headers.known_header_bits_;
    }

    [[nodiscard]] static const http_response_header* known_header(
        const http_response_headers& headers, std::uint32_t bit) noexcept {
        return headers.find_header_for_read({}, bit);
    }

    [[nodiscard]] static std::string_view known_header_value(
        const http_response_headers& headers, std::uint32_t bit) noexcept {
        const auto* const header_value = known_header(headers, bit);
        return header_value == nullptr ? std::string_view{} : header_value->value();
    }
};

struct http_response_header_access final {
    [[nodiscard]] static constexpr http_response_header make(const char* bytes_value, std::uint32_t name_size,
        std::uint32_t value_size, std::uint32_t known_bit, bool owned) noexcept {
        http_response_header header_value{};
        header_value.bytes_ = bytes_value;
        header_value.name_size_ = name_size;
        header_value.value_size_ = value_size;
        header_value.known_bit_ = known_bit;
        header_value.owned_ = owned;
        header_value.append_ = false;
        return header_value;
    }

    [[nodiscard]] static char* value_begin(http_response_header& header_value) noexcept {
        if (header_value.bytes_ == nullptr) {
            return nullptr;
        }
        return const_cast<char*>(header_value.bytes_) + header_value.name_size_;
    }

    [[nodiscard]] static char* value_end(http_response_header& header_value) noexcept {
        auto* const begin = value_begin(header_value);
        return begin == nullptr ? nullptr : begin + header_value.value_size_;
    }

    [[nodiscard]] static std::uint32_t known_bit(const http_response_header& header_value) noexcept {
        return header_value.known_bit_;
    }

    [[nodiscard]] static bool append(const http_response_header& header_value) noexcept {
        return header_value.append_;
    }

    static void set_append(http_response_header& header_value) noexcept {
        header_value.append_ = true;
    }
};

[[nodiscard]] inline std::uint32_t response_known_header_bits(const http_response& response) noexcept {
    return http_response_header_state_access::known_bits(response.headers());
}

[[nodiscard]] inline bool response_has_known_header(
    const http_response_headers& headers, std::uint32_t bit) noexcept {
    return (http_response_header_state_access::known_bits(headers) & bit) != 0;
}

[[nodiscard]] inline bool response_has_known_header(
    const http_response& response, std::uint32_t bit) noexcept {
    return response_has_known_header(response.headers(), bit);
}

// http_response_header stores both lengths in uint32_t and releases the one
// name/value allocation using their sum. Never let a public or runtime helper
// construct a descriptor whose wire/storage lengths cannot be represented;
// truncating either field would make the response header view disagree with
// the allocation and could turn a large application value into malformed wire
// bytes or an out-of-bounds read during emission.
[[nodiscard]] inline constexpr bool response_header_storage_size_fits(
    std::size_t name_size, std::size_t value_size) noexcept {
    constexpr auto max_size = static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());
    return name_size <= max_size && value_size <= max_size && value_size <= max_size - name_size;
}

inline void validate_response_header_storage_size(std::size_t name_size, std::size_t value_size) {
    if (!response_header_storage_size_fits(name_size, value_size)) {
        throw std::length_error("HTTP response header is too large");
    }
}

// The one field-identity rule: known fields match by bit, unknown fields by
// case-insensitive name among unknown fields only.
[[nodiscard]] inline bool response_header_matches(
    const http_response_header& header_value, std::string_view key, std::uint32_t known_bit) noexcept {
    const auto header_known_bit = http_response_header_access::known_bit(header_value);
    return known_bit != 0 ? header_known_bit == known_bit
                          : header_known_bit == 0 && http_ascii_equals_ignore_case(header_value.name(), key);
}

[[nodiscard]] inline bool response_header_storage_overlaps(
    const http_response_header& header_value, std::string_view value) noexcept {
    const auto name = header_value.name();
    if (name.data() == nullptr) {
        return false;
    }
    return http_text_overlaps(
        {name.data(), name.size() + header_value.value().size()}, value);
}

[[nodiscard]] inline bool response_header_views_contiguous(
    std::string_view name, std::string_view value) noexcept {
    if (name.data() == nullptr) {
        return false;
    }
    if (value.empty()) {
        return true;
    }
    if (value.data() == nullptr) {
        return false;
    }
    return reinterpret_cast<std::uintptr_t>(value.data()) ==
           reinterpret_cast<std::uintptr_t>(name.data() + name.size());
}

enum class http_binary_final_response_control_plan_error : std::uint8_t {
    invalid_status,
    upgrade_unavailable,
    connection_specific_field_forbidden
};

class http1_final_response_control;

[[nodiscard]] http1_final_response_control http1_final_response_control_plan(const http_response& response);

[[nodiscard]] std::optional<http_binary_final_response_control_plan_error> http_binary_final_response_control_plan(
    const http_response& response) noexcept;
// The protocol writer alone frames a response (RFC 9112 section 6.1, RFC 9113
// section 8.2.2), so an application Transfer-Encoding is accepted only as a
// hint that names exactly the single `chunked` coding, which every writer
// replaces with its own framing. Any other value claims a coding the writer
// never applies and fails closed on both protocols.
[[nodiscard]] inline bool response_transfer_encoding_replaceable(const http_response& response) noexcept {
    if (!response_has_known_header(response, response_header_transfer_encoding)) {
        return true;
    }
    return http_ascii_equals_ignore_case(
        http_trim_ows(http_response_header_state_access::known_header_value(
            response.headers(), response_header_transfer_encoding)),
        "chunked");
}

class http1_final_response_control final {
public:
    [[nodiscard]] http_connection_options connection_options() const noexcept {
        return connection_options_;
    }

    [[nodiscard]] http_upgrade_protocols upgrade_protocols() const noexcept {
        return upgrade_protocols_;
    }

private:
    friend http1_final_response_control http1_final_response_control_plan(const http_response&);

    http1_final_response_control(
        http_connection_options connection_options, http_upgrade_protocols upgrade_protocols) noexcept
        : connection_options_(connection_options),
          upgrade_protocols_(upgrade_protocols) {}

    http_connection_options connection_options_;
    http_upgrade_protocols upgrade_protocols_;
};

// Validate HTTP/1 control fields before the response mutates Connection state.
// Success owns the parsed repeated Connection and Upgrade fields. http_response
// already rejects a non-final status and malformed Connection, Upgrade and TE
// fields on insertion; these checks keep the commit self-contained.
[[nodiscard]] inline http1_final_response_control http1_final_response_control_plan(const http_response& response) {
    const auto status_code = response.status();
    if (!status_code.is_final()) {
        throw http1_response_head_error("invalid final HTTP response status");
    }
    if (!response_transfer_encoding_replaceable(response)) {
        throw http1_response_head_error("HTTP response Transfer-Encoding must be exactly chunked");
    }

    http_connection_options connection_options;
    http_upgrade_protocols upgrade_protocols;
    if (response_has_known_header(
            response, response_header_connection | response_header_upgrade)) {
        for (const auto& header : response.headers()) {
            switch (http_response_header_access::known_bit(header)) {
                case response_header_connection:
                    if (connection_options.parse_field(header.value(), http_connection_field_role::sender) !=
                        http_field_list_parse_status::ok) {
                        throw http1_response_head_error("invalid HTTP Connection header");
                    }
                    break;
                case response_header_upgrade:
                    if (upgrade_protocols.parse_field(header.value(), http_field_list_role::sender) !=
                        http_field_list_parse_status::ok) {
                        throw http1_response_head_error("invalid HTTP Upgrade header");
                    }
                    break;
                default:
                    break;
            }
        }
    }
    // RFC 9110 §15.5.22: a 426 response MUST send an Upgrade field.
    if (status_code == http_status::upgrade_required && !upgrade_protocols.has_protocol()) {
        throw http1_response_head_error("Upgrade Required response requires an Upgrade protocol");
    }
    return http1_final_response_control(connection_options, upgrade_protocols);
}

// Validate HTTP/2 and HTTP/3 control semantics before field encoding or stream
// state is mutated. Success proves that no connection-specific field exists;
// RFC 9113 section 8.2.2 and RFC 9114 section 4.2 require rejection rather than
// silent filtering. A replaceable Transfer-Encoding hint is not application
// metadata but writer-owned framing, so both encoders omit it.
[[nodiscard]] inline std::optional<http_binary_final_response_control_plan_error>
http_binary_final_response_control_plan(const http_response& response) noexcept {
    const auto status_code = response.status();
    if (!status_code.is_final()) {
        return http_binary_final_response_control_plan_error::invalid_status;
    }
    if (status_code == http_status::upgrade_required) {
        return http_binary_final_response_control_plan_error::upgrade_unavailable;
    }
    if (response_has_known_header(response,
            response_header_connection | response_header_upgrade | response_header_keep_alive |
                response_header_proxy_connection) ||
        !response_transfer_encoding_replaceable(response)) {
        return http_binary_final_response_control_plan_error::connection_specific_field_forbidden;
    }
    return std::nullopt;
}

// Shared response Content-Length semantics. HTTP/1 adds its chunked and
// close-delimited framing policy; HTTP/2 and HTTP/3 delimit content with DATA
// frames but enforce the same normalized field value.

enum class http_framed_response_head_plan_error : std::uint8_t {
    invalid_content_length,
    connect_tunnel_required,
    response_status_mismatch,
    response_representation_mismatch
};

class http_framed_response_head_plan final {
public:
    [[nodiscard]] http_response_body_plan body_plan() const noexcept {
        return body_plan_;
    }

    // The normalized Content-Length field to encode, if any.
    [[nodiscard]] std::optional<std::uint64_t> content_length() const noexcept {
        return content_length_mode_ == content_length_mode_type::omit
                   ? std::nullopt
                   : std::optional<std::uint64_t>(content_length_);
    }

    // Only an application-declared streaming length constrains subsequent DATA.
    // Framework-generated buffered lengths are already bound to their response
    // representation and therefore do not create a streaming accounting limit.
    [[nodiscard]] std::optional<std::uint64_t> streaming_content_length() const noexcept {
        return content_length_mode_ == content_length_mode_type::explicit_value
                   ? std::optional<std::uint64_t>(content_length_)
                   : std::nullopt;
    }

    [[nodiscard]] static http_framed_response_head_plan canonical(
        http_response_body_plan body_plan, std::uint64_t value) noexcept {
        return http_framed_response_head_plan(body_plan, content_length_mode_type::canonical, value);
    }

    [[nodiscard]] static std::variant<http_framed_response_head_plan, http_framed_response_head_plan_error>
    preserve_explicit(http_response_body_plan body_plan, const http_response& response) noexcept {
        if (!response_has_known_header(response, response_header_content_length)) {
            return omit(body_plan);
        }

        const auto value = http_response_header_state_access::known_header_value(
            response.headers(), response_header_content_length);
        http_content_length_state state;
        if (state.parse_field(value) != http_content_length_parse_status::ok) {
            return http_framed_response_head_plan_error::invalid_content_length;
        }
        return http_framed_response_head_plan(
            body_plan, content_length_mode_type::explicit_value, *state.value());
    }

    [[nodiscard]] static http_framed_response_head_plan omit(http_response_body_plan body_plan) noexcept {
        return http_framed_response_head_plan(body_plan, content_length_mode_type::omit);
    }

private:
    enum class content_length_mode_type : std::uint8_t {
        omit,
        canonical,
        explicit_value,
    };

    http_framed_response_head_plan(http_response_body_plan body_plan,
        content_length_mode_type content_length_mode, std::uint64_t content_length = 0) noexcept
        : body_plan_(body_plan),
          content_length_mode_(content_length_mode),
          content_length_(content_length) {}

    http_response_body_plan body_plan_;
    content_length_mode_type content_length_mode_;
    std::uint64_t content_length_{0};
};

static_assert(std::is_trivially_copyable_v<http_framed_response_head_plan>);
static_assert(sizeof(http_framed_response_head_plan) <= 24);

using http_framed_response_head_plan_result =
    std::variant<http_framed_response_head_plan, http_framed_response_head_plan_error>;

[[nodiscard]] inline http_framed_response_head_plan_result http_framed_buffered_response_head_plan(
    const http_response_body_plan& body_plan, std::uint64_t content_length,
    const http_response& response) noexcept {
    if (body_plan.response_status() != response.status()) {
        return http_framed_response_head_plan_error::response_status_mismatch;
    }
    if (content_length != body_plan.buffered_representation_length(response)) {
        return http_framed_response_head_plan_error::response_representation_mismatch;
    }
    // An empty suppressed HEAD body describes the GET representation only
    // through its declared Content-Length, if any (RFC 9110 section 8.6).
    if (body_plan.body_suppressed() && body_plan.status_allows_body() &&
        body_plan.explicit_content_length_allowed() && content_length == 0 &&
        (response.has_head_metadata_body() ||
            response_has_known_header(response, response_header_content_length))) {
        return http_framed_response_head_plan::preserve_explicit(body_plan, response);
    }
    if (body_plan.auto_content_length_allowed()) {
        return http_framed_response_head_plan::canonical(
            body_plan, body_plan.status_allows_body() ? content_length : 0);
    }
    if (body_plan.explicit_content_length_allowed()) {
        return http_framed_response_head_plan::preserve_explicit(body_plan, response);
    }
    return http_framed_response_head_plan::omit(body_plan);
}

[[nodiscard]] inline http_framed_response_head_plan_result http_framed_buffered_response_head_plan(
    const http_buffered_response_write_plan& write_plan, const http_response& response) noexcept {
    return http_framed_buffered_response_head_plan(
        write_plan.body_plan(), write_plan.content_length(), response);
}

[[nodiscard]] inline http_framed_response_head_plan_result http_framed_streaming_response_head_plan(
    const http_response_body_plan& body_plan, const http_response& response) noexcept {
    if (body_plan.response_status() != response.status()) {
        return http_framed_response_head_plan_error::response_status_mismatch;
    }
    if (body_plan.auto_content_length_allowed() && !body_plan.status_allows_body()) {
        return http_framed_response_head_plan::canonical(body_plan, 0);
    }
    if (body_plan.explicit_content_length_allowed()) {
        return http_framed_response_head_plan::preserve_explicit(body_plan, response);
    }
    return http_framed_response_head_plan::omit(body_plan);
}

// A successful CONNECT opens a tunnel and carries no Content-Length (RFC 9110
// §9.3.6); any other outcome must take the generic final-response path.
[[nodiscard]] inline http_framed_response_head_plan_result http_framed_connect_response_head_plan(
    const http_response_body_plan& body_plan) noexcept {
    if (body_plan.content_semantics() == ::ruvia::http_response_content_semantics::connect_tunnel) {
        return http_framed_response_head_plan::omit(body_plan);
    }
    return http_framed_response_head_plan_error::connect_tunnel_required;
}

}  // namespace ruvia::detail

#pragma once

namespace ruvia::detail {

// RFC 9113 §6.5.2 / RFC 9114 §4.2.2: decoded fields, including
// pseudo-fields, cost their name and value lengths plus 32 bytes.
// Rejected additions leave the accumulated size unchanged.
class binary_field_section_size final {
public:
    explicit constexpr binary_field_section_size(std::uint64_t limit) noexcept
        : limit_(limit) {}

    [[nodiscard]] bool add(std::string_view name, std::string_view value) noexcept {
        constexpr std::uint64_t field_metadata_bytes = 32;
        const auto remaining = limit_ - bytes_;
        if (remaining < field_metadata_bytes || name.size() > remaining - field_metadata_bytes ||
            value.size() > remaining - field_metadata_bytes - name.size()) {
            return false;
        }
        bytes_ += name.size() + value.size() + field_metadata_bytes;
        return true;
    }

private:
    std::uint64_t limit_;
    std::uint64_t bytes_{0};
};

// Borrow unchanged names; request writable storage only at the first uppercase
// byte. The provider must keep the input alive and return name.size() bytes.
template <typename prepare_type>
[[nodiscard]] inline std::string_view http_lower_header_name(
    std::string_view name, const prepare_type& prepare) {
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto byte = static_cast<unsigned char>(name[i]);
        if (byte < 'A' || byte > 'Z') {
            continue;
        }
        auto* target = prepare(name.size());
        if (i != 0) {
            std::memcpy(target, name.data(), i);
        }
        for (std::size_t index = i; index < name.size(); ++index) {
            target[index] = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(name[index])));
        }
        return std::string_view(target, name.size());
    }
    return name;
}

inline constexpr std::size_t http_origin_length_prefix_bytes = 2;
inline constexpr std::size_t http_max_prefixed_origin_length = 0xffff;

[[nodiscard]] inline bool reserve_http_prefixed_origin(
    std::size_t& remaining, std::string_view origin) noexcept {
    if (origin.size() > http_max_prefixed_origin_length ||
        remaining < http_origin_length_prefix_bytes ||
        origin.size() > remaining - http_origin_length_prefix_bytes) {
        return false;
    }
    remaining -= http_origin_length_prefix_bytes + origin.size();
    return true;
}

inline char* write_http_prefixed_origin(char* out, std::string_view origin) noexcept {
    *out++ = static_cast<char>(origin.size() >> 8);
    *out++ = static_cast<char>(origin.size());
    if (!origin.empty()) {
        std::memcpy(out, origin.data(), origin.size());
    }
    return out + origin.size();
}

// Shared RFC 8336 §2 / RFC 9412 §2 ORIGIN payload validator and serializer.
[[nodiscard]] inline std::variant<std::size_t, http_connection_advertisement_error>
http_origin_payload_size(std::span<const std::string_view> origins, std::size_t payload_limit) {
    std::size_t remaining = payload_limit;
    for (const auto origin : origins) {
        if (!is_valid_http_serialized_origin(origin)) {
            return http_connection_advertisement_error::invalid_origin;
        }
        if (!reserve_http_prefixed_origin(remaining, origin)) {
            return http_connection_advertisement_error::limit;
        }
    }
    return payload_limit - remaining;
}

inline void write_http_origin_payload(
    std::span<char> payload, std::span<const std::string_view> origins) noexcept {
    auto* out = payload.data();
    for (const auto origin : origins) {
        out = write_http_prefixed_origin(out, origin);
    }
}

// Local HTTP message content accounting is distinct from the peer-body accounting
// stored on the same bidirectional stream. In particular, a server receives request
// content while independently producing response content.
class http_local_content_known_length final {
public:
    [[nodiscard]] constexpr std::uint64_t declared_length() const noexcept {
        return declared_length_;
    }

private:
    friend class http_local_content_state;

    explicit constexpr http_local_content_known_length(std::uint64_t declared_length) noexcept
        : declared_length_(declared_length) {}

    std::uint64_t declared_length_;
};

enum class http_local_content_check : std::uint8_t {
    accepted,
    not_started,
    forbidden,
    length_exceeded,
    length_incomplete
};

class http_local_content_state final {
    enum class phase_type : std::uint8_t { unset,
        forbidden,
        unbounded };

public:
    constexpr http_local_content_state() noexcept = default;

    void begin_forbidden() noexcept {
        reset(content_type(phase_type::forbidden));
    }

    void begin_unbounded() noexcept {
        reset(content_type(phase_type::unbounded));
    }

    void begin_known_length(std::uint64_t length) noexcept {
        reset(content_type(http_local_content_known_length(length)));
    }

    [[nodiscard]] constexpr const void* unset() const& noexcept {
        return has_phase(phase_type::unset) ? this : nullptr;
    }
    const void* unset() const&& = delete;

    [[nodiscard]] constexpr const void* forbidden() const& noexcept {
        return has_phase(phase_type::forbidden) ? this : nullptr;
    }
    const void* forbidden() const&& = delete;

    [[nodiscard]] constexpr const void* unbounded() const& noexcept {
        return has_phase(phase_type::unbounded) ? this : nullptr;
    }
    const void* unbounded() const&& = delete;

    [[nodiscard]] constexpr const http_local_content_known_length* known_length() const& noexcept {
        return std::get_if<http_local_content_known_length>(&content_);
    }
    const http_local_content_known_length* known_length() const&& = delete;

    // Transactional preflight for one submit_data input. No counters change here.
    // A terminal known-length submission must complete the declared length exactly;
    // callers can retry a rejected input with a corrected terminal flag or size.
    [[nodiscard]] http_local_content_check check_accept(
        std::size_t bytes_value, bool terminal) const noexcept {
        if (unset() != nullptr) {
            return http_local_content_check::not_started;
        }
        if (forbidden() != nullptr) {
            return http_local_content_check::forbidden;
        }

        const auto amount = static_cast<std::uint64_t>(bytes_value);
        if (amount > std::numeric_limits<std::uint64_t>::max() - accepted_bytes_) {
            return http_local_content_check::length_exceeded;
        }
        const auto* known_length_content = known_length();
        if (known_length_content == nullptr) {
            return http_local_content_check::accepted;
        }
        const auto declared_length = known_length_content->declared_length();
        if (accepted_bytes_ > declared_length || amount > declared_length - accepted_bytes_) {
            return http_local_content_check::length_exceeded;
        }
        if (terminal && accepted_bytes_ + amount != declared_length) {
            return http_local_content_check::length_incomplete;
        }
        return http_local_content_check::accepted;
    }

    void commit(std::size_t bytes_value) noexcept {
        accepted_bytes_ += static_cast<std::uint64_t>(bytes_value);
    }

    [[nodiscard]] bool length_complete() const noexcept {
        if (unset() != nullptr) {
            return false;
        }
        const auto* known_length_content = known_length();
        return known_length_content == nullptr ||
               accepted_bytes_ == known_length_content->declared_length();
    }

private:
    using content_type = std::variant<phase_type, http_local_content_known_length>;

    [[nodiscard]] constexpr bool has_phase(phase_type phase) const noexcept {
        const auto* current = std::get_if<phase_type>(&content_);
        return current != nullptr && *current == phase;
    }

    void reset(content_type content) noexcept {
        content_ = content;
        accepted_bytes_ = 0;
    }

    content_type content_{phase_type::unset};
    std::uint64_t accepted_bytes_{0};
};

[[nodiscard]] inline bool http_binary_is_valid_outbound_regular_header(
    std::string_view name, std::string_view value) noexcept {
    return is_valid_http_header_name(name) && is_valid_http_header_value(value) &&
           !is_forbidden_http_binary_connection_field(name);
}

// Cross-field constraints only. Decoded :path syntax is already checked;
// senders combine this predicate with the origin-form grammar below.
[[nodiscard]] inline bool http_binary_request_path_matches_method_and_scheme(
    http_known_method method, std::string_view scheme, std::string_view path) noexcept {
    if (path.empty()) {
        return !is_http_request_scheme(scheme);
    }
    return path != "*" || method == http_known_method::options;
}

[[nodiscard]] inline bool http_binary_is_valid_regular_request_path(
    http_known_method method, std::string_view scheme, std::string_view path) noexcept {
    return http_binary_request_path_matches_method_and_scheme(method, scheme, path) &&
           (path.empty() || path == "*" || is_valid_origin_form_target(path));
}

[[nodiscard]] inline bool http_binary_is_valid_extended_connect_path(
    std::string_view scheme, std::string_view path) noexcept {
    return http_binary_request_path_matches_method_and_scheme(http_known_method::connect, scheme, path) &&
           (path.empty() || is_valid_origin_form_target(path));
}

[[nodiscard]] inline bool http_binary_is_valid_outbound_method(
    std::string_view method, http_known_method known_method) noexcept {
    if (known_method == http_known_method::connect) {
        return false;
    }
    return known_method != http_known_method::unknown || is_valid_http_method_token(method);
}

[[nodiscard]] inline bool http_binary_are_valid_outbound_request_headers(std::optional<std::string_view> authority,
    std::uint16_t default_port, std::span<const http_header_view> headers, bool allow_host,
    bool allow_trailers, binary_field_section_size& section_size, request_field_state& fields,
    http_request_field_source source, bool websocket_connect = false) noexcept {
    if (!is_valid_http_request_field_source(source)) {
        return false;
    }
    websocket_request_header_validator websocket_headers(http_field_list_role::sender);
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto& header = headers[i];
        if (!http_binary_is_valid_outbound_regular_header(header.name(), header.value()) ||
            !section_size.add(header.name(), header.value())) {
            return false;
        }
        const auto kind = classify_http_field(header.name());
        if ((!allow_trailers && (kind == http_field_kind::trailer || kind == http_field_kind::te)) ||
            (!allow_host && kind == http_field_kind::host)) {
            return false;
        }
        // Content-Length and Expect belong exclusively to the typed request
        // content and expectation plans; the validator refuses raw fields.
        if (validate_request_field(kind, header.value(), http_field_list_role::sender, fields, source) !=
            request_field_status::ok) {
            return false;
        }
        if (kind == http_field_kind::host && authority.has_value() &&
            !authority_matches_host(*authority, header.value(), default_port)) {
            return false;
        }
        // Host is already refused for websocket extended CONNECT by the outbound field
        // check (:authority is the sole target authority there).
        if (websocket_connect &&
            (kind == http_field_kind::sec_websocket_key ||
                (kind == http_field_kind::other &&
                    http_ascii_equals_ignore_case(header.name(), "sec-websocket-accept")) ||
                !websocket_headers.observe_header(kind, header.value()))) {
            return false;
        }
    }
    return !websocket_connect || websocket_headers.client_offers_valid();
}

[[nodiscard]] inline bool http_binary_is_valid_outbound_regular_request_head(std::string_view method,
    http_known_method known_method, std::string_view scheme, std::uint16_t default_port,
    std::optional<std::string_view> authority, std::string_view path,
    std::span<const http_header_view> headers,
    http_request_content_indication content_indication, std::string_view content_length_value,
    http_client_request_expectation expectation, http_request_field_source source, std::uint64_t limit) noexcept {
    if (!http_binary_is_valid_outbound_method(method, known_method) || !is_valid_uri_scheme(scheme) ||
        (authority.has_value() && !is_valid_binary_request_authority(scheme, *authority)) ||
        !http_binary_is_valid_regular_request_path(known_method, scheme, path) ||
        (!authority.has_value() && binary_regular_request_requires_authority(scheme, path))) {
        return false;
    }
    // An asterisk-form target carries no authority information; binary senders
    // omit :authority (RFC 9113 §8.3.1 / RFC 9114 §4.3.1).
    if (path == "*" && authority.has_value()) {
        return false;
    }
    binary_field_section_size section_size(limit);
    bool expect_continue = false;
    switch (expectation) {
        case http_client_request_expectation::none:
            break;
        case http_client_request_expectation::continue_value:
            expect_continue = true;
            break;
        default:
            return false;
    }
    if (!http_client_expectation_is_valid(expect_continue, content_indication) ||
        !section_size.add(":method", method) || !section_size.add(":scheme", scheme) ||
        (authority.has_value() && !section_size.add(":authority", *authority)) ||
        !section_size.add(":path", path) ||
        (expect_continue && !section_size.add("expect", http_continue_expectation_token))) {
        return false;
    }
    request_field_state fields{request_field_framing::binary};
    if (!http_binary_are_valid_outbound_request_headers(authority, default_port, headers,
            /*allow_host=*/true,
            /*allow_trailers=*/true, section_size, fields, source)) {
        return false;
    }
    if (!content_length_value.empty() && !section_size.add("content-length", content_length_value)) {
        return false;
    }
    if (content_indication == http_request_content_indication::no_content) {
        return true;
    }
    const auto content_semantics = classify_http_request_content_semantics(method);
    // RFC 9110 section 9.3.7 applies the Content-Type requirement to generation.
    return content_semantics != http_request_content_semantics::forbidden &&
           (source == http_request_field_source::forwarded ||
               content_semantics != http_request_content_semantics::content_type_required ||
               fields.seen(http_field_kind::content_type));
}

// Sinks expose clear(), reserve(count), add(lowercase_name, borrowed_value),
// add_lowercase(name, borrowed_value), and add_owned_value(lowercase_name, value).
// add_owned_value copies temporary formatting/date bytes; the other values must
// outlive submission. Each sink applies http_field_never_indexed to every field.
// Validation and size preflight complete before any sink mutation.
template <typename sink_type>
inline void append_http_binary_response_field(sink_type& fields, std::string_view name,
    std::string_view value, std::uint32_t known_bit) {
    const auto known_name = response_known_header_lowercase_name(known_bit);
    if (known_name.empty()) {
        fields.add_lowercase(name, value);
    } else {
        fields.add(known_name, value);
    }
}

[[nodiscard]] inline bool validate_http_binary_interim_response_fields(
    const http_interim_response_head& response, std::uint64_t limit) noexcept {
    const auto headers = response.headers();
    const auto status_token = http_status_code_token(response.status());
    binary_field_section_size section_size(limit);
    if (!section_size.add(":status", http_status_code_token_view(status_token))) {
        return false;
    }
    http_interim_response_sender_validator validator;
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto name = headers[i].name();
        const auto value = headers[i].value();
        if (validator.validate(name, value) != http_interim_response_header_validation_status::ok ||
            is_forbidden_http_binary_response_field(name) || !section_size.add(name, value)) {
            return false;
        }
    }
    return true;
}

template <typename sink_type>
[[nodiscard]] inline bool append_http_binary_interim_response_fields(
    sink_type& fields, const http_interim_response_head& response, std::uint64_t limit) {
    if (!validate_http_binary_interim_response_fields(response, limit)) {
        return false;
    }
    fields.clear();
    const auto headers = response.headers();
    const auto status_token = http_status_code_token(response.status());
    fields.reserve(headers.size() + 1);
    fields.add_owned_value(":status", http_status_code_token_view(status_token));
    for (std::size_t i = 0; i < headers.size(); ++i) {
        append_http_binary_response_field(fields, headers[i].name(), headers[i].value(),
            classify_response_header_name(headers[i].name()));
    }
    return true;
}

template <typename sink_type>
[[nodiscard]] inline bool append_http_binary_response_fields(sink_type& fields,
    const http_response& response, const http_framed_response_head_plan& plan, std::uint64_t limit) {
    // The owning header block already enforces syntax, sender metadata grammar
    // and singleton rules. Only binary framing and section limits remain here.
    const auto known_bits = response_known_header_bits(response);
    const auto status_token = http_status_code_token(plan.body_plan().response_status());
    const auto status_value = http_status_code_token_view(status_token);
    binary_field_section_size section_size(limit);
    if (!section_size.add(":status", status_value)) {
        return false;
    }
    // A replaceable chunked hint has no binary-protocol form; Content-Length
    // comes exclusively from the prepared plan used for content accounting.
    constexpr auto writer_framed_bits = response_header_content_length | response_header_transfer_encoding;
    for (const auto& header : response.headers()) {
        if ((http_response_header_access::known_bit(header) & writer_framed_bits) != 0) {
            continue;
        }
        if (!section_size.add(header.name(), header.value())) {
            return false;
        }
    }
    std::string_view generated_date;
    if ((known_bits & response_header_date) == 0) {
        generated_date = cached_date_value();
        if (!generated_date.empty() && !section_size.add("date", generated_date)) {
            return false;
        }
    }
    std::array<char, http_max_unsigned_decimal_size> content_length_bytes;
    std::string_view content_length_value;
    if (const auto content_length = plan.content_length()) {
        content_length_value = http_format_unsigned(content_length_bytes, *content_length);
        if (!section_size.add("content-length", content_length_value)) {
            return false;
        }
    }

    fields.clear();
    fields.reserve(response.headers().size() + 3);
    fields.add_owned_value(":status", status_value);
    for (const auto& header : response.headers()) {
        const auto known_bit = http_response_header_access::known_bit(header);
        if ((known_bit & writer_framed_bits) != 0) {
            continue;
        }
        append_http_binary_response_field(fields, header.name(), header.value(), known_bit);
    }
    if (!generated_date.empty()) {
        fields.add_owned_value("date", generated_date);
    }
    if (!content_length_value.empty()) {
        fields.add_owned_value("content-length", content_length_value);
    }
    return true;
}

[[nodiscard]] inline bool is_valid_http_binary_connect_response_head(const http_response& response) noexcept {
    const auto& body = response_body(response);
    if (!response.status().is_successful() || body.size() != 0 || body.file().has_value()) {
        return false;
    }
    return !response_has_known_header(
        response, response_header_content_length | response_header_transfer_encoding);
}

template <typename sink_type>
[[nodiscard]] inline bool append_http_binary_websocket_response_fields(
    sink_type& fields, const websocket_server_negotiation& negotiation, std::uint64_t limit) {
    // Negotiation fields are already lowercase and validated by their owner.
    const auto response_fields = negotiation.response_headers();
    const bool has_date = std::ranges::any_of(response_fields, [](const http_header& field) {
        return field.name() == "date";
    });
    const auto date = has_date ? std::string_view{} : cached_date_value();
    const auto subprotocol = negotiation.subprotocol();
    const auto extensions = negotiation.extensions();
    binary_field_section_size section_size(limit);
    if (!section_size.add(":status", "200") ||
        (!date.empty() && !section_size.add("date", date)) ||
        (!subprotocol.empty() && !section_size.add("sec-websocket-protocol", subprotocol)) ||
        (!extensions.empty() && !section_size.add("sec-websocket-extensions", extensions))) {
        return false;
    }
    for (const auto& field : response_fields) {
        if (!section_size.add(field.name(), field.value())) {
            return false;
        }
    }

    fields.clear();
    fields.reserve(response_fields.size() + 4);
    fields.add(":status", "200");
    if (!date.empty()) {
        fields.add_owned_value("date", date);
    }
    if (!subprotocol.empty()) {
        fields.add("sec-websocket-protocol", subprotocol);
    }
    if (!extensions.empty()) {
        fields.add("sec-websocket-extensions", extensions);
    }
    for (const auto& field : response_fields) {
        fields.add(field.name(), field.value());
    }
    return true;
}

// The first input byte occupies the low byte, independent of native byte order.
// The caller must provide at least eight readable bytes.
[[nodiscard]] inline std::uint64_t load_http_field_value_word(const char* data) noexcept {
    std::uint64_t bytes;
    std::memcpy(&bytes, data, sizeof(bytes));
    if constexpr (std::endian::native == std::endian::big) {
        bytes = 0;
        for (std::size_t index = 0; index < sizeof(bytes); ++index) {
            bytes |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[index])) << (index * 8);
        }
    }
    return bytes;
}

// The first set high bit identifies the first invalid input byte. DEL detection
// can also mark later bytes, but never changes the position of its first match.
[[nodiscard]] inline std::uint64_t invalid_http_field_value_mask(std::uint64_t bytes_value) noexcept {
    constexpr std::uint64_t ones = 0x0101010101010101ULL;
    constexpr std::uint64_t high_bits = ones * 0x80U;
    constexpr std::uint64_t low_bits = ones * 0x7fU;
    const auto del = bytes_value ^ low_bits;
    // Setting each high bit prevents inter-byte borrow when testing < SP.
    // Exclude original high-bit bytes so obs-text remains valid.
    const auto controls = ~((bytes_value | high_bits) - ones * 0x20U) & ~bytes_value;
    const auto special = (controls | ((del - ones) & ~del)) & high_bits;
    if (special == 0) {
        return 0;
    }
    // This exact HTAB mask cannot carry between bytes or hide a DEL match.
    const auto tabs = bytes_value ^ (ones * 9U);
    const auto tab_bits = ~(((tabs & low_bits) + low_bits) | tabs | low_bits);
    return special & ~tab_bits;
}

[[nodiscard]] inline std::size_t http_field_value_prefix_size(std::string_view value) noexcept {
    std::size_t index = 0;
    while (value.size() - index >= sizeof(std::uint64_t)) {
        const auto bytes_value = load_http_field_value_word(value.data() + index);
        if (const auto invalid = invalid_http_field_value_mask(bytes_value); invalid != 0) {
            return index + static_cast<std::size_t>(std::countr_zero(invalid)) / 8;
        }
        index += sizeof(bytes_value);
    }
    while (index < value.size() && is_http_field_value_char(static_cast<unsigned char>(value[index]))) {
        ++index;
    }
    return index;
}

enum class field_line_folding_policy { mime,
    http };

// A fold always starts with CRLF followed by SP/HTAB. Leave bare CR/LF and
// ordinary field separators visible to the protocol's field validator.
[[nodiscard]] inline std::size_t find_field_line_fold(
    std::string_view input, std::size_t start = 0) noexcept {
    auto position = input.find("\r\n", start);
    while (position != std::string_view::npos) {
        if (position + 2 < input.size() && is_http_ows_char(input[position + 2])) {
            return position;
        }
        position = input.find("\r\n", position + 2);
    }
    return std::string_view::npos;
}

// first_fold comes from find_field_line_fold. The caller owns storage through
// every use of the resulting views; input must not borrow that storage.
// MIME removes only CRLF. HTTP replaces the complete obs-fold, including its
// adjacent OWS/RWS, with SP before field values are interpreted (RFC 9112 5.2).
inline void copy_unfolded_field_lines(std::string_view input, std::size_t first_fold,
    field_line_folding_policy policy, std::string& storage) {
    storage.clear();
    storage.reserve(input.size());
    std::size_t segment_begin = 0;
    for (auto position = first_fold; position != std::string_view::npos;) {
        auto segment_end = position;
        auto next_segment = position + 2;
        if (policy == field_line_folding_policy::http) {
            while (segment_end > segment_begin && is_http_ows_char(input[segment_end - 1])) {
                --segment_end;
            }
            while (next_segment < input.size() && is_http_ows_char(input[next_segment])) {
                ++next_segment;
            }
        }
        storage.append(input.substr(segment_begin, segment_end - segment_begin));
        if (policy == field_line_folding_policy::http) {
            storage.push_back(' ');
        }
        segment_begin = next_segment;
        position = find_field_line_fold(input, next_segment);
    }
    storage.append(input.substr(segment_begin));
}

// Scan one complete RFC 9112 section 5 field line, including its CRLF.
// The caller excludes the final empty line and unfolds response obs-fold first.
// Views borrow buffer; only a successful scan advances cursor_value.
[[nodiscard]] inline std::optional<http_header_view> scan_http1_field_line(
    std::string_view buffer, std::size_t& cursor_value) noexcept {
    const auto name_start = cursor_value;
    auto position = cursor_value + http_token_prefix_size(buffer.substr(cursor_value));
    if (position == name_start || position >= buffer.size() || buffer[position] != ':') {
        return std::nullopt;
    }
    const auto name = buffer.substr(name_start, position - name_start);
    ++position;
    while (position < buffer.size() && is_http_ows_char(buffer[position])) {
        ++position;
    }
    const auto value_start = position;
    position += http_field_value_prefix_size(buffer.substr(position));
    if (position + 1 >= buffer.size() || buffer[position] != '\r' || buffer[position + 1] != '\n') {
        return std::nullopt;
    }
    // Leading OWS has already been skipped; trim only the trailing OWS.
    auto value_end = position;
    while (value_end > value_start && is_http_ows_char(buffer[value_end - 1])) {
        --value_end;
    }
    cursor_value = position + 2;
    return http_header_view{name, buffer.substr(value_start, value_end - value_start)};
}

// Request slots are assigned only when appended; unused inline slots need no
// per-request zeroing. Offsets keep their borrowed bytes independent of moves.
struct http_header_slice {
    std::uint32_t offset_;
    std::uint32_t length_;

    [[nodiscard]] std::string_view bind(std::string_view buffer) const noexcept {
        return buffer.substr(offset_, length_);
    }

    template <http_temporary_owning_char_string buffer_type>
    std::string_view bind(buffer_type&&) const = delete;
};

struct parsed_request_header_slot {
    http_header_slice name_;
    http_header_slice value_;
    http_field_kind kind_;
};

using known_request_header_index_type = std::int16_t;
// parse_http_header_block stops at max_http_header_fields, so every field index
// fits the index type and the request's descriptor block.
static_assert(max_http_header_fields <=
              static_cast<std::size_t>((std::numeric_limits<known_request_header_index_type>::max)()));

struct parsed_request_header_block {
    parsed_request_header_block() noexcept = default;

    http_header_slice method_;
    http_header_slice target_;
    http_protocol_version protocol_version_;
    http_header_descriptor_storage<parsed_request_header_slot> headers_;
    known_request_header_index_type host_header_index_{-1};
    http_connection_options connection_options_;
    http_content_length_state content_length_;
    request_field_state fields_{request_field_framing::http1};
    http_transfer_encoding_state transfer_encoding_;
    http_request_expectations expectations_;
};

[[nodiscard]] std::size_t find_http_header_end(
    std::string_view buffer, std::size_t search_offset) noexcept;
// True if an LF at or after search_offset (and within max_http_header_bytes)
// is not preceded by CR. Within a field section such a line can never end the
// CRLF-delimited head; the caller passes the end of the accepted request-line
// or status-line, or a later offset whose earlier bytes were already scanned.
[[nodiscard]] bool has_http_bare_line_feed(std::string_view buffer, std::size_t search_offset) noexcept;
// RFC 9112 section 2.2: a server SHOULD ignore at least one empty line received
// before the request-line. Leading CRLF or bare LF lines are skipped; they count
// toward max_http_header_bytes, which bounds how many are ignored. The scan
// resumes after `skipped` bytes already classified by an earlier call.
[[nodiscard]] std::size_t http_request_leading_empty_line_bytes(
    std::string_view buffer, std::size_t skipped) noexcept;
// find_http_header_end for a request head: the returned end is measured from
// the start of buffer and includes the ignored leading empty lines, whose
// count `leading_bytes` carries in (from an earlier call) and out.
[[nodiscard]] std::size_t find_http_request_head_end(
    std::string_view buffer, std::size_t& leading_bytes, std::size_t search_offset) noexcept;
// Classifies the request-line of a still incomplete head: a request-line error
// once the first line after the leading empty lines is CRLF-terminated, or as
// soon as its first byte cannot start a method token. Bytes before
// search_offset were classified by an earlier call. An accepted line sets
// request_line_end to the offset just past its CRLF; nullopt without it means
// more input is needed.
[[nodiscard]] std::optional<http_parse_error> classify_http_request_line_prefix(std::string_view buffer,
    std::size_t leading_bytes, std::size_t search_offset, std::size_t& request_line_end) noexcept;
// header_bytes is the find_http_request_head_end result; the request-line is
// parsed after the leading_bytes ignored leading empty lines.
[[nodiscard]] std::optional<http_parse_error> parse_http_header_block(std::string_view buffer,
    std::size_t leading_bytes, std::size_t header_bytes, parsed_request_header_block& block);

enum class http1_version_parse_result : std::uint8_t { invalid,
    unsupported,
    http10,
    http11 };

// HTTP-version = "HTTP/" DIGIT "." DIGIT (RFC 9112 section 2.3). A higher
// HTTP/1 minor version is processed as HTTP/1.1 (RFC 9110 section 6.2).
[[nodiscard]] inline http1_version_parse_result parse_http1_version(std::string_view version) noexcept {
    if (version.size() != 8 || !version.starts_with("HTTP/") || version[5] < '0' || version[5] > '9' ||
        version[6] != '.' || version[7] < '0' || version[7] > '9') {
        return http1_version_parse_result::invalid;
    }
    if (version[5] != '1') {
        return http1_version_parse_result::unsupported;
    }
    return version[7] == '0' ? http1_version_parse_result::http10 : http1_version_parse_result::http11;
}

// Callers decide whether a framing field applies before accumulating it and
// retain their own error enum. Unsupported transfer codings are recorded by
// the accumulator for the caller's whole-section policy, not rejected here.
template <typename error_type>
[[nodiscard]] std::optional<error_type> parse_http1_content_length_field(
    std::string_view value, http_content_length_state& state) noexcept {
    switch (state.parse_field(value)) {
        case http_content_length_parse_status::ok:
            return std::nullopt;
        case http_content_length_parse_status::invalid:
            return error_type::invalid_content_length;
        case http_content_length_parse_status::conflicting:
            return error_type::conflicting_content_length;
    }
    return std::nullopt;
}

template <typename error_type>
[[nodiscard]] std::optional<error_type> parse_http1_transfer_encoding_field(
    std::string_view value, http_transfer_encoding_state& state) {
    if (state.parse_field(value) == http_transfer_encoding_parse_status::malformed) {
        return error_type::invalid_transfer_encoding;
    }
    return std::nullopt;
}

// The two steps between an HTTP/1 client response head on the wire and the plan
// the parser acts on: reading the status line and header fields into borrowed
// values, then deciding from them (and the request that produced them) how the
// body is framed and whether the connection survives.

struct http_client_response_head_access final {
    [[nodiscard]] static http_client_response_head make(
        http_received_status status, http_protocol_version protocol_version) noexcept {
        return http_client_response_head(status, protocol_version);
    }

    [[nodiscard]] static std::vector<http_header>& headers(
        http_client_response_head& head) noexcept {
        return head.headers_;
    }
    static void content_length(http_client_response_head& head, std::optional<std::uint64_t> value) noexcept {
        head.content_length_ = value;
    }
};

struct http1_client_exchange_state_access final {
    [[nodiscard]] static constexpr http_known_method method(
        const http1_client_exchange_state& state_value) noexcept {
        return state_value.method_;
    }

    [[nodiscard]] static constexpr http1_close_policy close_policy(
        const http1_client_exchange_state& state_value) noexcept {
        return state_value.close_policy_;
    }

    [[nodiscard]] static constexpr http1_client_initial_content_state content_state(
        const http1_client_exchange_state& state_value) noexcept {
        return state_value.content_state_;
    }

    [[nodiscard]] static std::string_view offered_upgrade_protocols(
        const http1_client_exchange_state& state_value) noexcept {
        return state_value.offered_upgrade_protocols_;
    }
};

struct http1_client_parsed_response_head final {
    explicit http1_client_parsed_response_head(
        const http1_client_parsed_status_line& status_line) noexcept;

    http_received_status received_status_;
    // The status the response is processed as (received_status_.status()).
    http_status_code status_code_;
    http_protocol_version protocol_version_;
    bool saw_transfer_encoding_{false};
    bool all_upgrade_protocols_offered_{true};
    http_connection_options connection_options_;
    http_upgrade_protocols upgrade_protocols_;
    http_content_length_state content_length_;
    http_transfer_encoding_state transfer_encoding_;

    http_header_descriptor_storage<http_header_view> headers_;
};

using http1_client_status_line_parse_result_type =
    std::variant<http1_client_parsed_status_line, http1_client_response_parse_error>;
using http1_client_response_head_parse_result_type =
    std::variant<http1_client_parsed_response_head, http1_client_response_parse_error>;
using http1_client_response_planning_result_type =
    std::variant<http1_client_response_plan, http1_client_response_parse_error>;

// Parse a status line, excluding its CRLF.
[[nodiscard]] http1_client_status_line_parse_result_type parse_http1_client_status_line(
    std::string_view status_line) noexcept;

// Parse the field lines following a validated status line, each retaining its
// CRLF but excluding the final empty line. Response obs-fold has already been
// unfolded.
[[nodiscard]] http1_client_response_head_parse_result_type parse_http1_client_response_head_fields(
    const http1_client_parsed_status_line& status_line, std::string_view field_section,
    const http1_client_exchange_state& exchange_state);

// Decide the response plan a parsed head implies for this request.
[[nodiscard]] http1_client_response_planning_result_type plan_http1_client_response(
    const http1_client_exchange_state& exchange_state, const http1_client_parsed_response_head& response,
    http1_client_request_content_phase request_content_phase, bool interim_faulty_framing = false);

}  // namespace ruvia::detail
