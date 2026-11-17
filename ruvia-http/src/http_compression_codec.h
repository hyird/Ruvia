#pragma once

#include <brotli/decode.h>
#include <brotli/encode.h>
#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "http_c_library_memory.h"
#include "http_content_codec.h"

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
    explicit zlib_inflate_stream(int window_bits, std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)(), http_codec_memory* memory = nullptr);
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
    zlib_deflate_stream(int level, int window_bits, int mem_level, int strategy, std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)(), http_codec_memory* memory = nullptr);
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

// Brotli (RFC 7932) state with tracked working memory.
// Brotli retains the allocator context as its opaque pointer, so these types are
// pinned: owners embed them in storage whose address never changes.
//
// Construction rethrows an allocation failure captured during initialization.
// Any other Brotli refusal leaves the stream uninitialized so each owner maps it
// into its own error contract.

// Decoder whose native working memory is bounded by the caller's decoded-size
// cap rather than by the window and metablock sizes the peer declares: a
// declared ring buffer that the cap could never fill is refused, and
// native_budget_exceeded() then reports a cap violation instead of a codec
// failure.
class brotli_decoder_stream final {
public:
    explicit brotli_decoder_stream(std::size_t max_decoded_bytes, std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)(), http_codec_memory* memory = nullptr);
    ~brotli_decoder_stream();
    brotli_decoder_stream(const brotli_decoder_stream&) = delete;
    brotli_decoder_stream& operator=(const brotli_decoder_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return state_ != nullptr;
    }
    [[nodiscard]] BrotliDecoderState* native() const noexcept {
        return state_;
    }
    void rethrow_allocation_failure() const {
        allocation_.rethrow_if_failed();
    }
    [[nodiscard]] bool native_budget_exceeded() const noexcept {
        return allocation_.budget_exceeded();
    }

private:
    http_c_library_memory allocation_;
    BrotliDecoderState* state_;
};

// LZ77 window exponent of every HTTP Brotli encoder: Pingora's 512 KiB window,
// since a stream encoder lives as long as its response (SSE).
inline constexpr int brotli_stream_window_bits = 19;

// Encoder shared by whole-buffer and streaming HTTP content coding.
class brotli_encoder_stream final {
public:
    explicit brotli_encoder_stream(int window_bits, std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)(), http_codec_memory* memory = nullptr, int quality = 5);
    ~brotli_encoder_stream();
    brotli_encoder_stream(const brotli_encoder_stream&) = delete;
    brotli_encoder_stream& operator=(const brotli_encoder_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return state_ != nullptr;
    }
    // Consumes the input and drains the requested operation into output. False
    // means the total output budget was exceeded before appending a block.
    // After false or an exception, the owner must discard/rebuild this stream.
    [[nodiscard]] bool encode(std::string_view input, std::string& output, BrotliEncoderOperation operation, std::size_t max_encoded_bytes);

private:
    http_c_library_memory allocation_;
    BrotliEncoderState* state_;
};

// zstd (RFC 8878) state with tracked working memory and
// whose window never exceeds the 8 MiB HTTP limit of RFC 9659. zstd
// retains the allocator context as its opaque pointer, so these types are
// pinned: owners embed them in storage whose address never changes.
//
// Construction rethrows an allocation failure captured during initialization.
// Any other zstd refusal leaves the stream uninitialized so each owner maps it
// into its own error contract.

class zstd_decoder_stream final {
public:
    explicit zstd_decoder_stream(std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)(), http_codec_memory* memory = nullptr);
    ~zstd_decoder_stream();
    zstd_decoder_stream(const zstd_decoder_stream&) = delete;
    zstd_decoder_stream& operator=(const zstd_decoder_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return stream_ != nullptr;
    }
    [[nodiscard]] ZSTD_DStream* native() const noexcept {
        return stream_;
    }
    void rethrow_allocation_failure() const {
        allocation_.rethrow_if_failed();
    }
    [[nodiscard]] bool reset() noexcept;

private:
    http_c_library_memory allocation_;
    ZSTD_DStream* stream_;
};

// Encoder with the selected compression level and that level's own window.
// reset() ends any frame in progress and keeps both the parameters and the
// working memory, so the next frame is identical to one from a new encoder.
// false means the owner must destroy it.
class zstd_encoder_stream final {
public:
    explicit zstd_encoder_stream(std::size_t max_memory_bytes = (std::numeric_limits<std::size_t>::max)(), http_codec_memory* memory = nullptr, int level = 3);
    ~zstd_encoder_stream();
    zstd_encoder_stream(const zstd_encoder_stream&) = delete;
    zstd_encoder_stream& operator=(const zstd_encoder_stream&) = delete;

    [[nodiscard]] bool initialized() const noexcept {
        return context_ != nullptr;
    }
    [[nodiscard]] bool reset() noexcept;
    // The single zstd encode loop, shared by whole-buffer and streaming
    // coding: consumes the input and drains `operation` into output. False
    // means output would exceed `max_encoded_bytes` in total; no byte past
    // that cap is appended. After false or an exception, the owner must reset
    // or discard this stream.
    [[nodiscard]] bool encode(std::string_view input, std::string& output, ZSTD_EndDirective operation, std::size_t max_encoded_bytes);

private:
    http_c_library_memory allocation_;
    ZSTD_CCtx* context_;
};

// Shared pinned state for content and transfer decoding. Transfer stages embed
// this directly in their stack allocation; only the public opaque owner needs
// a separate allocation. There is exactly one incremental decode loop.
class http_content_decoder_stream final {
public:
    http_content_decoder_stream(http_content_coding coding, http_content_decode_options options);
    http_content_decoder_stream(const http_content_decoder_stream&) = delete;
    http_content_decoder_stream& operator=(const http_content_decoder_stream&) = delete;
    // Callers supply nonempty output, disjoint from compressed input.
    [[nodiscard]] http_content_decode_result decode(std::string_view input, std::span<char> output);
    [[nodiscard]] http_content_decode_result finish_input() noexcept;
    [[nodiscard]] decoder_state state() const noexcept {
        return phase_;
    }
    void reset();

private:
    friend class ::ruvia::http_content_decoder;

    void initialize();
    [[nodiscard]] http_content_decode_result decode_active(std::string_view input, std::span<char> output);
    [[nodiscard]] http_content_decode_result fail(std::size_t consumed, http_content_decode_error error) noexcept;
    using decoder_stream = std::variant<zlib_inflate_stream, brotli_decoder_stream, zstd_decoder_stream>;
    http_content_coding coding_;
    http_content_decode_options options_;
    std::optional<decoder_stream> stream_;
    decoder_state phase_{decoder_state::active};
    http_content_decode_error failure_{http_content_decode_error::invalid_content};
    std::size_t decoded_bytes_{0};
    bool received_input_{false};
    bool boundary_{false};
};

}  // namespace ruvia::detail
