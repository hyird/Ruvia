#include "http_range.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "decimal_text.h"
#include "http_content_negotiation.h"
#include "http_field_text.h"
#include "http_header.h"
#include "http_multipart.h"

namespace ruvia {
namespace {

constexpr std::string_view boundary_prefix = "--";
constexpr std::string_view part_type_prefix = "\r\nContent-Type: ";
constexpr std::string_view part_range_prefix = "\r\nContent-Range: bytes ";
constexpr std::string_view part_head_end = "\r\n\r\n";
constexpr std::string_view part_end = "\r\n";
constexpr std::string_view multipart_end = "--\r\n";

void write_multipart_range_type(std::string& output, std::string_view boundary) {
    constexpr std::string_view prefix = "multipart/byteranges; boundary=";
    const bool quote = http_token_prefix_size(boundary) != boundary.size();
    output.reserve(prefix.size() + boundary.size() + (quote ? 2U : 0U));
    output.append(prefix);
    if (quote) {
        output.push_back('"');
    }
    output.append(boundary);
    if (quote) {
        output.push_back('"');
    }
}

void append_range_part_head(std::string& output, std::string_view boundary, std::string_view media_type, http_byte_range range, std::uint64_t representation_length) {
    output.append(boundary_prefix).append(boundary).append(part_type_prefix).append(media_type).append(part_range_prefix);
    append_decimal(output, range.offset_);
    output.push_back('-');
    append_decimal(output, range.offset_ + range.length_ - 1);
    output.push_back('/');
    append_decimal(output, representation_length);
    output.append(part_head_end);
}

void append_multipart_range_end(std::string& output, std::string_view boundary) {
    output.append(boundary_prefix).append(boundary).append(multipart_end);
}

struct multipart_range_size final {
    multipart_range_size(std::size_t boundary_bytes, std::uint64_t representation_length) noexcept :
        metadata_(boundary_prefix.size() + boundary_bytes + multipart_end.size()),
        total_(metadata_),
        fixed_part_bytes_(boundary_prefix.size() + boundary_bytes + part_type_prefix.size() + part_range_prefix.size() + 2U + decimal_size(representation_length) + part_head_end.size() + part_end.size()) {}

    [[nodiscard]] bool add(http_byte_range range, std::size_t media_type_bytes) noexcept {
        constexpr auto max_bytes = (std::numeric_limits<std::uint64_t>::max)();
        const auto overhead = fixed_part_bytes_ + decimal_size(range.offset_) + decimal_size(range.offset_ + range.length_ - 1);
        if (media_type_bytes > max_bytes - overhead) {
            return false;
        }
        const auto part_metadata = overhead + static_cast<std::uint64_t>(media_type_bytes);
        if (part_metadata > max_bytes - total_ || range.length_ > max_bytes - total_ - part_metadata) {
            return false;
        }
        metadata_ += part_metadata;
        total_ += part_metadata + range.length_;
        return true;
    }

    [[nodiscard]] bool add_all(std::span<const http_byte_range> ranges, std::size_t media_type_bytes) noexcept {
        return std::ranges::all_of(ranges, [&](http_byte_range range) {
            return add(range, media_type_bytes);
        });
    }

    std::uint64_t metadata_;
    std::uint64_t total_;
    std::uint64_t fixed_part_bytes_;
};

}  // namespace

http_multipart_byte_range_plan make_http_multipart_byte_range_plan(const http_byte_range_set& ranges, http_content_length representation_length_value, std::string_view media_type, std::string_view boundary) {
    const auto representation_length = representation_length_value.value();
    if (ranges.ignored() || ranges.unsatisfiable() || ranges.size() < 2 || ranges.size() > http_byte_range_set::capacity || representation_length == 0 || !is_valid_http_content_type_field_value(media_type) || !multipart_boundary::try_parse(boundary).has_value()) {
        throw std::invalid_argument("invalid multipart byte-range plan input");
    }

    multipart_range_size size(boundary.size(), representation_length);
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        const auto range = ranges[index];
        if (range.length_ == 0 || range.offset_ > representation_length || range.length_ > representation_length - range.offset_) {
            throw std::invalid_argument("multipart range is outside the representation");
        }
        if (!size.add(range, media_type.size())) {
            throw std::length_error("multipart response content length overflows uint64_t");
        }
    }
    if (!std::in_range<std::size_t>(size.metadata_)) {
        throw std::length_error("multipart response metadata size overflows size_t");
    }

    http_multipart_byte_range_plan plan;
    auto& storage = *plan.state_;
    auto& metadata = storage.metadata_;
    write_multipart_range_type(storage.content_type_, boundary);
    metadata.reserve(static_cast<std::size_t>(size.metadata_));
    const auto record_metadata = [&storage](std::size_t offset) noexcept {
        const auto length = storage.metadata_.size() - offset;
        if (storage.segment_count_ != 0) {
            auto& previous = storage.segments_[storage.segment_count_ - 1];
            if (previous.kind_ == http_multipart_byte_range_plan::segment_kind::metadata && previous.metadata_offset_ + previous.metadata_length_ == offset) {
                previous.metadata_length_ = storage.metadata_.size() - previous.metadata_offset_;
                return;
            }
        }
        storage.segments_[storage.segment_count_++] = {http_multipart_byte_range_plan::segment_kind::metadata, offset, length, 0, 0};
    };

    for (std::size_t index = 0; index < ranges.size(); ++index) {
        const auto range = ranges[index];
        const auto part_offset = metadata.size();
        append_range_part_head(metadata, boundary, media_type, range, representation_length);
        record_metadata(part_offset);

        storage.segments_[storage.segment_count_++] = {http_multipart_byte_range_plan::segment_kind::file, 0, 0, range.offset_, range.length_};
        const auto separator_offset = metadata.size();
        metadata.append(part_end);
        record_metadata(separator_offset);
    }

    const auto closing_offset = metadata.size();
    append_multipart_range_end(metadata, boundary);
    record_metadata(closing_offset);
    storage.content_length_ = size.total_;
    return plan;
}

http_multipart_byte_range_plan http_multipart_byte_range_plan::clone() const {
    if (state_ == nullptr) {
        throw std::invalid_argument("multipart response plan must contain segments");
    }
    http_multipart_byte_range_plan copy;
    copy.state_->content_type_.assign(state_->content_type_);
    copy.state_->metadata_.assign(state_->metadata_);
    copy.state_->segments_ = state_->segments_;
    copy.state_->segment_count_ = state_->segment_count_;
    copy.state_->content_length_ = state_->content_length_;
    return copy;
}

http_byte_range_set resolve_http_byte_range_set(std::string_view field_value, http_content_length representation_length_value, http_byte_range_options options) {
    const auto representation_length = representation_length_value.value();
    http_byte_range_set result;
    if (options.max_ranges_ == 0) {
        return result;
    }
    field_value = http_trim_ows(field_value);
    constexpr std::string_view unit_prefix = "bytes=";
    if (!http_ascii_starts_with_ignore_case(field_value, unit_prefix)) {
        return result;
    }

    const bool strict = options.order_policy_ == http_byte_range_order_policy::strict_ascending_disjoint;
    const auto add_range = [&](std::uint64_t offset, std::uint64_t length) -> bool {
        if (strict) {
            if (result.count_ > 0) {
                const auto prev = result[result.count_ - 1];
                if (offset < prev.offset_ + prev.length_) {
                    return false;
                }
            }
            if (result.count_ == options.max_ranges_) {
                return false;
            }
            result.append_slot({offset, length});
            return true;
        }
        auto start = offset;
        auto end = offset + length;
        auto target = result.count_;
        std::size_t kept = 0;
        for (std::size_t index = 0; index < result.count_; ++index) {
            const auto current = result[index];
            const auto current_end = current.offset_ + current.length_;
            if (current.offset_ <= end && start <= current_end) {
                start = (std::min)(start, current.offset_);
                end = (std::max)(end, current_end);
                if (target == result.count_) {
                    target = kept++;
                }
                continue;
            }
            result.mutable_at(kept++) = current;
        }
        if (target == result.count_) {
            if (result.count_ == options.max_ranges_) {
                return false;
            }
            result.append_slot({start, end - start});
        } else {
            result.resize_slots(kept);
            result.mutable_at(target) = {start, end - start};
        }
        return true;
    };

    auto specs = http_trim_ows(field_value.substr(unit_prefix.size()));
    if (specs.empty()) {
        return result;
    }
    result.ignored_ = false;
    std::size_t empty_members = 0;
    bool saw_range_spec = false;
    bool has_more = false;
    do {
        const auto comma = specs.find(',');
        auto spec = http_trim_ows(specs.substr(0, comma));
        has_more = comma != std::string_view::npos;
        specs = has_more ? specs.substr(comma + 1) : std::string_view{};
        if (spec.empty()) {
            if (++empty_members > 32) {
                return http_byte_range_set{};
            }
            continue;
        }
        saw_range_spec = true;
        const auto dash = spec.find('-');
        if (dash == std::string_view::npos || spec.find('-', dash + 1) != std::string_view::npos) {
            return http_byte_range_set{};
        }
        const auto first = spec.substr(0, dash);
        const auto last = spec.substr(dash + 1);
        if (first.empty()) {
            const auto suffix = parse_saturating_decimal(last);
            if (!suffix) {
                return http_byte_range_set{};
            }
            if (*suffix == 0 || representation_length == 0) {
                continue;
            }
            const auto length = (std::min)(*suffix, representation_length);
            if (!add_range(representation_length - length, length)) {
                return http_byte_range_set{};
            }
            continue;
        }
        const auto parsed_start = parse_saturating_decimal(first);
        const auto parsed_end = parse_saturating_decimal(last);
        if (!parsed_start || (!last.empty() && !parsed_end)) {
            return http_byte_range_set{};
        }
        const auto start = *parsed_start;
        const auto end = parsed_end.value_or(0);
        if (!last.empty()) {
            if (start > end) {
                return http_byte_range_set{};
            }
            if (start == (std::numeric_limits<std::uint64_t>::max)() && end == start) {
                auto normalized_first = first;
                auto normalized_last = last;
                while (normalized_first.size() > 1 && normalized_first.front() == '0') {
                    normalized_first.remove_prefix(1);
                }
                while (normalized_last.size() > 1 && normalized_last.front() == '0') {
                    normalized_last.remove_prefix(1);
                }
                if (normalized_first.size() > normalized_last.size() || (normalized_first.size() == normalized_last.size() && normalized_first > normalized_last)) {
                    return http_byte_range_set{};
                }
            }
        }
        if (start >= representation_length) {
            continue;
        }
        const auto clamped_end = last.empty() ? representation_length - 1 : (std::min)(end, representation_length - 1);
        if (!add_range(start, clamped_end - start + 1)) {
            return http_byte_range_set{};
        }
    } while (has_more);
    if (result.count_ == 0 && !saw_range_spec) {
        return http_byte_range_set{};
    }
    return result;
}

std::optional<http_content_range> parse_http_content_range(std::string_view field_value) noexcept {
    constexpr std::string_view unit = "bytes";
    if (!http_ascii_starts_with_ignore_case(field_value, unit) || field_value.size() == unit.size() || field_value[unit.size()] != ' ') {
        return std::nullopt;
    }
    const auto spec = field_value.substr(unit.size() + 1);
    const auto dash = spec.find('-');
    if (dash == std::string_view::npos) {
        return std::nullopt;
    }
    const auto slash = spec.find('/', dash + 1);
    if (slash == std::string_view::npos || spec.find('/', slash + 1) != std::string_view::npos || spec.find('-', dash + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto first = parse_integer<std::uint64_t>(spec.substr(0, dash));
    const auto last = parse_integer<std::uint64_t>(spec.substr(dash + 1, slash - dash - 1));
    const auto total = parse_integer<std::uint64_t>(spec.substr(slash + 1));
    if (!first || !last || !total) {
        return std::nullopt;
    }
    if (*first > *last || *last >= *total) {
        return std::nullopt;
    }
    return http_content_range{
        .first_ = *first,
        .last_ = *last,
        .total_ = *total,
    };
}

std::optional<std::uint64_t> compute_http_byte_range_content_length(std::span<const http_byte_range> ranges, http_content_length representation_length_value, std::string_view media_type, std::string_view boundary) noexcept {
    const auto representation_length = representation_length_value.value();
    if (ranges.empty() || representation_length == 0) {
        return std::nullopt;
    }
    for (const auto& range : ranges) {
        if (range.length_ == 0 || range.offset_ > representation_length || range.length_ > representation_length - range.offset_) {
            return std::nullopt;
        }
    }
    if (ranges.size() == 1) {
        return ranges.front().length_;
    }
    if (!is_valid_http_content_type_field_value(media_type) || !multipart_boundary::try_parse(boundary).has_value()) {
        return std::nullopt;
    }
    multipart_range_size size(boundary.size(), representation_length);
    if (!size.add_all(ranges, media_type.size())) {
        return std::nullopt;
    }
    return size.total_;
}

std::optional<std::uint64_t> compute_http_byte_range_content_length(const http_byte_range_set& ranges, http_content_length representation_length, std::string_view media_type, std::string_view boundary) noexcept {
    if (ranges.ignored() || ranges.unsatisfiable()) {
        return std::nullopt;
    }
    return compute_http_byte_range_content_length(ranges.ranges(), representation_length, media_type, boundary);
}

http_byte_range_slicer::http_byte_range_slicer(http_byte_range_slicer&& other) noexcept :
    single_range_(other.single_range_),
    ranges_(std::move(other.ranges_)),
    media_type_(std::move(other.media_type_)),
    boundary_(std::move(other.boundary_)),
    content_type_(std::move(other.content_type_)),
    content_range_(other.content_range_),
    content_range_size_(std::exchange(other.content_range_size_, 0)),
    single_mode_(std::exchange(other.single_mode_, false)),
    representation_length_(std::exchange(other.representation_length_, 0)),
    content_length_(std::exchange(other.content_length_, 0)),
    current_offset_(std::exchange(other.current_offset_, 0)),
    range_progress_(std::exchange(other.range_progress_, 0)),
    range_index_(std::exchange(other.range_index_, 0)) {}

http_byte_range_slicer& http_byte_range_slicer::operator=(http_byte_range_slicer&& other) noexcept {
    if (this != &other) {
        single_range_ = other.single_range_;
        ranges_ = std::move(other.ranges_);
        media_type_ = std::move(other.media_type_);
        boundary_ = std::move(other.boundary_);
        content_type_ = std::move(other.content_type_);
        content_range_ = other.content_range_;
        content_range_size_ = std::exchange(other.content_range_size_, 0);
        single_mode_ = std::exchange(other.single_mode_, false);
        representation_length_ = std::exchange(other.representation_length_, 0);
        content_length_ = std::exchange(other.content_length_, 0);
        current_offset_ = std::exchange(other.current_offset_, 0);
        range_progress_ = std::exchange(other.range_progress_, 0);
        range_index_ = std::exchange(other.range_index_, 0);
    }
    return *this;
}

http_byte_range_slicer::http_byte_range_slicer(const http_byte_range_set& ranges, http_content_length representation_length, std::string_view media_type, std::string_view boundary) :
    http_byte_range_slicer(ranges.ignored() ? std::span<const http_byte_range>{} : ranges.ranges(), representation_length, media_type, boundary) {}

http_byte_range_slicer::http_byte_range_slicer(std::span<const http_byte_range> ranges, http_content_length representation_length_value, std::string_view media_type, std::string_view boundary) :
    representation_length_(representation_length_value.value()) {
    if (ranges.empty() || representation_length_ == 0) {
        throw std::invalid_argument("HTTP byte-range slicer requires at least one range");
    }
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        const auto range = ranges[index];
        if (range.length_ == 0 || range.offset_ > representation_length_ || range.length_ > representation_length_ - range.offset_) {
            throw std::invalid_argument("HTTP byte range is outside representation");
        }
        if (index > 0) {
            const auto prev = ranges[index - 1];
            if (range.offset_ < prev.offset_ + prev.length_) {
                throw std::invalid_argument("HTTP byte-range slicer requires ascending non-overlapping ranges");
            }
        }
    }
    if (ranges.size() == 1) {
        if (!media_type.empty() && !is_valid_http_content_type_field_value(media_type)) {
            throw std::invalid_argument("invalid HTTP media type for byte-range slicer");
        }
        single_mode_ = true;
        single_range_ = ranges.front();
        if (!media_type.empty()) {
            content_type_.assign(media_type);
        }
        std::size_t pos = 0;
        const auto append_sv = [&](std::string_view sv) noexcept {
            std::memcpy(content_range_.data() + pos, sv.data(), sv.size());
            pos += sv.size();
        };
        const auto append_u64 = [&](std::uint64_t val) noexcept {
            std::array<char, max_decimal_size> dec{};
            append_sv(format_decimal(dec, val));
        };
        append_sv("bytes ");
        append_u64(single_range_.offset_);
        content_range_[pos++] = '-';
        append_u64(single_range_.offset_ + single_range_.length_ - 1);
        content_range_[pos++] = '/';
        append_u64(representation_length_);
        content_range_size_ = static_cast<std::uint8_t>(pos);
        content_length_ = single_range_.length_;
        return;
    }
    if (!is_valid_http_content_type_field_value(media_type) || !multipart_boundary::try_parse(boundary).has_value()) {
        throw std::invalid_argument("invalid multipart media type or boundary for byte-range slicer");
    }
    multipart_range_size size(boundary.size(), representation_length_);
    if (!size.add_all(ranges, media_type.size())) {
        throw std::length_error("multipart byte-range response content length overflows uint64_t");
    }
    content_length_ = size.total_;
    write_multipart_range_type(content_type_, boundary);
    ranges_.assign(ranges.begin(), ranges.end());
    media_type_.assign(media_type);
    boundary_.assign(boundary);
}

void http_byte_range_slicer::write(std::string_view chunk, std::string& output) {
    write(current_offset_, chunk, output);
}

void http_byte_range_slicer::write(std::uint64_t chunk_offset, std::string_view chunk, std::string& output) {
    const std::size_t total_ranges = range_count();
    if (total_ranges == 0) {
        throw std::logic_error("HTTP byte-range slicer has no ranges");
    }
    if (chunk_offset < current_offset_ || chunk_offset > representation_length_ || chunk.size() > representation_length_ - chunk_offset) {
        throw std::invalid_argument("HTTP byte-range slicer chunk offset is out of bounds");
    }
    if (range_index_ == total_ranges) {
        current_offset_ = chunk_offset + chunk.size();
        return;
    }
    const auto needed_offset = range_at(range_index_).offset_ + range_progress_;
    if (chunk_offset > needed_offset) {
        throw std::invalid_argument("HTTP byte-range slicer chunk skipped over required range bytes");
    }
    const std::uint64_t chunk_end = chunk_offset + chunk.size();
    if (chunk_end <= needed_offset) {
        current_offset_ = chunk_end;
        return;
    }

    // Multipart framing can overwrite the terminator or invalidate the source
    // before its next slice. The single-range append handles overlap directly.
    if (multipart() && detail::http_text_aliases_string(chunk, output)) {
        const std::string chunk_copy(chunk);
        write(chunk_offset, chunk_copy, output);
        return;
    }
    const auto output_size = output.size();
    auto index = range_index_;
    auto progress = range_progress_;
    try {
        while (index < total_ranges) {
            const auto range = range_at(index);
            const std::uint64_t next_needed = range.offset_ + progress;
            if (chunk_end <= next_needed) {
                break;
            }
            if (progress == 0 && multipart()) {
                append_range_part_head(output, boundary_, media_type_, range, representation_length_);
            }
            const auto slice_start = static_cast<std::size_t>(next_needed - chunk_offset);
            const auto copy_bytes = static_cast<std::size_t>((std::min<std::uint64_t>)(range.length_ - progress, chunk.size() - slice_start));
            detail::append_http_text(output, chunk.substr(slice_start, copy_bytes));
            progress += copy_bytes;
            if (progress != range.length_) {
                break;
            }
            if (multipart()) {
                output.append(part_end);
            }
            ++index;
            progress = 0;
        }
        if (index == total_ranges && multipart()) {
            append_multipart_range_end(output, boundary_);
        }
    } catch (...) {
        // All writes append; shrinking restores the original bytes without allocation.
        output.erase(output_size);
        throw;
    }
    current_offset_ = chunk_end;
    range_index_ = index;
    range_progress_ = progress;
}

void http_byte_range_slicer::finish(std::string&) {
    if (!complete()) {
        throw std::logic_error("HTTP byte-range slicer finished before all ranges completed");
    }
}

void http_byte_range_slicer::reset(std::uint64_t start_offset) noexcept {
    current_offset_ = start_offset;
    range_progress_ = 0;
    range_index_ = 0;
}

}  // namespace ruvia
