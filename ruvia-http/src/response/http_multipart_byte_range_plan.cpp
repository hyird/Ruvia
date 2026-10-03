#include "ruvia/http/http_multipart_byte_range_plan.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>

#include "ruvia/http/HttpMediaType.h"
#include "ruvia/http/detail/coding/HttpContentCoding.h"

namespace ruvia {
namespace {

[[nodiscard]] bool valid_boundary(std::string_view boundary) noexcept {
    if (boundary.empty() || boundary.size() > 70 || boundary.back() == ' ') {
        return false;
    }
    for (const unsigned char value : boundary) {
        const bool alpha_numeric = (value >= '0' && value <= '9') ||
                                   (value >= 'A' && value <= 'Z') ||
                                   (value >= 'a' && value <= 'z');
        constexpr std::string_view punctuation{"'()+_,-./:=? "};
        if (!alpha_numeric && punctuation.find(static_cast<char>(value)) == std::string_view::npos) {
            return false;
        }
    }
    return true;
}

void append_number(std::pmr::string& target, std::uint64_t value) {
    std::array<char, 32> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error != std::errc{}) {
        throw std::length_error("multipart range number formatting failed");
    }
    target.append(buffer.data(), end);
}

}  // namespace

http_multipart_byte_range_plan make_http_multipart_byte_range_plan(
    const http_byte_range_set& ranges, std::uint64_t representation_length,
    std::string_view media_type, std::string_view boundary,
    std::string_view content_encoding, std::pmr::memory_resource* resource) {
    if (ranges.ignored() || ranges.unsatisfiable() || ranges.size() < 2 ||
        ranges.size() > http_byte_range_set::capacity || representation_length == 0 ||
        !isValidHttpContentTypeFieldValue(media_type) ||
        (!content_encoding.empty() &&
            !detail::isValidHttpContentEncodingFieldValue(
                content_encoding, detail::HttpFieldListRole::kSender)) ||
        !valid_boundary(boundary) || resource == nullptr) {
        throw std::invalid_argument("invalid multipart byte-range plan input");
    }

    http_multipart_byte_range_plan plan(resource);
    plan.content_type_.append("multipart/byteranges; boundary=");
    const bool boundary_is_token = std::ranges::all_of(boundary, [](const unsigned char value) {
        return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
               (value >= 'a' && value <= 'z') ||
               std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(value)) !=
                   std::string_view::npos;
    });
    if (!boundary_is_token) {
        plan.content_type_.push_back('"');
    }
    plan.content_type_.append(boundary);
    if (!boundary_is_token) {
        plan.content_type_.push_back('"');
    }
    plan.segments_.reserve(ranges.size() * 3 + 1);

    const auto append_metadata = [&plan](std::string_view bytes) {
        const auto offset = plan.metadata_.size();
        plan.metadata_.append(bytes.data(), bytes.size());
        if (plan.content_length_ >
            (std::numeric_limits<std::uint64_t>::max)() - bytes.size()) {
            throw std::length_error("multipart response content length overflows uint64_t");
        }
        plan.content_length_ += bytes.size();
        plan.segments_.push_back({http_multipart_byte_range_plan::segment_kind::metadata,
            offset, bytes.size(), 0, 0});
    };

    for (std::size_t index = 0; index < ranges.size(); ++index) {
        const auto range = ranges[index];
        if (range.length_ == 0 || range.offset_ > representation_length ||
            range.length_ > representation_length - range.offset_) {
            throw std::invalid_argument("multipart range is outside the representation");
        }

        std::pmr::string part(resource);
        part.append("--");
        part.append(boundary);
        part.append("\r\nContent-Type: ");
        part.append(media_type);
        if (!content_encoding.empty()) {
            part.append("\r\nContent-Encoding: ");
            part.append(content_encoding);
        }
        part.append("\r\nContent-Range: bytes ");
        append_number(part, range.offset_);
        part.push_back('-');
        append_number(part, range.offset_ + range.length_ - 1);
        part.push_back('/');
        append_number(part, representation_length);
        part.append("\r\n\r\n");
        append_metadata(part);

        if (plan.content_length_ >
            (std::numeric_limits<std::uint64_t>::max)() - range.length_) {
            throw std::length_error("multipart response content length overflows uint64_t");
        }
        plan.content_length_ += range.length_;
        plan.segments_.push_back({http_multipart_byte_range_plan::segment_kind::file,
            0, 0, range.offset_, range.length_});
        append_metadata("\r\n");
    }

    std::pmr::string closing(resource);
    closing.append("--");
    closing.append(boundary);
    closing.append("--\r\n");
    append_metadata(closing);
    return plan;
}

http_multipart_byte_range_plan http_multipart_byte_range_plan::clone(
    std::pmr::memory_resource* resource) const {
    if (resource == nullptr) {
        throw std::invalid_argument("multipart plan clone requires a memory resource");
    }
    http_multipart_byte_range_plan copy(resource);
    copy.content_type_.assign(content_type_);
    copy.metadata_.assign(metadata_);
    copy.segments_.assign(segments_.begin(), segments_.end());
    copy.content_length_ = content_length_;
    return copy;
}

}  // namespace ruvia
