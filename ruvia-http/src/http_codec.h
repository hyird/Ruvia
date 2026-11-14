#pragma once

#include <zlib.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "http_c_library_memory.h"
#include "http_content_coding.h"

// Compression library bindings shared by content codings, transfer codings and
// WebSocket permessage-deflate. Field grammar stays in http_connection_fields.h;
// only translation units that drive a native codec include this header.
namespace ruvia::detail {

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
    explicit codec_reset_guard(codec_type*& slot) noexcept :
        slot_(slot),
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
[[nodiscard]] inline bool append_decoded_bytes(std::string& output, const char* bytes_value, std::size_t size, std::size_t max_decoded_bytes) {
    if (output.size() > max_decoded_bytes || size > max_decoded_bytes - output.size()) {
        return false;
    }
    output.append(bytes_value, size);
    return true;
}

template <typename stream_type, typename operation_type>
[[nodiscard]] inline std::optional<std::string> encode_whole_content_with_stream(stream_type& encoder, operation_type finish_operation, std::string_view input, std::size_t max_encoded_bytes) {
    std::string output;
    if (!encoder.encode(input, output, finish_operation, max_encoded_bytes)) {
        return std::nullopt;
    }
    return output;
}

template <typename stream_type, typename operation_type, typename... arg_types>
[[nodiscard]] inline std::optional<std::string> encode_whole_content(const char* init_error, operation_type finish_operation, std::string_view input, std::size_t max_encoded_bytes, arg_types&&... args) {
    stream_type encoder(std::forward<arg_types>(args)...);
    if (!encoder.initialized()) {
        throw http_content_codec_error(init_error);
    }
    return encode_whole_content_with_stream(encoder, finish_operation, input, max_encoded_bytes);
}

[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_gzip_content(std::string_view input, std::size_t max_decoded_bytes);
[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_deflate_content(std::string_view input, std::size_t max_decoded_bytes);
[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_brotli_content(std::string_view input, std::size_t max_decoded_bytes);
[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_zstd_content(std::string_view input, std::size_t max_decoded_bytes);

[[nodiscard]] std::optional<std::string> encode_gzip_content(std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_deflate_content(std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_brotli_content(std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_zstd_content(std::string_view input, std::size_t max_encoded_bytes);

// A whole-buffer encode, one finishing pass of the stream's single encode
// loop, over caller-owned codec state that is freshly initialized or reset.
// Every path leaves the state consumed (finished or mid-stream), so an owner
// that reuses it resets it afterwards.
[[nodiscard]] std::optional<std::string> encode_zlib_content(zlib_deflate_stream& deflater, std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_brotli_content(brotli_encoder_stream& encoder, std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_zstd_content(zstd_encoder_stream& encoder, std::string_view input, std::size_t max_encoded_bytes);
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
    const auto count = static_cast<uInt>(std::min<std::size_t>(input.size() - supplied, (std::numeric_limits<uInt>::max)()));
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data() + supplied));
    stream.avail_in = count;
    supplied += count;
}

// zlib keeps the caller's input and output pointers in the z_stream. The
// borrowed buffers only live for one call, so the scope clears them when that
// call ends, including by exception.
class zlib_borrow_scope final {
public:
    explicit zlib_borrow_scope(z_stream& stream) noexcept :
        stream_(stream) {}
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
    explicit zlib_inflate_stream(int window_bits, std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)());
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
    zlib_deflate_stream(int level, int window_bits, int mem_level, int strategy, std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)());
    // The gzip or deflate content-coding stream with zlib's default parameters.
    explicit zlib_deflate_stream(zlib_content_format format, std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)());
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
    [[nodiscard]] bool encode(std::string_view input, std::string& output, int flush, std::size_t max_encoded_bytes);

private:
    http_c_library_memory allocation_;
    z_stream stream_;
};

}  // namespace ruvia::detail
