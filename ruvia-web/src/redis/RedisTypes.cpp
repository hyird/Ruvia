#include <stdexcept>
#include <string>
#include <utility>

#include "ruvia/web/redis/Redis.h"

namespace ruvia {

RedisError::RedisError(Code code, std::string_view message)
    : std::runtime_error(std::string(message)),
      code_(code) {}

RedisError::Code RedisError::code() const noexcept {
    return code_;
}

RedisKeyValue::RedisKeyValue(const RedisKeyValue& other, allocator_type allocator)
    : key_(other.key_, allocator.resource()),
      value_(other.value_, allocator.resource()) {}

RedisKeyValue::RedisKeyValue(RedisKeyValue&& other, allocator_type allocator)
    : key_(std::move(other.key_), allocator.resource()),
      value_(std::move(other.value_), allocator.resource()) {}

RedisScoredValue::RedisScoredValue(const RedisScoredValue& other, allocator_type allocator)
    : value_(other.value_, allocator.resource()),
      score_(other.score_) {}

RedisScoredValue::RedisScoredValue(RedisScoredValue&& other, allocator_type allocator)
    : value_(std::move(other.value_), allocator.resource()),
      score_(other.score_) {}

RedisStreamEntry::RedisStreamEntry(const RedisStreamEntry& other, allocator_type allocator)
    : id_(other.id_, allocator.resource()),
      fields_(other.fields_, allocator.resource()) {}

RedisStreamEntry::RedisStreamEntry(RedisStreamEntry&& other, allocator_type allocator)
    : id_(std::move(other.id_), allocator.resource()),
      fields_(std::move(other.fields_), allocator.resource()) {}

RedisStreamReadResult::RedisStreamReadResult(const RedisStreamReadResult& other, allocator_type allocator)
    : stream_(other.stream_, allocator.resource()),
      entries_(other.entries_, allocator.resource()) {}

RedisStreamReadResult::RedisStreamReadResult(RedisStreamReadResult&& other, allocator_type allocator)
    : stream_(std::move(other.stream_), allocator.resource()),
      entries_(std::move(other.entries_), allocator.resource()) {}

RedisValue::RedisValue(std::pmr::memory_resource* resource)
    : RedisValue(detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(resource)) {}

RedisValue::RedisValue(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
    : string_(resource),
      array_(resource) {}

RedisValue::RedisValue(const RedisValue& other, allocator_type allocator)
    : kind_(other.kind_),
      string_(other.string_, allocator.resource()),
      integer_(other.integer_),
      array_(other.array_, allocator.resource()) {}

RedisValue::RedisValue(RedisValue&& other, allocator_type allocator)
    : kind_(other.kind_),
      string_(std::move(other.string_), allocator.resource()),
      integer_(other.integer_),
      array_(std::move(other.array_), allocator.resource()) {}

RedisValue::Kind RedisValue::kind() const noexcept {
    return kind_;
}

bool RedisValue::null() const noexcept {
    return kind_ == Kind::kNull;
}

std::string_view RedisValue::string() const& {
    if (kind_ != Kind::kString) {
        throw std::logic_error("redis value is not a string");
    }
    return string_;
}

std::string_view RedisValue::error() const& {
    if (kind_ != Kind::kError) {
        throw std::logic_error("redis value is not an error");
    }
    return string_;
}

std::int64_t RedisValue::integer() const {
    if (kind_ != Kind::kInteger) {
        throw std::logic_error("redis value is not an integer");
    }
    return integer_;
}

std::span<const RedisValue> RedisValue::array() const& {
    if (kind_ != Kind::kArray) {
        throw std::logic_error("redis value is not an array");
    }
    return array_;
}

RedisValue RedisValue::nullValue(std::pmr::memory_resource* resource) {
    RedisValue value(resource);
    value.kind_ = Kind::kNull;
    return value;
}

RedisValue RedisValue::stringValue(std::string_view input, std::pmr::memory_resource* resource) {
    RedisValue value(resource);
    value.kind_ = Kind::kString;
    value.string_.assign(input.data(), input.size());
    return value;
}

RedisValue RedisValue::errorValue(std::string_view input, std::pmr::memory_resource* resource) {
    RedisValue value(resource);
    value.kind_ = Kind::kError;
    value.string_.assign(input.data(), input.size());
    return value;
}

RedisValue RedisValue::integerValue(std::int64_t input, std::pmr::memory_resource* resource) {
    RedisValue value(resource);
    value.kind_ = Kind::kInteger;
    value.integer_ = input;
    return value;
}

RedisValue RedisValue::arrayValue(
    std::pmr::vector<RedisValue> values, std::pmr::memory_resource* resource) {
    RedisValue value(resource);
    value.kind_ = Kind::kArray;
    value.array_ = std::move(values);
    return value;
}

}  // namespace ruvia
