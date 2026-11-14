#pragma once

#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "borrowed_text.h"
#include "http_content_codec.h"
#include "http_content_coding.h"

namespace ruvia {

struct http_content_transcoder_options final {
    std::size_t max_decoded_bytes_{(std::numeric_limits<std::size_t>::max)()};
    std::size_t max_encoded_bytes_{(std::numeric_limits<std::size_t>::max)()};
    std::size_t max_memory_bytes_{(std::numeric_limits<std::size_t>::max)()};
};

// Streaming content transcoder (and multi-stage incremental decoder when
// target_coding is identity). Decodes source_codings in reverse wire order and
// encodes into target_coding.
// write() and finish() return the representation's data error
// (invalid_content or decoded_size_exceeded) as a value; the transcoder is then
// failed, output keeps the bytes produced before the error, and only reset()
// makes it usable again. Codec failures throw http_content_codec_error and
// allocation exceptions keep their type; either also leaves it failed. Writing
// after a failure or finish, or finishing a failed transcoder, throws
// std::logic_error; finish() after success is a no-op.
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

    [[nodiscard]] std::optional<http_content_decode_error> write(std::string_view input, std::string& output, bool flush = false);
    [[nodiscard]] std::optional<http_content_decode_error> finish(std::string& output);
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
// Exceeding max_bytes_ is the passthrough fallback, never an error; a source
// representation that cannot be decoded is returned as invalid_content and
// leaves the transcoder failed until reset().
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

    // Staging versus fallback is reported by passthrough().
    [[nodiscard]] std::optional<http_content_decode_error> write(std::string_view input);
    [[nodiscard]] std::optional<http_content_decode_error> write(std::string_view input, std::string& passthrough_output);

    [[nodiscard]] std::variant<http_staged_transcode_result, http_content_decode_error> finish();
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

}  // namespace ruvia
