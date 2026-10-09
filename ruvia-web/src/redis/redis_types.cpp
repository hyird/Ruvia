#include <stdexcept>
#include <string>
#include <utility>

#include "ruvia/web/redis/redis.h"

namespace ruvia {

redis_error::redis_error(code_type code, std::string_view message)
    : std::runtime_error(std::string(message)),
      code_(code) {}

redis_error::code_type redis_error::code() const noexcept {
    return code_;
}

redis_key_value::redis_key_value(const redis_key_value& other, allocator_type allocator)
    : key_(other.key_, allocator.resource()),
      value_(other.value_, allocator.resource()) {}

redis_key_value::redis_key_value(redis_key_value&& other, allocator_type allocator)
    : key_(std::move(other.key_), allocator.resource()),
      value_(std::move(other.value_), allocator.resource()) {}

redis_scored_value::redis_scored_value(const redis_scored_value& other, allocator_type allocator)
    : value_(other.value_, allocator.resource()),
      score_(other.score_) {}

redis_scored_value::redis_scored_value(redis_scored_value&& other, allocator_type allocator)
    : value_(std::move(other.value_), allocator.resource()),
      score_(other.score_) {}

redis_stream_entry::redis_stream_entry(const redis_stream_entry& other, allocator_type allocator)
    : id_(other.id_, allocator.resource()),
      fields_(other.fields_, allocator.resource()) {}

redis_stream_entry::redis_stream_entry(redis_stream_entry&& other, allocator_type allocator)
    : id_(std::move(other.id_), allocator.resource()),
      fields_(std::move(other.fields_), allocator.resource()) {}

redis_stream_read_result::redis_stream_read_result(const redis_stream_read_result& other, allocator_type allocator)
    : stream_(other.stream_, allocator.resource()),
      entries_(other.entries_, allocator.resource()) {}

redis_stream_read_result::redis_stream_read_result(redis_stream_read_result&& other, allocator_type allocator)
    : stream_(std::move(other.stream_), allocator.resource()),
      entries_(std::move(other.entries_), allocator.resource()) {}

redis_value::redis_value(std::pmr::memory_resource* resource)
    : redis_value(detail::resolved_pmr_resource_tag{}, detail::pmr_resource_or_default(resource)) {}

redis_value::redis_value(detail::resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
    : string_(resource),
      array_(resource) {}

redis_value::redis_value(const redis_value& other, allocator_type allocator)
    : kind_(other.kind_),
      string_(other.string_, allocator.resource()),
      integer_(other.integer_),
      array_(other.array_, allocator.resource()) {}

redis_value::redis_value(redis_value&& other, allocator_type allocator)
    : kind_(other.kind_),
      string_(std::move(other.string_), allocator.resource()),
      integer_(other.integer_),
      array_(std::move(other.array_), allocator.resource()) {}

redis_value::kind_type redis_value::kind() const noexcept {
    return kind_;
}

bool redis_value::null() const noexcept {
    return kind_ == kind_type::null;
}

std::string_view redis_value::string() const& {
    if (kind_ != kind_type::string) {
        throw std::logic_error("redis value is not a string");
    }
    return string_;
}

std::string_view redis_value::error() const& {
    if (kind_ != kind_type::error) {
        throw std::logic_error("redis value is not an error");
    }
    return string_;
}

std::int64_t redis_value::integer() const {
    if (kind_ != kind_type::integer) {
        throw std::logic_error("redis value is not an integer");
    }
    return integer_;
}

std::span<const redis_value> redis_value::array() const& {
    if (kind_ != kind_type::array) {
        throw std::logic_error("redis value is not an array");
    }
    return array_;
}

redis_value redis_value::null_value(std::pmr::memory_resource* resource) {
    redis_value value(resource);
    value.kind_ = kind_type::null;
    return value;
}

redis_value redis_value::string_value(std::string_view input, std::pmr::memory_resource* resource) {
    redis_value value(resource);
    value.kind_ = kind_type::string;
    value.string_.assign(input.data(), input.size());
    return value;
}

redis_value redis_value::error_value(std::string_view input, std::pmr::memory_resource* resource) {
    redis_value value(resource);
    value.kind_ = kind_type::error;
    value.string_.assign(input.data(), input.size());
    return value;
}

redis_value redis_value::integer_value(std::int64_t input, std::pmr::memory_resource* resource) {
    redis_value value(resource);
    value.kind_ = kind_type::integer;
    value.integer_ = input;
    return value;
}

redis_value redis_value::array_value(
    std::pmr::vector<redis_value> values, std::pmr::memory_resource* resource) {
    redis_value value(resource);
    value.kind_ = kind_type::array;
    value.array_ = std::move(values);
    return value;
}

}  // namespace ruvia
