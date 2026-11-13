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

#include "ruvia/http/http_header.h"

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
    return http_ascii_equals_ignore_case(token, "gzip") ||
           http_ascii_equals_ignore_case(token, "x-gzip");
}

// Codings supported for incoming request bodies, as an Accept-Encoding field value.
[[nodiscard]] inline constexpr std::string_view http_supported_request_content_codings() noexcept {
    return "gzip, deflate, br, zstd";
}

class http_unsupported_content_coding final {
public:
    constexpr http_unsupported_content_coding() noexcept = default;
    explicit constexpr http_unsupported_content_coding(bool depth_exceeded) noexcept
        : depth_exceeded_(depth_exceeded) {}

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

    explicit constexpr http_content_coding_field_result(content_coding_sequence codings) noexcept
        : value_(codings) {}

    explicit constexpr http_content_coding_field_result(
        http_unsupported_content_coding unsupported) noexcept
        : value_(unsupported) {}

    explicit constexpr http_content_coding_field_result(http_invalid_content_coding_field invalid) noexcept
        : value_(invalid) {}

    std::variant<content_coding_sequence, http_unsupported_content_coding,
        http_invalid_content_coding_field>
        value_;
};

// Parses one logical Content-Encoding field value using recipient list rules.
// Empty list members are ignored; an empty value therefore means identity.
[[nodiscard]] http_content_coding_field_result parse_http_content_coding(
    std::string_view value) noexcept;

// Folds every Content-Encoding header line into one recipient-side decision.
// Empty list members are ignored; no field or only empty members means identity.
[[nodiscard]] http_content_coding_field_result parse_http_content_coding_headers(
    std::span<const http_header> headers) noexcept;
[[nodiscard]] http_content_coding_field_result parse_http_content_coding_headers(
    const http_response_headers& headers) noexcept;

// Uses the parser's classified header descriptors without rescanning wire names.
// Admission keeps Content-Encoding opaque. Inspect invalid()/unsupported()
// before using codings(); an empty span in either error state is not identity.
[[nodiscard]] http_content_coding_field_result request_content_coding(
    const http_request& request) noexcept;

// Allowlist compressibility policy for dynamic response compression and static
// precompression: text/*, +json, +xml, and common text/font/wasm/icon media
// types, excluding any type naming a zip container.
[[nodiscard]] bool is_http_media_type_compressible(std::string_view content_type_value) noexcept;
}  // namespace ruvia

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
[[nodiscard]] std::optional<std::string> encode_http_content(
    http_content_coding coding, std::string_view input, http_content_encode_options options);

// Applies the listed codings in order. Each intermediate representation is
// bounded by max_encoded_bytes and released before the next stage completes.
[[nodiscard]] std::optional<std::string> encode_http_content(
    std::span<const http_content_coding> codings, std::string_view input,
    http_content_encode_options options);

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

    [[nodiscard]] std::optional<std::string> encode(
        std::string_view input, http_content_encode_options options);

private:
    struct impl_type;
    http_content_coding coding_;
    impl_type* impl_{nullptr};
};

// Data errors only: a codec that cannot run throws http_content_codec_error.
enum class http_content_decode_error : std::uint8_t {
    unsupported_coding,
    invalid_content,
    decoded_size_exceeded
};

struct http_content_decode_options final {
    std::size_t max_decoded_bytes_{};
};

// gzip members and zstd frames are consumed through the end of input; Brotli
// trailing bytes are rejected. The exact decoded-size cap applies across the
// complete representation, and an identity representation over the cap is
// rejected before allocating. An empty representation decodes to empty content
// for every coding. Codec failures throw http_content_codec_error.
[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_http_content(
    http_content_coding coding, std::string_view input, http_content_decode_options options);

// Decodes the listed codings in reverse order. gzip members and zstd frames are
// consumed through the end of their input; Brotli trailing bytes are rejected.
// An empty coded representation decodes to empty content. The exact
// decoded-size cap applies to every intermediate representation, and each one
// is released before the next stage completes. Codec failures throw
// http_content_codec_error.
[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_http_content(
    std::span<const http_content_coding> codings, std::string_view input,
    http_content_decode_options options);
}  // namespace ruvia

namespace ruvia {

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
    explicit http_content_encoder(
        http_content_coding coding, http_content_encoder_options options = {});
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
    http_content_transcode_error(http_content_decode_error error, const char* message)
        : http_content_codec_error(message),
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
    http_content_transcoder(http_content_coding source_coding, http_content_coding target_coding,
        http_content_transcoder_options options = {});
    http_content_transcoder(std::span<const http_content_coding> source_codings,
        http_content_coding target_coding, http_content_transcoder_options options = {});
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
    http_staged_transcode_result(bool transformed, http_content_coding coding,
        std::size_t decoded_bytes, std::string bytes) noexcept
        : bytes_(std::move(bytes)),
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
    http_staged_content_transcoder(http_content_coding source_coding,
        http_content_coding target_coding, http_staged_transcode_options options = {});
    http_staged_content_transcoder(std::span<const http_content_coding> source_codings,
        http_content_coding target_coding, http_staged_transcode_options options = {});
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
    http_stream_encoder_lease(http_stream_encoder_lease&& other) noexcept
        : codecs_(std::exchange(other.codecs_, nullptr)),
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

    http_stream_encoder_lease(http_worker_codecs& codecs, http_content_encoder& encoder) noexcept
        : codecs_(&codecs),
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
        explicit coding_encoders_type(http_content_coding coding) noexcept
            : buffered_(coding) {}

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

namespace ruvia {

// Returns the media type before any parameters. The view borrows the input;
// owning string temporaries cannot be passed through borrowed_text.
[[nodiscard]] std::string_view http_media_type_only(borrowed_text value) noexcept;

// Validates one Content-Type field value, including RFC 9110's optional empty
// semicolon-delimited parameter slots. Nonempty parameters require name=value
// without whitespace around '='. A value with more than 64 nonempty parameters
// is invalid.
[[nodiscard]] bool is_valid_http_content_type_field_value(std::string_view value) noexcept;

enum class http_accept_token_match_mode : std::uint8_t {
    exact,
    language_prefix,
};

// Accumulates one offered representation's match across multiple Accept field
// lines, equivalent to a comma-joined field without allocating or copying it.
// More specific matches override less specific ones, including q=0 exclusions.
// Language-prefix mode uses the longest matching range; equal specificity takes
// the highest quality, independently of field order. Token fields allow only
// an optional q weight; unknown or additional parameters are unacceptable.
// Media ranges permit empty parameter slots; these do not affect specificity
// or quality. Nonempty parameters constrain the offered type, even after q.
// A media range, or an offered type, with more than 64 nonempty parameters
// (q included) is invalid and never matches.
class http_accept_match final {
public:
    void update_media_type(std::string_view field, std::string_view offered) noexcept;
    void update_token(std::string_view field, std::string_view offered,
        http_accept_token_match_mode mode) noexcept;

    [[nodiscard]] bool matched() const noexcept {
        return specificity_ >= 0 && quality_ > 0;
    }

    // 0 means no acceptable match. The value otherwise uses HTTP qvalue's
    // thousand-point scale (1 through 1000).
    [[nodiscard]] int quality() const noexcept {
        return matched() ? quality_ : 0;
    }

private:
    int specificity_{-1};
    int quality_{0};
    std::size_t parameter_specificity_{0};
};

}  // namespace ruvia

// Accept-Encoding negotiation (RFC 9110 section 12.5.3): the per-coding weights a
// request expresses, and the response coding the server picks from them.
// gzip and its deprecated x-gzip spelling share one case-insensitive weight.

namespace ruvia {

class http_request;

struct http_accepted_encoding_quality {
    int explicit_quality_{-1};
    int wildcard_quality_{-1};

    void update(std::string_view accept_encoding, std::string_view coding) noexcept;

    // HTTP qvalue on a thousand-point scale, with 0 for an unacceptable coding.
    // Explicit weights override the wildcard. Without an explicit identity
    // weight, identity defaults to 1000 unless a zero wildcard excludes it.
    [[nodiscard]] int quality(bool is_identity = false) const noexcept {
        if (explicit_quality_ >= 0) {
            return explicit_quality_;
        }
        if (is_identity) {
            return wildcard_quality_ == 0 ? 0 : 1000;
        }
        return wildcard_quality_ > 0 ? wildcard_quality_ : 0;
    }

    [[nodiscard]] bool accepts() const noexcept {
        return accepts(false);
    }

    [[nodiscard]] bool accepts(bool is_identity) const noexcept {
        return quality(is_identity) > 0;
    }
};

[[nodiscard]] bool http_accepts_encoding(
    std::string_view accept_encoding, std::string_view coding) noexcept;

struct http_response_coding_qualities final {
    // A missing field and an explicitly empty field have different RFC 9110
    // semantics: absence accepts any coding, while an empty value accepts only
    // the identity/no-encoding representation.
    bool field_present_{false};
    bool has_non_empty_item_{false};
    http_accepted_encoding_quality gzip_;
    http_accepted_encoding_quality deflate_;
    http_accepted_encoding_quality brotli_;
    http_accepted_encoding_quality zstd_;
    http_accepted_encoding_quality identity_;

    void update(std::string_view accept_encoding) noexcept;

    [[nodiscard]] bool accepts(http_content_coding coding) const noexcept {
        return score(coding) >= 0;
    }

private:
    friend class http_response_coding_selection;

    [[nodiscard]] int score(http_content_coding coding) const noexcept {
        const http_accepted_encoding_quality* coding_quality = nullptr;
        switch (coding) {
            case http_content_coding::identity:
                if (field_present_ && !has_non_empty_item_) {
                    return 1000;
                }
                return identity_.quality(true) > 0 ? identity_.quality(true) : -1;
            case http_content_coding::gzip:
                coding_quality = &gzip_;
                break;
            case http_content_coding::deflate:
                coding_quality = &deflate_;
                break;
            case http_content_coding::brotli:
                coding_quality = &brotli_;
                break;
            case http_content_coding::zstd:
                coding_quality = &zstd_;
                break;
        }
        if (coding_quality == nullptr) {
            return -1;
        }
        if (!field_present_) {
            return 999;
        }
        return coding_quality->quality() > 0 ? coding_quality->quality() : -1;
    }
};

// A representation policy supplies the codings it can actually produce or
// retrieve. Keeping this set typed prevents callers from reimplementing
// Accept-Encoding ranking with raw q-value integers.
class http_response_coding_candidates final {
public:
    [[nodiscard]] static constexpr http_response_coding_candidates empty() noexcept {
        return http_response_coding_candidates(0);
    }

    [[nodiscard]] static constexpr http_response_coding_candidates identity_only() noexcept {
        return http_response_coding_candidates(bit(http_content_coding::identity));
    }

    [[nodiscard]] static constexpr http_response_coding_candidates all() noexcept {
        return http_response_coding_candidates(
            bit(http_content_coding::identity) | bit(http_content_coding::gzip) |
            bit(http_content_coding::deflate) | bit(http_content_coding::brotli) |
            bit(http_content_coding::zstd));
    }

    constexpr http_response_coding_candidates& include(http_content_coding coding) noexcept {
        bits_ = static_cast<std::uint8_t>(bits_ | bit(coding));
        return *this;
    }

    [[nodiscard]] constexpr bool contains(http_content_coding coding) const noexcept {
        return (bits_ & bit(coding)) != 0;
    }

private:
    explicit constexpr http_response_coding_candidates(std::uint8_t bits) noexcept
        : bits_(bits) {}

    [[nodiscard]] static constexpr std::uint8_t bit(http_content_coding coding) noexcept {
        switch (coding) {
            case http_content_coding::identity:
                return 1u;
            case http_content_coding::gzip:
                return 2u;
            case http_content_coding::deflate:
                return 16u;
            case http_content_coding::brotli:
                return 4u;
            case http_content_coding::zstd:
                return 8u;
        }
        return 0u;
    }

    std::uint8_t bits_;
};

// The selected coding and the identity fallback decision come from the same
// Accept-Encoding snapshot. Keeping them together prevents a runtime from
// selecting one coding and separately observing a stale or differently parsed
// identity quality. A selection exists even when the client accepts none of
// the candidates: acceptable() then reports the failed negotiation, which the
// Web layer answers with 406 Not Acceptable once a response with content needs
// it, and the snapshot still answers accepts() for a coding a handler applied.
class http_response_coding_selection final {
public:
    [[nodiscard]] static http_response_coding_selection select(
        const http_response_coding_qualities& qualities) noexcept;
    [[nodiscard]] static http_response_coding_selection select(
        const http_response_coding_qualities& qualities,
        http_response_coding_candidates candidates) noexcept;
    [[nodiscard]] static constexpr http_response_coding_selection select(
        const http_response_coding_qualities& qualities,
        std::span<const http_content_coding> candidates) noexcept;

    // Ranks the same Accept-Encoding snapshot over the codings one
    // representation store actually holds (for example indexed precompressed
    // files), so that store never parses the request's fields again.
    [[nodiscard]] constexpr http_response_coding_selection select(
        http_response_coding_candidates candidates) const noexcept {
        return rank(scores_, candidates, explicit_identity_);
    }

    [[nodiscard]] constexpr http_response_coding_selection select(
        std::span<const http_content_coding> candidates) const noexcept {
        return rank_ordered(scores_, candidates, explicit_identity_);
    }
    // False when the client accepts none of the candidates.
    [[nodiscard]] constexpr bool acceptable() const noexcept {
        return acceptable_;
    }

    // The selected coding; identity, as a placeholder only, when !acceptable().
    [[nodiscard]] constexpr http_content_coding coding() const noexcept {
        return coding_;
    }

    [[nodiscard]] constexpr bool identity_accepted() const noexcept {
        return accepts(http_content_coding::identity);
    }

    // The selected coding is the server's preference, while this predicate
    // retains the complete client acceptability snapshot for a response that
    // was already encoded by application code or a representation store. A
    // missing Accept-Encoding field accepts every coding; an explicitly
    // present field uses the parsed q-value set, including wildcard rules.
    [[nodiscard]] constexpr bool accepts(http_content_coding coding) const noexcept {
        const auto index = static_cast<std::size_t>(coding);
        return index < scores_.size() && scores_[index] >= 0;
    }

    // Extension tokens are evaluated from a borrowed request without retaining
    // it or allocating extension storage in every negotiation snapshot.
    [[nodiscard]] bool accepts(std::string_view coding, const http_request& request) const noexcept;

private:
    // Client score per http_content_coding value; -1 marks an unacceptable coding.
    using scores_type = std::array<std::int16_t, 5>;

    constexpr http_response_coding_selection(http_content_coding coding, bool acceptable,
        scores_type scores, bool explicit_identity = false) noexcept
        : scores_(scores),
          coding_(coding),
          acceptable_(acceptable),
          explicit_identity_(explicit_identity) {}

    [[nodiscard]] static constexpr scores_type make_scores(
        const http_response_coding_qualities& qualities) noexcept {
        scores_type scores{};
        for (std::size_t index = 0; index < scores.size(); ++index) {
            scores[index] =
                static_cast<std::int16_t>(qualities.score(static_cast<http_content_coding>(index)));
        }
        return scores;
    }

    [[nodiscard]] static constexpr http_response_coding_selection rank(
        scores_type scores, http_response_coding_candidates candidates,
        bool explicit_identity = false) noexcept;

    [[nodiscard]] static constexpr http_response_coding_selection rank_ordered(
        scores_type scores, std::span<const http_content_coding> candidates,
        bool explicit_identity) noexcept;

    scores_type scores_;
    http_content_coding coding_;
    bool acceptable_;
    bool explicit_identity_{false};
};

// Picks the best response coding from the supplied representation candidates
// (every coding the server can produce for the one-argument form). The highest
// client q-value wins; ties resolve by server preference br > zstd > gzip >
// deflate > identity. A coding with q=0 or one the client never accepts is
// excluded.
inline http_response_coding_selection http_response_coding_selection::select(
    const http_response_coding_qualities& qualities) noexcept {
    return select(qualities, http_response_coding_candidates::all());
}

inline http_response_coding_selection http_response_coding_selection::select(
    const http_response_coding_qualities& qualities,
    http_response_coding_candidates candidates) noexcept {
    return rank(make_scores(qualities), candidates, qualities.identity_.explicit_quality_ >= 0);
}

constexpr http_response_coding_selection http_response_coding_selection::select(
    const http_response_coding_qualities& qualities,
    std::span<const http_content_coding> candidates) noexcept {
    return rank_ordered(
        make_scores(qualities), candidates, qualities.identity_.explicit_quality_ >= 0);
}

constexpr http_response_coding_selection http_response_coding_selection::rank(
    scores_type scores, http_response_coding_candidates candidates, bool explicit_identity) noexcept {
    constexpr http_content_coding preference[] = {
        http_content_coding::brotli,
        http_content_coding::zstd,
        http_content_coding::gzip,
        http_content_coding::deflate,
        http_content_coding::identity,
    };
    http_content_coding best = http_content_coding::identity;
    bool found = false;
    int best_score = -1;
    for (const auto coding : preference) {
        if (!candidates.contains(coding)) {
            continue;
        }
        const int score = scores[static_cast<std::size_t>(coding)];
        if (score > best_score) {
            best_score = score;
            best = coding;
            found = true;
        }
    }
    return http_response_coding_selection(best, found, scores, explicit_identity);
}

constexpr http_response_coding_selection http_response_coding_selection::rank_ordered(
    scores_type scores, std::span<const http_content_coding> candidates,
    bool explicit_identity) noexcept {
    http_content_coding best = http_content_coding::identity;
    bool found = false;
    int best_score = -1;
    bool identity_in_candidates = false;
    for (const auto coding : candidates) {
        const auto index = static_cast<std::size_t>(coding);
        if (index >= scores.size()) {
            continue;
        }
        if (coding == http_content_coding::identity) {
            identity_in_candidates = true;
        }
        const int raw_score = scores[index];
        if (raw_score < 0) {
            continue;
        }
        const int score =
            (coding == http_content_coding::identity && !explicit_identity) ? 0 : raw_score;
        if (score > best_score) {
            best_score = score;
            best = coding;
            found = true;
        }
    }
    const int identity_score =
        scores[static_cast<std::size_t>(http_content_coding::identity)];
    if (!identity_in_candidates && identity_score >= 0) {
        if (!found || (explicit_identity && identity_score > best_score)) {
            best = http_content_coding::identity;
            best_score = identity_score;
            found = true;
        }
    }
    return http_response_coding_selection(best, found, scores, explicit_identity);
}

class http_response;

// Custom-coding queries borrow the request only during the call; negotiation
// snapshots never allocate storage for unknown extension tokens.
[[nodiscard]] bool http_accepts_encoding(
    const http_request& request, std::string_view coding) noexcept;

enum class http_response_content_coding_status : std::uint8_t {
    absent,
    accepted,
    not_acceptable,
    invalid,
};

// Parses all Content-Encoding lines once and evaluates the required custom
// codings together in one Accept-Encoding walk. Only stacks with more than
// eight distinct extension codings require temporary storage.
[[nodiscard]] http_response_content_coding_status evaluate_http_response_content_coding(
    const http_response_coding_selection& selection, const http_response& response,
    const http_request* request = nullptr);

// Folds every Accept-Encoding field line of a received request into one
// quality snapshot and selects among all producible codings. This is the one
// Accept-Encoding preference authority for HTTP/1, HTTP/2 and subrequests.
[[nodiscard]] http_response_coding_selection http_response_coding_selection_for(const http_request& request) noexcept;

// Selects a content coding from Accept-Encoding using the caller's ordered
// candidate list for tie-breaking, comparing explicit/implicit identity weight
// and returning std::nullopt when every candidate (and identity) is rejected.
[[nodiscard]] http_response_coding_selection select_http_accept_encoding(
    std::string_view accept_encoding, std::span<const http_content_coding> candidates) noexcept;
[[nodiscard]] http_response_coding_selection select_http_accept_encoding(
    std::span<const http_header> request_headers,
    std::span<const http_content_coding> candidates) noexcept;
[[nodiscard]] http_response_coding_selection select_http_accept_encoding(
    std::span<const http_header_view> request_headers,
    std::span<const http_content_coding> candidates) noexcept;
[[nodiscard]] http_response_coding_selection select_http_accept_encoding(
    const http_request& request, std::span<const http_content_coding> candidates) noexcept;

[[nodiscard]] std::optional<http_content_coding> negotiate_http_content_coding(
    std::string_view accept_encoding, std::span<const http_content_coding> candidates) noexcept;
[[nodiscard]] std::optional<http_content_coding> negotiate_http_content_coding(
    std::span<const http_header> request_headers,
    std::span<const http_content_coding> candidates) noexcept;
[[nodiscard]] std::optional<http_content_coding> negotiate_http_content_coding(
    std::span<const http_header_view> request_headers,
    std::span<const http_content_coding> candidates) noexcept;
[[nodiscard]] std::optional<http_content_coding> negotiate_http_content_coding(
    const http_request& request, std::span<const http_content_coding> candidates) noexcept;

}  // namespace ruvia

namespace ruvia {

namespace detail {
struct multipart_part_access;
struct multipart_stream_part_access;
}  // namespace detail

// Buffered multipart parsing borrows unmodified Content-Disposition names,
// filenames, Content-Type values, and part bodies from the caller-owned
// request body; unfolded header blocks and escaped names own string storage.
class multipart_part final {
public:
    multipart_part(const multipart_part&) = default;
    multipart_part(multipart_part&&) noexcept = default;
    multipart_part& operator=(const multipart_part&) = default;
    multipart_part& operator=(multipart_part&&) noexcept = default;

    [[nodiscard]] std::string_view name() const& noexcept RUVIA_LIFETIMEBOUND {
        if (const auto* borrowed = std::get_if<std::string_view>(&name_)) {
            return *borrowed;
        }
        return std::get<std::string>(name_);
    }
    [[nodiscard]] std::string_view name() const&& = delete;

    [[nodiscard]] std::string_view filename() const& noexcept RUVIA_LIFETIMEBOUND {
        if (const auto* borrowed = std::get_if<std::string_view>(&filename_)) {
            return *borrowed;
        }
        return std::get<std::string>(filename_);
    }
    [[nodiscard]] std::string_view filename() const&& = delete;

    [[nodiscard]] bool has_filename() const noexcept {
        return filename_present_;
    }
    [[nodiscard]] std::string_view content_type() const& noexcept RUVIA_LIFETIMEBOUND {
        if (const auto* borrowed = std::get_if<std::string_view>(&content_type_)) {
            return *borrowed;
        }
        return std::get<std::string>(content_type_);
    }
    std::string_view content_type() const&& = delete;

    [[nodiscard]] std::string_view body() const noexcept {
        return body_;
    }

private:
    friend struct detail::multipart_part_access;
    friend class multipart_parser;

    using text_storage_type = std::variant<std::string_view, std::string>;
    using content_type_storage_type = text_storage_type;

    multipart_part(text_storage_type name, text_storage_type filename,
        content_type_storage_type content_type_value, std::string_view body,
        bool filename_present) noexcept
        : name_(std::move(name)),
          filename_(std::move(filename)),
          content_type_(std::move(content_type_value)),
          body_(body),
          filename_present_(filename_present) {}
    text_storage_type name_;
    text_storage_type filename_;
    content_type_storage_type content_type_;
    std::string_view body_;
    bool filename_present_{false};
};

// RFC 2046 multipart boundary value. The validated bytes are stored inline so
// every buffered/streaming parser consumes the same allocation-free value and
// cannot observe a dangling Content-Type substring.
class multipart_boundary final {
public:
    explicit multipart_boundary(std::string_view value) {
        if (!valid(value)) {
            throw std::invalid_argument("invalid multipart boundary");
        }
        assign(value);
    }

    // Validation-only entry for parsing paths that must report failure as a
    // value rather than drive control flow through the throwing constructor.
    [[nodiscard]] static std::optional<multipart_boundary> try_create(
        std::string_view value) noexcept {
        if (!valid(value)) {
            return std::nullopt;
        }
        multipart_boundary boundary;
        boundary.assign(value);
        return boundary;
    }

    [[nodiscard]] constexpr std::string_view value() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::string_view(bytes_.data(), size_);
    }
    [[nodiscard]] std::string_view value() const&& = delete;

private:
    constexpr multipart_boundary() noexcept = default;

    static constexpr std::size_t max_size = 70;

    [[nodiscard]] static constexpr bool non_space_char(char value) noexcept {
        return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
               (value >= 'a' && value <= 'z') || value == '\'' || value == '(' || value == ')' ||
               value == '+' || value == '_' || value == ',' || value == '-' || value == '.' ||
               value == '/' || value == ':' || value == '=' || value == '?';
    }

    [[nodiscard]] static constexpr bool valid(std::string_view value) noexcept {
        if (value.empty() || value.size() > max_size || value.back() == ' ') {
            return false;
        }
        return std::ranges::all_of(
            value, [](char byte) noexcept { return byte == ' ' || non_space_char(byte); });
    }

    constexpr void assign(std::string_view value) noexcept {
        for (std::size_t index = 0; index < value.size(); ++index) {
            bytes_[index] = value[index];
        }
        size_ = static_cast<std::uint8_t>(value.size());
    }

    std::array<char, max_size> bytes_{};
    std::uint8_t size_{0};
};

struct multipart_parse_options final {
    multipart_boundary boundary_;
    std::size_t max_parts_{1024};
    // Cumulative wire header bytes bound decoded names, filenames and metadata.
    std::size_t max_metadata_bytes_{1024 * 1024};
};

class multipart_boundary_not_applicable final {
private:
    friend class multipart_boundary_parse_result;

    constexpr multipart_boundary_not_applicable() noexcept = default;
};

class multipart_boundary_parse_failure final {
public:
    [[nodiscard]] http_protocol_error protocol_error() const noexcept {
        return http_protocol_error(
            http_status::bad_request, "multipart_invalid_boundary", "invalid multipart boundary");
    }

private:
    friend class multipart_boundary_parse_result;

    constexpr multipart_boundary_parse_failure() noexcept = default;
};

// Content-Type boundary extraction distinguishes another media type from a
// malformed multipart/form-data declaration. The successful alternative owns
// its decoded boundary bytes.
class multipart_boundary_parse_result final {
public:
    [[nodiscard]] constexpr const multipart_boundary* boundary() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<multipart_boundary>(&value_);
    }
    const multipart_boundary* boundary() const&& = delete;

    [[nodiscard]] constexpr const multipart_boundary_not_applicable* not_applicable() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<multipart_boundary_not_applicable>(&value_);
    }
    const multipart_boundary_not_applicable* not_applicable() const&& = delete;

    [[nodiscard]] constexpr const multipart_boundary_parse_failure* failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<multipart_boundary_parse_failure>(&value_);
    }
    const multipart_boundary_parse_failure* failure() const&& = delete;

private:
    friend multipart_boundary_parse_result parse_multipart_boundary(std::string_view);

    using value_type = std::variant<multipart_boundary, multipart_boundary_not_applicable,
        multipart_boundary_parse_failure>;

    explicit multipart_boundary_parse_result(multipart_boundary boundary)
        : value_(boundary) {}

    [[nodiscard]] static constexpr multipart_boundary_parse_result make_not_applicable() noexcept {
        return multipart_boundary_parse_result(multipart_boundary_not_applicable());
    }

    [[nodiscard]] static constexpr multipart_boundary_parse_result make_failure() noexcept {
        return multipart_boundary_parse_result(multipart_boundary_parse_failure());
    }

    explicit constexpr multipart_boundary_parse_result(
        multipart_boundary_not_applicable not_applicable) noexcept
        : value_(not_applicable) {}

    explicit constexpr multipart_boundary_parse_result(multipart_boundary_parse_failure failure) noexcept
        : value_(failure) {}

    value_type value_;
};

// Extracts the RFC 2046 boundary parameter from multipart/form-data. MIME
// quoted-pairs are decoded into the owned multipart_boundary value. Empty HTTP
// parameter slots are ignored; a nonempty, unique boundary remains required.
[[nodiscard]] multipart_boundary_parse_result parse_multipart_boundary(std::string_view content_type_value);

enum class multipart_chunk_phase : std::uint8_t {
    complete,
    first,
    middle,
    last,
};

class multipart_stream_part final {
public:
    [[nodiscard]] std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] std::string_view filename() const noexcept {
        return filename_;
    }

    [[nodiscard]] constexpr bool has_filename() const noexcept {
        return filename_present_;
    }

    [[nodiscard]] std::string_view content_type() const noexcept {
        return content_type_;
    }

    [[nodiscard]] std::string_view body() const noexcept {
        return body_;
    }

    [[nodiscard]] constexpr multipart_chunk_phase phase() const noexcept {
        return phase_;
    }

private:
    friend struct detail::multipart_stream_part_access;

    constexpr multipart_stream_part(std::string_view name, std::string_view filename,
        std::string_view content_type_value, std::string_view body, multipart_chunk_phase phase,
        bool filename_present) noexcept
        : name_(name),
          filename_(filename),
          content_type_(content_type_value),
          body_(body),
          phase_(phase),
          filename_present_(filename_present) {}

    std::string_view name_;
    std::string_view filename_;
    std::string_view content_type_;
    std::string_view body_;
    multipart_chunk_phase phase_{multipart_chunk_phase::middle};
    bool filename_present_{false};
};

class multipart_poll_need_input final {
private:
    friend class multipart_poll_result;
    constexpr multipart_poll_need_input() noexcept = default;
};

class multipart_poll_done final {
private:
    friend class multipart_poll_result;
    constexpr multipart_poll_done() noexcept = default;
};

enum class multipart_parse_error : std::uint8_t {
    incomplete_body,
    invalid_delimiter,
    preamble_too_large,
    part_headers_too_large,
    invalid_part_headers,
    invalid_content_disposition,
    missing_field_name,
    delimiter_line_too_large,
    too_many_parts,
    metadata_too_large,
};

class multipart_poll_failure final {
public:
    [[nodiscard]] http_protocol_error protocol_error() const noexcept;

private:
    friend class multipart_poll_result;

    explicit constexpr multipart_poll_failure(multipart_parse_error error) noexcept
        : error_(error) {}

    multipart_parse_error error_;
};

// Incremental multipart parsing has four mutually exclusive outcomes. Only
// the part alternative exposes borrowed part metadata/body, while only a
// failure exposes a protocol error. Need-input and done are payload-free
// lifecycle signals. All borrowed views remain valid only until the next
// feed(), finish_input(), or poll() call.
class multipart_poll_result final {
public:
    [[nodiscard]] constexpr const multipart_poll_need_input* need_input() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<multipart_poll_need_input>(&value_);
    }
    const multipart_poll_need_input* need_input() const&& = delete;

    [[nodiscard]] constexpr const multipart_stream_part* part() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<multipart_stream_part>(&value_);
    }
    const multipart_stream_part* part() const&& = delete;

    [[nodiscard]] constexpr const multipart_poll_done* done() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<multipart_poll_done>(&value_);
    }
    const multipart_poll_done* done() const&& = delete;

    [[nodiscard]] constexpr const multipart_poll_failure* failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<multipart_poll_failure>(&value_);
    }
    const multipart_poll_failure* failure() const&& = delete;

private:
    friend class multipart_parser;

    using value_type = std::variant<multipart_poll_need_input, multipart_stream_part, multipart_poll_done,
        multipart_poll_failure>;

    explicit constexpr multipart_poll_result(multipart_poll_need_input value) noexcept
        : value_(value) {}

    explicit constexpr multipart_poll_result(multipart_stream_part value) noexcept
        : value_(value) {}

    explicit constexpr multipart_poll_result(multipart_poll_done value) noexcept
        : value_(value) {}

    explicit constexpr multipart_poll_result(multipart_poll_failure value) noexcept
        : value_(value) {}

    [[nodiscard]] static constexpr multipart_poll_result make_need_input() noexcept {
        return multipart_poll_result(multipart_poll_need_input());
    }

    [[nodiscard]] static constexpr multipart_poll_result make_part(multipart_stream_part part) noexcept {
        return multipart_poll_result(part);
    }

    [[nodiscard]] static constexpr multipart_poll_result make_done() noexcept {
        return multipart_poll_result(multipart_poll_done());
    }

    [[nodiscard]] static constexpr multipart_poll_result make_failure(
        multipart_parse_error error) noexcept {
        return multipart_poll_result(multipart_poll_failure(error));
    }

    value_type value_;
};

class multipart_parser final {
public:
    explicit multipart_parser(multipart_parse_options options);

    multipart_parser(const multipart_parser&) = delete;
    multipart_parser& operator=(const multipart_parser&) = delete;
    multipart_parser(multipart_parser&&) = delete;
    multipart_parser& operator=(multipart_parser&&) = delete;

    // Copies input into parser-owned buffer storage, including chunks borrowed from
    // the latest part body. Allocation failure leaves that body valid for retry.
    // finish_input() marks the enclosing HTTP body's end so a close delimiter
    // ending exactly at EOF can be distinguished from a line split across chunks.
    // A protocol failure is terminal: later poll() calls repeat the exact error
    // and feed() rejects further bytes.
    void feed(std::string_view chunk);
    void finish_input() noexcept;

    // If owning part metadata throws, the part stays pending without consuming
    // its limits. poll() may be retried after allocations become available.
    [[nodiscard]] multipart_poll_result poll();

private:
    struct complete_input_tag_type final {};

    friend std::vector<multipart_part> parse_multipart_body(std::string_view, multipart_parse_options);

    multipart_parser(std::string_view complete_body, multipart_parse_options options, complete_input_tag_type);

    enum class progress_state_type : std::uint8_t { boundary,
        headers,
        body,
        done };

    using state_type = std::variant<progress_state_type, multipart_parse_error>;

    enum class step_progress_type : std::uint8_t {
        need_input,
        continue_value,
        done,
    };

    using step_result_type = std::variant<step_progress_type, multipart_parse_error>;

    [[nodiscard]] std::string_view buffer_view() const noexcept;
    void consume(std::size_t bytes) noexcept;
    void reset_scan_cursors() noexcept;
    void compact_pending();
    [[nodiscard]] multipart_poll_result fail(multipart_parse_error error) noexcept;
    [[nodiscard]] step_result_type process_boundary();
    [[nodiscard]] step_result_type process_headers();
    [[nodiscard]] multipart_stream_part make_part(std::string_view body, bool part_end);
    [[nodiscard]] multipart_poll_result read_body_chunk();

    multipart_boundary boundary_;
    std::variant<std::string_view, http_io_buffer> input_;
    multipart_part::text_storage_type current_name_;
    multipart_part::text_storage_type current_filename_;
    multipart_part::text_storage_type current_content_type_;
    std::string current_headers_;
    state_type state_{progress_state_type::boundary};
    std::size_t pending_erase_bytes_{0};
    std::size_t remaining_parts_;
    std::size_t remaining_metadata_bytes_;
    std::size_t header_scan_offset_{0};
    std::size_t delimiter_scan_offset_{0};
    std::size_t delimiter_padding_offset_{0};
    bool input_eof_{false};
    bool next_chunk_is_first_{false};
    bool first_boundary_{true};
    bool has_current_filename_{false};
};

// Parses a complete multipart/form-data body without I/O. Returned part bodies
// borrow `body`. Names, filenames, and Content-Type values from unfolded header
// blocks own string storage; unmodified header blocks keep borrowing `body`.
[[nodiscard]] std::vector<multipart_part> parse_multipart_body(
    std::string_view body, multipart_parse_options options);

template <detail::http_temporary_owning_char_string body_type>
std::vector<multipart_part> parse_multipart_body(body_type&&, multipart_parse_options) = delete;

}  // namespace ruvia

namespace ruvia {

struct sse_message final {
    // SSE messages may be retained before they are formatted. Keep their text
    // zero-copy while preventing a temporary owning string from leaving an
    // already-dangling view in the saved message.
    // Absence emits no data field, so an event/id/retry-only block does not
    // dispatch a MessageEvent. A present empty value emits `data:` and therefore
    // dispatches one event whose data is empty.
    std::optional<::ruvia::borrowed_text> data_{};
    ::ruvia::borrowed_text event_{};
    std::optional<::ruvia::borrowed_text> id_{};
    std::optional<std::chrono::milliseconds> retry_{};
};

// Build one complete UTF-8 event-stream block. Invalid event/id line syntax
// or a field that is not well-formed UTF-8 throws std::invalid_argument.
[[nodiscard]] std::string format_sse_message(const sse_message& message);

struct sse_parser_options final {
    // Bytes buffered for the event being assembled: its data lines with their
    // LFs, its event type, and a pending event or id field value. Comments and
    // ignored fields buffer nothing. Exceeding it fails the stream.
    protocol_byte_limit event_limit_{protocol_byte_limit::limited(default_max_sse_event_bytes)};
};

enum class sse_parser_state : std::uint8_t {
    active,
    // finish() ended the stream.
    finished,
    // An event exceeded sse_parser_options::event_limit_.
    failed,
};

// One dispatched event (WHATWG HTML, "Dispatch the event"). Its views borrow
// the parser and remain valid until the parser's next parse(), finish(),
// reset(), move or destruction. Every view is well-formed UTF-8.
class sse_event final {
public:
    // The event type; "message" when the event named none.
    [[nodiscard]] constexpr std::string_view type() const noexcept {
        return type_;
    }
    // The data lines joined by LF, without a trailing LF.
    [[nodiscard]] constexpr std::string_view data() const noexcept {
        return data_;
    }
    [[nodiscard]] constexpr std::string_view last_event_id() const noexcept {
        return last_event_id_;
    }

private:
    friend class sse_parser;

    constexpr sse_event(std::string_view type, std::string_view data, std::string_view last_event_id) noexcept
        : type_(type),
          data_(data),
          last_event_id_(last_event_id) {}

    std::string_view type_;
    std::string_view data_;
    std::string_view last_event_id_;
};

struct sse_parse_result final {
    // Input bytes the parser consumed: through the blank line that completed
    // event_, up to the field bytes that failed the stream, or all of it.
    std::size_t consumed_bytes_{0};
    std::optional<sse_event> event_{};
};

// Sans-I/O text/event-stream parser (WHATWG HTML, "Parsing an event stream")
// for one response body, fed in chunks of any size. It skips one leading
// UTF-8 BOM, ends lines at CRLF, LF or CR (a CRLF split across chunks
// included), replaces ill-formed UTF-8 with U+FFFD in event values, and
// handles the event, data, id and retry fields; comments and other fields are
// ignored. Each input byte is scanned once: an incomplete line keeps only its
// field's routed value, never the raw line. The end of the stream discards an
// incomplete line or event. Only reset() leaves the finished or failed state;
// it starts a new stream and keeps buffer capacity within a small bound.
class sse_parser final {
public:
    explicit sse_parser(sse_parser_options options = {});
    sse_parser(const sse_parser&) = delete;
    sse_parser& operator=(const sse_parser&) = delete;
    sse_parser(sse_parser&&) noexcept = default;
    sse_parser& operator=(sse_parser&&) = delete;
    ~sse_parser() = default;

    // Consumes input until the next event is complete, the stream fails, or
    // the input ends. Call it again with the unconsumed suffix. A parser that
    // is not active throws std::logic_error.
    [[nodiscard]] sse_parse_result parse(std::string_view input) & RUVIA_LIFETIMEBOUND;
    sse_parse_result parse(std::string_view input) && = delete;
    // End of the stream: an active parser discards any incomplete event and
    // becomes finished. No effect otherwise.
    void finish() noexcept;
    void reset() noexcept;

    [[nodiscard]] sse_parser_state state() const noexcept {
        return state_;
    }
    // The stream's last event ID string, set by each blank line from the id
    // fields received so far; the value of a reconnection's Last-Event-ID.
    [[nodiscard]] std::string_view last_event_id() const& noexcept RUVIA_LIFETIMEBOUND {
        return last_event_id_;
    }
    std::string_view last_event_id() const&& = delete;
    // The latest valid retry field; absent until one arrives.
    [[nodiscard]] std::optional<std::chrono::milliseconds> reconnection_time() const noexcept {
        return reconnection_time_;
    }

private:
    enum class line_phase : std::uint8_t {
        name,
        // After the colon: one leading space is still to be skipped.
        value_start,
        value,
        comment,
    };
    enum class field_kind : std::uint8_t {
        ignored,
        data,
        event,
        id,
        retry,
    };

    [[nodiscard]] std::size_t buffered_bytes() const noexcept;
    void append_name(std::string_view bytes) noexcept;
    void classify_field() noexcept;
    // False when the bytes would exceed the event limit.
    [[nodiscard]] bool append_value(std::string_view bytes);
    [[nodiscard]] bool end_line();
    // True when the blank line dispatches an event.
    [[nodiscard]] bool dispatch();
    void reset_line() noexcept;
    void release_event_buffers() noexcept;
    void fail() noexcept;

    std::string data_;
    std::string event_type_;
    // A pending event or id value, committed at its line end.
    std::string value_;
    std::string last_event_id_buffer_;
    std::string last_event_id_;
    std::optional<std::chrono::milliseconds> reconnection_time_{};
    protocol_byte_limit event_limit_;
    std::uint64_t retry_value_{0};
    // Field name bytes, kept only up to the longest field name; longer names
    // name no field.
    std::array<char, 5> name_{};
    std::uint8_t name_size_{0};
    bool name_overflow_{false};
    bool retry_digits_{false};
    bool retry_valid_{true};
    // The views of the last returned event still borrow data_ and event_type_.
    bool event_published_{false};
    // The previous input ended in CR: an LF opening the next one belongs to it.
    bool skip_line_feed_{false};
    // Leading BOM bytes matched so far while the stream start is unsettled.
    std::uint8_t bom_matched_{0};
    bool bom_settled_{false};
    line_phase phase_{line_phase::name};
    field_kind field_{field_kind::ignored};
    sse_parser_state state_{sse_parser_state::active};
};

}  // namespace ruvia
