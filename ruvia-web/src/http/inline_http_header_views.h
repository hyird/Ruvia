#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/ip_address.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/error.h"
#include "ruvia/web/middleware.h"
namespace ruvia::detail {

// Length-checked, data-independent (constant-time) byte comparison for secrets
// and authentication tags. It never
// early-outs on the first differing byte, so the time taken does not leak how
// far an attacker-supplied value matched the expected one. Unequal lengths are
// reported immediately; that is not secret since these values are fixed-length.
[[nodiscard]] inline bool constant_time_bytes_equal(
    std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    return left.empty() || CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// Field views held inline up to `inline_capacity`, otherwise in one vector
// sized up front; the chosen storage never changes after construction.
template <std::size_t inline_capacity = 8>
class inline_http_header_views final {
public:
    template <typename fields_type>
        requires requires(const fields_type& fields) { fields.size(); }
    explicit inline_http_header_views(const fields_type& fields)
        : inline_http_header_views(fields.size()) {
        for (const auto& field : fields) {
            push_back(http_header_view{field.name(), field.value()});
        }
    }

    // Reserves room for `count` views; at most `count` push_back calls follow.
    explicit inline_http_header_views(std::size_t count) {
        if (count > inline_.size()) {
            overflow_.reserve(count);
        }
    }

    inline_http_header_views(const inline_http_header_views&) = delete;
    inline_http_header_views& operator=(const inline_http_header_views&) = delete;
    inline_http_header_views(inline_http_header_views&&) = delete;
    inline_http_header_views& operator=(inline_http_header_views&&) = delete;

    void push_back(http_header_view field) {
        if (overflow_.capacity() != 0) {
            overflow_.push_back(field);
        } else {
            inline_[size_++] = field;
        }
    }

    [[nodiscard]] std::span<const http_header_view> view() const& noexcept RUVIA_LIFETIMEBOUND {
        if (overflow_.capacity() != 0) {
            return overflow_;
        }
        return std::span<const http_header_view>(inline_.data(), size_);
    }
    std::span<const http_header_view> view() const&& = delete;

    [[nodiscard]] operator std::span<const http_header_view>() const& noexcept RUVIA_LIFETIMEBOUND {
        return view();
    }
    operator std::span<const http_header_view>() const&& = delete;

private:
    std::array<http_header_view, inline_capacity> inline_;
    std::vector<http_header_view> overflow_;
    std::size_t size_{0};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Concatenates pieces into inline storage when they fit, otherwise into one
// string sized up front. view() borrows this object, so it neither copies nor moves.
template <std::size_t inline_capacity>
class inline_text_concat final {
public:
    inline_text_concat(std::initializer_list<std::string_view> pieces) {
        for (const auto piece : pieces) {
            if (piece.size() > (std::numeric_limits<std::size_t>::max)() - size_) {
                throw std::length_error("concatenated text length overflow");
            }
            size_ += piece.size();
        }
        if (size_ <= inline_capacity) {
            auto* output = inline_.data();
            for (const auto piece : pieces) {
                output = std::ranges::copy(piece, output).out;
            }
            return;
        }
        heap_.reserve(size_);
        for (const auto piece : pieces) {
            heap_.append(piece);
        }
    }

    inline_text_concat(const inline_text_concat&) = delete;
    inline_text_concat& operator=(const inline_text_concat&) = delete;

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return size_ <= inline_capacity ? std::string_view(inline_.data(), size_) : std::string_view(heap_);
    }
    std::string_view view() const&& = delete;

private:
    std::array<char, inline_capacity> inline_;
    std::string heap_;
    std::size_t size_{0};
};

// Append-only text with inline storage; spilling preserves the caller's resource.
template <std::size_t inline_capacity>
class inline_text_buffer final {
public:
    explicit inline_text_buffer(std::size_t reserve_hint = 0)
        : reserve_hint_(reserve_hint) {}

    inline_text_buffer(const inline_text_buffer&) = delete;
    inline_text_buffer& operator=(const inline_text_buffer&) = delete;

    void push_back(char value) {
        append(&value, 1);
    }

    void append(const char* data, std::size_t count) {
        if (count == 0) {
            return;
        }
        const auto current_size = size();
        if (count > heap_.max_size() - current_size) {
            throw std::length_error("appended text length overflow");
        }
        if (heap_.empty() && count <= inline_capacity - inline_size_) {
            std::ranges::copy(std::string_view(data, count), inline_.data() + inline_size_);
            inline_size_ += count;
            return;
        }
        if (heap_.empty()) {
            const auto spill_capacity = inline_capacity <= heap_.max_size() / 2 ? inline_capacity * 2 : heap_.max_size();
            heap_.reserve((std::max)({reserve_hint_, current_size + count, spill_capacity}));
            heap_.assign(inline_.data(), inline_size_);
        }
        heap_.append(data, count);
    }

    void append(std::string_view text) {
        append(text.data(), text.size());
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return heap_.empty() ? inline_size_ : heap_.size();
    }

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return heap_.empty() ? std::string_view(inline_.data(), inline_size_) : std::string_view(heap_);
    }
    std::string_view view() const&& = delete;

private:
    std::array<char, inline_capacity> inline_;
    std::string heap_;
    std::size_t inline_size_{0};
    std::size_t reserve_hint_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {
// The digests behind signed cookies (SHA-256) and JWT HS256/HS384/HS512.
enum class hmac_digest : std::uint8_t {
    sha256,
    sha384,
    sha512,
};

inline constexpr std::size_t hmac_max_digest_size = 64;
// OpenSSL's one-shot MAC takes int-sized key and message lengths.
inline constexpr std::size_t hmac_max_input_size = static_cast<std::size_t>((std::numeric_limits<int>::max)());

[[nodiscard]] constexpr std::size_t hmac_digest_size(hmac_digest digest) noexcept {
    switch (digest) {
        case hmac_digest::sha256:
            return 32;
        case hmac_digest::sha384:
            return 48;
        case hmac_digest::sha512:
            return 64;
    }
    return 0;
}

[[nodiscard]] constexpr const char* openssl_digest_name(hmac_digest digest) noexcept {
    switch (digest) {
        case hmac_digest::sha256:
            return "SHA256";
        case hmac_digest::sha384:
            return "SHA384";
        case hmac_digest::sha512:
            return "SHA512";
    }
    return nullptr;
}

// Writes the hmac_digest_size(digest) bytes of HMAC(key, message) to the front
// of `output`. Returns false when key or message exceeds hmac_max_input_size,
// `output` is too small, or OpenSSL fails; callers check the sizes first to
// report them in their own terms.
[[nodiscard]] inline bool compute_hmac(
    hmac_digest digest, std::string_view key, std::string_view message, std::span<std::uint8_t> output) noexcept {
    const auto* const digest_name = openssl_digest_name(digest);
    const auto digest_size = hmac_digest_size(digest);
    if (digest_name == nullptr || key.size() > hmac_max_input_size || message.size() > hmac_max_input_size ||
        output.size() < digest_size) {
        return false;
    }
    std::size_t mac_size = 0;
    return EVP_Q_mac(nullptr, "HMAC", nullptr, digest_name, nullptr, key.data(), key.size(),
               reinterpret_cast<const unsigned char*>(message.data()), message.size(),
               reinterpret_cast<unsigned char*>(output.data()), output.size(), &mac_size) != nullptr &&
           mac_size == digest_size;
}
}  // namespace ruvia::detail

namespace ruvia::detail {

// base64(HMAC-SHA256) — fixed width, standard alphabet with padding.
inline constexpr std::size_t cookie_signature_size = 44;
static_assert(cookie_signature_size == http_base64_encoded_size(hmac_digest_size(hmac_digest::sha256)));

// Writes exactly cookie_signature_size characters to `output`. The cookie `name`
// is bound into the MAC (length-framed together with `value`) so a value signed
// for one cookie is not a valid signature for another under the same secret.
// Throws std::invalid_argument on an empty secret and std::length_error when
// the secret or length-framed message cannot be represented by OpenSSL's HMAC
// length parameters.
inline void write_cookie_signature(
    char* output, std::string_view secret, std::string_view name, std::string_view value) {
    if (secret.empty()) {
        throw std::invalid_argument("signed cookie secret must not be empty");
    }
    constexpr auto max_cookie_name_bytes =
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());
    if (secret.size() > hmac_max_input_size) {
        throw std::length_error("signed cookie secret is too large");
    }
    if (name.size() > max_cookie_name_bytes) {
        throw std::length_error("signed cookie name is too large");
    }
    // Length-frame the name so the name/value boundary is unambiguous regardless
    // of the bytes either contains (prevents name||value collisions).
    const auto name_len = static_cast<std::uint32_t>(name.size());
    const std::array<char, 4> name_len_bytes{
        static_cast<char>((name_len >> 24) & 0xFF),
        static_cast<char>((name_len >> 16) & 0xFF),
        static_cast<char>((name_len >> 8) & 0xFF),
        static_cast<char>(name_len & 0xFF),
    };
    // Assemble lenPrefix||name||value for the one-shot HMAC. Inline storage keeps
    // signing a typical cookie allocation-free.
    if (name.size() > hmac_max_input_size - name_len_bytes.size() ||
        value.size() > hmac_max_input_size - name_len_bytes.size() - name.size()) {
        throw std::length_error("signed cookie message is too large for HMAC");
    }
    const inline_text_concat<512> message(
        {std::string_view(name_len_bytes.data(), name_len_bytes.size()), name, value});

    std::array<std::uint8_t, hmac_digest_size(hmac_digest::sha256)> digest;
    if (!compute_hmac(hmac_digest::sha256, secret, message.view(), digest)) {
        throw std::runtime_error("signed cookie HMAC computation failed");
    }
    http_base64_encode(output, std::span<const std::uint8_t>(digest));
}
}  // namespace ruvia::detail

namespace ruvia::detail {

using ::ruvia::rate_limit_key_buffer_bytes;

[[nodiscard]] inline std::string_view format_ipv4_octets(
    const std::uint8_t* bytes_value, std::span<char, 15> buffer) noexcept {
    char* cursor_value = buffer.data();
    char* const end = buffer.data() + buffer.size();
    for (std::size_t i = 0; i < 4; ++i) {
        if (i != 0) {
            *cursor_value++ = '.';
        }
        cursor_value = std::to_chars(cursor_value, end, bytes_value[i]).ptr;
    }
    return std::string_view(buffer.data(), static_cast<std::size_t>(cursor_value - buffer.data()));
}

[[nodiscard]] inline std::string_view rate_limit_key_for(
    const ip_address& address, std::span<char, rate_limit_key_buffer_bytes> buffer) noexcept {
    if (address.family() == ip_address_family::ipv4) {
        return format_ipv4_octets(address.bytes().data(), std::span<char, 15>(buffer.data(), 15));
    }
    const auto raw = address.bytes();
    if (address.scope_id() != 0) {
        buffer[0] = 'v';
        buffer[1] = '6';
        buffer[2] = 's';
        buffer[3] = '/';
        http_encode_lower_hex(buffer.data() + 4, std::span<const unsigned char>(raw.data(), 16));
        buffer[36] = '/';
        const auto end = std::to_chars(
            buffer.data() + 37, buffer.data() + rate_limit_key_buffer_bytes, address.scope_id())
                             .ptr;
        return std::string_view(
            buffer.data(), static_cast<std::size_t>(end - buffer.data()));
    }
    if (address.is_ipv4_mapped()) {
        return format_ipv4_octets(raw.data() + 12, std::span<char, 15>(buffer.data(), 15));
    }
    buffer[0] = 'v';
    buffer[1] = '6';
    buffer[2] = '/';
    http_encode_lower_hex(buffer.data() + 3, std::span<const unsigned char>(raw.data(), 8));
    return std::string_view(buffer.data(), 19);
}

[[nodiscard]] inline std::string_view rate_limit_key_for(
    const ip_address& address, char (&buffer)[rate_limit_key_buffer_bytes]) noexcept {
    return rate_limit_key_for(address, std::span<char, rate_limit_key_buffer_bytes>(buffer));
}

[[nodiscard]] inline std::string_view rate_limit_key_for(
    std::string_view remote_address, std::span<char, rate_limit_key_buffer_bytes> buffer) noexcept {
    const auto parsed = ip_address::parse(remote_address);
    if (!parsed.has_value()) {
        return remote_address;
    }
    return rate_limit_key_for(*parsed, buffer);
}

[[nodiscard]] inline std::string_view rate_limit_key_for(
    std::string_view remote_address, char (&buffer)[rate_limit_key_buffer_bytes]) noexcept {
    return rate_limit_key_for(remote_address, std::span<char, rate_limit_key_buffer_bytes>(buffer));
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// Delays are unsigned milliseconds: Reset can reach two windows, beyond any
// signed millisecond count once a window nears milliseconds::max().
class rate_limit_rejection final {
public:
    constexpr rate_limit_rejection(
        std::uint64_t retry_after_ms, std::uint64_t reset_after_ms, std::size_t max_requests) noexcept
        : retry_after_ms_(retry_after_ms),
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
    rate_limiter(std::optional<rate_limit_rule> default_rule_per_worker,
        route_rate_limit_presence route_rules, std::size_t capacity_value);

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
    [[nodiscard]] std::optional<rate_limit_rejection> allow_default(
        std::string_view remote_address, std::chrono::steady_clock::time_point now) noexcept;

    // `rule_declaration` identifies the rule's declaration (its worker-local
    // middleware instance), distinct for every declaration on the route.
    [[nodiscard]] std::optional<rate_limit_rejection> allow_route(std::uintptr_t route_scope,
        const void* rule_declaration, std::string_view remote_address, const rate_limit_rule& rule,
        std::chrono::steady_clock::time_point now) noexcept;

    [[nodiscard]] rate_limit_decision decide_default(
        std::string_view key, std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] rate_limit_decision decide_rule(rate_limit_scope scope, std::string_view key,
        const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept;

private:
    [[nodiscard]] std::optional<rate_limit_rejection> allow(rate_limit_scope scope, std::string_view key,
        const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept;
    std::optional<rate_limit_rule> default_rule_per_worker_;
    std::vector<rate_limit_slot> slots_;
    rate_limit_hash_key hash_key_{};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// The instant a request's rate-limit decisions are made for: the arrival its
// owner recorded (test_app supplies one), else a steady-clock reading taken
// only once a rule is actually consulted.
[[nodiscard]] inline std::chrono::steady_clock::time_point rate_limit_instant(
    const std::optional<std::chrono::steady_clock::time_point>& arrival) noexcept {
    return arrival.has_value() ? *arrival : std::chrono::steady_clock::now();
}

[[nodiscard]] inline std::optional<rate_limit_rejection> decide_request_rate_limit(rate_limiter* limiter,
    const ip_address& client_address, const std::optional<std::chrono::steady_clock::time_point>& arrival) noexcept {
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
    explicit rate_limit_rejected(rate_limit_rejection rejection) noexcept
        : rejection_(rejection) {}

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
