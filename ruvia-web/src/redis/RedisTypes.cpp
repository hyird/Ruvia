#include "ruvia/web/redis/Redis.h"

#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/web/detail/redis/RedisUtils.h"

#include <stdexcept>
#include <utility>

namespace ruvia {

RedisError::RedisError(Code code, std::string_view message)
    : code_(code),
      message_(message, detail::processResource()) {}

RedisError::RedisError(const RedisError& other)
    : code_(other.code_),
      message_(other.message_, detail::processResource()) {}

RedisError& RedisError::operator=(const RedisError& other) {
    if (this != &other) {
        code_ = other.code_;
        message_ = other.message_;
    }
    return *this;
}

const char* RedisError::what() const noexcept {
    return message_.c_str();
}

RedisError::Code RedisError::code() const noexcept {
    return code_;
}

std::string_view RedisError::message() const & noexcept {
    return message_;
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

std::string_view RedisValue::string() const & {
    if (kind_ != Kind::kString && kind_ != Kind::kError) {
        throw std::logic_error("redis value is not a string");
    }
    return string_;
}

std::int64_t RedisValue::integer() const {
    if (kind_ != Kind::kInteger) {
        throw std::logic_error("redis value is not an integer");
    }
    return integer_;
}

std::span<const RedisValue> RedisValue::array() const & {
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
