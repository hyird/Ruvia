#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "borrowed_text.h"
#include "http_error.h"
#include "http_response.h"
#include "rate_limiting.h"

// Worker-local sliding-window rate limiting: client keys, the per-worker
// table, the 429 rejection carried through error dispatch, and its headers.
namespace ruvia::detail {

using ::ruvia::rate_limit_key_buffer_bytes;

// Dotted IPv4 text, at most 15 bytes, so it always fits the key buffer.
[[nodiscard]] inline std::string_view format_ipv4_key(const ip_address& address, std::span<char, rate_limit_key_buffer_bytes> buffer) noexcept {
    std::array<char, ip_address::max_text_size> text;
    const auto formatted = address.normalized_ipv4().format_to(text);
    std::copy(formatted.begin(), formatted.end(), buffer.begin());
    return std::string_view(buffer.data(), formatted.size());
}

[[nodiscard]] inline std::string_view rate_limit_key_for(const ip_address& address, std::span<char, rate_limit_key_buffer_bytes> buffer) noexcept {
    if (address.family() == ip_address_family::ipv4) {
        return format_ipv4_key(address, buffer);
    }
    const auto raw = address.bytes();
    if (address.scope_id() != 0) {
        buffer[0] = 'v';
        buffer[1] = '6';
        buffer[2] = 's';
        buffer[3] = '/';
        http_encode_lower_hex(buffer.data() + 4, std::span<const unsigned char>(raw.data(), 16));
        buffer[36] = '/';
        const auto end = std::to_chars(buffer.data() + 37, buffer.data() + rate_limit_key_buffer_bytes, address.scope_id()).ptr;
        return std::string_view(buffer.data(), static_cast<std::size_t>(end - buffer.data()));
    }
    if (address.is_ipv4_mapped()) {
        return format_ipv4_key(address, buffer);
    }
    buffer[0] = 'v';
    buffer[1] = '6';
    buffer[2] = '/';
    http_encode_lower_hex(buffer.data() + 3, std::span<const unsigned char>(raw.data(), 8));
    return std::string_view(buffer.data(), 19);
}

[[nodiscard]] inline std::string_view rate_limit_key_for(const ip_address& address, char (&buffer)[rate_limit_key_buffer_bytes]) noexcept {
    return rate_limit_key_for(address, std::span<char, rate_limit_key_buffer_bytes>(buffer));
}

[[nodiscard]] inline std::string_view rate_limit_key_for(std::string_view remote_address, std::span<char, rate_limit_key_buffer_bytes> buffer) noexcept {
    const auto parsed = ip_address::parse(remote_address);
    if (!parsed.has_value()) {
        return remote_address;
    }
    return rate_limit_key_for(*parsed, buffer);
}

[[nodiscard]] inline std::string_view rate_limit_key_for(std::string_view remote_address, char (&buffer)[rate_limit_key_buffer_bytes]) noexcept {
    return rate_limit_key_for(remote_address, std::span<char, rate_limit_key_buffer_bytes>(buffer));
}

// Delays are unsigned milliseconds: Reset can reach two windows, beyond any
// signed millisecond count once a window nears milliseconds::max().
class rate_limit_rejection final {
public:
    constexpr rate_limit_rejection(std::uint64_t retry_after_ms, std::uint64_t reset_after_ms, std::size_t max_requests) noexcept :
        retry_after_ms_(retry_after_ms),
        reset_after_ms_(reset_after_ms),
        max_requests_(max_requests) {}

    // For a key the table holds, the earliest delay after which the refusing
    // rule's estimate admits it again. Refusals leave the estimate unchanged,
    // so retrying earlier only meets another refusal.
    [[nodiscard]] constexpr std::uint64_t retry_after_ms() const noexcept {
        return retry_after_ms_;
    }

    // The delay after which the key's sliding-window estimate reaches zero and
    // its whole budget is available again; never shorter than retry_after_ms().
    [[nodiscard]] constexpr std::uint64_t reset_after_ms() const noexcept {
        return reset_after_ms_;
    }

    // The limit of the rule that refused the request.
    [[nodiscard]] constexpr std::size_t max_requests() const noexcept {
        return max_requests_;
    }

private:
    std::uint64_t retry_after_ms_;
    std::uint64_t reset_after_ms_;
    std::size_t max_requests_;
};

enum class route_rate_limit_presence : std::uint8_t {
    absent,
    present,
};

// The counter family a decision consumes: the route a rule guards and the rule
// declaration itself, so stacked rules on one route each keep their own count
// and window.
struct rate_limit_scope final {
    std::uintptr_t route_{0};
    std::uintptr_t rule_{0};

    friend constexpr bool operator==(const rate_limit_scope&, const rate_limit_scope&) noexcept = default;
};

// Normalized IPv6 /64 keys use only 19 bytes. The larger bound also
// accommodates canonical peer strings carrying an IPv6 scope identifier.
inline constexpr std::size_t rate_limit_max_key_bytes = 64;

// Slots one lookup or insertion probes from a key's home slot. A key whose
// probe window holds only live entries of other keys takes the overflow decision.
inline constexpr std::size_t rate_limit_probe_window = 32;

// The 128-bit key of the table's slot-placement PRF.
using rate_limit_hash_key = std::array<std::uint64_t, 2>;

// Each key estimates its request count over the trailing window the way
// pingora-limits' Rate does: the previous window's count weighted by the part
// of it still inside the trailing window, plus the current window's count.
// Windows start at the key's first request and roll on that key's own grid.
struct rate_limit_slot final {
    rate_limit_scope scope_{};
    // 0 marks an empty slot; occupied slots never hash to 0.
    std::uint64_t key_hash_{0};
    std::chrono::steady_clock::time_point window_start_{};
    // Elapsed milliseconds from window_start_: two whole windows must pass
    // before another key can reclaim the slot.
    std::uint64_t stale_after_ms_{0};
    // Admissions only; never exceeds the rule's max_requests_.
    std::size_t current_count_{0};
    std::size_t previous_count_{0};
    std::uint8_t key_size_{0};
    std::array<char, rate_limit_max_key_bytes> key_{};
};
static_assert(sizeof(rate_limit_slot) <= 128, "worker rate-limit slots must stay compact");

// The one validation of a configured app-wide or router-wide rule: a
// positive limit and window, a known overflow policy and a power-of-two
// table capacity. Throws std::invalid_argument.
void validate_rate_limit_config(const rate_limit_config& config);

// One sliding-window table owned and accessed by exactly one
// web_worker_runtime worker. Startup allocates every slot;
// request-path lookup mutates ordinary worker-local state and performs no
// allocation, locking, atomics, or cross-thread coordination. Keys are (route
// scope, rule declaration, normalized remote address). Home slots come from a
// keyed hash whose key each table draws at startup, so which keys share a
// probe window cannot be computed offline to lock a victim out or to escape
// counting.
class rate_limiter final {
public:
    rate_limiter(std::optional<rate_limit_rule> default_rule_per_worker, route_rate_limit_presence route_rules, std::size_t capacity_value);

    rate_limiter(const rate_limiter&) = delete;
    rate_limiter& operator=(const rate_limiter&) = delete;

    // Owner thread: adopts storage preallocated for the first routed listener.
    // An already active table, including global-rate-limit counters, is kept.
    void adopt_route_storage(rate_limiter& prepared) noexcept;

    [[nodiscard]] bool has_default_rule() const noexcept {
        return default_rule_per_worker_.has_value();
    }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return slots_.size();
    }

    // Every decision takes the instant it is made for as a value, so the table
    // itself never reads a clock. Instants must not run backwards per key; an
    // earlier one counts as the start of the key's current window.
    [[nodiscard]] std::optional<rate_limit_rejection> allow_default(std::string_view remote_address, std::chrono::steady_clock::time_point now) noexcept;

    // `rule_declaration` identifies the rule's declaration (its worker-local
    // middleware instance), distinct for every declaration on the route.
    [[nodiscard]] std::optional<rate_limit_rejection> allow_route(std::uintptr_t route_scope, const void* rule_declaration, std::string_view remote_address, const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept;

    [[nodiscard]] rate_limit_decision decide_default(std::string_view key, std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] rate_limit_decision decide_rule(rate_limit_scope scope, std::string_view key, const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept;

private:
    [[nodiscard]] std::optional<rate_limit_rejection> allow(rate_limit_scope scope, std::string_view key, const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept;
    std::optional<rate_limit_rule> default_rule_per_worker_;
    std::vector<rate_limit_slot> slots_;
    rate_limit_hash_key hash_key_{};
};

// The instant a request's rate-limit decisions are made for: the arrival its
// owner recorded (test_app supplies one), else a steady-clock reading taken
// only once a rule is actually consulted.
[[nodiscard]] inline std::chrono::steady_clock::time_point rate_limit_instant(const std::optional<std::chrono::steady_clock::time_point>& arrival) noexcept {
    return arrival.has_value() ? *arrival : std::chrono::steady_clock::now();
}

[[nodiscard]] inline std::optional<rate_limit_rejection> decide_request_rate_limit(rate_limiter* limiter, const ip_address& client_address, const std::optional<std::chrono::steady_clock::time_point>& arrival) noexcept {
    if (limiter == nullptr || !limiter->has_default_rule()) {
        return std::nullopt;
    }
    char key_buffer[rate_limit_key_buffer_bytes];
    return limiter->allow_default(rate_limit_key_for(client_address, key_buffer), rate_limit_instant(arrival));
}

// Thrown by a route rate-limit middleware. Error dispatch answers it the way a
// driver answers the app-wide rule: rate_limit_rejection_error() through the
// application error handler, followed by the rejection headers.
class rate_limit_rejected final : public std::exception {
public:
    explicit rate_limit_rejected(rate_limit_rejection rejection) noexcept :
        rejection_(rejection) {}

    [[nodiscard]] const char* what() const noexcept override {
        return "rate limit exceeded";
    }

    [[nodiscard]] const rate_limit_rejection& rejection() const noexcept {
        return rejection_;
    }

private:
    rate_limit_rejection rejection_;
};

[[nodiscard]] http_error_info rate_limit_rejection_error() noexcept;

// Every 429 a rate-limit rule produces carries the same fields: Retry-After
// (RFC 6585 section 4, RFC 9110 section 10.2.3) and the X-RateLimit-* fields
// naming the refusing rule.
void apply_rate_limit_rejection_headers(http_response& response, const rate_limit_rejection& rejection);

}  // namespace ruvia::detail
