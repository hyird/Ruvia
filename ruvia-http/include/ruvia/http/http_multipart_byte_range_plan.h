#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/http/http_byte_range.h"

namespace ruvia {

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

}  // namespace ruvia
