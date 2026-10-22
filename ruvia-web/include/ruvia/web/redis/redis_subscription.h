#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/web/detail/redis/redis_argument_pack.h"
namespace ruvia {

namespace detail {
class redis_subscription_connection;
class redis_subscription_set;
struct redis_types_access;
}  // namespace detail

enum class redis_pubsub_event_kind : std::uint8_t {
    // A SUBSCRIBE or PSUBSCRIBE confirmation for one channel or pattern.
    subscribed,
    // An UNSUBSCRIBE or PUNSUBSCRIBE confirmation for one channel or pattern.
    unsubscribed,
    // A published message. pattern() is set when a pattern subscription
    // delivered it.
    message,
};

// One Pub/Sub reply in wire order. Every view borrows the subscription's
// current reply storage and is valid until the next read() starts or the
// subscription is destroyed; copy what outlives that.
class redis_pubsub_event final {
public:
    [[nodiscard]] redis_pubsub_event_kind kind() const noexcept {
        return kind_;
    }

    // The channel a message was published to, or the channel a confirmation
    // names; empty for a pattern confirmation.
    [[nodiscard]] std::string_view channel() const noexcept {
        return channel_;
    }

    // The matched pattern of a pattern subscription; empty otherwise.
    [[nodiscard]] std::string_view pattern() const noexcept {
        return pattern_;
    }

    [[nodiscard]] std::string_view payload() const noexcept {
        return payload_;
    }

    // Channels and patterns this connection is subscribed to after a
    // confirmation; zero for a message.
    [[nodiscard]] std::int64_t subscription_count() const noexcept {
        return subscription_count_;
    }

private:
    friend struct detail::redis_types_access;

    redis_pubsub_event(redis_pubsub_event_kind kind, std::string_view channel, std::string_view pattern,
        std::string_view payload, std::int64_t subscription_count) noexcept
        : kind_(kind),
          channel_(channel),
          pattern_(pattern),
          payload_(payload),
          subscription_count_(subscription_count) {}

    redis_pubsub_event_kind kind_;
    std::string_view channel_;
    std::string_view pattern_;
    std::string_view payload_;
    std::int64_t subscription_count_;
};

// One dedicated Pub/Sub connection, created by redis_handle::subscription().
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
// closes the subscription and throws redis_error(timeout).
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
    scoped_operation<void> subscribe(std::span<const std::string_view> channels) &;
    scoped_operation<void> subscribe(std::span<const std::string_view>) && = delete;
    scoped_operation<void> psubscribe(std::span<const std::string_view> patterns) &;
    scoped_operation<void> psubscribe(std::span<const std::string_view>) && = delete;
    // An empty list unsubscribes from every channel (pattern) of this connection.
    scoped_operation<void> unsubscribe(std::span<const std::string_view> channels = {}) &;
    scoped_operation<void> unsubscribe(std::span<const std::string_view> = {}) && = delete;
    scoped_operation<void> punsubscribe(std::span<const std::string_view> patterns = {}) &;
    scoped_operation<void> punsubscribe(std::span<const std::string_view> = {}) && = delete;

    template <typename... channels_type>
        requires detail::redis_argument_pack<channels_type...>
    [[nodiscard]] scoped_operation<void> subscribe(channels_type&&... channels) & {
        const std::string_view views[]{std::string_view(channels)...};
        return subscribe(std::span<const std::string_view>(views));
    }
    template <typename... channels_type>
        requires detail::redis_argument_pack<channels_type...>
    scoped_operation<void> subscribe(channels_type&&...) && = delete;

    template <typename... patterns_type>
        requires detail::redis_argument_pack<patterns_type...>
    [[nodiscard]] scoped_operation<void> psubscribe(patterns_type&&... patterns) & {
        const std::string_view views[]{std::string_view(patterns)...};
        return psubscribe(std::span<const std::string_view>(views));
    }
    template <typename... patterns_type>
        requires detail::redis_argument_pack<patterns_type...>
    scoped_operation<void> psubscribe(patterns_type&&...) && = delete;

    template <typename... channels_type>
        requires detail::redis_argument_pack<channels_type...>
    [[nodiscard]] scoped_operation<void> unsubscribe(channels_type&&... channels) & {
        const std::string_view views[]{std::string_view(channels)...};
        return unsubscribe(std::span<const std::string_view>(views));
    }
    template <typename... channels_type>
        requires detail::redis_argument_pack<channels_type...>
    scoped_operation<void> unsubscribe(channels_type&&...) && = delete;

    template <typename... patterns_type>
        requires detail::redis_argument_pack<patterns_type...>
    [[nodiscard]] scoped_operation<void> punsubscribe(patterns_type&&... patterns) & {
        const std::string_view views[]{std::string_view(patterns)...};
        return punsubscribe(std::span<const std::string_view>(views));
    }
    template <typename... patterns_type>
        requires detail::redis_argument_pack<patterns_type...>
    scoped_operation<void> punsubscribe(patterns_type&&...) && = delete;
    // The next confirmation or message. std::nullopt once the subscription is
    // closed by abort(), its stop token or worker shutdown. The server ending
    // the connection throws redis_error(io_error); an error reply (for example
    // a denied channel) throws redis_error(command_error) and leaves the
    // connection open; a malformed push closes it with protocol_error.
    // Requires a completed subscribe() first: std::logic_error otherwise.
    [[nodiscard]] scoped_operation<std::optional<redis_pubsub_event>> read() &;
    scoped_operation<std::optional<redis_pubsub_event>> read() && = delete;

    // Closes the connection now; a pending read() completes with std::nullopt.
    void abort() noexcept;

private:
    friend class redis_handle;

    redis_subscription(detail::redis_subscription_set& subscriptions, operation_scope& scope,
        operation_options options);

    static void expire_capability(void* target) noexcept;
    scoped_operation<void> send(std::string_view command, std::span<const std::string_view> names, bool require_names);

    std::unique_ptr<detail::redis_subscription_connection, pmr_object_deleter<detail::redis_subscription_connection>>
        connection_;
    // Declared after the connection so it unlinks before the connection closes.
    scoped_capability_registration registration_;
};

}  // namespace ruvia
