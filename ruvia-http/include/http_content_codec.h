#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "borrowed_text.h"
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

// Produces one complete content-coded representation within the exact output
// cap. nullopt means only that max_encoded_bytes_ would be exceeded; zero
// permits only a zero-byte encoding. Codec failures throw
// http_content_codec_error.
[[nodiscard]] std::optional<std::string> encode_http_content(http_content_coding coding, std::string_view input, http_content_encode_options options);

// Applies the listed codings in order. Each intermediate representation is
// bounded by max_encoded_bytes and released before the next stage completes.
[[nodiscard]] std::optional<std::string> encode_http_content(std::span<const http_content_coding> codings, std::string_view input, http_content_encode_options options);

// Reusable whole-buffer encoder for one coding, for an owner that encodes many
// representations on one thread. The first encode() creates the codec state;
// gzip, deflate and zstd keep it and reset it after every call that returns,
// and discard it after an exception, so no history and no failed state crosses
// representations. Brotli has no reset, so its state is created per call.
// Results follow encode_http_content() exactly: the same bytes, cap, and
// failures. The context is not thread-safe.
class http_content_encode_context final {
public:
    explicit http_content_encode_context(http_content_coding coding) noexcept;
    ~http_content_encode_context();
    http_content_encode_context(const http_content_encode_context&) = delete;
    http_content_encode_context& operator=(const http_content_encode_context&) = delete;
    http_content_encode_context(http_content_encode_context&&) = delete;
    http_content_encode_context& operator=(http_content_encode_context&&) = delete;

    [[nodiscard]] http_content_coding coding() const noexcept {
        return coding_;
    }

    [[nodiscard]] std::optional<std::string> encode(std::string_view input, http_content_encode_options options);

private:
    struct impl_type;
    http_content_coding coding_;
    impl_type* impl_{nullptr};
};

// Data errors only: a codec that cannot run throws http_content_codec_error.
enum class http_content_decode_error : std::uint8_t { unsupported_coding,
    invalid_content,
    decoded_size_exceeded };

struct http_content_decode_options final {
    std::size_t max_decoded_bytes_{};
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
struct http_content_encoder_options final {
    std::size_t max_encoded_bytes_{(std::numeric_limits<std::size_t>::max)()};
    std::size_t max_memory_bytes_{(std::numeric_limits<std::size_t>::max)()};
};

// Synchronous incremental representation encoder. Each write consumes all of
// its input and appends to caller-owned output; it does not retain either view.
// Input may borrow output. Compressed writes snapshot that input before an
// append can invalidate it.
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
    void reset();

private:
    struct impl_type;
    enum class phase : unsigned char { active,
        finished,
        failed };
    http_content_coding coding_;
    impl_type* impl_{nullptr};
    phase phase_{phase::active};
};

class http_worker_codecs;

// RAII lease for a pooled incremental encoder from http_worker_codecs.
// Destroying a non-discarded lease resets and returns the encoder to the pool;
// discard() destroys a failed encoder instead of pooling it.
class http_stream_encoder_lease final {
public:
    http_stream_encoder_lease(const http_stream_encoder_lease&) = delete;
    http_stream_encoder_lease& operator=(const http_stream_encoder_lease&) = delete;
    http_stream_encoder_lease(http_stream_encoder_lease&& other) noexcept :
        codecs_(std::exchange(other.codecs_, nullptr)),
        encoder_(std::exchange(other.encoder_, nullptr)) {}
    ~http_stream_encoder_lease() {
        release();
    }

    [[nodiscard]] http_content_encoder& encoder() & noexcept RUVIA_LIFETIMEBOUND {
        return *encoder_;
    }
    http_content_encoder& encoder() && = delete;

    void discard() noexcept;

private:
    friend class http_worker_codecs;

    http_stream_encoder_lease(http_worker_codecs& codecs, http_content_encoder& encoder) noexcept :
        codecs_(&codecs),
        encoder_(&encoder) {}

    void release() noexcept;

    http_worker_codecs* codecs_{nullptr};
    http_content_encoder* encoder_{nullptr};
};

// Per-worker reusable buffered and streaming content encoder pool.
class http_worker_codecs final {
public:
    http_worker_codecs() noexcept;
    ~http_worker_codecs();
    http_worker_codecs(const http_worker_codecs&) = delete;
    http_worker_codecs& operator=(const http_worker_codecs&) = delete;

    [[nodiscard]] http_content_encode_context& encoder(http_content_coding coding);
    [[nodiscard]] http_stream_encoder_lease acquire_stream_encoder(http_content_coding coding);

private:
    friend class http_stream_encoder_lease;

    static constexpr std::size_t max_pooled_stream_encoders = 8;

    struct stream_encoder_pool_type final {
        std::array<http_content_encoder*, max_pooled_stream_encoders> free_{};
        std::size_t count_{0};
    };

    struct coding_encoders_type final {
        explicit coding_encoders_type(http_content_coding coding) noexcept :
            buffered_(coding) {}

        http_content_encode_context buffered_;
        stream_encoder_pool_type streaming_;
    };

    [[nodiscard]] coding_encoders_type& encoders(http_content_coding coding);
    void release_stream_encoder(http_content_encoder& encoder) noexcept;
    void destroy_stream_encoder(http_content_encoder& encoder) noexcept;

    coding_encoders_type gzip_;
    coding_encoders_type deflate_;
    coding_encoders_type brotli_;
    coding_encoders_type zstd_;
};

inline void http_stream_encoder_lease::release() noexcept {
    if (encoder_ != nullptr) {
        auto* const codecs = std::exchange(codecs_, nullptr);
        codecs->release_stream_encoder(*std::exchange(encoder_, nullptr));
    }
}

inline void http_stream_encoder_lease::discard() noexcept {
    if (encoder_ != nullptr) {
        auto* const codecs = std::exchange(codecs_, nullptr);
        codecs->destroy_stream_encoder(*std::exchange(encoder_, nullptr));
    }
}

}  // namespace ruvia
