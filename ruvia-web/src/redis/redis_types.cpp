#include <memory_resource>
#include <stdexcept>
#include <string>
#include <utility>

#include "ruvia/web/redis/redis.h"

#include "util/pmr_owned_storage.h"

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

redis_scan_result& redis_scan_result::operator=(redis_scan_result&& other) {
    if (this != &other) {
        auto owned = detail::rebind_owned_storage(other.values_, values_.get_allocator().resource());
        values_.swap(owned);
        next_cursor_ = std::exchange(other.next_cursor_, std::nullopt);
        other.values_.clear();
    }
    return *this;
}

redis_hash_scan_result& redis_hash_scan_result::operator=(redis_hash_scan_result&& other) {
    if (this != &other) {
        auto owned = detail::rebind_owned_storage(other.entries_, entries_.get_allocator().resource());
        entries_.swap(owned);
        next_cursor_ = std::exchange(other.next_cursor_, std::nullopt);
        other.entries_.clear();
    }
    return *this;
}

redis_z_scan_result& redis_z_scan_result::operator=(redis_z_scan_result&& other) {
    if (this != &other) {
        auto owned = detail::rebind_owned_storage(other.entries_, entries_.get_allocator().resource());
        entries_.swap(owned);
        next_cursor_ = std::exchange(other.next_cursor_, std::nullopt);
        other.entries_.clear();
    }
    return *this;
}

redis_stream_entry::redis_stream_entry(const redis_stream_entry& other, allocator_type allocator)
    : id_(other.id_, allocator.resource()),
      fields_(other.fields_, allocator.resource()) {}

redis_stream_entry::redis_stream_entry(redis_stream_entry&& other, allocator_type allocator)
    : id_(detail::rebind_owned_storage(other.id_, allocator.resource(), other.fields_)),
      fields_(detail::rebind_owned_storage(other.fields_, allocator.resource(), other.id_)) {
    other.id_.clear();
    other.fields_.clear();
}

redis_stream_entry& redis_stream_entry::operator=(redis_stream_entry&& other) {
    if (this != &other) {
        redis_stream_entry owned(std::move(other), allocator_type(id_.get_allocator().resource()));
        id_.swap(owned.id_);
        fields_.swap(owned.fields_);
    }
    return *this;
}

redis_stream_read_result::redis_stream_read_result(const redis_stream_read_result& other, allocator_type allocator)
    : stream_(other.stream_, allocator.resource()),
      entries_(other.entries_, allocator.resource()) {}

redis_stream_read_result::redis_stream_read_result(redis_stream_read_result&& other, allocator_type allocator)
    : stream_(detail::rebind_owned_storage(other.stream_, allocator.resource(), other.entries_)),
      entries_(detail::rebind_owned_storage(other.entries_, allocator.resource(), other.stream_)) {
    other.stream_.clear();
    other.entries_.clear();
}

redis_stream_read_result& redis_stream_read_result::operator=(redis_stream_read_result&& other) {
    if (this != &other) {
        redis_stream_read_result owned(std::move(other), allocator_type(stream_.get_allocator().resource()));
        stream_.swap(owned.stream_);
        entries_.swap(owned.entries_);
    }
    return *this;
}

redis_x_read_group_result& redis_x_read_group_result::operator=(redis_x_read_group_result&& other) {
    if (this != &other) {
        auto owned = detail::rebind_owned_storage(other.streams_, streams_.get_allocator().resource());
        streams_.swap(owned);
        other.streams_.clear();
    }
    return *this;
}

redis_value::redis_value(std::pmr::memory_resource* resource)
    : redis_value(resolved_pmr_resource_tag{}, pmr_resource_or_default(resource)) {}

redis_value::redis_value(resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
    : string_(resource),
      array_(resource) {}

redis_value::redis_value(const redis_value& other, allocator_type allocator)
    : kind_(other.kind_),
      string_(other.string_, allocator.resource()),
      integer_(other.integer_),
      array_(other.array_, allocator.resource()) {}

redis_value::redis_value(redis_value&& other, allocator_type allocator)
    : kind_(other.kind_),
      string_(detail::rebind_owned_storage(other.string_, allocator.resource(), other.array_)),
      integer_(other.integer_),
      array_(detail::rebind_owned_storage(other.array_, allocator.resource(), other.string_)) {
    other.kind_ = kind_type::null;
    other.integer_ = 0;
    other.string_.clear();
    other.array_.clear();
}

redis_value& redis_value::operator=(redis_value&& other) {
    if (this != &other) {
        redis_value owned(std::move(other), allocator_type(string_.get_allocator().resource()));
        string_.swap(owned.string_);
        array_.swap(owned.array_);
        kind_ = owned.kind_;
        integer_ = owned.integer_;
    }
    return *this;
}

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

redis_value redis_value::array_value(std::size_t capacity, std::pmr::memory_resource* resource) {
    redis_value value(resource);
    value.kind_ = kind_type::array;
    value.array_.reserve(capacity);
    return value;
}

}  // namespace ruvia
