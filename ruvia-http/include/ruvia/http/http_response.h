#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
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

#include "ruvia/http/http_request.h"

namespace ruvia {

class http_response_headers;

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

// Accept-Encoding negotiation (RFC 9110 section 12.5.3): the per-coding weights a
// request expresses, and the response coding the server picks from them.
// gzip and its deprecated x-gzip spelling share one case-insensitive weight.

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

// Platform-native path text (wchar_t on Windows, char elsewhere), as carried
// by response file bodies.
using http_native_path_char_type = std::filesystem::path::value_type;
using http_native_path_string_type = std::basic_string<http_native_path_char_type>;
using http_native_path_view_type = std::basic_string_view<http_native_path_char_type>;

[[nodiscard]] inline http_native_path_view_type http_native_path_view(const std::filesystem::path& path) noexcept {
    return http_native_path_view_type(path.native());
}
http_native_path_view_type http_native_path_view(const std::filesystem::path&&) = delete;

inline void http_assign_native_path(http_native_path_string_type& output, const std::filesystem::path& path) {
    const auto native = http_native_path_view(path);
    output.assign(native.data(), native.size());
}

[[nodiscard]] inline std::filesystem::path http_make_path_from_native_path(const http_native_path_char_type* path) {
    return path == nullptr ? std::filesystem::path{} : std::filesystem::path(path);
}

[[nodiscard]] inline std::filesystem::path http_make_path_from_native_path(const http_native_path_string_type& path) {
    return http_make_path_from_native_path(path.c_str());
}

// Opaque runtime-supplied identity of the file whose metadata was used to
// frame a response. HTTP transports this token; the runtime validates it after
// opening the actual file handle.
class http_response_file_identity final {
public:
    [[nodiscard]] static constexpr http_response_file_identity unchecked() noexcept {
        return http_response_file_identity({}, false);
    }

    [[nodiscard]] static constexpr http_response_file_identity checked(
        std::array<std::uint64_t, 4> words) noexcept {
        return http_response_file_identity(words, true);
    }

    [[nodiscard]] constexpr bool requires_validation() const noexcept {
        return checked_;
    }
    [[nodiscard]] constexpr const std::array<std::uint64_t, 4>& words() const& noexcept {
        return words_;
    }
    [[nodiscard]] constexpr const std::array<std::uint64_t, 4>& words() const&& = delete;

    friend constexpr bool operator==(
        const http_response_file_identity&, const http_response_file_identity&) noexcept = default;

private:
    constexpr http_response_file_identity(std::array<std::uint64_t, 4> words, bool checked) noexcept
        : words_(words),
          checked_(checked) {}

    std::array<std::uint64_t, 4> words_{};
    bool checked_{false};
};

// An open native file a runtime already framed a response from, carried by
// the response body so the transport sends from that same descriptor instead of
// resolving the path again. HTTP performs no I/O on it: it stores the native
// value and invokes the runtime-supplied release function exactly once when
// the owning body is destroyed or replaced.
class http_response_file_descriptor final {
public:
    using native_type = std::uintptr_t;
    using release_type = void (*)(native_type) noexcept;

    constexpr http_response_file_descriptor() noexcept = default;
    constexpr http_response_file_descriptor(native_type native_value, release_type release) noexcept
        : native_(native_value),
          release_(release) {}

    http_response_file_descriptor(const http_response_file_descriptor&) = delete;
    http_response_file_descriptor& operator=(const http_response_file_descriptor&) = delete;

    constexpr http_response_file_descriptor(http_response_file_descriptor&& other) noexcept
        : native_(std::exchange(other.native_, native_type{})),
          release_(std::exchange(other.release_, nullptr)) {}

    http_response_file_descriptor& operator=(http_response_file_descriptor&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        http_response_file_descriptor retired(std::move(other));
        std::swap(native_, retired.native_);
        std::swap(release_, retired.release_);
        // Commit both owners and the return reference before release can reenter.
        return *this;
    }

    ~http_response_file_descriptor() {
        reset();
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return release_ != nullptr;
    }

    // Borrowed while this descriptor is alive; meaningful only when engaged.
    [[nodiscard]] constexpr native_type native() const noexcept {
        return native_;
    }

private:
    void reset() noexcept {
        if (release_ != nullptr) {
            std::exchange(release_, nullptr)(std::exchange(native_, native_type{}));
        }
    }

    native_type native_{};
    release_type release_{nullptr};
};

// Read-only descriptor for a response file. Its path and any opened file are
// borrowed and remain valid for the lifetime of the response body from which
// it was obtained.
class http_response_file_view final {
public:
    constexpr http_response_file_view(const http_native_path_char_type* native_path, std::uint64_t size,
        std::uint64_t offset, std::uint64_t length, http_response_file_identity identity,
        const http_response_file_descriptor* opened = nullptr) noexcept
        : native_path_(native_path),
          opened_(opened != nullptr && *opened ? opened : nullptr),
          size_(size),
          offset_(offset),
          length_(length),
          identity_(identity) {}

    [[nodiscard]] constexpr const http_native_path_char_type* native_path_c_str() const noexcept {
        return native_path_;
    }
    [[nodiscard]] std::filesystem::path to_path() const {
        return std::filesystem::path(native_path_);
    }
    [[nodiscard]] constexpr std::uint64_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] constexpr std::uint64_t offset() const noexcept {
        return offset_;
    }
    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }
    [[nodiscard]] constexpr http_response_file_identity identity() const noexcept {
        return identity_;
    }
    // The descriptor the response was framed from, or null when the transport
    // must open native_path_c_str() and validate identity() itself.
    [[nodiscard]] constexpr const http_response_file_descriptor* opened_file() const noexcept {
        return opened_;
    }

private:
    const http_native_path_char_type* native_path_;
    const http_response_file_descriptor* opened_;
    std::uint64_t size_;
    std::uint64_t offset_;
    std::uint64_t length_;
    http_response_file_identity identity_;
};

// A borrowed view of one response-body segment. Any bytes and file path remain
// valid only while the owning http_response is alive and unmodified.
struct http_response_body_segment_view final {
    std::string_view bytes_{};
    std::optional<http_response_file_view> file_{};
};

// A byte range already resolved against the selected representation.
struct http_byte_range final {
    std::uint64_t offset_{};
    std::uint64_t length_{};

    [[nodiscard]] constexpr std::uint64_t offset() const noexcept {
        return offset_;
    }

    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }
};
enum class http_byte_range_order_policy : std::uint8_t {
    coalesce,
    strict_ascending_disjoint,
};

struct http_byte_range_options final {
    std::size_t max_ranges_{16};
    http_byte_range_order_policy order_policy_{http_byte_range_order_policy::coalesce};
    bool strict_order_{false};
};

// Bounded result for a byte-range set. Inline storage covers up to 16 ranges
// without allocation; larger configured max_ranges_ spill into heap storage.
class http_byte_range_set final {
public:
    static constexpr std::size_t capacity = 16;

    using range = http_byte_range;

    http_byte_range_set() noexcept = default;

    [[nodiscard]] constexpr bool ignored() const noexcept {
        return ignored_;
    }
    [[nodiscard]] constexpr bool unsatisfiable() const noexcept {
        return !ignored_ && count_ == 0;
    }
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return count_;
    }
    [[nodiscard]] std::span<const range> ranges() const& noexcept RUVIA_LIFETIMEBOUND {
        return overflow_.empty() ? std::span<const range>(ranges_.data(), count_)
                                 : std::span<const range>(overflow_.data(), count_);
    }
    std::span<const range> ranges() const&& = delete;

    [[nodiscard]] const range* begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return ranges().data();
    }
    const range* begin() const&& = delete;

    [[nodiscard]] const range* end() const& noexcept RUVIA_LIFETIMEBOUND {
        return ranges().data() + count_;
    }
    const range* end() const&& = delete;

    // Throws std::out_of_range for index >= size().
    [[nodiscard]] const range& operator[](std::size_t index) const& RUVIA_LIFETIMEBOUND {
        if (index >= count_) {
            throw std::out_of_range("HTTP byte range index out of range");
        }
        return overflow_.empty() ? ranges_[index] : overflow_[index];
    }
    const range& operator[](std::size_t) const&& = delete;

private:
    friend http_byte_range_set resolve_http_byte_range_set(
        std::string_view, http_content_length, http_byte_range_options);

    range& mutable_at(std::size_t index) noexcept {
        return overflow_.empty() ? ranges_[index] : overflow_[index];
    }

    void append_slot(range value) {
        if (overflow_.empty() && count_ < capacity) {
            ranges_[count_++] = value;
            return;
        }
        if (overflow_.empty()) {
            overflow_.reserve(capacity * 2);
            overflow_.insert(overflow_.end(), ranges_.begin(), ranges_.begin() + count_);
        }
        overflow_.push_back(value);
        count_ = overflow_.size();
    }

    void resize_slots(std::size_t new_count) noexcept {
        count_ = new_count;
        if (!overflow_.empty()) {
            overflow_.resize(new_count);
        }
    }

    std::array<range, capacity> ranges_{};
    std::vector<range> overflow_;
    std::size_t count_{};
    bool ignored_{true};
};

[[nodiscard]] http_byte_range_set resolve_http_byte_range_set(
    std::string_view field_value, http_content_length representation_length_value,
    http_byte_range_options options);

[[nodiscard]] inline http_byte_range_set resolve_http_byte_range_set(
    std::string_view field_value, http_content_length representation_length_value) noexcept {
    return resolve_http_byte_range_set(field_value, representation_length_value, {});
}

// Parsed Content-Range: "bytes first-last/total" (RFC 9110 section 14.4).
struct http_content_range final {
    std::uint64_t first_{};
    std::uint64_t last_{};
    std::uint64_t total_{};

    [[nodiscard]] constexpr std::uint64_t first() const noexcept {
        return first_;
    }
    [[nodiscard]] constexpr std::uint64_t last() const noexcept {
        return last_;
    }
    [[nodiscard]] constexpr std::uint64_t total() const noexcept {
        return total_;
    }
    [[nodiscard]] constexpr std::uint64_t offset() const noexcept {
        return first_;
    }
    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return last_ - first_ + 1;
    }
    [[nodiscard]] constexpr http_byte_range range() const noexcept {
        return {first_, last_ - first_ + 1};
    }

    friend constexpr bool operator==(const http_content_range&, const http_content_range&) noexcept = default;
};

[[nodiscard]] std::optional<http_content_range> parse_http_content_range(
    std::string_view field_value) noexcept;

[[nodiscard]] std::optional<std::uint64_t> compute_http_byte_range_content_length(
    std::span<const http_byte_range> ranges, http_content_length representation_length,
    std::string_view media_type = {}, std::string_view boundary = {}) noexcept;
[[nodiscard]] std::optional<std::uint64_t> compute_http_byte_range_content_length(
    const http_byte_range_set& ranges, http_content_length representation_length,
    std::string_view media_type = {}, std::string_view boundary = {}) noexcept;

// Streaming byte-range slicer for single-range 206 bodies and multi-range
// multipart/byteranges streams. Accepts sequential representation chunks (or
// partial chunks starting at a specified representation offset) and appends the
// framed output bytes to the caller's buffer.
class http_byte_range_slicer final {
public:
    http_byte_range_slicer(const http_byte_range_set& ranges,
        http_content_length representation_length, std::string_view media_type = {},
        std::string_view boundary = {});
    http_byte_range_slicer(std::span<const http_byte_range> ranges,
        http_content_length representation_length, std::string_view media_type = {},
        std::string_view boundary = {});

    [[nodiscard]] bool multipart() const noexcept {
        return ranges_.size() > 1;
    }
    [[nodiscard]] std::uint64_t content_length() const noexcept {
        return content_length_;
    }
    [[nodiscard]] std::string_view content_type() const& noexcept RUVIA_LIFETIMEBOUND {
        return content_type_;
    }
    std::string_view content_type() const&& = delete;
    [[nodiscard]] std::string_view content_range() const& noexcept RUVIA_LIFETIMEBOUND {
        return content_range_;
    }
    std::string_view content_range() const&& = delete;
    [[nodiscard]] std::uint64_t current_offset() const noexcept {
        return current_offset_;
    }
    [[nodiscard]] bool complete() const noexcept {
        return complete_;
    }

    void write(std::string_view chunk, std::string& output);
    void write(std::uint64_t chunk_offset, std::string_view chunk, std::string& output);
    void finish(std::string& output);
    void reset(std::uint64_t start_offset = 0) noexcept;

private:
    void emit_part_header(std::size_t index, std::string& output) const;

    std::vector<http_byte_range> ranges_;
    std::string media_type_;
    std::string boundary_;
    std::string content_type_;
    std::string content_range_;
    std::uint64_t representation_length_{0};
    std::uint64_t content_length_{0};
    std::uint64_t current_offset_{0};
    std::uint64_t range_progress_{0};
    std::size_t range_index_{0};
    bool complete_{false};
};

// Owns copied framing metadata and ordered file-slice descriptors. Observed
// views borrow the plan. Construction and clone propagate allocation failures.
class http_multipart_byte_range_plan final {
public:
    enum class segment_kind : std::uint8_t {
        metadata,
        file,
    };

    struct segment final {
        segment_kind kind_{};
        std::size_t metadata_offset_{};
        std::size_t metadata_length_{};
        std::uint64_t file_offset_{};
        std::uint64_t file_length_{};
    };

    http_multipart_byte_range_plan(const http_multipart_byte_range_plan&) = delete;
    http_multipart_byte_range_plan& operator=(const http_multipart_byte_range_plan&) = delete;
    http_multipart_byte_range_plan(http_multipart_byte_range_plan&&) noexcept = default;
    http_multipart_byte_range_plan& operator=(http_multipart_byte_range_plan&&) = delete;

    [[nodiscard]] std::string_view content_type() const& noexcept RUVIA_LIFETIMEBOUND {
        return state_ == nullptr ? std::string_view{} : std::string_view(state_->content_type_);
    }
    [[nodiscard]] std::string_view content_type() const&& = delete;
    [[nodiscard]] std::string_view metadata() const& noexcept RUVIA_LIFETIMEBOUND {
        return state_ == nullptr ? std::string_view{} : std::string_view(state_->metadata_);
    }
    [[nodiscard]] std::string_view metadata() const&& = delete;
    /// Ordered wire fragments, not multipart parts. Adjacent metadata may share a
    /// descriptor; consumers must not assume a fixed segment count per range.
    [[nodiscard]] std::span<const segment> segments() const& noexcept RUVIA_LIFETIMEBOUND {
        return state_ == nullptr
                   ? std::span<const segment>{}
                   : std::span<const segment>(state_->segments_.data(), state_->segment_count_);
    }
    [[nodiscard]] std::span<const segment> segments() const&& = delete;
    [[nodiscard]] std::uint64_t content_length() const noexcept {
        return state_ == nullptr ? 0 : state_->content_length_;
    }
    [[nodiscard]] http_multipart_byte_range_plan clone() const;

private:
    friend http_multipart_byte_range_plan make_http_multipart_byte_range_plan(
        const http_byte_range_set&, http_content_length, std::string_view, std::string_view);

    struct storage final {
        static constexpr std::size_t max_segments = http_byte_range_set::capacity * 2 + 1;

        storage() = default;

        std::string content_type_;
        std::string metadata_;
        std::array<segment, max_segments> segments_{};
        std::size_t segment_count_{};
        std::uint64_t content_length_{};
    };

    http_multipart_byte_range_plan()
        : state_(std::make_unique<storage>()) {}

    std::unique_ptr<storage> state_;
};

// Each body part carries the selected representation's Content-Type and its
// Content-Range (RFC 9110 §15.3.7.2). Parts never carry Content-Encoding: a
// slice of a content-coded representation is not itself a coded body, so
// plan_http_representation_response() answers multiple ranges of a coded
// representation with the full representation instead.
[[nodiscard]] http_multipart_byte_range_plan make_http_multipart_byte_range_plan(
    const http_byte_range_set& ranges, http_content_length representation_length,
    std::string_view media_type, std::string_view boundary);

// Metadata for an already selected, existing origin representation. This plan
// does not describe cache misses or select a representation. ETag text is
// borrowed only during planning; the resulting plan retains no input views.
struct http_selected_representation_metadata final {
    http_content_length length_{};
    borrowed_text etag_{};
    std::optional<std::time_t> last_modified_{};
    // Only authorizes a date-form If-Range when last_modified is also present.
    // Ordinary date preconditions do not require a strong validator.
    bool strong_date_validator_{false};
    // The representation carries a content coding (Content-Encoding). Body
    // parts of multipart/byteranges cannot declare the coding of the whole
    // representation they slice, and a header-section Content-Encoding would
    // claim the multipart body itself is coded (RFC 9110 §8.4, §14.6), so a
    // request for several ranges of such a representation is answered with
    // the full representation (ignoring Range is permitted, RFC 9110 §14.2).
    // A single range remains a single-part 206.
    bool content_coded_{false};
};

enum class http_range_request_policy {
    ignore,
    honor_byte_ranges,
};

struct http_representation_response_options final {
    // Origin response status before evaluating preconditions or Range.
    http_status_code normal_status_{http_status::ok};
    http_range_request_policy range_policy_{http_range_request_policy::ignore};
};

// An exclusive origin response selection. Owns only status/slicing values;
// response body permission and wire framing are decided at write submission.
class http_representation_response_plan final {
public:
    struct full_type final {
        http_status_code status_;
    };
    struct not_modified_type final {};
    struct precondition_failed_type final {};
    struct range_unsatisfiable_type final {};
    using partial_type = http_byte_range;
    using multipart_type = http_byte_range_set;

    [[nodiscard]] const full_type* full() const& noexcept;
    [[nodiscard]] const full_type* full() const&& = delete;
    [[nodiscard]] const not_modified_type* not_modified() const& noexcept;
    [[nodiscard]] const not_modified_type* not_modified() const&& = delete;
    [[nodiscard]] const precondition_failed_type* precondition_failed() const& noexcept;
    [[nodiscard]] const precondition_failed_type* precondition_failed() const&& = delete;
    [[nodiscard]] const range_unsatisfiable_type* range_unsatisfiable() const& noexcept;
    [[nodiscard]] const range_unsatisfiable_type* range_unsatisfiable() const&& = delete;
    [[nodiscard]] const partial_type* partial() const& noexcept;
    [[nodiscard]] const partial_type* partial() const&& = delete;
    [[nodiscard]] const multipart_type* multipart_ranges() const& noexcept;
    [[nodiscard]] const multipart_type* multipart_ranges() const&& = delete;
    [[nodiscard]] http_status_code status() const noexcept;
    // True when a partial() or multipart_ranges() outcome answers a request
    // whose If-Range matched. RFC 9110 §15.3.7: such a 206 SHOULD NOT repeat
    // representation header fields beyond the required ones (Date,
    // Cache-Control, ETag, Expires, Content-Location, Vary, plus Content-Range
    // or the multipart Content-Type), since the client already holds them.
    // Without If-Range a 206 MUST carry every representation field a 200 would.
    [[nodiscard]] bool authorized_by_if_range() const noexcept {
        return authorized_by_if_range_;
    }

private:
    friend http_representation_response_plan plan_http_representation_response(
        const http_request&, http_selected_representation_metadata,
        http_representation_response_options);

    using value_type = std::variant<full_type, not_modified_type, precondition_failed_type, range_unsatisfiable_type, partial_type, multipart_type>;

    template <typename alternative_type>
    explicit http_representation_response_plan(alternative_type alternative, bool authorized_by_if_range = false) noexcept
        : value_(alternative),
          authorized_by_if_range_(authorized_by_if_range) {}

    value_type value_;
    bool authorized_by_if_range_{false};
};

// Call after normal request checks, before processing request content or
// performing the method, with the target resource's current representation.
// The method must be supported and allowed. For extensions, the caller must
// establish that it selects or modifies that representation; non-representation
// extensions and extensions redefining conditional semantics are outside this API.
// CONNECT, OPTIONS, and TRACE ignore conditions, as do ineligible normal statuses.
[[nodiscard]] http_representation_response_plan plan_http_representation_response(
    const http_request& request, http_selected_representation_metadata representation,
    http_representation_response_options options = {});

enum class http_connect_udp_error : std::uint8_t;
class http1_response_head_plan;

enum class http_response_content_semantics : std::uint8_t {
    informational,
    protocol_switch,
    connect_tunnel,
    without_content,
    with_content,
};

class http_response_body_plan final {
public:
    [[nodiscard]] constexpr http_known_method request_method() const noexcept {
        return request_method_;
    }
    [[nodiscard]] constexpr http_status_code response_status() const noexcept {
        return response_status_;
    }
    [[nodiscard]] constexpr http_response_content_semantics content_semantics() const noexcept {
        return semantics_;
    }
    [[nodiscard]] constexpr bool status_allows_body() const noexcept {
        return status_allows_body_;
    }
    [[nodiscard]] constexpr bool body_suppressed() const noexcept {
        return body_suppressed_;
    }
    [[nodiscard]] constexpr bool auto_content_length_allowed() const noexcept {
        return auto_content_length_allowed_;
    }
    [[nodiscard]] constexpr bool explicit_content_length_allowed() const noexcept {
        return explicit_content_length_allowed_;
    }
    [[nodiscard]] constexpr bool transfer_encoding_allowed() const noexcept {
        return transfer_encoding_allowed_;
    }
    [[nodiscard]] std::uint64_t buffered_representation_length(const http_response& response) const noexcept;

private:
    friend http_response_body_plan plan_http_response_body(http_known_method, http_status_code) noexcept;
    constexpr http_response_body_plan(http_known_method method, http_status_code status, http_response_content_semantics semantics,
        bool status_allows_body, bool body_suppressed, bool auto_content_length_allowed,
        bool explicit_content_length_allowed, bool transfer_encoding_allowed) noexcept
        : request_method_(method),
          response_status_(status),
          semantics_(semantics),
          status_allows_body_(status_allows_body),
          body_suppressed_(body_suppressed),
          auto_content_length_allowed_(auto_content_length_allowed),
          explicit_content_length_allowed_(explicit_content_length_allowed),
          transfer_encoding_allowed_(transfer_encoding_allowed) {}

    http_known_method request_method_;
    http_status_code response_status_;
    http_response_content_semantics semantics_;
    bool status_allows_body_;
    bool body_suppressed_;
    bool auto_content_length_allowed_;
    bool explicit_content_length_allowed_;
    bool transfer_encoding_allowed_;
};

[[nodiscard]] http_response_body_plan plan_http_response_body(
    http_known_method request_method, http_status_code response_status) noexcept;

namespace detail {

// A buffered response sends payload bytes only when its method/status permit a
// body and the representation is non-empty.
[[nodiscard]] constexpr bool http_buffered_response_sends_body(
    const http_response_body_plan& body_plan, std::uint64_t content_length) noexcept {
    return !body_plan.body_suppressed() && content_length != 0;
}

}  // namespace detail

class http_buffered_response_write_plan final {
public:
    [[nodiscard]] http_known_method request_method() const noexcept {
        return body_plan_.request_method();
    }
    [[nodiscard]] http_status_code response_status() const noexcept {
        return body_plan_.response_status();
    }
    [[nodiscard]] http_response_body_plan body_plan() const noexcept {
        return body_plan_;
    }
    [[nodiscard]] std::uint64_t content_length() const noexcept {
        return content_length_;
    }
    [[nodiscard]] bool body_suppressed() const noexcept {
        return body_plan_.body_suppressed();
    }
    [[nodiscard]] bool status_allows_body() const noexcept {
        return body_plan_.status_allows_body();
    }
    [[nodiscard]] bool auto_content_length_allowed() const noexcept {
        return body_plan_.auto_content_length_allowed();
    }
    [[nodiscard]] bool explicit_content_length_allowed() const noexcept {
        return body_plan_.explicit_content_length_allowed();
    }
    [[nodiscard]] bool transfer_encoding_allowed() const noexcept {
        return body_plan_.transfer_encoding_allowed();
    }
    [[nodiscard]] bool send_body() const noexcept {
        return detail::http_buffered_response_sends_body(body_plan_, content_length_);
    }
    [[nodiscard]] bool matches_response(const http_response& response) const noexcept;

private:
    friend http_buffered_response_write_plan plan_buffered_http_response_write(
        http_known_method, const http_response&) noexcept;
    http_buffered_response_write_plan(http_response_body_plan body_plan, std::uint64_t content_length) noexcept
        : body_plan_(body_plan),
          content_length_(content_length) {}

    http_response_body_plan body_plan_;
    std::uint64_t content_length_;
};

[[nodiscard]] http_buffered_response_write_plan plan_buffered_http_response_write(
    http_known_method request_method, const http_response& response) noexcept;

class set_cookie_plan;

enum class http_response_header_mode : std::uint8_t {
    replace,
    append,
};

enum class http_response_header_transfer : std::uint8_t {
    merge,
    assign,
    apply,
};

// Content-Type values whose field lines are constants of this library. Setting
// one shares a static descriptor: no allocation and no grammar re-validation.
enum class http_content_type : std::uint8_t {
    text_plain_utf8,   // text/plain; charset=UTF-8
    text_html_utf8,    // text/html; charset=UTF-8
    application_json,  // application/json
};

struct http_response_header_options {
    http_response_header_mode mode_{http_response_header_mode::replace};
};

namespace detail {

struct http_response_body_access;
struct http_response_header_access;
struct http_response_header_state_access;

}  // namespace detail

struct http_response_header {
public:
    [[nodiscard]] std::string_view name() const noexcept {
        return bytes_ == nullptr ? std::string_view{} : std::string_view(bytes_, name_size_);
    }

    [[nodiscard]] std::string_view value() const noexcept {
        return bytes_ == nullptr ? std::string_view{}
                                 : std::string_view(bytes_ + name_size_, value_size_);
    }

private:
    friend class http_response;
    friend class http_response_headers;
    friend struct detail::http_response_header_access;

    http_response_header() noexcept = default;

    const char* bytes_{nullptr};
    std::uint32_t name_size_{0};
    std::uint32_t value_size_{0};
    std::uint32_t known_bit_{0};
    bool owned_{false};
    bool append_{false};
};

static_assert(std::is_trivially_copyable_v<http_response_header>);
static_assert(sizeof(http_response_header) <= 24);

// Owns a response header block: descriptor storage, the known-header index, every
// field mutation rule, and descriptor staging. A clone is a header-only
// transaction: edit it around external operations, then publish it with
// http_response::commit_headers.
class http_response_headers final {
public:
    using value_type = http_response_header;
    using const_iterator = const http_response_header*;

    // Moving transfers every descriptor and the index. The source remains
    // usable, independent, and empty.
    http_response_headers(http_response_headers&& other) noexcept;
    http_response_headers(const http_response_headers&) = delete;
    http_response_headers& operator=(const http_response_headers&) = delete;
    http_response_headers& operator=(http_response_headers&&) = delete;
    ~http_response_headers();

    [[nodiscard]] const_iterator begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return data();
    }
    [[nodiscard]] const_iterator begin() const&& = delete;

    [[nodiscard]] const_iterator end() const& noexcept RUVIA_LIFETIMEBOUND {
        return data() + size();
    }
    [[nodiscard]] const_iterator end() const&& = delete;

    [[nodiscard]] const_iterator cbegin() const& noexcept RUVIA_LIFETIMEBOUND {
        return begin();
    }
    [[nodiscard]] const_iterator cbegin() const&& = delete;

    [[nodiscard]] const_iterator cend() const& noexcept RUVIA_LIFETIMEBOUND {
        return end();
    }
    [[nodiscard]] const_iterator cend() const&& = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return spilled_ ? heap_.size() : size_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const&& = delete;
    // Inputs may borrow current header storage. Refresh views after mutation.
    void header(std::string_view key, std::string_view value);
    void header(std::string_view key, std::string_view value, http_response_header_options options);
    // Validated like header(). Name and value may be retained as borrowed views
    // without copying. Keep their bytes valid and unchanged while this block or
    // any clone retains them.
    void header_stable_view(std::string_view key, std::string_view value);
    // Replaces Content-Type with a library constant.
    void content_type(http_content_type type);
    void remove_header(std::string_view key);
    // Replace cookies with the same name, Domain/Path scope, and Partitioned
    // state; partitioned and unpartitioned cookies may coexist.
    void set_cookie(const set_cookie_plan& plan);
    void allow_methods(
        std::uint32_t method_mask, std::span<const std::string_view> extension_methods = {});
    void content_range(
        http_content_length offset, http_content_length length, http_content_length size);
    void content_range_unsatisfied(http_content_length size);
    void add_vary_token(std::string_view token);
    void reserve(std::size_t count);
    void transfer_from(const http_response_headers& source, http_response_header_transfer mode);
    // Owned bytes are copied so either block may change or be destroyed;
    // stable views are shared. Reserves room for additional_headers more fields.
    // Throws length_error before cloning if the total field count cannot fit.
    [[nodiscard]] http_response_headers clone(std::size_t additional_headers = 0) const;

private:
    friend class http_response;
    friend struct detail::http_response_header_state_access;

    using iterator = http_response_header*;

    static constexpr std::size_t known_header_count = 22;
    static constexpr std::size_t inline_capacity = 8;
    struct inline_storage_type {
        alignas(http_response_header) std::byte bytes_[sizeof(http_response_header)];
    };

    // Stages up to three owned descriptors without touching the table and
    // publishes them later without allocation or failure.
    class descriptor_stage;

    http_response_headers() noexcept;

    // Mutable iteration for the owning block and its friends. Named apart from
    // the public const begin()/end() so a non-const block still iterates
    // through the public overloads.
    [[nodiscard]] iterator mutable_begin() & noexcept {
        return data();
    }

    [[nodiscard]] iterator mutable_end() & noexcept {
        return data() + size();
    }

    // Header-only transaction publication. `staged` is left empty and independent.
    void commit(http_response_headers&& staged) noexcept;
    // Appends the source's fields in place when none can interact with an
    // existing field. Returns false, leaving this block unchanged, otherwise.
    [[nodiscard]] bool try_append_disjoint(const http_response_headers& source, http_response_header_transfer mode);
    // Replaces the field's line with `prepared`, or appends it; ownership of
    // `prepared` transfers either way.
    void publish_header(http_response_header prepared);
    // Releases `existing`, adopts `prepared` in its slot, and collapses the
    // field's other lines when `existing` was one of several.
    http_response_header& replace_header(http_response_header& existing, http_response_header prepared) noexcept;

    // Field rules and the known-header index.
    void record_known_header_index(std::uint32_t known_bit, std::size_t index) noexcept;
    void rebuild_known_header_index() noexcept;
    [[nodiscard]] http_response_header* find_header_for_update(
        std::string_view key, std::uint32_t known_bit) noexcept;
    [[nodiscard]] const http_response_header* find_header_for_read(
        std::string_view key, std::uint32_t known_bit) const noexcept;
    // Owns lookup, duplicate collapse, and index publication for every upsert.
    // Mutation receives the retained slot, or nullptr when it must append.
    template <typename Mutation>
    http_response_header& upsert_header(std::string_view key, std::uint32_t known_bit, Mutation&& mutate);
    http_response_header& prepare_header_value_storage(
        std::string_view key, std::size_t value_size, std::uint32_t known_bit);
    void set_header_validated(std::string_view key, std::string_view value, std::uint32_t known_bit, bool stable_view = false);
    void append_header_validated(
        std::string_view key, std::string_view value, std::uint32_t known_bit);
    http_response_header& append_header_uninitialized_value(
        std::string_view key, std::size_t value_size, std::uint32_t known_bit);
    [[nodiscard]] http_response_header& collapse_response_headers(
        http_response_header& retained, std::uint32_t known_bit) noexcept;
    bool remove_header_validated(std::string_view key, std::uint32_t known_bit) noexcept;
    void upsert_set_cookie_header_validated(std::string_view value);
    [[nodiscard]] http_response_header* find_set_cookie_header(std::string_view wire_prefix,
        std::string_view cookie_name, bool has_path, std::string_view path,
        std::string_view domain, bool partitioned) noexcept;
    void erase_later_set_cookie_headers(http_response_header& retained,
        std::string_view cookie_name, bool has_path, std::string_view path,
        std::string_view domain, bool partitioned) noexcept;
    // Descriptor storage.
    [[nodiscard]] http_response_header* inline_data() noexcept;
    [[nodiscard]] const http_response_header* inline_data() const noexcept;
    [[nodiscard]] http_response_header* data() noexcept;
    [[nodiscard]] const http_response_header* data() const noexcept;
    [[nodiscard]] http_response_header make_owned_header(
        std::string_view name, std::string_view value, std::uint32_t known_bit);
    [[nodiscard]] http_response_header make_uninitialized_header(
        std::string_view name, std::size_t value_size, std::uint32_t known_bit);
    [[nodiscard]] static std::optional<http_response_header> make_static_header(
        std::string_view name, std::string_view value, std::uint32_t known_bit) noexcept;
    [[nodiscard]] std::optional<http_response_header> make_shared_header(
        std::string_view name, std::string_view value, std::uint32_t known_bit, bool stable_view) const noexcept;
    [[nodiscard]] bool try_assign_owned_in_place(http_response_header& header, std::string_view name,
        std::string_view value, std::uint32_t known_bit) noexcept;
    [[nodiscard]] bool borrows_owned_header(std::string_view name, std::string_view value) const noexcept;
    http_response_header& append_header(http_response_header header);
    http_response_header& add_uninitialized_value(
        std::string_view name, std::size_t value_size, std::uint32_t known_bit);
    http_response_header& add(
        std::string_view name, std::string_view value, std::uint32_t known_bit, bool stable_view = false);
    void assign(http_response_header& header, std::string_view name, std::string_view value,
        std::uint32_t known_bit, bool stable_view = false);
    http_response_header& assign_uninitialized_value(http_response_header& header, std::string_view name,
        std::size_t value_size, std::uint32_t known_bit);
    void release_header(http_response_header& header) noexcept;
    // Drops every header from new_end onward; the dropped headers must
    // already be released.
    void truncate(iterator new_end) noexcept;
    http_response_header& append_prepared_header(http_response_header header) noexcept;
    void clear() noexcept;
    void spill(std::size_t min_capacity);
    void move_from(http_response_headers&& other) noexcept;

    std::vector<http_response_header> heap_;
    std::array<inline_storage_type, inline_capacity> inline_;
    std::size_t size_{0};
    bool spilled_{false};
    std::uint32_t known_header_bits_{0};
    std::array<std::int16_t, known_header_count> known_header_indexes_;
};

namespace detail {

class http_response_body;

class http_empty_response_body final {
public:
    [[nodiscard]] constexpr bool head_metadata_only() const noexcept {
        return head_metadata_only_;
    }

private:
    friend class http_response_body;
    explicit constexpr http_empty_response_body(bool head_metadata_only = false) noexcept
        : head_metadata_only_(head_metadata_only) {}

    bool head_metadata_only_{false};
};

class http_static_response_bytes final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    friend class http_response_body;

    explicit constexpr http_static_response_bytes(std::string_view bytes_value) noexcept
        : bytes_(bytes_value) {}

    std::string_view bytes_;
};

class http_owned_response_bytes final {
public:
    [[nodiscard]] std::string_view bytes() const& noexcept {
        return bytes_;
    }
    [[nodiscard]] std::string_view bytes() const&& = delete;

    [[nodiscard]] constexpr bool file_representation() const noexcept {
        return file_representation_;
    }

private:
    friend class http_response_body;

    explicit http_owned_response_bytes(std::string_view bytes_value)
        : bytes_(bytes_value) {}

    http_owned_response_bytes(std::string&& bytes_value, bool file_representation) noexcept
        : bytes_(std::move(bytes_value)),
          file_representation_(file_representation) {}

    std::string bytes_;
    bool file_representation_{false};
};

class http_owned_response_file final {
public:
    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }

    [[nodiscard]] http_response_file_view view() const& noexcept {
        return http_response_file_view(native_path_.c_str(), size_, offset_, length_, identity_, &opened_);
    }
    [[nodiscard]] http_response_file_view view() const&& = delete;

private:
    friend class http_response_body;

    http_owned_response_file(http_native_path_string_type&& native_path,
        std::uint64_t size, std::uint64_t offset, std::uint64_t length,
        http_response_file_identity identity, http_response_file_descriptor opened) noexcept
        : native_path_(std::move(native_path)),
          size_(size),
          offset_(offset),
          length_(length),
          identity_(identity),
          opened_(std::move(opened)) {}

    http_owned_response_file(http_native_path_view_type file,
        std::uint64_t size, std::uint64_t offset, std::uint64_t length,
        http_response_file_identity identity, http_response_file_descriptor opened)
        : http_owned_response_file(http_native_path_string_type(file), size, offset,
              length, identity, std::move(opened)) {}
    http_native_path_string_type native_path_;
    std::uint64_t size_;
    std::uint64_t offset_;
    std::uint64_t length_;
    http_response_file_identity identity_;
    http_response_file_descriptor opened_;
};

class http_multipart_response_body final {
public:
    http_multipart_response_body(http_native_path_view_type path,
        std::uint64_t size, http_response_file_identity identity, http_multipart_byte_range_plan plan,
        http_response_file_descriptor opened)
        : native_path_(path),
          plan_(std::move(plan)),
          size_(size),
          identity_(identity),
          opened_(std::move(opened)) {
        if (plan_.segments().empty()) {
            throw std::invalid_argument("multipart response plan must contain segments");
        }
        bool contains_file = false;
        for (const auto& segment : plan_.segments()) {
            if (segment.kind_ == http_multipart_byte_range_plan::segment_kind::file) {
                if (segment.file_length_ == 0 || segment.file_offset_ > size_ ||
                    segment.file_length_ > size_ - segment.file_offset_) {
                    throw std::invalid_argument("multipart response file segment is out of range");
                }
                contains_file = true;
            }
        }
        if (!contains_file) {
            throw std::invalid_argument("multipart response plan must contain a file segment");
        }
    }

    [[nodiscard]] std::size_t segment_count() const noexcept {
        return plan_.segments().size();
    }
    [[nodiscard]] http_response_body_segment_view segment(std::size_t index) const {
        const auto& item = plan_.segments()[index];
        if (item.kind_ == http_multipart_byte_range_plan::segment_kind::metadata) {
            return {.bytes_ = plan_.metadata().substr(item.metadata_offset_, item.metadata_length_)};
        }
        return {.file_ = http_response_file_view(native_path_.c_str(), size_, item.file_offset_,
                    item.file_length_, identity_, &opened_)};
    }
    [[nodiscard]] std::uint64_t content_length() const noexcept {
        return plan_.content_length();
    }
    [[nodiscard]] http_response_file_view file() const noexcept {
        return http_response_file_view(native_path_.c_str(), size_, 0, size_, identity_, &opened_);
    }

private:
    friend class http_response_body;

    http_multipart_response_body(http_native_path_string_type&& native_path,
        http_multipart_byte_range_plan&& plan, std::uint64_t size,
        http_response_file_identity identity, http_response_file_descriptor opened) noexcept
        : native_path_(std::move(native_path)),
          plan_(std::move(plan)),
          size_(size),
          identity_(identity),
          opened_(std::move(opened)) {}

    http_native_path_string_type native_path_;
    http_multipart_byte_range_plan plan_;
    std::uint64_t size_{};
    http_response_file_identity identity_{http_response_file_identity::unchecked()};
    http_response_file_descriptor opened_;
};

// Owns exactly one legal buffered response-body representation. The common
// bytes()/file()/size() observations are derived from the active alternative.
class http_response_body final {
public:
    http_response_body() noexcept
        : value_(http_empty_response_body{}) {}

    http_response_body(const http_response_body&) = delete;
    http_response_body& operator=(const http_response_body&) = delete;
    http_response_body(http_response_body&& other) noexcept
        : value_(std::move(other.value_)) {
        other.set_empty();
    }
    http_response_body& operator=(http_response_body&&) = delete;

    [[nodiscard]] const http_static_response_bytes* static_bytes() const& noexcept {
        return std::get_if<http_static_response_bytes>(&value_);
    }
    [[nodiscard]] const http_static_response_bytes* static_bytes() const&& = delete;

    [[nodiscard]] const http_owned_response_bytes* owned_bytes() const& noexcept {
        return std::get_if<http_owned_response_bytes>(&value_);
    }
    [[nodiscard]] const http_owned_response_bytes* owned_bytes() const&& = delete;

    [[nodiscard]] const http_owned_response_file* owned_file() const& noexcept {
        return std::get_if<http_owned_response_file>(&value_);
    }
    [[nodiscard]] const http_owned_response_file* owned_file() const&& = delete;

    [[nodiscard]] const http_multipart_response_body* multipart_body() const& noexcept {
        return std::get_if<http_multipart_response_body>(&value_);
    }
    [[nodiscard]] const http_multipart_response_body* multipart_body() const&& = delete;

    [[nodiscard]] std::string_view bytes() const& noexcept {
        if (const auto* body = static_bytes()) {
            return body->bytes();
        }
        if (const auto* body = owned_bytes()) {
            return body->bytes();
        }
        return {};
    }
    [[nodiscard]] std::string_view bytes() const&& = delete;

    [[nodiscard]] std::optional<http_response_file_view> file() const& noexcept {
        if (const auto* body = owned_file()) {
            return body->view();
        }
        if (const auto* body = multipart_body()) {
            return body->file();
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<http_response_file_view> file() const&& = delete;

    [[nodiscard]] bool file_representation() const noexcept {
        if (const auto* body = owned_bytes()) {
            return body->file_representation();
        }
        return owned_file() != nullptr || multipart_body() != nullptr;
    }

    [[nodiscard]] bool head_metadata_only() const noexcept {
        if (const auto* body = std::get_if<http_empty_response_body>(&value_)) {
            return body->head_metadata_only();
        }
        return false;
    }

    [[nodiscard]] std::uint64_t size() const noexcept {
        if (const auto* body = owned_file()) {
            return body->length();
        }
        if (const auto* body = multipart_body()) {
            return body->content_length();
        }
        return static_cast<std::uint64_t>(bytes().size());
    }

private:
    friend class ::ruvia::http_response;
    friend struct http_response_body_access;

    using value_type = std::variant<http_empty_response_body, http_static_response_bytes,
        http_owned_response_bytes, http_owned_response_file, http_multipart_response_body>;
    static_assert(std::is_nothrow_move_constructible_v<value_type>);

    template <typename body_type>
    void replace(body_type&& body) noexcept {
        // Publish the complete alternative before a resource callback can reenter.
        value_type retired(std::move(value_));
        value_.emplace<std::remove_cvref_t<body_type>>(std::forward<body_type>(body));
    }

    void set_empty() noexcept {
        replace(http_empty_response_body{});
    }

    void set_head_metadata_only() noexcept {
        replace(http_empty_response_body(true));
    }

    void set_copy(std::string_view bytes_value) {
        if (bytes_value.empty()) {
            set_empty();
            return;
        }
        http_owned_response_bytes body(bytes_value);
        replace(std::move(body));
    }

    void set_static(std::string_view bytes_value) noexcept {
        if (bytes_value.empty()) {
            set_empty();
            return;
        }
        replace(http_static_response_bytes(bytes_value));
    }

    void set_owned(std::string&& bytes_value) noexcept {
        if (bytes_value.empty()) {
            set_empty();
            return;
        }
        http_owned_response_bytes body(std::move(bytes_value), false);
        replace(std::move(body));
    }

    // Kept even when empty: an empty file is still a file representation.
    void set_file_bytes(std::string&& bytes_value) noexcept {
        http_owned_response_bytes body(std::move(bytes_value), true);
        replace(std::move(body));
    }

    void set_owned_file(http_native_path_view_type file,
        std::uint64_t size, std::uint64_t offset, std::uint64_t length,
        http_response_file_identity identity, http_response_file_descriptor opened) {
        http_owned_response_file body(file, size, offset, length, identity, std::move(opened));
        replace(std::move(body));
    }

    void set_multipart(http_native_path_view_type file,
        std::uint64_t size, http_response_file_identity identity,
        http_multipart_byte_range_plan plan, http_response_file_descriptor opened) {
        http_multipart_response_body replacement(
            file, size, identity, std::move(plan), std::move(opened));
        replace(std::move(replacement));
    }

    value_type value_;
};

static_assert(std::is_nothrow_move_constructible_v<http_owned_response_bytes>);
static_assert(std::is_nothrow_move_constructible_v<http_owned_response_file>);
static_assert(std::is_nothrow_move_constructible_v<http_multipart_response_body>);

}  // namespace detail

// Composes one header block with one body. Header rules belong to
// http_response_headers; this type only publishes body and headers together.
class http_response final {
public:
    http_response() noexcept;

    http_response(const http_response&) = delete;
    http_response& operator=(const http_response&) = delete;
    // Moving transfers headers and body. The source remains reusable with
    // empty headers and body.
    http_response(http_response&& other) noexcept;
    http_response& operator=(http_response&& other) noexcept;

    [[nodiscard]] http_status_code status() const noexcept;
    [[nodiscard]] const http_response_headers& headers() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http_response_headers& headers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const&& = delete;
    // A generic http_response is always final (200..599). Interim 1xx progress
    // messages use http_interim_response_head; 101 uses a dedicated protocol driver.
    void status(http_status_code status_code);
    // Header mutations follow http_response_headers.
    void header(std::string_view key, std::string_view value);
    void header(std::string_view key, std::string_view value, http_response_header_options options);
    void header_stable_view(std::string_view key, std::string_view value);
    void content_type(http_content_type type);
    // Format Allow from known-method bits followed by extension method values.
    // Extension methods must be nonempty HTTP tokens and may borrow current headers.
    // The resulting field is owned by this response; refresh views after mutation.
    void allow_methods(
        std::uint32_t method_mask, std::span<const std::string_view> extension_methods = {});
    void remove_header(std::string_view key);
    void set_cookie(const set_cookie_plan& plan);
    // Copy the source's application headers with RFC-aware append and cookie semantics.
    // Assignment preserves the destination Content-Type.
    void transfer_headers_from(const http_response& source, http_response_header_transfer mode);
    // Publish a header-only transaction staged from headers().clone() without
    // allocation. The staged block is left empty and independent.
    void commit_headers(http_response_headers&& staged) noexcept;
    // Borrowed byte representation; file and empty bodies return an empty view.
    [[nodiscard]] std::string_view body() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_bytes();
    }
    std::string_view body() const&& = delete;
    [[nodiscard]] std::string_view body_bytes() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view body_bytes() const&& = delete;
    [[nodiscard]] std::optional<http_response_file_view> file_body() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<http_response_file_view> file_body() const&& = delete;
    [[nodiscard]] bool has_multipart_file_body() const noexcept;
    // True for file_body(), multipart_file_body() and file_bytes_body() bodies:
    // the bytes are a file's representation, which a sender transmits as is.
    [[nodiscard]] bool has_file_representation() const noexcept;
    // 将正文替换为仅描述 HEAD 所选表示的空正文，保留首部并释放原正文资源。
    // 序列化 HEAD 时保留显式 Content-Length；未提供长度时不推断为零。
    // 标记随移动转移；重新设置正文会清除它，不影响 GET 等方法的正文规则。
    void head_metadata_body() noexcept;
    [[nodiscard]] bool has_head_metadata_body() const noexcept;
    // File segments retain their path and identity even when their length is
    // zero. Empty byte bodies have no segments; multipart bodies retain their
    // planned sequence of file ranges and boundary bytes.
    [[nodiscard]] std::size_t body_segment_count() const noexcept;
    [[nodiscard]] http_response_body_segment_view body_segment(std::size_t index) const& RUVIA_LIFETIMEBOUND;
    http_response_body_segment_view body_segment(std::size_t index) const&& = delete;
    void body(std::string_view value);
    void owned_body(std::string&& value) noexcept;
    void static_body(std::string_view value) noexcept;
    // Own bytes read from a file: all of it, a byte range, or a multipart
    // byte-range payload of it. Unlike owned_body(), the body remains a file
    // representation, even when empty.
    void file_bytes_body(std::string&& value) noexcept;
    // Own the native path and preserve the
    // runtime's opaque identity token. `opened`, when engaged, is the descriptor the
    // runtime framed the response from; the body owns it and releases it on
    // destruction or replacement. Path and range validation precede body
    // replacement; a rejected call releases `opened`.
    void file_body(http_native_path_view_type file, http_content_length size,
        http_content_length offset, http_content_length length, http_response_file_identity identity,
        http_response_file_descriptor opened = {});
    void multipart_file_body(http_native_path_view_type file, http_content_length size,
        http_response_file_identity identity, http_multipart_byte_range_plan&& plan,
        http_response_file_descriptor opened = {});
    void content_range(
        http_content_length offset, http_content_length length, http_content_length size);
    void content_range_unsatisfied(http_content_length size);
    void add_vary_token(std::string_view token);
    void reserve_headers(std::size_t count);
    // Apply a streaming coding, or atomically replace a buffered representation
    // and its coding-dependent metadata. The coding must be a nonempty HTTP coding list;
    // outer SP/HTAB are removed before storing its field value.
    // Both set Content-Encoding, weaken a strong ETag, drop an ETag that is not a
    // quoted entity-tag, and drop Accept-Ranges; the replacement sets
    // Content-Length, the streaming form removes it. A failed replacement leaves
    // this response unchanged.
    void apply_content_encoding(std::string_view content_encoding);
    void replace_body_with_content_encoding(
        std::string&& value, std::string_view content_encoding);

private:
    friend std::variant<http_response, http_connect_udp_error> prepare_http_connect_udp_response(http_response, http_protocol_version);
    friend http1_response_head_plan prepare_http1_upgrade_response_head(http_response&, const http_request&);
    friend struct detail::http_response_body_access;

    void apply_encoded_representation(std::string_view content_encoding, std::string* body);

    http_status_code status_code_{http_status::ok};
    http_response_headers headers_;
    detail::http_response_body body_;
};

class http_origin_view;
enum class cookie_prefix : std::uint8_t {
    secure,  // serializes the name as "__Secure-<name>"; requires secure
    host,    // serializes the name as "__Host-<name>"; requires secure, Path=/, no Domain
};

enum class cookie_same_site : std::uint8_t {
    strict,
    lax,
    none,  // the literal SameSite=None attribute; requires Secure
};

enum class cookie_priority : std::uint8_t {
    low,
    medium,
    high,
};

enum class cookie_attribute_policy : std::uint8_t {
    omit,
    emit,
};

// Expires value for deleting a cookie (Thu, 01 Jan 1970 00:00:00 GMT). RFC 6265
// section 4.1.1 limits max-age-av to non-zero digits, so deletion uses an
// Expires date in the past (RFC 6265 section 3.1) instead of Max-Age=0.
inline constexpr std::chrono::system_clock::time_point cookie_deletion_expires{};

// RFC 6265bis user-agent cookie-age-limit (400 days): the outbound cookie jar caps
// received lifetimes at this value. The Set-Cookie writer does not enforce it,
// since the limit binds user agents, not servers.
inline constexpr std::int64_t max_cookie_age_seconds = 34560000;

// One Cookie request cookie-pair, "name=value". The '=' is always written,
// including for an empty name, so a nameless value that itself looks like a
// cookie-pair stays behind '='. Callers join pairs with "; " (RFC 6265
// section 4.2.1).
[[nodiscard]] constexpr std::size_t cookie_request_pair_size(std::string_view name, std::string_view value) noexcept {
    return name.size() + 1U + value.size();
}

// Writes cookie_request_pair_size(name, value) bytes at out and returns the
// end of the written pair. Only inputs overlapping the written bytes need a
// snapshot: short snapshots use the stack, otherwise heap storage.
// Allocation failure leaves the output untouched.
char* write_cookie_request_pair(char* out, std::string_view name, std::string_view value);

// Cookie request pairs always include '=', including for an empty name.
// Inputs may borrow header_value, including its terminating NUL.
// Failure preserves the original contents; capacity may grow.
template <typename string>
void append_cookie_request_pair(string& header_value, std::string_view name, std::string_view value) {
    const auto start = header_value.size();
    const std::size_t separator_bytes = start == 0 ? 0 : 2;
    const auto remaining = header_value.max_size() - start;
    if (remaining < separator_bytes + 1 || name.size() > remaining - separator_bytes - 1 ||
        value.size() > remaining - separator_bytes - 1 - name.size()) {
        throw std::length_error("cookie request header length is not representable");
    }
    const auto borrowed_offset = [&](std::string_view input) -> std::optional<std::size_t> {
        if (input.empty()) {
            return std::nullopt;
        }
        const auto first = reinterpret_cast<std::uintptr_t>(header_value.data());
        const auto input_address = reinterpret_cast<std::uintptr_t>(input.data());
        if (input_address < first || input_address - first > start) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(input_address - first);
    };
    const auto name_offset = borrowed_offset(name);
    const auto value_offset = borrowed_offset(value);
    // Growing may reallocate; the value-initialized bytes keep a borrowed
    // terminator readable as NUL until the writer consumes it.
    header_value.resize(start + separator_bytes + cookie_request_pair_size(name, value));
    auto* const bytes = header_value.data();
    if (name_offset) {
        name = std::string_view(bytes + *name_offset, name.size());
    }
    if (value_offset) {
        value = std::string_view(bytes + *value_offset, value.size());
    }
    // The pair is written before the separator, which would otherwise
    // overwrite a borrowed terminator first.
    try {
        (void)write_cookie_request_pair(bytes + start + separator_bytes, name, value);
    } catch (...) {
        header_value.resize(start);
        throw;
    }
    if (separator_bytes != 0) {
        bytes[start] = ';';
        bytes[start + 1] = ' ';
    }
}

// Cookie octet validation and name-prefix rules are protocol rules shared by
// the Set-Cookie writer and outbound cookie jar. is_valid_cookie_value accepts
// the RFC 6265 cookie-value grammar, including one DQUOTE pair around the octets.
[[nodiscard]] bool is_valid_cookie_value(std::string_view value) noexcept;
// The prefix a wire cookie name starts with, matched ASCII case-insensitively
// as user agents do (RFC 6265bis 4.1.3); nullopt when it carries none.
[[nodiscard]] std::optional<cookie_prefix> cookie_name_prefix(std::string_view name) noexcept;
// RFC 6265bis 4.1.3: __Secure- requires Secure; __Host- also requires Path=/
// and no Domain. `path` is the Path attribute value, empty when absent.
[[nodiscard]] bool cookie_prefix_requirements_met(
    cookie_prefix prefix, bool secure, std::string_view path, std::string_view domain) noexcept;
[[nodiscard]] std::string_view http_cookie_prefix_text(cookie_prefix prefix) noexcept;

struct cookie_options final {
    // Cookie attributes are retained by set_cookie_plan until serialization.
    // Keep their zero-copy representation, but reject owning-string rvalues so
    // a stored options value cannot silently contain an already-dangling view.
    ::ruvia::borrowed_text path_{"/"};
    ::ruvia::borrowed_text domain_{};
    std::optional<cookie_same_site> same_site_{};
    std::optional<cookie_priority> priority_{};
    // UTC, rounded down to whole seconds; year >= 1601. Lifetime caps belong to user agents.
    std::optional<std::chrono::system_clock::time_point> expires_{};
    // Zero means expire now: written as Expires=cookie_deletion_expires (replacing
    // expires_), since max-age-av requires a non-zero first digit (RFC 6265 4.1.1).
    std::optional<std::chrono::seconds> max_age_{};
    std::optional<cookie_prefix> prefix_{};
    cookie_attribute_policy http_only_{cookie_attribute_policy::omit};
    cookie_attribute_policy secure_{cookie_attribute_policy::omit};
    cookie_attribute_policy partitioned_{cookie_attribute_policy::omit};
};

// Visits literal semicolon-separated Cookie name/value pairs. A quote has no
// special meaning in a Cookie field. Segments without '=' are skipped, names
// and values are OWS-trimmed, and returning false stops traversal. Views borrow
// the input field.
template <typename visitor_type>
inline void http_visit_cookie_pairs(std::string_view value, visitor_type&& visitor) {
    while (!value.empty()) {
        const auto semicolon = value.find(';');
        const auto part = http_trim_ows(
            semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
        const auto equals = part.find('=');
        if (equals != std::string_view::npos &&
            !visitor(http_trim_ows(part.substr(0, equals)), http_trim_ows(part.substr(equals + 1)))) {
            return;
        }
        if (semicolon == std::string_view::npos) {
            return;
        }
        value.remove_prefix(semicolon + 1);
    }
}

// Validates and fixes the exact Set-Cookie field-value shape before a runtime
// allocates its output buffer. The plan borrows name, value, path and domain
// unchanged until write() completes, so owning-string and cookie_options
// temporaries are rejected at construction. Construction throws std::length_error if the wire
// length cannot be represented by std::size_t.
class set_cookie_plan final {
public:
    set_cookie_plan(std::string_view name, std::string_view value, const cookie_options& options);

    template <typename name_type, typename value_type>
        requires(detail::http_temporary_owning_char_string<name_type> ||
                    detail::http_temporary_owning_char_string<value_type>)
    set_cookie_plan(name_type&&, value_type&&, const cookie_options&) = delete;

    set_cookie_plan(std::string_view, std::string_view, cookie_options&&) = delete;
    set_cookie_plan(std::string_view, std::string_view, const cookie_options&&) = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] std::string_view wire_prefix() const noexcept {
        return prefix_text_;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::string_view domain() const noexcept {
        return domain_;
    }

    // output must cover size() bytes and not overlap the borrowed inputs.
    void write(char* output) const;

private:
    friend class http_response_headers;
    struct written_fields final {
        std::string_view wire_name_;
        std::string_view path_;
        std::string_view domain_;
    };
    [[nodiscard]] written_fields write_fields(char* output) const noexcept;
    std::array<char, 32> expires_buffer_{};
    std::string_view name_;
    std::string_view value_;
    std::string_view path_;
    std::string_view domain_;
    std::string_view prefix_text_;
    std::string_view priority_text_;
    std::string_view same_site_text_;
    std::uint64_t max_age_value_{0};
    std::size_t expires_size_{0};
    std::size_t size_{0};
    bool http_only_{false};
    bool secure_{false};
    bool partitioned_{false};
};

struct http_cookie_jar_options final {
    std::size_t max_cookies_{256};
    std::size_t max_cookie_bytes_{std::size_t{32} * 1024};
};

// Sans-I/O RFC 6265 client cookie jar for a fixed origin. Validates Domain,
// Path, Secure, SameSite=None, Partitioned, and __Secure-/__Host- prefixes,
// caps Max-Age/Expires at 400 days, evicts expired entries, and enforces
// count and byte budgets.
class http_cookie_jar final {
public:
    explicit http_cookie_jar(const http_origin_view& origin, http_cookie_jar_options options = {});
    http_cookie_jar(http_scheme scheme, std::string_view host, http_cookie_jar_options options = {});

    void add_configured_cookie(std::string_view name, std::string_view value);

    bool absorb_set_cookie(std::string_view request_target, std::string_view set_cookie_value,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    void retain_response_cookies(std::string_view request_target,
        std::span<const http_header> headers,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
    void retain_response_cookies(std::string_view request_target,
        std::span<const http_header_view> headers,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    [[nodiscard]] std::string cookie_header(std::string_view request_target,
        bool include_received = true,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    template <typename fn_type>
    void visit_matching_cookies(std::string_view request_target, bool include_received,
        std::chrono::system_clock::time_point now, fn_type&& fn) {
        if (cookies_.empty()) {
            return;
        }
        if (next_expiry_.has_value() && now >= *next_expiry_) {
            discard_expired(now);
        }
        if (cookies_.empty()) {
            return;
        }
        const auto path = request_path_only(request_target);
        for (const auto& cookie : cookies_) {
            if (!cookie.configured_ && !include_received) {
                continue;
            }
            if (cookie.secure_ && scheme_ != http_scheme::https) {
                continue;
            }
            if (!cookie_path_matches(path, cookie.path_)) {
                continue;
            }
            fn(std::string_view(cookie.name_), std::string_view(cookie.value_));
        }
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return cookies_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return cookies_.empty();
    }
    [[nodiscard]] std::size_t storage_bytes() const noexcept {
        return cookie_bytes_;
    }
    void clear() noexcept;

    [[nodiscard]] static bool cookie_domain_matches(
        std::string_view host, std::string_view domain) noexcept;
    [[nodiscard]] static bool cookie_path_matches(
        std::string_view request_path, std::string_view cookie_path) noexcept;
    [[nodiscard]] static std::string_view request_path_only(std::string_view target) noexcept;
    [[nodiscard]] static std::string_view default_cookie_path(std::string_view target) noexcept;

private:
    struct stored_cookie final {
        stored_cookie(std::string_view name, std::string_view value)
            : stored_cookie(name, value, "/", {}) {}
        stored_cookie(std::string_view name, std::string_view value,
            std::string_view path, std::string_view domain)
            : name_(name),
              value_(value),
              path_(path),
              domain_(domain) {}

        std::string name_;
        std::string value_;
        std::string path_;
        std::string domain_;
        std::optional<std::chrono::system_clock::time_point> expires_;
        bool secure_{false};
        bool partitioned_{false};
        bool configured_{true};
    };

    struct received_cookie final {
        std::string_view name_;
        std::string_view value_;
        std::string_view path_;
        std::string_view domain_;
        std::optional<std::chrono::system_clock::time_point> expires_;
        bool secure_{false};
        bool partitioned_{false};
        bool expired_{false};
    };

    [[nodiscard]] std::optional<received_cookie> accept_set_cookie_view(
        std::string_view request_target, std::string_view set_cookie_value,
        std::chrono::system_clock::time_point now) const;
    bool store_received_cookie(const received_cookie& cookie);
    void reserve_cookie_slot();
    void discard_expired(std::chrono::system_clock::time_point now);
    [[nodiscard]] static std::size_t entry_storage_bytes(std::string_view name,
        std::string_view value, std::string_view path, std::string_view domain) noexcept;
    [[nodiscard]] bool has_capacity(
        std::size_t replaced_bytes, std::size_t replacement_bytes, bool adding) const noexcept;
    void recompute_next_expiry() noexcept;

    std::string host_;
    http_scheme scheme_{http_scheme::http};
    std::size_t max_cookies_{256};
    std::size_t max_cookie_bytes_{std::size_t{32} * 1024};
    std::vector<stored_cookie> cookies_;
    std::optional<std::chrono::system_clock::time_point> next_expiry_{};
    std::size_t cookie_bytes_{0};
};

enum class http_set_cookie_attribute : std::uint8_t {
    secure = 1U << 0,
    partitioned = 1U << 1,
    http_only = 1U << 2,
};

// Borrowed, allocation-free Set-Cookie fields for outbound client runtimes,
// following the RFC 6265 section 5.2 user-agent algorithm. A cookie without '=',
// with an empty name, over 4096 name+value bytes, or with a control byte other
// than HTAB is rejected. Unknown and oversized attributes are ignored. path() is
// empty when the last Path value is
// empty or does not start with '/', meaning the caller's default-path. An empty
// Domain value is ignored; a leading '.' is removed, so domain() may be empty
// (host-only) and is not validated beyond domain-matching by the caller. Expires follows RFC 6265 cookie-date token grammar, including its
// permitted suffixes; this does not relax the separate HTTP-date grammar.
// Partitioned is reported independently of Secure; callers own origin policy.
class http_set_cookie_view final {
public:
    [[nodiscard]] constexpr std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] constexpr std::string_view value() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] constexpr std::string_view domain() const noexcept {
        return domain_;
    }

    [[nodiscard]] constexpr std::optional<std::time_t> expires() const noexcept {
        return expires_;
    }

    [[nodiscard]] constexpr std::optional<std::int64_t> max_age_seconds() const noexcept {
        return max_age_seconds_;
    }

    [[nodiscard]] constexpr bool has(http_set_cookie_attribute attribute) const noexcept {
        const auto mask = static_cast<std::uint8_t>(attribute);
        return mask != 0U && (mask & (mask - 1U)) == 0U && (attributes_ & mask) != 0U;
    }

    [[nodiscard]] constexpr bool http_only() const noexcept {
        return has(http_set_cookie_attribute::http_only);
    }

    [[nodiscard]] constexpr bool same_site_none() const noexcept {
        return same_site_ == cookie_same_site::none;
    }

    [[nodiscard]] constexpr std::optional<cookie_same_site> same_site() const noexcept {
        return same_site_;
    }

private:
    friend std::optional<http_set_cookie_view> parse_set_cookie(std::string_view value) noexcept;

    constexpr http_set_cookie_view(std::string_view name, std::string_view value) noexcept
        : name_(name),
          value_(value) {}

    constexpr void set(http_set_cookie_attribute attribute) noexcept {
        attributes_ |= static_cast<std::uint8_t>(attribute);
    }

    std::string_view name_;
    std::string_view value_;
    std::string_view path_;
    std::string_view domain_;
    std::optional<std::time_t> expires_;
    std::optional<std::int64_t> max_age_seconds_;
    std::optional<cookie_same_site> same_site_{};
    std::uint8_t attributes_{0};
};

[[nodiscard]] std::optional<http_set_cookie_view> parse_set_cookie(std::string_view value) noexcept;

template <detail::http_temporary_owning_char_string value_type>
std::optional<http_set_cookie_view> parse_set_cookie(value_type&&) = delete;

// Wire width of an RFC 9110 IMF-fixdate, e.g. "Sun, 06 Nov 1994 08:49:37 GMT".
inline constexpr std::size_t http_imf_fixdate_size = 29;

// RFC 9110 IMF-fixdate, or no value if the timestamp cannot be represented.
// The fixed-size result contains the 29 wire bytes without a terminator.
[[nodiscard]] std::optional<std::array<char, http_imf_fixdate_size>> format_http_date(std::time_t value) noexcept;

// Parse an HTTP-date (RFC 7231 section 7.1.1.1: IMF-fixdate / RFC 850 / asctime) as used by Date,
// Expires, Last-Modified, If-Modified-Since. Returns std::nullopt if malformed.
[[nodiscard]] std::optional<std::time_t> parse_http_date(std::string_view value) noexcept;

enum class cache_control_directive : std::uint16_t {
    no_store = 1U << 0,
    no_cache = 1U << 1,
    no_transform = 1U << 2,
    must_revalidate = 1U << 3,
    proxy_revalidate = 1U << 4,
    private_value = 1U << 5,
    public_value = 1U << 6,
    immutable = 1U << 7,
    only_if_cached = 1U << 8,
    max_stale_any = 1U << 9,
    private_unqualified = 1U << 10,
    no_cache_unqualified = 1U << 11,
};

// Parsed HTTP Cache-Control directives (RFC 9111 section 5.2). Unknown directives are ignored.
// Boolean directives are queried by directive enum; delta-seconds directives are optional (absent = not present).
// This type reports wire directives only; cache freshness and reuse policy belong to the caller.
class cache_control final {
public:
    [[nodiscard]] constexpr bool has(cache_control_directive directive) const noexcept {
        const auto mask = static_cast<std::uint16_t>(directive);
        return mask != 0U && (mask & (mask - 1U)) == 0U && (directives_ & mask) != 0U;
    }

    [[nodiscard]] constexpr bool no_store() const noexcept {
        return has(cache_control_directive::no_store);
    }
    [[nodiscard]] constexpr bool no_cache() const noexcept {
        return has(cache_control_directive::no_cache);
    }
    [[nodiscard]] constexpr bool no_transform() const noexcept {
        return has(cache_control_directive::no_transform);
    }
    [[nodiscard]] constexpr bool must_revalidate() const noexcept {
        return has(cache_control_directive::must_revalidate);
    }
    [[nodiscard]] constexpr bool proxy_revalidate() const noexcept {
        return has(cache_control_directive::proxy_revalidate);
    }
    [[nodiscard]] constexpr bool is_private() const noexcept {
        return has(cache_control_directive::private_value);
    }
    [[nodiscard]] constexpr bool is_unqualified_private() const noexcept {
        return has(cache_control_directive::private_unqualified);
    }
    [[nodiscard]] constexpr bool is_field_qualified_private() const noexcept {
        return is_private() && !is_unqualified_private();
    }
    [[nodiscard]] constexpr bool is_unqualified_no_cache() const noexcept {
        return has(cache_control_directive::no_cache_unqualified);
    }
    [[nodiscard]] constexpr bool is_field_qualified_no_cache() const noexcept {
        return no_cache() && !is_unqualified_no_cache();
    }
    [[nodiscard]] constexpr bool is_public() const noexcept {
        return has(cache_control_directive::public_value);
    }
    [[nodiscard]] constexpr bool immutable() const noexcept {
        return has(cache_control_directive::immutable);
    }
    [[nodiscard]] constexpr bool only_if_cached() const noexcept {
        return has(cache_control_directive::only_if_cached);
    }
    [[nodiscard]] constexpr bool max_stale_any() const noexcept {
        return has(cache_control_directive::max_stale_any);
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> max_age() const noexcept {
        return max_age_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> max_stale() const noexcept {
        return max_stale_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> min_fresh() const noexcept {
        return min_fresh_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> s_maxage() const noexcept {
        return s_max_age_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> stale_while_revalidate() const noexcept {
        return stale_while_revalidate_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> stale_if_error() const noexcept {
        return stale_if_error_;
    }

    [[nodiscard]] constexpr std::size_t private_field_count() const noexcept {
        return is_unqualified_private() ? 0U : private_fields_.count_;
    }
    [[nodiscard]] constexpr std::string_view private_field(
        std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return is_unqualified_private() ? std::string_view{} : private_fields_.at(index);
    }
    std::string_view private_field(std::size_t) const&& = delete;
    [[nodiscard]] bool has_private_field(std::string_view field_name) const noexcept;

    [[nodiscard]] constexpr std::size_t no_cache_field_count() const noexcept {
        return is_unqualified_no_cache() ? 0U : no_cache_fields_.count_;
    }
    [[nodiscard]] constexpr std::string_view no_cache_field(
        std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return is_unqualified_no_cache() ? std::string_view{} : no_cache_fields_.at(index);
    }
    std::string_view no_cache_field(std::size_t) const&& = delete;
    [[nodiscard]] bool has_no_cache_field(std::string_view field_name) const noexcept;

private:
    friend class cache_control_field_parser;
    friend std::optional<cache_control> parse_targeted_cache_control(
        std::string_view value) noexcept;
    template <typename input_type>
    friend std::optional<cache_control> parse_targeted_cache_control_from_input(
        input_type input) noexcept;

    static constexpr std::size_t max_nominated_fields = 16;
    static constexpr std::size_t max_nominated_field_bytes = 256;

    struct nominated_field_set final {
        std::array<char, max_nominated_field_bytes> storage_{};
        std::array<std::uint16_t, max_nominated_fields> offsets_{};
        std::array<std::uint8_t, max_nominated_fields> lengths_{};
        std::uint16_t storage_used_{0};
        std::uint8_t count_{0};

        constexpr void clear() noexcept {
            storage_used_ = 0;
            count_ = 0;
        }

        [[nodiscard]] constexpr std::string_view at(std::size_t index) const noexcept {
            if (index >= count_) {
                return {};
            }
            return std::string_view(storage_.data() + offsets_[index], lengths_[index]);
        }

        [[nodiscard]] bool contains(std::string_view field_name) const noexcept;
        [[nodiscard]] bool add_lowercase(std::string_view field_name) noexcept;
    };

    constexpr void set(cache_control_directive directive) noexcept {
        directives_ |= static_cast<std::uint16_t>(directive);
    }
    constexpr void clear_directive(cache_control_directive directive) noexcept {
        directives_ &= static_cast<std::uint16_t>(~static_cast<std::uint16_t>(directive));
    }

    std::uint16_t directives_{0};
    std::optional<std::uint64_t> max_age_;
    std::optional<std::uint64_t> max_stale_;
    std::optional<std::uint64_t> min_fresh_;
    std::optional<std::uint64_t> s_max_age_;
    std::optional<std::uint64_t> stale_while_revalidate_;
    std::optional<std::uint64_t> stale_if_error_;
    nominated_field_set private_fields_{};
    nominated_field_set no_cache_fields_{};
};

// Incrementally parses every Cache-Control field line as one logical directive
// list (RFC 9110 section 5.2). State spans updates so duplicate freshness
// directives keep the first occurrence even when they appear on different lines.
class cache_control_field_parser final {
public:
    void update(std::string_view field_value) noexcept;

    [[nodiscard]] cache_control finish() const noexcept {
        return value_;
    }

private:
    cache_control value_;
    bool max_age_seen_{false};
    bool max_stale_seen_{false};
    bool min_fresh_seen_{false};
    bool s_max_age_seen_{false};
    bool stale_while_revalidate_seen_{false};
    bool stale_if_error_seen_{false};
};

struct targeted_cache_control_selection final {
    cache_control control_{};
    std::string_view selected_header_{};
    bool targeted_{false};
};

class http_vary_spec;

namespace detail {
[[nodiscard]] cache_control parse_cache_control_headers(http_header_range_view headers) noexcept;
[[nodiscard]] std::optional<cache_control> parse_targeted_cache_control_headers(
    http_header_range_view headers, std::string_view header_name) noexcept;
[[nodiscard]] targeted_cache_control_selection resolve_targeted_cache_control_headers(
    http_header_range_view headers, std::span<const std::string_view> target_headers) noexcept;
[[nodiscard]] std::string rewrite_http_cache_control_max_age_headers(
    http_header_range_view headers, std::uint64_t client_max_age);
[[nodiscard]] std::optional<std::string> format_http_vary_cache_key_headers(
    const http_vary_spec& vary, http_header_range_view request_headers);
[[nodiscard]] std::uint64_t compute_http_cache_age_headers(
    http_header_range_view response_headers, std::time_t request_time, std::time_t response_time,
    std::time_t now) noexcept;
void merge_http_304_response_headers_into(
    std::vector<http_header>& cached_headers, http_header_range_view not_modified_headers);
void merge_http_304_response_headers_into(
    http_response_headers& cached_headers, http_header_range_view not_modified_headers);
}  // namespace detail

// Parse a Cache-Control field value (a single line, or several joined by commas).
[[nodiscard]] cache_control parse_cache_control(std::string_view value) noexcept;
template <http_header_field_source headers_type>
[[nodiscard]] cache_control parse_cache_control(const headers_type& headers) noexcept {
    return detail::parse_cache_control_headers(headers);
}

// RFC 9213 Targeted HTTP Cache Control (e.g. CDN-Cache-Control).
// Parses an RFC 8941 Structured Field Dictionary of cache directives; returns
// std::nullopt when absent, empty, or syntactically invalid per RFC 9213 §2.1.
[[nodiscard]] std::optional<cache_control> parse_targeted_cache_control(
    std::string_view value) noexcept;
template <http_header_field_source headers_type>
[[nodiscard]] std::optional<cache_control> parse_targeted_cache_control(
    const headers_type& headers,
    std::string_view header_name = "CDN-Cache-Control") noexcept {
    return detail::parse_targeted_cache_control_headers(headers, header_name);
}

// RFC 9213 §2.2 target-list selection: evaluates `target_headers` in priority
// order and returns the first present, valid non-empty targeted policy; falls
// back to standard Cache-Control when no targeted header matches.
template <http_header_field_source headers_type>
[[nodiscard]] targeted_cache_control_selection resolve_targeted_cache_control(
    const headers_type& headers,
    std::span<const std::string_view> target_headers) noexcept {
    return detail::resolve_targeted_cache_control_headers(headers, target_headers);
}

// RFC 9111 §5.2.2.7 & §5.2.2.3 field-qualified header stripping for shared caches.
// Returns false if unqualified `private` or `no-store` forbids shared-cache
// storage; returns true when storable (after stripping any field-qualified
// `private="field1, field2"` headers in place).
bool strip_http_cache_private_headers(
    std::vector<http_header>& headers, const cache_control& control) noexcept;
bool strip_http_cache_private_headers(
    http_response_headers& headers, const cache_control& control);
bool strip_http_cache_private_headers(
    http_response& response, const cache_control& control);
bool strip_http_cache_private_headers(std::vector<http_header>& headers) noexcept;
bool strip_http_cache_private_headers(http_response_headers& headers);
bool strip_http_cache_private_headers(http_response& response);

bool strip_http_cache_no_cache_headers(
    std::vector<http_header>& headers, const cache_control& control) noexcept;
bool strip_http_cache_no_cache_headers(
    http_response_headers& headers, const cache_control& control);
bool strip_http_cache_no_cache_headers(
    http_response& response, const cache_control& control);
[[nodiscard]] bool http_cache_control_has_no_transform(std::string_view field_value) noexcept;
template <http_header_field_source headers_type>
[[nodiscard]] bool http_cache_control_has_no_transform(const headers_type& headers) noexcept {
    return parse_cache_control(headers).no_transform();
}

struct http_cache_control_directive_view final {
    std::string_view raw_{};
    std::string_view name_{};
    std::optional<std::string_view> argument_{};
};

// Rewrites Cache-Control to preserve all directives except max-age and s-maxage
// while setting client max-age=<client_max_age>. Header-block overloads also
// clear Expires so the rewritten max-age governs client freshness.
[[nodiscard]] std::string rewrite_http_cache_control_max_age(
    std::string_view field_value, std::uint64_t client_max_age);
template <http_header_field_source headers_type>
[[nodiscard]] std::string rewrite_http_cache_control_max_age(
    const headers_type& headers, std::uint64_t client_max_age) {
    return detail::rewrite_http_cache_control_max_age_headers(headers, client_max_age);
}
void rewrite_http_response_cache_max_age(
    std::vector<http_header>& response_headers, std::uint64_t client_max_age);
void rewrite_http_response_cache_max_age(
    http_response_headers& response_headers, std::uint64_t client_max_age);
void rewrite_http_response_cache_max_age(
    http_response& response, std::uint64_t client_max_age);

// Parsed multi-line Vary specification (RFC 9111 section 4.1). Field names are
// normalized to lowercase, deduplicated, and sorted in fixed ASCII order.
class http_vary_spec final {
public:
    http_vary_spec() noexcept = default;

    void update(std::string_view field_value);
    template <http_header_field_source headers_type>
    void update_headers(const headers_type& headers) {
        update_header_range(headers);
    }

    [[nodiscard]] constexpr bool is_wildcard() const noexcept {
        return wildcard_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return !wildcard_ && field_names_.empty();
    }
    [[nodiscard]] std::span<const std::string> field_names() const& noexcept RUVIA_LIFETIMEBOUND {
        return field_names_;
    }
    std::span<const std::string> field_names() const&& = delete;

private:
    void update_header_range(detail::http_header_range_view headers);

    std::vector<std::string> field_names_;
    bool wildcard_{false};
};

[[nodiscard]] http_vary_spec parse_http_vary(std::string_view field_value);
template <http_header_field_source headers_type>
[[nodiscard]] http_vary_spec parse_http_vary(const headers_type& headers) {
    http_vary_spec spec;
    spec.update_headers(headers);
    return spec;
}

// Normalizes the request header values nominated by `vary` in canonical field
// order for cache-key generation. Returns std::nullopt when vary.is_wildcard().
template <http_header_field_source headers_type>
[[nodiscard]] std::optional<std::string> format_http_vary_cache_key(
    const http_vary_spec& vary, const headers_type& request_headers) {
    return detail::format_http_vary_cache_key_headers(vary, request_headers);
}

template <http_header_field_source first_type, http_header_field_source second_type>
[[nodiscard]] bool http_vary_requests_match(const http_vary_spec& vary,
    const first_type& first_request, const second_type& second_request) {
    const auto first_key = format_http_vary_cache_key(vary, first_request);
    const auto second_key = format_http_vary_cache_key(vary, second_request);
    return first_key.has_value() && second_key.has_value() && *first_key == *second_key;
}
// RFC 9111 section 4.2.3 Age calculation.
struct http_cache_age_input final {
    std::optional<std::time_t> date_value_{};
    std::optional<std::uint64_t> age_value_{};
    std::time_t request_time_{0};
    std::time_t response_time_{0};
    std::time_t now_{0};
};

[[nodiscard]] std::optional<std::uint64_t> parse_http_age(std::string_view field_value) noexcept;

[[nodiscard]] constexpr std::uint64_t compute_http_cache_age(
    const http_cache_age_input& input) noexcept {
    const auto add_saturating = [](std::uint64_t left, std::uint64_t right) constexpr noexcept {
        constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        return left > maximum - right ? maximum : left + right;
    };
    const std::uint64_t apparent_age =
        (input.date_value_.has_value() && input.response_time_ > *input.date_value_)
            ? static_cast<std::uint64_t>(input.response_time_) -
                  static_cast<std::uint64_t>(*input.date_value_)
            : 0U;
    const std::uint64_t response_delay =
        input.response_time_ > input.request_time_
            ? static_cast<std::uint64_t>(input.response_time_) -
                  static_cast<std::uint64_t>(input.request_time_)
            : 0U;
    const std::uint64_t corrected_age_value =
        add_saturating(input.age_value_.value_or(0U), response_delay);
    const std::uint64_t corrected_initial_age = (std::max)(apparent_age, corrected_age_value);
    const std::uint64_t resident_time =
        input.now_ > input.response_time_
            ? static_cast<std::uint64_t>(input.now_) -
                  static_cast<std::uint64_t>(input.response_time_)
            : 0U;
    return add_saturating(corrected_initial_age, resident_time);
}

[[nodiscard]] constexpr std::uint64_t compute_http_cache_age(
    std::optional<std::time_t> date_value, std::optional<std::uint64_t> age_value,
    std::time_t request_time, std::time_t response_time, std::time_t now) noexcept {
    return compute_http_cache_age(http_cache_age_input{
        .date_value_ = date_value,
        .age_value_ = age_value,
        .request_time_ = request_time,
        .response_time_ = response_time,
        .now_ = now,
    });
}

template <http_header_field_source headers_type>
[[nodiscard]] std::uint64_t compute_http_cache_age(const headers_type& response_headers,
    std::time_t request_time, std::time_t response_time, std::time_t now) noexcept {
    return detail::compute_http_cache_age_headers(
        response_headers, request_time, response_time, now);
}

// RFC 9110 section 15.4.5 and RFC 9111 section 4.3.4: representation metadata
// and framing fields that must not be emitted in a 304 Not Modified response or
// overwrite stored representation headers during 304 cache freshening.
[[nodiscard]] bool is_http_304_forbidden_header(std::string_view field_name) noexcept;

void sanitize_http_304_response_headers(std::vector<http_header>& headers);
void sanitize_http_304_response_headers(http_response_headers& headers);
void sanitize_http_304_response_headers(http_response& response);

template <http_header_field_source headers_type>
void merge_http_304_response_headers(
    std::vector<http_header>& cached_headers, const headers_type& not_modified_headers) {
    if constexpr (std::same_as<std::remove_cvref_t<headers_type>, std::vector<http_header>>) {
        if (&cached_headers == &not_modified_headers) {
            auto copy = not_modified_headers;
            detail::merge_http_304_response_headers_into(cached_headers, copy);
            return;
        }
    }
    detail::merge_http_304_response_headers_into(cached_headers, not_modified_headers);
}
template <http_header_field_source headers_type>
void merge_http_304_response_headers(
    http_response_headers& cached_headers, const headers_type& not_modified_headers) {
    if constexpr (std::same_as<std::remove_cvref_t<headers_type>, http_response_headers>) {
        if (&cached_headers == &not_modified_headers) {
            auto copy = not_modified_headers.clone();
            detail::merge_http_304_response_headers_into(cached_headers, copy);
            return;
        }
    }
    detail::merge_http_304_response_headers_into(cached_headers, not_modified_headers);
}
void merge_http_304_response_headers(
    http_response& cached_response, const http_response& not_modified_response);

struct http_conditional_method_plan final {
    bool evaluates_preconditions_;
    bool uses_not_modified_response_;
    bool evaluates_if_modified_since_;
    bool evaluates_range_;
};

[[nodiscard]] inline constexpr http_conditional_method_plan get_http_conditional_method_plan(
    http_known_method method) noexcept {
    switch (method) {
        case http_known_method::get:
            return {true, true, true, true};
        case http_known_method::head:
            return {true, true, true, false};
        case http_known_method::post:
        case http_known_method::put:
        case http_known_method::delete_value:
        case http_known_method::patch:
            return {true, false, false, false};
        case http_known_method::options:
        case http_known_method::connect:
        case http_known_method::unknown:
            return {false, false, false, false};
    }
    return {false, false, false, false};
}

// Precondition and range fields borrowed from the request. Presence of If-Range
// is tracked separately because an empty value is still a present field.
struct http_conditional_headers final {
    std::string_view if_unmodified_since_;
    std::string_view if_modified_since_;
    std::string_view range_;
    std::string_view if_range_;
    bool has_if_range_;
};

// Repeated If-Modified-Since and If-Unmodified-Since lines are ignored as
// invalid date conditions (RFC 9110 sections 13.1.3 and 13.1.4); their views
// are empty here rather than exposing the last line as an effective condition.
[[nodiscard]] http_conditional_headers get_http_conditional_headers(const http_request& request) noexcept;
http_conditional_headers get_http_conditional_headers(const http_request&&) = delete;

// One entity-tag precondition field's outcome, accumulated across every field
// line of that name. `valid` is false for a malformed field, which the caller
// must treat differently from a field that simply did not match. A valid
// wildcard is the only member across all accumulated field lines.
struct http_etag_field_condition final {
    bool present_{false};
    bool valid_{true};
    bool matched_{false};
    bool wildcard_{false};

    void update(std::string_view value, std::string_view expected, bool strong) noexcept;

    [[nodiscard]] bool matches() const noexcept {
        return valid_ && (wildcard_ || matched_);
    }
};

struct http_etag_preconditions final {
    http_etag_field_condition if_match_;
    http_etag_field_condition if_none_match_;
};

[[nodiscard]] http_etag_preconditions get_http_etag_preconditions(
    const http_request& request, std::string_view etag) noexcept;

// If-Modified-Since / If-Unmodified-Since: the "<=" comparisons of RFC 9110
// section 13.1.3 and 13.1.4. A malformed date is ignored.
[[nodiscard]] bool http_date_not_modified(
    std::string_view header_value, std::time_t modified_seconds) noexcept;

[[nodiscard]] bool http_date_unmodified(
    std::string_view header_value, std::time_t modified_seconds) noexcept;

// Whether an If-Range still authorises a range response. A date validator here
// must match EXACTLY, unlike If-Modified-Since.
[[nodiscard]] bool http_if_range_allows(std::string_view header_value, std::string_view etag,
    std::optional<std::time_t> modified_seconds, bool date_validator_strong) noexcept;

// Single-value RFC 9110 §8.8.2 entity-tag validation, comparison, and weakening.
[[nodiscard]] bool is_valid_http_entity_tag(std::string_view value) noexcept;
[[nodiscard]] bool is_weak_http_entity_tag(std::string_view value) noexcept;
[[nodiscard]] bool is_strong_http_entity_tag(std::string_view value) noexcept;
[[nodiscard]] bool http_strong_etag_equals(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] bool http_weak_etag_equals(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] std::string weaken_http_etag(std::string_view etag);

// Case-insensitive set of field/option tokens parsed from one or more
// Connection header lines (RFC 9110 section 7.6.1).
class http_connection_nominated_fields final {
public:
    http_connection_nominated_fields() noexcept = default;

    void update(std::string_view connection_field_value);
    template <http_header_field_source headers_type>
    void update_headers(const headers_type& headers) {
        update_header_range(headers);
    }

    [[nodiscard]] bool contains(std::string_view field_name) const noexcept;
    [[nodiscard]] constexpr bool has_close() const noexcept {
        return has_close_;
    }
    [[nodiscard]] constexpr bool has_keep_alive() const noexcept {
        return has_keep_alive_;
    }
    [[nodiscard]] constexpr bool has_upgrade() const noexcept {
        return has_upgrade_;
    }
    [[nodiscard]] constexpr bool has_te() const noexcept {
        return has_te_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return tokens_.empty();
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return tokens_.size();
    }
    [[nodiscard]] std::span<const std::string> tokens() const& noexcept RUVIA_LIFETIMEBOUND {
        return tokens_;
    }
    std::span<const std::string> tokens() const&& = delete;

    [[nodiscard]] std::vector<std::string> non_standard_options() const;

    [[nodiscard]] std::string rebuild_connection_field(
        bool retain_upgrade = false, bool include_non_standard = true) const;

private:
    void update_header_range(detail::http_header_range_view headers);

    std::vector<std::string> tokens_;
    bool has_close_{false};
    bool has_keep_alive_{false};
    bool has_upgrade_{false};
    bool has_te_{false};
};

[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(
    std::string_view connection_field_value);
template <http_header_field_source headers_type>
[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(
    const headers_type& headers) {
    http_connection_nominated_fields nominated;
    nominated.update_headers(headers);
    return nominated;
}

// Protocol hop-by-hop fields (RFC 9110 section 7.6.1): Connection, Keep-Alive,
// Proxy-Connection, TE, Transfer-Encoding, Trailer, Upgrade,
// Proxy-Authenticate, and Proxy-Authorization.
[[nodiscard]] bool is_http_hop_by_hop_field_name(std::string_view field_name) noexcept;

// Standard connection control/hop-by-hop tokens ("close", "keep-alive", and the
// protocol hop-by-hop field names).
[[nodiscard]] bool is_http_standard_connection_option(std::string_view option) noexcept;

[[nodiscard]] bool is_http_hop_by_hop_or_nominated_field(std::string_view field_name,
    const http_connection_nominated_fields& nominated, bool retain_upgrade = false) noexcept;

[[nodiscard]] bool is_http_hop_by_hop_trailer_field(std::string_view field_name,
    const http_connection_nominated_fields& nominated = http_connection_nominated_fields{}) noexcept;

[[nodiscard]] bool is_forbidden_http_request_trailer_field_name(std::string_view field_name) noexcept;
[[nodiscard]] bool is_forbidden_http_response_trailer_field_name(std::string_view field_name) noexcept;

struct http_cors_policy_config final {
    bool allow_any_origin_{false};
    // Exact WHATWG serialized origins ("https://example.com", "null"), "*", or
    // suffix domain patterns starting with "." or "*." (e.g. ".example.com").
    std::vector<std::string> allowed_origins_;
    // Domain suffixes matched on a label boundary (".example.com", "example.com",
    // "*.example.com", or scheme-qualified "https://.example.com").
    std::vector<std::string> allowed_origin_suffixes_;
    // Allowed preflight methods (e.g. "GET", "POST", "OPTIONS"), or "*" for any
    // valid method token.
    std::vector<std::string> allowed_methods_;
    // Allowed request header names, or "*" to reflect Access-Control-Request-Headers.
    std::vector<std::string> allowed_headers_;
    bool reflect_request_headers_{false};
    std::vector<std::string> expose_headers_;
    bool allow_credentials_{false};
    std::optional<std::chrono::seconds> max_age_{};
};

struct http_cors_preflight_input final {
    borrowed_text method_{"OPTIONS"};
    borrowed_text origin_{};
    borrowed_text access_control_request_method_{};
    borrowed_text access_control_request_headers_{};
};

class http_cors_preflight_result final {
public:
    http_cors_preflight_result() noexcept = default;

    http_cors_preflight_result(bool accepted, std::vector<http_header> headers) noexcept
        : headers_(std::move(headers)),
          accepted_(accepted) {}

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return accepted_;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return accepted_;
    }

    [[nodiscard]] std::span<const http_header> headers() const& noexcept RUVIA_LIFETIMEBOUND {
        return headers_;
    }
    std::span<const http_header> headers() const&& = delete;

    [[nodiscard]] std::vector<http_header> take_headers() && noexcept {
        accepted_ = false;
        return std::move(headers_);
    }

private:
    std::vector<http_header> headers_;
    bool accepted_{false};
};

// Stateless, pre-validated CORS policy primitive for per-request preflight
// evaluation and actual response header rewriting.
class http_cors_policy final {
public:
    explicit http_cors_policy(const http_cors_policy_config& config);

    [[nodiscard]] bool allow_any_origin() const noexcept {
        return allow_any_origin_;
    }
    [[nodiscard]] bool allow_credentials() const noexcept {
        return allow_credentials_;
    }
    [[nodiscard]] bool reflect_request_headers() const noexcept {
        return reflect_request_headers_;
    }
    [[nodiscard]] bool allows_origin(std::string_view origin) const noexcept;
    [[nodiscard]] bool allows_method(std::string_view method) const noexcept;
    [[nodiscard]] bool allows_request_header(std::string_view name) const noexcept;

    [[nodiscard]] http_cors_preflight_result evaluate_preflight(
        http_cors_preflight_input input) const;
    [[nodiscard]] http_cors_preflight_result evaluate_preflight(
        const http_request& request) const;

    // Removes controlled Access-Control-* headers from the existing response,
    // writes computed CORS headers when the origin is permitted, and merges
    // Vary tokens (such as Origin) without duplicates.
    bool rewrite_response_headers(
        borrowed_text request_origin, std::vector<http_header>& response_headers) const;
    bool rewrite_response_headers(
        borrowed_text request_origin, http_response_headers& response_headers) const;
    bool rewrite_response_headers(
        borrowed_text request_origin, http_response& response) const;
    // Applies server-side CORS headers (including OPTIONS preflight and Allow
    // promotion) without overwriting headers already set by the route handler.
    bool apply_response_headers(const http_request& request, http_response& response) const;

private:
    struct suffix_rule final {
        std::string scheme_;
        std::string host_suffix_;
        std::optional<std::uint16_t> port_{};
        bool subdomain_only_{false};
    };

    std::vector<std::string> exact_origins_;
    std::vector<suffix_rule> suffix_rules_;
    std::vector<std::string> allowed_methods_;
    std::string joined_allowed_methods_;
    std::vector<std::string> allowed_headers_;
    std::string joined_allowed_headers_;
    std::string joined_expose_headers_;
    std::string max_age_;
    bool allow_any_origin_{false};
    bool allow_any_method_{false};
    bool reflect_request_headers_{false};
    bool allow_credentials_{false};
};

[[nodiscard]] bool is_http_cors_controlled_response_header(std::string_view name) noexcept;

void merge_http_vary_tokens(
    std::vector<http_header>& headers, std::span<const std::string_view> tokens);
void merge_http_vary_token(std::vector<http_header>& headers, std::string_view token);

enum class http_transfer_coding : std::uint8_t { gzip,
    deflate };

// Parsing is bounded to prevent attacker-controlled unbounded state growth.
inline constexpr std::size_t max_transfer_codings = 8;

// A bounded inline sequence. It owns no memory resource, so a parsed framing
// plan may outlive the per-request storage its head was parsed from.
class http_transfer_codings final {
public:
    void push_back(http_transfer_coding coding) {
        if (size_ == max_transfer_codings) {
            throw std::length_error("too many transfer codings");
        }
        data_[size_++] = coding;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] http_transfer_coding* data() & noexcept RUVIA_LIFETIMEBOUND {
        return data_.data();
    }
    [[nodiscard]] const http_transfer_coding* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return data_.data();
    }
    http_transfer_coding* data() && = delete;
    const http_transfer_coding* data() const&& = delete;
    [[nodiscard]] http_transfer_coding* begin() & noexcept RUVIA_LIFETIMEBOUND {
        return data_.data();
    }
    [[nodiscard]] const http_transfer_coding* begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return data_.data();
    }
    http_transfer_coding* begin() && = delete;
    const http_transfer_coding* begin() const&& = delete;
    [[nodiscard]] http_transfer_coding* end() & noexcept RUVIA_LIFETIMEBOUND {
        return data_.data() + size_;
    }
    [[nodiscard]] const http_transfer_coding* end() const& noexcept RUVIA_LIFETIMEBOUND {
        return data_.data() + size_;
    }
    http_transfer_coding* end() && = delete;
    const http_transfer_coding* end() const&& = delete;
    [[nodiscard]] http_transfer_coding& operator[](std::size_t index) & noexcept RUVIA_LIFETIMEBOUND {
        return data_[index];
    }
    [[nodiscard]] const http_transfer_coding& operator[](std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return data_[index];
    }
    http_transfer_coding& operator[](std::size_t) && = delete;
    const http_transfer_coding& operator[](std::size_t) const&& = delete;

private:
    std::array<http_transfer_coding, max_transfer_codings> data_{};
    std::size_t size_{0};
};

enum class http_transfer_coding_decode_error : std::uint8_t {
    invalid_content,
    decoded_size_exceeded,
};
namespace detail {
class transfer_coding_decoder;
}

class http_transfer_coding_decode_need_input final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }

private:
    friend class http_transfer_coding_decode_result;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    explicit constexpr http_transfer_coding_decode_need_input(std::size_t consumed_bytes) noexcept
        : consumed_bytes_(consumed_bytes) {}
    std::size_t consumed_bytes_;
};

class http_transfer_coding_decode_output_view final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }
    [[nodiscard]] constexpr std::string_view bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return bytes_;
    }
    std::string_view bytes() const&& = delete;

private:
    friend class http_transfer_coding_decode_result;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    constexpr http_transfer_coding_decode_output_view(std::size_t consumed_bytes, std::string_view bytes_value) noexcept
        : consumed_bytes_(consumed_bytes),
          bytes_(bytes_value) {}
    std::size_t consumed_bytes_;
    std::string_view bytes_;
};

class http_transfer_coding_decode_complete final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }

private:
    friend class http_transfer_coding_decode_result;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    explicit constexpr http_transfer_coding_decode_complete(std::size_t consumed_bytes) noexcept
        : consumed_bytes_(consumed_bytes) {}
    std::size_t consumed_bytes_;
};

class http_transfer_coding_decode_failure final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }
    [[nodiscard]] constexpr http_transfer_coding_decode_error error() const noexcept {
        return error_;
    }

private:
    friend class http_transfer_coding_decode_result;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    constexpr http_transfer_coding_decode_failure(std::size_t consumed_bytes, http_transfer_coding_decode_error error) noexcept
        : consumed_bytes_(consumed_bytes),
          error_(error) {}
    std::size_t consumed_bytes_;
    http_transfer_coding_decode_error error_;
};

class http_transfer_coding_decoder_failure final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }

private:
    friend class http_transfer_coding_decode_result;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    explicit constexpr http_transfer_coding_decoder_failure(std::size_t consumed_bytes) noexcept
        : consumed_bytes_(consumed_bytes) {}
    std::size_t consumed_bytes_;
};

class http_transfer_coding_decode_result final {
public:
    [[nodiscard]] std::size_t consumed_bytes() const noexcept {
        return std::visit([](const auto& result_value) { return result_value.consumed_bytes(); }, value_);
    }
    [[nodiscard]] const http_transfer_coding_decode_need_input* need_input() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_transfer_coding_decode_need_input>(&value_);
    }
    const http_transfer_coding_decode_need_input* need_input() const&& = delete;
    [[nodiscard]] const http_transfer_coding_decode_output_view* output() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_transfer_coding_decode_output_view>(&value_);
    }
    const http_transfer_coding_decode_output_view* output() const&& = delete;
    [[nodiscard]] const http_transfer_coding_decode_complete* complete() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_transfer_coding_decode_complete>(&value_);
    }
    const http_transfer_coding_decode_complete* complete() const&& = delete;
    [[nodiscard]] const http_transfer_coding_decode_failure* failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_transfer_coding_decode_failure>(&value_);
    }
    const http_transfer_coding_decode_failure* failure() const&& = delete;
    [[nodiscard]] const http_transfer_coding_decoder_failure* decoder_failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_transfer_coding_decoder_failure>(&value_);
    }
    const http_transfer_coding_decoder_failure* decoder_failure() const&& = delete;

private:
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    using value_type = std::variant<http_transfer_coding_decode_need_input, http_transfer_coding_decode_output_view, http_transfer_coding_decode_complete, http_transfer_coding_decode_failure, http_transfer_coding_decoder_failure>;
    template <typename result_type>
    explicit http_transfer_coding_decode_result(result_type result_value) noexcept
        : value_(std::move(result_value)) {}
    value_type value_;
};

// The sole transfer-decoding entry point for one or more codings. Copies the
// protocol-order sequence during construction and decodes it in reverse.
// Input is never retained; output views borrow the caller's scratch storage.
// Consume each view before reusing it. Keep the decoder at a stable address
// until destruction. Drain output with decode({}, scratch)
// before requesting more input; finish_input() commits framing EOF only after
// all input/output is drained. Each layer enforces decoded_limit independently.
// A layer that received no coded bytes before framing EOF completes as empty.
// decode/finish_input return typed failures because incremental drivers must
// retain the exact wire consumption even when a layer fails; failure is terminal.
class http_transfer_coding_stack_decoder final {
public:
    http_transfer_coding_stack_decoder(
        std::span<const http_transfer_coding> codings, protocol_byte_limit decoded_limit);
    ~http_transfer_coding_stack_decoder();
    http_transfer_coding_stack_decoder(const http_transfer_coding_stack_decoder&) = delete;
    http_transfer_coding_stack_decoder& operator=(const http_transfer_coding_stack_decoder&) = delete;
    http_transfer_coding_stack_decoder(http_transfer_coding_stack_decoder&&) = delete;
    http_transfer_coding_stack_decoder& operator=(http_transfer_coding_stack_decoder&&) = delete;
    [[nodiscard]] http_transfer_coding_decode_result decode(
        std::string_view input, std::span<char> output) noexcept;
    [[nodiscard]] http_transfer_coding_decode_result finish_input() noexcept;

private:
    struct impl_type;
    impl_type* impl_{nullptr};
};

inline constexpr std::string_view http1_chunk_data_terminator = "\r\n";
inline constexpr std::string_view http1_last_chunk_prefix = "0\r\n";
inline constexpr std::string_view http1_trailer_section_terminator = "\r\n";

// The HTTP/1 chunk-size line, including its terminating CRLF. The caller owns
// framing decisions; this value only formats one chunk's wire prefix.
class http1_chunk_header final {
public:
    explicit http1_chunk_header(std::size_t chunk_size) noexcept {
        auto* cursor_value = storage_.data() + storage_.size();
        *--cursor_value = '\n';
        *--cursor_value = '\r';
        do {
            *--cursor_value = http_lower_hex_digit(static_cast<int>(chunk_size & 0x0fU));
            chunk_size >>= 4U;
        } while (chunk_size != 0);
        offset_ = static_cast<std::size_t>(cursor_value - storage_.data());
    }

    [[nodiscard]] std::string_view view() const& noexcept {
        return std::string_view(storage_.data() + offset_, storage_.size() - offset_);
    }
    [[nodiscard]] std::string_view view() const&& = delete;

private:
    // Construction writes every byte exposed by view(), plus the final offset.
    std::array<char, sizeof(std::size_t) * 2 + 2> storage_;
    std::size_t offset_;
};

// A request-body failure detected by a server driver while enforcing the
// HTTP-owned content contract. The driver supplies runtime byte limits and I/O
// completion facts; HTTP owns their final response status and diagnostic.
enum class http_request_body_failure : std::uint8_t {
    too_large,
    incomplete,
    buffer_limit,
};

[[nodiscard]] inline http_protocol_error http_request_body_protocol_error(
    http_request_body_failure failure) noexcept {
    switch (failure) {
        case http_request_body_failure::too_large:
            return http_protocol_error(
                http_status::content_too_large, "request_body_too_large", "request body is too large");
        case http_request_body_failure::incomplete:
            return http_protocol_error(
                http_status::bad_request, "incomplete_request_body", "incomplete request body");
        case http_request_body_failure::buffer_limit:
            return http_protocol_error(http_status::service_unavailable, "inbound_buffer_limit",
                "inbound buffer capacity exhausted");
    }
    return http_protocol_error(http_status::bad_request, "invalid_request_body", "invalid request body");
}

[[nodiscard]] inline http_protocol_error http_request_transfer_coding_error(
    http_transfer_coding_decode_error error) noexcept {
    switch (error) {
        case http_transfer_coding_decode_error::invalid_content:
            return http_protocol_error(http_status::bad_request, "invalid_transfer_coding_body",
                "invalid transfer-coding body");
        case http_transfer_coding_decode_error::decoded_size_exceeded:
            return http_request_body_protocol_error(http_request_body_failure::too_large);
    }
    return http_protocol_error(
        http_status::bad_request, "invalid_transfer_coding_body", "invalid transfer-coding body");
}

// Status for a request body whose Content-Encoding could not be decoded.
[[nodiscard]] inline http_protocol_error http_request_content_decode_error(
    http_content_decode_error error) noexcept {
    switch (error) {
        case http_content_decode_error::unsupported_coding:
            return http_protocol_error(http_status::unsupported_media_type,
                "unsupported_content_coding", "request Content-Encoding is not supported");
        case http_content_decode_error::invalid_content:
            return http_protocol_error(
                http_status::bad_request, "invalid_content_coding", "failed to decode request body");
        case http_content_decode_error::decoded_size_exceeded:
            return http_request_body_protocol_error(http_request_body_failure::too_large);
    }
    return http_protocol_error(
        http_status::bad_request, "invalid_content_coding", "failed to decode request body");
}

[[nodiscard]] inline std::optional<http_request_body_failure> http_request_body_size_failure(
    std::size_t size, protocol_byte_limit limit) noexcept {
    return limit.exceeds(size)
               ? std::optional<http_request_body_failure>(http_request_body_failure::too_large)
               : std::nullopt;
}

[[nodiscard]] inline std::optional<http_request_body_failure> http_request_body_addition_failure(
    std::size_t current_size, std::size_t additional_size, protocol_byte_limit limit) noexcept {
    return limit.addition_exceeds(current_size, additional_size)
               ? std::optional<http_request_body_failure>(http_request_body_failure::too_large)
               : std::nullopt;
}

enum class http1_chunk_decode_error : std::uint8_t {
    invalid_framing,
    body_limit_exceeded,
    framing_limit_exceeded,
};
class http1_chunk_decode_need_more final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }

private:
    friend class http1_chunk_decode_result;
    explicit constexpr http1_chunk_decode_need_more(std::size_t consumed_bytes) noexcept
        : consumed_bytes_(consumed_bytes) {}
    std::size_t consumed_bytes_;
};

class http1_chunk_decode_body_chunk_view final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }
    [[nodiscard]] constexpr std::string_view bytes() const& noexcept {
        return bytes_;
    }
    std::string_view bytes() const&& = delete;

private:
    friend class http1_chunk_decode_result;
    constexpr http1_chunk_decode_body_chunk_view(std::size_t consumed_bytes, std::string_view bytes_value) noexcept
        : consumed_bytes_(consumed_bytes),
          bytes_(bytes_value) {}
    std::size_t consumed_bytes_;
    std::string_view bytes_;
};

class http1_chunk_decode_complete_view final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }
    // The received trailer section, empty when there was none. The fields
    // borrow the decoder that produced this result (see
    // http1_chunked_body_decoder).
    [[nodiscard]] constexpr std::span<const http_header> trailers() const& noexcept {
        return trailers_;
    }
    std::span<const http_header> trailers() const&& = delete;

private:
    friend class http1_chunk_decode_result;
    explicit constexpr http1_chunk_decode_complete_view(
        std::size_t consumed_bytes, std::span<const http_header> trailers) noexcept
        : consumed_bytes_(consumed_bytes),
          trailers_(trailers) {}
    std::size_t consumed_bytes_;
    std::span<const http_header> trailers_;
};

class http1_chunk_decode_failure final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }
    [[nodiscard]] constexpr http1_chunk_decode_error error() const noexcept {
        return error_;
    }

private:
    friend class http1_chunk_decode_result;
    constexpr http1_chunk_decode_failure(
        std::size_t consumed_bytes, http1_chunk_decode_error error) noexcept
        : consumed_bytes_(consumed_bytes),
          error_(error) {}
    std::size_t consumed_bytes_;
    http1_chunk_decode_error error_;
};

class http1_chunk_decode_result final {
public:
    [[nodiscard]] std::size_t consumed_bytes() const noexcept {
        return std::visit([](const auto& result_value) { return result_value.consumed_bytes(); }, value_);
    }
    [[nodiscard]] const http1_chunk_decode_need_more* need_more() const& noexcept {
        return std::get_if<http1_chunk_decode_need_more>(&value_);
    }
    const http1_chunk_decode_need_more* need_more() const&& = delete;
    [[nodiscard]] const http1_chunk_decode_body_chunk_view* body_chunk() const& noexcept {
        return std::get_if<http1_chunk_decode_body_chunk_view>(&value_);
    }
    const http1_chunk_decode_body_chunk_view* body_chunk() const&& = delete;
    [[nodiscard]] const http1_chunk_decode_complete_view* complete() const& noexcept {
        return std::get_if<http1_chunk_decode_complete_view>(&value_);
    }
    const http1_chunk_decode_complete_view* complete() const&& = delete;
    [[nodiscard]] const http1_chunk_decode_failure* failure() const& noexcept {
        return std::get_if<http1_chunk_decode_failure>(&value_);
    }
    const http1_chunk_decode_failure* failure() const&& = delete;

private:
    friend class http1_chunked_body_decoder;
    using value_type = std::variant<http1_chunk_decode_need_more, http1_chunk_decode_body_chunk_view,
        http1_chunk_decode_complete_view, http1_chunk_decode_failure>;
    template <typename result_type>
    explicit http1_chunk_decode_result(result_type result_value) noexcept
        : value_(std::move(result_value)) {}
    [[nodiscard]] static http1_chunk_decode_result make_need_more(std::size_t consumed_bytes) noexcept;
    [[nodiscard]] static http1_chunk_decode_result make_body_chunk(
        std::size_t consumed_bytes, std::string_view bytes) noexcept;
    [[nodiscard]] static http1_chunk_decode_result make_complete(
        std::size_t consumed_bytes, std::span<const http_header> trailers = {}) noexcept;
    [[nodiscard]] static http1_chunk_decode_result make_failure(
        std::size_t consumed_bytes, http1_chunk_decode_error error) noexcept;
    value_type value_;
};

// How a received trailer section treats obs-fold (RFC 9112 section 5.2). A
// server may reject a request carrying it (the default); a user agent must
// replace each obs-fold in a response with SP before interpreting the value,
// so a response decoder accepts it. An accepted fold is validated, against the
// field syntax and the section budget, and published as its HTTP unfolding:
// the obs-fold and adjacent OWS become one SP.
enum class http1_trailer_obs_fold : std::uint8_t {
    reject,
    accept,
};

// The message whose trailer section a decoder receives. A received field its
// sender was not permitted to generate as a trailer of that message (RFC 9110
// section 6.5.1) is not a framing error; it is discarded rather than published
// (RFC 9112 section 7.1.2).
enum class http1_trailer_message : std::uint8_t {
    request,
    response,
};

struct http1_chunked_body_decoder_config final {
    protocol_byte_limit body_limit_{protocol_byte_limit::unlimited()};
    http1_trailer_obs_fold obs_fold_{http1_trailer_obs_fold::reject};
    http1_trailer_message trailer_message_{http1_trailer_message::request};
};

// Incremental sans-I/O HTTP/1 chunk framing decoder. Payload views borrow the
// supplied input and remain valid only until it is modified;
// incomplete framing stays unconsumed for the caller to retain. The body limit
// counts chunk payload bytes, which may still be transfer-encoded.
// RFC 9112 places no limit on the chunk count, and a zero-size chunk ends the
// body, so empty chunks cannot repeat. Each chunk-size line (including
// extensions) and the encoded trailer section (including its final empty line)
// are bounded independently by max_http_header_bytes. Searches stop at those
// caps, and reaching a cap with no terminator reports framing_limit_exceeded
// without inspecting later bytes. Size-line bytes beyond the shortest hex size
// (leading zeros and chunk-ext) also share one per-message budget of
// max_http_header_bytes (RFC 9112 section 7.1.1); exceeding it reports
// framing_limit_exceeded. Malformed framing within the permitted prefix still
// reports invalid_framing.
// Trailer sections must also fit the http_field_section_budget byte rule
// (name.size() + value.size() + 4 per field, plus 2 for the terminating empty
// line; see http_trailer_section_budget()). obs-fold in the trailer section
// follows http1_chunked_body_decoder_config::obs_fold_, and fields the sender
// was not permitted to generate follow trailer_message_.
// The complete view publishes the remaining fields in received order, names
// lowercase and values without surrounding OWS. The decoder owns them: storage
// is allocated only for a section that keeps a
// field, and the view stays valid until the next decode() call, or until the
// decoder is moved from, assigned to or destroyed.
class http1_chunked_body_decoder final {
public:
    explicit http1_chunked_body_decoder(http1_chunked_body_decoder_config config = {});
    ~http1_chunked_body_decoder();
    http1_chunked_body_decoder(const http1_chunked_body_decoder&) = delete;
    http1_chunked_body_decoder& operator=(const http1_chunked_body_decoder&) = delete;
    http1_chunked_body_decoder(http1_chunked_body_decoder&&) noexcept;
    http1_chunked_body_decoder& operator=(http1_chunked_body_decoder&&) noexcept;

    [[nodiscard]] http1_chunk_decode_result decode(std::string_view available);
    [[nodiscard]] http1_chunk_decode_result decode(std::string_view available, std::size_t max_body_bytes);
    template <detail::http_temporary_owning_char_string input_type>
    http1_chunk_decode_result decode(input_type&&) = delete;
    template <detail::http_temporary_owning_char_string input_type>
    http1_chunk_decode_result decode(input_type&&, std::size_t) = delete;

private:
    enum class progress : std::uint8_t {
        size_line,
        body,
        delimiter,
        trailers,
        complete,
    };

    [[nodiscard]] http1_chunk_decode_result fail(std::size_t consumed_bytes, http1_chunk_decode_error error) noexcept;
    [[nodiscard]] bool consume_delimiter(std::string_view available, std::size_t& cursor) noexcept;
    [[nodiscard]] bool publish_trailers(std::string_view trailers);

    protocol_byte_limit body_limit_;
    http1_trailer_obs_fold obs_fold_;
    // A failed decoder keeps reporting its error without consuming input.
    std::variant<progress, http1_chunk_decode_error> state_{progress::size_line};
    // Bytes of the pending size line or trailer section already scanned for LF.
    std::size_t size_line_search_offset_{0};
    std::size_t trailer_search_offset_{0};
    std::size_t remaining_{0};
    std::size_t decoded_bytes_{0};
    // Charged size-line bytes beyond the shortest hex size; never exceeds
    // max_http_header_bytes.
    std::size_t framing_overhead_bytes_{0};
    http1_trailer_message trailer_message_;
    std::vector<http_header> trailers_;
};

[[nodiscard]] inline http_protocol_error http_request_chunk_decode_error(
    http1_chunk_decode_error error) noexcept {
    switch (error) {
        case http1_chunk_decode_error::invalid_framing:
            return http_protocol_error(
                http_status::bad_request, "invalid_chunked_body", "invalid chunked request body");
        case http1_chunk_decode_error::body_limit_exceeded:
            return http_request_body_protocol_error(http_request_body_failure::too_large);
        case http1_chunk_decode_error::framing_limit_exceeded:
            return http_protocol_error(http_status::content_too_large, "chunked_framing_too_large",
                "request body framing is too large");
    }
    return http_protocol_error(
        http_status::bad_request, "invalid_chunked_body", "invalid chunked request body");
}

class http1_server_request_parse_state;

// Whether the runtime consumed the complete request message before attempting
// to reuse its transport. This is a protocol lifecycle fact, not a product
// policy bool: RFC 9112 section 9.3 forbids reuse with unread request content.
enum class http1_request_body_consumption : std::uint8_t { complete,
    incomplete };

class http1_request_without_body final {
private:
    friend class http1_request_body_plan;
    constexpr http1_request_without_body() noexcept = default;
};

class http1_known_length_request_body final {
public:
    [[nodiscard]] constexpr std::size_t content_length() const noexcept {
        return content_length_;
    }

private:
    friend class http1_request_body_plan;

    explicit constexpr http1_known_length_request_body(std::size_t content_length) noexcept
        : content_length_(content_length) {}

    std::size_t content_length_;
};

class http1_chunked_request_body final {
public:
    [[nodiscard]] const http_transfer_codings& transfer_codings() const& noexcept RUVIA_LIFETIMEBOUND {
        return transfer_codings_;
    }
    const http_transfer_codings& transfer_codings() const&& = delete;

private:
    friend class http1_request_body_plan;

    explicit http1_chunked_request_body(http_transfer_codings transfer_codings)
        : transfer_codings_(std::move(transfer_codings)) {}

    http_transfer_codings transfer_codings_;
};

// Immutable framing contract produced only by the HTTP/1 parser and consumed
// by any runtime driver. Content-Length belongs only to the known-length
// alternative; transfer-coding order belongs only to the final-chunked
// alternative. A caller cannot synthesize a plan that bypasses wire validation.
class http1_request_body_plan final {
public:
    [[nodiscard]] constexpr const http1_request_without_body* without_body() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_request_without_body>(&framing_);
    }
    [[nodiscard]] constexpr const http1_request_without_body* without_body() const&& = delete;

    [[nodiscard]] constexpr const http1_known_length_request_body* known_length() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_known_length_request_body>(&framing_);
    }
    [[nodiscard]] constexpr const http1_known_length_request_body* known_length() const&& = delete;

    [[nodiscard]] constexpr const http1_chunked_request_body* chunked() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_chunked_request_body>(&framing_);
    }
    [[nodiscard]] constexpr const http1_chunked_request_body* chunked() const&& = delete;

    // Chunked framing requires consuming the terminating zero chunk even when
    // the decoded content is empty.
    [[nodiscard]] constexpr bool requires_consumption() const noexcept {
        if (const auto* known = known_length()) {
            return known->content_length() != 0;
        }
        return chunked() != nullptr;
    }

    // The consumption a response must record when the runtime never read the
    // request body: incomplete only if there is anything to read.
    [[nodiscard]] constexpr http1_request_body_consumption consumption_if_unread() const noexcept {
        return requires_consumption() ? http1_request_body_consumption::incomplete
                                      : http1_request_body_consumption::complete;
    }

    [[nodiscard]] http_request_expectations expectations() const noexcept {
        return expectations_;
    }

    [[nodiscard]] http_server_expectation_plan expectation_plan(
        http_unsupported_expectation_policy unsupported_policy) const noexcept {
        return expectations_.server_plan(requires_consumption()
                                             ? http_request_content_indication::will_follow
                                             : http_request_content_indication::no_content,
            unsupported_policy);
    }

private:
    friend class http1_server_request_parse_state;

    using framing_type =
        std::variant<http1_request_without_body, http1_known_length_request_body, http1_chunked_request_body>;

    explicit http1_request_body_plan(http_request_expectations expectations) noexcept
        : http1_request_body_plan(framing_type(http1_request_without_body()), expectations) {}

    http1_request_body_plan(std::size_t content_length, http_request_expectations expectations) noexcept
        : http1_request_body_plan(framing_type(http1_known_length_request_body(content_length)), expectations) {}

    http1_request_body_plan(
        http_transfer_codings transfer_codings, http_request_expectations expectations)
        : http1_request_body_plan(framing_type(http1_chunked_request_body(std::move(transfer_codings))), expectations) {}

    http1_request_body_plan(framing_type framing, http_request_expectations expectations)
        : framing_(std::move(framing)),
          expectations_(expectations) {}

    framing_type framing_;
    http_request_expectations expectations_;
};

// Whether a connection is reused after a completed HTTP/1 exchange. Both the
// client request writer and the server response planner express the same wire
// decision with this single shared policy: allow_reuse keeps the connection
// poolable, close_after_response emits Connection: close and closes it.
enum class http1_close_policy : std::uint8_t {
    allow_reuse,
    close_after_response,
};
// Immutable request-side HTTP/1 persistence contract. Parsing establishes the
// version and initial disposition; later body-consumption policy may only close.
class http1_request_connection_plan final {
public:
    [[nodiscard]] static constexpr http1_request_connection_plan http11_close() noexcept {
        return {http_protocol_version::http11, http1_close_policy::close_after_response, false};
    }

    [[nodiscard]] constexpr http_protocol_version protocol_version() const noexcept {
        return version_;
    }
    [[nodiscard]] constexpr http1_close_policy disposition() const noexcept {
        return disposition_;
    }
    [[nodiscard]] constexpr http1_request_connection_plan require_close() const noexcept {
        return {version_, http1_close_policy::close_after_response, tunnel_request_};
    }

    // A CONNECT request. A 2xx final response turns the connection into a
    // tunnel; any other final response must close it, because a client may
    // already have sent tunnel bytes optimistically (RFC 9110 9.3.6) and they
    // must never be parsed as further requests.
    [[nodiscard]] constexpr bool tunnel_request() const noexcept {
        return tunnel_request_;
    }
    [[nodiscard]] constexpr http1_request_connection_plan with_tunnel_request() const noexcept {
        return {version_, disposition_, true};
    }

private:
    friend constexpr http1_request_connection_plan plan_http10_request_connection(bool, bool) noexcept;
    friend constexpr http1_request_connection_plan plan_http11_request_connection(bool) noexcept;

    constexpr http1_request_connection_plan(http_protocol_version version,
        http1_close_policy disposition, bool tunnel_request) noexcept
        : version_(version),
          disposition_(disposition),
          tunnel_request_(tunnel_request) {}

    http_protocol_version version_;
    http1_close_policy disposition_;
    bool tunnel_request_;
};

[[nodiscard]] inline constexpr http1_request_connection_plan plan_http10_request_connection(
    bool close, bool keep_alive) noexcept {
    return {http_protocol_version::http10,
        !close && keep_alive ? http1_close_policy::allow_reuse : http1_close_policy::close_after_response, false};
}

[[nodiscard]] inline constexpr http1_request_connection_plan plan_http11_request_connection(
    bool close) noexcept {
    return {http_protocol_version::http11,
        close ? http1_close_policy::close_after_response : http1_close_policy::allow_reuse, false};
}

[[nodiscard]] inline constexpr http1_request_connection_plan apply_request_body_consumption(
    http1_request_connection_plan plan, http1_request_body_consumption consumption) noexcept {
    return consumption == http1_request_body_consumption::complete ? plan : plan.require_close();
}

enum class http1_server_request_parse_failure_source : std::uint8_t { request_line,
    message };

// Cleartext listeners can silently discard inputs whose request-line failure
// shows the peer was not speaking HTTP (for example, TLS bytes on an HTTP
// port). Input whose first line (after ignored empty lines; the available
// bytes while no CRLF has arrived) has a whitespace-separated word naming an
// HTTP version ("HTTP/" in any case, or token "/" DIGITs "." DIGITs) is HTTP
// and is answered with 400 (RFC 9112 sections 2.2 and 3). Otherwise input is
// dropped once its first line is CRLF-terminated, or at once when its first
// byte cannot begin a method token.
[[nodiscard]] bool should_drop_invalid_cleartext_http1_input(
    std::string_view buffer, http1_server_request_parse_failure_source source_value) noexcept;

class http1_server_need_request_head final {
private:
    friend class http1_server_request_parse_state;

    // Resumable scan progress: leading empty-line bytes already skipped and,
    // once accepted, the end of the request-line. Each input byte is thereby
    // classified once across repeated parse_head calls.
    std::size_t leading_bytes_{0};
    std::size_t request_line_end_{0};
};

class http1_server_request_head_ready final {
public:
    [[nodiscard]] constexpr std::size_t header_bytes() const noexcept {
        return header_bytes_;
    }

private:
    friend class http1_server_request_parse_state;

    explicit constexpr http1_server_request_head_ready(std::size_t header_bytes) noexcept
        : header_bytes_(header_bytes) {
        if (header_bytes_ == 0) {
            std::terminate();
        }
    }

    std::size_t header_bytes_;
};

class http1_server_request_parse_failure final {
public:
    [[nodiscard]] http_protocol_error protocol_error() const noexcept {
        return http_parse_protocol_error(error_);
    }

    [[nodiscard]] constexpr http1_server_request_parse_failure_source source() const noexcept {
        return error_ == http_parse_error::invalid_request_line ||
                       error_ == http_parse_error::unsupported_http_version
                   ? http1_server_request_parse_failure_source::request_line
                   : http1_server_request_parse_failure_source::message;
    }

private:
    friend class http1_server_request_parse_state;

    explicit constexpr http1_server_request_parse_failure(http_parse_error error) noexcept
        : error_(error) {}

    http_parse_error error_;
};

class http1_server_request_parse_state final {
public:
    http1_server_request_parse_state() noexcept;

    // 分配失败以 std::bad_alloc 传出且不发布 head_ready/成功状态；释放资源后可用同一 buffer 与 offset 重试，但部分元数据可能残留。
    void parse_head(std::string_view buffer, std::size_t header_search_offset = 0);

    template <detail::http_temporary_owning_char_string buffer_type>
    void parse_head(buffer_type&&, std::size_t = 0) = delete;
    [[nodiscard]] const http1_server_need_request_head* need_request_head() const& noexcept {
        return std::get_if<http1_server_need_request_head>(&progress_);
    }
    [[nodiscard]] const http1_server_need_request_head* need_request_head() const&& = delete;

    [[nodiscard]] const http1_server_request_head_ready* head_ready() const& noexcept {
        return std::get_if<http1_server_request_head_ready>(&progress_);
    }
    [[nodiscard]] const http1_server_request_head_ready* head_ready() const&& = delete;

    [[nodiscard]] const http1_server_request_parse_failure* failure() const& noexcept {
        return std::get_if<http1_server_request_parse_failure>(&progress_);
    }
    [[nodiscard]] const http1_server_request_parse_failure* failure() const&& = delete;

    // The request, body plan and connection plan parse_head() derived from
    // its last input. A failure keeps the accepted request version and a
    // connection plan that requires close; an incomplete head keeps an empty
    // request.
    [[nodiscard]] const http_request& request() const& noexcept RUVIA_LIFETIMEBOUND {
        return request_;
    }
    const http_request& request() const&& = delete;
    [[nodiscard]] const http1_request_body_plan& body_plan() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_plan_;
    }
    const http1_request_body_plan& body_plan() const&& = delete;
    [[nodiscard]] const http1_request_connection_plan& connection_plan() const& noexcept RUVIA_LIFETIMEBOUND {
        return connection_plan_;
    }
    const http1_request_connection_plan& connection_plan() const&& = delete;

    // Each extraction moves one result out and leaves the default value
    // behind; the state then serves only parse_head() or destruction. Taking
    // the request releases its descriptor storage.
    [[nodiscard]] http_request take_request() && noexcept;
    [[nodiscard]] http1_request_body_plan take_body_plan() && noexcept;
    [[nodiscard]] http1_request_connection_plan take_connection_plan() && noexcept;

private:
    http_request request_;
    http1_request_body_plan body_plan_{http1_request_body_plan(http_request_expectations{})};
    http1_request_connection_plan connection_plan_{http1_request_connection_plan::http11_close()};
    // The compact request owns its descriptor block across parse attempts.
    // Only this small progress value changes, so scan/head/error metadata
    // exists solely in the alternative where it is meaningful.
    using progress = std::variant<http1_server_need_request_head, http1_server_request_head_ready,
        http1_server_request_parse_failure>;

    progress progress_{http1_server_need_request_head{}};
};

// An HTTP/1 response head this endpoint cannot send: an interim, final,
// CONNECT or CONNECT-UDP head that breaks a message rule (RFC 9110, RFC 9112)
// or the head size bound. It reports an application or runtime bug, never a
// peer error, so a server answers it like any other handler failure.
class http1_response_head_error final : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

inline constexpr std::size_t response_head_stack_bytes = 512;
inline constexpr std::size_t response_head_retained_heap_bytes = std::size_t{4} * 1024;

// Reusable scratch storage for serialized HTTP response heads. Keeps small heads
// inline and retains only bounded heap capacity across requests.
class http_response_head_buffer final {
    struct stack_state_type final {
        std::size_t used_{0};
    };
    struct heap_state_type final {};

public:
    http_response_head_buffer() noexcept = default;

    http_response_head_buffer(const http_response_head_buffer& other);
    http_response_head_buffer(http_response_head_buffer&& other) noexcept;
    http_response_head_buffer& operator=(const http_response_head_buffer& other);
    http_response_head_buffer& operator=(http_response_head_buffer&& other) noexcept;
    void reset() noexcept;
    // value may alias storage, including a written range returned by stack_cursor().
    void append(std::string_view value);
    void append(char value);
    void append_unsigned(std::uint64_t value);
    // Reserve beyond the current size without spilling a fitting inline range.
    void reserve_additional(std::size_t size);
    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view view() const&& = delete;
    [[nodiscard]] bool can_append_on_stack(std::size_t size) const noexcept;

    template <typename writer_type>
        requires std::is_nothrow_invocable_r_v<void, writer_type&, char*>
    void append_generated(std::size_t size, writer_type&& writer) {
        if (char* cursor_value = stack_cursor(size); cursor_value != nullptr) {
            writer(cursor_value);
            commit_stack(cursor_value + size);
            return;
        }
        reserve_additional(size);
        const auto previous_size = heap_.size();
        heap_.resize(previous_size + size);
        writer(heap_.data() + previous_size);
    }

    // Returns uncommitted inline scratch at the current end if bound fits.
    // This is a capacity check, not a reservation; the buffer must stay alive
    // and unmoved, and intervening mutations may change the logical end.
    [[nodiscard]] char* stack_cursor(std::size_t bound) & noexcept RUVIA_LIFETIMEBOUND {
        auto* const stack_state = std::get_if<stack_state_type>(&state_);
        if (stack_state == nullptr || bound > stack_.size() - stack_state->used_) {
            return nullptr;
        }
        return stack_.data() + stack_state->used_;
    }
    [[nodiscard]] char* stack_cursor(std::size_t) && = delete;

    // Sets the inline length to end, including the array's one-past endpoint.
    // This may shorten existing content. A spilled buffer throws logic_error;
    // a null or foreign endpoint throws invalid_argument. Rejection preserves
    // the committed content. No pointer subtraction is performed on a foreign
    // array, even when an endpoint was taken from another live buffer.
    void commit_stack(const char* end) {
        auto* const stack_state = std::get_if<stack_state_type>(&state_);
        if (stack_state == nullptr) {
            throw std::logic_error("HTTP response head no longer uses inline storage");
        }
        const auto begin_address = reinterpret_cast<std::uintptr_t>(stack_.data());
        const auto end_address = reinterpret_cast<std::uintptr_t>(end);
        const auto offset = end_address - begin_address;
        if (end == nullptr || end_address < begin_address || offset > stack_.size()) {
            throw std::invalid_argument("HTTP response head endpoint is outside its inline storage");
        }
        stack_state->used_ = static_cast<std::size_t>(offset);
    }

private:
    void spill_to_heap(std::size_t min_capacity);
    void copy_committed_state(const http_response_head_buffer& other) noexcept;
    std::array<char, response_head_stack_bytes> stack_{};
    std::string heap_;
    std::variant<stack_state_type, heap_state_type> state_{stack_state_type{}};
};

class http1_buffered_response_plan;

// A complete buffered representation is length-delimited by the response writer.
class http1_buffered_response_head final {
public:
    [[nodiscard]] constexpr std::uint64_t content_length() const noexcept {
        return content_length_;
    }

private:
    friend class http1_response_head_plan;

    explicit constexpr http1_buffered_response_head(std::uint64_t content_length) noexcept
        : content_length_(content_length) {}

    std::uint64_t content_length_{0};
};

// The response writer owns canonical Transfer-Encoding: chunked and the runtime
// emits the matching chunk frames. Application headers cannot redefine framing.
class http1_chunked_response_stream_head final {
private:
    friend class http1_response_head_plan;

    constexpr http1_chunked_response_stream_head() noexcept = default;
};

// The response has no declared message-body length. When content is allowed, the
// runtime closes the connection to delimit it; application Content-Length and
// Transfer-Encoding fields therefore cannot survive into the wire head.
class http1_close_delimited_response_stream_head final {
private:
    friend class http1_response_head_plan;

    constexpr http1_close_delimited_response_stream_head() noexcept = default;
};

// Final HTTP/1 head framing is an exclusive protocol value, not the old
// "suppress automatic Content-Length" boolean. The body plan keeps method/status
// content semantics attached to the exact wire-framing alternative selected by
// the HTTP/1 planner.
class http1_response_head_plan final {
public:
    [[nodiscard]] constexpr const http1_buffered_response_head* buffered() const& noexcept {
        return std::get_if<http1_buffered_response_head>(&framing_);
    }
    [[nodiscard]] constexpr const http1_buffered_response_head* buffered() const&& = delete;

    [[nodiscard]] constexpr const http1_chunked_response_stream_head* chunked_stream() const& noexcept {
        return std::get_if<http1_chunked_response_stream_head>(&framing_);
    }
    [[nodiscard]] constexpr const http1_chunked_response_stream_head* chunked_stream() const&& = delete;

    [[nodiscard]] constexpr const http1_close_delimited_response_stream_head* close_delimited_stream()
        const& noexcept {
        return std::get_if<http1_close_delimited_response_stream_head>(&framing_);
    }
    [[nodiscard]] constexpr const http1_close_delimited_response_stream_head* close_delimited_stream()
        const&& = delete;

    [[nodiscard]] constexpr http_response_body_plan body_plan() const noexcept {
        return body_plan_;
    }

    [[nodiscard]] constexpr http_protocol_version protocol_version() const noexcept {
        return protocol_version_;
    }

private:
    friend http1_buffered_response_plan get_http1_buffered_response_plan(
        http_buffered_response_write_plan, http1_request_connection_plan) noexcept;
    friend constexpr http1_response_head_plan http1_chunked_response_stream_head_plan(
        http_response_body_plan, http1_request_connection_plan) noexcept;
    friend constexpr http1_response_head_plan http1_close_delimited_response_stream_head_plan(
        http_response_body_plan, http1_request_connection_plan) noexcept;

    using framing_type = std::variant<http1_buffered_response_head,
        http1_chunked_response_stream_head, http1_close_delimited_response_stream_head>;

    [[nodiscard]] static constexpr framing_type buffered_framing(std::uint64_t content_length) noexcept {
        return framing_type(http1_buffered_response_head(content_length));
    }

    [[nodiscard]] static constexpr framing_type chunked_stream_framing() noexcept {
        return framing_type(http1_chunked_response_stream_head());
    }

    [[nodiscard]] static constexpr framing_type close_delimited_stream_framing() noexcept {
        return framing_type(http1_close_delimited_response_stream_head());
    }

    constexpr http1_response_head_plan(http_response_body_plan body_plan,
        http_protocol_version protocol_version, framing_type framing) noexcept
        : body_plan_(body_plan),
          protocol_version_(protocol_version),
          framing_(framing) {}

    http_response_body_plan body_plan_;
    http_protocol_version protocol_version_;
    framing_type framing_;
};

[[nodiscard]] constexpr http1_response_head_plan http1_chunked_response_stream_head_plan(
    http_response_body_plan body_plan, http1_request_connection_plan connection_plan) noexcept {
    return http1_response_head_plan(
        body_plan, connection_plan.protocol_version(), http1_response_head_plan::chunked_stream_framing());
}

[[nodiscard]] constexpr http1_response_head_plan http1_close_delimited_response_stream_head_plan(
    http_response_body_plan body_plan, http1_request_connection_plan connection_plan) noexcept {
    return http1_response_head_plan(body_plan, connection_plan.protocol_version(),
        http1_response_head_plan::close_delimited_stream_framing());
}

// The runtime writes one inseparable HTTP/1 buffered response contract. The
// embedded head owns method/status/body semantics and representation length
// exactly once; direct fact access avoids restoring a parallel write-plan copy.
class http1_buffered_response_plan final {
public:
    [[nodiscard]] constexpr http_response_body_plan body_plan() const noexcept {
        return head_plan_.body_plan();
    }

    [[nodiscard]] http_status_code response_status() const noexcept {
        return body_plan().response_status();
    }

    [[nodiscard]] constexpr std::uint64_t content_length() const noexcept {
        return head_plan_.buffered()->content_length();
    }

    [[nodiscard]] bool send_body() const noexcept {
        return detail::http_buffered_response_sends_body(body_plan(), content_length());
    }

    [[nodiscard]] constexpr const http1_response_head_plan& head_plan() const& noexcept {
        return head_plan_;
    }
    [[nodiscard]] constexpr const http1_response_head_plan& head_plan() const&& = delete;

private:
    friend http1_buffered_response_plan get_http1_buffered_response_plan(
        http_buffered_response_write_plan, http1_request_connection_plan) noexcept;

    explicit constexpr http1_buffered_response_plan(http1_response_head_plan head_plan) noexcept
        : head_plan_(head_plan) {}

    http1_response_head_plan head_plan_;
};

static_assert(std::is_trivially_copyable_v<http1_buffered_response_plan>);
static_assert(sizeof(http1_buffered_response_plan) == sizeof(http1_response_head_plan));

[[nodiscard]] inline http1_buffered_response_plan get_http1_buffered_response_plan(
    http_buffered_response_write_plan write_plan, http1_request_connection_plan connection_plan) noexcept {
    return http1_buffered_response_plan(
        http1_response_head_plan(write_plan.body_plan(), connection_plan.protocol_version(),
            http1_response_head_plan::buffered_framing(write_plan.content_length())));
}

// Stable, non-detail entry points for HTTP response serialization plans. The
// plan implementations remain shared with the HTTP/1 and HTTP/2 protocol code.

namespace detail {

[[nodiscard]] bool is_forbidden_response_trailer_name(std::string_view name) noexcept;
[[nodiscard]] bool is_forbidden_http_binary_response_field(std::string_view name) noexcept;

}  // namespace detail

class http_response_trailer_section;

// Borrowed proof that the complete terminal section passed the shared response-
// trailer rules. The source span must outlive its synchronous consumption.
class http_response_trailer_section final {
public:
    [[nodiscard]] std::span<const http_header_view> fields() const noexcept {
        return fields_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return fields_.empty();
    }

private:
    friend http_response_trailer_section validate_http_response_trailers(
        std::span<const http_header_view>);
    explicit http_response_trailer_section(std::span<const http_header_view> fields_value) noexcept
        : fields_(fields_value) {}

    std::span<const http_header_view> fields_;
};

// Serialize a finalized HTTP/1 response head into caller-owned reusable scratch.
// The plan must describe the same response representation.
// Canonical Content-Length is measured and emitted from the same decimal token.
// An explicit Date is preserved without consulting the clock; otherwise a
// cached current Date is generated when a usable clock is available. A head
// that breaks a message rule or the head size bound throws
// http1_response_head_error. The plan owns framing: an application
// Transfer-Encoding is accepted only as exactly `chunked` (OWS and ASCII case
// aside) and is replaced by the plan's framing; any other value throws.
void append_http1_response_head(
    const http_response& response, http_response_head_buffer& head,
    const http1_response_head_plan& plan);

// Append only field lines; chunk delimiters belong to the caller. Inputs may
// alias output. Allocation failure leaves the existing output bytes unchanged.
void append_http1_response_trailers(
    std::string& output, const http_response_trailer_section& trailers);

// Validate a complete response trailer section and return a borrowed proof for
// synchronous protocol submission. The input storage must outlive its use.
[[nodiscard]] http_response_trailer_section validate_http_response_trailers(
    std::span<const http_header_view> trailers);

template <detail::http_temporary_owning_range<http_header_view> headers_type>
http_response_trailer_section validate_http_response_trailers(headers_type&&) = delete;
http_response_trailer_section validate_http_response_trailers(
    std::initializer_list<http_header_view>) = delete;

// Immutable, bodyless response-head view for 1xx progress messages that precede
// a final response. 101 is deliberately excluded: switching protocols transfers
// connection ownership and therefore requires its dedicated protocol driver.
// Header elements and their strings are borrowed and must remain stable through
// the synchronous sans-I/O submit call.
class http_interim_response_head final {
public:
    explicit http_interim_response_head(http_status_code status_code, borrowed_http_headers headers = {});

    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_code_;
    }

    [[nodiscard]] constexpr std::span<const http_header_view> headers() const noexcept {
        return headers_;
    }

private:
    http_status_code status_code_;
    std::span<const http_header_view> headers_;
};

// Connection lifecycle after the encoded interim response. RFC 9112 requires
// a sender of Connection: close to begin closing after the response containing
// that option, even though doing so leaves the request without a final response.
enum class http1_interim_connection_disposition : std::uint8_t {
    unchanged,
    close_after_interim_response,
};

class http1_interim_response_buffer_too_small final {
public:
    [[nodiscard]] constexpr std::size_t required_head_bytes() const noexcept {
        return required_head_bytes_;
    }

private:
    friend std::variant<class prepared_http1_interim_response, http1_interim_response_buffer_too_small>
    prepare_http1_interim_response_head(const http_interim_response_head&, std::span<char>);

    explicit constexpr http1_interim_response_buffer_too_small(std::size_t required_head_bytes) noexcept
        : required_head_bytes_(required_head_bytes) {}

    std::size_t required_head_bytes_;
};

// A transactionally encoded HTTP/1.1 interim head. The byte view points into
// the caller's output buffer. When Connection: close is present, the owner must
// initiate connection closure as soon as this interim head has been written.
class prepared_http1_interim_response final {
public:
    [[nodiscard]] constexpr std::string_view head() const noexcept {
        return head_;
    }

    [[nodiscard]] constexpr http1_interim_connection_disposition connection_disposition()
        const noexcept {
        return connection_disposition_;
    }

private:
    friend std::variant<prepared_http1_interim_response, http1_interim_response_buffer_too_small>
    prepare_http1_interim_response_head(const http_interim_response_head&, std::span<char>);

    constexpr prepared_http1_interim_response(
        std::string_view head, http1_interim_connection_disposition connection_disposition) noexcept
        : head_(head),
          connection_disposition_(connection_disposition) {}

    std::string_view head_;
    http1_interim_connection_disposition connection_disposition_;
};

// Allocation-free HTTP/1.1 interim-head writer. It validates the complete
// borrowed field set and exact 64-field/64-KiB bounds before touching the
// output buffer, throwing http1_response_head_error for a head it cannot send.
// No Server or Date field is injected: the encoded fields are exactly those
// represented by http_interim_response_head.
[[nodiscard]] std::variant<prepared_http1_interim_response, http1_interim_response_buffer_too_small>
prepare_http1_interim_response_head(const http_interim_response_head& response, std::span<char> head_buffer);

enum class http_response_stream_framing : std::uint8_t {
    http1_chunked,
    // RFC 9112 6.1: a server MUST NOT send Transfer-Encoding to a client that did
    // not indicate HTTP/1.1. An HTTP/1.0 stream therefore carries no chunk framing;
    // the body is delimited by the connection close (RFC 9112 6.3), so it also
    // announces Connection: close and the session shuts the socket afterwards.
    http1_close_delimited,
    http2_frames,
    // RFC 9114 §4.1: HTTP/3 delimits content with DATA frames and carries a
    // trailer section in a trailing HEADERS frame, exactly like HTTP/2.
    http3_frames
};
// A trailer section is terminal message metadata, not an independently queued
// side channel. The caller declares whether this head commit is reserving a
// terminal trailer section. Trailers are optional metadata a recipient may
// discard (RFC 9110 6.5), so framing that cannot carry them (HTTP/1.0
// close-delimited, known length, or a suppressed body) drops them; only a
// status that can never carry content rejects the intent before the head.
enum class http_response_trailer_intent : std::uint8_t { none,
    present };

// Whether a validated trailer section commits the response to sending trailers.
// An empty section is not "no decision": it is the decision to send none, which
// the head must state before any body byte goes out.
[[nodiscard]] inline http_response_trailer_intent response_trailer_intent(
    const http_response_trailer_section& section) noexcept {
    return section.empty() ? http_response_trailer_intent::none : http_response_trailer_intent::present;
}

// Authoritative phase immediately after the initial response head is submitted.
// Body-suppressed responses (HEAD / 304) complete at the initial head
// (message_ended) and discard trailers.
enum class http_response_stream_head_disposition : std::uint8_t { body_open,
    message_ended };

class http_response_stream_commit_plan final {
public:
    [[nodiscard]] http_status_code response_status() const noexcept {
        return body_plan_.response_status();
    }

    [[nodiscard]] http_response_stream_framing framing() const noexcept {
        return framing_;
    }

    [[nodiscard]] http_response_body_plan body_plan() const noexcept {
        return body_plan_;
    }

    // HTTP/1.1 requires chunked framing for trailers (RFC 9112 7.1.2).
    // HTTP/2 and HTTP/3 carry them in a trailing field section. A body-suppressed
    // response (HEAD / 304) ends at its initial head and discards trailers.
    [[nodiscard]] bool trailers_available() const noexcept {
        return !body_plan_.body_suppressed() &&
               (framing_ == http_response_stream_framing::http1_chunked ||
                   framing_ == http_response_stream_framing::http2_frames ||
                   framing_ == http_response_stream_framing::http3_frames);
    }

    [[nodiscard]] http_response_stream_head_disposition head_disposition() const noexcept {
        return body_plan_.body_suppressed() ? http_response_stream_head_disposition::message_ended
                                            : http_response_stream_head_disposition::body_open;
    }

    // A status that can never carry content (1xx/204/205/304) has no message
    // body to terminate with a trailer section; asking for one is a handler
    // error. Every other unavailable framing discards the trailers instead.
    [[nodiscard]] bool status_permits_trailers() const noexcept {
        return body_plan_.status_allows_body();
    }

    [[nodiscard]] bool trailer_intent_allowed() const noexcept {
        return !trailer_intent_present_ || status_permits_trailers();
    }

private:
    friend http_response_stream_commit_plan plan_http_response_stream_commit(
        http_response_stream_framing, http_known_method, http_status_code, http_response_trailer_intent) noexcept;

    http_response_stream_commit_plan(http_response_stream_framing framing, http_response_body_plan body_plan,
        bool trailer_intent_present) noexcept
        : framing_(framing),
          body_plan_(body_plan),
          trailer_intent_present_(trailer_intent_present) {}

    http_response_stream_framing framing_{http_response_stream_framing::http1_chunked};
    http_response_body_plan body_plan_;
    bool trailer_intent_present_{false};
};

[[nodiscard]] inline http_response_stream_commit_plan plan_http_response_stream_commit(
    http_response_stream_framing framing, http_known_method request_method, http_status_code response_status,
    http_response_trailer_intent trailer_intent) noexcept {
    const auto body_plan = plan_http_response_body(request_method, response_status);
    return http_response_stream_commit_plan(framing, body_plan,
        trailer_intent == http_response_trailer_intent::present);
}

// Preparation normalizes the initial response metadata to the supplied commit
// plan. Mutating the response afterward can invalidate that relationship; this
// object is not a permanent proof over a mutable response.
class http_response_stream_head final {
public:
    [[nodiscard]] http_response& response() & noexcept {
        return response_;
    }
    [[nodiscard]] http_response& response() && = delete;

    [[nodiscard]] const http_response& response() const& noexcept {
        return response_;
    }
    [[nodiscard]] const http_response& response() const&& = delete;

    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const& noexcept {
        return commit_plan_;
    }
    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const&& = delete;

private:
    friend http_response_stream_head prepare_http_response_stream_head(
        http_response, http_response_stream_commit_plan);

    http_response_stream_head(http_response response, http_response_stream_commit_plan commit_plan)
        : response_(std::move(response)),
          commit_plan_(commit_plan) {}

    http_response response_;
    http_response_stream_commit_plan commit_plan_;
};

// An application Transfer-Encoding must be exactly the `chunked` coding (OWS
// and ASCII case aside): it is a hint the framework's writer replaces with its
// own framing. Any other value throws http1_response_head_error for an HTTP/1
// framing and std::invalid_argument for HTTP/2 or HTTP/3 before the response
// changes.
[[nodiscard]] http_response_stream_head prepare_http_response_stream_head(
    http_response response, http_response_stream_commit_plan commit_plan);

class http1_response_stream_plan final {
public:
    [[nodiscard]] http_response_stream_framing framing() const noexcept {
        return framing_;
    }

    [[nodiscard]] http1_request_connection_plan request_connection_plan() const noexcept {
        return request_connection_plan_;
    }

    [[nodiscard]] http1_close_policy close_policy() const noexcept {
        return close_policy_;
    }

    [[nodiscard]] http_known_method request_method() const noexcept {
        return request_method_;
    }

private:
    friend http1_response_stream_plan http1_plan_response_stream(
        const http1_server_request_parse_state&, http1_close_policy, http1_request_body_consumption) noexcept;

    http1_response_stream_plan(http_response_stream_framing framing,
        http1_request_connection_plan request_connection_plan, http1_close_policy close_policy,
        http_known_method request_method) noexcept
        : framing_(framing),
          request_connection_plan_(request_connection_plan),
          close_policy_(close_policy),
          request_method_(request_method) {}

    http_response_stream_framing framing_;
    http1_request_connection_plan request_connection_plan_;
    http1_close_policy close_policy_{http1_close_policy::close_after_response};
    http_known_method request_method_{http_known_method::unknown};
};

// Pure HTTP/1 response-stream planning. The runtime contributes only its typed
// product policy (for example, a per-connection request limit) and the request
// body's consumption state; HTTP retains the request disposition and candidate
// framing until the response status is known. Commit can therefore distinguish
// a body-allowed HTTP/1.0 stream, which requires close delimiting, from a
// body-suppressed response that is already self-delimited. A route that consumed
// the complete request body before streaming passes `complete`; the parser's
// body plan still describes the original request framing, so planning for an
// unread body passes `body_plan_.consumption_if_unread()`.
[[nodiscard]] inline http1_response_stream_plan http1_plan_response_stream(
    const http1_server_request_parse_state& parsed_value, http1_close_policy close_policy,
    http1_request_body_consumption consumption) noexcept {
    const auto framing = parsed_value.request().protocol_version() == http_protocol_version::http11
                             ? http_response_stream_framing::http1_chunked
                             : http_response_stream_framing::http1_close_delimited;
    return http1_response_stream_plan(framing,
        apply_request_body_consumption(parsed_value.connection_plan(), consumption), close_policy,
        parsed_value.request().known_method());
}

enum class http1_connection_close_field_policy : std::uint8_t { close_only,
    preserve_upgrade };

inline void http1_mark_connection_close(http_response& response,
    http1_connection_close_field_policy field_policy = http1_connection_close_field_policy::close_only) {
    // A runtime close verdict dominates keep-alive. Collapse repeated fields
    // after the socket lifecycle is decided, while preserving the Upgrade
    // option required by any retained Upgrade field and any custom hop-by-hop
    // field names declared in Connection (RFC 9110 section 7.6.1).
    const std::string_view base =
        field_policy == http1_connection_close_field_policy::preserve_upgrade ? "close, Upgrade"
                                                                              : "close";
    std::string merged;
    for (const auto& header : response.headers()) {
        if (!http_ascii_equals_ignore_case(header.name(), "Connection")) {
            continue;
        }
        detail::http_visit_comma_separated_quoted(header.value(), [&merged, base](std::string_view option) {
            if (!http_ascii_equals_ignore_case(option, "close") &&
                !http_ascii_equals_ignore_case(option, "keep-alive") &&
                !http_ascii_equals_ignore_case(option, "Upgrade")) {
                if (merged.empty()) {
                    merged.assign(base);
                }
                merged.append(", ");
                merged.append(option);
            }
            return true;
        });
    }
    if (merged.empty()) {
        response.header_stable_view("Connection", base);
    } else {
        response.header("Connection", std::move(merged));
    }
}

class prepared_http1_response_stream;

// Commit response-side HTTP/1 persistence after the runtime has folded in
// request-body completion and server policy. This is the sole protocol mutation:
// it honors an application-provided Connection: close and emits the version-
// appropriate Connection field. Success retains the exact request version for
// head serialization; a head this endpoint cannot send (for example a 426
// without an Upgrade protocol, or a Transfer-Encoding other than exactly
// `chunked`) throws http1_response_head_error before the response changes.
[[nodiscard]] http1_request_connection_plan http1_commit_final_response(
    http_response& response, http1_request_connection_plan plan);

// Commit-time result. This object combines request/version/runtime constraints with
// the response method/status and Connection options, then binds the exact header
// bytes to the authoritative connection disposition.
class prepared_http1_response_stream final {
public:
    [[nodiscard]] http_response& response() & noexcept {
        return head_.response();
    }
    [[nodiscard]] http_response& response() && = delete;

    [[nodiscard]] const http_response& response() const& noexcept {
        return head_.response();
    }
    [[nodiscard]] const http_response& response() const&& = delete;

    [[nodiscard]] const http1_response_head_plan& response_head_plan() const& noexcept {
        return response_head_plan_;
    }
    [[nodiscard]] const http1_response_head_plan& response_head_plan() const&& = delete;

    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const& noexcept {
        return head_.commit_plan();
    }
    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const&& = delete;

    [[nodiscard]] http1_request_connection_plan connection_plan() const noexcept {
        return connection_plan_;
    }

private:
    friend prepared_http1_response_stream prepare_http1_response_stream_head(
        http_response, const http1_response_stream_plan&, http_response_trailer_intent);

    prepared_http1_response_stream(http_response_stream_head head, http1_response_head_plan response_head_plan,
        http1_request_connection_plan connection_plan) noexcept
        : head_(std::move(head)),
          response_head_plan_(response_head_plan),
          connection_plan_(connection_plan) {}

    http_response_stream_head head_;
    http1_response_head_plan response_head_plan_;
    http1_request_connection_plan connection_plan_;
};

[[nodiscard]] prepared_http1_response_stream prepare_http1_response_stream_head(
    http_response response, const http1_response_stream_plan& plan,
    http_response_trailer_intent trailer_intent = http_response_trailer_intent::none);

// The successful head ends HTTP framing and transfers the connection to a
// duplex byte stream. Payload belongs to that stream, not to an HTTP response.
// A 2xx head with content or framing fields throws http1_response_head_error.
[[nodiscard]] http1_response_head_plan prepare_http1_connect_response_head(
    const http_response& response, http_protocol_version version);

// RFC 9110 §7.8: whether an HTTP/1.1 `request` asks to switch protocols -- an
// Upgrade field offering at least one protocol, sent with the Upgrade
// connection option. Any other request's Upgrade field is ignored, as is a
// CONNECT's: its 2xx establishes a tunnel instead.
[[nodiscard]] bool http1_request_offers_upgrade(const http_request& request) noexcept;

// Makes `response` -- a head without content or framing fields -- the 101
// (Switching Protocols) head after which an HTTP/1.1 connection carries the
// protocol it names (RFC 9110 §7.8, §15.2.2): the response's own Upgrade
// field, else the first protocol `request` offered, with the Upgrade
// connection option added when missing. A request that offers no upgrade, a
// protocol it did not offer, content or framing fields throw
// http1_response_head_error before `response` changes.
[[nodiscard]] http1_response_head_plan prepare_http1_upgrade_response_head(
    http_response& response, const http_request& request);

// Already-expanded proxy URI path, borrowed until open_udp_tunnel() returns.
struct http_client_udp_tunnel_request_view final {
    borrowed_text target_{};
    borrowed_http_headers headers_{};
};

struct http_connect_udp_target_view final {
    borrowed_text host_{};
    port_number port_{};
};
struct http_connect_udp_target final {
    http_connect_udp_target() noexcept = default;
    http_connect_udp_target(std::string host, port_number port = {}) noexcept;
    http_connect_udp_target(const http_connect_udp_target&) = default;
    http_connect_udp_target(http_connect_udp_target&&) noexcept = default;
    http_connect_udp_target& operator=(const http_connect_udp_target&) = default;
    http_connect_udp_target& operator=(http_connect_udp_target&&) noexcept = default;

    std::string host_;
    std::uint16_t port_{0};
};
enum class http_connect_udp_error : std::uint8_t {
    invalid_target,
    invalid_path,
    invalid_request,
    invalid_response,
    invalid_capsule_protocol,
};
// RFC 9298's default URI template. IPv6 hosts have no brackets or zone ID;
// all reserved bytes are percent-encoded during template expansion.
[[nodiscard]] std::variant<std::string, http_connect_udp_error> encode_http_connect_udp_path(
    http_connect_udp_target_view target);
[[nodiscard]] std::variant<http_connect_udp_target, http_connect_udp_error> parse_http_connect_udp_path(
    std::string_view path);

struct http_connect_udp_request_view final {
    // HTTP/1.1 Upgrade versus HTTP/2 Extended CONNECT.
    http_protocol_version version_{http_protocol_version::http11};
    borrowed_text method_{"GET"};
    borrowed_text protocol_{"connect-udp"};
    borrowed_text scheme_{"https"};
    borrowed_text authority_{};
    borrowed_text path_{};
    borrowed_http_headers headers_{};
};
// Recipient checks. The connect-udp token selects the Capsule Protocol, so
// Capsule-Protocol is optional; a non-Boolean, ?0 or repeated field is treated as
// absent (RFC 9297 section 3.4). Responses with 204/205/206 are malformed (RFC 9297 section 3.2).
[[nodiscard]] std::optional<http_connect_udp_error> validate_http_connect_udp_request(http_connect_udp_request_view request) noexcept;
// Sender check for a CONNECT-UDP request this endpoint emits: the recipient
// rules plus exactly one Capsule-Protocol: ?1 field (RFC 9297 section 3.4
// SHOULD, kept mandatory for Ruvia senders on HTTP/1.1 and HTTP/2).
[[nodiscard]] std::optional<http_connect_udp_error> validate_http_connect_udp_request_to_send(
    http_connect_udp_request_view request) noexcept;
[[nodiscard]] std::optional<http_connect_udp_error> validate_http_connect_udp_response(
    http_protocol_version version, std::uint16_t status, std::span<const http_header_view> headers) noexcept;
[[nodiscard]] std::optional<http_connect_udp_error> validate_http_connect_udp_response(
    http_protocol_version version, std::uint16_t status, std::span<const http_header> headers) noexcept;
// Classifies the HTTP/1 Upgrade token; full validation remains mandatory.
[[nodiscard]] bool is_http_connect_udp_upgrade_request(const http_request& request) noexcept;
[[nodiscard]] std::optional<http_connect_udp_error> validate_http_connect_udp_request(const http_request& request) noexcept;
// Takes application response metadata, validates it (2xx other than 204/205/206;
// an application Capsule-Protocol field must be a single ?1), and supplies the
// Capsule-Protocol and version-specific status/Upgrade fields. HTTP/1 status 101
// can only be produced by this dedicated driver boundary.
[[nodiscard]] std::variant<http_response, http_connect_udp_error> prepare_http_connect_udp_response(http_response response, http_protocol_version version);
// The HTTP/1 head plan of a response prepare_http_connect_udp_response()
// produced; throws http1_response_head_error for any other response.
[[nodiscard]] http1_response_head_plan prepare_http1_connect_udp_response_head(const http_response& response);

enum class http_capsule_codec_error : std::uint8_t { need_more_data,
    value_out_of_range,
    output_too_small };
// RFC 9297 Capsule-Protocol is a Structured Field Boolean Item.
[[nodiscard]] std::optional<bool> parse_http_capsule_protocol(std::string_view value) noexcept;

inline constexpr std::uint64_t http_datagram_capsule_type = 0;
struct http_udp_datagram_view final {
    std::uint64_t context_id_{0};
    std::span<const char> payload_{};
};
// RFC 9298: context ID zero carries a UDP payload; unknown contexts are dropped.
// The payload view borrows `input`.
[[nodiscard]] std::optional<http_udp_datagram_view> decode_http_udp_datagram(std::span<const char> input) noexcept;
template <detail::http_temporary_owning_range<char> input_type>
std::optional<http_udp_datagram_view> decode_http_udp_datagram(input_type&&) noexcept = delete;
[[nodiscard]] std::variant<std::size_t, http_capsule_codec_error> encode_http_udp_datagram_prefix(std::span<char> output, std::uint64_t context_id = 0) noexcept;

struct http_capsule_event final {
    std::uint64_t type_{0};
    std::span<const char> payload_{};
    bool end_capsule_{false};
};
using http_capsule_callback_type = void (*)(void*, http_capsule_event);
enum class http_capsule_status : std::uint8_t { need_more_data,
    end,
    truncated,
    limit };
struct http_capsule_feed_result final {
    http_capsule_status status_{http_capsule_status::need_more_data};
    std::size_t consumed_bytes_{};
    bool capsule_complete_{};
};
// max_capsule_length_ bounds the capsules a reader keeps (see
// http_capsule_retention); dropped capsules are skipped at any length.
struct http_capsule_config final {
    std::uint64_t max_capsule_length_{16 * 1024 * 1024};
};
// Which capsules a reader keeps. RFC 9297 section 3.2: a recipient silently
// drops a capsule it does not use and skips over it. Dropped capsules produce
// no callback, are not buffered and are not subject to max_capsule_length_.
enum class http_capsule_retention : std::uint8_t {
    // Every capsule: a generic Capsule Protocol reader handles unknown types.
    all,
    // DATAGRAM capsules (RFC 9297 section 3.5) only.
    datagrams,
    // DATAGRAM capsules carrying RFC 9298 Context ID 0 (UDP payloads); other
    // Context IDs are unknown and dropped. The Context ID is inspected before
    // the length limit applies. A DATAGRAM too short for its Context ID is kept
    // so that the reader rejects it as malformed.
    udp_payloads,
};

// RFC 9297 incremental Capsule Protocol framing for HTTP/1 tunnels. Header state is inline; payload
// fragments borrow feed input (or, for a kept CONNECT-UDP Context ID, decoder
// storage) only during callbacks. No owned payload or event queue is created.
// The retention of a capsule is decided from the feed call that completes its
// header (and, for udp_payloads, its Context ID).
class http_capsule_decoder final {
public:
    explicit http_capsule_decoder(http_capsule_config config = {});
    [[nodiscard]] http_capsule_status feed(std::span<const char> input, bool fin, http_capsule_callback_type callback,
        void* context, http_capsule_retention retention = http_capsule_retention::all);
    template <typename visitor_type>
        requires std::invocable<visitor_type&, http_capsule_event>
    [[nodiscard]] http_capsule_status feed(std::span<const char> input, bool fin,
        visitor_type&& visitor,
        http_capsule_retention retention = http_capsule_retention::all) {
        auto& target = visitor;
        return feed(
            input, fin,
            +[](void* context, http_capsule_event event) {
                (*static_cast<std::remove_reference_t<visitor_type>*>(context))(event);
            },
            std::addressof(target), retention);
    }
    // Stops at the first complete kept capsule. The unconsumed suffix stays with
    // the caller; FIN is committed only after all input is consumed. This permits
    // pull-based adapters without buffering a queue of complete capsules.
    [[nodiscard]] http_capsule_feed_result feed_one(std::span<const char> input, bool fin,
        http_capsule_callback_type callback, void* context,
        http_capsule_retention retention = http_capsule_retention::all);
    template <typename visitor_type>
        requires std::invocable<visitor_type&, http_capsule_event>
    [[nodiscard]] http_capsule_feed_result feed_one(std::span<const char> input, bool fin,
        visitor_type&& visitor,
        http_capsule_retention retention = http_capsule_retention::all) {
        auto& target = visitor;
        return feed_one(
            input, fin,
            +[](void* context, http_capsule_event event) {
                (*static_cast<std::remove_reference_t<visitor_type>*>(context))(event);
            },
            std::addressof(target), retention);
    }

private:
    enum class payload_mode : std::uint8_t { deliver,
        skip,
        context_id };
    [[nodiscard]] http_capsule_feed_result feed_impl(std::span<const char> input, bool fin,
        http_capsule_callback_type callback, void* context, bool one, http_capsule_retention retention);
    http_capsule_config config_;
    std::array<char, 16> header_{};
    std::array<char, 8> context_id_{};
    std::size_t header_size_{0}, type_size_{0}, context_id_size_{0};
    std::uint64_t type_{0}, remaining_{0};
    payload_mode mode_{payload_mode::deliver};
    bool payload_{false}, feeding_{false};
    http_capsule_status status_{http_capsule_status::need_more_data};
};
[[nodiscard]] std::variant<std::size_t, http_capsule_codec_error> encode_http_capsule_header(std::span<char> output, std::uint64_t type, std::uint64_t length) noexcept;
enum class http_datagram_error : std::uint8_t {
    send_closed,
    payload_too_large,
    malformed,
};
// A valid output snapshot produced by http_datagram_session. The inline prefix
// belongs to this plan; payload() continues to borrow the caller's input.
class http_datagram_write_plan final {
public:
    [[nodiscard]] constexpr std::string_view prefix() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::string_view(prefix_.data(), prefix_size_);
    }
    std::string_view prefix() const&& = delete;

    [[nodiscard]] constexpr std::span<const char> payload() const noexcept {
        return payload_;
    }

private:
    friend class http_datagram_session;
    http_datagram_write_plan() noexcept = default;

    std::array<char, 24> prefix_{};
    std::size_t prefix_size_{0};
    std::span<const char> payload_{};
};
// One accepted HTTP Datagram channel, with optional CONNECT-UDP semantics. Construct after request negotiation (clients
// may optimistically send before the response). Owns no memory or transport.
// Returned payloads borrow caller input; unknown context IDs are dropped.
class http_datagram_session final {
public:
    http_datagram_session() noexcept = default;
    void close_send() noexcept {
        send_open_ = false;
    }
    void close_receive() noexcept {
        receive_open_ = false;
    }
    [[nodiscard]] std::variant<http_datagram_write_plan, http_datagram_error> prepare_datagram(
        std::span<const char> payload) const noexcept;
    template <detail::http_temporary_owning_range<char> payload_type>
    std::variant<http_datagram_write_plan, http_datagram_error> prepare_datagram(payload_type&&) const noexcept = delete;
    [[nodiscard]] std::optional<std::span<const char>> receive_datagram(
        std::span<const char> input) const noexcept;
    template <detail::http_temporary_owning_range<char> input_type>
    std::optional<std::span<const char>> receive_datagram(input_type&&) const noexcept = delete;
    [[nodiscard]] std::variant<http_datagram_write_plan, http_datagram_error> prepare_udp_datagram(
        std::span<const char> payload) const noexcept;
    template <detail::http_temporary_owning_range<char> payload_type>
    std::variant<http_datagram_write_plan, http_datagram_error> prepare_udp_datagram(payload_type&&) const noexcept = delete;
    // Capsule input is the complete DATAGRAM capsule value, excluding type/length.
    // Empty optional means silently dropped.
    [[nodiscard]] std::variant<std::optional<http_udp_datagram_view>, http_datagram_error> receive_udp_datagram(
        std::span<const char> input) const noexcept;
    template <detail::http_temporary_owning_range<char> input_type>
    std::variant<std::optional<http_udp_datagram_view>, http_datagram_error> receive_udp_datagram(input_type&&) const noexcept = delete;

private:
    [[nodiscard]] static std::variant<http_datagram_write_plan, http_datagram_error> plan_datagram_capsule(
        std::span<const char> payload, std::string_view context_prefix) noexcept;

    bool send_open_{true};
    bool receive_open_{true};
};
}  // namespace ruvia
