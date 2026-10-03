#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/HttpByteRange.h"

namespace ruvia {

class http_multipart_byte_range_plan final {
public:
    enum class segment_kind : std::uint8_t {
        metadata,
        file,
    };

    struct segment final {
        segment_kind kind{};
        std::size_t metadata_offset{};
        std::size_t metadata_length{};
        std::uint64_t file_offset{};
        std::uint64_t file_length{};
    };

    http_multipart_byte_range_plan(const http_multipart_byte_range_plan&) = delete;
    http_multipart_byte_range_plan& operator=(const http_multipart_byte_range_plan&) = delete;
    http_multipart_byte_range_plan(http_multipart_byte_range_plan&&) noexcept = default;
    http_multipart_byte_range_plan& operator=(http_multipart_byte_range_plan&&) = delete;

    [[nodiscard]] std::string_view content_type() const& noexcept {
        return content_type_;
    }
    [[nodiscard]] std::string_view content_type() const&& = delete;
    [[nodiscard]] std::string_view metadata() const& noexcept {
        return metadata_;
    }
    [[nodiscard]] std::string_view metadata() const&& = delete;
    [[nodiscard]] std::span<const segment> segments() const& noexcept {
        return segments_;
    }
    [[nodiscard]] std::span<const segment> segments() const&& = delete;
    [[nodiscard]] std::uint64_t content_length() const noexcept {
        return content_length_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return content_type_.get_allocator().resource();
    }
    [[nodiscard]] http_multipart_byte_range_plan clone(
        std::pmr::memory_resource* resource) const;

private:
    friend http_multipart_byte_range_plan make_http_multipart_byte_range_plan(
        const http_byte_range_set&, std::uint64_t, std::string_view, std::string_view,
        std::string_view, std::pmr::memory_resource*);

    explicit http_multipart_byte_range_plan(std::pmr::memory_resource* resource)
        : content_type_(resource),
          metadata_(resource),
          segments_(resource) {}

    std::pmr::string content_type_;
    std::pmr::string metadata_;
    std::pmr::vector<segment> segments_;
    std::uint64_t content_length_{};
};

[[nodiscard]] http_multipart_byte_range_plan make_http_multipart_byte_range_plan(
    const http_byte_range_set& ranges, std::uint64_t representation_length,
    std::string_view media_type, std::string_view boundary,
    std::string_view content_encoding, std::pmr::memory_resource* resource);

}  // namespace ruvia
