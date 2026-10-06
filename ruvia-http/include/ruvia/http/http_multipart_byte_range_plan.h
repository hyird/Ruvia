#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/HttpByteRange.h"
#include "ruvia/http/detail/util/HttpPmrObject.h"

namespace ruvia {

// Owns copied framing metadata and ordered file-slice descriptors. Observed
// views borrow the plan; its PMR resource must outlive it. Construction and
// clone propagate allocation failures.
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
        return state_->content_type_;
    }
    [[nodiscard]] std::string_view content_type() const&& = delete;
    [[nodiscard]] std::string_view metadata() const& noexcept {
        return state_->metadata_;
    }
    [[nodiscard]] std::string_view metadata() const&& = delete;
    [[nodiscard]] std::span<const segment> segments() const& noexcept {
        return state_->segments_;
    }
    [[nodiscard]] std::span<const segment> segments() const&& = delete;
    [[nodiscard]] std::uint64_t content_length() const noexcept {
        return state_->content_length_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return state_.get_deleter().resource;
    }
    [[nodiscard]] http_multipart_byte_range_plan clone(
        std::pmr::memory_resource* resource) const;

private:
    friend http_multipart_byte_range_plan make_http_multipart_byte_range_plan(
        const http_byte_range_set&, std::uint64_t, std::string_view, std::string_view,
        std::string_view, std::pmr::memory_resource*);

    struct storage final {
        explicit storage(std::pmr::memory_resource* resource)
            : content_type_(std::string_view{}, resource),
              metadata_(std::string_view{}, resource),
              segments_(std::initializer_list<segment>{}, resource) {}

        std::pmr::string content_type_;
        std::pmr::string metadata_;
        std::pmr::vector<segment> segments_;
        std::uint64_t content_length_{};
    };

    explicit http_multipart_byte_range_plan(std::pmr::memory_resource* resource)
        : state_(detail::makeHttpPmrObject<storage>(resource, resource)) {}

    std::unique_ptr<storage, detail::HttpPmrObjectDeleter<storage>> state_;
};

[[nodiscard]] http_multipart_byte_range_plan make_http_multipart_byte_range_plan(
    const http_byte_range_set& ranges, std::uint64_t representation_length,
    std::string_view media_type, std::string_view boundary,
    std::string_view content_encoding, std::pmr::memory_resource* resource);

}  // namespace ruvia
