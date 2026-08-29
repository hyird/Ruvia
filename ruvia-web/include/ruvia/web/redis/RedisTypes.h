#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/BorrowedText.h"
#include "ruvia/core/TcpSocketOptions.h"

namespace ruvia {

class RedisValue;
class RedisKeyValue;
class RedisScoredValue;
class RedisScanResult;
class RedisHashScanResult;
class RedisZScanResult;
class RedisXReadGroupResult;

struct RedisConfig {
    // Host name or unbracketed address only; keep the port in port.
    std::string host{"127.0.0.1"};
    // Must be non-zero.
    std::uint16_t port{6379};
    std::string username{};
    std::string password{};
    std::uint32_t database{0};
    // Must be greater than zero.
    std::size_t poolSizePerWorker{4};
    // Blocking commands are routed to a separate lazy-connect pool under the
    // same alias, so they cannot consume ordinary request/reply connections.
    // Must be greater than zero.
    std::size_t blockingPoolSizePerWorker{1};
    // Absence explicitly disables the corresponding timeout. connectTimeout is one
    // deadline shared by DNS resolution, TCP establishment, AUTH, and SELECT.
    // commandTimeout bounds a whole logical command or pipeline on the ordinary
    // pool, including write and every reply read, rather than restarting for
    // each I/O wait; startup commands honor the earlier of both deadlines.
    // The isolated blocking pool does not inherit this timeout: typed finite
    // waits derive a per-operation deadline from their Redis wait, and infinite
    // waits require an explicit StopToken or operation timeout.
    std::optional<std::chrono::milliseconds> connectTimeout{std::chrono::seconds(5)};
    std::optional<std::chrono::milliseconds> commandTimeout{std::chrono::seconds(30)};
    std::optional<std::chrono::milliseconds> acquireTimeout{std::chrono::seconds(5)};
    // Absence disables the reply byte limit.
    std::optional<std::size_t> maxReplyBytes{64 * 1024 * 1024};
    // Must be greater than zero.
    std::size_t maxArrayDepth{64};
    TcpNoDelayPolicy tcpNoDelay{TcpNoDelayPolicy::kEnable};
    TcpKeepAlivePolicy tcpKeepAlive{TcpKeepAlivePolicy::kSystemDefault};
};

class RedisBlockWait final {
public:
    [[nodiscard]] static RedisBlockWait forDuration(std::chrono::milliseconds duration) {
        if (duration.count() <= 0) {
            throw std::invalid_argument("redis block duration must be greater than zero");
        }
        return RedisBlockWait(duration);
    }

    [[nodiscard]] static RedisBlockWait indefinitely() noexcept {
        return RedisBlockWait(std::nullopt);
    }

    [[nodiscard]] bool infinite() const noexcept {
        return !duration_.has_value();
    }

    [[nodiscard]] std::optional<std::chrono::milliseconds> duration() const noexcept {
        return duration_;
    }

private:
    explicit RedisBlockWait(std::optional<std::chrono::milliseconds> duration) noexcept
        : duration_(duration) {}

    std::optional<std::chrono::milliseconds> duration_;
};

struct RedisStreamReadView final {
    BorrowedText stream{};
    BorrowedText id{};
};

enum class RedisXReadGroupAcknowledgementPolicy : std::uint8_t {
    kTrackPending,
    kNoAck,
};

struct RedisXReadGroupOptions final {
    std::optional<std::uint64_t> count{};
    std::optional<RedisBlockWait> block{};
    RedisXReadGroupAcknowledgementPolicy acknowledgement{
        RedisXReadGroupAcknowledgementPolicy::kTrackPending};
};

enum class RedisSetCondition : std::uint8_t {
    kIfAbsent,
    kIfPresent,
};

class RedisSetExpiration final {
public:
    [[nodiscard]] static RedisSetExpiration expiresAfter(std::chrono::milliseconds duration) {
        if (duration.count() <= 0) {
            throw std::invalid_argument("redis set expiration must be greater than zero");
        }
        return RedisSetExpiration(duration);
    }

    [[nodiscard]] static RedisSetExpiration keepExisting() noexcept {
        return RedisSetExpiration(KeepExisting{});
    }

    [[nodiscard]] const std::chrono::milliseconds* duration() const& noexcept {
        return std::get_if<std::chrono::milliseconds>(&value_);
    }
    [[nodiscard]] const std::chrono::milliseconds* duration() const&& = delete;

    [[nodiscard]] bool keepsExisting() const noexcept {
        return std::get_if<KeepExisting>(&value_) != nullptr;
    }

private:
    struct KeepExisting final {};
    using Value = std::variant<std::chrono::milliseconds, KeepExisting>;

    explicit RedisSetExpiration(std::chrono::milliseconds duration) noexcept
        : value_(duration) {}

    explicit RedisSetExpiration(KeepExisting keep) noexcept
        : value_(keep) {}

    Value value_;
};

enum class RedisSetPreviousValuePolicy : std::uint8_t {
    kDiscard,
    kReturn,
};

struct RedisSetOptions final {
    std::optional<RedisSetCondition> condition{};
    std::optional<RedisSetExpiration> expiration{};
    RedisSetPreviousValuePolicy previousValue{RedisSetPreviousValuePolicy::kDiscard};
};

namespace detail {
struct RedisTypesAccess;
}

class RedisSetResult final {
public:
    [[nodiscard]] constexpr bool applied() const noexcept {
        return applied_;
    }

    [[nodiscard]] const std::optional<std::pmr::string>& previous() const& noexcept {
        return previous_;
    }
    const std::optional<std::pmr::string>& previous() const&& = delete;

private:
    friend struct detail::RedisTypesAccess;

    RedisSetResult(bool applied, std::optional<std::pmr::string> previous) noexcept
        : applied_(applied),
          previous_(std::move(previous)) {}

    bool applied_{false};
    std::optional<std::pmr::string> previous_;
};

class RedisScanCursor final {
public:
    friend constexpr bool operator==(RedisScanCursor, RedisScanCursor) noexcept = default;

private:
    friend struct detail::RedisTypesAccess;

    explicit constexpr RedisScanCursor(std::uint64_t value) noexcept
        : value_(value) {}

    std::uint64_t value_{0};
};

enum class RedisTtlState : std::uint8_t {
    kMissing,
    kPersistent,
    kExpiring,
};

class RedisTtl final {
public:
    [[nodiscard]] constexpr RedisTtlState state() const noexcept {
        return state_;
    }

    [[nodiscard]] constexpr std::optional<std::chrono::milliseconds> remaining() const noexcept {
        return remaining_;
    }

private:
    friend struct detail::RedisTypesAccess;

    constexpr RedisTtl(
        RedisTtlState state, std::optional<std::chrono::milliseconds> remaining) noexcept
        : state_(state),
          remaining_(remaining) {}

    RedisTtlState state_;
    std::optional<std::chrono::milliseconds> remaining_;
};

struct RedisScanOptions {
    // A scan options value may be retained before the command copies its
    // arguments. Keep MATCH zero-copy while rejecting owning-string rvalues
    // that would leave a saved options value with an already-dangling view.
    std::optional<RedisScanCursor> cursor{};
    ::ruvia::BorrowedText match{};
    std::optional<std::uint64_t> count{};
};

class RedisKeyValue final {
public:
    RedisKeyValue(const RedisKeyValue&) = default;
    RedisKeyValue& operator=(const RedisKeyValue&) = default;
    RedisKeyValue(RedisKeyValue&&) noexcept = default;
    // A different PMR resource can require allocation during assignment.
    // NOLINTNEXTLINE(performance-noexcept-move-constructor)
    RedisKeyValue& operator=(RedisKeyValue&&) = default;

    [[nodiscard]] std::string_view key() const& noexcept {
        return key_;
    }
    [[nodiscard]] std::string_view key() const&& = delete;

    [[nodiscard]] std::string_view value() const& noexcept {
        return value_;
    }
    [[nodiscard]] std::string_view value() const&& = delete;

private:
    friend struct detail::RedisTypesAccess;

    RedisKeyValue(std::string_view key, std::string_view value, std::pmr::memory_resource* resource)
        : RedisKeyValue(key, value, detail::ResolvedPmrResourceTag{},
              detail::pmrResourceOrDefault(resource)) {}

    RedisKeyValue(std::string_view key, std::string_view value, detail::ResolvedPmrResourceTag,
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
    // A different PMR resource can require allocation during assignment.
    // NOLINTNEXTLINE(performance-noexcept-move-constructor)
    RedisScoredValue& operator=(RedisScoredValue&&) = default;

    [[nodiscard]] std::string_view value() const& noexcept {
        return value_;
    }
    [[nodiscard]] std::string_view value() const&& = delete;

    [[nodiscard]] double score() const noexcept {
        return score_;
    }

private:
    friend struct detail::RedisTypesAccess;

    RedisScoredValue(std::string_view value, double score, std::pmr::memory_resource* resource)
        : RedisScoredValue(value, score, detail::ResolvedPmrResourceTag{},
              detail::pmrResourceOrDefault(resource)) {}

    RedisScoredValue(std::string_view value, double score, detail::ResolvedPmrResourceTag,
        std::pmr::memory_resource* resource)
        : value_(value.data(), value.size(), resource),
          score_(score) {}

    std::pmr::string value_;
    double score_{0};
};

class RedisScanResult final {
public:
    [[nodiscard]] bool done() const noexcept {
        return !nextCursor_.has_value();
    }

    [[nodiscard]] std::optional<RedisScanCursor> nextCursor() const noexcept {
        return nextCursor_;
    }

    [[nodiscard]] std::span<const std::pmr::string> values() const& noexcept {
        return values_;
    }
    [[nodiscard]] std::span<const std::pmr::string> values() const&& = delete;

private:
    friend struct detail::RedisTypesAccess;

    explicit RedisScanResult(std::pmr::memory_resource* resource)
        : values_(detail::pmrResourceOrDefault(resource)) {}

    std::optional<RedisScanCursor> nextCursor_;
    std::pmr::vector<std::pmr::string> values_;
};

class RedisHashScanResult final {
public:
    [[nodiscard]] bool done() const noexcept {
        return !nextCursor_.has_value();
    }

    [[nodiscard]] std::optional<RedisScanCursor> nextCursor() const noexcept {
        return nextCursor_;
    }

    [[nodiscard]] std::span<const RedisKeyValue> entries() const& noexcept {
        return entries_;
    }
    [[nodiscard]] std::span<const RedisKeyValue> entries() const&& = delete;

private:
    friend struct detail::RedisTypesAccess;

    explicit RedisHashScanResult(std::pmr::memory_resource* resource)
        : entries_(detail::pmrResourceOrDefault(resource)) {}

    std::optional<RedisScanCursor> nextCursor_;
    std::pmr::vector<RedisKeyValue> entries_;
};

class RedisZScanResult final {
public:
    [[nodiscard]] bool done() const noexcept {
        return !nextCursor_.has_value();
    }

    [[nodiscard]] std::optional<RedisScanCursor> nextCursor() const noexcept {
        return nextCursor_;
    }

    [[nodiscard]] std::span<const RedisScoredValue> entries() const& noexcept {
        return entries_;
    }
    [[nodiscard]] std::span<const RedisScoredValue> entries() const&& = delete;

private:
    friend struct detail::RedisTypesAccess;

    explicit RedisZScanResult(std::pmr::memory_resource* resource)
        : entries_(detail::pmrResourceOrDefault(resource)) {}

    std::optional<RedisScanCursor> nextCursor_;
    std::pmr::vector<RedisScoredValue> entries_;
};

class RedisStreamEntry final {
public:
    RedisStreamEntry(const RedisStreamEntry&) = default;
    RedisStreamEntry& operator=(const RedisStreamEntry&) = default;
    RedisStreamEntry(RedisStreamEntry&&) noexcept = default;
    // A different PMR resource can require allocation during assignment.
    // NOLINTNEXTLINE(performance-noexcept-move-constructor)
    RedisStreamEntry& operator=(RedisStreamEntry&&) = default;

    [[nodiscard]] std::string_view id() const& noexcept {
        return id_;
    }
    [[nodiscard]] std::string_view id() const&& = delete;

    [[nodiscard]] std::span<const RedisKeyValue> fields() const& noexcept {
        return fields_;
    }
    [[nodiscard]] std::span<const RedisKeyValue> fields() const&& = delete;

private:
    friend struct detail::RedisTypesAccess;

    RedisStreamEntry(std::string_view id, std::pmr::memory_resource* resource)
        : id_(id.data(), id.size(), resource),
          fields_(resource) {}

    std::pmr::string id_;
    std::pmr::vector<RedisKeyValue> fields_;
};

class RedisStreamReadResult final {
public:
    RedisStreamReadResult(const RedisStreamReadResult&) = default;
    RedisStreamReadResult& operator=(const RedisStreamReadResult&) = default;
    RedisStreamReadResult(RedisStreamReadResult&&) noexcept = default;
    // A different PMR resource can require allocation during assignment.
    // NOLINTNEXTLINE(performance-noexcept-move-constructor)
    RedisStreamReadResult& operator=(RedisStreamReadResult&&) = default;

    [[nodiscard]] std::string_view stream() const& noexcept {
        return stream_;
    }
    [[nodiscard]] std::string_view stream() const&& = delete;

    [[nodiscard]] std::span<const RedisStreamEntry> entries() const& noexcept {
        return entries_;
    }
    [[nodiscard]] std::span<const RedisStreamEntry> entries() const&& = delete;

private:
    friend struct detail::RedisTypesAccess;

    RedisStreamReadResult(std::string_view stream, std::pmr::memory_resource* resource)
        : stream_(stream.data(), stream.size(), resource),
          entries_(resource) {}

    std::pmr::string stream_;
    std::pmr::vector<RedisStreamEntry> entries_;
};

class RedisXReadGroupResult final {
public:
    RedisXReadGroupResult(const RedisXReadGroupResult&) = default;
    RedisXReadGroupResult& operator=(const RedisXReadGroupResult&) = default;
    RedisXReadGroupResult(RedisXReadGroupResult&&) noexcept = default;
    RedisXReadGroupResult& operator=(RedisXReadGroupResult&&) = default;

    [[nodiscard]] std::span<const RedisStreamReadResult> streams() const& noexcept {
        return streams_;
    }
    [[nodiscard]] std::span<const RedisStreamReadResult> streams() const&& = delete;

private:
    friend struct detail::RedisTypesAccess;

    explicit RedisXReadGroupResult(std::pmr::memory_resource* resource)
        : streams_(resource) {}

    std::pmr::vector<RedisStreamReadResult> streams_;
};

namespace detail {

class RedisPool;
struct RedisCommandExecutor;
class RedisRegistry;

}  // namespace detail

class RedisError final : public std::runtime_error {
public:
    enum class Code : std::uint8_t {
        kNotConfigured,
        kConnectFailed,
        kAuthFailed,
        kProtocolError,
        kCommandError,
        kIoError,
        kTimeout,
        kCancelled,
        kClosing,
        kTransactionAborted
    };

    RedisError(Code code, std::string_view message);

    [[nodiscard]] Code code() const noexcept;

private:
    Code code_;
};

class RedisValue final {
public:
    enum class Kind : std::uint8_t { kNull,
        kString,
        kInteger,
        kArray,
        kError };

    RedisValue(const RedisValue&) = default;
    RedisValue& operator=(const RedisValue&) = default;
    RedisValue(RedisValue&&) noexcept = default;
    // A different PMR resource can require allocation during assignment.
    // NOLINTNEXTLINE(performance-noexcept-move-constructor)
    RedisValue& operator=(RedisValue&&) = default;

    [[nodiscard]] Kind kind() const noexcept;
    [[nodiscard]] bool null() const noexcept;
    [[nodiscard]] std::string_view string() const&;
    [[nodiscard]] std::string_view string() const&& = delete;
    [[nodiscard]] std::string_view error() const&;
    [[nodiscard]] std::string_view error() const&& = delete;
    [[nodiscard]] std::int64_t integer() const;
    [[nodiscard]] std::span<const RedisValue> array() const&;
    [[nodiscard]] std::span<const RedisValue> array() const&& = delete;

private:
    friend class detail::RedisPool;
    friend struct detail::RedisTypesAccess;

    explicit RedisValue(std::pmr::memory_resource* resource = nullptr);
    [[nodiscard]] static RedisValue nullValue(std::pmr::memory_resource* resource);
    [[nodiscard]] static RedisValue stringValue(
        std::string_view value, std::pmr::memory_resource* resource);
    [[nodiscard]] static RedisValue errorValue(
        std::string_view value, std::pmr::memory_resource* resource);
    [[nodiscard]] static RedisValue integerValue(
        std::int64_t value, std::pmr::memory_resource* resource);
    [[nodiscard]] static RedisValue arrayValue(
        std::pmr::vector<RedisValue> values, std::pmr::memory_resource* resource);

    RedisValue(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource);

    Kind kind_{Kind::kNull};
    std::pmr::string string_;
    std::int64_t integer_{0};
    std::pmr::vector<RedisValue> array_;
};

}  // namespace ruvia
