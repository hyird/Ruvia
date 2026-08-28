#include "ruvia/web/redis/Redis.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace ruvia {

RedisError::RedisError(Code code, std::string_view message)
    : std::runtime_error(std::string(message)),
      code_(code) {}

RedisError::Code RedisError::code() const noexcept {
    return code_;
}

RedisValue::RedisValue(std::pmr::memory_resource* resource)
    : RedisValue(detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(resource)) {}

RedisValue::RedisValue(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
    : string_(resource),
      array_(resource) {}

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

RedisValue RedisValue::arrayValue(std::pmr::vector<RedisValue> values, std::pmr::memory_resource* resource) {
    RedisValue value(resource);
    value.kind_ = Kind::kArray;
    value.array_ = std::move(values);
    return value;
}

}  // namespace ruvia
