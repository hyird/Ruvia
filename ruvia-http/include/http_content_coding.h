#pragma once

#include <algorithm>
#include <array>
#include <chrono>
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
#include <vector>

#include "borrowed_text.h"
#include "http_header.h"
#include "http_status.h"

namespace ruvia {

class http_response_headers;
class http_request;

namespace detail {
struct http_content_coding_field_result_access;
}  // namespace detail

// Content codings supported by the complete-buffer codec facade. gzip is RFC
// 1952, br is RFC 7932, and zstd follows RFC 8878 with RFC 9659's HTTP window
// limit.
enum class http_content_coding : std::uint8_t {
    identity,
    gzip,
    deflate,
    brotli,
    zstd,
};

[[nodiscard]] std::string_view http_content_coding_token(http_content_coding coding) noexcept;

// The coding a trimmed token names, case-insensitively; x-gzip is gzip. Null
// for any other token, including an empty one.
[[nodiscard]] std::optional<http_content_coding> http_content_coding_from_token(std::string_view token) noexcept;

// Parsing is bounded to prevent attacker-controlled unbounded decode stages: a
// longer Content-Encoding stack is reported as unsupported.
inline constexpr std::size_t max_content_codings = 8;

// RFC 9110 section 8.4.1.3 treats x-gzip as gzip. The token is already trimmed.
[[nodiscard]] inline bool http_is_gzip_coding_token(std::string_view token) noexcept {
    return http_ascii_equals_ignore_case(token, "gzip") || http_ascii_equals_ignore_case(token, "x-gzip");
}

// Codings supported for incoming request bodies, as an Accept-Encoding field value.
[[nodiscard]] inline constexpr std::string_view http_supported_request_content_codings() noexcept {
    return "gzip, deflate, br, zstd";
}

class http_unsupported_content_coding final {
public:
    constexpr http_unsupported_content_coding() noexcept = default;
    explicit constexpr http_unsupported_content_coding(bool depth_exceeded) noexcept :
        depth_exceeded_(depth_exceeded) {}

    [[nodiscard]] static constexpr http_status_code status() noexcept {
        return http_status::unsupported_media_type;
    }

    [[nodiscard]] constexpr bool depth_exceeded() const noexcept {
        return depth_exceeded_;
    }

private:
    bool depth_exceeded_{false};
};

class http_invalid_content_coding_field final {
public:
    [[nodiscard]] static constexpr http_status_code status() noexcept {
        return http_status::bad_request;
    }
};

// A Content-Encoding field value is an ordered coding stack this library can
// decode, a syntactically valid stack containing an unsupported coding, or
// malformed field syntax. Codings are listed in the order they were applied.
class http_content_coding_field_result final {
public:
    [[nodiscard]] std::span<const http_content_coding> codings() const& noexcept RUVIA_LIFETIMEBOUND {
        if (const auto* codings = std::get_if<content_coding_sequence>(&value_)) {
            return std::span<const http_content_coding>(codings->values_.data(), codings->size_);
        }
        return {};
    }
    std::span<const http_content_coding> codings() const&& = delete;

    [[nodiscard]] const http_unsupported_content_coding* unsupported() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_unsupported_content_coding>(&value_);
    }
    const http_unsupported_content_coding* unsupported() const&& = delete;

    [[nodiscard]] const http_invalid_content_coding_field* invalid() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_invalid_content_coding_field>(&value_);
    }
    const http_invalid_content_coding_field* invalid() const&& = delete;

private:
    friend struct detail::http_content_coding_field_result_access;

    struct content_coding_sequence final {
        std::array<http_content_coding, max_content_codings> values_{};
        std::uint8_t size_{0};
    };

    explicit constexpr http_content_coding_field_result(content_coding_sequence codings) noexcept :
        value_(codings) {}

    explicit constexpr http_content_coding_field_result(http_unsupported_content_coding unsupported) noexcept :
        value_(unsupported) {}

    explicit constexpr http_content_coding_field_result(http_invalid_content_coding_field invalid) noexcept :
        value_(invalid) {}

    std::variant<content_coding_sequence, http_unsupported_content_coding, http_invalid_content_coding_field> value_;
};

// Parses one logical Content-Encoding field value using recipient list rules.
// Empty list members are ignored; an empty value therefore means identity.
[[nodiscard]] http_content_coding_field_result parse_http_content_coding(std::string_view value) noexcept;

// Folds every Content-Encoding header line into one recipient-side decision.
// Empty list members are ignored; no field or only empty members means identity.
[[nodiscard]] http_content_coding_field_result parse_http_content_coding_headers(std::span<const http_header> headers) noexcept;
[[nodiscard]] http_content_coding_field_result parse_http_content_coding_headers(const http_response_headers& headers) noexcept;

// Uses the parser's classified header descriptors without rescanning wire names.
// Admission keeps Content-Encoding opaque. Inspect invalid()/unsupported()
// before using codings(); an empty span in either error state is not identity.
[[nodiscard]] http_content_coding_field_result request_content_coding(const http_request& request) noexcept;

// Allowlist compressibility policy for dynamic response compression and static
// precompression: text/*, +json, +xml, and common text/font/wasm/icon media
// types, excluding any type naming a zip container.
[[nodiscard]] bool is_http_media_type_compressible(std::string_view content_type_value) noexcept;

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

class http_content_transcode_error final : public http_content_codec_error {
public:
    http_content_transcode_error(http_content_decode_error error, const char* message) :
        http_content_codec_error(message),
        error_(error) {}

    [[nodiscard]] constexpr http_content_decode_error decode_error() const noexcept {
        return error_;
    }

private:
    http_content_decode_error error_;
};

struct http_content_transcoder_options final {
    std::size_t max_decoded_bytes_{(std::numeric_limits<std::size_t>::max)()};
    std::size_t max_encoded_bytes_{(std::numeric_limits<std::size_t>::max)()};
    std::size_t max_memory_bytes_{(std::numeric_limits<std::size_t>::max)()};
};

// Streaming content transcoder (and multi-stage incremental decoder when
// target_coding is identity). Decodes source_codings in reverse wire order and
// encodes into target_coding.
class http_content_transcoder final {
public:
    http_content_transcoder(http_content_coding source_coding, http_content_coding target_coding, http_content_transcoder_options options = {});
    http_content_transcoder(std::span<const http_content_coding> source_codings, http_content_coding target_coding, http_content_transcoder_options options = {});
    ~http_content_transcoder();
    http_content_transcoder(const http_content_transcoder&) = delete;
    http_content_transcoder& operator=(const http_content_transcoder&) = delete;
    http_content_transcoder(http_content_transcoder&&) = delete;
    http_content_transcoder& operator=(http_content_transcoder&&) = delete;

    [[nodiscard]] http_content_coding target_coding() const noexcept {
        return target_coding_;
    }

    void write(std::string_view input, std::string& output, bool flush = false);
    void finish(std::string& output);
    void reset();

private:
    struct impl_type;
    enum class phase : unsigned char { active,
        finished,
        failed };
    http_content_coding target_coding_;
    impl_type* impl_{nullptr};
    phase phase_{phase::active};
};

struct http_staged_transcode_options final {
    std::size_t min_bytes_{0};
    std::size_t max_bytes_{std::size_t{64} * 1024 * 1024};
    std::size_t max_memory_bytes_{(std::numeric_limits<std::size_t>::max)()};
    bool require_smaller_than_source_{false};
};

class http_staged_transcode_result final {
public:
    http_staged_transcode_result(bool transformed, http_content_coding coding, std::size_t decoded_bytes, std::string bytes) noexcept :
        bytes_(std::move(bytes)),
        decoded_bytes_(decoded_bytes),
        coding_(coding),
        transformed_(transformed) {}

    http_staged_transcode_result(const http_staged_transcode_result&) = default;
    http_staged_transcode_result(http_staged_transcode_result&&) noexcept = default;
    http_staged_transcode_result& operator=(const http_staged_transcode_result&) = delete;
    http_staged_transcode_result& operator=(http_staged_transcode_result&&) = delete;

    [[nodiscard]] constexpr bool transformed() const noexcept {
        return transformed_;
    }
    [[nodiscard]] constexpr http_content_coding coding() const noexcept {
        return coding_;
    }
    [[nodiscard]] constexpr std::size_t decoded_bytes() const noexcept {
        return decoded_bytes_;
    }

    [[nodiscard]] std::string_view bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return bytes_;
    }
    std::string_view bytes() const&& = delete;

    [[nodiscard]] std::string take_bytes() && noexcept {
        return std::move(bytes_);
    }

private:
    std::string bytes_;
    std::size_t decoded_bytes_{0};
    http_content_coding coding_{http_content_coding::identity};
    bool transformed_{false};
};

// Size-range staging transcoder for responses without Content-Length (e.g.
// chunked or HTTP/2/3 streams). Stages raw and decoded bytes up to max_bytes_;
// if the final decoded length lies within [min_bytes_, max_bytes_], encodes
// into target_coding, otherwise falls back to the original untransformed stream.
// The two-argument write(input, passthrough_output) overload spills staged
// raw bytes directly into passthrough_output as soon as max_bytes_ is exceeded.
class http_staged_content_transcoder final {
public:
    http_staged_content_transcoder(http_content_coding source_coding, http_content_coding target_coding, http_staged_transcode_options options = {});
    http_staged_content_transcoder(std::span<const http_content_coding> source_codings, http_content_coding target_coding, http_staged_transcode_options options = {});
    ~http_staged_content_transcoder();
    http_staged_content_transcoder(const http_staged_content_transcoder&) = delete;
    http_staged_content_transcoder& operator=(const http_staged_content_transcoder&) = delete;
    http_staged_content_transcoder(http_staged_content_transcoder&&) = delete;
    http_staged_content_transcoder& operator=(http_staged_content_transcoder&&) = delete;

    [[nodiscard]] http_content_coding target_coding() const noexcept {
        return target_coding_;
    }
    [[nodiscard]] bool passthrough() const noexcept;
    [[nodiscard]] std::size_t staged_raw_bytes() const noexcept;
    [[nodiscard]] std::size_t decoded_bytes() const noexcept;

    // Returns true while still staging within max_bytes_, or false once
    // switched to passthrough fallback.
    bool write(std::string_view input);
    bool write(std::string_view input, std::string& passthrough_output);

    [[nodiscard]] http_staged_transcode_result finish();
    void reset();

private:
    struct impl_type;
    enum class phase : unsigned char { active,
        finished,
        failed };
    http_content_coding target_coding_;
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
