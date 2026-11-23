#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "borrowed_text.h"
#include "decode_step.h"
#include "http_codec_memory.h"
#include "http_content_coding.h"

namespace ruvia {

// A codec that could not run (for example its native state could not be
// created or reset, or the library reported an internal error). It is an
// internal failure, never a property of the representation; encode and decode
// both throw it, and allocation exceptions retain their original type.
class http_content_codec_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct http_content_encode_options final {
    std::size_t max_encoded_bytes_{};
};

struct http_content_decode_options final {
    std::size_t max_decoded_bytes_{};
    std::size_t max_memory_bytes_{(std::numeric_limits<std::size_t>::max)()};
    http_codec_memory* memory_{nullptr};
};

// gzip members and zstd frames are consumed through the end of input; Brotli
// trailing bytes are rejected. The exact decoded-size cap applies across the
// complete representation, and an identity representation over the cap is
// rejected before allocating. An empty representation decodes to empty content
// for every coding. Codec failures throw http_content_codec_error.
[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_http_content(http_content_coding coding, std::string_view input, http_content_decode_options options);

// Decodes the listed codings in reverse order. gzip members and zstd frames are
// consumed through the end of their input; Brotli trailing bytes are rejected.
// An empty coded representation decodes to empty content. The exact
// decoded-size cap applies to every intermediate representation, and each one
// is released before the next stage completes. Codec failures throw
// http_content_codec_error.
[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_http_content(std::span<const http_content_coding> codings, std::string_view input, http_content_decode_options options);

using http_content_decode_failure = decode_failure<http_content_decode_error>;
using http_content_decode_result = decode_step<decode_need_input, decode_output, decode_complete, http_content_decode_failure>;

// The sole incremental content decoder, also used by transfer decoding and the
// whole-buffer functions above. Input is never retained. Identity output borrows
// input without copying; compressed output borrows the caller's nonempty scratch.
// Compressed input must not overlap scratch. Drop consumed_bytes() and consume output
// before reusing scratch. Drain with decode({}, scratch) before providing more
// input or calling finish_input(); EOF rejects truncated streams. gzip members
// and zstd frames may be concatenated, so only EOF commits their final boundary.
// Empty representations complete at EOF, including compressed codings.
// Every returned byte is within the exact cumulative decoded-size limit.
// Terminal calls repeat their terminal result without consuming input; only
// reset() starts another representation. Exceptions mark the decoder failed
// and retain their original type. memory_ must outlive the decoder.
// A native data error with valid output delivers that output first (state() is
// already failed); the next decode() or finish_input() returns its typed failure.
class http_content_decoder final {
public:
    http_content_decoder(http_content_coding coding, http_content_decode_options options);
    ~http_content_decoder();
    http_content_decoder(const http_content_decoder&) = delete;
    http_content_decoder& operator=(const http_content_decoder&) = delete;
    http_content_decoder(http_content_decoder&&) = delete;
    http_content_decoder& operator=(http_content_decoder&&) = delete;

    [[nodiscard]] http_content_decode_result decode(std::string_view input RUVIA_LIFETIMEBOUND, std::span<char> output RUVIA_LIFETIMEBOUND) &;
    http_content_decode_result decode(std::string_view, std::span<char>) && = delete;
    template <detail::http_temporary_owning_char_string input_type>
    http_content_decode_result decode(input_type&&, std::span<char>) & = delete;
    template <std::same_as<char> element_type>
    http_content_decode_result decode(std::initializer_list<element_type>, std::span<char>) & = delete;
    [[nodiscard]] http_content_decode_result finish_input() & noexcept;
    http_content_decode_result finish_input() && = delete;
    [[nodiscard]] decoder_state state() const noexcept;
    void reset();

private:
    struct impl_type;
    impl_type* impl_;
};

struct http_content_encoder_options final {
    std::size_t max_memory_bytes_{(std::numeric_limits<std::size_t>::max)()};
    http_codec_memory* memory_{nullptr};
    // nullopt uses the coding's default. gzip/deflate: -1..9, Brotli: 0..11,
    // zstd: ZSTD_minCLevel()..ZSTD_maxCLevel(). identity rejects a level.
    std::optional<int> compression_level_{};
};

// The one content encoder: whole-buffer encode() and incremental write()/
// finish() run the same codec stream. An unknown coding value throws
// std::invalid_argument. options.max_memory_bytes_ bounds native working memory;
// options.memory_ optionally shares a request budget and custom allocator.
// Each write consumes all of its input and appends to caller-owned output; it
// does not retain either view. Input may borrow output. Compressed writes
// snapshot that input before an append can invalidate it.
// Output can be cleared between calls and remains valid after encoder destruction.
// Empty input is valid, including repeated flushes; it does not end the stream.
// Codec failures throw http_content_codec_error; allocation exceptions retain
// their original type when the codec returns through its C API. Brotli builds
// that exit on internal OOM cannot be recovered by this boundary.
// Either failure is terminal because input/output may have
// progressed. Later operations throw std::logic_error. finish() is idempotent;
// writing after successful finish throws std::logic_error without changing output.
// reset() returns an encoder in any phase to a fresh active stream. zlib and
// Zstd reuse their working memory; Brotli has no native reset and rebuilds,
// which may allocate. A reset failure leaves the encoder failed.
class http_content_encoder final {
public:
    explicit http_content_encoder(http_content_coding coding, http_content_encoder_options options = {});
    ~http_content_encoder();
    http_content_encoder(const http_content_encoder&) = delete;
    http_content_encoder& operator=(const http_content_encoder&) = delete;
    http_content_encoder(http_content_encoder&&) = delete;
    http_content_encoder& operator=(http_content_encoder&&) = delete;

    [[nodiscard]] http_content_coding coding() const noexcept {
        return coding_;
    }

    // flush emits pending bytes for low-latency response streams such as SSE.
    void write(std::string_view input, std::string& output, bool flush = false);
    void finish(std::string& output);
    // One complete representation within the exact output cap. nullopt means
    // only that max_encoded_bytes_ would be exceeded; zero permits only a
    // zero-byte encoding. Requires a fresh stream: after an incremental
    // write that consumes input or flushes a codec, encode() throws without
    // changing the stream. reset() before the next representation.
    [[nodiscard]] std::optional<std::string> encode(std::string_view input, http_content_encode_options options);
    void reset();
    [[nodiscard]] decoder_state state() const noexcept {
        switch (phase_) {
            case phase::fresh:
            case phase::streaming:
                return decoder_state::active;
            case phase::finished:
                return decoder_state::finished;
            case phase::failed:
                return decoder_state::failed;
        }
        std::terminate();
    }

private:
    struct impl_type;
    enum class phase : std::uint8_t {
        fresh,
        streaming,
        finished,
        failed,
    };
    http_content_coding coding_;
    impl_type* impl_{nullptr};
    phase phase_{phase::fresh};
};

// A fresh http_content_encoder's encode().
[[nodiscard]] std::optional<std::string> encode_http_content(http_content_coding coding, std::string_view input, http_content_encode_options options, http_content_encoder_options encoder_options = {});

class http_content_codecs;

// RAII lease for a pooled encoder from http_content_codecs. Destroying a
// non-discarded lease resets and returns the encoder to the pool, unless the
// scope is unwinding an exception: failed native state never crosses
// representations. discard() destroys a failed encoder instead of pooling it.
class http_content_encoder_lease final {
public:
    http_content_encoder_lease(const http_content_encoder_lease&) = delete;
    http_content_encoder_lease& operator=(const http_content_encoder_lease&) = delete;
    http_content_encoder_lease(http_content_encoder_lease&& other) noexcept :
        codecs_(std::exchange(other.codecs_, nullptr)),
        encoder_(std::exchange(other.encoder_, nullptr)),
        uncaught_exceptions_(other.uncaught_exceptions_) {}
    ~http_content_encoder_lease() {
        if (std::uncaught_exceptions() != uncaught_exceptions_) {
            discard();
        } else {
            release();
        }
    }

    [[nodiscard]] http_content_encoder& encoder() & noexcept RUVIA_LIFETIMEBOUND {
        return *encoder_;
    }
    http_content_encoder& encoder() && = delete;

    void discard() noexcept;

private:
    friend class http_content_codecs;

    http_content_encoder_lease(http_content_codecs& codecs, http_content_encoder& encoder) noexcept :
        codecs_(&codecs),
        encoder_(&encoder),
        uncaught_exceptions_(std::uncaught_exceptions()) {}

    void release() noexcept;

    http_content_codecs* codecs_{nullptr};
    http_content_encoder* encoder_{nullptr};
    int uncaught_exceptions_{0};
};

// Reusable single-threaded content encoder pool, for buffered and streaming
// responses alike. Not thread-safe.
class http_content_codecs final {
public:
    explicit http_content_codecs(http_content_encoder_options options = {}) noexcept :
        options_(options) {}
    ~http_content_codecs();
    http_content_codecs(const http_content_codecs&) = delete;
    http_content_codecs& operator=(const http_content_codecs&) = delete;

    // identity has no encoder and throws std::logic_error.
    [[nodiscard]] http_content_encoder_lease acquire_encoder(http_content_coding coding);

private:
    friend class http_content_encoder_lease;

    static constexpr std::size_t max_pooled_encoders = 8;

    struct encoder_pool_type final {
        std::array<http_content_encoder*, max_pooled_encoders> free_{};
        std::size_t count_{0};
    };

    [[nodiscard]] encoder_pool_type& pool(http_content_coding coding);
    void release_encoder(http_content_encoder& encoder) noexcept;
    static void destroy_encoder(http_content_encoder& encoder) noexcept;

    http_content_encoder_options options_{};
    encoder_pool_type gzip_;
    encoder_pool_type deflate_;
    encoder_pool_type brotli_;
    encoder_pool_type zstd_;
};

inline void http_content_encoder_lease::release() noexcept {
    if (encoder_ != nullptr) {
        auto* const codecs = std::exchange(codecs_, nullptr);
        codecs->release_encoder(*std::exchange(encoder_, nullptr));
    }
}

inline void http_content_encoder_lease::discard() noexcept {
    if (encoder_ != nullptr) {
        codecs_ = nullptr;
        http_content_codecs::destroy_encoder(*std::exchange(encoder_, nullptr));
    }
}

}  // namespace ruvia
