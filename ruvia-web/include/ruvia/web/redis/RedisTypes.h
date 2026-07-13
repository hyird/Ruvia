#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/core/memory/PmrResource.h"

namespace ruvia {

class RedisValue;
class RedisKeyValue;
class RedisScoredValue;
class RedisScanResult;
class RedisHashScanResult;
class RedisZScanResult;

struct RedisConfig {
    // Host name or unbracketed address only; keep the port in port.
    std::pmr::string host{"127.0.0.1"};
    // Must be non-zero.
    std::uint16_t port{6379};
    std::pmr::string username;
    std::pmr::string password;
    std::uint32_t database{0};
    // Must be greater than zero.
    std::size_t poolSizePerWorker{4};
    // Absence disables the corresponding timeout.
    std::optional<std::chrono::milliseconds> connectTimeout;
    std::optional<std::chrono::milliseconds> commandTimeout;
    std::optional<std::chrono::milliseconds> acquireTimeout;
    // Absence disables the reply byte limit.
    std::optional<std::size_t> maxReplyBytes{64 * 1024 * 1024};
    // Must be greater than zero.
    std::size_t maxArrayDepth{64};
    bool tcpNoDelay{true};
    bool keepAlive{false};
};

enum class RedisSetCondition : std::uint8_t {
    kIfAbsent,
    kIfPresent,
};

class RedisSetExpiration final {
public:
    [[nodiscard]] static RedisSetExpiration expiresAfter(
        std::chrono::milliseconds duration) {
        if (duration.count() <= 0) {
            throw std::invalid_argument(
                "redis set expiration must be greater than zero");
        }
        return RedisSetExpiration(duration);
    }

    [[nodiscard]] static RedisSetExpiration keepExisting() noexcept {
        return RedisSetExpiration(KeepExisting{});
    }

    [[nodiscard]] const std::chrono::milliseconds* duration() const noexcept {
        return std::get_if<std::chrono::milliseconds>(&value_);
    }

    [[nodiscard]] bool keepsExisting() const noexcept {
        return std::get_if<KeepExisting>(&value_) != nullptr;
    }

private:
    struct KeepExisting final {};
    using Value = std::variant<
        std::chrono::milliseconds,
        KeepExisting>;

    explicit RedisSetExpiration(std::chrono::milliseconds duration) noexcept
        : value_(duration) {}

    explicit RedisSetExpiration(KeepExisting keep) noexcept
        : value_(keep) {}

    Value value_;
};

struct RedisSetOptions final {
    std::optional<RedisSetCondition> condition;
    std::optional<RedisSetExpiration> expiration;
    bool returnPrevious{false};
};

struct RedisScanOptions {
    std::uint64_t cursor{0};
    std::string_view match;
    std::optional<std::uint64_t> count;
};

namespace detail {

struct RedisTypesAccess;

}  // namespace detail

class RedisKeyValue final {
public:
    RedisKeyValue(const RedisKeyValue&) = default;
    RedisKeyValue& operator=(const RedisKeyValue&) = default;
    RedisKeyValue(RedisKeyValue&&) noexcept = default;
    RedisKeyValue& operator=(RedisKeyValue&&) noexcept = default;

    [[nodiscard]] std::string_view key() const noexcept {
        return std::string_view(key_.data(), key_.size());
    }

    [[nodiscard]] std::string_view value() const noexcept {
        return std::string_view(value_.data(), value_.size());
    }

private:
    friend struct detail::RedisTypesAccess;

    RedisKeyValue(std::string_view key, std::string_view value, std::pmr::memory_resource* resource)
        : RedisKeyValue(key, value, detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(resource)) {}

    RedisKeyValue(
        std::string_view key,
        std::string_view value,
        detail::ResolvedPmrResourceTag,
        std::pmr::memory_resource* resource)
        : key_(key.data(), key.size(), resource),
          value_(value.data(), value.size(), resource) {}

    std::pmr::string key_;
    std::pmr::string value_;
};

class RedisScoredValue final {
public:
    RedisScoredValue(const RedisScoredValue&) = default;
    RedisScoredValue& operator=(const RedisScoredValue&) = default;
    RedisScoredValue(RedisScoredValue&&) noexcept = default;
    RedisScoredValue& operator=(RedisScoredValue&&) noexcept = default;

    [[nodiscard]] std::string_view value() const noexcept {
        return std::string_view(value_.data(), value_.size());
    }

    [[nodiscard]] double score() const noexcept {
        return score_;
    }

private:
    friend struct detail::RedisTypesAccess;

    RedisScoredValue(std::string_view value, double score, std::pmr::memory_resource* resource)
        : RedisScoredValue(value, score, detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(resource)) {}

    RedisScoredValue(
        std::string_view value,
        double score,
        detail::ResolvedPmrResourceTag,
        std::pmr::memory_resource* resource)
        : value_(value.data(), value.size(), resource),
          score_(score) {}

    std::pmr::string value_;
    double score_{0};
};

class RedisScanResult final {
public:
    [[nodiscard]] std::uint64_t cursor() const noexcept {
        return cursor_;
    }

    [[nodiscard]] std::span<const std::pmr::string> values() const noexcept {
        return std::span<const std::pmr::string>(values_.data(), values_.size());
    }

private:
    friend struct detail::RedisTypesAccess;

    explicit RedisScanResult(std::pmr::memory_resource* resource)
        : values_(detail::pmrResourceOrDefault(resource)) {}

    std::uint64_t cursor_{0};
    std::pmr::vector<std::pmr::string> values_;
};

class RedisHashScanResult final {
public:
    [[nodiscard]] std::uint64_t cursor() const noexcept {
        return cursor_;
    }

    [[nodiscard]] std::span<const RedisKeyValue> entries() const noexcept {
        return std::span<const RedisKeyValue>(entries_.data(), entries_.size());
    }

private:
    friend struct detail::RedisTypesAccess;

    explicit RedisHashScanResult(std::pmr::memory_resource* resource)
        : entries_(detail::pmrResourceOrDefault(resource)) {}

    std::uint64_t cursor_{0};
    std::pmr::vector<RedisKeyValue> entries_;
};

class RedisZScanResult final {
public:
    [[nodiscard]] std::uint64_t cursor() const noexcept {
        return cursor_;
    }

    [[nodiscard]] std::span<const RedisScoredValue> entries() const noexcept {
        return std::span<const RedisScoredValue>(entries_.data(), entries_.size());
    }

private:
    friend struct detail::RedisTypesAccess;

    explicit RedisZScanResult(std::pmr::memory_resource* resource)
        : entries_(detail::pmrResourceOrDefault(resource)) {}

    std::uint64_t cursor_{0};
    std::pmr::vector<RedisScoredValue> entries_;
};

namespace detail {

inline constexpr std::string_view kDefaultRedisAlias = "default";

struct RedisDefinition final {
    std::pmr::string alias;
    RedisConfig config;
};

class RedisPool;
class RedisRegistry;

}  // namespace detail

class RedisError : public std::exception {
public:
    enum class Code {
        kNotConfigured,
        kPoolExhausted,
        kConnectFailed,
        kAuthFailed,
        kProtocolError,
        kCommandError,
        kIoError,
        kTimeout,
        kTransactionAborted
    };

    RedisError(Code code, std::string_view message);
    RedisError(const RedisError& other);
    RedisError& operator=(const RedisError& other);
    RedisError(RedisError&&) noexcept = default;
    RedisError& operator=(RedisError&&) noexcept = default;

    [[nodiscard]] const char* what() const noexcept override;
    [[nodiscard]] Code code() const noexcept;
    [[nodiscard]] std::string_view message() const noexcept;

private:
    Code code_;
    std::pmr::string message_;
};

class RedisValue final {
public:
    enum class Kind {
        kNull,
        kString,
        kInteger,
        kArray,
        kError
    };

    RedisValue(const RedisValue&) = default;
    RedisValue& operator=(const RedisValue&) = default;
    RedisValue(RedisValue&&) noexcept = default;
    RedisValue& operator=(RedisValue&&) noexcept = default;

    [[nodiscard]] Kind kind() const noexcept;
    [[nodiscard]] bool null() const noexcept;
    [[nodiscard]] std::string_view string() const;
    [[nodiscard]] std::int64_t integer() const;
    [[nodiscard]] std::span<const RedisValue> array() const;

private:
    friend class detail::RedisPool;
    friend struct detail::RedisTypesAccess;

    explicit RedisValue(std::pmr::memory_resource* resource = nullptr);
    [[nodiscard]] static RedisValue nullValue(std::pmr::memory_resource* resource);
    [[nodiscard]] static RedisValue stringValue(std::string_view value, std::pmr::memory_resource* resource);
    [[nodiscard]] static RedisValue errorValue(std::string_view value, std::pmr::memory_resource* resource);
    [[nodiscard]] static RedisValue integerValue(std::int64_t value, std::pmr::memory_resource* resource);
    [[nodiscard]] static RedisValue arrayValue(std::pmr::vector<RedisValue> values, std::pmr::memory_resource* resource);

    RedisValue(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource);

    Kind kind_{Kind::kNull};
    std::pmr::string string_;
    std::int64_t integer_{0};
    std::pmr::vector<RedisValue> array_;
};

}  // namespace ruvia
