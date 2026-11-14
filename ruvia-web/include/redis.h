#pragma once

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "borrowed_text.h"
#include "event_loop.h"
#include "http_client_types.h"
#include "middleware.h"
#include "model.h"
#include "stop_token.h"
#include "task.h"
#include "task_scope.h"
#include "worker_ref.h"

namespace ruvia {

class redis_value;
class redis_key_value;
class redis_scored_value;
class redis_scan_result;
class redis_hash_scan_result;
class redis_z_scan_result;
class redis_x_read_group_result;

class redis_search_mapping;

// Owns every command argument, including binary text. Copies inherit the source
// resource; assignment retains the destination resource.
class redis_compiled_command final {
public:
    redis_compiled_command(const redis_compiled_command&) = default;
    redis_compiled_command(redis_compiled_command&&) noexcept = default;
    redis_compiled_command& operator=(const redis_compiled_command&) = default;
    redis_compiled_command& operator=(redis_compiled_command&& other) noexcept {
        if (this != &other) {
            arguments_ = std::move(other.arguments_);
            other.arguments_.clear();
        }
        return *this;
    }

    [[nodiscard]] std::span<const std::string> arguments() const& noexcept RUVIA_LIFETIMEBOUND {
        return arguments_;
    }
    std::span<const std::string> arguments() const&& = delete;
    [[nodiscard]] std::vector<std::string> take_arguments() && noexcept {
        return std::move(arguments_);
    }

private:
    friend class redis_search_mapping;
    explicit redis_compiled_command(std::vector<std::string>&& arguments) noexcept :
        arguments_(std::move(arguments)) {}

    std::vector<std::string> arguments_;
};

struct redis_config final {
    // Validated unbracketed transport host; DNS names may retain one trailing dot.
    std::string host_{"127.0.0.1"};
    // Checked before narrowing; must be non-zero.
    port_number port_{6379};
    std::string username_;
    std::string password_;
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
    // waits bound the reply by their Redis wait plus a one-second grace, armed
    // when the command is written, so slot acquisition (acquire_timeout) and a
    // lazy connect (connect_timeout) do not consume the server-side window.
    // Infinite waits require an explicit stop_token or operation timeout.
    std::optional<timeout_duration> connect_timeout_{std::chrono::seconds(5)};
    std::optional<timeout_duration> command_timeout_{std::chrono::seconds(30)};
    std::optional<timeout_duration> acquire_timeout_{std::chrono::seconds(5)};
    // Absence disables the reply byte limit.
    std::optional<std::size_t> max_reply_bytes_{64 * 1024 * 1024};
    // Must be greater than zero.
    std::size_t max_array_depth_{64};
    client_tcp_config tcp_{.keep_alive_ = tcp_keep_alive_policy::system_default};
    // Liveness of a redis_subscription's dedicated connection while a read()
    // waits: after one interval without input it sends PING (unless a command
    // written since the last input is still awaiting its reply), and after one
    // more interval without input it closes the subscription, failing the
    // read with redis_error(timeout). Any received byte restarts the interval;
    // PING replies are consumed and never surface as events. Absence disables
    // the probe; a configured interval must be greater than zero.
    std::optional<timeout_duration> subscription_ping_interval_{std::chrono::seconds(30)};
};

class redis_block_wait final {
public:
    // Positive fractional milliseconds round up; values beyond the millisecond
    // range saturate. Non-positive waits must use an explicit policy instead.
    [[nodiscard]] static redis_block_wait for_duration(timeout_duration duration) {
        if (duration.value().count() <= 0) {
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

    // Canonical, safely converted Redis wire duration; absence means infinite.
    [[nodiscard]] std::optional<std::chrono::milliseconds> duration() const noexcept {
        return duration_.has_value() ? std::optional(duration_->value()) : std::nullopt;
    }

private:
    explicit redis_block_wait(std::optional<timeout_duration> duration) noexcept :
        duration_(duration) {}

    std::optional<timeout_duration> duration_;
};

struct redis_stream_read_view final {
    borrowed_text stream_{};
    borrowed_text id_{};
};

enum class redis_x_read_group_acknowledgement_policy : std::uint8_t {
    track_pending,
    no_ack,
};
// Strictly positive Redis batch count in signed 64-bit wire range. Checked
// before narrowing so negative or zero inputs cannot wrap into huge counts.
class redis_positive_count final {
public:
    constexpr redis_positive_count() noexcept = default;

    template <std::integral integer_type>
    constexpr redis_positive_count(integer_type value) :
        value_(is_valid_value(value) ? static_cast<std::uint64_t>(value) : throw std::invalid_argument("Redis count must be in 1..9223372036854775807")) {}

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        return std::cmp_greater(+value, 0) && std::in_range<std::int64_t>(+value);
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<redis_positive_count> try_from_value(integer_type value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return redis_positive_count(static_cast<std::uint64_t>(value), validated_tag_type{});
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }

    friend constexpr auto operator<=>(redis_positive_count, redis_positive_count) = default;

private:
    struct validated_tag_type {};

    constexpr redis_positive_count(std::uint64_t value, validated_tag_type) noexcept :
        value_(value) {}

    std::uint64_t value_{1};
};

static_assert(std::is_trivially_copyable_v<redis_positive_count>);
static_assert(sizeof(redis_positive_count) == sizeof(std::uint64_t));

struct redis_x_read_group_options final {
    std::optional<redis_positive_count> count_{};
    std::optional<redis_block_wait> block_{};
    redis_x_read_group_acknowledgement_policy acknowledgement_{redis_x_read_group_acknowledgement_policy::track_pending};
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

    [[nodiscard]] const std::chrono::milliseconds* duration() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<std::chrono::milliseconds>(&value_);
    }
    [[nodiscard]] const std::chrono::milliseconds* duration() const&& = delete;

    [[nodiscard]] bool keeps_existing() const noexcept {
        return std::get_if<keep_existing_type>(&value_) != nullptr;
    }

private:
    struct keep_existing_type final {};
    using value_type = std::variant<std::chrono::milliseconds, keep_existing_type>;

    explicit redis_set_expiration(std::chrono::milliseconds duration) noexcept :
        value_(duration) {}

    explicit redis_set_expiration(keep_existing_type keep) noexcept :
        value_(keep) {}

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
    redis_set_result(const redis_set_result&) = default;
    redis_set_result(redis_set_result&&) noexcept = default;
    redis_set_result& operator=(const redis_set_result&) = default;
    redis_set_result& operator=(redis_set_result&&) noexcept = default;

    [[nodiscard]] constexpr bool applied() const noexcept {
        return applied_;
    }

    [[nodiscard]] const std::optional<std::string>& previous() const& noexcept RUVIA_LIFETIMEBOUND {
        return previous_;
    }
    const std::optional<std::string>& previous() const&& = delete;

private:
    friend struct detail::redis_types_access;

    redis_set_result(bool applied, std::optional<std::string> previous) noexcept :
        applied_(applied),
        previous_(std::move(previous)) {}
    bool applied_{false};
    std::optional<std::string> previous_;
};

class redis_scan_cursor final {
public:
    friend constexpr bool operator==(redis_scan_cursor, redis_scan_cursor) noexcept = default;

private:
    friend struct detail::redis_types_access;

    explicit constexpr redis_scan_cursor(std::uint64_t value) noexcept :
        value_(value) {}

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

    constexpr redis_ttl(redis_ttl_state state_value, std::optional<std::chrono::milliseconds> remaining) noexcept :
        state_(state_value),
        remaining_(remaining) {}

    redis_ttl_state state_;
    std::optional<std::chrono::milliseconds> remaining_;
};

struct redis_scan_options final {
    // A scan options value may be retained before the command copies its
    // arguments. Keep MATCH zero-copy while rejecting owning-string rvalues
    // that would leave a saved options value with an already-dangling view.
    std::optional<redis_scan_cursor> cursor_{};
    // nullopt omits MATCH; an engaged empty pattern matches only empty names.
    // Assign nullopt (not "") to disable filtering.
    std::optional<::ruvia::borrowed_text> match_{};
    std::optional<redis_positive_count> count_{};
};

class redis_key_value final {
public:
    redis_key_value(const redis_key_value&) = default;
    redis_key_value& operator=(const redis_key_value&) = default;
    redis_key_value(redis_key_value&&) noexcept = default;
    redis_key_value& operator=(redis_key_value&&) noexcept = default;

    [[nodiscard]] std::string_view key() const& noexcept RUVIA_LIFETIMEBOUND {
        return key_;
    }
    [[nodiscard]] std::string_view key() const&& = delete;

    [[nodiscard]] std::string_view value() const& noexcept RUVIA_LIFETIMEBOUND {
        return value_;
    }
    [[nodiscard]] std::string_view value() const&& = delete;

private:
    friend struct detail::redis_types_access;

    redis_key_value(std::string&& key, std::string&& value) noexcept :
        key_(std::move(key)),
        value_(std::move(value)) {}

    std::string key_;
    std::string value_;
};

class redis_scored_value final {
public:
    redis_scored_value(const redis_scored_value&) = default;
    redis_scored_value& operator=(const redis_scored_value&) = default;
    redis_scored_value(redis_scored_value&&) noexcept = default;
    redis_scored_value& operator=(redis_scored_value&&) noexcept = default;

    [[nodiscard]] std::string_view value() const& noexcept RUVIA_LIFETIMEBOUND {
        return value_;
    }
    [[nodiscard]] std::string_view value() const&& = delete;

    [[nodiscard]] double score() const noexcept {
        return score_;
    }

private:
    friend struct detail::redis_types_access;

    redis_scored_value(std::string&& value, double score) noexcept :
        value_(std::move(value)),
        score_(score) {}

    std::string value_;
    double score_{0};
};

// Composite results retain their destination resource during move assignment.
// Allocator-extended moves and move assignment own every nested value in the
// target resource, even when a different source resource compares equal. Such
// rebinding may allocate; failure leaves both values unchanged. Moves within
// the same resource transfer storage without allocation.
class redis_scan_result final {
public:
    redis_scan_result(const redis_scan_result&) = delete;
    redis_scan_result& operator=(const redis_scan_result&) = delete;
    redis_scan_result(redis_scan_result&&) noexcept = default;
    redis_scan_result& operator=(redis_scan_result&& other) noexcept = default;

    [[nodiscard]] bool done() const noexcept {
        return !next_cursor_.has_value();
    }

    [[nodiscard]] std::optional<redis_scan_cursor> next_cursor() const noexcept {
        return next_cursor_;
    }

    [[nodiscard]] std::span<const std::string> values() const& noexcept RUVIA_LIFETIMEBOUND {
        return values_;
    }
    [[nodiscard]] std::span<const std::string> values() const&& = delete;

private:
    friend struct detail::redis_types_access;

    redis_scan_result() noexcept = default;

    std::optional<redis_scan_cursor> next_cursor_;
    std::vector<std::string> values_;
};

class redis_hash_scan_result final {
public:
    redis_hash_scan_result(const redis_hash_scan_result&) = delete;
    redis_hash_scan_result& operator=(const redis_hash_scan_result&) = delete;
    redis_hash_scan_result(redis_hash_scan_result&&) noexcept = default;
    redis_hash_scan_result& operator=(redis_hash_scan_result&& other) noexcept = default;

    [[nodiscard]] bool done() const noexcept {
        return !next_cursor_.has_value();
    }

    [[nodiscard]] std::optional<redis_scan_cursor> next_cursor() const noexcept {
        return next_cursor_;
    }

    [[nodiscard]] std::span<const redis_key_value> entries() const& noexcept RUVIA_LIFETIMEBOUND {
        return entries_;
    }
    [[nodiscard]] std::span<const redis_key_value> entries() const&& = delete;

private:
    friend struct detail::redis_types_access;

    redis_hash_scan_result() noexcept = default;

    std::optional<redis_scan_cursor> next_cursor_;
    std::vector<redis_key_value> entries_;
};

class redis_z_scan_result final {
public:
    redis_z_scan_result(const redis_z_scan_result&) = delete;
    redis_z_scan_result& operator=(const redis_z_scan_result&) = delete;
    redis_z_scan_result(redis_z_scan_result&&) noexcept = default;
    redis_z_scan_result& operator=(redis_z_scan_result&& other) noexcept = default;

    [[nodiscard]] bool done() const noexcept {
        return !next_cursor_.has_value();
    }

    [[nodiscard]] std::optional<redis_scan_cursor> next_cursor() const noexcept {
        return next_cursor_;
    }

    [[nodiscard]] std::span<const redis_scored_value> entries() const& noexcept RUVIA_LIFETIMEBOUND {
        return entries_;
    }
    [[nodiscard]] std::span<const redis_scored_value> entries() const&& = delete;

private:
    friend struct detail::redis_types_access;

    redis_z_scan_result() noexcept = default;

    std::optional<redis_scan_cursor> next_cursor_;
    std::vector<redis_scored_value> entries_;
};

class redis_stream_entry final {
public:
    redis_stream_entry(const redis_stream_entry&) = default;
    redis_stream_entry& operator=(const redis_stream_entry&) = default;
    redis_stream_entry(redis_stream_entry&&) noexcept = default;
    redis_stream_entry& operator=(redis_stream_entry&&) noexcept = default;

    [[nodiscard]] std::string_view id() const& noexcept RUVIA_LIFETIMEBOUND {
        return id_;
    }
    [[nodiscard]] std::string_view id() const&& = delete;

    // XREADGROUP retains the ID of a deleted pending message with no fields.
    [[nodiscard]] std::span<const redis_key_value> fields() const& noexcept RUVIA_LIFETIMEBOUND {
        return fields_;
    }
    [[nodiscard]] std::span<const redis_key_value> fields() const&& = delete;

private:
    friend struct detail::redis_types_access;

    explicit redis_stream_entry(std::string&& id) noexcept :
        id_(std::move(id)) {}

    std::string id_;
    std::vector<redis_key_value> fields_;
};

class redis_stream_read_result final {
public:
    redis_stream_read_result(const redis_stream_read_result&) = default;
    redis_stream_read_result& operator=(const redis_stream_read_result&) = default;
    redis_stream_read_result(redis_stream_read_result&&) noexcept = default;
    redis_stream_read_result& operator=(redis_stream_read_result&&) noexcept = default;

    [[nodiscard]] std::string_view stream() const& noexcept RUVIA_LIFETIMEBOUND {
        return stream_;
    }
    [[nodiscard]] std::string_view stream() const&& = delete;

    [[nodiscard]] std::span<const redis_stream_entry> entries() const& noexcept RUVIA_LIFETIMEBOUND {
        return entries_;
    }
    [[nodiscard]] std::span<const redis_stream_entry> entries() const&& = delete;

private:
    friend struct detail::redis_types_access;

    explicit redis_stream_read_result(std::string&& stream) noexcept :
        stream_(std::move(stream)) {}

    std::string stream_;
    std::vector<redis_stream_entry> entries_;
};

class redis_x_read_group_result final {
public:
    redis_x_read_group_result(const redis_x_read_group_result&) = delete;
    redis_x_read_group_result& operator=(const redis_x_read_group_result&) = delete;
    redis_x_read_group_result(redis_x_read_group_result&&) noexcept = default;
    redis_x_read_group_result& operator=(redis_x_read_group_result&& other) noexcept = default;

    [[nodiscard]] std::span<const redis_stream_read_result> streams() const& noexcept RUVIA_LIFETIMEBOUND {
        return streams_;
    }
    [[nodiscard]] std::span<const redis_stream_read_result> streams() const&& = delete;

private:
    friend struct detail::redis_types_access;

    redis_x_read_group_result() noexcept = default;
    std::vector<redis_stream_read_result> streams_;
};

namespace detail {

class redis_pool;

}  // namespace detail

class redis_error final : public std::runtime_error {
public:
    enum class code_type : std::uint8_t { not_configured,
        connect_failed,
        auth_failed,
        protocol_error,
        command_error,
        io_error,
        timeout,
        cancelled,
        closing };

    redis_error(code_type code, std::string_view message);

    [[nodiscard]] code_type code() const noexcept;

private:
    code_type code_;
};

namespace detail {

template <text_scalar t_type>
[[nodiscard]] t_type convert_redis_text(std::string_view text) {
    auto parsed = parse_text_scalar<t_type>(text);
    if (std::holds_alternative<text_scalar_parse_error>(parsed)) {
        if constexpr (std::is_same_v<t_type, bool>) {
            throw redis_error(redis_error::code_type::protocol_error, "redis value is not a boolean");
        } else {
            throw redis_error(redis_error::code_type::protocol_error, "redis value is not a valid number");
        }
    }
    return std::get<t_type>(std::move(parsed));
}

}  // namespace detail
class redis_value final {
public:
    enum class kind_type : std::uint8_t { null,
        string,
        integer,
        array,
        error };

    redis_value(const redis_value& other) = default;
    redis_value& operator=(const redis_value& other);
    redis_value(redis_value&&) noexcept = default;
    redis_value& operator=(redis_value&& other) noexcept;

    [[nodiscard]] kind_type kind() const noexcept;
    [[nodiscard]] bool null() const noexcept;
    [[nodiscard]] bool is_null() const noexcept {
        return null();
    }
    [[nodiscard]] bool is_string() const noexcept {
        return kind() == kind_type::string;
    }
    [[nodiscard]] bool is_integer() const noexcept {
        return kind() == kind_type::integer;
    }
    [[nodiscard]] bool is_array() const noexcept {
        return kind() == kind_type::array;
    }
    [[nodiscard]] bool is_error() const noexcept {
        return kind() == kind_type::error;
    }
    [[nodiscard]] std::string_view string() const& RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view string() const&& = delete;
    [[nodiscard]] std::string_view error() const& RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view error() const&& = delete;
    [[nodiscard]] std::int64_t integer() const;
    [[nodiscard]] std::span<const redis_value> array() const& RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::span<const redis_value> array() const&& = delete;

    template <detail::text_scalar t_type>
    [[nodiscard]] std::optional<t_type> as() const& {
        if (is_null()) {
            return std::nullopt;
        }
        if (is_integer()) {
            if constexpr (std::is_same_v<t_type, bool>) {
                if (integer_ == 0) {
                    return false;
                }
                if (integer_ == 1) {
                    return true;
                }
                throw redis_error(redis_error::code_type::protocol_error, "redis value is not a boolean");
            } else if constexpr (detail::is_model_integer<t_type>) {
                if (!std::in_range<t_type>(integer_)) {
                    throw redis_error(redis_error::code_type::protocol_error, "redis value is out of range");
                }
                return static_cast<t_type>(integer_);
            } else if constexpr (std::is_floating_point_v<t_type>) {
                return static_cast<t_type>(integer_);
            }
        }
        return detail::convert_redis_text<t_type>(string());
    }
    template <detail::text_scalar t_type>
    [[nodiscard]] std::optional<t_type> as() const&& = delete;

private:
    friend class detail::redis_pool;
    friend struct detail::redis_types_access;

    redis_value() noexcept;
    [[nodiscard]] static redis_value null_value() noexcept;
    [[nodiscard]] static redis_value string_value(std::string_view value);
    [[nodiscard]] static redis_value error_value(std::string_view value);
    [[nodiscard]] static redis_value integer_value(std::int64_t value) noexcept;
    [[nodiscard]] static redis_value array_value(std::size_t capacity);

    kind_type kind_{kind_type::null};
    std::string string_;
    std::int64_t integer_{0};
    std::vector<redis_value> array_;
};

class redis_session;

namespace detail {
class redis_client_runtime;
class redis_client_state;
struct request_io_metrics;
struct redis_orm_script;
class redis_subscription_connection;
class redis_subscription_set;
class worker_client_registry_view;

// One textual argument the variadic Redis entry points accept. A span of
// string_view is not itself convertible to string_view, so a caller that already
// holds a contiguous sequence still selects the span overload.
//
// Variadic entry points synchronously clone every view into operation- or
// batch-owned storage, so owning-string temporaries remain alive long enough.
// Explicit span/view APIs keep their existing caller-owned lifetime contract.
template <typename arg_type>
concept redis_argument = std::convertible_to<arg_type&&, std::string_view>;

template <typename... args_type>
concept redis_argument_pack = sizeof...(args_type) > 0 && (redis_argument<args_type> && ...);

// Commands that take alternating name/value arguments (MSET, HSET). An odd
// count would leave a trailing name with no value, which Redis rejects at the
// wire level; requiring an even count turns that into a compile error.
template <typename... args_type>
concept redis_pair_argument_pack = redis_argument_pack<args_type...> && sizeof...(args_type) % 2 == 0;

// One scoped operation owns the command and its decoding. Mappers must return
// owning values in the handle's reclaimable worker memory domain.
template <typename result_type, typename mapper_type>
task<result_type> map_redis_command(task<redis_value> command, mapper_type mapper) {
    auto reply = co_await std::move(command);
    if constexpr (std::is_void_v<result_type>) {
        mapper(std::move(reply));
    } else {
        co_return mapper(std::move(reply));
    }
}

struct redis_owned_command final {
    std::string wire_;
};

[[nodiscard]] redis_owned_command make_owned_redis_command(std::span<const std::string_view> args);
[[nodiscard]] redis_owned_command make_owned_redis_command(std::string_view first, std::span<const std::string_view> rest = {});

// Shared typed command layouts for direct execution, pipelines and transactions.
[[nodiscard]] redis_owned_command make_redis_integer_command(std::string_view command, std::string_view key, std::int64_t value);
[[nodiscard]] redis_owned_command make_redis_range_command(std::string_view command, std::string_view key, std::int64_t start, std::int64_t stop, bool with_scores = false);
[[nodiscard]] redis_owned_command make_redis_zadd_command(std::string_view key, double score, std::string_view member);
struct redis_command_payload final {
    std::reference_wrapper<redis_pool> pool_;
    operation_options options_;
    std::vector<redis_owned_command> commands_;
};

enum class redis_batch_mode : std::uint8_t {
    pipeline,
    transaction,
};

template <redis_batch_mode mode>
class redis_batch final {
public:
    redis_batch(const redis_batch&) = delete;
    redis_batch& operator=(const redis_batch&) = delete;
    redis_batch(redis_batch&& other) noexcept;
    redis_batch& operator=(redis_batch&&) = delete;

    // 非事务 pipeline 接受 WAIT/WAITAOF；其他阻塞命令以及事务内屏障均拒绝。
    // 屏障要求继承的 operation_options 有 timeout_ 或可取消的 stop_token_。
    redis_batch& command(std::span<const std::string_view> args);
    redis_batch& command(std::initializer_list<std::string_view> args) = delete;

    // Every command owns its arguments before this call returns.
    template <typename... args_type>
        requires redis_argument_pack<args_type...>
    redis_batch& command(args_type&&... args) {
        const std::string_view views[]{std::string_view(args)...};
        return command(std::span<const std::string_view>(views));
    }

    redis_batch& get(std::string_view key) {
        return trusted("GET", key);
    }

    redis_batch& set(std::string_view key, std::string_view value) {
        return trusted("SET", key, value);
    }

    redis_batch& get_del(std::string_view key) {
        return trusted("GETDEL", key);
    }

    redis_batch& append(std::string_view key, std::string_view value) {
        return trusted("APPEND", key, value);
    }

    redis_batch& strlen(std::string_view key) {
        return trusted("STRLEN", key);
    }

    redis_batch& del(std::string_view key) {
        return trusted("DEL", key);
    }

    redis_batch& unlink(std::string_view key) {
        return trusted("UNLINK", key);
    }

    redis_batch& exists(std::string_view key) {
        return trusted("EXISTS", key);
    }

    redis_batch& touch(std::string_view key) {
        return trusted("TOUCH", key);
    }

    redis_batch& type(std::string_view key) {
        return trusted("TYPE", key);
    }

    redis_batch& rename(std::string_view key, std::string_view new_key) {
        return trusted("RENAME", key, new_key);
    }

    redis_batch& rename_nx(std::string_view key, std::string_view new_key) {
        return trusted("RENAMENX", key, new_key);
    }

    redis_batch& incr(std::string_view key) {
        return trusted("INCR", key);
    }

    redis_batch& incr_by(std::string_view key, std::int64_t value) {
        require_ready();
        return append_owned(make_redis_integer_command("INCRBY", key, value));
    }

    redis_batch& decr(std::string_view key) {
        return trusted("DECR", key);
    }

    redis_batch& decr_by(std::string_view key, std::int64_t value) {
        require_ready();
        return append_owned(make_redis_integer_command("DECRBY", key, value));
    }

    redis_batch& hget(std::string_view key, std::string_view field) {
        return trusted("HGET", key, field);
    }

    redis_batch& hset(std::string_view key, std::string_view field, std::string_view value) {
        return trusted("HSET", key, field, value);
    }

    redis_batch& hdel(std::string_view key, std::string_view field) {
        return trusted("HDEL", key, field);
    }

    redis_batch& hexists(std::string_view key, std::string_view field) {
        return trusted("HEXISTS", key, field);
    }

    redis_batch& hlen(std::string_view key) {
        return trusted("HLEN", key);
    }

    redis_batch& hget_all(std::string_view key) {
        return trusted("HGETALL", key);
    }

    redis_batch& lpush(std::string_view key, std::string_view value) {
        return trusted("LPUSH", key, value);
    }

    redis_batch& rpush(std::string_view key, std::string_view value) {
        return trusted("RPUSH", key, value);
    }

    redis_batch& lpop(std::string_view key) {
        return trusted("LPOP", key);
    }

    redis_batch& rpop(std::string_view key) {
        return trusted("RPOP", key);
    }

    redis_batch& llen(std::string_view key) {
        return trusted("LLEN", key);
    }

    redis_batch& lrange(std::string_view key, std::int64_t start, std::int64_t stop) {
        require_ready();
        return append_owned(make_redis_range_command("LRANGE", key, start, stop));
    }

    redis_batch& sadd(std::string_view key, std::string_view member) {
        return trusted("SADD", key, member);
    }

    redis_batch& srem(std::string_view key, std::string_view member) {
        return trusted("SREM", key, member);
    }

    redis_batch& smembers(std::string_view key) {
        return trusted("SMEMBERS", key);
    }

    redis_batch& scard(std::string_view key) {
        return trusted("SCARD", key);
    }

    redis_batch& zadd(std::string_view key, double score, std::string_view member) {
        require_ready();
        return append_owned(make_redis_zadd_command(key, score, member));
    }

    redis_batch& zrem(std::string_view key, std::string_view member) {
        return trusted("ZREM", key, member);
    }

    redis_batch& zrange(std::string_view key, std::int64_t start, std::int64_t stop) {
        require_ready();
        return append_owned(make_redis_range_command("ZRANGE", key, start, stop));
    }

    redis_batch& zscore(std::string_view key, std::string_view member) {
        return trusted("ZSCORE", key, member);
    }

    redis_batch& zcard(std::string_view key) {
        return trusted("ZCARD", key);
    }

    redis_batch& publish(std::string_view channel, std::string_view payload) {
        return trusted("PUBLISH", channel, payload);
    }

    // Consumes the batch before returning the lazy task, so the coroutine frame
    // owns every command and never borrows this builder through `this`.
    [[nodiscard]] task<std::vector<redis_value>> exec() &&;

private:
    friend class ::ruvia::redis_session;

    // `io_metrics` is the request's counter block, or null outside a request.
    redis_batch(redis_pool& pool, operation_options options, operation_scope& scope, request_io_metrics* io_metrics) noexcept;
    void require_ready() const;
    [[nodiscard]] redis_command_payload consume();
    void expire() noexcept;
    redis_batch& append_owned(redis_owned_command command);

    template <typename... args_type>
    redis_batch& trusted(std::string_view command_name, args_type... args) {
        require_ready();
        const std::string_view rest[]{args...};
        return append_owned(make_owned_redis_command(command_name, std::span<const std::string_view>(rest)));
    }

    [[nodiscard]] static task<std::vector<redis_value>> execute_owned(request_io_metrics* io_metrics, redis_command_payload payload);
    static void expire_capability(void* target) noexcept;

    redis_pool* pool_;
    operation_options options_;
    std::vector<redis_owned_command> commands_;
    request_io_metrics* io_metrics_;
    scoped_capability_registration registration_;
};

}  // namespace detail

using redis_pipeline = detail::redis_batch<detail::redis_batch_mode::pipeline>;
using redis_transaction = detail::redis_batch<detail::redis_batch_mode::transaction>;

enum class redis_pubsub_confirmation_kind : std::uint8_t {
    subscribed,
    pattern_subscribed,
    unsubscribed,
    pattern_unsubscribed,
};

class redis_pubsub_confirmation final {
public:
    [[nodiscard]] redis_pubsub_confirmation_kind kind() const noexcept {
        return kind_;
    }
    // Present empty names are legal. Absence is Redis's nil confirmation when
    // an unsubscribe-all command has no matching subscriptions to remove.
    [[nodiscard]] std::optional<std::string_view> name() const noexcept {
        return name_;
    }
    [[nodiscard]] std::int64_t subscription_count() const noexcept {
        return subscription_count_;
    }

private:
    friend struct detail::redis_types_access;
    redis_pubsub_confirmation(redis_pubsub_confirmation_kind kind, std::optional<std::string_view> name, std::int64_t subscription_count) noexcept :
        kind_(kind),
        name_(name),
        subscription_count_(subscription_count) {}

    redis_pubsub_confirmation_kind kind_;
    std::optional<std::string_view> name_;
    std::int64_t subscription_count_;
};

class redis_pubsub_message final {
public:
    [[nodiscard]] std::string_view channel() const noexcept {
        return channel_;
    }
    // Absence means a direct message; a present empty pattern is still a
    // pattern delivery. Neither channel nor payload has an absence sentinel.
    [[nodiscard]] std::optional<std::string_view> pattern() const noexcept {
        return pattern_;
    }
    [[nodiscard]] std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend struct detail::redis_types_access;
    redis_pubsub_message(std::string_view channel, std::optional<std::string_view> pattern, std::string_view payload) noexcept :
        channel_(channel),
        pattern_(pattern),
        payload_(payload) {}

    std::string_view channel_;
    std::optional<std::string_view> pattern_;
    std::string_view payload_;
};

// Exactly one confirmation or message, in wire order. Every text view borrows
// the subscription's current reply until its next read() starts or destruction.
// Copy text that must outlive that boundary.
class redis_pubsub_event final {
public:
    [[nodiscard]] const redis_pubsub_confirmation* confirmation() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<redis_pubsub_confirmation>(&value_);
    }
    const redis_pubsub_confirmation* confirmation() const&& = delete;
    [[nodiscard]] const redis_pubsub_message* message() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<redis_pubsub_message>(&value_);
    }
    const redis_pubsub_message* message() const&& = delete;

private:
    friend struct detail::redis_types_access;
    explicit redis_pubsub_event(redis_pubsub_confirmation value) noexcept :
        value_(value) {}
    explicit redis_pubsub_event(redis_pubsub_message value) noexcept :
        value_(value) {}

    std::variant<redis_pubsub_confirmation, redis_pubsub_message> value_;
};

// One dedicated Pub/Sub connection, created by redis_session::subscription().
// Channels and patterns are multiplexed on it; ordinary commands stay on the
// handle's pools. The connection is opened by the first subscribe() and lives
// until abort(), the handle's stop token, worker shutdown, a failed operation
// or destruction. There is no reconnect: Pub/Sub delivers at most once, and a
// closed subscription has lost whatever was published meanwhile, so the
// application decides how to recover.
//
// Commands only write; their confirmations arrive through read() in wire order,
// interleaved with messages. One command and one read may be in flight at a
// time (a second throws std::logic_error); unstarted operations may coexist.
// Reads observe the subscription's stop token and operation timeout; a timeout
// closes the subscription and throws redis_error(timeout). While a read waits,
// redis_config::subscription_ping_interval_ probes an idle connection with
// PING; a peer that stays silent for one more interval closes the
// subscription and the read throws redis_error(timeout). PING replies are
// consumed internally and never returned by read().
//
// Worker-affine: use and destroy it on the owning worker before the handle's
// scope ends. Destroying it with an operation in flight terminates.
class redis_subscription final {
public:
    redis_subscription(const redis_subscription&) = delete;
    redis_subscription& operator=(const redis_subscription&) = delete;
    redis_subscription(redis_subscription&&) = delete;
    redis_subscription& operator=(redis_subscription&&) = delete;
    ~redis_subscription();

    // Non-empty name lists; names are binary-safe.
    [[nodiscard]] task<void> subscribe(std::span<const std::string_view> channels) &;
    task<void> subscribe(std::span<const std::string_view>) && = delete;
    [[nodiscard]] task<void> psubscribe(std::span<const std::string_view> patterns) &;
    task<void> psubscribe(std::span<const std::string_view>) && = delete;
    // An empty list unsubscribes from every channel (pattern) of this connection.
    [[nodiscard]] task<void> unsubscribe(std::span<const std::string_view> channels = {}) &;
    task<void> unsubscribe(std::span<const std::string_view> = {}) && = delete;
    [[nodiscard]] task<void> punsubscribe(std::span<const std::string_view> patterns = {}) &;
    task<void> punsubscribe(std::span<const std::string_view> = {}) && = delete;

    template <typename... channels_type>
        requires detail::redis_argument_pack<channels_type...>
    [[nodiscard]] task<void> subscribe(channels_type&&... channels) & {
        const std::string_view views[]{std::string_view(channels)...};
        return subscribe(std::span<const std::string_view>(views));
    }
    template <typename... channels_type>
        requires detail::redis_argument_pack<channels_type...>
    task<void> subscribe(channels_type&&...) && = delete;

    template <typename... patterns_type>
        requires detail::redis_argument_pack<patterns_type...>
    [[nodiscard]] task<void> psubscribe(patterns_type&&... patterns) & {
        const std::string_view views[]{std::string_view(patterns)...};
        return psubscribe(std::span<const std::string_view>(views));
    }
    template <typename... patterns_type>
        requires detail::redis_argument_pack<patterns_type...>
    task<void> psubscribe(patterns_type&&...) && = delete;

    template <typename... channels_type>
        requires detail::redis_argument_pack<channels_type...>
    [[nodiscard]] task<void> unsubscribe(channels_type&&... channels) & {
        const std::string_view views[]{std::string_view(channels)...};
        return unsubscribe(std::span<const std::string_view>(views));
    }
    template <typename... channels_type>
        requires detail::redis_argument_pack<channels_type...>
    task<void> unsubscribe(channels_type&&...) && = delete;

    template <typename... patterns_type>
        requires detail::redis_argument_pack<patterns_type...>
    [[nodiscard]] task<void> punsubscribe(patterns_type&&... patterns) & {
        const std::string_view views[]{std::string_view(patterns)...};
        return punsubscribe(std::span<const std::string_view>(views));
    }
    template <typename... patterns_type>
        requires detail::redis_argument_pack<patterns_type...>
    task<void> punsubscribe(patterns_type&&...) && = delete;
    // The next confirmation or message. std::nullopt once the subscription is
    // closed by abort(), its stop token or worker shutdown. The server ending
    // the connection throws redis_error(io_error); an error reply (for example
    // a denied channel) throws redis_error(command_error) and leaves the
    // connection open; a malformed push closes it with protocol_error; an
    // unanswered idle probe closes it with timeout.
    // Requires a completed subscribe() first: std::logic_error otherwise.
    // `operation` bounds this read only: its timeout may only tighten the
    // subscription's, and its token cancels the read while the subscription's
    // own token still ends the stream with std::nullopt. A non-positive
    // timeout throws std::invalid_argument at the call; an already-stopped
    // token throws redis_error(cancelled) and leaves the subscription open.
    // Once started, its timeout or stop request closes the connection -- a
    // Redis connection interrupted mid-reply is never reused -- and throws
    // redis_error(timeout / cancelled).
    [[nodiscard]] task<std::optional<redis_pubsub_event>> read(operation_options operation = {}) &;
    task<std::optional<redis_pubsub_event>> read(operation_options = {}) && = delete;

    // Closes the connection now; a pending read() completes with std::nullopt.
    void abort() noexcept;

private:
    friend class redis_session;

    redis_subscription(detail::redis_subscription_set& subscriptions, operation_scope& scope, operation_options options);

    static void expire_capability(void* target) noexcept;
    task<void> send(std::string_view command, std::span<const std::string_view> names, bool require_names);

    std::unique_ptr<detail::redis_subscription_connection> connection_;
    // Declared after the connection so it unlinks before the connection closes.
    scoped_capability_registration registration_;
};

struct redis_repository_config;
template <typename entity_type>
class redis_repository;

class redis_session final {
public:
    redis_session(const redis_session& other) noexcept;
    redis_session& operator=(const redis_session&) = delete;

    // Returns a request-scoped view whose policy is applied to every typed
    // command and inherited by pipelines and transactions created from it.
    // Empty tokens inherit; supplied states replace user cancellation. Timeout
    // can only tighten. Accumulation requires a caller-owned stop_group.
    [[nodiscard]] redis_session with_options(operation_options options) const;

    // Borrows the owning worker; an expired capability cannot expose it.
    [[nodiscard]] const worker_ref& worker() const& RUVIA_LIFETIMEBOUND;
    const worker_ref& worker() const&& = delete;

    // 单次租约没有可依赖的前序写入历史；WAIT/WAITAOF 在此同步拒绝。
    // 将写入与屏障合并到同一非事务 pipeline，参见 pipeline()。
    // XREAD/XREADGROUP 在 STREAMS 前只接受 COUNT、BLOCK 及 XREADGROUP 的 CLAIM、NOACK；
    // 含 BLOCK 时走阻塞池，其他选项同步抛出 std::invalid_argument。
    [[nodiscard]] task<redis_value> command(std::span<const std::string_view> args) const;
    task<redis_value> command(std::initializer_list<std::string_view> args) const = delete;
    // Takes ownership synchronously, before the returned cold task can suspend.
    [[nodiscard]] task<redis_value> command(redis_compiled_command command) const {
        if (command.arguments().empty()) {
            throw std::invalid_argument("Redis compiled command has been consumed");
        }
        return scoped(command_owned(std::move(command).take_arguments()));
    }

    [[nodiscard]] task<void> ping() const;
    [[nodiscard]] task<std::string> ping(std::string_view message) const;
    [[nodiscard]] task<std::optional<std::string>> get(std::string_view key) const;
    [[nodiscard]] task<std::vector<std::optional<std::string>>> mget(std::span<const std::string_view> keys) const;
    task<std::vector<std::optional<std::string>>> mget(std::initializer_list<std::string_view> keys) const = delete;
    [[nodiscard]] task<redis_set_result> set(std::string_view key, std::string_view value, redis_set_options options = {}) const;
    [[nodiscard]] task<void> mset(std::span<const std::pair<std::string_view, std::string_view>> items) const;
    task<void> mset(std::initializer_list<std::pair<std::string_view, std::string_view>> items) const = delete;
    [[nodiscard]] task<std::optional<std::string>> get_del(std::string_view key) const;
    [[nodiscard]] task<std::int64_t> append(std::string_view key, std::string_view value) const;
    [[nodiscard]] task<std::int64_t> strlen(std::string_view key) const;
    [[nodiscard]] task<std::int64_t> incr_by(std::string_view key, std::int64_t value) const;
    [[nodiscard]] task<std::int64_t> decr(std::string_view key) const;
    [[nodiscard]] task<std::int64_t> decr_by(std::string_view key, std::int64_t value) const;
    [[nodiscard]] task<std::int64_t> del(std::string_view key) const;
    [[nodiscard]] task<std::int64_t> del(std::span<const std::string_view> keys) const;
    task<std::int64_t> del(std::initializer_list<std::string_view> keys) const = delete;
    [[nodiscard]] task<std::int64_t> unlink(std::string_view key) const;
    [[nodiscard]] task<std::int64_t> unlink(std::span<const std::string_view> keys) const;
    task<std::int64_t> unlink(std::initializer_list<std::string_view> keys) const = delete;
    [[nodiscard]] task<bool> exists(std::string_view key) const;
    [[nodiscard]] task<bool> touch(std::string_view key) const;
    [[nodiscard]] task<std::string> type(std::string_view key) const;
    [[nodiscard]] task<void> rename(std::string_view key, std::string_view new_key) const;
    [[nodiscard]] task<bool> rename_nx(std::string_view key, std::string_view new_key) const;
    [[nodiscard]] task<bool> expire(std::string_view key, std::chrono::seconds ttl) const;
    [[nodiscard]] task<bool> pexpire(std::string_view key, std::chrono::milliseconds ttl) const;
    [[nodiscard]] task<bool> expire_at(std::string_view key, std::chrono::system_clock::time_point expires_at) const;
    [[nodiscard]] task<bool> persist(std::string_view key) const;
    [[nodiscard]] task<redis_ttl> ttl(std::string_view key) const;
    [[nodiscard]] task<redis_ttl> pttl(std::string_view key) const;
    [[nodiscard]] task<std::int64_t> incr(std::string_view key) const;
    [[nodiscard]] task<std::optional<std::string>> hget(std::string_view key, std::string_view field) const;
    [[nodiscard]] task<std::int64_t> hset(std::string_view key, std::string_view field, std::string_view value) const;
    [[nodiscard]] task<std::int64_t> hset(std::string_view key, std::span<const std::pair<std::string_view, std::string_view>> fields) const;
    task<std::int64_t> hset(std::string_view key, std::initializer_list<std::pair<std::string_view, std::string_view>> fields_value) const = delete;
    [[nodiscard]] task<std::vector<std::optional<std::string>>> hmget(std::string_view key, std::span<const std::string_view> fields) const;
    task<std::vector<std::optional<std::string>>> hmget(std::string_view key, std::initializer_list<std::string_view> fields_value) const = delete;
    [[nodiscard]] task<std::vector<redis_key_value>> hget_all(std::string_view key) const;
    [[nodiscard]] task<std::int64_t> hdel(std::string_view key, std::string_view field) const;
    [[nodiscard]] task<bool> hexists(std::string_view key, std::string_view field) const;
    [[nodiscard]] task<std::int64_t> hlen(std::string_view key) const;
    [[nodiscard]] task<std::vector<std::string>> hkeys(std::string_view key) const;
    [[nodiscard]] task<std::vector<std::string>> hvals(std::string_view key) const;
    [[nodiscard]] task<std::int64_t> hincr_by(std::string_view key, std::string_view field, std::int64_t value) const;
    [[nodiscard]] task<std::int64_t> lpush(std::string_view key, std::string_view value) const;
    [[nodiscard]] task<std::int64_t> rpush(std::string_view key, std::string_view value) const;
    [[nodiscard]] task<std::optional<std::string>> lpop(std::string_view key) const;
    [[nodiscard]] task<std::optional<std::string>> rpop(std::string_view key) const;
    [[nodiscard]] task<std::int64_t> llen(std::string_view key) const;
    [[nodiscard]] task<std::vector<std::string>> lrange(std::string_view key, std::int64_t start, std::int64_t stop) const;
    [[nodiscard]] task<std::optional<std::string>> lindex(std::string_view key, std::int64_t index) const;
    [[nodiscard]] task<void> lset(std::string_view key, std::int64_t index, std::string_view value) const;
    [[nodiscard]] task<void> ltrim(std::string_view key, std::int64_t start, std::int64_t stop) const;
    [[nodiscard]] task<std::int64_t> lrem(std::string_view key, std::int64_t count, std::string_view value) const;
    [[nodiscard]] task<std::int64_t> sadd(std::string_view key, std::string_view member) const;
    [[nodiscard]] task<std::int64_t> srem(std::string_view key, std::string_view member) const;
    [[nodiscard]] task<std::vector<std::string>> smembers(std::string_view key) const;
    [[nodiscard]] task<std::int64_t> scard(std::string_view key) const;
    [[nodiscard]] task<bool> sismember(std::string_view key, std::string_view member) const;
    [[nodiscard]] task<std::optional<std::string>> spop(std::string_view key) const;
    [[nodiscard]] task<std::optional<std::string>> srand_member(std::string_view key) const;
    [[nodiscard]] task<std::vector<std::string>> sinter(std::span<const std::string_view> keys) const;
    task<std::vector<std::string>> sinter(std::initializer_list<std::string_view> keys) const = delete;
    [[nodiscard]] task<std::vector<std::string>> sunion(std::span<const std::string_view> keys) const;
    task<std::vector<std::string>> sunion(std::initializer_list<std::string_view> keys) const = delete;
    [[nodiscard]] task<std::vector<std::string>> sdiff(std::span<const std::string_view> keys) const;
    task<std::vector<std::string>> sdiff(std::initializer_list<std::string_view> keys) const = delete;
    [[nodiscard]] task<std::int64_t> zadd(std::string_view key, double score, std::string_view member) const;
    [[nodiscard]] task<std::int64_t> zrem(std::string_view key, std::string_view member) const;
    [[nodiscard]] task<std::vector<std::string>> zrange(std::string_view key, std::int64_t start, std::int64_t stop) const;
    [[nodiscard]] task<std::vector<redis_scored_value>> zrange_with_scores(std::string_view key, std::int64_t start, std::int64_t stop) const;
    [[nodiscard]] task<std::optional<double>> zscore(std::string_view key, std::string_view member) const;
    [[nodiscard]] task<std::int64_t> zcard(std::string_view key) const;
    [[nodiscard]] task<std::int64_t> zcount(std::string_view key, double min, double max) const;
    [[nodiscard]] task<redis_scan_result> scan(redis_scan_options options = {}) const;
    [[nodiscard]] task<redis_hash_scan_result> hscan(std::string_view key, redis_scan_options options = {}) const;
    [[nodiscard]] task<redis_scan_result> sscan(std::string_view key, redis_scan_options options = {}) const;
    [[nodiscard]] task<redis_z_scan_result> zscan(std::string_view key, redis_scan_options options = {}) const;
    [[nodiscard]] task<redis_value> eval(std::string_view script, std::span<const std::string_view> keys = {}, std::span<const std::string_view> args = {}) const;
    [[nodiscard]] task<redis_value> eval_sha(std::string_view sha1, std::span<const std::string_view> keys = {}, std::span<const std::string_view> args = {}) const;
    [[nodiscard]] task<std::string> script_load(std::string_view script) const;
    [[nodiscard]] task<std::vector<bool>> script_exists(std::span<const std::string_view> sha1s) const;
    [[nodiscard]] task<std::optional<redis_key_value>> blpop(std::string_view key, redis_block_wait wait) const {
        const std::string_view keys[]{key};
        return blpop(std::span<const std::string_view>(keys), wait);
    }
    [[nodiscard]] task<std::optional<redis_key_value>> blpop(std::span<const std::string_view> keys, redis_block_wait wait) const;
    [[nodiscard]] task<std::optional<redis_key_value>> brpop(std::string_view key, redis_block_wait wait) const {
        const std::string_view keys[]{key};
        return brpop(std::span<const std::string_view>(keys), wait);
    }
    [[nodiscard]] task<std::optional<redis_key_value>> brpop(std::span<const std::string_view> keys, redis_block_wait wait) const;
    [[nodiscard]] task<std::optional<redis_x_read_group_result>> xread_group(std::string_view group, std::string_view consumer, std::span<const redis_stream_read_view> streams, redis_x_read_group_options options = {}) const;

    // Multi-argument commands as ordinary arguments: mget(a, b, c) instead of a
    // hand-built array plus a span. Every span overload above clones its
    // arguments into owned storage before returning -- the command is built
    // synchronously and only the owned copy is moved into the coroutine -- so the
    // temporary array each of these creates only has to outlive the call.
    //
    // A span argument is not convertible to string_view and therefore never
    // selects one of these; a caller that already holds a sequence keeps using
    // the span overload unchanged.
    template <typename... args_type>
        requires detail::redis_argument_pack<args_type...>
    [[nodiscard]] task<redis_value> command(args_type&&... args) const {
        const std::string_view views[]{std::string_view(args)...};
        return command(std::span<const std::string_view>(views));
    }

    template <typename... keys_type>
        requires detail::redis_argument_pack<keys_type...>
    [[nodiscard]] task<std::vector<std::optional<std::string>>> mget(keys_type&&... keys) const {
        const std::string_view views[]{std::string_view(keys)...};
        return mget(std::span<const std::string_view>(views));
    }
    template <typename... keys_type>
        requires(detail::redis_argument_pack<keys_type...> && sizeof...(keys_type) >= 2)
    [[nodiscard]] task<std::int64_t> del(keys_type&&... keys) const {
        const std::string_view views[]{std::string_view(keys)...};
        return del(std::span<const std::string_view>(views));
    }

    template <typename... keys_type>
        requires(detail::redis_argument_pack<keys_type...> && sizeof...(keys_type) >= 2)
    [[nodiscard]] task<std::int64_t> unlink(keys_type&&... keys) const {
        const std::string_view views[]{std::string_view(keys)...};
        return unlink(std::span<const std::string_view>(views));
    }

    // Alternating key/value arguments: mset(k1, v1, k2, v2).
    template <typename... args_type>
        requires detail::redis_pair_argument_pack<args_type...>
    [[nodiscard]] task<void> mset(args_type&&... args) const {
        const auto items = pair_arguments(std::string_view(args)...);
        return mset(std::span<const std::pair<std::string_view, std::string_view>>(items));
    }

    // Four or more arguments; hset(key, field, value) stays on the single-field
    // overload above rather than routing one pair through the batch path.
    template <typename... args_type>
        requires(detail::redis_pair_argument_pack<args_type...> && sizeof...(args_type) >= 4)
    [[nodiscard]] task<std::int64_t> hset(std::string_view key, args_type&&... args) const {
        const auto fields_value = pair_arguments(std::string_view(args)...);
        return hset(key, std::span<const std::pair<std::string_view, std::string_view>>(fields_value));
    }

    template <typename... field_types>
        requires detail::redis_argument_pack<field_types...>
    [[nodiscard]] task<std::vector<std::optional<std::string>>> hmget(std::string_view key, field_types&&... fields) const {
        const std::string_view views[]{std::string_view(fields)...};
        return hmget(key, std::span<const std::string_view>(views));
    }

    template <typename... keys_type>
        requires detail::redis_argument_pack<keys_type...>
    [[nodiscard]] task<std::vector<std::string>> sinter(keys_type&&... keys) const {
        const std::string_view views[]{std::string_view(keys)...};
        return sinter(std::span<const std::string_view>(views));
    }

    template <typename... keys_type>
        requires detail::redis_argument_pack<keys_type...>
    [[nodiscard]] task<std::vector<std::string>> sunion(keys_type&&... keys) const {
        const std::string_view views[]{std::string_view(keys)...};
        return sunion(std::span<const std::string_view>(views));
    }

    template <typename... keys_type>
        requires detail::redis_argument_pack<keys_type...>
    [[nodiscard]] task<std::vector<std::string>> sdiff(keys_type&&... keys) const {
        const std::string_view views[]{std::string_view(keys)...};
        return sdiff(std::span<const std::string_view>(views));
    }

    template <typename... sha1s_type>
        requires detail::redis_argument_pack<sha1s_type...>
    [[nodiscard]] task<std::vector<bool>> script_exists(sha1s_type&&... sha1s) const {
        const std::string_view views[]{std::string_view(sha1s)...};
        return script_exists(std::span<const std::string_view>(views));
    }

    // 每批命令共享一次租约及同一连接，WAIT/WAITAOF 只保护该批中位于
    // 屏障之前的写入，不保护此前通过独立 handle 调用完成的写入。
    // 迁移示例（不能写成先 co_await set()，再独立 command("WAIT", ...)）：
    //   auto batch = redis.with_options({.timeout_ = std::chrono::seconds(2)}).pipeline();
    //   batch.set("key", "value").command("WAIT", "1", "1000");
    //   auto replies = co_await std::move(batch).exec();
    // 检查每条写入回复是否为 error，再检查 replies[1].integer() >= 1；
    // WAITAOF 使用 command("WAITAOF", "1", "1", "1000")，检查返回数组的
    // 本地与副本 fsync 数量。服务端超时仍返回计数，不代表已达到目标。
    // 整批继承 operation_options 的总超时/取消；普通池 command_timeout_
    // 仍限制整批写入与全部回复，不因屏障而停用或逐条重启。
    // 屏障必须有 operation timeout 或可取消 token；timeout=0 的 Redis
    // 参数不绕过客户端限制。取消/传输失败关闭连接，已完成写入不回滚。
    // WAIT/WAITAOF 不赋予 Redis 强一致性；事务内不阻塞，故 transaction 拒绝。
    // Redis 的 current connection 契约：
    // https://redis.io/docs/latest/commands/wait/
    // https://redis.io/docs/latest/commands/waitaof/
    [[nodiscard]] redis_pipeline pipeline() const;
    [[nodiscard]] redis_transaction transaction() const;

    // Delivers to a channel on the ordinary pool; returns the receiver count.
    [[nodiscard]] task<std::int64_t> publish(std::string_view channel, std::string_view payload) const;
    // A dedicated Pub/Sub connection bound to this handle's scope, stop token
    // and operation timeout. Like an infinite blocking wait, it requires a
    // stoppable token or a finite timeout (std::invalid_argument otherwise).
    [[nodiscard]] redis_subscription subscription() const;

    // Validates `config` against the entity schema: the prefix must be
    // non-empty without NUL bytes, and each index column must be a unique,
    // identifier-named entity column whose type fits the index kind.
    // Violations throw std::invalid_argument.
    template <typename entity_type>
    [[nodiscard]] redis_repository<entity_type> repository() const;
    template <typename entity_type>
    [[nodiscard]] redis_repository<entity_type> repository(const redis_repository_config& config) const;

private:
    // Reassembles a flat alternating argument list into the pair sequence the
    // span overloads take.
    template <typename... args_type>
    [[nodiscard]] static constexpr auto pair_arguments(args_type... args) {
        static_assert(sizeof...(args_type) % 2 == 0);
        const std::string_view flat[]{args...};
        std::array<std::pair<std::string_view, std::string_view>, sizeof...(args_type) / 2> pairs{};
        for (std::size_t index = 0; index < pairs.size(); ++index) {
            pairs[index] = {flat[2 * index], flat[2 * index + 1]};
        }
        return pairs;
    }

    friend class detail::redis_client_runtime;
    template <typename entity_type>
    friend class redis_repository;

    // ORM commands are compiled by the framework straight into owned storage
    // and are never stateful or blocking. The storage is moved into the
    // command when it already lives in the handle's resource and copied into
    // that resource otherwise.
    [[nodiscard]] task<redis_value> command_owned(std::vector<std::string>&& args) const;
    // ORM scripts run as EVALSHA; a NOSCRIPT reply loads the body through
    // EVAL within the same operation.
    [[nodiscard]] task<redis_value> script_owned(const detail::redis_orm_script& script, std::vector<std::string>&& args) const;

    template <typename result_type, typename mapper_type>
    [[nodiscard]] task<result_type> command_mapped(std::vector<std::string>&& args, mapper_type mapper) const {
        registration_.require_active();
        return scoped(detail::map_redis_command<result_type>(command_owned(std::move(args)), std::move(mapper)));
    }

    template <typename result_type, typename mapper_type>
    [[nodiscard]] task<result_type> script_mapped(const detail::redis_orm_script& script, std::vector<std::string>&& args, mapper_type mapper) const {
        registration_.require_active();
        return scoped(detail::map_redis_command<result_type>(script_owned(script, std::move(args)), std::move(mapper)));
    }

    // `io_metrics` is the request's counter block, or null outside a request.
    redis_session(detail::redis_client_runtime& runtime, operation_scope& operation_scope, detail::request_io_metrics* io_metrics) noexcept;
    // A standalone client handle: no request, so no request I/O metrics.
    redis_session(detail::redis_client_runtime& runtime, operation_scope& operation_scope, operation_options options) noexcept;

    template <typename t_type>
    [[nodiscard]] task<t_type> scoped(ruvia::task<t_type> task_value) const {
        return make_scoped_operation(registration_.scope(), std::move(task_value));
    }

    // `reply_timeout` starts when the command is written (see redis_pool).
    [[nodiscard]] task<redis_value> execute(detail::redis_pool& pool, detail::redis_owned_command&& command, std::optional<timeout_duration> reply_timeout = std::nullopt) const;

    // One scoped pooled command whose reply `mapper` decodes in the handle's
    // memory domain, with the map_redis_command mapper contract.
    template <typename result_type, typename mapper_type>
    [[nodiscard]] task<result_type> execute_mapped(detail::redis_pool& pool, detail::redis_owned_command&& command, mapper_type mapper, std::optional<timeout_duration> reply_timeout = std::nullopt) const {
        return scoped(detail::map_redis_command<result_type>(execute(pool, std::move(command), reply_timeout), std::move(mapper)));
    }

    template <typename result_type, typename mapper_type>
    [[nodiscard]] task<result_type> mapped(std::initializer_list<std::string_view> args, mapper_type mapper) const {
        registration_.require_active();
        return execute_mapped<result_type>(pool(), detail::make_owned_redis_command(std::span<const std::string_view>(args.begin(), args.size())), std::move(mapper));
    }

    friend class detail::worker_client_registry_view;
    [[nodiscard]] redis_session bind_caller_scope(::ruvia::operation_scope& scope, detail::request_io_metrics* io_metrics, const stop_token& stop_token_value) &&;
    static void expire_capability(void* target) noexcept;
    [[nodiscard]] detail::redis_pool& pool() const noexcept;
    [[nodiscard]] detail::redis_pool& blocking_pool() const noexcept;
    detail::redis_client_runtime* runtime_;
    detail::request_io_metrics* io_metrics_{nullptr};
    operation_options operation_options_;
    scoped_capability_registration registration_;
};

#ifdef RUVIA_ENABLE_REDIS
// One Redis client bound to one event_loop, independent of HTTP and application.
// Handles, repositories, cold operations and PMR results borrow this owner.
// Destroy them on the bound loop before destroying the client; shutdown joins
// started operations but does not extend the allocator lifetime of results.
class redis_client final {
public:
    redis_client(event_loop loop, const redis_config& config);
    ~redis_client();
    redis_client(const redis_client&) = delete;
    redis_client& operator=(const redis_client&) = delete;
    redis_client(redis_client&&) = delete;
    redis_client& operator=(redis_client&&) = delete;

    // Lazy, single-start and worker-affine, like ordinary commands.
    [[nodiscard]] task<void> connect() &;
    task<void> connect() && = delete;
    // Each call registers one allocation-free capability with the client
    // operation scope, so keep the returned handle for a sequence of commands
    // instead of calling redis() per command. Like context::redis(), the
    // handle carries no operation options; derive them with with_options().
    [[nodiscard]] redis_session redis() const&;
    redis_session redis() const&& = delete;

    // Thread-safe close request; sockets are closed only on the bound loop.
    void close() noexcept;
    // Await on the bound loop to cancel and join all started operations.
    [[nodiscard]] task<void> shutdown() &;
    task<void> shutdown() && = delete;
    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND;
    const worker_ref& worker() const&& = delete;

private:
    std::shared_ptr<detail::redis_client_state> state_;
};
#endif

}  // namespace ruvia

#ifdef RUVIA_ENABLE_REDIS
#endif

namespace ruvia {
enum class redis_index_kind : std::uint8_t;
class redis_predicate;
struct redis_find_options;
struct redis_write_options;
class redis_write_result;
}  // namespace ruvia

namespace ruvia::detail {

// Typed reply accessors that enforce the redis_value kind. A reply's type is
// chosen by the (untrusted) server, so a mismatch throws
// redis_error(protocol_error) rather than the raw accessors' std::logic_error,
// and an error reply throws redis_error(command_error).
[[nodiscard]] std::string_view redis_value_string(const redis_value& value);
[[nodiscard]] std::int64_t redis_value_integer(const redis_value& value);
[[nodiscard]] std::span<const redis_value> redis_value_array(const redis_value& value);
void throw_if_redis_error(const redis_value& value);

// Redis hash fields support owning scalar values and one string/integer key.
// Character types are text units, not numbers, so they are not Redis numbers.
template <typename t_type>
inline constexpr bool is_redis_entity_native_integer = is_model_integer<std::remove_cvref_t<t_type>>;

template <typename t_type>
inline constexpr bool is_redis_entity_native_float = std::is_same_v<std::remove_cvref_t<t_type>, float> || std::is_same_v<std::remove_cvref_t<t_type>, double>;

template <typename t_type>
inline constexpr bool is_redis_entity_native_number = is_redis_entity_native_integer<t_type> || is_redis_entity_native_float<t_type>;

// The one classification of Redis column value types: entity validation,
// index validation, query compilation and writes all derive from it.
enum class redis_column_kind : std::uint8_t {
    unsupported,
    text,
    bool_value,
    signed_value,
    unsigned_value,
    float32,
    float64,
};

template <typename t_type>
consteval redis_column_kind redis_number_kind() {
    if constexpr (std::is_same_v<t_type, float>) {
        return redis_column_kind::float32;
    } else if constexpr (std::is_same_v<t_type, double>) {
        return redis_column_kind::float64;
    } else if constexpr (std::is_signed_v<t_type>) {
        return redis_column_kind::signed_value;
    } else {
        return redis_column_kind::unsigned_value;
    }
}

template <typename t_type>
consteval redis_column_kind redis_column_value_kind() {
    using value_type = std::remove_cvref_t<t_type>;
    if constexpr (std::is_same_v<value_type, string> || std::is_same_v<value_type, std::string>) {
        return redis_column_kind::text;
    } else if constexpr (std::is_same_v<value_type, bool>) {
        return redis_column_kind::bool_value;
    } else if constexpr (is_redis_entity_native_number<value_type>) {
        return redis_number_kind<value_type>();
    } else {
        return redis_column_kind::unsupported;
    }
}

template <typename t_type>
inline constexpr bool is_redis_entity_scalar = redis_column_value_kind<t_type>() != redis_column_kind::unsupported;

template <typename t_type>
inline constexpr bool is_redis_entity_primary_key_scalar = redis_column_value_kind<t_type>() == redis_column_kind::text || redis_column_value_kind<t_type>() == redis_column_kind::signed_value || redis_column_value_kind<t_type>() == redis_column_kind::unsigned_value;

[[noreturn]] inline void throw_invalid_redis_scalar(std::string_view message) {
    throw redis_error(redis_error::code_type::protocol_error, message);
}

inline constexpr std::size_t redis_scalar_scratch_size = 128;
using redis_scalar_scratch_type = std::array<char, redis_scalar_scratch_size>;

template <typename t_type>
[[nodiscard]] std::string_view encode_redis_number(t_type value, std::span<char, redis_scalar_scratch_size> scratch) {
    if constexpr (std::is_floating_point_v<t_type>) {
        if (!std::isfinite(value)) {
            throw_invalid_redis_scalar("non-finite Redis scalar");
        }
        const auto result_value = std::to_chars(scratch.data(), scratch.data() + scratch.size(), value, std::chars_format::general, std::numeric_limits<t_type>::max_digits10);
        if (result_value.ec != std::errc{}) {
            throw_invalid_redis_scalar("failed to encode Redis scalar");
        }
        return {scratch.data(), static_cast<std::size_t>(result_value.ptr - scratch.data())};
    } else if constexpr (std::is_unsigned_v<t_type>) {
        return http_format_unsigned(std::span<char, http_max_unsigned_decimal_size>(scratch.data(), http_max_unsigned_decimal_size), value);
    } else {
        return http_format_signed(std::span<char, http_max_signed_decimal_size>(scratch.data(), http_max_signed_decimal_size), value);
    }
}

template <typename t_type>
[[nodiscard]] std::string_view encode_redis_scalar(const t_type& value, std::span<char, redis_scalar_scratch_size> scratch) {
    using scalar_type = std::remove_cvref_t<t_type>;
    if constexpr (std::is_same_v<scalar_type, string>) {
        return value.view();
    } else if constexpr (std::is_same_v<scalar_type, std::string> || std::is_same_v<scalar_type, std::string> || std::is_same_v<scalar_type, std::string_view>) {
        return value;
    } else if constexpr (std::is_same_v<scalar_type, bool>) {
        return value ? std::string_view("1") : std::string_view("0");
    } else if constexpr (is_redis_entity_native_number<scalar_type>) {
        return encode_redis_number(value, scratch);
    } else {
        static_assert(always_false<scalar_type>, "unsupported Redis scalar type");
    }
}

template <typename t_type>
[[nodiscard]] t_type decode_redis_scalar(std::string_view value) {
    using scalar_type = std::remove_cvref_t<t_type>;
    if constexpr (std::is_same_v<scalar_type, string>) {
        string result_value;
        result_value.assign_owned(value);
        return result_value;
    } else if constexpr (std::is_same_v<scalar_type, std::string>) {
        return std::string(value.data(), value.size());
    } else if constexpr (std::is_same_v<scalar_type, bool>) {
        if (value == "0") {
            return false;
        }
        if (value == "1") {
            return true;
        }
        throw_invalid_redis_scalar("malformed Redis boolean scalar");
    } else if constexpr (is_redis_entity_native_number<scalar_type>) {
        const auto parsed = parse_text_scalar<scalar_type>(value);
        if (std::holds_alternative<text_scalar_parse_error>(parsed)) {
            throw_invalid_redis_scalar("malformed Redis scalar");
        }
        return std::get<scalar_type>(parsed);
    } else {
        static_assert(always_false<scalar_type>, "unsupported Redis scalar type");
    }
}

// Every ORM entity key is `redis_entity_key_root + lower-hex(prefix) + ':' + id`.
inline constexpr std::string_view redis_entity_key_root = "ruvia:orm:";

[[nodiscard]] inline std::string redis_entity_storage_prefix(std::string_view prefix) {
    std::string result;
    result.reserve(redis_entity_key_root.size() + prefix.size() * 2U + 1U);
    result.append(redis_entity_key_root);
    http_append_lower_hex(result, prefix);
    result.push_back(':');
    return result;
}

[[nodiscard]] inline std::string redis_entity_key(std::string_view storage_prefix, std::string_view id) {
    if (id.empty()) {
        throw std::invalid_argument("Redis entity ID must not be empty");
    }
    std::string key;
    key.reserve(storage_prefix.size() + id.size());
    key.append(storage_prefix);
    key.append(id);
    return key;
}

[[nodiscard]] inline bool redis_entity_key_matches(std::string_view storage_prefix, std::string_view id, std::string_view key) noexcept {
    return !id.empty() && key.size() == storage_prefix.size() + id.size() && key.starts_with(storage_prefix) && key.substr(storage_prefix.size()) == id;
}

template <typename entity_type>
[[nodiscard]] std::optional<std::string_view> try_redis_entity_id(const entity_type& entity, std::span<char, redis_scalar_scratch_size> scratch) {
    std::optional<std::string_view> id;
    for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
        if constexpr (column_type::options.primary_key_) {
            if (entity.template is_set<column_type::name>() && !entity.template is_null<column_type::name>()) {
                const auto encoded = encode_redis_scalar(entity.template get<column_type::name>(), scratch);
                if (!encoded.empty()) {
                    id = encoded;
                }
            }
        }
    });
    return id;
}

template <typename entity_type>
[[nodiscard]] std::string_view redis_entity_id(const entity_type& entity, std::span<char, redis_scalar_scratch_size> scratch) {
    const auto id = try_redis_entity_id(entity, scratch);
    if (!id) {
        throw std::invalid_argument("Redis entity requires an ID");
    }
    return *id;
}

enum class redis_binary_operator : std::uint8_t { equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
    logical_and,
    logical_or,
    in,
    not_in };
enum class redis_literal_kind : std::uint8_t { null,
    string,
    signed_integer,
    unsigned_integer,
    float32,
    real,
    boolean };

// Literals own their text immediately. Only Redis scalar values are accepted;
// SQL expressions, statements and backend parameter types cannot enter this AST.
class redis_literal final {
    using storage_type = std::variant<std::monostate, std::string, std::int64_t, std::uint64_t, float, double, bool>;

public:
    redis_literal() noexcept = default;
    explicit redis_literal(std::nullptr_t) noexcept {}
    explicit redis_literal(std::string_view value) :
        storage_(std::in_place_type<std::string>, value) {}
    explicit redis_literal(const char* value) :
        redis_literal(value ? std::string_view(value) : throw std::invalid_argument("Redis predicate text must not be null")) {}
    template <typename value_type>
        requires std::is_same_v<value_type, bool>
    explicit redis_literal(value_type value) :
        storage_(value) {}
    template <typename value_type>
        requires is_model_integer<value_type>
    explicit redis_literal(value_type value) {
        if constexpr (std::is_signed_v<value_type>) {
            storage_.emplace<std::int64_t>(value);
        } else {
            storage_.emplace<std::uint64_t>(value);
        }
    }
    template <typename value_type>
        requires std::is_floating_point_v<value_type>
    explicit redis_literal(value_type value) {
        if constexpr (std::is_same_v<value_type, float>) {
            storage_.emplace<float>(value);
        } else {
            storage_.emplace<double>(checked_model_float<double>(value));
        }
    }
    explicit redis_literal(const string& value) :
        redis_literal(value.view()) {}
    redis_literal(redis_literal&&) noexcept = default;
    redis_literal& operator=(redis_literal&&) = delete;
    redis_literal(const redis_literal&) = default;
    redis_literal& operator=(const redis_literal&) = delete;

    redis_literal_kind kind() const noexcept {
        return static_cast<redis_literal_kind>(storage_.index());
    }
    std::string_view text() const noexcept {
        return std::get<std::string>(storage_);
    }
    std::int64_t signed_value() const noexcept {
        return std::get<std::int64_t>(storage_);
    }
    std::uint64_t unsigned_value() const noexcept {
        return std::get<std::uint64_t>(storage_);
    }
    float float32_value() const noexcept {
        return std::get<float>(storage_);
    }
    double real_value() const noexcept {
        return std::get<double>(storage_);
    }
    bool boolean_value() const noexcept {
        return std::get<bool>(storage_);
    }

private:
    storage_type storage_;
};

template <typename value_type>
concept redis_predicate_argument = requires(value_type&& value) { redis_literal(std::forward<value_type>(value)); };

// Only redis_field_reference creates leaves, and only redis_predicate combines
// them, so the tree has exactly these shapes: a comparison or IN is a binary
// node {field, literal} or {field, list}, BETWEEN is {field, lower, upper}, a
// list holds literals, and logical AND/OR is a binary node of two predicates.
struct redis_predicate_node final {
    enum class kind_type : std::uint8_t { field,
        literal,
        binary,
        between,
        list,
        logical_not };

    explicit redis_predicate_node(kind_type kind) noexcept :
        kind_(kind) {}
    explicit redis_predicate_node(redis_literal&& value) noexcept :
        kind_(kind_type::literal),
        literal_(std::move(value)) {}
    redis_predicate_node(const redis_predicate_node& other, std::size_t offset = 0) :
        kind_(other.kind_),
        binary_(other.binary_),
        field_(other.field_),
        entity_prefix_(other.entity_prefix_),
        literal_(other.literal_),
        operands_(other.operands_) {
        for (auto& operand : operands_) {
            operand += offset;
        }
    }
    redis_predicate_node(redis_predicate_node&&) noexcept = default;
    redis_predicate_node& operator=(redis_predicate_node&&) = delete;
    redis_predicate_node& operator=(const redis_predicate_node&) = delete;
    kind_type kind_;
    redis_binary_operator binary_{redis_binary_operator::equal};
    std::string_view field_{};
    std::string_view entity_prefix_{};
    redis_literal literal_;
    std::vector<std::size_t> operands_;
};

struct redis_predicate_storage final {
    explicit redis_predicate_storage(std::size_t initial_capacity = 0) {
        if (initial_capacity != 0) {
            nodes_.reserve(initial_capacity);
        }
    }
    redis_predicate_storage(const redis_predicate_storage&) = default;
    std::vector<redis_predicate_node> nodes_;
    std::size_t root_{};
    std::size_t add(redis_predicate_node::kind_type kind) {
        nodes_.emplace_back(kind);
        return nodes_.size() - 1;
    }
    template <redis_predicate_argument value_type>
    std::size_t add_value(value_type&& value) {
        redis_literal literal(std::forward<value_type>(value));
        nodes_.emplace_back(std::move(literal));
        return nodes_.size() - 1;
    }
};

using redis_orm_arguments_type = std::vector<std::string>;

// An ORM Lua script and the lowercase hex SHA1 Redis caches it under.
struct redis_orm_script final {
    std::string_view body_;
    std::string_view sha1_;
};

[[nodiscard]] const redis_orm_script& redis_orm_write_script();
[[nodiscard]] const redis_orm_script& redis_orm_delete_script();

// Both argument lists start with EVALSHA and the script digest.
[[nodiscard]] redis_orm_arguments_type redis_orm_write_arguments(std::string_view storage_prefix, std::string_view id, std::string_view mode, redis_write_options options, std::size_t extra_arguments);
[[nodiscard]] redis_orm_arguments_type redis_orm_delete_arguments(std::string_view storage_prefix, std::string_view id);
// Runs an EVALSHA argument list for `script`; a NOSCRIPT reply loads the body
// through EVAL once within the same operation. `io_metrics` is the request's
// counter block, or null outside a request.
[[nodiscard]] task<redis_value> execute_redis_orm_script(redis_pool& pool, const redis_orm_script& script, redis_orm_arguments_type args, request_io_metrics* io_metrics, operation_options options);
[[nodiscard]] redis_write_result redis_orm_exec_result(redis_value&& reply);
[[nodiscard]] redis_write_result redis_orm_delete_result(redis_value&& reply);
void redis_orm_status_result(redis_value&& reply);
[[nodiscard]] std::uint64_t redis_orm_count(const redis_value& reply);
[[nodiscard]] std::uint64_t redis_orm_count_result(redis_value&& reply);
[[nodiscard]] bool redis_orm_exists_result(redis_value&& reply);

// What the RediSearch compiler needs to know about one entity column.
struct redis_column_info final {
    std::string_view name_;
    redis_column_kind kind_{redis_column_kind::unsupported};
    bool primary_key_{false};
};

// The per-entity column table every compiler entry point below consumes.
template <typename entity_type>
inline constexpr auto redis_query_columns = [] {
    std::array<redis_column_info, std::tuple_size_v<typename entity_type::columns_type>> columns{};
    for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t i> {
        columns[i] = {column_type::name.view(), redis_column_value_kind<typename column_type::value_type>(), column_type::options.primary_key_};
    });
    return columns;
}();

struct redis_index_definition final {
    std::string column_;
    redis_index_kind kind_{};
    bool sortable_{false};
};

// A repository's validated storage and index layout. Only
// normalize_redis_mapping builds one, so every index column exists, is a safe
// identifier outside the reserved __ruvia_ namespace, is unique, and has an
// index kind that fits its value kind. `columns_` views the entity's static
// schema; `entity_prefix_` owns the declared prefix predicates are bound to,
// `prefix_` names the index, and `storage_prefix_` is the precomputed
// `"ruvia:orm:<hex(prefix_)>:"` hash-key prefix.
// `column_index_kinds_[i]` is the index kind of `columns_[i]` (none when the
// column is not indexed), so writes address it by descriptor index.
struct redis_mapping final {
    std::span<const redis_column_info> columns_;
    std::string entity_prefix_;
    std::string prefix_;
    std::string storage_prefix_;
    std::vector<redis_index_definition> indexes_;
    std::vector<redis_index_kind> column_index_kinds_;

    [[nodiscard]] redis_index_kind index_kind(std::string_view column) const noexcept {
        for (const auto& index : indexes_) {
            if (index.column_ == column) {
                return index.kind_;
            }
        }
        return redis_index_kind{};
    }
    [[nodiscard]] bool sortable(std::string_view column) const noexcept {
        for (const auto& index : indexes_) {
            if (index.column_ == column) {
                return index.sortable_;
            }
        }
        return false;
    }
};

// `columns` must be the entity's static descriptor table.
[[nodiscard]] redis_mapping normalize_redis_mapping(std::span<const redis_column_info> columns, std::string_view entity_prefix, const redis_repository_config& config);

// A TAG value is kept in a shadow hash field. The leading x makes an empty
// value distinct from a missing field, and hexadecimal encoding keeps every
// byte outside RediSearch's TAG grammar.
[[nodiscard]] std::string encode_redis_tag(std::string_view value);

// The canonical ID when the predicate is exactly `primary key == literal`.
[[nodiscard]] std::optional<std::string_view> redis_primary_key(const redis_mapping& mapping, const redis_predicate& predicate, std::span<char, redis_scalar_scratch_size> scratch);

// Appends the write-script triples for one entity field: the value (or its
// deletion when absent), its presence marker and, for TAG-encodable columns,
// its TAG shadow. A value in a NUMERIC index must be exactly representable.
void append_redis_entity_field(redis_orm_arguments_type& args, std::string_view field, std::optional<std::string_view> value, bool numeric_index, bool tag_shadow);

template <typename entity_type>
entity_type decode_redis_entity(std::span<const redis_value> fields_value, std::string_view key, std::string_view storage_prefix) {
    if (fields_value.size() % 2 != 0) {
        throw redis_error(redis_error::code_type::protocol_error, "invalid entity hash reply");
    }
    std::array<std::optional<std::string_view>, std::tuple_size_v<typename entity_type::columns_type>> values{};
    bool marker = false;
    for (std::size_t i = 0; i < fields_value.size(); i += 2) {
        const auto name = redis_value_string(fields_value[i]);
        const auto value = redis_value_string(fields_value[i + 1]);
        if (name.starts_with("__ruvia_")) {
            if (name == "__ruvia_entity") {
                if (marker || value != "1") {
                    throw redis_error(redis_error::code_type::protocol_error, "invalid entity storage marker");
                }
                marker = true;
            }
            continue;
        }
        bool matched = false;
        for_each_descriptor<typename entity_type::columns_type>([&]<typename field_type, std::size_t index> {
            if (!matched && name == field_type::name.view()) {
                if (values[index].has_value()) {
                    throw redis_error(redis_error::code_type::protocol_error, "duplicate entity field");
                }
                values[index] = value;
                matched = true;
            }
        });
    }
    if (!marker) {
        throw redis_error(redis_error::code_type::protocol_error, "entity storage marker missing");
    }
    entity_type entity;
    for_each_descriptor<typename entity_type::columns_type>([&]<typename field_type, std::size_t index> {
        if (values[index].has_value()) {
            entity.template set<field_type::name>(decode_redis_scalar<typename field_type::value_type>(*values[index]));
        } else if constexpr (field_type::options.nullable_) {
            entity.template set_null<field_type::name>();
        } else {
            throw redis_error(redis_error::code_type::protocol_error, "required entity field missing from hash");
        }
    });
    redis_scalar_scratch_type id_scratch;
    const auto id = try_redis_entity_id(entity, id_scratch);
    if (!id) {
        throw redis_error(redis_error::code_type::protocol_error, "invalid stored entity ID");
    }
    if (!redis_entity_key_matches(storage_prefix, *id, key)) {
        throw redis_error(redis_error::code_type::protocol_error, "entity ID does not match its Redis key");
    }
    return entity;
}

template <typename entity_type>
entity_type decode_redis_entity(const redis_value& reply, std::string_view key, std::string_view storage_prefix) {
    return decode_redis_entity<entity_type>(redis_value_array(reply), key, storage_prefix);
}

template <typename entity_type>
struct redis_map_one final {
    std::string key_;
    std::size_t storage_prefix_size_{0};
    std::optional<entity_type> operator()(redis_value&& reply) const {
        const auto fields_value = redis_value_array(reply);
        if (fields_value.empty()) {
            return std::nullopt;
        }
        return decode_redis_entity<entity_type>(fields_value, key_, std::string_view(key_).substr(0, storage_prefix_size_));
    }
};

template <typename entity_type>
struct redis_map_search final {
    std::string_view storage_prefix_;
    std::pair<entity_rows<entity_type>, std::uint64_t> decode(std::span<const redis_value> values) const {
        if (values.empty() || values.size() % 2 != 1) {
            throw redis_error(redis_error::code_type::protocol_error, "invalid entity search reply");
        }
        const auto count = redis_orm_count(values.front());
        const auto documents = (values.size() - 1) / 2;
        if (count < documents) {
            throw redis_error(redis_error::code_type::protocol_error, "invalid entity search count");
        }
        entity_rows<entity_type> rows;
        rows.reserve(documents);
        for (std::size_t i = 1; i < values.size(); i += 2) {
            const auto key = redis_value_string(values[i]);
            // A document may expire between index matching and field loading.
            if (values[i + 1].null()) {
                continue;
            }
            rows.push_back(decode_redis_entity<entity_type>(values[i + 1], key, storage_prefix_));
        }
        return {std::move(rows), count};
    }
    std::pair<entity_rows<entity_type>, std::uint64_t> operator()(redis_value&& reply) const {
        return decode(redis_value_array(reply));
    }
};

template <typename entity_type>
struct redis_map_many final {
    redis_map_search<entity_type> search_;
    // Set when the caller left `take_` unset: the reply must then hold every
    // match after this skip, or find() would silently truncate.
    std::optional<std::uint64_t> unpaged_skip_;
    entity_rows<entity_type> operator()(redis_value&& reply) const {
        const auto values = redis_value_array(reply);
        const auto documents = values.size() / 2;
        auto [rows, count] = search_.decode(values);
        if (unpaged_skip_.has_value() && count > *unpaged_skip_ && count - *unpaged_skip_ > documents) {
            throw redis_error(redis_error::code_type::command_error, "redis find() matched more entities than one default page; set take_ to page the results");
        }
        return std::move(rows);
    }
};

template <typename entity_type>
struct redis_map_first final {
    redis_map_search<entity_type> search_;
    std::optional<entity_type> operator()(redis_value&& reply) const {
        auto result_value = search_(std::move(reply));
        if (result_value.first.empty()) {
            return std::nullopt;
        }
        if (result_value.first.size() != 1) {
            throw redis_error(redis_error::code_type::protocol_error, "multiple entities in find_one reply");
        }
        return std::move(result_value.first[0]);
    }
};

}  // namespace ruvia::detail

namespace ruvia {

template <typename entity_type, fixed_string name>
class redis_field_reference;

namespace detail {
struct redis_predicate_access;
}

// An owning Redis filter. Literal text is copied into the process resource when
// the filter is built, so the filter may outlive the strings used to build it.
// Compilation copies every argument into the schema mapping's memory resource.
// NUMERIC predicates retain literal precision: float operands keep float's
// stored encoding for float32 columns and widen for other numeric columns;
// double operands never narrow. Cast explicitly when float rounding is intended.
class redis_predicate final {
public:
    redis_predicate() noexcept = default;
    redis_predicate(const redis_predicate& other) {
        if (other.storage_) {
            storage_ = std::make_unique<detail::redis_predicate_storage>(*other.storage_);
        }
    }
    redis_predicate& operator=(const redis_predicate& other) {
        if (this != &other) {
            *this = redis_predicate(other);
        }
        return *this;
    }
    redis_predicate(redis_predicate&&) noexcept = default;
    redis_predicate& operator=(redis_predicate&&) noexcept = default;
    [[nodiscard]] bool empty() const noexcept {
        return storage_ == nullptr;
    }
    redis_predicate& and_where(const redis_predicate& other) {
        return combine(detail::redis_binary_operator::logical_and, other);
    }
    redis_predicate& or_where(const redis_predicate& other) {
        return combine(detail::redis_binary_operator::logical_or, other);
    }
    friend redis_predicate operator&&(redis_predicate left, const redis_predicate& right) {
        left.append(detail::redis_binary_operator::logical_and, right);
        return left;
    }
    friend redis_predicate operator||(redis_predicate left, const redis_predicate& right) {
        left.append(detail::redis_binary_operator::logical_or, right);
        return left;
    }
    [[nodiscard]] redis_predicate operator!() const {
        if (empty()) {
            throw std::invalid_argument("cannot negate an empty Redis predicate");
        }
        redis_predicate result(*this);
        auto& storage = *result.storage_;
        const auto operand = storage.root_;
        storage.root_ = storage.add(detail::redis_predicate_node::kind_type::logical_not);
        storage.nodes_[storage.root_].operands_ = {operand};
        return result;
    }

private:
    template <typename, fixed_string>
    friend class redis_field_reference;
    friend struct detail::redis_predicate_access;
    static redis_predicate create(std::size_t initial_capacity = 0) {
        redis_predicate result;
        result.storage_ = std::make_unique<detail::redis_predicate_storage>(initial_capacity);
        return result;
    }
    redis_predicate& combine(detail::redis_binary_operator operation, const redis_predicate& other) {
        if (other.empty()) {
            throw std::invalid_argument("cannot combine empty Redis predicates");
        }
        if (empty()) {
            *this = other;
        } else {
            append(operation, other);
        }
        return *this;
    }
    void append(detail::redis_binary_operator operation, const redis_predicate& right) {
        if (empty() || right.empty()) {
            throw std::invalid_argument("cannot combine empty Redis predicates");
        }
        auto& storage = *storage_;
        const auto left_root = storage.root_;
        const auto offset = storage.nodes_.size();
        const auto right_root = right.storage_->root_ + offset;
        const auto count = right.storage_->nodes_.size();
        detail::redis_predicate_node root_node(detail::redis_predicate_node::kind_type::binary);
        root_node.binary_ = operation;
        root_node.operands_ = {left_root, right_root};
        storage.nodes_.reserve(offset + count + 1);
        try {
            for (std::size_t index = 0; index < count; ++index) {
                storage.nodes_.emplace_back(right.storage_->nodes_[index], offset);
            }
            storage.nodes_.push_back(std::move(root_node));
        } catch (...) {
            while (storage.nodes_.size() > offset) {
                storage.nodes_.pop_back();
            }
            throw;
        }
        storage.root_ = storage.nodes_.size() - 1;
    }
    std::unique_ptr<detail::redis_predicate_storage> storage_;
};

struct redis_column_options final {
    bool primary_key_{false};
    bool nullable_{false};
};

template <fixed_string field_name, typename t_type, redis_column_options options_type = {}>
struct redis_column final {
    static_assert(detail::is_redis_entity_scalar<t_type>, "Redis columns require owning scalar values");
    static_assert(!options_type.primary_key_ || (detail::is_redis_entity_primary_key_scalar<t_type> && !options_type.nullable_), "Redis primary keys must be non-nullable strings or integers");
    static constexpr auto name = field_name;
    using value_type = t_type;
    static constexpr auto options = options_type;
};

namespace detail {
template <typename t_type>
struct is_redis_column : std::false_type {};
template <fixed_string name, typename t_type, redis_column_options options_type>
struct is_redis_column<redis_column<name, t_type, options_type>> : std::true_type {};

template <typename columns>
inline constexpr bool redis_columns = []<std::size_t... indices>(std::index_sequence<indices...>) {
    return (is_redis_column<std::tuple_element_t<indices, columns>>::value && ...);
}(std::make_index_sequence<std::tuple_size_v<columns>>{});

// Column names are unique through entity_value_storage.
template <fixed_string key_prefix, typename... columns>
struct redis_entity_storage final {
    static_assert((is_redis_column<columns>::value && ...), "Redis entities require RUVIA_REDIS_COLUMN descriptors");
    static_assert((std::size_t{columns::options.primary_key_} + ... + std::size_t{0}) == 1, "Redis entities require exactly one primary key");
    static constexpr auto prefix_value = key_prefix;
    using type = entity_value_storage<columns...>;
};

template <typename entity>
concept redis_entity_schema = requires(const entity& value) {
    typename entity::redis_entity_type;
    typename entity::columns_type;
    requires std::same_as<entity, typename entity::redis_entity_type>;
    requires redis_columns<typename entity::columns_type>;
    { entity::prefix() } -> std::convertible_to<std::string_view>;
};
struct redis_predicate_access final {
    // Null for an empty predicate.
    static const redis_predicate_storage* storage(const redis_predicate& predicate) noexcept {
        return predicate.storage_.get();
    }
};
}  // namespace detail

template <typename entity_type, fixed_string name>
class redis_field_reference final {
    static_assert(detail::redis_entity_schema<entity_type>, "Redis field references require a Redis entity");
    static_assert(entity_type::template column_index<name>() < std::tuple_size_v<typename entity_type::columns_type>);
    using operation = detail::redis_binary_operator;
    using kind = detail::redis_predicate_node::kind_type;

public:
    template <detail::redis_predicate_argument value_type>
    [[nodiscard]] redis_predicate operator==(value_type&& value) const {
        return compare(operation::equal, std::forward<value_type>(value));
    }
    template <detail::redis_predicate_argument value_type>
    [[nodiscard]] redis_predicate operator!=(value_type&& value) const {
        return compare(operation::not_equal, std::forward<value_type>(value));
    }
    template <detail::redis_predicate_argument value_type>
    [[nodiscard]] redis_predicate operator<(value_type&& value) const {
        return compare(operation::less, std::forward<value_type>(value));
    }
    template <detail::redis_predicate_argument value_type>
    [[nodiscard]] redis_predicate operator<=(value_type&& value) const {
        return compare(operation::less_equal, std::forward<value_type>(value));
    }
    template <detail::redis_predicate_argument value_type>
    [[nodiscard]] redis_predicate operator>(value_type&& value) const {
        return compare(operation::greater, std::forward<value_type>(value));
    }
    template <detail::redis_predicate_argument value_type>
    [[nodiscard]] redis_predicate operator>=(value_type&& value) const {
        return compare(operation::greater_equal, std::forward<value_type>(value));
    }
    [[nodiscard]] redis_predicate is_null() const {
        return compare(operation::equal, nullptr);
    }
    [[nodiscard]] redis_predicate is_not_null() const {
        return compare(operation::not_equal, nullptr);
    }
    template <detail::redis_predicate_argument lower_type, detail::redis_predicate_argument upper_type>
    [[nodiscard]] redis_predicate between(lower_type&& lower, upper_type&& upper) const {
        auto result_value = create_field(4);
        auto& storage = *result_value.storage_;
        const auto lower_node = storage.add_value(std::forward<lower_type>(lower));
        const auto upper_node = storage.add_value(std::forward<upper_type>(upper));
        storage.root_ = storage.add(kind::between);
        storage.nodes_[storage.root_].operands_ = {0, lower_node, upper_node};
        return result_value;
    }
    template <typename value_type>
        requires detail::redis_predicate_argument<const value_type&>
    [[nodiscard]] redis_predicate in(std::span<const value_type> values) const {
        return list(values, operation::in);
    }
    template <typename value_type>
        requires detail::redis_predicate_argument<const value_type&>
    [[nodiscard]] redis_predicate in(std::initializer_list<value_type> values) const {
        return in(std::span<const value_type>(values.begin(), values.size()));
    }
    template <typename value_type>
        requires detail::redis_predicate_argument<const value_type&>
    [[nodiscard]] redis_predicate not_in(std::span<const value_type> values) const {
        return list(values, operation::not_in);
    }
    template <typename value_type>
        requires detail::redis_predicate_argument<const value_type&>
    [[nodiscard]] redis_predicate not_in(std::initializer_list<value_type> values) const {
        return not_in(std::span<const value_type>(values.begin(), values.size()));
    }

private:
    static redis_predicate create_field(std::size_t initial_capacity) {
        auto result_value = redis_predicate::create(initial_capacity);
        auto& storage = *result_value.storage_;
        const auto index = storage.add(kind::field);
        storage.nodes_[index].field_ = name.view();
        storage.nodes_[index].entity_prefix_ = entity_type::prefix();
        return result_value;
    }
    template <detail::redis_predicate_argument value_type>
    static redis_predicate compare(operation op, value_type&& value) {
        auto result_value = create_field(3);
        auto& storage = *result_value.storage_;
        const auto literal = storage.add_value(std::forward<value_type>(value));
        storage.root_ = storage.add(kind::binary);
        storage.nodes_[storage.root_].binary_ = op;
        storage.nodes_[storage.root_].operands_ = {0, literal};
        return result_value;
    }
    template <typename value_type>
    static redis_predicate list(std::span<const value_type> values, operation op) {
        auto result_value = create_field(values.size() + 3);
        auto& storage = *result_value.storage_;
        std::vector<std::size_t> operands;
        operands.reserve(values.size());
        for (const auto& value : values) {
            operands.push_back(storage.add_value(value));
        }
        const auto list_node = storage.add(kind::list);
        storage.nodes_[list_node].operands_ = std::move(operands);
        storage.root_ = storage.add(kind::binary);
        storage.nodes_[storage.root_].binary_ = op;
        storage.nodes_[storage.root_].operands_ = {0, list_node};
        return result_value;
    }
};

#define RUVIA_REDIS_COLUMN(name, value_type, ...) ::ruvia::redis_column<::ruvia::fixed_string{#name}, value_type __VA_OPT__(, ) __VA_ARGS__>

// set<field>() 检查整数范围与有限浮点溢出；整数列不隐式接受浮点输入。
// 失败保留原值和 unset/null 状态；经可变 get() 或显式转换执行的赋值由调用方负责。
#define RUVIA_REDIS_ENTITY(name, key_prefix, ...)                                                                      \
    struct name final {                                                                                                \
    private:                                                                                                           \
        using descriptor_type = ::ruvia::detail::redis_entity_storage<::ruvia::fixed_string{key_prefix}, __VA_ARGS__>; \
        using storage_type = typename descriptor_type::type;                                                           \
        storage_type entity_;                                                                                          \
        RUVIA_DETAIL_ENTITY_OWNER_API(name, storage_type, entity_)                                                     \
        using redis_entity_type = name;                                                                                \
        using columns_type = typename storage_type::columns_type;                                                      \
        static constexpr std::string_view prefix() noexcept {                                                          \
            return descriptor_type::prefix_value.view();                                                               \
        }                                                                                                              \
        template <::ruvia::fixed_string field_name>                                                                    \
        static constexpr ::ruvia::redis_field_reference<name, field_name> column() noexcept {                          \
            return {};                                                                                                 \
        }                                                                                                              \
        RUVIA_DETAIL_ENTITY_VALUE_API(entity_)                                                                         \
    };

enum class redis_order_direction : std::uint8_t {
    asc,
    desc,
};

struct redis_find_order final {
    borrowed_text column_{};
    redis_order_direction direction_{redis_order_direction::asc};
};

// RediSearch always pages results; this page size applies when `take_` is unset.
inline constexpr std::uint64_t redis_find_default_take = 100;

// Redis filters and pagination have no SQL relation, lock or cache options.
// With `take_` unset, find() returns every match or throws redis_error
// (command_error) when more than redis_find_default_take entities match after
// `skip_`; it never truncates silently. find_and_count() returns one page of
// that size together with the total count.
struct redis_find_options final {
    redis_predicate where_{};
    std::optional<redis_find_order> order_{};
    std::optional<query_row_count> skip_{};
    std::optional<query_row_count> take_{};
};

enum class redis_index_kind : std::uint8_t {
    none,
    tag,
    text,
    numeric,
};

struct redis_index_config final {
    borrowed_text column_{};
    redis_index_kind kind_{redis_index_kind::tag};
    bool sortable_{false};
};

struct redis_repository_config final {
    borrowed_text prefix_{};
    std::vector<redis_index_config> indexes_;
};

enum class redis_search_mode : std::uint8_t {
    rows,
    first,
    count,
};

// Validated, owning schema configuration; no connection or worker is required.
// Only immutable entity descriptors have static lifetime. The supplied memory
// resource must outlive the mapping and commands compiled from it.
class redis_search_mapping final {
public:
    template <typename entity_type>
        requires detail::redis_entity_schema<entity_type>
    [[nodiscard]] static redis_search_mapping create(const redis_repository_config& config = {}) {
        return redis_search_mapping(detail::normalize_redis_mapping(detail::redis_query_columns<entity_type>, entity_type::prefix(), config));
    }

    redis_search_mapping(const redis_search_mapping& other);
    redis_search_mapping(redis_search_mapping&&) noexcept = default;
    redis_search_mapping& operator=(const redis_search_mapping& other);
    redis_search_mapping& operator=(redis_search_mapping&& other);

    // rows uses take_ or redis_find_default_take; first caps that page at one.
    // count validates all options but counts all matches, ignoring pagination.
    // Commands own all arguments in this mapping's resource.
    [[nodiscard]] redis_compiled_command compile_find(const redis_find_options& options = {}, redis_search_mode mode = redis_search_mode::rows) const;
    [[nodiscard]] redis_compiled_command compile_create_index() const;
    // Drops only the index, never its documents.
    [[nodiscard]] redis_compiled_command compile_drop_index() const;

private:
    template <typename>
    friend class redis_repository;
    explicit redis_search_mapping(detail::redis_mapping&& mapping) noexcept :
        mapping_(std::move(mapping)) {}
    void swap(redis_search_mapping& other) noexcept;
    void require_valid() const;

    detail::redis_mapping mapping_;
};

// Omitted expiration preserves an existing TTL; new entities are persistent.
struct redis_write_options final {
    std::optional<std::chrono::milliseconds> ttl_{};
    bool persist_{false};
};

class redis_write_result final {
public:
    explicit constexpr redis_write_result(std::uint64_t affected_entities) noexcept :
        affected_entities_(affected_entities) {}
    [[nodiscard]] constexpr std::uint64_t affected_entities() const noexcept {
        return affected_entities_;
    }

private:
    std::uint64_t affected_entities_{};
};

template <typename entity_type>
class redis_repository final {
    static_assert(detail::redis_entity_schema<entity_type>, "Redis repositories require a RUVIA_REDIS_ENTITY declaration");

public:
    redis_repository(const redis_repository&) = delete;
    redis_repository& operator=(const redis_repository&) = delete;

    // A repository is tied to its worker operation scope. Moving it transfers
    // its value registration and PMR mapping; copying would create a second
    // owner with ambiguous mapping lifetime.
    redis_repository(redis_repository&& other) noexcept :
        session_(other.session_),
        mapping_(std::move(other.mapping_)),
        registration_(std::move(other.registration_), this) {
        // The source no longer has a registration. Destroy moved-from
        // PMR containers now: implementations may retain allocator-owned state.
        other.mapping_.reset();
    }
    redis_repository& operator=(redis_repository&&) = delete;

    [[nodiscard]] task<redis_write_result> insert(const entity_type& entity, redis_write_options options = {}) const {
        detail::redis_scalar_scratch_type id_scratch;
        return write(write_mode::insert, detail::redis_entity_id(entity, id_scratch), entity, options);
    }

    [[nodiscard]] task<redis_write_result> upsert(const entity_type& entity, redis_write_options options = {}) const {
        detail::redis_scalar_scratch_type id_scratch;
        return write(write_mode::upsert, detail::redis_entity_id(entity, id_scratch), entity, options);
    }

    [[nodiscard]] task<redis_write_result> update(const redis_predicate& predicate, const entity_type& changes, redis_write_options options = {}) const {
        detail::redis_scalar_scratch_type id_scratch;
        const auto id = require_primary_key(predicate, id_scratch);
        return write(write_mode::update, id, changes, options);
    }

    [[nodiscard]] task<entity_rows<entity_type>> find(const redis_find_options& options = {}) const {
        auto args = mapping_->compile_find(options).take_arguments();
        detail::redis_map_many<entity_type> mapper{{mapping().storage_prefix_}, options.take_.has_value() ? std::nullopt : std::optional<std::uint64_t>(options.skip_ ? options.skip_->value() : 0U)};
        return session_.template command_mapped<entity_rows<entity_type>>(std::move(args), std::move(mapper));
    }
    template <std::same_as<redis_predicate> predicate_type>
    [[nodiscard]] task<entity_rows<entity_type>> find(const predicate_type& where) const {
        return find(redis_find_options{.where_ = where});
    }

    [[nodiscard]] task<std::optional<entity_type>> find_one(const redis_find_options& options = {}) const {
        const bool plain = !options.order_.has_value() && !options.skip_ && !options.take_;
        if (plain) {
            detail::redis_scalar_scratch_type id_scratch;
            if (const auto id = detail::redis_primary_key(mapping(), options.where_, id_scratch)) {
                auto key = detail::redis_entity_key(mapping().storage_prefix_, *id);
                detail::redis_orm_arguments_type args;
                args.reserve(2);
                args.emplace_back("HGETALL");
                args.emplace_back(key);
                detail::redis_map_one<entity_type> mapper{std::move(key), mapping().storage_prefix_.size()};
                return session_.template command_mapped<std::optional<entity_type>>(std::move(args), std::move(mapper));
            }
        }
        auto args = mapping_->compile_find(options, redis_search_mode::first).take_arguments();
        detail::redis_map_first<entity_type> mapper{{mapping().storage_prefix_}};
        return session_.template command_mapped<std::optional<entity_type>>(std::move(args), std::move(mapper));
    }

    template <std::same_as<redis_predicate> predicate_type>
    [[nodiscard]] task<std::optional<entity_type>> find_one(const predicate_type& where) const {
        return find_one(redis_find_options{.where_ = where});
    }
    [[nodiscard]] task<std::pair<entity_rows<entity_type>, std::uint64_t>> find_and_count(const redis_find_options& options = {}) const {
        auto args = mapping_->compile_find(options).take_arguments();
        detail::redis_map_search<entity_type> mapper{mapping().storage_prefix_};
        return session_.template command_mapped<std::pair<entity_rows<entity_type>, std::uint64_t>>(std::move(args), std::move(mapper));
    }

    [[nodiscard]] task<std::uint64_t> count(const redis_find_options& options = {}) const {
        return session_.template command_mapped<std::uint64_t>(mapping_->compile_find(options, redis_search_mode::count).take_arguments(), detail::redis_orm_count_result);
    }

    template <std::same_as<redis_predicate> predicate_type>
    [[nodiscard]] task<std::uint64_t> count(const predicate_type& where) const {
        return count(redis_find_options{.where_ = where});
    }

    [[nodiscard]] task<bool> exists(const redis_find_options& options = {}) const {
        if (!options.order_.has_value() && !options.skip_ && !options.take_) {
            detail::redis_scalar_scratch_type id_scratch;
            if (const auto id = detail::redis_primary_key(mapping(), options.where_, id_scratch)) {
                const auto key = detail::redis_entity_key(mapping().storage_prefix_, *id);
                return session_.exists(key);
            }
        }
        return session_.template command_mapped<bool>(mapping_->compile_find(options, redis_search_mode::count).take_arguments(), detail::redis_orm_exists_result);
    }

    template <std::same_as<redis_predicate> predicate_type>
    [[nodiscard]] task<bool> exists(const predicate_type& where) const {
        return exists(redis_find_options{.where_ = where});
    }

    [[nodiscard]] task<redis_write_result> delete_by(const redis_predicate& predicate) const {
        detail::redis_scalar_scratch_type id_scratch;
        const auto id = require_primary_key(predicate, id_scratch);
        return delete_by_id(id);
    }

    [[nodiscard]] task<redis_write_result> remove(const entity_type& entity) const {
        detail::redis_scalar_scratch_type id_scratch;
        return delete_by_id(detail::redis_entity_id(entity, id_scratch));
    }

    [[nodiscard]] task<bool> expire(const redis_predicate& predicate, std::chrono::seconds ttl) const {
        detail::redis_scalar_scratch_type id_scratch;
        const auto id = require_primary_key(predicate, id_scratch);
        const auto key = detail::redis_entity_key(mapping().storage_prefix_, id);
        return session_.expire(key, ttl);
    }

    [[nodiscard]] task<redis_ttl> ttl(const redis_predicate& predicate) const {
        detail::redis_scalar_scratch_type id_scratch;
        const auto id = require_primary_key(predicate, id_scratch);
        const auto key = detail::redis_entity_key(mapping().storage_prefix_, id);
        return session_.pttl(key);
    }

    // Explicit schema management. Repository acquisition never creates an
    // index, and an existing index is reported by Redis.
    [[nodiscard]] task<void> create_index() const {
        return session_.template command_mapped<void>(mapping_->compile_create_index().take_arguments(), detail::redis_orm_status_result);
    }

    [[nodiscard]] task<void> drop_index() const {
        return session_.template command_mapped<void>(mapping_->compile_drop_index().take_arguments(), detail::redis_orm_status_result);
    }

private:
    friend class redis_session;

    explicit redis_repository(const redis_session& session_value, const redis_repository_config& config) :
        session_(session_value),
        mapping_(redis_search_mapping::create<entity_type>(config)),
        registration_(session_value.registration_.scope(), this, &redis_repository::expire_capability) {}

    [[nodiscard]] const detail::redis_mapping& mapping() const {
        if (!mapping_) {
            throw std::logic_error("Redis repository mapping has expired");
        }
        return mapping_->mapping_;
    }

    static void expire_capability(void* target) noexcept {
        auto& repository = *static_cast<redis_repository*>(target);
        repository.mapping_.reset();
    }

    [[nodiscard]] std::string_view require_primary_key(const redis_predicate& predicate, std::span<char, detail::redis_scalar_scratch_size> scratch) const {
        const auto result_value = detail::redis_primary_key(mapping(), predicate, scratch);
        if (!result_value) {
            throw std::invalid_argument("Redis repository operation requires an exact primary-key predicate");
        }
        return *result_value;
    }

    [[nodiscard]] task<redis_write_result> delete_by_id(std::string_view id) const {
        return session_.template script_mapped<redis_write_result>(detail::redis_orm_delete_script(), detail::redis_orm_delete_arguments(mapping().storage_prefix_, id), detail::redis_orm_delete_result);
    }

    enum class write_mode : std::uint8_t {
        insert,
        upsert,
        update,
    };

    // The write script's ARGV mode token.
    [[nodiscard]] static constexpr std::string_view write_mode_token(write_mode mode) {
        switch (mode) {
            case write_mode::insert:
                return "insert";
            case write_mode::upsert:
                return "upsert";
            case write_mode::update:
                return "update";
        }
        throw std::logic_error("invalid Redis write mode");
    }

    [[nodiscard]] task<redis_write_result> write(write_mode mode, std::string_view id, const entity_type& entity, redis_write_options options) const {
        static constexpr std::size_t required = [] {
            std::size_t count = 0;
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
                if constexpr (!column_type::options.nullable_) {
                    ++count;
                }
            });
            return count;
        }();
        static constexpr std::size_t max_mutation_arguments = [] {
            std::size_t count = 0;
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
                constexpr auto value_kind = detail::redis_column_value_kind<typename column_type::value_type>();
                constexpr bool tag_shadow = value_kind == detail::redis_column_kind::text || value_kind == detail::redis_column_kind::bool_value;
                count += tag_shadow ? 9U : 6U;
            });
            return count;
        }();
        auto args = detail::redis_orm_write_arguments(mapping().storage_prefix_, id, write_mode_token(mode), options, 1 + required + max_mutation_arguments);
        detail::redis_scalar_scratch_type scalar_scratch;
        args.emplace_back(detail::encode_redis_scalar(required, scalar_scratch));
        detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
            if constexpr (!column_type::options.nullable_) {
                args.emplace_back(column_type::name.view());
            }
        });

        const auto& column_index_kinds = mapping().column_index_kinds_;
        detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t index> {
            constexpr auto value_kind = detail::redis_column_value_kind<typename column_type::value_type>();
            constexpr bool tag_shadow = value_kind == detail::redis_column_kind::text || value_kind == detail::redis_column_kind::bool_value;
            const auto field = column_type::name.view();
            const bool numeric_index = !tag_shadow && column_index_kinds[index] == redis_index_kind::numeric;
            if constexpr (column_type::options.primary_key_) {
                if (mode == write_mode::update && entity.template is_set<column_type::name>() && detail::encode_redis_scalar(entity.template get<column_type::name>(), scalar_scratch) != id) {
                    throw std::invalid_argument("an entity update cannot change its ID");
                }
                detail::append_redis_entity_field(args, field, id, numeric_index, tag_shadow);
            } else if (entity.template is_set<column_type::name>()) {
                if (entity.template is_null<column_type::name>()) {
                    detail::append_redis_entity_field(args, field, std::nullopt, false, tag_shadow);
                } else {
                    const auto value = detail::encode_redis_scalar(entity.template get<column_type::name>(), scalar_scratch);
                    detail::append_redis_entity_field(args, field, value, numeric_index, tag_shadow);
                }
            }
        });

        return session_.template script_mapped<redis_write_result>(detail::redis_orm_write_script(), std::move(args), detail::redis_orm_exec_result);
    }

    redis_session session_;
    std::optional<redis_search_mapping> mapping_;
    scoped_capability_registration registration_;
};

template <typename entity_type>
redis_repository<entity_type> redis_session::repository() const {
    return repository<entity_type>(redis_repository_config{});
}

template <typename entity_type>
redis_repository<entity_type> redis_session::repository(const redis_repository_config& config) const {
    registration_.require_active();
    return redis_repository<entity_type>(*this, config);
}

}  // namespace ruvia
