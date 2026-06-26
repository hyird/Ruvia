#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ruvia {

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
    // Set to 0 to disable the corresponding timeout.
    std::chrono::milliseconds connectTimeout{0};
    std::chrono::milliseconds commandTimeout{0};
    std::chrono::milliseconds acquireTimeout{0};
    // Set to 0 to disable the reply byte limit.
    std::size_t maxReplyBytes{64 * 1024 * 1024};
    // Must be greater than zero.
    std::size_t maxArrayDepth{64};
    bool tcpNoDelay{true};
    bool keepAlive{false};
};

struct RedisSetOptions {
    std::chrono::milliseconds ttl{0};
    bool nx{false};
    bool xx{false};
    bool get{false};
    bool keepTtl{false};
};

struct RedisScanOptions {
    std::uint64_t cursor{0};
    std::string_view match;
    std::uint64_t count{0};
};

struct RedisKeyValue {
    std::pmr::string key;
    std::pmr::string value;
};

struct RedisScoredValue {
    std::pmr::string value;
    double score{0};
};

struct RedisScanResult {
    std::uint64_t cursor{0};
    std::pmr::vector<std::pmr::string> values;
};

struct RedisHashScanResult {
    std::uint64_t cursor{0};
    std::pmr::vector<RedisKeyValue> entries;
};

struct RedisZScanResult {
    std::uint64_t cursor{0};
    std::pmr::vector<RedisScoredValue> entries;
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

    explicit RedisValue(std::pmr::memory_resource* resource = nullptr);

    RedisValue(const RedisValue&) = default;
    RedisValue& operator=(const RedisValue&) = default;
    RedisValue(RedisValue&&) noexcept = default;
    RedisValue& operator=(RedisValue&&) noexcept = default;

    [[nodiscard]] Kind kind() const noexcept;
    [[nodiscard]] bool null() const noexcept;
    [[nodiscard]] std::string_view string() const;
    [[nodiscard]] std::int64_t integer() const;
    [[nodiscard]] std::span<const RedisValue> array() const;

    [[nodiscard]] static RedisValue nullValue(std::pmr::memory_resource* resource);
    [[nodiscard]] static RedisValue stringValue(std::string_view value, std::pmr::memory_resource* resource);
    [[nodiscard]] static RedisValue errorValue(std::string_view value, std::pmr::memory_resource* resource);
    [[nodiscard]] static RedisValue integerValue(std::int64_t value, std::pmr::memory_resource* resource);
    [[nodiscard]] static RedisValue arrayValue(std::pmr::vector<RedisValue> values, std::pmr::memory_resource* resource);

private:
    friend class detail::RedisPool;

    Kind kind_{Kind::kNull};
    std::pmr::string string_;
    std::int64_t integer_{0};
    std::pmr::vector<RedisValue> array_;
};

}  // namespace ruvia
