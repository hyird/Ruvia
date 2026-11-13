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

redis_value::redis_value() noexcept = default;

redis_value& redis_value::operator=(const redis_value& other) {
    if (this != &other) {
        redis_value owned(other);
        string_.swap(owned.string_);
        array_.swap(owned.array_);
        kind_ = owned.kind_;
        integer_ = owned.integer_;
    }
    return *this;
}

redis_value& redis_value::operator=(redis_value&& other) noexcept {
    if (this != &other) {
        redis_value owned(std::move(other));
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

redis_value redis_value::null_value() noexcept {
    redis_value value;
    value.kind_ = kind_type::null;
    return value;
}

redis_value redis_value::string_value(std::string_view input) {
    redis_value value;
    value.kind_ = kind_type::string;
    value.string_.assign(input.data(), input.size());
    return value;
}

redis_value redis_value::error_value(std::string_view input) {
    redis_value value;
    value.kind_ = kind_type::error;
    value.string_.assign(input.data(), input.size());
    return value;
}

redis_value redis_value::integer_value(std::int64_t input) noexcept {
    redis_value value;
    value.kind_ = kind_type::integer;
    value.integer_ = input;
    return value;
}

redis_value redis_value::array_value(std::size_t capacity) {
    redis_value value;
    value.kind_ = kind_type::array;
    value.array_.reserve(capacity);
    return value;
}

}  // namespace ruvia
