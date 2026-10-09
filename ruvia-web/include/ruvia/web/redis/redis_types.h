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

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/tcp_socket_options.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/web/client_tls_config.h"

namespace ruvia {

class redis_value;
class redis_key_value;
class redis_scored_value;
class redis_scan_result;
class redis_hash_scan_result;
class redis_z_scan_result;
class redis_x_read_group_result;

struct redis_config {
    // Host name or unbracketed address only; keep the port in port.
    std::string host_{"127.0.0.1"};
    // Must be non-zero.
    std::uint16_t port_{6379};
    std::string username_{};
    std::string password_{};
    client_tls_config tls_{};
    std::uint32_t database_{0};
    // Must be greater than zero.
    std::size_t pool_size_per_worker_{4};
    // Blocking commands are routed to a separate lazy-connect pool under the
    // same alias, so they cannot consume ordinary request/reply connections.
    // Must be greater than zero.
    std::size_t blocking_pool_size_per_worker_{1};
    // Absence explicitly disables the corresponding timeout. connect_timeout is one
    // deadline shared by DNS resolution, TCP establishment, AUTH, and SELECT.
    // command_timeout bounds a whole logical command or pipeline on the ordinary
    // pool, including write and every reply read, rather than restarting for
    // each I/O wait; startup commands honor the earlier of both deadlines.
    // The isolated blocking pool does not inherit this timeout: typed finite
    // waits derive a per-operation deadline from their Redis wait, and infinite
    // waits require an explicit stop_token or operation timeout.
    std::optional<std::chrono::milliseconds> connect_timeout_{std::chrono::seconds(5)};
    std::optional<std::chrono::milliseconds> command_timeout_{std::chrono::seconds(30)};
    std::optional<std::chrono::milliseconds> acquire_timeout_{std::chrono::seconds(5)};
    // Absence disables the reply byte limit.
    std::optional<std::size_t> max_reply_bytes_{64 * 1024 * 1024};
    // Must be greater than zero.
    std::size_t max_array_depth_{64};
    tcp_no_delay_policy tcp_no_delay_{tcp_no_delay_policy::enable};
    tcp_keep_alive_policy tcp_keep_alive_{tcp_keep_alive_policy::system_default};
};

class redis_block_wait final {
public:
    [[nodiscard]] static redis_block_wait for_duration(std::chrono::milliseconds duration) {
        if (duration.count() <= 0) {
            throw std::invalid_argument("redis block duration must be greater than zero");
        }
        return redis_block_wait(duration);
    }

    [[nodiscard]] static redis_block_wait indefinitely() noexcept {
        return redis_block_wait(std::nullopt);
    }

    [[nodiscard]] bool infinite() const noexcept {
        return !duration_.has_value();
    }

    [[nodiscard]] std::optional<std::chrono::milliseconds> duration() const noexcept {
        return duration_;
    }

private:
    explicit redis_block_wait(std::optional<std::chrono::milliseconds> duration) noexcept
        : duration_(duration) {}

    std::optional<std::chrono::milliseconds> duration_;
};

struct redis_stream_read_view final {
    borrowed_text stream_{};
    borrowed_text id_{};
};

enum class redis_x_read_group_acknowledgement_policy : std::uint8_t {
    track_pending,
    no_ack,
};

struct redis_x_read_group_options final {
    std::optional<std::uint64_t> count_{};
    std::optional<redis_block_wait> block_{};
    redis_x_read_group_acknowledgement_policy acknowledgement_{
        redis_x_read_group_acknowledgement_policy::track_pending};
};

enum class redis_set_condition : std::uint8_t {
    if_absent,
    if_present,
};

class redis_set_expiration final {
public:
    [[nodiscard]] static redis_set_expiration expires_after(std::chrono::milliseconds duration) {
        if (duration.count() <= 0) {
            throw std::invalid_argument("redis set expiration must be greater than zero");
        }
        return redis_set_expiration(duration);
    }

    [[nodiscard]] static redis_set_expiration keep_existing() noexcept {
        return redis_set_expiration(keep_existing_type{});
    }

    [[nodiscard]] const std::chrono::milliseconds* duration() const& noexcept {
        return std::get_if<std::chrono::milliseconds>(&value_);
    }
    [[nodiscard]] const std::chrono::milliseconds* duration() const&& = delete;

    [[nodiscard]] bool keeps_existing() const noexcept {
        return std::get_if<keep_existing_type>(&value_) != nullptr;
    }

private:
    struct keep_existing_type final {};
    using value_type = std::variant<std::chrono::milliseconds, keep_existing_type>;

    explicit redis_set_expiration(std::chrono::milliseconds duration) noexcept
        : value_(duration) {}

    explicit redis_set_expiration(keep_existing_type keep) noexcept
        : value_(keep) {}

    value_type value_;
};

enum class redis_set_previous_value_policy : std::uint8_t {
    discard,
    return_value,
};

struct redis_set_options final {
    std::optional<redis_set_condition> condition_{};
    std::optional<redis_set_expiration> expiration_{};
    redis_set_previous_value_policy previous_value_{redis_set_previous_value_policy::discard};
};

namespace detail {
struct redis_types_access;
}

class redis_set_result final {
public:
    [[nodiscard]] constexpr bool applied() const noexcept {
        return applied_;
    }

    [[nodiscard]] const std::optional<std::pmr::string>& previous() const& noexcept {
        return previous_;
    }
    const std::optional<std::pmr::string>& previous() const&& = delete;

private:
    friend struct detail::redis_types_access;

    redis_set_result(bool applied, std::optional<std::pmr::string> previous) noexcept
        : applied_(applied),
          previous_(std::move(previous)) {}

    bool applied_{false};
    std::optional<std::pmr::string> previous_;
};

class redis_scan_cursor final {
public:
    friend constexpr bool operator==(redis_scan_cursor, redis_scan_cursor) noexcept = default;

private:
    friend struct detail::redis_types_access;

    explicit constexpr redis_scan_cursor(std::uint64_t value) noexcept
        : value_(value) {}

    std::uint64_t value_{0};
};

enum class redis_ttl_state : std::uint8_t {
    missing,
    persistent,
    expiring,
};

class redis_ttl final {
public:
    [[nodiscard]] constexpr redis_ttl_state state() const noexcept {
        return state_;
    }

    [[nodiscard]] constexpr std::optional<std::chrono::milliseconds> remaining() const noexcept {
        return remaining_;
    }

private:
    friend struct detail::redis_types_access;

    constexpr redis_ttl(
        redis_ttl_state state_value, std::optional<std::chrono::milliseconds> remaining) noexcept
        : state_(state_value),
          remaining_(remaining) {}

    redis_ttl_state state_;
    std::optional<std::chrono::milliseconds> remaining_;
};

struct redis_scan_options {
    // A scan options value may be retained before the command copies its
    // arguments. Keep MATCH zero-copy while rejecting owning-string rvalues
    // that would leave a saved options value with an already-dangling view.
    std::optional<redis_scan_cursor> cursor_{};
    ::ruvia::borrowed_text match_{};
    std::optional<std::uint64_t> count_{};
};

class redis_key_value final {
public:
    using allocator_type = std::pmr::polymorphic_allocator<std::byte>;
    redis_key_value(const redis_key_value& other, allocator_type allocator);
    redis_key_value(redis_key_value&& other, allocator_type allocator);
    redis_key_value(const redis_key_value&) = default;
    redis_key_value& operator=(const redis_key_value&) = default;
    redis_key_value(redis_key_value&&) noexcept = default;
    // A different PMR resource can require allocation during assignment.
    // NOLINTNEXTLINE(performance-noexcept-move-constructor)
    redis_key_value& operator=(redis_key_value&&) = default;

    [[nodiscard]] std::string_view key() const& noexcept {
        return key_;
    }
    [[nodiscard]] std::string_view key() const&& = delete;

    [[nodiscard]] std::string_view value() const& noexcept {
        return value_;
    }
    [[nodiscard]] std::string_view value() const&& = delete;

private:
    friend struct detail::redis_types_access;

    redis_key_value(std::string_view key, std::string_view value, std::pmr::memory_resource* resource)
        : redis_key_value(key, value, detail::resolved_pmr_resource_tag{},
              detail::pmr_resource_or_default(resource)) {}

    redis_key_value(std::string_view key, std::string_view value, detail::resolved_pmr_resource_tag,
        std::pmr::memory_resource* resource)
        : key_(key.data(), key.size(), resource),
          value_(value.data(), value.size(), resource) {}

    std::pmr::string key_;
    std::pmr::string value_;
};

class redis_scored_value final {
public:
    using allocator_type = std::pmr::polymorphic_allocator<std::byte>;
    redis_scored_value(const redis_scored_value& other, allocator_type allocator);
    redis_scored_value(redis_scored_value&& other, allocator_type allocator);
    redis_scored_value(const redis_scored_value&) = default;
    redis_scored_value& operator=(const redis_scored_value&) = default;
    redis_scored_value(redis_scored_value&&) noexcept = default;
    // A different PMR resource can require allocation during assignment.
    // NOLINTNEXTLINE(performance-noexcept-move-constructor)
    redis_scored_value& operator=(redis_scored_value&&) = default;

    [[nodiscard]] std::string_view value() const& noexcept {
        return value_;
    }
    [[nodiscard]] std::string_view value() const&& = delete;

    [[nodiscard]] double score() const noexcept {
        return score_;
    }

private:
    friend struct detail::redis_types_access;

    redis_scored_value(std::string_view value, double score, std::pmr::memory_resource* resource)
        : redis_scored_value(value, score, detail::resolved_pmr_resource_tag{},
              detail::pmr_resource_or_default(resource)) {}

    redis_scored_value(std::string_view value, double score, detail::resolved_pmr_resource_tag,
        std::pmr::memory_resource* resource)
        : value_(value.data(), value.size(), resource),
          score_(score) {}

    std::pmr::string value_;
    double score_{0};
};

class redis_scan_result final {
public:
    [[nodiscard]] bool done() const noexcept {
        return !next_cursor_.has_value();
    }

    [[nodiscard]] std::optional<redis_scan_cursor> next_cursor() const noexcept {
        return next_cursor_;
    }

    [[nodiscard]] std::span<const std::pmr::string> values() const& noexcept {
        return values_;
    }
    [[nodiscard]] std::span<const std::pmr::string> values() const&& = delete;

private:
    friend struct detail::redis_types_access;

    explicit redis_scan_result(std::pmr::memory_resource* resource)
        : values_(detail::pmr_resource_or_default(resource)) {}

    std::optional<redis_scan_cursor> next_cursor_;
    std::pmr::vector<std::pmr::string> values_;
};

class redis_hash_scan_result final {
public:
    [[nodiscard]] bool done() const noexcept {
        return !next_cursor_.has_value();
    }

    [[nodiscard]] std::optional<redis_scan_cursor> next_cursor() const noexcept {
        return next_cursor_;
    }

    [[nodiscard]] std::span<const redis_key_value> entries() const& noexcept {
        return entries_;
    }
    [[nodiscard]] std::span<const redis_key_value> entries() const&& = delete;

private:
    friend struct detail::redis_types_access;

    explicit redis_hash_scan_result(std::pmr::memory_resource* resource)
        : entries_(detail::pmr_resource_or_default(resource)) {}

    std::optional<redis_scan_cursor> next_cursor_;
    std::pmr::vector<redis_key_value> entries_;
};

class redis_z_scan_result final {
public:
    [[nodiscard]] bool done() const noexcept {
        return !next_cursor_.has_value();
    }

    [[nodiscard]] std::optional<redis_scan_cursor> next_cursor() const noexcept {
        return next_cursor_;
    }

    [[nodiscard]] std::span<const redis_scored_value> entries() const& noexcept {
        return entries_;
    }
    [[nodiscard]] std::span<const redis_scored_value> entries() const&& = delete;

private:
    friend struct detail::redis_types_access;

    explicit redis_z_scan_result(std::pmr::memory_resource* resource)
        : entries_(detail::pmr_resource_or_default(resource)) {}

    std::optional<redis_scan_cursor> next_cursor_;
    std::pmr::vector<redis_scored_value> entries_;
};

class redis_stream_entry final {
public:
    using allocator_type = std::pmr::polymorphic_allocator<std::byte>;
    redis_stream_entry(const redis_stream_entry& other, allocator_type allocator);
    redis_stream_entry(redis_stream_entry&& other, allocator_type allocator);
    redis_stream_entry(const redis_stream_entry&) = default;
    redis_stream_entry& operator=(const redis_stream_entry&) = default;
    redis_stream_entry(redis_stream_entry&&) noexcept = default;
    // A different PMR resource can require allocation during assignment.
    // NOLINTNEXTLINE(performance-noexcept-move-constructor)
    redis_stream_entry& operator=(redis_stream_entry&&) = default;

    [[nodiscard]] std::string_view id() const& noexcept {
        return id_;
    }
    [[nodiscard]] std::string_view id() const&& = delete;

    // XREADGROUP retains the ID of a deleted pending message with no fields.
    [[nodiscard]] std::span<const redis_key_value> fields() const& noexcept {
        return fields_;
    }
    [[nodiscard]] std::span<const redis_key_value> fields() const&& = delete;

private:
    friend struct detail::redis_types_access;

    redis_stream_entry(std::string_view id, std::pmr::memory_resource* resource)
        : id_(id.data(), id.size(), resource),
          fields_(resource) {}

    std::pmr::string id_;
    std::pmr::vector<redis_key_value> fields_;
};

class redis_stream_read_result final {
public:
    using allocator_type = std::pmr::polymorphic_allocator<std::byte>;
    redis_stream_read_result(const redis_stream_read_result& other, allocator_type allocator);
    redis_stream_read_result(redis_stream_read_result&& other, allocator_type allocator);
    redis_stream_read_result(const redis_stream_read_result&) = default;
    redis_stream_read_result& operator=(const redis_stream_read_result&) = default;
    redis_stream_read_result(redis_stream_read_result&&) noexcept = default;
    // A different PMR resource can require allocation during assignment.
    // NOLINTNEXTLINE(performance-noexcept-move-constructor)
    redis_stream_read_result& operator=(redis_stream_read_result&&) = default;

    [[nodiscard]] std::string_view stream() const& noexcept {
        return stream_;
    }
    [[nodiscard]] std::string_view stream() const&& = delete;

    [[nodiscard]] std::span<const redis_stream_entry> entries() const& noexcept {
        return entries_;
    }
    [[nodiscard]] std::span<const redis_stream_entry> entries() const&& = delete;

private:
    friend struct detail::redis_types_access;

    redis_stream_read_result(std::string_view stream, std::pmr::memory_resource* resource)
        : stream_(stream.data(), stream.size(), resource),
          entries_(resource) {}

    std::pmr::string stream_;
    std::pmr::vector<redis_stream_entry> entries_;
};

class redis_x_read_group_result final {
public:
    redis_x_read_group_result(const redis_x_read_group_result&) = default;
    redis_x_read_group_result& operator=(const redis_x_read_group_result&) = default;
    redis_x_read_group_result(redis_x_read_group_result&&) noexcept = default;
    redis_x_read_group_result& operator=(redis_x_read_group_result&&) = default;

    [[nodiscard]] std::span<const redis_stream_read_result> streams() const& noexcept {
        return streams_;
    }
    [[nodiscard]] std::span<const redis_stream_read_result> streams() const&& = delete;

private:
    friend struct detail::redis_types_access;

    explicit redis_x_read_group_result(std::pmr::memory_resource* resource)
        : streams_(resource) {}

    std::pmr::vector<redis_stream_read_result> streams_;
};

namespace detail {

class redis_pool;
struct redis_command_executor;
class redis_registry;

}  // namespace detail

class redis_error final : public std::runtime_error {
public:
    enum class code_type : std::uint8_t {
        not_configured,
        connect_failed,
        auth_failed,
        protocol_error,
        command_error,
        io_error,
        timeout,
        cancelled,
        closing,
        transaction_aborted
    };

    redis_error(code_type code, std::string_view message);

    [[nodiscard]] code_type code() const noexcept;

private:
    code_type code_;
};

class redis_value final {
public:
    // PMR containers construct every nested result in their own resource.
    using allocator_type = std::pmr::polymorphic_allocator<std::byte>;
    redis_value(const redis_value& other, allocator_type allocator);
    redis_value(redis_value&& other, allocator_type allocator);
    enum class kind_type : std::uint8_t { null,
        string,
        integer,
        array,
        error };

    redis_value(const redis_value&) = default;
    redis_value& operator=(const redis_value&) = default;
    redis_value(redis_value&&) noexcept = default;
    // A different PMR resource can require allocation during assignment.
    // NOLINTNEXTLINE(performance-noexcept-move-constructor)
    redis_value& operator=(redis_value&&) = default;

    [[nodiscard]] kind_type kind() const noexcept;
    [[nodiscard]] bool null() const noexcept;
    [[nodiscard]] std::string_view string() const&;
    [[nodiscard]] std::string_view string() const&& = delete;
    [[nodiscard]] std::string_view error() const&;
    [[nodiscard]] std::string_view error() const&& = delete;
    [[nodiscard]] std::int64_t integer() const;
    [[nodiscard]] std::span<const redis_value> array() const&;
    [[nodiscard]] std::span<const redis_value> array() const&& = delete;

private:
    friend class detail::redis_pool;
    friend struct detail::redis_types_access;

    explicit redis_value(std::pmr::memory_resource* resource = nullptr);
    [[nodiscard]] static redis_value null_value(std::pmr::memory_resource* resource);
    [[nodiscard]] static redis_value string_value(
        std::string_view value, std::pmr::memory_resource* resource);
    [[nodiscard]] static redis_value error_value(
        std::string_view value, std::pmr::memory_resource* resource);
    [[nodiscard]] static redis_value integer_value(
        std::int64_t value, std::pmr::memory_resource* resource);
    [[nodiscard]] static redis_value array_value(
        std::pmr::vector<redis_value> values, std::pmr::memory_resource* resource);

    redis_value(detail::resolved_pmr_resource_tag, std::pmr::memory_resource* resource);

    kind_type kind_{kind_type::null};
    std::pmr::string string_;
    std::int64_t integer_{0};
    std::pmr::vector<redis_value> array_;
};

}  // namespace ruvia
