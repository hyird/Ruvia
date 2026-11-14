#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <asio/associated_allocator.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/associated_executor.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/buffer.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/completion_condition.hpp>
#include <asio/detail/socket_ops.hpp>
#include <asio/error.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/ssl/error.hpp>
#include <asio/ssl/stream.hpp>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/types.h>
#include <openssl/x509.h>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_wait.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/web/http_client_types.h"
#include "ruvia/web/http_server.h"
#include "ruvia/web/middleware.h"

#if defined(__unix__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#error "ruvia-web file responses need POSIX file descriptors or Windows file handles"
#endif
#if defined(_WIN32)
#include <winerror.h>
#endif
#if defined(__linux__)
#include <sys/ioctl.h>
#include <sys/socket.h>
#endif
#if defined(_WIN32)
#include <mswsock.h>

#include <asio/windows/overlapped_ptr.hpp>
#endif

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

namespace ruvia::detail {

// The protocol policy of every client and server TLS context. Startup only.
// - TLS 1.1 or newer, no TLS compression.
// - No renegotiation: RFC 9113 §9.2.1 requires it disabled for HTTP/2 over
//   TLS 1.2, and nothing here relies on it for other protocols.
// - TLS 1.2 cipher suites limited to ECDHE key exchange with AEAD ciphers
//   (RFC 9325 §4.1-4.2; none is on RFC 9113 Appendix A's prohibited list),
//   TLS_ECDHE_*_WITH_AES_128_GCM_SHA256 first (RFC 9325 §4.2.1). TLS 1.3
//   suites, all AEAD, keep OpenSSL's defaults.
// - TLS 1.1 alone additionally permits ECDHE AES-CBC-SHA suites and its
//   mandatory legacy handshake signatures. Certificate and key security
//   checks, and all TLS 1.2/1.3 security checks, retain the original callback.
// - The connection-wide outgoing record limit includes every key epoch and is
//   capped at 2^23. It therefore stays below RFC 9846 §5.5's per-key AES-GCM
//   limit and reaches neither a record sequence nor a KeyUpdate epoch limit.
// Servers additionally choose by their own preference order.
void validate_tls_record_limit(std::uint64_t limit);
void apply_tls_protocol_policy(SSL_CTX& context, std::uint64_t record_limit);

// Native drivers accepting an explicit cipher list use the same preference
// order. The security callback excludes the final four suites in TLS 1.2.
inline constexpr char tls_cipher_suites[] =
    "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
    "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
    "ECDHE-ECDSA-AES128-SHA:ECDHE-RSA-AES128-SHA:"
    "ECDHE-ECDSA-AES256-SHA:ECDHE-RSA-AES256-SHA";

// The policy's SSL_CTX message callback, which enforces the outgoing record
// limit. A context that installs its own message callback must forward every
// message to it first.
void count_tls_records(int writing, int version, int content_type, const void* bytes, std::size_t size,
    SSL* ssl, void* argument) noexcept;

// 原生数据库驱动自己创建 SSL_CTX。作用域只包围一次同步 start/cont/poll 调用，
// 不跨协程挂起：构造通知只安装回调和自有元数据，不驱动未初始化的 SSL 对象。
// HANDSHAKE_START 回调才在已初始化的 SSL 上禁用重协商和压缩。
// 证书回调在实际握手中验证证书链和 SAN，先于任何数据库应用数据。
class native_tls_context_scope final {
public:
    native_tls_context_scope(std::string_view host, std::uint64_t record_limit);
    ~native_tls_context_scope();
    native_tls_context_scope(const native_tls_context_scope&) = delete;
    native_tls_context_scope& operator=(const native_tls_context_scope&) = delete;
    void rethrow_failure() const;

private:
    friend void register_tls_policy_indexes();
    static void new_context(void* parent, void*, CRYPTO_EX_DATA*, int index, long, void*) noexcept;
    static thread_local native_tls_context_scope* current_;
    std::string_view host_;
    std::uint64_t record_limit_;
    native_tls_context_scope* previous_;
    std::exception_ptr failure_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

[[nodiscard]] inline bool is_ascii_dns_alnum(unsigned char byte) noexcept {
    return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z');
}

[[nodiscard]] inline bool is_ascii_dns_digit(unsigned char byte) noexcept {
    return byte >= '0' && byte <= '9';
}

// Validates an ASCII transport hostname with an optional root dot. This is
// intentionally narrower than a DNS owner name or URI reg-name; the
// four-numeric-label exclusion is lexical and does not parse IPv4 addresses.
[[nodiscard]] inline bool is_valid_dns_host(std::string_view host) noexcept {
    if (host.ends_with('.')) {
        host.remove_suffix(1);
    }
    if (host.empty() || host.ends_with('.') || host.size() > 253) {
        return false;
    }

    std::size_t labels = 0;
    bool saw_dot = false;
    bool all_labels_numeric = true;
    std::size_t label_start = 0;
    while (label_start < host.size()) {
        const auto label_end = host.find('.', label_start);
        const auto end = label_end == std::string_view::npos ? host.size() : label_end;
        const auto label_length = end - label_start;
        if (label_length == 0 || label_length > 63) {
            return false;
        }
        const auto first = static_cast<unsigned char>(host[label_start]);
        const auto last = static_cast<unsigned char>(host[end - 1]);
        if (!is_ascii_dns_alnum(first) || !is_ascii_dns_alnum(last)) {
            return false;
        }

        bool label_numeric = true;
        for (std::size_t i = label_start; i < end; ++i) {
            const auto byte = static_cast<unsigned char>(host[i]);
            if (!is_ascii_dns_alnum(byte) && byte != '-') {
                return false;
            }
            label_numeric = label_numeric && is_ascii_dns_digit(byte);
        }
        all_labels_numeric = all_labels_numeric && label_numeric;
        ++labels;
        if (label_end == std::string_view::npos) {
            break;
        }
        saw_dot = true;
        label_start = end + 1;
    }
    return !(saw_dot && labels == 4 && all_labels_numeric);
}

}  // namespace ruvia::detail

// What a finished TLS handshake negotiated with the peer: the selected ALPN
// protocol, the offered server_name and the verified peer certificate identity.

namespace ruvia::detail {

// The ALPN protocol the handshake selected; empty when none was negotiated.
[[nodiscard]] inline std::string_view tls_selected_alpn(SSL& ssl) noexcept {
    const unsigned char* selected = nullptr;
    unsigned int size = 0;
    SSL_get0_alpn_selected(&ssl, &selected, &size);
    return {reinterpret_cast<const char*>(selected), size};
}

// The host_name the client offered in server_name (RFC 6066 §3); empty when
// it offered none. The view lives as long as the SSL.
[[nodiscard]] inline std::string_view tls_server_name(SSL& ssl) noexcept {
    const char* name = SSL_get_servername(&ssl, TLSEXT_NAMETYPE_host_name);
    return name == nullptr ? std::string_view{} : std::string_view(name);
}

// Surface certificate presence independently of its Subject: RFC 5280 section
// 4.1.2.6 permits an empty DN with a critical subjectAltName. Conversion failure
// must not turn a verified identity into the "no certificate" alternative.
inline std::optional<std::string_view> extract_tls_client_certificate(SSL* ssl, std::string& out) {
    out.clear();
    const X509* const certificate = SSL_get0_peer_certificate(ssl);
    if (certificate == nullptr) {
        return std::nullopt;
    }
    const X509_NAME* subject = X509_get_subject_name(certificate);
    if (subject == nullptr) {
        throw std::runtime_error("TLS client certificate has no subject");
    }
    // Render the DN in RFC 2253 form through a memory BIO. Unlike
    // X509_NAME_oneline into a fixed 256-byte buffer, this captures the full
    // subject without silent truncation and produces the unambiguous,
    // standard-parseable form recommended for authorization.
    const auto bio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()), &BIO_free);
    if (bio == nullptr) {
        throw std::runtime_error("TLS client certificate subject buffer could not be allocated");
    }
    if (X509_NAME_print_ex(bio.get(), subject, 0, XN_FLAG_RFC2253) < 0) {
        throw std::runtime_error("TLS client certificate subject could not be rendered");
    }
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio.get(), &data);
    if (length < 0 || (length > 0 && data == nullptr)) {
        throw std::runtime_error("TLS client certificate subject could not be read");
    }
    if (length > 0) {
        out.assign(data, static_cast<std::size_t>(length));
    }
    return std::string_view(out);
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// Fills bytes from OpenSSL's CSPRNG. Returns false when the generator cannot
// produce output; callers translate that into their own failure contract.
[[nodiscard]] inline bool fill_random_bytes(std::span<unsigned char> bytes) noexcept {
    return RAND_bytes_ex(nullptr, bytes.data(), bytes.size(), 0) == 1;
}

// Lowercase hex characters of a token: 24 random bytes, 192 bits.
inline constexpr std::size_t secure_token_size = 48;

[[nodiscard]] inline std::string_view generate_secure_token(std::span<char, secure_token_size> buffer) {
    std::array<unsigned char, secure_token_size / 2> raw;
    if (!fill_random_bytes(raw)) {
        throw std::runtime_error("secure random generation failed");
    }
    http_encode_lower_hex(buffer.data(), std::span<const unsigned char>(raw));
    return std::string_view(buffer.data(), buffer.size());
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// The certificate one server TLS connection presented, answering RFC 9110
// §7.4's question whether it is valid for a request's target host. RFC 9525
// §6.3 matching: subjectAltName DNS-ID or IP-ID only (the subject CN is never
// consulted), a wildcard only as a whole left-most label. Matching against the
// certificate, not the SNI name, keeps RFC 9113 §9.1.1 connection reuse
// working for every name the certificate covers.
//
// Lives on the connection's frame after the handshake and is used only on its
// thread. Verdicts are cached for the last few distinct hosts, so a
// connection pays OpenSSL's certificate scan once per host, not per request.
class tls_served_certificate final {
public:
    // `certificate` is the leaf the handshake selected and installed on its
    // SSL (SSL_get_certificate), borrowed for the connection's lifetime;
    // null fails every check closed.
    explicit tls_served_certificate(X509* certificate) noexcept
        : certificate_(certificate) {}

    tls_served_certificate(const tls_served_certificate&) = delete;
    tls_served_certificate& operator=(const tls_served_certificate&) = delete;

    // `origin` must ask for verify_certificate.
    [[nodiscard]] bool covers(const http_target_origin& origin) noexcept;

private:
    // DNS names are at most 253 octets without the trailing dot; a longer
    // host is checked without caching.
    static constexpr std::size_t cached_key_capacity = 253;
    static constexpr std::size_t key_storage_capacity = 256;
    static constexpr std::size_t cache_size = 4;

    struct cached_verdict final {
        std::uint8_t key_offset_{0};
        std::uint8_t key_size_{0};
        http_target_host_kind kind_{http_target_host_kind::dns_name};
        bool covered_{false};
    };

    [[nodiscard]] bool check(const http_target_origin& origin) const noexcept;

    X509* certificate_;
    std::array<char, key_storage_capacity> key_storage_{};
    std::array<cached_verdict, cache_size> cache_{};
    std::uint16_t key_used_{0};
    std::uint8_t cached_{0};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Throws asio::system_error for the failed `operation` with the oldest queued
// OpenSSL error, translated into the system or SSL category. Callers clear the
// queue before the call.
[[noreturn]] void throw_tls_credential_error(const char* operation);
// A certificate, its chain and private key, parsed from PEM once and
// installed into every SSL_CTX or server connection that presents them.
// Copies share the immutable OpenSSL objects through their own reference
// counts, so presenters never re-read or re-parse the source (and cannot
// observe different files if they rotate later).
class tls_certificate final {
public:
    // The one PEM identity loader for files. File names are NUL-terminated
    // UTF-8, which OpenSSL's file BIOs decode (on Windows into _wfopen). The
    // password is only borrowed while the key is decrypted; an empty password
    // uses the noninteractive default. Throws on unreadable, invalid or
    // mismatched files.
    [[nodiscard]] static tls_certificate load(const char* certificate_chain_file,
        const char* private_key_file, std::string_view private_key_password);
    // The same identity from PEM text in memory: the leaf certificate
    // followed by its chain, and the (possibly encrypted) private key. The
    // text is read in place, never copied; the password is borrowed as by
    // load(). Throws on invalid or mismatched PEM.
    [[nodiscard]] static tls_certificate load_pem(std::string_view certificate_chain,
        std::string_view private_key, std::string_view private_key_password);

    tls_certificate(const tls_certificate& other);
    tls_certificate(tls_certificate&& other) noexcept;
    tls_certificate& operator=(tls_certificate&& other) noexcept;
    ~tls_certificate();

    // Startup only: the context is not yet shared with any connection.
    void install(SSL_CTX& context) const;
    // Makes this the identity one server connection presents, replacing the
    // whole identity it held; called during its handshake, before the
    // certificate is chosen. The connection takes its own references. False
    // when OpenSSL refuses the identity.
    [[nodiscard]] bool present(SSL& connection) const noexcept;

private:
    // Takes additional references; throws std::bad_alloc when the chain
    // cannot be referenced.
    tls_certificate(X509* certificate, STACK_OF(X509) * chain, EVP_PKEY* private_key);
    // Checks that the scratch context's certificate and key match, then
    // references its identity.
    [[nodiscard]] static tls_certificate adopt_loaded(SSL_CTX& scratch);

    void release() noexcept;

    X509* certificate_{};
    STACK_OF(X509) * chain_ {};
    EVP_PKEY* private_key_{};
};

// A certificate trust store parsed once at startup and shared read-only by
// every SSL_CTX that verifies peers against it.
class tls_trust_store final {
public:
    // Loads a PEM CA bundle; the file name is NUL-terminated UTF-8.
    [[nodiscard]] static tls_trust_store load_file(const char* file);
    // OpenSSL's default verify locations (the system trust store).
    [[nodiscard]] static tls_trust_store load_default();

    tls_trust_store(const tls_trust_store& other);
    tls_trust_store(tls_trust_store&& other) noexcept;
    tls_trust_store& operator=(const tls_trust_store& other);
    tls_trust_store& operator=(tls_trust_store&& other) noexcept;
    ~tls_trust_store();

    // Startup only: replaces the context's trust store with this shared one.
    void install(SSL_CTX& context) const noexcept;

private:
    // Adopts the caller's reference.
    explicit tls_trust_store(X509_STORE* store) noexcept
        : store_(store) {}

    X509_STORE* store_{};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// A TLS file name in UTF-8, the encoding OpenSSL's file BIOs decode (on
// Windows into a wide name for _wfopen). Converting through char8_t never
// narrows a native wide path through the ANSI code page.
using tls_file_name = std::u8string;

// The one TLS path validator, shared by servers and clients: a path must not
// be empty, and since OpenSSL and the database drivers consume C strings, it
// must not contain NUL either. Throws std::invalid_argument.
[[nodiscard]] inline tls_file_name make_tls_file_name(const std::filesystem::path& file) {
    if (file.empty()) {
        throw std::invalid_argument("TLS file paths must not be empty");
    }
    auto name = file.u8string();
    if (name.find(u8'\0') != tls_file_name::npos) {
        throw std::invalid_argument("TLS file paths must not contain NUL bytes");
    }
    return name;
}

// The NUL-terminated UTF-8 form handed to OpenSSL and the database drivers.
[[nodiscard]] inline const char* tls_file_name_c_str(const tls_file_name& name RUVIA_LIFETIMEBOUND) noexcept {
    return reinterpret_cast<const char*>(name.c_str());
}
const char* tls_file_name_c_str(tls_file_name&&) = delete;

// One identity's validated file names. The private key password is not kept:
// it is needed only while the key is loaded.
struct tls_identity_files final {
    tls_file_name certificate_chain_file_;
    tls_file_name private_key_file_;
};

[[nodiscard]] inline tls_identity_files make_tls_identity_files(const tls_identity_config& identity) {
    return {
        .certificate_chain_file_ = make_tls_file_name(identity.certificate_chain_file_),
        .private_key_file_ = make_tls_file_name(identity.private_key_file_),
    };
}

[[nodiscard]] inline tls_identity_files copy_tls_identity_files(const tls_identity_files& files) {
    return files;
}

// Reads and parses the files once; throws on unreadable, invalid or mismatched files.
[[nodiscard]] inline tls_certificate load_tls_certificate(
    const tls_identity_files& files, std::string_view private_key_password) {
    return tls_certificate::load(tls_file_name_c_str(files.certificate_chain_file_),
        tls_file_name_c_str(files.private_key_file_), private_key_password);
}
[[nodiscard]] inline tls_trust_store load_tls_trust_store(const tls_file_name& ca_file) {
    return tls_trust_store::load_file(tls_file_name_c_str(ca_file));
}
}  // namespace ruvia::detail

namespace ruvia::detail {

class tls_certificate_snapshot final {
public:
    struct entry_type final {
        tls_certificate_id id_{};
        std::shared_ptr<const tls_certificate> certificate_;
    };

    [[nodiscard]] const tls_certificate* find(tls_certificate_id id) const noexcept {
        const auto it = std::ranges::lower_bound(entries_, id, {}, &entry_type::id_);
        return (it != entries_.end() && it->id_ == id) ? it->certificate_.get() : nullptr;
    }

    [[nodiscard]] bool contains(tls_certificate_id id) const noexcept {
        return find(id) != nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }

    void insert_or_assign(tls_certificate_id id, std::shared_ptr<const tls_certificate> certificate) {
        const auto it = std::ranges::lower_bound(entries_, id, {}, &entry_type::id_);
        if (it != entries_.end() && it->id_ == id) {
            it->certificate_ = std::move(certificate);
        } else {
            entries_.insert(it, entry_type{id, std::move(certificate)});
        }
    }

    void erase(tls_certificate_id id) noexcept {
        const auto it = std::ranges::lower_bound(entries_, id, {}, &entry_type::id_);
        if (it != entries_.end() && it->id_ == id) {
            entries_.erase(it);
        }
    }

private:
    std::vector<entry_type> entries_;
};

struct tls_certificate_store_access final {
    [[nodiscard]] static std::shared_ptr<const tls_certificate_snapshot> snapshot(
        const tls_certificate_store& store) noexcept;
};

// Same validated loader used by fixed listener identities and incremental IDs.
[[nodiscard]] tls_certificate load_server_tls_certificate(const tls_identity_config& identity);

}  // namespace ruvia::detail

namespace ruvia::detail {

// A failed claim leaves the lane untouched; the caller supplies its domain error.
class operation_lane_lease final {
public:
    explicit operation_lane_lease(bool& active) noexcept
        : active_(active ? nullptr : &active) {
        if (active_ != nullptr) {
            *active_ = true;
        }
    }

    operation_lane_lease(const operation_lane_lease&) = delete;
    operation_lane_lease& operator=(const operation_lane_lease&) = delete;
    operation_lane_lease(operation_lane_lease&& other) noexcept
        : active_(std::exchange(other.active_, nullptr)) {}
    operation_lane_lease& operator=(operation_lane_lease&&) = delete;

    ~operation_lane_lease() noexcept {
        if (active_ != nullptr) {
            *active_ = false;
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return active_ != nullptr;
    }

private:
    bool* active_;
};

[[nodiscard]] inline operation_lane_lease claim_operation_lane(bool& active, const char* in_progress_message) {
    operation_lane_lease lease(active);
    if (!lease) {
        throw std::logic_error(in_progress_message);
    }
    return lease;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

struct request_io_metrics final {
    std::uint64_t db_count_{0};
    std::uint64_t db_duration_micros_{0};
    std::uint64_t redis_count_{0};
    std::uint64_t redis_duration_micros_{0};
};

enum class request_io_kind : std::uint8_t {
    db = 0,
    redis = 1,
};

class scoped_request_io_timer final {
public:
    // `metrics` is the request's counter block, or null outside a request
    // (worker callbacks, standalone clients); null disables timing.
    scoped_request_io_timer(request_io_metrics* metrics, request_io_kind kind) noexcept
        : metrics_(metrics),
          kind_(kind),
          started_at_(metrics_ != nullptr ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{}) {}

    ~scoped_request_io_timer() {
        if (metrics_ == nullptr) {
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started_at_)
                                 .count();
        const auto micros = elapsed < 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(elapsed);
        if (kind_ == request_io_kind::db) {
            ++metrics_->db_count_;
            metrics_->db_duration_micros_ += micros;
        } else {
            ++metrics_->redis_count_;
            metrics_->redis_duration_micros_ += micros;
        }
    }

    scoped_request_io_timer(const scoped_request_io_timer&) = delete;
    scoped_request_io_timer& operator=(const scoped_request_io_timer&) = delete;

private:
    request_io_metrics* metrics_{nullptr};
    request_io_kind kind_{request_io_kind::db};
    std::chrono::steady_clock::time_point started_at_{};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

inline constexpr std::string_view default_capability_alias = "default";

inline void validate_capability_alias(std::string_view alias, const char* empty_message) {
    if (alias.empty()) {
        throw std::invalid_argument(empty_message);
    }
}

// Every named worker capability has the same startup representation: an alias
// plus validated, normalized configuration storage.
template <typename storage_type>
struct named_capability_definition final {
    using config_storage_type = storage_type;

    std::string alias_;
    storage_type config_;
};

// Replaces the definition registered under replacement.alias_ in place, so
// existing aliases preserve registration order, or appends a new alias.
template <typename definition_type>
void upsert_named_capability_definition(
    std::vector<definition_type>& definitions, definition_type&& replacement) {
    const std::string_view alias = replacement.alias_;
    for (auto& definition : definitions) {
        if (std::string_view(definition.alias_) == alias) {
            std::destroy_at(&definition);
            std::construct_at(&definition, std::move(replacement));
            return;
        }
    }
    definitions.push_back(std::move(replacement));
}

// Validates and normalizes the replacement before mutating retained application
// state; the new definition takes one owned copy of the name and configuration.
template <typename definition_type, typename source_config_type>
void upsert_named_capability_definition(std::vector<definition_type>& definitions,
    std::string_view alias, const source_config_type& source_config, const char* empty_message) {
    validate_capability_alias(alias, empty_message);
    upsert_named_capability_definition(definitions,
        definition_type{std::string(alias), typename definition_type::config_storage_type(source_config)});
}

// Validates the complete startup alias set without allocating. Capability
// owners call this before constructing pools, so an invalid later definition
// cannot leave a partially built worker capability graph behind.
template <typename entries_type>
void validate_capability_aliases(
    const entries_type& entries, const char* empty_message, const char* duplicate_message) {
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const std::string_view alias = entries[index].alias_;
        validate_capability_alias(alias, empty_message);
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (std::string_view(entries[previous].alias_) == alias) {
                throw std::invalid_argument(duplicate_message);
            }
        }
    }
}

// Immutable startup-built index shared by worker-local named capabilities.
// It owns one copy of each alias and binds it to the source registration
// position; source definitions need not outlive build(). Request-time lookup
// performs one allocation-free binary search.
class named_capability_index final {
public:
    named_capability_index() = default;
    void build(std::initializer_list<std::string_view> aliases) {
        build_aliases(aliases, [](std::string_view alias) noexcept { return alias; });
    }

    template <typename entries_type>
    void build(const entries_type& entries) {
        build_aliases(
            entries, [](const auto& entry_value) noexcept -> std::string_view { return entry_value.alias_; });
    }

    [[nodiscard]] std::optional<std::size_t> find(std::string_view alias) const noexcept {
        const auto match =
            std::ranges::lower_bound(aliases_, alias, {}, &named_capability_index::alias_view);
        if (match == aliases_.end() || match->alias_ != alias) {
            return std::nullopt;
        }
        return match->index_;
    }

    [[nodiscard]] std::optional<std::size_t> default_index() const noexcept {
        return default_index_;
    }

private:
    struct indexed_alias_type final {
        std::string alias_;
        std::size_t index_;
    };

    [[nodiscard]] static std::string_view alias_view(const indexed_alias_type& entry_value) noexcept {
        return entry_value.alias_;
    }

    template <typename entries_type, typename alias_projection_type>
    void build_aliases(const entries_type& entries, alias_projection_type alias_projection) {
        std::vector<indexed_alias_type> aliases;
        aliases.reserve(entries.size());
        std::optional<std::size_t> default_index;
        std::size_t index = 0;
        for (const auto& entry : entries) {
            const std::string_view alias = alias_projection(entry);
            aliases.push_back(indexed_alias_type{std::string(alias), index});
            if (alias == default_capability_alias) {
                default_index = index;
            }
            ++index;
        }
        std::ranges::sort(aliases, {}, &named_capability_index::alias_view);
        aliases_.swap(aliases);
        default_index_ = default_index;
    }

    std::vector<indexed_alias_type> aliases_;
    std::optional<std::size_t> default_index_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Receipts cover execution AND destruction of posted controls. Waiting never
// cancels early: borrowed command state cannot retire before every receipt.
class runtime_control_completion final {
    struct state final {
        explicit state(worker_ref caller)
            : caller_(std::move(caller)) {}
        ~state() {
            if (failure_ && !observed_) {
                report_unhandled_failure("runtime control", failure_);
            }
        }
        worker_ref caller_;
        std::mutex mutex_;
        std::exception_ptr failure_;
        std::coroutine_handle<> continuation_{};
        std::size_t completed_{};
        bool released_{};
        bool observed_{};
    };

public:
    template <typename function_type>
    class command;
    class receipt final {
    public:
        void complete(std::exception_ptr failure = {}) const noexcept {
            std::lock_guard lock(state_->mutex_);
            ++state_->completed_;
            if (!state_->failure_) {
                state_->failure_ = std::move(failure);
            }
        }
        template <typename function_type>
        [[nodiscard]] command<std::decay_t<function_type>> bind(function_type function) && {
            return command<std::decay_t<function_type>>(std::move(*this), std::move(function));
        }

    private:
        receipt(std::shared_ptr<state> state_value, std::shared_ptr<void> lifetime) noexcept
            : state_(std::move(state_value)),
              lifetime_(std::move(lifetime)) {}
        std::shared_ptr<state> state_;
        std::shared_ptr<void> lifetime_;
        friend class runtime_control_completion;
    };

    // Capture lists do not specify closure member declaration order. Keep the
    // receipt in a named first member so command payload destruction completes
    // before its final receipt can resume another worker.
    template <typename function_type>
    class command final {
    public:
        command(receipt completion, function_type function)
            : completion_(std::move(completion)),
              function_(std::move(function)) {}
        command(const command&) = delete;
        command& operator=(const command&) = delete;
        command(command&&) = default;
        command& operator=(command&&) = delete;
        template <typename... args_type>
            requires std::invocable<function_type&, const receipt&, args_type...>
        decltype(auto) operator()(args_type&&... args) {
            return std::invoke(function_, std::as_const(completion_), std::forward<args_type>(args)...);
        }

    private:
        receipt completion_;
        function_type function_;
    };

    class wait_operation final {
    public:
        wait_operation(const wait_operation&) = delete;
        wait_operation& operator=(const wait_operation&) = delete;
        void operator&() const = delete;
        ~wait_operation() {
            if (started_ && !resumed_) {
                std::terminate();
            }
        }
        bool await_ready() {
            if (!state_->caller_.is_current()) {
                throw std::logic_error("runtime control completion must be awaited on its caller worker");
            }
            std::lock_guard lock(state_->mutex_);
            started_ = true;
            return state_->released_;
        }
        bool await_suspend(std::coroutine_handle<> continuation) {
            std::lock_guard lock(state_->mutex_);
            if (state_->released_) {
                return false;
            }
            if (state_->continuation_) {
                std::terminate();
            }
            state_->continuation_ = continuation;
            return true;
        }
        void await_resume() noexcept {
            resumed_ = true;
        }

    private:
        explicit wait_operation(std::shared_ptr<state> value) noexcept
            : state_(std::move(value)) {}
        std::shared_ptr<state> state_;
        bool started_{};
        bool resumed_{};
        friend class runtime_control_completion;
    };
    explicit runtime_control_completion(worker_ref caller)
        : state_(std::make_shared<state>(std::move(caller))),
          lifetime_(state_.get(), [value = state_](void*) noexcept {
              std::coroutine_handle<> continuation;
              {
                  std::lock_guard lock(value->mutex_);
                  value->released_ = true;
                  continuation = std::exchange(value->continuation_, {});
              }
              if (continuation) {
                  value->caller_.defer_resume(continuation);
              }
          }) {}
    runtime_control_completion(const runtime_control_completion&) = delete;
    runtime_control_completion& operator=(const runtime_control_completion&) = delete;
    [[nodiscard]] receipt make_receipt() const {
        if (!lifetime_) {
            throw std::logic_error("runtime control completion is already waiting");
        }
        return receipt(state_, lifetime_);
    }
    template <typename function_type>
    [[nodiscard]] command<std::decay_t<function_type>> make_command(function_type function) const {
        return make_receipt().bind(std::move(function));
    }
    [[nodiscard]] wait_operation wait() & {
        if (!state_->caller_.is_current()) {
            throw std::logic_error("runtime control must wait on its caller worker");
        }
        if (waiting_) {
            throw std::logic_error("runtime control completion can only wait once");
        }
        waiting_ = true;
        lifetime_.reset();
        return wait_operation(state_);
    }
    wait_operation wait() && = delete;
    [[nodiscard]] std::size_t completed() const noexcept {
        std::lock_guard lock(state_->mutex_);
        return state_->completed_;
    }
    [[nodiscard]] std::exception_ptr failure() const noexcept {
        std::lock_guard lock(state_->mutex_);
        state_->observed_ = true;
        return state_->failure_;
    }

private:
    std::shared_ptr<state> state_;
    std::shared_ptr<void> lifetime_;
    bool waiting_{};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Serializes admitted controls without holding a mutex across suspension.
// The owning state must outlive every acquire and granted lease.
class runtime_control_gate final {
public:
    class acquire_operation;
    class lease final {
    public:
        lease(const lease&) = delete;
        lease& operator=(const lease&) = delete;
        lease(lease&& other) noexcept
            : gate_(std::exchange(other.gate_, nullptr)) {}
        lease& operator=(lease&&) = delete;
        ~lease() {
            if (gate_) {
                gate_->release();
            }
        }

    private:
        explicit lease(runtime_control_gate& gate) noexcept
            : gate_(&gate) {}
        runtime_control_gate* gate_;
        friend class acquire_operation;
    };

    class acquire_operation final {
    public:
        acquire_operation(const acquire_operation&) = delete;
        acquire_operation& operator=(const acquire_operation&) = delete;
        void operator&() const = delete;
        ~acquire_operation() {
            if (started_ && !resumed_) {
                std::terminate();
            }
        }
        bool await_ready() {
            if (!caller_.is_current()) {
                throw std::logic_error("runtime control must be awaited on its caller worker");
            }
            std::lock_guard lock(gate_.mutex_);
            started_ = true;
            if (gate_.active_) {
                return false;
            }
            gate_.active_ = true;
            return true;
        }
        bool await_suspend(std::coroutine_handle<> continuation) {
            std::lock_guard lock(gate_.mutex_);
            if (!gate_.active_) {
                gate_.active_ = true;
                return false;
            }
            continuation_ = continuation;
            if (gate_.tail_) {
                gate_.tail_->next_ = this;
            } else {
                gate_.head_ = this;
            }
            gate_.tail_ = this;
            return true;
        }
        lease await_resume() noexcept {
            resumed_ = true;
            return lease(gate_);
        }

    private:
        acquire_operation(runtime_control_gate& gate, worker_ref caller) noexcept
            : gate_(gate),
              caller_(std::move(caller)) {}
        runtime_control_gate& gate_;
        worker_ref caller_;
        acquire_operation* next_{};
        std::coroutine_handle<> continuation_{};
        bool started_{};
        bool resumed_{};
        friend class runtime_control_gate;
    };

    runtime_control_gate() = default;
    runtime_control_gate(const runtime_control_gate&) = delete;
    runtime_control_gate& operator=(const runtime_control_gate&) = delete;
    ~runtime_control_gate() {
        if (active_ || head_) {
            std::terminate();
        }
    }
    [[nodiscard]] acquire_operation acquire(worker_ref caller) & noexcept {
        return acquire_operation(*this, std::move(caller));
    }
    acquire_operation acquire(worker_ref) && = delete;

private:
    void release() noexcept {
        worker_ref caller;
        std::coroutine_handle<> continuation;
        {
            std::lock_guard lock(mutex_);
            if (!head_) {
                active_ = false;
                return;
            }
            auto* next = head_;
            head_ = next->next_;
            if (!head_) {
                tail_ = nullptr;
            }
            next->next_ = nullptr;
            caller = next->caller_;
            continuation = std::exchange(next->continuation_, {});
        }
        caller.defer_resume(continuation);
    }
    std::mutex mutex_;
    acquire_operation* head_{};
    acquire_operation* tail_{};
    bool active_{};
};

}  // namespace ruvia::detail

// The connection-slot bookkeeping every pooled client shares: lease scheduling,
// stop-token cancellation addressed by operation id, and one deadline timer per
// slot. Only the error type and what an expiry or cancellation does to the
// driver's connection differ, so those are the policy.

namespace ruvia::detail {

enum class pool_slot_abort_reason : std::uint8_t {
    none,
    cancelled,
    closing,
};

// policy_type supplies:
//   using owner_type = ...;          // the driver pool that owns the connections
//   using deadline_kind_type = ...;  // scoped enum naming what a deadline guards
//   [[noreturn]] static void throw_acquire_failure(const owner_type&, worker_wait_status);
//   [[noreturn]] static void throw_aborted(const owner_type&, pool_slot_abort_reason);
//   static void expire_slot(owner_type&, std::size_t slot, deadline_kind_type) noexcept;
//   static void cancel_slot(owner_type&, std::size_t slot) noexcept;
//
// The pool is worker-affine and address-stable: slot state lives in one array
// sized at construction, and pending timers and cancellation registrations
// borrow it by address.
template <typename policy_type>
class slot_pool final {
public:
    using owner_type = typename policy_type::owner_type;
    using deadline_kind_type = typename policy_type::deadline_kind_type;
    using cancellation_target_type = worker_cancellation_target<slot_pool>;

    slot_pool(owner_type& owner_value, std::size_t slot_count, const worker_ref& worker_value)
        : owner_(owner_value),
          worker_(worker_value),
          slots_(slot_count),
          scheduler_(slot_count, worker_),
          cancellation_target_(make_worker_cancellation_target(*this, worker_)) {}

    slot_pool(owner_type&, std::size_t, worker_ref&&) = delete;

    ~slot_pool() {
        cancellation_target_->detach(*this);
    }

    slot_pool(const slot_pool&) = delete;
    slot_pool& operator=(const slot_pool&) = delete;
    slot_pool(slot_pool&&) = delete;
    slot_pool& operator=(slot_pool&&) = delete;

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

    class acquire_awaiter final {
    public:
        acquire_awaiter(owner_type& owner, pool_lease_acquire acquire) noexcept
            : owner_(owner),
              acquire_(std::move(acquire)) {}
        acquire_awaiter(const acquire_awaiter&) = delete;
        acquire_awaiter& operator=(const acquire_awaiter&) = delete;
        acquire_awaiter(acquire_awaiter&&) noexcept = default;
        acquire_awaiter& operator=(acquire_awaiter&&) = delete;
        void operator&() const = delete;

        [[nodiscard]] bool await_ready() {
            return acquire_.await_ready();
        }
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
            return acquire_.await_suspend(continuation);
        }
        [[nodiscard]] std::size_t await_resume() {
            const auto result_value = acquire_.await_resume();
            if (result_value.has_value()) {
                return result_value.value();
            }
            policy_type::throw_acquire_failure(owner_, result_value.status());
        }

    private:
        owner_type& owner_;
        pool_lease_acquire acquire_;
    };

    // The common free-slot path completes directly in the caller's frame.
    // Saturated pools use the same scheduler, timeout and cancellation path.
    [[nodiscard]] acquire_awaiter acquire(const operation_deadline& timeout,
        std::optional<timeout_duration> acquire_timeout, stop_token stop_token_value) {
        return acquire_awaiter(owner_, scheduler_.acquire({.timeout_ = timeout.constrained_by(acquire_timeout).timeout(),
                                           .stop_token_ = std::move(stop_token_value)}));
    }

    // Leases a free slot without waiting; nullopt while every slot is leased
    // or the pool is closing. A free slot is exactly what acquire() leases
    // without suspending.
    [[nodiscard]] std::optional<std::size_t> try_acquire() {
        auto acquire = scheduler_.acquire();
        if (!acquire.await_ready()) {
            return std::nullopt;
        }
        const auto result_value = acquire.await_resume();
        if (!result_value.has_value()) {
            return std::nullopt;
        }
        return result_value.value();
    }

    // A release that names no live lease is a caller bug and cannot be
    // reported through a noexcept path.
    void release(std::size_t slot) noexcept {
        scheduler_.release(slot);
    }

    // Refuses new leases and marks every slot without an abort reason as
    // closing; on true the owner must abort every connection, since a later
    // stop no longer reaches cancel_slot(). Returns false when the pool was
    // already closed; the owner then has nothing left to tear down.
    [[nodiscard]] bool close() noexcept {
        if (!scheduler_.close()) {
            return false;
        }
        for (auto& slot : slots_) {
            if (slot.abort_reason_ == pool_slot_abort_reason::none) {
                slot.abort_reason_ = pool_slot_abort_reason::closing;
            }
        }
        return true;
    }

    [[nodiscard]] bool closing() const noexcept {
        return scheduler_.closing();
    }

    // Starts an operation on a leased slot: clears a cancellation its previous
    // operation left, while closing stays permanent.
    void reset_abort(std::size_t slot) noexcept {
        slots_[slot].abort_reason_ = closing() ? pool_slot_abort_reason::closing : pool_slot_abort_reason::none;
    }

    // Releases a lease on scope exit, however the operation between acquire
    // and release ends.
    class lease_guard final {
    public:
        lease_guard(slot_pool& pool, std::size_t slot) noexcept
            : pool_(pool),
              slot_(slot) {}
        lease_guard(const lease_guard&) = delete;
        lease_guard& operator=(const lease_guard&) = delete;
        ~lease_guard() {
            pool_.release(slot_);
        }

    private:
        slot_pool& pool_;
        std::size_t slot_;
    };

    // Binds the operation's stop token to one slot for the duration of the
    // operation. A stop delivered on (or posted to) the worker reaches
    // cancel_operation_by_id(), which marks the slot cancelled and lets the
    // policy abort the driver connection. A cancellation belongs to the
    // operation it ended; closing is permanent, so an operation still holding
    // a lease after close() (an open stream or transaction) observes it, even
    // when its stop arrives later.
    class cancellation_guard final {
    public:
        cancellation_guard(slot_pool& pool, std::size_t slot, const stop_token& stop_token_value)
            : registration_(pool.cancellation_target_, pool.slots_[slot].cancellation_id_) {
            pool.reset_abort(slot);
            registration_.arm(stop_token_value);
        }
        cancellation_guard(const cancellation_guard&) = delete;
        cancellation_guard& operator=(const cancellation_guard&) = delete;
        ~cancellation_guard() {
            finish();
        }

        void finish() noexcept {
            registration_.reset();
        }

        // The operation's id; cancel_operation_by_id() with it acts as a stop.
        [[nodiscard]] std::uint64_t id() const noexcept {
            return registration_.id();
        }

    private:
        worker_cancellation_registration<cancellation_target_type> registration_;
    };

    // worker_cancellation_target hook; runs on the worker. A closing slot
    // keeps reporting closing: its owner aborts every connection right after
    // close(), so a later stop has nothing left to cancel.
    void cancel_operation_by_id(std::uint64_t cancellation_id) noexcept {
        if (cancellation_id == 0) {
            return;
        }
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            auto& slot = slots_[index];
            if (slot.cancellation_id_ != cancellation_id) {
                continue;
            }
            slot.cancellation_id_ = 0;
            if (slot.abort_reason_ != pool_slot_abort_reason::closing) {
                slot.abort_reason_ = pool_slot_abort_reason::cancelled;
                policy_type::cancel_slot(owner_, index);
            }
            return;
        }
    }

    [[nodiscard]] bool aborted(std::size_t slot) const noexcept {
        return slots_[slot].abort_reason_ != pool_slot_abort_reason::none;
    }

    void throw_if_aborted(std::size_t slot) const {
        const auto reason = slots_[slot].abort_reason_;
        if (reason != pool_slot_abort_reason::none) {
            policy_type::throw_aborted(owner_, reason);
        }
    }

    // Replaces the slot's absolute deadline. An absent deadline leaves the
    // slot unarmed; an expired one returns false so the caller reports its own
    // timeout. A scheduling failure rolls the slot back before propagating.
    [[nodiscard]] bool arm_deadline(
        std::size_t slot, std::optional<std::chrono::steady_clock::time_point> deadline, deadline_kind_type kind) {
        auto& state = slots_[slot];
        (void)clear_deadline(slot);
        if (!deadline.has_value()) {
            return true;
        }
        if (*deadline <= std::chrono::steady_clock::now()) {
            return false;
        }
        const auto deadline_value = *deadline;
        state.deadline_.arm(deadline_value, kind);
        try {
            (void)worker_.schedule_timer(state.deadline_timer_, deadline_value,
                [this, slot](worker_timer_outcome outcome) noexcept {
                    if (outcome != worker_timer_outcome::expired) {
                        return;
                    }
                    const auto expired = slots_[slot].deadline_.expire(std::chrono::steady_clock::now());
                    if (expired.has_value()) {
                        policy_type::expire_slot(owner_, slot, *expired);
                    }
                });
        } catch (...) {
            state.deadline_timer_.cancel();
            state.deadline_.reset();
            throw;
        }
        return true;
    }

    // Disarms the slot's deadline; true when it had already expired.
    [[nodiscard]] bool clear_deadline(std::size_t slot) noexcept {
        auto& state = slots_[slot];
        state.deadline_timer_.cancel();
        return state.deadline_.clear();
    }

    // Expires an armed deadline whose time has passed without waiting for the
    // timer; true when the deadline is (now) expired.
    [[nodiscard]] bool poll_deadline(std::size_t slot) noexcept {
        auto& deadline = slots_[slot].deadline_;
        return deadline.expire(std::chrono::steady_clock::now()).has_value() || deadline.expired();
    }

    [[nodiscard]] const deadline_kind_type* deadline_kind(std::size_t slot) const noexcept {
        return slots_[slot].deadline_.kind();
    }

private:
    struct slot_state final {
        std::uint64_t cancellation_id_{0};
        pool_slot_abort_reason abort_reason_{pool_slot_abort_reason::none};
        deadline_lifecycle<deadline_kind_type> deadline_;
        worker_timer_registration deadline_timer_;
    };

    // Fixed-size, never relocated: timer registrations are neither movable
    // nor copyable, and pending timers address their slot by index.
    class slot_array final {
    public:
        explicit slot_array(std::size_t slot_count)
            : data_(std::make_unique<slot_state[]>(slot_count)),
              size_(slot_count) {}
        slot_array(const slot_array&) = delete;
        slot_array& operator=(const slot_array&) = delete;

        [[nodiscard]] std::size_t size() const noexcept {
            return size_;
        }
        [[nodiscard]] slot_state& operator[](std::size_t index) noexcept {
            return data_[index];
        }
        [[nodiscard]] const slot_state& operator[](std::size_t index) const noexcept {
            return data_[index];
        }
        [[nodiscard]] slot_state* begin() noexcept {
            return data_.get();
        }
        [[nodiscard]] slot_state* end() noexcept {
            return data_.get() + size_;
        }

    private:
        std::unique_ptr<slot_state[]> data_;
        std::size_t size_;
    };

    owner_type& owner_;
    const worker_ref& worker_;
    slot_array slots_;
    pool_lease_scheduler scheduler_;
    std::shared_ptr<cancellation_target_type> cancellation_target_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class operation_interrupt;

// Routes stop requests to the running operations of one I/O owner on its
// worker. The owner keeps it at a stable address until those operations have
// completed. The first stoppable operation creates the shared target; it is
// detached on destruction, so a stop delivered later reaches no owner.
class operation_interrupt_source final {
public:
    operation_interrupt_source() noexcept = default;
    operation_interrupt_source(const operation_interrupt_source&) = delete;
    operation_interrupt_source& operator=(const operation_interrupt_source&) = delete;
    ~operation_interrupt_source() {
        if (target_) {
            target_->detach(*this);
        }
    }

    void cancel_operation_by_id(std::uint64_t id) noexcept;

private:
    friend class operation_interrupt;

    std::shared_ptr<worker_cancellation_target<operation_interrupt_source>> target_;
    operation_interrupt* running_{};
};

enum class operation_interrupt_cause : std::uint8_t { cancelled,
    timed_out };

// The deadline and stop token of one running operation. Both are delivered on
// the operation's worker, where the first records its cause and calls
// `interrupt(target)` once: a read wakes its wait and keeps buffered input, an
// output operation aborts its stream. A stop requested before construction
// throws operation_canceled before anything is armed, so the operation has no
// effect. Construct it before the operation's first wait and keep it until
// that operation completes.
class operation_interrupt final {
public:
    using interrupt_type = void (*)(void*) noexcept;

    operation_interrupt(operation_interrupt_source& source, const worker_ref& worker_value,
        const operation_options& options, void* target, interrupt_type interrupt, const char* operation_name)
        : source_(source),
          target_(target),
          interrupt_(interrupt),
          operation_name_(operation_name) {
        if (options.stop_token_.stop_requested()) {
            throw_cause(operation_interrupt_cause::cancelled, operation_name_);
        }
        if (options.stop_token_.stoppable() && !source_.target_) {
            source_.target_ = make_worker_cancellation_target(source_, worker_value);
        }
        if (options.timeout_.has_value()) {
            (void)worker_value.schedule_timer(timer_, options.timeout_->deadline_from(std::chrono::steady_clock::now()),
                [this](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        fire(operation_interrupt_cause::timed_out);
                    }
                });
        }
        // Linked before arming: a stop requested meanwhile is delivered inline.
        next_ = source_.running_;
        source_.running_ = this;
        if (options.stop_token_.stoppable()) {
            try {
                cancellation_.emplace(source_.target_, id_);
                cancellation_->arm(options.stop_token_);
            } catch (...) {
                cancellation_.reset();
                unlink();
                throw;
            }
        }
    }

    operation_interrupt(const operation_interrupt&) = delete;
    operation_interrupt& operator=(const operation_interrupt&) = delete;

    ~operation_interrupt() {
        // Joins a stop callback before this operation leaves the source.
        cancellation_.reset();
        unlink();
    }

    [[nodiscard]] bool interrupted() const noexcept {
        return cause_.has_value();
    }

    [[nodiscard]] std::optional<operation_interrupt_cause> cause() const noexcept {
        return cause_;
    }

    // Throws std::system_error (timed_out or operation_canceled) once interrupted.
    void throw_if_interrupted() const {
        if (cause_.has_value()) {
            throw_cause(*cause_, operation_name_);
        }
    }

    [[noreturn]] static void throw_cause(operation_interrupt_cause cause, const char* operation_name) {
        throw std::system_error(std::make_error_code(cause == operation_interrupt_cause::timed_out
                                                         ? std::errc::timed_out
                                                         : std::errc::operation_canceled),
            operation_name);
    }

private:
    friend class operation_interrupt_source;

    void unlink() noexcept {
        for (auto** link = &source_.running_; *link != nullptr; link = &(*link)->next_) {
            if (*link == this) {
                *link = next_;
                return;
            }
        }
    }

    void fire(operation_interrupt_cause cause) noexcept {
        if (cause_.has_value()) {
            return;
        }
        cause_ = cause;
        interrupt_(target_);
    }

    operation_interrupt_source& source_;
    void* target_;
    interrupt_type interrupt_;
    const char* operation_name_;
    std::optional<operation_interrupt_cause> cause_;
    operation_interrupt* next_{};
    std::uint64_t id_{};
    worker_timer_registration timer_;
    // Declared last: its reset joins a pending stop callback first.
    std::optional<worker_cancellation_registration<worker_cancellation_target<operation_interrupt_source>>> cancellation_;
};

inline void operation_interrupt_source::cancel_operation_by_id(std::uint64_t id) noexcept {
    for (auto* running = running_; running != nullptr; running = running->next_) {
        if (id != 0 && running->id_ == id) {
            running->fire(operation_interrupt_cause::cancelled);
            return;
        }
    }
}

// One operation made of several interruptible steps: each step runs with the
// time left and the same stop token, and a step's interruption is recognized
// as this operation's own.
class operation_steps final {
public:
    explicit operation_steps(operation_options operation) noexcept
        : deadline_(operation.timeout_),
          stop_(std::move(operation.stop_token_)) {}

    // The time left and the same stop token for the next step; std::nullopt
    // once the deadline has passed.
    [[nodiscard]] std::optional<operation_options> try_next() const {
        if (deadline_.expired()) {
            return std::nullopt;
        }
        return operation_options{.timeout_ = deadline_.timeout(), .stop_token_ = stop_};
    }

    // Throws std::system_error (timed_out) once the deadline has passed.
    [[nodiscard]] operation_options next(const char* operation_name) const {
        auto step = try_next();
        if (!step.has_value()) {
            operation_interrupt::throw_cause(operation_interrupt_cause::timed_out, operation_name);
        }
        return std::move(*step);
    }

    // Whether this operation's own deadline or stop token accounts for `cause`.
    [[nodiscard]] bool interrupted_by(operation_interrupt_cause cause) const noexcept {
        return cause == operation_interrupt_cause::timed_out ? deadline_.expired() : stop_.stop_requested();
    }

    // Whether `error` is a step interrupted by this operation's deadline or stop token.
    [[nodiscard]] bool interrupted(const std::system_error& error) const noexcept {
        return (error.code() == std::errc::timed_out && interrupted_by(operation_interrupt_cause::timed_out)) ||
               (error.code() == std::errc::operation_canceled && interrupted_by(operation_interrupt_cause::cancelled));
    }

private:
    operation_deadline deadline_;
    stop_token stop_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// A configured host whose port is a separate field: brackets and a lone colon
// (host:port) are rejected; an unbracketed IPv6 literal has several colons.
inline void ensure_config_host(std::string_view host, const char* empty_message, const char* invalid_message) {
    if (host.empty()) {
        throw std::invalid_argument(empty_message);
    }
    std::size_t colon_count = 0;
    for (const auto ch : host) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte <= 0x20 || byte == 0x7F || byte == '/' || byte == '\\' || byte == '[' || byte == ']') {
            throw std::invalid_argument(invalid_message);
        }
        colon_count += byte == ':' ? 1 : 0;
    }
    if (colon_count == 1) {
        throw std::invalid_argument(invalid_message);
    }
}

inline void ensure_positive_size(std::size_t value, const char* message) {
    if (value == 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensure_positive_optional_size(
    const std::optional<std::size_t>& value, const char* message) {
    if (value.has_value() && *value == 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensure_non_zero_port(std::uint16_t port, const char* message) {
    if (port == 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensure_non_zero_optional_port(
    const std::optional<port_number>& port, const char* message) {
    if (port.has_value()) {
        ensure_non_zero_port(port->value(), message);
    }
}

template <typename rep_type, typename period_type>
void ensure_positive_duration(std::chrono::duration<rep_type, period_type> value, const char* message) {
    if (value.count() <= 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensure_positive_duration(timeout_duration value, const char* message) {
    ensure_positive_duration(value.value(), message);
}

template <typename rep_type, typename period_type>
void ensure_positive_optional_duration(
    const std::optional<std::chrono::duration<rep_type, period_type>>& value, const char* message) {
    if (value.has_value() && value->count() <= 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensure_positive_optional_duration(
    const std::optional<timeout_duration>& value, const char* message) {
    if (value.has_value()) {
        ensure_positive_duration(*value, message);
    }
}

template <typename first_duration_type, typename... rest_durations_type>
void ensure_positive_optional_durations(
    const char* message, const first_duration_type& first, const rest_durations_type&... rest) {
    ensure_positive_optional_duration(first, message);
    (ensure_positive_optional_duration(rest, message), ...);
}

}  // namespace ruvia::detail

namespace ruvia::detail {

class connection_lifecycle;

// Monotonic milliseconds shared by worker-local deadlines (connection scanner,
// WebSocket liveness, tunnel drain). Values are comparable only with each
// other, never with wall-clock time.
[[nodiscard]] inline std::int64_t steady_now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Worker-owned connection activity tracking options, copied from the server
// configuration after validate_server_limits has checked every duration.
struct connection_scanner_options final {
    timeout_duration scan_interval_{std::chrono::seconds(1)};
    // Inactivity timeouts measured from the connection's last successful I/O
    // (idle_timeout_ also bounds phase::awaiting_stream, measured from entering
    // it). Absence disables the corresponding phase timeout.
    std::optional<timeout_duration> idle_timeout_{};
    std::optional<timeout_duration> initial_read_timeout_{};
    std::optional<timeout_duration> payload_read_timeout_{};
    std::optional<timeout_duration> write_timeout_{};
    // Absolute phase deadlines, independent of successful I/O progress.
    std::optional<timeout_duration> initial_read_completion_timeout_{};
    std::optional<timeout_duration> payload_read_completion_timeout_{};
};

// Product limit: a connection that a timeout asked to end through its drain
// callback (an HTTP/2 GOAWAY drain, bounded by its own round-trip limit) has
// this long to do so before the scanner closes its socket, whatever its
// activity in the meantime.
inline constexpr std::chrono::milliseconds connection_close_grace{std::chrono::seconds(2)};

// What the scanner acts on when it ends a connection. close() ends it at once
// (timeout past its grace, eviction, worker stop); cancel() aborts its pending
// read so the owner can end it gracefully. A TCP session binds its socket; a
// QUIC connection, which shares its worker's UDP socket, binds itself.
class connection_transport final {
public:
    using action_type = void (*)(void*) noexcept;

    constexpr connection_transport() noexcept = default;
    constexpr connection_transport(void* target, action_type close_action, action_type cancel_action) noexcept
        : target_(target),
          close_(close_action),
          cancel_(cancel_action) {}

    [[nodiscard]] static connection_transport tcp(asio::ip::tcp::socket& socket) noexcept;
    [[nodiscard]] bool valid() const noexcept {
        return target_ != nullptr;
    }

    void close() const noexcept {
        close_(target_);
    }
    void cancel() const noexcept {
        cancel_(target_);
    }

private:
    void* target_{nullptr};
    action_type close_{nullptr};
    action_type cancel_{nullptr};
};

// Worker-affine connection timeout owner. Its implementation is allocated once
// per scanner; entries and checks remain caller-owned intrusive nodes, with no
// per-connection allocation by the scanner.
class connection_scanner final {
private:
    struct impl_type;

public:
    // Product-owned callbacks. A tick runs on the worker during each scan; a
    // drain callback runs on the worker when a connection timeout asks the
    // connection to end gracefully (HTTP/2 or HTTP/3 GOAWAY).
    using periodic_check_type = void (*)(void*, std::int64_t) noexcept;
    using drain_check_type = void (*)(void*) noexcept;

    enum class phase : std::uint8_t {
        idle,
        // A multiplexed session (HTTP/2) with no open stream: idle_timeout runs
        // from entering the phase, so frames that open no stream (PING,
        // SETTINGS, WINDOW_UPDATE) and this side's writes do not renew it.
        awaiting_stream,
        reading_initial,
        reading_payload,
        long_lived,
        writing,
    };

    class entry;

    // Per-connection node; callback registration is allocation-free.
    // The entry detaches any remaining nodes when it is destroyed.
    class periodic_check_registration final {
    public:
        periodic_check_registration() noexcept = default;
        ~periodic_check_registration() noexcept;
        periodic_check_registration(const periodic_check_registration&) = delete;
        periodic_check_registration& operator=(const periodic_check_registration&) = delete;
        periodic_check_registration(periodic_check_registration&&) = delete;
        periodic_check_registration& operator=(periodic_check_registration&&) = delete;
        void reset() noexcept;

    private:
        friend class connection_scanner;
        friend class entry;
        entry* entry_{nullptr};
        periodic_check_registration* prev_{nullptr};
        periodic_check_registration* next_{nullptr};
        void* target_{nullptr};
        periodic_check_type tick_{nullptr};
        drain_check_type drain_{nullptr};
    };

    // Connection-owned node. touch()/set_phase() sample monotonic activity time;
    // the scanner samples its comparison clock once per tick. Delayed scans
    // never backdate activity or shorten configured timeouts.
    //
    // Writes have their own stall clock, independent of the phase that times
    // reads (downstream write_timeout bounds every write, as in Pingora): it
    // runs while the entry is in the `writing` phase or a write_scope is open,
    // and only write progress renews it. write_timeout closes a connection
    // whose clock lapses, whatever its phase, so a full-duplex reader (HTTP/2)
    // that keeps the entry idle, reading or long_lived cannot hide a peer that
    // stopped reading.
    // Destruction detaches the entry and its checks, including during a periodic
    // callback. Destroying the scanner detaches its entries and checks.
    class entry final {
    public:
        entry() noexcept = default;
        ~entry() noexcept;
        entry(const entry&) = delete;
        entry& operator=(const entry&) = delete;
        entry(entry&&) = delete;
        entry& operator=(entry&&) = delete;
        void bind_lifecycle(connection_lifecycle* lifecycle) noexcept {
            lifecycle_ = lifecycle;
        }
        [[nodiscard]] connection_lifecycle* lifecycle() const noexcept {
            return lifecycle_;
        }
        void touch() noexcept;
        // Records transport progress of a write: activity that also renews
        // the write stall clock.
        void note_write_progress() noexcept;
        // Not for long_lived: an upgrade enters that phase via enter_long_lived().
        // Ends a request-head read started by read_request_head().
        void set_phase(phase next_phase) noexcept;
        // Reads the head of the connection's next request, timed by
        // `timeout_phase`. No request is admitted until a later set_phase();
        // touch() keeps the read open. A timeout cancels the read and sets
        // close_requested().
        void read_request_head(phase timeout_phase) noexcept;
        // The scanner asked the connection to end gracefully: it cancelled a
        // request-head read, or invoked a drain callback for a timeout -- or
        // it already closed the socket to evict the connection. The owner
        // closes without waiting for further input. Activity no longer
        // extends the connection: the scanner closes its socket once the
        // request's grace passes -- one scan interval for a cancelled read
        // (covering a cancel the transport lost), connection_close_grace for
        // a drain callback.
        [[nodiscard]] bool close_requested() const noexcept {
            return close_requested_;
        }

        // Hands the entry back, when it ends, to the phase it had when it
        // began: same phase, same phase start and request-head state, with
        // the time of the end as activity. A write nested in another phase
        // (held output flushed in the middle of a handler) thus leaves that
        // phase's deadlines, and drain's view of the request, as they were.
        class phase_scope final {
        public:
            explicit phase_scope(entry& entry_value) noexcept
                : entry_(entry_value),
                  phase_started_ms_(entry_value.phase_started_ms_),
                  phase_(entry_value.phase_),
                  reading_request_head_(entry_value.reading_request_head_) {}
            phase_scope(entry& entry_value, phase next_phase) noexcept
                : phase_scope(entry_value) {
                entry_.set_phase(next_phase);
            }
            ~phase_scope();
            phase_scope(const phase_scope&) = delete;
            phase_scope& operator=(const phase_scope&) = delete;

        private:
            entry& entry_;
            std::int64_t phase_started_ms_;
            phase phase_;
            bool reading_request_head_;
        };

        // Arms the write stall clock for one write without changing the
        // phase, for a writer that runs beside the connection's reader. The
        // clock starts when the outermost scope opens.
        class write_scope final {
        public:
            explicit write_scope(entry& entry_value) noexcept;
            ~write_scope();
            write_scope(const write_scope&) = delete;
            write_scope& operator=(const write_scope&) = delete;

        private:
            entry& entry_;
        };

        // Moves an established upgrade into the untimed long_lived phase.
        void enter_long_lived() noexcept;
        [[nodiscard]] std::int64_t last_active_ms() const noexcept;
        // `drain` is optional: a connection whose read or idle phase times
        // out is asked once through it to end gracefully, instead of being
        // closed.
        void register_periodic_check(periodic_check_registration& registration, void* target,
            periodic_check_type tick, drain_check_type drain = nullptr) noexcept;

    private:
        friend class connection_scanner;
        friend class periodic_check_registration;
        friend struct impl_type;
        [[nodiscard]] bool linked() const noexcept;
        // A write is under way: the write stall clock runs, and a timeout
        // cannot end the connection gracefully.
        [[nodiscard]] bool write_pending() const noexcept {
            return phase_ == phase::writing || write_scopes_ != 0;
        }
        [[nodiscard]] bool has_drain_check() const noexcept;
        void notify_drain() noexcept;
        void remove_periodic_check(periodic_check_registration& registration) noexcept;
        void detach_periodic_checks() noexcept;
        // Between requests with nothing in flight: an HTTP/1 keep-alive
        // connection awaiting its next request head, or a multiplexed session
        // without a stream. Such a connection may give up its slot.
        [[nodiscard]] bool reusable() const noexcept {
            return !close_requested_ &&
                   ((reading_request_head_ && phase_ == phase::idle) || phase_ == phase::awaiting_stream);
        }
        // Links the entry at the newest end of the scanner's reusable list
        // when it became reusable, unlinks it when it stopped being so.
        void sync_reusable() noexcept;
        void unlink_reusable() noexcept;
        connection_transport transport_{};
        connection_lifecycle* lifecycle_{};
        connection_scanner* scanner_{nullptr};
        entry* prev_{nullptr};
        entry* next_{nullptr};
        std::int64_t last_active_ms_{0};
        std::int64_t phase_started_ms_{0};
        // Last write progress, or the start of the write; the write stall
        // clock's reference while write_pending().
        std::int64_t write_progress_ms_{0};
        // Valid while close_requested_: when the scanner closes the socket.
        std::int64_t close_deadline_ms_{0};
        phase phase_{phase::idle};
        bool reading_request_head_{false};
        bool close_requested_{false};
        std::uint32_t write_scopes_{0};
        periodic_check_registration* periodic_checks_{nullptr};
        periodic_check_registration* periodic_scan_next_{nullptr};
        // Links in the scanner's reusable list, oldest first; null outside it.
        entry* reusable_prev_{nullptr};
        entry* reusable_next_{nullptr};
    };

    // Binds a connection's transport to an entry for the guard lifetime; its
    // destructor safely detaches even if the scanner was destroyed first. An
    // entry has one registration owner: binding an already-bound entry throws
    // std::logic_error without changing its existing binding or checks.
    class guard final {
    public:
        guard(connection_scanner& scanner, entry& entry_value, connection_transport transport_value);
        ~guard();
        guard(const guard&) = delete;
        guard& operator=(const guard&) = delete;

    private:
        entry& entry_;
    };
    connection_scanner(worker_ref worker_value, connection_scanner_options options);
    // Detaches every entry and check. A periodic check may destroy its scanner
    // (or stop and restart it): the running scan visits no further node and
    // releases the remaining scanner state when it returns.
    // Destruction on another thread waits for an in-progress scan to finish.
    ~connection_scanner() noexcept;
    connection_scanner(const connection_scanner&) = delete;
    connection_scanner& operator=(const connection_scanner&) = delete;

    void start();
    void stop() noexcept;
    // Closes every connection at once (a worker stop): there is no drain.
    void close_all() noexcept;
    // Admission at the connection cap, as nginx reuses idle keep-alive
    // connections when worker_connections run out: closes the connection
    // that has been reusable longest (see entry::reusable(); one with a write
    // still under way is skipped) and returns whether one was found. Its
    // socket closes now; its slot is released once its session unwinds.
    [[nodiscard]] bool evict_oldest_reusable() noexcept;

private:
    friend class entry;
    friend class guard;
    friend struct impl_type;
    static void detach_entry(entry& entry) noexcept;
    std::unique_ptr<impl_type> impl_;
};

// asio::async_write completion condition that records each partial transfer on
// the connection's scanner entry, so write_timeout measures inactivity rather
// than the time to drain the whole write.
class connection_write_progress final {
public:
    explicit connection_write_progress(connection_scanner::entry& entry) noexcept
        : entry_(&entry) {}

    [[nodiscard]] std::size_t operator()(const std::error_code& error, std::size_t bytes_transferred) const noexcept {
        if (bytes_transferred != 0) {
            entry_->note_write_progress();
        }
        return asio::transfer_all()(error, bytes_transferred);
    }

private:
    connection_scanner::entry* entry_;
};

struct connection_scanner::impl_type final {
    explicit impl_type(connection_scanner* owner_value, worker_ref worker_value, connection_scanner_options options);

    // Detaches every entry and registration and disarms the timer. Returns true
    // when called from inside this scanner's own scan callback: that scan still
    // runs on this stack and takes ownership of the impl, freeing it on return.
    [[nodiscard]] bool retire() noexcept;

    void start();
    void stop() noexcept;
    void register_entry(entry& entry, connection_transport transport_value);
    void unregister_entry(entry& entry) noexcept;
    void close_all() noexcept;
    // Arms the next scan. Scanning parks (no timer armed) once no entry is
    // tracked and the quiet period's trim has run; the next registration
    // re-arms it, so an idle worker holds no scanner timer work.
    void schedule();
    // Visits every entry; returns whether none was in a request exchange or
    // active since `interval_start`.
    [[nodiscard]] bool scan(std::int64_t interval_start) noexcept;
    [[nodiscard]] bool is_timed_out(const entry& entry, std::int64_t now) const noexcept;
    // A write is pending and has made no progress for write_timeout.
    [[nodiscard]] bool write_stalled(const entry& entry, std::int64_t now) const noexcept;
    [[nodiscard]] static bool active_since(const entry& entry, std::int64_t interval_start) noexcept;
    // Ends a timed-out connection: the first time gracefully when its owner
    // can (a pending request-head read, or a drain callback outside writing),
    // otherwise -- including once a graceful request's grace has passed -- by
    // closing its socket.
    void expire(entry& entry) noexcept;
    // Cancels the entry's pending read and closes its socket at `deadline_ms`
    // unless the owner ended the connection first.
    static void request_close(entry& entry, std::int64_t deadline_ms) noexcept;
    static void record_close_request(entry& entry, std::int64_t deadline_ms) noexcept;
    struct timer_state_type final {
        explicit timer_state_type(impl_type* owner_value) noexcept
            : owner_(owner_value) {}
        std::mutex mutex_;
        impl_type* owner_;
    };

    connection_scanner* owner_;
    worker_ref worker_;
    worker_timer_registration timer_;
    std::shared_ptr<timer_state_type> timer_state_;
    connection_scanner_options options_;
    std::int64_t cached_now_ms_{0};
    // Latest activity of an entry that has since been unregistered.
    std::int64_t retired_activity_ms_{(std::numeric_limits<std::int64_t>::min)()};
    entry sentinel_{};
    // Entries for which entry::reusable() holds, oldest first.
    entry reusable_sentinel_{};
    entry* scan_current_{nullptr};
    entry* scan_next_{nullptr};
    std::atomic_bool running_{false};
    // Worker-local: set only by the timer callback while scan() runs callbacks.
    bool scanning_{false};
    bool release_after_scan_{false};
    // Worker-local: the current quiet period has been trimmed.
    bool quiet_trimmed_{false};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class inbound_buffer_limit_error final : public std::bad_alloc {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "inbound buffer memory budget exhausted";
    }
};

class inbound_buffer_account;
class inbound_buffer_reservation;

// A worker's live inbound buffer bytes, shared by the inbound_buffer_account of
// each of its connections. Worker-affine, no synchronization. This owner must
// outlive every connection account.
class inbound_buffer_budget final {
public:
    explicit inbound_buffer_budget(std::size_t limit)
        : limit_(limit) {
        if (limit_ == 0) {
            throw std::invalid_argument("inbound buffer budget requires a positive limit");
        }
    }
    ~inbound_buffer_budget() {
        if (used_ != 0) {
            std::terminate();
        }
    }
    inbound_buffer_budget(const inbound_buffer_budget&) = delete;
    inbound_buffer_budget& operator=(const inbound_buffer_budget&) = delete;

private:
    friend class inbound_buffer_account;
    const std::size_t limit_;
    std::size_t used_{};
};

// One connection's inbound buffers. Each reservation is checked against the
// connection's limit and its worker's budget, both charged together. Every
// reservation must be returned before this owner is destroyed.
class inbound_buffer_account final {
public:
    inbound_buffer_account(inbound_buffer_budget& worker, std::size_t limit)
        : worker_(worker),
          limit_(limit) {
        if (limit_ == 0) {
            throw std::invalid_argument("inbound buffer resource requires a positive limit");
        }
    }
    ~inbound_buffer_account() {
        if (used_ != 0) {
            std::terminate();
        }
    }
    inbound_buffer_account(const inbound_buffer_account&) = delete;
    inbound_buffer_account& operator=(const inbound_buffer_account&) = delete;

    [[nodiscard]] inbound_buffer_reservation reserve(std::size_t bytes_value);

private:
    friend class inbound_buffer_reservation;

    void charge_bytes(std::size_t bytes_value) {
        if (bytes_value > limit_ - used_ || bytes_value > worker_.limit_ - worker_.used_) {
            throw inbound_buffer_limit_error();
        }
        used_ += bytes_value;
        worker_.used_ += bytes_value;
    }

    void release_bytes(std::size_t bytes_value) noexcept {
        if (bytes_value > used_ || bytes_value > worker_.used_) {
            std::terminate();
        }
        used_ -= bytes_value;
        worker_.used_ -= bytes_value;
    }

    inbound_buffer_budget& worker_;
    const std::size_t limit_;
    std::size_t used_{};
};

// Move-only RAII reservation of live inbound buffer capacity against an
// inbound_buffer_account (no-op when unbound).
class inbound_buffer_reservation final {
public:
    constexpr inbound_buffer_reservation() noexcept = default;
    explicit constexpr inbound_buffer_reservation(inbound_buffer_account* account) noexcept
        : account_(account) {}
    inbound_buffer_reservation(inbound_buffer_account& account, std::size_t bytes_value)
        : account_(&account) {
        if (bytes_value != 0) {
            account_->charge_bytes(bytes_value);
            bytes_ = bytes_value;
        }
    }
    ~inbound_buffer_reservation() {
        release();
    }

    inbound_buffer_reservation(const inbound_buffer_reservation&) = delete;
    inbound_buffer_reservation& operator=(const inbound_buffer_reservation&) = delete;

    inbound_buffer_reservation(inbound_buffer_reservation&& other) noexcept
        : account_(std::exchange(other.account_, nullptr)),
          bytes_(std::exchange(other.bytes_, 0)) {}

    inbound_buffer_reservation& operator=(inbound_buffer_reservation&& other) noexcept {
        if (this != &other) {
            release();
            account_ = std::exchange(other.account_, nullptr);
            bytes_ = std::exchange(other.bytes_, 0);
        }
        return *this;
    }

    [[nodiscard]] constexpr inbound_buffer_account* account() const noexcept {
        return account_;
    }

    [[nodiscard]] constexpr std::size_t bytes() const noexcept {
        return bytes_;
    }

    void resize_to(std::size_t target_bytes) {
        if (account_ == nullptr || target_bytes == bytes_) {
            return;
        }
        if (target_bytes > bytes_) {
            account_->charge_bytes(target_bytes - bytes_);
        } else {
            account_->release_bytes(bytes_ - target_bytes);
        }
        bytes_ = target_bytes;
    }

    void shrink_to(std::size_t target_bytes) noexcept {
        if (account_ != nullptr && target_bytes < bytes_) {
            account_->release_bytes(bytes_ - target_bytes);
            bytes_ = target_bytes;
        }
    }

    void clear() noexcept {
        if (account_ != nullptr && bytes_ != 0) {
            account_->release_bytes(bytes_);
            bytes_ = 0;
        }
    }

    void release() noexcept {
        clear();
        account_ = nullptr;
    }

    void swap(inbound_buffer_reservation& other) noexcept {
        std::swap(account_, other.account_);
        std::swap(bytes_, other.bytes_);
    }

private:
    inbound_buffer_account* account_{nullptr};
    std::size_t bytes_{0};
};

inline inbound_buffer_reservation inbound_buffer_account::reserve(std::size_t bytes_value) {
    return inbound_buffer_reservation(*this, bytes_value);
}

[[nodiscard]] inline std::size_t inbound_string_heap_bytes(const std::string& value) noexcept {
    static const std::size_t sso_capacity = std::string{}.capacity();
    return value.capacity() > sso_capacity ? value.capacity() : 0;
}

inline void inbound_reserve(
    std::string& target, inbound_buffer_reservation& reservation, std::size_t capacity) {
    if (capacity <= target.capacity()) {
        return;
    }
    static const std::size_t sso_capacity = std::string{}.capacity();
    if (capacity > sso_capacity) {
        reservation.resize_to(capacity);
    }
    try {
        target.reserve(capacity);
    } catch (...) {
        reservation.shrink_to(inbound_string_heap_bytes(target));
        throw;
    }
    reservation.resize_to(inbound_string_heap_bytes(target));
}

inline void inbound_resize(
    std::string& target, inbound_buffer_reservation& reservation, std::size_t new_size) {
    if (new_size > target.capacity()) {
        static const std::size_t sso_capacity = std::string{}.capacity();
        if (new_size > sso_capacity) {
            reservation.resize_to(new_size);
        }
        try {
            target.resize(new_size);
        } catch (...) {
            reservation.shrink_to(inbound_string_heap_bytes(target));
            throw;
        }
        reservation.resize_to(inbound_string_heap_bytes(target));
    } else {
        target.resize(new_size);
        reservation.shrink_to(inbound_string_heap_bytes(target));
    }
}

inline void inbound_append(
    std::string& target, inbound_buffer_reservation& reservation, std::string_view data) {
    if (data.empty()) {
        return;
    }
    if (data.size() > target.max_size() - target.size()) {
        throw std::length_error("inbound buffer length overflow");
    }
    const auto needed = target.size() + data.size();
    if (needed > target.capacity()) {
        const auto doubled =
            target.capacity() > target.max_size() / 2 ? target.max_size() : target.capacity() * 2;
        inbound_reserve(target, reservation, (std::max)(needed, doubled));
    }
    target.append(data.data(), data.size());
    reservation.resize_to(inbound_string_heap_bytes(target));
}

inline void inbound_clear_retaining_small(std::string& target,
    inbound_buffer_reservation& reservation,
    std::size_t retained_bytes = http_retained_string_bytes) noexcept {
    http_clear_string_retaining_small(target, retained_bytes);
    reservation.shrink_to(inbound_string_heap_bytes(target));
}

inline void inbound_discard(std::string& target, inbound_buffer_reservation& reservation) noexcept {
    std::string{}.swap(target);
    reservation.clear();
}

[[nodiscard]] inline std::span<char> inbound_prepare(
    http_io_buffer& buffer, inbound_buffer_reservation& reservation, std::size_t bytes) {
    const auto spare = buffer.prepare_spare();
    if (spare.size() >= bytes) {
        return spare.first(bytes);
    }
    constexpr std::size_t max_bytes =
        static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)());
    if (bytes > max_bytes - buffer.size()) {
        throw std::length_error("http_io_buffer size exceeds the addressable range");
    }
    const auto doubled = buffer.capacity() > max_bytes / 2 ? max_bytes : buffer.capacity() * 2;
    const auto next_capacity = (std::max)(buffer.size() + bytes, doubled);
    if (next_capacity > buffer.capacity()) {
        reservation.resize_to(next_capacity);
    }
    try {
        const auto prepared = buffer.prepare(bytes);
        reservation.resize_to(buffer.capacity());
        return prepared;
    } catch (...) {
        reservation.shrink_to(buffer.capacity());
        throw;
    }
}

inline void inbound_reserve(
    http_io_buffer& buffer, inbound_buffer_reservation& reservation, std::size_t capacity) {
    if (capacity <= buffer.capacity()) {
        return;
    }
    reservation.resize_to(capacity);
    try {
        buffer.reserve(capacity);
    } catch (...) {
        reservation.shrink_to(buffer.capacity());
        throw;
    }
    reservation.resize_to(buffer.capacity());
}

inline void inbound_append(
    http_io_buffer& buffer, inbound_buffer_reservation& reservation, std::string_view bytes) {
    if (bytes.empty()) {
        return;
    }
    const auto spare = buffer.prepare_spare();
    if (spare.size() < bytes.size()) {
        constexpr std::size_t max_bytes =
            static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)());
        if (bytes.size() > max_bytes - buffer.size()) {
            throw std::length_error("http_io_buffer size exceeds the addressable range");
        }
        const auto doubled = buffer.capacity() > max_bytes / 2 ? max_bytes : buffer.capacity() * 2;
        const auto next_capacity = (std::max)(buffer.size() + bytes.size(), doubled);
        if (next_capacity > buffer.capacity()) {
            reservation.resize_to(next_capacity);
        }
    }
    try {
        buffer.append(bytes);
        reservation.resize_to(buffer.capacity());
    } catch (...) {
        reservation.shrink_to(buffer.capacity());
        throw;
    }
}

inline void inbound_shrink_to(
    http_io_buffer& buffer, inbound_buffer_reservation& reservation, std::size_t bytes) {
    buffer.shrink_to(bytes);
    reservation.shrink_to(buffer.capacity());
}

}  // namespace ruvia::detail

namespace ruvia::detail {

inline constexpr std::size_t http_body_buffer_bytes = std::size_t{8} * 1024;
// The smallest capacity a buffered known-length body grows to while it is read
// straight into its own storage. It matches the WebSocket read window: a peer
// that sends only headers can make the declared length allocate at most this
// much before any body bytes arrive; beyond it capacity doubles from arrivals.
inline constexpr std::size_t known_length_min_read_bytes = std::size_t{64} * 1024;

}  // namespace ruvia::detail

namespace ruvia::detail {

// http_error_info borrows its fields. The caller's http_protocol_error lvalue
// must outlive any suspension of the error handler that reads this view.
[[nodiscard]] inline http_error_info copy_http_protocol_error_info(
    const http_protocol_error& error RUVIA_LIFETIMEBOUND) noexcept {
    return http_error_info({.status_ = error.status(),
        .code_ = error.code(),
        .message_ = std::string_view(error.what())});
}
http_error_info copy_http_protocol_error_info(const http_protocol_error&&) = delete;

}  // namespace ruvia::detail

namespace ruvia::detail {

enum class http_stream_end : std::uint8_t { keep_open,
    end };

// Whether a read error marks the end of input rather than a read failure: a
// TCP FIN or TLS close_notify (eof), or a TLS peer that closed TCP without
// close_notify (stream_truncated). Message-delimited protocols above the
// stream still detect a truncated message themselves; the transport only
// reports that no more bytes follow.
[[nodiscard]] inline bool is_stream_read_end(const std::error_code& error) noexcept {
    return error == asio::error::eof || error == asio::ssl::error::stream_truncated;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// A failed I/O operation on the inbound connection's own transport: the socket
// or TLS stream this server accepted. The type carries the failure's
// provenance, so connection_failure_sink can tell a connection that ended (the
// peer reset it, or this server cancelled its I/O) from a handler or upstream
// failure that happens to carry the same error code. Handlers still observe it
// as a std::system_error.
class inbound_transport_error final : public std::system_error {
public:
    inbound_transport_error(std::error_code code, const char* what_arg)
        : std::system_error(code, what_arg) {}
};

// Errors with which an inbound connection ends without any fault on this side:
// the peer closed it (including a TLS peer that closed TCP without
// close_notify), reset or aborted it, the network gave up on a vanished peer,
// or this server ended the I/O itself. Cancellation may carry Asio's native
// operation_aborted code or the generic operation_canceled condition used by
// the TLS output pump; bad_descriptor is I/O started after our own close
// (stop, drain, or a timeout). Only this server closes an inbound socket, so
// these codes cannot come from anything else on the inbound transport.
[[nodiscard]] inline bool is_inbound_connection_end(const std::error_code& error) noexcept {
    return is_stream_read_end(error) || error == asio::error::connection_reset ||
           error == asio::error::connection_aborted || error == asio::error::broken_pipe ||
           error == asio::error::operation_aborted || error == std::errc::operation_canceled ||
           error == asio::error::bad_descriptor || error == asio::error::timed_out ||
           error == asio::error::not_connected ||
           error == asio::error::shut_down;
}

}  // namespace ruvia::detail

// Native file-open primitive for the web-layer server drivers. This performs real
// OS file I/O (::open / ::CreateFileW with an owning fd/HANDLE), so it belongs in
// ruvia-web, NOT in the pure sans-I/O ruvia-http protocol library. ruvia-http only
// owns the http_response_file_view DESCRIPTOR (path + size/offset) used to frame
// Content-Length/Range, plus an opaque http_response_file_descriptor token whose
// release function is supplied here; opening, reading and closing the file are
// runtime driver concerns.

namespace ruvia::detail {

struct response_file_snapshot final {
    http_response_file_identity identity_{http_response_file_identity::unchecked()};
    std::uint64_t size_{0};
    std::uint64_t modified_token_{0};
    std::time_t modified_seconds_{0};
};

// One open handle plus the metadata read from it. context::file frames the
// response from this snapshot and hands the same handle to the transport.
struct opened_response_file final {
    http_response_file_descriptor handle_;
    response_file_snapshot snapshot_;
};

inline constexpr std::uint64_t windows_to_unix_epoch100ns = UINT64_C(116444736000000000);
inline constexpr std::uint64_t windows_file_time_ticks_per_second = UINT64_C(10000000);

// FILETIME is 100ns ticks from 1601-01-01. Floor fractional seconds, including
// times before 1970; do not collapse them to the Unix epoch.
[[nodiscard]] inline std::time_t unix_seconds_from_windows_file_time_ticks(std::uint64_t ticks) noexcept {
    if (ticks < windows_to_unix_epoch100ns) {
        const auto before_epoch = windows_to_unix_epoch100ns - ticks;
        return -static_cast<std::time_t>(before_epoch / windows_file_time_ticks_per_second) -
               static_cast<std::time_t>(before_epoch % windows_file_time_ticks_per_second != 0);
    }
    return static_cast<std::time_t>(
        (ticks - windows_to_unix_epoch100ns) / windows_file_time_ticks_per_second);
}

#if defined(__unix__)
using native_file_type = int;

class native_file_descriptor final {
public:
    explicit native_file_descriptor(int fd = -1) noexcept
        : fd_(fd) {}

    ~native_file_descriptor() {
        reset();
    }

    native_file_descriptor(const native_file_descriptor&) = delete;
    native_file_descriptor& operator=(const native_file_descriptor&) = delete;

    native_file_descriptor(native_file_descriptor&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}

    native_file_descriptor& operator=(native_file_descriptor&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return fd_ >= 0;
    }

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    [[nodiscard]] int release() noexcept {
        return std::exchange(fd_, -1);
    }

    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = -1;
    }

private:
    int fd_;
};

[[nodiscard]] inline std::error_code last_native_file_error() noexcept {
    return std::error_code(errno, std::system_category());
}

[[nodiscard]] inline response_file_snapshot snapshot_native_file_handle(
    int fd, std::error_code& ec) noexcept {
    struct stat status{};
    if (::fstat(fd, &status) != 0) {
        ec = last_native_file_error();
        return {};
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        ec = std::make_error_code(std::errc::not_supported);
        return {};
    }
    const auto modified_seconds = status.st_mtim.tv_sec;
    const auto modified_nanoseconds = status.st_mtim.tv_nsec;
    const auto changed_seconds = status.st_ctim.tv_sec;
    const auto changed_nanoseconds = status.st_ctim.tv_nsec;
    const std::array<std::uint64_t, 4> words{static_cast<std::uint64_t>(status.st_dev),
        static_cast<std::uint64_t>(status.st_ino), static_cast<std::uint64_t>(changed_seconds),
        static_cast<std::uint64_t>(changed_nanoseconds)};
    ec = {};
    return response_file_snapshot{http_response_file_identity::checked(words),
        static_cast<std::uint64_t>(status.st_size),
        static_cast<std::uint64_t>(modified_seconds) * UINT64_C(1000000000) +
            static_cast<std::uint64_t>(modified_nanoseconds),
        static_cast<std::time_t>(modified_seconds)};
}

// O_NONBLOCK keeps a FIFO (or another special file swapped in for a regular
// one) from blocking the worker in open() before the same-handle fstat rejects
// it; it has no effect on reads from a regular file.
inline constexpr int native_file_open_flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;

// The one open of a file by path; every response, index and read handle
// starts here.
[[nodiscard]] inline native_file_descriptor open_native_file(
    const http_native_path_char_type* path, std::error_code& ec) noexcept {
    native_file_descriptor input(::open(path, native_file_open_flags));
    ec = input ? std::error_code{} : last_native_file_error();
    return input;
}

inline void release_response_file_handle(http_response_file_descriptor::native_type value) noexcept {
    ::close(static_cast<int>(value));
}

[[nodiscard]] inline int borrowed_native_file(const http_response_file_descriptor& handle) noexcept {
    return static_cast<int>(handle.native());
}

[[nodiscard]] inline http_response_file_descriptor adopt_response_file_handle(native_file_descriptor input) noexcept {
    return http_response_file_descriptor(
        static_cast<http_response_file_descriptor::native_type>(input.release()), &release_response_file_handle);
}

// An independent handle to the same open file, so a read state that outlives
// the response (an abandoned pool read) never borrows the response's handle.
// Reads are positional, so the shared file offset is never relied on.
[[nodiscard]] inline native_file_descriptor duplicate_native_file(
    const http_response_file_descriptor& handle, std::error_code& ec) noexcept {
    native_file_descriptor duplicate(::fcntl(borrowed_native_file(handle), F_DUPFD_CLOEXEC, 0));
    ec = duplicate ? std::error_code{} : last_native_file_error();
    return duplicate;
}

#if defined(O_PATH)
inline constexpr int native_directory_open_flags = O_PATH | O_DIRECTORY | O_CLOEXEC;
#else
inline constexpr int native_directory_open_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
#endif

// A directory handle that later opens resolve entries against, so a request
// walks only the path below it instead of the whole absolute path.
[[nodiscard]] inline native_file_descriptor open_native_directory(
    const http_native_path_char_type* path, std::error_code& ec) noexcept {
    native_file_descriptor directory(::open(path, native_directory_open_flags));
    ec = directory ? std::error_code{} : last_native_file_error();
    return directory;
}

// The filesystem instance of a directory: device and inode.
struct native_directory_identity final {
    std::uint64_t device_{0};
    std::uint64_t inode_{0};

    friend bool operator==(const native_directory_identity&, const native_directory_identity&) = default;
};

[[nodiscard]] inline native_directory_identity native_directory_identity_of(
    const struct stat& status, std::error_code& ec) noexcept {
    if (!S_ISDIR(status.st_mode)) {
        ec = std::make_error_code(std::errc::not_a_directory);
        return {};
    }
    ec = {};
    return {static_cast<std::uint64_t>(status.st_dev), static_cast<std::uint64_t>(status.st_ino)};
}

[[nodiscard]] inline native_directory_identity native_directory_handle_identity(
    int directory, std::error_code& ec) noexcept {
    struct stat status{};
    if (::fstat(directory, &status) != 0) {
        ec = last_native_file_error();
        return {};
    }
    return native_directory_identity_of(status, ec);
}

[[nodiscard]] inline native_directory_identity native_directory_path_identity(
    const http_native_path_char_type* path, std::error_code& ec) noexcept {
    struct stat status{};
    if (::stat(path, &status) != 0) {
        ec = last_native_file_error();
        return {};
    }
    return native_directory_identity_of(status, ec);
}

// Opens `relative` ('/'-separated, as an index records it) below `directory`
// one component at a time, refusing a symlink, "." or ".." in any component.
[[nodiscard]] inline native_file_descriptor open_native_file_beneath_by_components(
    int directory, std::string_view relative, std::error_code& ec) noexcept {
    // NAME_MAX of every supported filesystem; a longer component cannot exist.
    constexpr std::size_t max_component_bytes = 255;
    std::array<char, max_component_bytes + 1> component{};
    native_file_descriptor parent;
    for (;;) {
        const auto separator = relative.find('/');
        const auto name = relative.substr(0, separator);
        if (name.empty() || name == "." || name == "..") {
            ec = std::make_error_code(std::errc::permission_denied);
            return native_file_descriptor();
        }
        if (name.size() > max_component_bytes) {
            ec = std::make_error_code(std::errc::filename_too_long);
            return native_file_descriptor();
        }
        std::memcpy(component.data(), name.data(), name.size());
        component[name.size()] = '\0';
        const int at = parent ? parent.get() : directory;
        if (separator == std::string_view::npos) {
            native_file_descriptor input(::openat(at, component.data(), native_file_open_flags | O_NOFOLLOW));
            ec = input ? std::error_code{} : last_native_file_error();
            return input;
        }
        // O_DIRECTORY | O_NOFOLLOW fails on a symlink rather than opening it.
        native_file_descriptor next(::openat(at, component.data(), native_directory_open_flags | O_NOFOLLOW));
        if (!next) {
            ec = last_native_file_error();
            return native_file_descriptor();
        }
        parent = std::move(next);
        relative.remove_prefix(separator + 1);
    }
}

#if defined(__linux__) && defined(SYS_openat2)
// struct open_how and its resolve flags from the kernel ABI
// (include/uapi/linux/openat2.h), declared here so the build needs only the
// syscall number from the libc headers.
struct native_open_how final {
    std::uint64_t flags_;
    std::uint64_t mode_;
    std::uint64_t resolve_;
};
inline constexpr std::uint64_t native_resolve_no_magiclinks = 0x02;
inline constexpr std::uint64_t native_resolve_no_symlinks = 0x04;
inline constexpr std::uint64_t native_resolve_beneath = 0x08;

// Cleared once the kernel reports openat2 missing; the component walk then
// serves every later open in the process.
inline std::atomic<bool> native_openat2_available{true};
#endif

// Opens `relative` below `directory` without following a symlink in any
// component and without leaving `directory`: an index never records a
// symlink, so a path that now resolves through one was swapped after it was
// indexed and is refused. The handle is not validated here: the caller
// snapshots it (index) or checks it against the indexed identity (response).
[[nodiscard]] inline native_file_descriptor open_native_file_beneath(
    int directory, const http_native_path_char_type* relative, std::error_code& ec) noexcept {
#if defined(__linux__) && defined(SYS_openat2)
    if (native_openat2_available.load(std::memory_order_relaxed)) {
        const native_open_how how{.flags_ = static_cast<std::uint64_t>(native_file_open_flags | O_NOFOLLOW),
            .mode_ = 0,
            .resolve_ = native_resolve_beneath | native_resolve_no_symlinks | native_resolve_no_magiclinks};
        const auto opened = ::syscall(SYS_openat2, directory, relative, &how, sizeof(how));
        if (opened >= 0) {
            ec = {};
            return native_file_descriptor(static_cast<int>(opened));
        }
        // ENOSYS: kernel before 5.6; EPERM: a seccomp filter that refuses the
        // syscall. The component walk enforces the same rule.
        if (errno == ENOSYS) {
            native_openat2_available.store(false, std::memory_order_relaxed);
        } else if (errno != EPERM) {
            ec = last_native_file_error();
            return native_file_descriptor();
        }
    }
#endif
    return open_native_file_beneath_by_components(directory, relative, ec);
}

// Reads exactly `size` bytes at `offset` without moving the descriptor's file
// position, so a handle shared by several ranges needs no seek. A file that
// ends early is shorter than the snapshot it was framed from: it changed.
[[nodiscard]] inline std::error_code read_native_file_at(
    int fd, std::uint64_t offset, char* output, std::size_t size) noexcept {
    while (size != 0) {
        const auto count = ::pread(fd, output, size, static_cast<off_t>(offset));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return last_native_file_error();
        }
        if (count == 0) {
            return std::make_error_code(std::errc::state_not_recoverable);
        }
        output += count;
        offset += static_cast<std::uint64_t>(count);
        size -= static_cast<std::size_t>(count);
    }
    return {};
}

#if defined(__linux__)
// Reads only bytes already in the page cache (RWF_NOWAIT), never waiting for
// the disk; 0 when none are. A kernel without RWF_NOWAIT clears `supported`.
[[nodiscard]] inline std::size_t read_native_file_cached(
    int fd, std::uint64_t offset, char* output, std::size_t size, bool& supported) noexcept {
#if defined(SYS_preadv2) && defined(RWF_NOWAIT)
    if (!supported || offset > static_cast<std::uint64_t>((std::numeric_limits<off_t>::max)())) {
        return 0;
    }
    iovec buffer{output, size};
    // The kernel ABI splits the offset into unsigned longs; on 64-bit ABIs the
    // high word is zero.
    const auto low = static_cast<unsigned long>(offset);
    unsigned long high = 0;
    if constexpr (sizeof(unsigned long) < sizeof(offset)) {
        high = static_cast<unsigned long>(offset >> 32);
    }
    const auto count = ::syscall(SYS_preadv2, fd, &buffer, 1UL, low, high, RWF_NOWAIT);
    if (count < 0) {
        if (errno == ENOSYS || errno == EOPNOTSUPP || errno == EINVAL || errno == EPERM) {
            supported = false;
        }
        return 0;
    }
    return static_cast<std::size_t>(count);
#else
    static_cast<void>(fd);
    static_cast<void>(offset);
    static_cast<void>(output);
    static_cast<void>(size);
    supported = false;
    return 0;
#endif
}
#endif
#else
using native_file_type = HANDLE;

class native_file_descriptor final {
public:
    explicit native_file_descriptor(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        : handle_(handle) {}

    ~native_file_descriptor() {
        reset();
    }

    native_file_descriptor(const native_file_descriptor&) = delete;
    native_file_descriptor& operator=(const native_file_descriptor&) = delete;

    native_file_descriptor(native_file_descriptor&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}

    native_file_descriptor& operator=(native_file_descriptor&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    [[nodiscard]] HANDLE release() noexcept {
        return std::exchange(handle_, INVALID_HANDLE_VALUE);
    }

    void reset() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
        handle_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_;
};

[[nodiscard]] inline std::error_code last_native_file_error() noexcept {
    return std::error_code(static_cast<int>(::GetLastError()), std::system_category());
}

[[nodiscard]] inline std::uint64_t windows_file_time_token(LARGE_INTEGER value) noexcept {
    return static_cast<std::uint64_t>(value.QuadPart);
}

[[nodiscard]] inline std::time_t windows_file_time_seconds(LARGE_INTEGER value) noexcept {
    return unix_seconds_from_windows_file_time_ticks(windows_file_time_token(value));
}

[[nodiscard]] inline response_file_snapshot snapshot_native_file_handle(
    HANDLE handle, std::error_code& ec) noexcept {
    FILE_ID_INFO id{};
    FILE_STANDARD_INFO standard{};
    FILE_BASIC_INFO basic{};
    if (::GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) == 0 ||
        ::GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) ==
            0 ||
        ::GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) == 0) {
        ec = last_native_file_error();
        return {};
    }
    if (standard.Directory != FALSE || standard.EndOfFile.QuadPart < 0) {
        ec = std::make_error_code(std::errc::not_supported);
        return {};
    }
    std::uint64_t file_id_low = 0;
    std::uint64_t file_id_high = 0;
    static_assert(sizeof(id.FileId.Identifier) == 16);
    std::memcpy(&file_id_low, id.FileId.Identifier, sizeof(file_id_low));
    std::memcpy(&file_id_high, id.FileId.Identifier + sizeof(file_id_low), sizeof(file_id_high));
    const std::array<std::uint64_t, 4> words{static_cast<std::uint64_t>(id.VolumeSerialNumber),
        file_id_low, file_id_high, windows_file_time_token(basic.ChangeTime)};
    ec = {};
    return response_file_snapshot{http_response_file_identity::checked(words),
        static_cast<std::uint64_t>(standard.EndOfFile.QuadPart),
        windows_file_time_token(basic.LastWriteTime), windows_file_time_seconds(basic.LastWriteTime)};
}

// FILE_SHARE_DELETE lets a served file be renamed or replaced while a handle
// to it stays open, as POSIX allows.
inline constexpr DWORD native_file_share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
inline constexpr DWORD native_file_open_flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN;

// The one open of a file by path; every response, index and read handle
// starts here.
[[nodiscard]] inline native_file_descriptor open_native_file(
    const http_native_path_char_type* path, std::error_code& ec) noexcept {
    native_file_descriptor input(::CreateFileW(
        path, GENERIC_READ, native_file_share_mode, nullptr, OPEN_EXISTING, native_file_open_flags, nullptr));
    ec = input ? std::error_code{} : last_native_file_error();
    return input;
}

inline void release_response_file_handle(http_response_file_descriptor::native_type value) noexcept {
    ::CloseHandle(reinterpret_cast<HANDLE>(value));
}

[[nodiscard]] inline HANDLE borrowed_native_file(const http_response_file_descriptor& handle) noexcept {
    return reinterpret_cast<HANDLE>(handle.native());
}

[[nodiscard]] inline http_response_file_descriptor adopt_response_file_handle(native_file_descriptor input) noexcept {
    return http_response_file_descriptor(
        reinterpret_cast<http_response_file_descriptor::native_type>(input.release()), &release_response_file_handle);
}

// An independent handle to the same open file, so a read state that outlives
// the response (an abandoned pool read) never borrows the response's handle.
// Reads carry their offset, so the shared file pointer is never relied on.
[[nodiscard]] inline native_file_descriptor duplicate_native_file(
    const http_response_file_descriptor& handle, std::error_code& ec) noexcept {
    HANDLE duplicate = nullptr;
    if (::DuplicateHandle(::GetCurrentProcess(), borrowed_native_file(handle), ::GetCurrentProcess(), &duplicate, 0,
            FALSE, DUPLICATE_SAME_ACCESS) == 0) {
        ec = last_native_file_error();
        return native_file_descriptor();
    }
    ec = {};
    return native_file_descriptor(duplicate);
}

// Reads exactly `size` bytes at `offset`. A synchronous handle honours the
// OVERLAPPED offset and completes inline, so ranges sharing a handle need no
// seek. A file that ends early is shorter than the snapshot it was framed
// from: it changed.
[[nodiscard]] inline std::error_code read_native_file_at(
    HANDLE handle, std::uint64_t offset, char* output, std::size_t size) noexcept {
    while (size != 0) {
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset & UINT64_C(0xffffffff));
        position.OffsetHigh = static_cast<DWORD>(offset >> 32);
        const auto requested = static_cast<DWORD>(
            std::min<std::size_t>(size, static_cast<std::size_t>(UINT32_C(0x7fffffff))));
        DWORD count = 0;
        if (::ReadFile(handle, output, requested, &count, &position) == 0) {
            const auto error = ::GetLastError();
            return error == ERROR_HANDLE_EOF ? std::make_error_code(std::errc::state_not_recoverable)
                                             : std::error_code(static_cast<int>(error), std::system_category());
        }
        if (count == 0) {
            return std::make_error_code(std::errc::state_not_recoverable);
        }
        output += count;
        offset += count;
        size -= count;
    }
    return {};
}
#endif

// Opens `path` once and snapshots the handle it opened.
[[nodiscard]] inline opened_response_file open_response_file(
    const http_native_path_char_type* path, std::error_code& ec) noexcept {
    auto input = open_native_file(path, ec);
    if (ec) {
        return {};
    }
    const auto snapshot = snapshot_native_file_handle(input.get(), ec);
    if (ec) {
        return {};
    }
    return {adopt_response_file_handle(std::move(input)), snapshot};
}

// Whether `input` still holds the representation framed with `identity` and
// `size`. A file rewritten in place keeps its handle valid, so a handle opened
// at framing must be checked again before its bytes are sent under the framed
// validators; state_not_recoverable when it no longer matches.
[[nodiscard]] inline std::error_code validate_native_file(
    native_file_type input, http_response_file_identity identity, std::uint64_t size) noexcept {
    if (!identity.requires_validation()) {
        return {};
    }
    std::error_code error;
    const auto snapshot = snapshot_native_file_handle(input, error);
    if (error) {
        return error;
    }
    if (snapshot.identity_ != identity || snapshot.size_ != size) {
        return std::make_error_code(std::errc::state_not_recoverable);
    }
    return {};
}

[[nodiscard]] inline std::error_code validate_native_file(
    native_file_type input, http_response_file_view file) noexcept {
    return validate_native_file(input, file.identity(), file.size());
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// Owner-thread phase of one server worker:
// - running: admits connections and requests;
// - stopped: the core runtime stopped; every connection is closed now.
enum class http_server_worker_state : std::uint8_t {
    fresh,
    running,
    stopped,
};

// True only while the worker admits new connections and requests.
[[nodiscard]] inline bool http_server_worker_running(http_server_worker_state state_value) noexcept {
    return state_value == http_server_worker_state::running;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// One worker's admitted-connection gauge. Only the owning worker counts; any
// thread reads it relaxed (stats(), and the Windows acceptor choosing the
// least-loaded worker).
class connection_count final {
public:
    explicit connection_count(connection_count* parent = nullptr) noexcept
        : parent_(parent) {}
    connection_count(const connection_count&) = delete;
    connection_count& operator=(const connection_count&) = delete;

    [[nodiscard]] std::size_t active() const noexcept {
        return active_.load(std::memory_order_relaxed);
    }

    // Worker thread: one admitted connection.
    void acquire() noexcept {
        active_.fetch_add(1, std::memory_order_relaxed);
        if (parent_) {
            parent_->acquire();
        }
    }

    // Worker thread: one closed connection.
    void release() noexcept {
        const auto previous = active_.fetch_sub(1, std::memory_order_relaxed);
        if (previous == 0) {
            std::terminate();
        }
        if (parent_) {
            parent_->release();
        }
        if (previous == 1) {
            std::vector<std::weak_ptr<wait_state>> ready;
            {
                std::lock_guard lock(mutex_);
                ready.swap(waiters_);
            }
            for (auto& weak : ready) {
                if (auto state = weak.lock()) {
                    state->closed_.store(true, std::memory_order_release);
                    detail::worker_ref_access::defer_or_terminate(state->worker_, [state] { state->changed_.notify(); });
                }
            }
        }
    }

    // The caller retains the gauge; registrations never borrow its coroutine.
    static task<void> wait(std::shared_ptr<connection_count> count, worker_ref caller, operation_options options, stop_token host_stop) {
        if (!caller.is_current()) {
            throw std::logic_error("connection wait must run on its caller worker");
        }
        stop_group stops(options.stop_token_, host_stop);
        options.stop_token_ = stops.token();
        auto state = std::make_shared<wait_state>(caller);
        operation_interrupt_source source;
        operation_interrupt interrupt(source, caller, options, state.get(), [](void* target) noexcept { static_cast<wait_state*>(target)->changed_.notify(); }, "wait_connections_closed");
        {
            std::lock_guard lock(count->mutex_);
            if (count->active() != 0) {
                count->waiters_.push_back(state);
            } else {
                state->closed_.store(true, std::memory_order_relaxed);
            }
        }
        struct subscription final {
            connection_count& count_;
            wait_state* state_;
            ~subscription() {
                std::lock_guard lock(count_.mutex_);
                std::erase_if(count_.waiters_, [this](const auto& weak) {
                    const auto state = weak.lock();
                    return !state || state.get() == state_;
                });
            }
        } registration{*count, state.get()};
        interrupt.throw_if_interrupted();
        while (!state->closed_.load(std::memory_order_acquire)) {
            co_await state->changed_.wait();
            interrupt.throw_if_interrupted();
        }
        interrupt.throw_if_interrupted();
    }

private:
    std::atomic<std::size_t> active_{};
    struct wait_state final {
        explicit wait_state(worker_ref worker)
            : worker_(std::move(worker)),
              changed_(worker_) {}
        worker_ref worker_;
        worker_signal changed_;
        std::atomic<bool> closed_{false};
    };
    connection_count* parent_;
    std::mutex mutex_;
    std::vector<std::weak_ptr<wait_state>> waiters_;
};

// One admitted connection's place in its worker's connection_count, as a
// linear value. Every transport's connection owns exactly one: a TCP session
// through accepted_connection_lease, a QUIC connection directly.
class connection_slot final {
public:
    explicit connection_slot(connection_count& count) noexcept
        : count_(&count) {
        count_->acquire();
    }

    connection_slot(const connection_slot&) = delete;
    connection_slot& operator=(const connection_slot&) = delete;

    connection_slot(connection_slot&& other) noexcept
        : count_(std::exchange(other.count_, nullptr)) {}

    connection_slot& operator=(connection_slot&&) = delete;

    ~connection_slot() {
        if (count_ != nullptr) {
            count_->release();
        }
    }

private:
    connection_count* count_;
};

// Merge endpoint gauges across workers and successive listener instances.
inline void append_listener_stats(http_server_stats& total, const http_server_stats& worker) {
    for (const auto& listener : worker.listeners_) {
        auto found = std::find_if(total.listeners_.begin(), total.listeners_.end(), [&](const auto& existing) {
            return existing.address_ == listener.address_ && existing.port_ == listener.port_;
        });
        if (found == total.listeners_.end()) {
            total.listeners_.push_back(listener);
        } else {
            found->active_connections_ += listener.active_connections_;
        }
    }
}

// Process-wide monotonically increasing IDs cannot alias across workers,
// hosts, or restarts. Exhaustion terminates rather than recycling a stale ID.
class connection_id_sequence final {
public:
    connection_id_sequence() noexcept = default;

    connection_id_sequence(const connection_id_sequence&) = delete;
    connection_id_sequence& operator=(const connection_id_sequence&) = delete;

    [[nodiscard]] std::uint64_t next() noexcept {
        const auto id = next_.fetch_add(1, std::memory_order_relaxed);
        if (id == 0) {
            std::terminate();
        }
        return id;
    }

private:
    inline static std::atomic<std::uint64_t> next_{1};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

[[nodiscard]] inline ip_address to_ip_address(const asio::ip::address& address) noexcept {
    if (address.is_v4()) {
        return ip_address::ipv4(address.to_v4().to_bytes());
    }
    const auto v6 = address.to_v6();
    return ip_address::ipv6(v6.to_bytes(), static_cast<std::uint32_t>(v6.scope_id()));
}

[[nodiscard]] inline asio::ip::address to_asio_address(const ip_address& address) noexcept {
    if (address.family() == ip_address_family::ipv4) {
        asio::ip::address_v4::bytes_type bytes{};
        std::copy(address.bytes().begin(), address.bytes().end(), bytes.begin());
        return asio::ip::address_v4(bytes);
    }
    asio::ip::address_v6::bytes_type bytes{};
    std::copy(address.bytes().begin(), address.bytes().end(), bytes.begin());
    return asio::ip::address_v6(bytes, address.scope_id());
}

// The peer address a connection's services name, shared by TCP and QUIC
// sessions: dotted IPv4, also for an IPv4-mapped IPv6 peer, otherwise the
// RFC 5952 IPv6 text.
inline void assign_peer_address(std::string& output, const asio::ip::address& address) {
    asio::ip::address_v4 v4;
    if (address.is_v4()) {
        v4 = address.to_v4();
    } else {
        const auto v6 = address.to_v6();
        if (!v6.is_v4_mapped()) {
            const auto bytes = v6.to_bytes();
            std::array<char, 64> ipv6_buffer;
            asio::error_code ec;
            const char* formatted = asio::detail::socket_ops::inet_ntop(
                ASIO_OS_DEF(AF_INET6), bytes.data(), ipv6_buffer.data(),
                ipv6_buffer.size(), v6.scope_id(), ec);
            if (formatted == nullptr) {
                asio::detail::throw_error(ec);
            }
            output.assign(formatted, std::strlen(formatted));
            return;
        }
        v4 = asio::ip::make_address_v4(asio::ip::v4_mapped, v6);
    }
    const auto v4_bytes = v4.to_bytes();
    std::array<char, 15> buffer;
    const auto formatted = format_ipv4_octets(v4_bytes.data(), buffer);
    output.assign(formatted.data(), formatted.size());
}

}  // namespace ruvia::detail

namespace ruvia::detail {

enum class accept_error_action : std::uint8_t { retry,
    report_and_retry,
    fail };

// Peer disappearance and readiness races are normal accept outcomes. Resource
// and network failures leave the listener usable but must remain observable.
[[nodiscard]] inline accept_error_action classify_accept_error(const std::error_code& error) noexcept {
    if (error == asio::error::connection_aborted || error == asio::error::connection_reset ||
        error == asio::error::eof || error == asio::error::try_again ||
        error == asio::error::would_block || error == asio::error::interrupted
#if defined(_WIN32)
        || (error.category() == asio::system_category() && error.value() == ERROR_NETNAME_DELETED)
#endif
    ) {
        return accept_error_action::retry;
    }
    if (error == asio::error::no_descriptors || error == asio::error::no_buffer_space ||
        error == asio::error::no_memory || error == std::errc::too_many_files_open_in_system ||
        error == std::errc::protocol_error || error == asio::error::network_down ||
        error == asio::error::network_unreachable || error == asio::error::host_unreachable ||
        error == asio::error::no_protocol_option || error == asio::error::operation_not_supported ||
        error == asio::error::no_permission || error == asio::error::timed_out) {
        return accept_error_action::report_and_retry;
    }
#if defined(EHOSTDOWN) && defined(ENONET)
    if (error.category() == asio::system_category() && (error.value() == EHOSTDOWN || error.value() == ENONET)) {
        return accept_error_action::report_and_retry;
    }
#endif
    return accept_error_action::fail;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

inline void validate_client_tcp_config(const client_tcp_config& config) {
    switch (config.no_delay_) {
        case tcp_no_delay_policy::system_default:
        case tcp_no_delay_policy::enable:
            break;
        default:
            throw std::invalid_argument("TCP no-delay policy is invalid");
    }
    switch (config.keep_alive_) {
        case tcp_keep_alive_policy::system_default:
        case tcp_keep_alive_policy::enable:
            break;
        default:
            throw std::invalid_argument("TCP keepalive policy is invalid");
    }
}

// Returns the error of the first enabled policy that cannot be applied.
[[nodiscard]] inline std::error_code apply_tcp_socket_policies(
    asio::ip::tcp::socket& socket, const client_tcp_config& config) noexcept {
    std::error_code error;
    if (config.no_delay_ == tcp_no_delay_policy::enable) {
        socket.set_option(asio::ip::tcp::no_delay(true), error);
        if (error) {
            return error;
        }
    }
    if (config.keep_alive_ == tcp_keep_alive_policy::enable) {
        socket.set_option(asio::socket_base::keep_alive(true), error);
    }
    return error;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

class connection_lifecycle;

// Intrusive, worker-affine registry. No lock is held while invoking user code
// or releasing state. IDs are retired before close callbacks can reenter.
class connection_registry final {
public:
    [[nodiscard]] bool close_connection(std::uint64_t id) noexcept;
    void close_listener(const void* listener) noexcept;

private:
    friend class connection_lifecycle;
    connection_lifecycle* head_{};
};

class connection_lifecycle final {
public:
    connection_lifecycle(connection_registry& registry, const void* listener, connection_event event,
        connection_open_callback_ref opened, connection_close_callback_ref closed, std::uint32_t interface_index = 0,
        connection_transport transport = {})
        : registry_(&registry),
          listener_(listener),
          event_(std::move(event)),
          closed_(closed),
          transport_(transport),
          bound_(transport.valid()),
          interface_index_(interface_index) {
        next_ = registry.head_;
        if (next_ != nullptr) {
            next_->prev_ = this;
        }
        registry.head_ = this;
        format_local();
        try {
            if (opened) {
                state_ = opened(event_);
            }
        } catch (...) {
            if (prev_ != nullptr) {
                prev_->next_ = next_;
            } else {
                registry_->head_ = next_;
            }
            if (next_ != nullptr) {
                next_->prev_ = prev_;
            }
            registry_ = nullptr;
            if (bound_) {
                transport_.close();
            }
            info_.reason_ = connection_close_reason::handler_error;
            if (closed_) {
                closed_(event_, state_, info_);
            }
            throw;
        }
    }
    connection_lifecycle(const connection_lifecycle&) = delete;
    connection_lifecycle& operator=(const connection_lifecycle&) = delete;
    connection_lifecycle& operator=(connection_lifecycle&&) = delete;
    connection_lifecycle(connection_lifecycle&& other) noexcept
        : registry_(std::exchange(other.registry_, nullptr)),
          listener_(other.listener_),
          prev_(other.prev_),
          next_(other.next_),
          event_(other.event_),
          closed_(other.closed_),
          state_(std::move(other.state_)),
          info_(other.info_),
          transport_(other.transport_),
          aborted_(other.aborted_),
          bound_(other.bound_),
          interface_index_(other.interface_index_),
          local_text_(other.local_text_) {
        if (registry_ != nullptr) {
            if (prev_ != nullptr) {
                prev_->next_ = this;
            } else {
                registry_->head_ = this;
            }
            if (next_ != nullptr) {
                next_->prev_ = this;
            }
        }
    }
    ~connection_lifecycle() {
        if (registry_ == nullptr) {
            return;
        }
        if (prev_ != nullptr) {
            prev_->next_ = next_;
        } else {
            registry_->head_ = next_;
        }
        if (next_ != nullptr) {
            next_->prev_ = prev_;
        }
        registry_ = nullptr;
        const auto closed = closed_;
        const auto event = event_;
        const auto info = info_;
        const auto state = std::move(state_);
        if (closed) {
            closed(event, state, info);
        }
    }
    [[nodiscard]] std::uint64_t connection_id() const noexcept {
        return event_.connection_id();
    }
    [[nodiscard]] const std::shared_ptr<void>& state() const noexcept {
        return state_;
    }
    [[nodiscard]] std::string_view local_address() const noexcept {
        return local_text_.data();
    }
    [[nodiscard]] const ip_endpoint& local_endpoint() const noexcept {
        return event_.local();
    }
    [[nodiscard]] const ip_endpoint& remote_endpoint() const noexcept {
        return event_.remote();
    }
    void set_reason(connection_close_reason reason) noexcept {
        // Keep the initiating cause when teardown subsequently cancels I/O.
        if (info_.reason_ == connection_close_reason::normal ||
            (info_.reason_ == connection_close_reason::peer_eof && reason != connection_close_reason::normal)) {
            info_.reason_ = reason;
        }
    }
    void note_read(std::size_t bytes) noexcept {
        add_bytes(info_.bytes_in_, bytes);
    }
    void note_write(std::size_t bytes) noexcept {
        add_bytes(info_.bytes_out_, bytes);
    }
    void note_request() noexcept {
        add_bytes(info_.requests_, 1);
    }
    void note_error(const std::error_code& error) noexcept {
        if (!error || error == asio::error::would_block || error == asio::error::try_again ||
            error == asio::error::interrupted) {
            return;
        }
        if (is_stream_read_end(error)) {
            set_reason(connection_close_reason::peer_eof);
        } else if (error == asio::error::timed_out || error == std::errc::timed_out) {
            set_reason(connection_close_reason::timeout);
        } else if (error == asio::error::operation_aborted || error == std::errc::operation_canceled ||
                   error == asio::error::bad_descriptor) {
            set_reason(connection_close_reason::cancelled);
        } else if (error == std::errc::protocol_error || error.category() == asio::error::get_ssl_category()) {
            set_reason(connection_close_reason::protocol_error);
        } else {
            set_reason(connection_close_reason::transport_error);
        }
    }
    void note_exception(const std::exception_ptr& failure) noexcept {
        if (failure == nullptr) {
            return;
        }
        try {
            std::rethrow_exception(failure);
        } catch (const inbound_transport_error& error) {
            note_error(error.code());
        } catch (...) {
            set_reason(connection_close_reason::handler_error);
        }
    }
    [[nodiscard]] std::uint32_t interface_index() const noexcept {
        return interface_index_;
    }
    [[nodiscard]] std::uint16_t local_port() const noexcept {
        return event_.local().port();
    }
    void bind_transport(connection_transport transport) noexcept {
        transport_ = transport;
        bound_ = true;
        if (aborted_) {
            transport_.close();
        }
    }
    void abort() noexcept {
        set_reason(connection_close_reason::aborted);
        aborted_ = true;
        if (bound_) {
            transport_.close();
        }
    }
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_;
    }

private:
    friend class connection_registry;
    static void add_bytes(std::uint64_t& value, std::size_t bytes) noexcept {
        const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        value += (std::min)(maximum - value, static_cast<std::uint64_t>(bytes));
    }
    void format_local() noexcept {
        const auto address = to_asio_address(event_.local().address());
        asio::error_code error;
        if (address.is_v4()) {
            const auto bytes = address.to_v4().to_bytes();
            (void)asio::detail::socket_ops::inet_ntop(ASIO_OS_DEF(AF_INET), bytes.data(),
                local_text_.data(), local_text_.size(), 0, error);
        } else {
            const auto v6 = address.to_v6();
            const auto bytes = v6.to_bytes();
            (void)asio::detail::socket_ops::inet_ntop(ASIO_OS_DEF(AF_INET6), bytes.data(),
                local_text_.data(), local_text_.size(), v6.scope_id(), error);
        }
    }
    connection_registry* registry_;
    const void* listener_;
    connection_lifecycle* prev_{};
    connection_lifecycle* next_{};
    connection_event event_;
    connection_close_callback_ref closed_;
    std::shared_ptr<void> state_;
    connection_close_info info_;
    connection_transport transport_;
    bool aborted_{};
    bool bound_{};
    std::uint32_t interface_index_{};
    std::array<char, 64> local_text_{};
};

inline bool connection_registry::close_connection(std::uint64_t id) noexcept {
    if (id == 0) {
        return false;
    }
    for (auto* node = head_; node != nullptr; node = node->next_) {
        if (node->connection_id() == id) {
            node->abort();
            return true;
        }
    }
    return false;
}

inline void connection_registry::close_listener(const void* listener) noexcept {
    for (auto* node = head_; node != nullptr; node = node->next_) {
        if (node->listener_ == listener) {
            node->abort();
        }
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// The caller owns the socket and the already-read prefix until all I/O joins.
// Prefix delivery is asynchronous, like socket reads, and never reads a byte twice.
class inbound_socket_stream final {
public:
    using executor_type = asio::ip::tcp::socket::executor_type;
    using lowest_layer_type = asio::ip::tcp::socket;

    explicit inbound_socket_stream(asio::ip::tcp::socket& socket, std::span<const std::byte> prefix = {},
        connection_lifecycle* lifecycle = nullptr) noexcept
        : socket_(socket),
          prefix_(prefix),
          lifecycle_(lifecycle) {}

    executor_type get_executor() noexcept {
        return socket_.get_executor();
    }
    lowest_layer_type& lowest_layer() noexcept {
        return socket_;
    }
    const lowest_layer_type& lowest_layer() const noexcept {
        return socket_;
    }
    auto native_handle() {
        return socket_.native_handle();
    }
    bool non_blocking() const {
        return socket_.non_blocking();
    }
    void shutdown(asio::ip::tcp::socket::shutdown_type what, std::error_code& error) {
        socket_.shutdown(what, error);
    }
    template <typename buffers_type>
    std::size_t send(const buffers_type& buffers, asio::socket_base::message_flags flags, std::error_code& error) {
        const auto bytes = socket_.send(buffers, flags, error);
        if (lifecycle_ != nullptr) {
            lifecycle_->note_write(bytes);
            lifecycle_->note_error(error);
        }
        return bytes;
    }

    template <typename buffers_type>
    std::size_t read_some(const buffers_type& buffers, std::error_code& error) {
        if (!prefix_.empty()) {
            error.clear();
            return consume_prefix(buffers);
        }
        const auto bytes = socket_.read_some(buffers, error);
        if (lifecycle_ != nullptr) {
            lifecycle_->note_read(bytes);
            lifecycle_->note_error(error);
        }
        return bytes;
    }

    template <typename buffers_type, typename handler_type>
    void async_read_some(const buffers_type& buffers, handler_type&& handler) {
        if (prefix_.empty()) {
            socket_.async_read_some(buffers, completion<std::decay_t<handler_type>, false>{
                                                 std::forward<handler_type>(handler), socket_.get_executor(), lifecycle_});
            return;
        }
        const auto bytes = consume_prefix(buffers);
        asio::post(socket_.get_executor(), [handler = std::forward<handler_type>(handler), bytes]() mutable {
            std::move(handler)(std::error_code{}, bytes);
        });
    }

    template <typename buffers_type, typename handler_type>
    void async_write_some(const buffers_type& buffers, handler_type&& handler) {
        socket_.async_write_some(buffers, completion<std::decay_t<handler_type>, true>{
                                              std::forward<handler_type>(handler), socket_.get_executor(), lifecycle_});
    }

private:
    template <typename handler_type, bool writing>
    struct completion final {
        handler_type handler_;
        inbound_socket_stream::executor_type fallback_executor_;
        connection_lifecycle* lifecycle_;
        using executor_type = asio::associated_executor_t<handler_type, inbound_socket_stream::executor_type>;
        using allocator_type = asio::associated_allocator_t<handler_type>;
        using cancellation_slot_type = asio::associated_cancellation_slot_t<handler_type>;
        [[nodiscard]] executor_type get_executor() const noexcept {
            return asio::get_associated_executor(handler_, fallback_executor_);
        }
        [[nodiscard]] allocator_type get_allocator() const noexcept {
            return asio::get_associated_allocator(handler_);
        }
        [[nodiscard]] cancellation_slot_type get_cancellation_slot() const noexcept {
            return asio::get_associated_cancellation_slot(handler_);
        }
        void operator()(const std::error_code& error, std::size_t bytes) {
            if (lifecycle_ != nullptr) {
                if constexpr (writing) {
                    lifecycle_->note_write(bytes);
                } else {
                    lifecycle_->note_read(bytes);
                }
                // Protocol drivers also cancel reads for a successful drain.
                // Timeout, shutdown and explicit abort record their cause first.
                if (error != asio::error::operation_aborted && error != asio::error::bad_descriptor) {
                    lifecycle_->note_error(error);
                }
            }
            std::move(handler_)(error, bytes);
        }
    };
    template <typename buffers_type>
    std::size_t consume_prefix(const buffers_type& buffers) noexcept {
        const auto bytes = asio::buffer_copy(buffers, asio::buffer(prefix_.data(), prefix_.size()));
        prefix_ = prefix_.subspan(bytes);
        return bytes;
    }
    asio::ip::tcp::socket& socket_;
    std::span<const std::byte> prefix_;
    connection_lifecycle* lifecycle_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// SSL 的读操作也可能发送 KeyUpdate 等控制记录。取消读允许保留已接收的
// 密文字节，但不能截断已从 SSL BIO 取出的输出，否则后续复用会丢失记录。
// 屏蔽单操作的写取消；socket 关闭和既有连接超时仍能终止底层写入。
class tls_socket_transport final {
public:
    using executor_type = asio::ip::tcp::socket::executor_type;
    using lowest_layer_type = asio::ip::tcp::socket;

    explicit tls_socket_transport(inbound_socket_stream& socket) noexcept
        : socket_(socket) {}

    [[nodiscard]] executor_type get_executor() noexcept {
        return socket_.get_executor();
    }

    [[nodiscard]] lowest_layer_type& lowest_layer() noexcept {
        return socket_.lowest_layer();
    }

    [[nodiscard]] const lowest_layer_type& lowest_layer() const noexcept {
        return socket_.lowest_layer();
    }

    template <typename buffers_type, typename handler_type>
    auto async_read_some(const buffers_type& buffers, handler_type&& handler) {
        return socket_.async_read_some(buffers, std::forward<handler_type>(handler));
    }

    template <typename buffers_type, typename handler_type>
    auto async_write_some(const buffers_type& buffers, handler_type&& handler) {
        return socket_.async_write_some(buffers,
            asio::bind_cancellation_slot(asio::cancellation_slot{}, std::forward<handler_type>(handler)));
    }

private:
    inbound_socket_stream& socket_;
};

using server_tls_stream = asio::ssl::stream<tls_socket_transport>;

}  // namespace ruvia::detail

namespace ruvia::detail {

// How a TCP connection session ended, which decides how its socket closes.
// A stopping worker closes every connection immediately, whatever its mode.
enum class http_connection_close : std::uint8_t {
    // Transport failure, a stalled transfer, or a vanished peer: close at once.
    abort,
    // No further peer input is expected: the peer ended its input (FIN or TLS
    // close_notify), or this side ended a wait for a request head (timeout or
    // drain). TLS sends close_notify first (RFC 9112 §9.8, RFC 9846 §6.1).
    notify,
    // This side ended the connection while peer input may still arrive: after
    // the final HTTP/1 response, or after HTTP/2 GOAWAY. Lingering close
    // (RFC 9112 §9.6): TLS close_notify, TCP send shutdown, then peer input is
    // read and discarded until its end, a deadline or a byte budget, so unread
    // input cannot make TCP reset the connection and destroy the last response.
    linger,
};

// Half-closes plain TCP after the final response. The session's close still
// lingers and closes the socket.
inline void shutdown_http_connection_send(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
    socket.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
}

// The lingering close of a plain connection, short of the final TCP close.
// The caller closes TCP after completion and must not start this operation
// once the worker is stopping; scanner close_all, a fixed deadline and the
// worker scanner bound it. The task borrows its arguments until completion.
[[nodiscard]] task<void> linger_http_connection(
    asio::ip::tcp::socket& socket, connection_scanner& scanner, const worker_ref& worker_value);

// Sends TLS close_notify for a non-aborted session without waiting for the
// peer's alert, then lingers like linger_http_connection() when `mode` is
// http_connection_close::linger. Same bounds and caller duties as above.
[[nodiscard]] task<void> close_http_connection_gracefully(server_tls_stream& stream,
    http_connection_close mode, connection_scanner& scanner, const worker_ref& worker_value);

}  // namespace ruvia::detail

// The one reader for file response bodies (a whole file, a byte range, or
// multipart/byteranges parts interleaved with in-memory boundaries) on every
// transport. Only a single large range on Windows plain TCP leaves through a
// kernel TransmitFile instead (http1_file_response_body).
//
// Opening and blocking reads run on the worker's blocking pool, never on the
// worker. Each round trip reads ahead up to response_file_read_ahead_bytes;
// Linux tries later reads with RWF_NOWAIT first. The owned read state moves
// through the pool, without copying its buffered bytes. A worker without a
// pool (application::blocking_pool(nullptr)) cannot produce a file body: the
// read fails with blocking_status::unavailable on every platform.
//
// A response framed from an open handle (context::file, static_root) is read
// through a duplicate of that handle, so the path is never resolved again.
//
// Every file range is re-validated against its snapshot identity on the read
// that completes it, so the final bytes of a range are never released for a
// file that changed while being sent. The first file range is validated
// before anything is yielded, so a transport can answer a failure with an
// error response while its head is still unsent.

namespace ruvia::detail {

// Bytes read per blocking round trip. Kept within the process resource's
// pooled block range, so the buffer is recycled across responses.
inline constexpr std::size_t response_file_read_ahead_bytes = 64 * 1024;

// Where a file body's blocking reads run -- the worker's blocking pool -- and
// what bounds or abandons them.
struct response_file_io final {
    worker_ref worker_;
    stop_token stop_;
    // Bounds each read the response waits on. Servers pass write_timeout_: the
    // next write waits for the read. An abandoned read keeps running on the
    // pool and frees its own handle and buffer; the response does not wait.
    std::optional<timeout_duration> timeout_;
};

// Why a file body could not be produced.
class response_file_failure final {
public:
    // The read ran: the open, read or snapshot validation failed with `error`.
    [[nodiscard]] static response_file_failure read_failed(std::error_code error);
    // The read never completed: the pool refused, timed out or abandoned it.
    [[nodiscard]] static response_file_failure read_not_completed(blocking_status status);
    // The read threw on the pool thread.
    [[nodiscard]] static response_file_failure read_threw(std::exception_ptr exception) noexcept;

    // The error a response whose head was not committed is answered with:
    // 503 file_read_unavailable when the read never completed, otherwise 500
    // file_read_failed. Neither names the file or the system error.
    [[nodiscard]] std::exception_ptr response_error() const;

    // The fault for the connection failure channel, or null for an expected
    // outcome: a file removed, replaced or changed since it was framed, a
    // full pool queue (counted by blocking_pool_stats), or shutdown.
    [[nodiscard]] const std::exception_ptr& fault() const& noexcept RUVIA_LIFETIMEBOUND {
        return fault_;
    }
    const std::exception_ptr& fault() const&& = delete;

private:
    response_file_failure(bool unavailable, std::exception_ptr fault) noexcept
        : fault_(std::move(fault)),
          unavailable_(unavailable) {}

    std::exception_ptr fault_;
    bool unavailable_;
};

class response_body_chunk final {
public:
    enum class kind_type : std::uint8_t {
        data,
        end,
        failed,
    };

    [[nodiscard]] static constexpr response_body_chunk data(std::string_view bytes) noexcept {
        return response_body_chunk(kind_type::data, bytes);
    }

    [[nodiscard]] static constexpr response_body_chunk end() noexcept {
        return response_body_chunk(kind_type::end, {});
    }

    [[nodiscard]] static constexpr response_body_chunk failed() noexcept {
        return response_body_chunk(kind_type::failed, {});
    }

    [[nodiscard]] constexpr kind_type kind() const noexcept {
        return kind_;
    }

    // Valid until the next call to response_file_source::next() or
    // release_buffer().
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    constexpr response_body_chunk(kind_type kind, std::string_view bytes) noexcept
        : bytes_(bytes),
          kind_(kind) {}

    std::string_view bytes_;
    kind_type kind_;
};

class response_file_source final {
public:
    // `response` is borrowed until the last chunk was consumed.
    response_file_source(const http_response& response, response_file_io io);

    response_file_source(const response_file_source&) = delete;
    response_file_source& operator=(const response_file_source&) = delete;
    response_file_source(response_file_source&&) = delete;
    response_file_source& operator=(response_file_source&&) = delete;

    // Opens and validates the first file range without reading it or
    // allocating a read buffer: one pool round trip that lets a transport
    // commit its head before any flow-control credit exists. Idempotent.
    [[nodiscard]] task<bool> open_first();

    // Yields the body in order: in-memory segments as borrowed views, file
    // ranges as read-ahead buffers of at most `max_bytes` (nonzero). `failed`
    // means a file could not be opened or read, ended early, changed
    // identity, or the read did not complete; failure() says why. Once the
    // head is committed the connection (HTTP/1) or stream (HTTP/2) must end.
    [[nodiscard]] task<response_body_chunk> next(std::size_t max_bytes = response_file_read_ahead_bytes);

    // Returns the read buffer once the last chunk's bytes were consumed, so a
    // body waiting for flow-control credit holds none; the next file chunk
    // allocates again.
    void release_buffer() noexcept;

    // Whether the next chunk next() yields will be read from a file, so a
    // flow-controlled transport can wait for credit before reading it.
    [[nodiscard]] bool file_bytes_pending() const noexcept;

    [[nodiscard]] const response_file_failure* failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return failure_ ? &*failure_ : nullptr;
    }
    const response_file_failure* failure() const&& = delete;

private:
    // Owned by exactly one thread at a time: it moves into the blocking
    // callable and back, so an abandoned read frees it on the pool thread
    // through the synchronized process resource.
    class read_buffer final {
    public:
        read_buffer() noexcept = default;

        [[nodiscard]] char* data() & RUVIA_LIFETIMEBOUND;
        char* data() && = delete;
        void reset() noexcept;

    private:
        struct buffer_deleter {
            void operator()(char* bytes) const noexcept;
        };

        std::unique_ptr<char, buffer_deleter> data_;
    };

    struct read_state final {
        read_state();

        http_native_path_string_type path_;
        std::uint64_t size_{};
        http_response_file_identity identity_{http_response_file_identity::unchecked()};
        // A duplicate of the response's handle, or empty until the pool opens
        // path_ for a body framed without one.
        native_file_descriptor input_;
        read_buffer buffer_;
        std::uint64_t position_{};
        std::size_t filled_{};
        std::error_code error_{};
        // input_ matched identity_ and size_ when it was first used.
        bool validated_{false};
#if defined(__linux__)
        bool cached_read_supported_{true};
#endif
    };

    static void read_ahead(read_state& state, std::size_t bytes, bool completes_range);
    [[nodiscard]] task<bool> fill(std::size_t bytes, bool completes_range);
    [[nodiscard]] bool settle_read();
    void select_file(http_response_file_view file);

    const http_response& response_;
    response_file_io io_;
    read_state state_;
    std::optional<response_file_failure> failure_;
    const http_native_path_char_type* current_path_{};
    std::uint64_t range_remaining_{};
    std::size_t next_segment_{};
    std::size_t segment_count_{};
    bool first_file_opened_{false};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

#if defined(_WIN32)
// On Windows plain TCP a single file range longer than this leaves through an
// overlapped TransmitFile from the response's own handle. Shorter ranges,
// multipart bodies, TLS and every other platform read through
// response_file_source, which sends the head and first chunk together.
inline constexpr std::uint64_t transmit_file_min_bytes = std::uint64_t{16} * 1024;

struct http_response_file_write_result final {
    std::error_code error_;
    // Whether every byte of the head reached the socket before error_.
    bool head_written_{false};
};

// Sends `head`, then the range of `file` from `input`, a handle the caller
// already validated against the file's snapshot. The first TransmitFile
// carries the head. Each later slice starts only while `input` still matches
// the snapshot, and the final byte waits for that check, so a file that
// changes while it is sent never completes its framed length. Touches
// `progress` after every transfer.
task<http_response_file_write_result> write_http_response_file(asio::ip::tcp::socket& socket,
    std::string_view head, native_file_type input, http_response_file_view file, connection_scanner::entry& progress);
#endif

}  // namespace ruvia::detail

namespace ruvia::detail {

#if defined(_WIN32)
inline task<http_response_file_write_result> write_http_response_file(asio::ip::tcp::socket& socket,
    std::string_view head, native_file_type input, http_response_file_view file, connection_scanner::entry& progress) {
    http_response_file_write_result result;
    // A null OVERLAPPED would make every TransmitFile call synchronous until
    // the peer drained the whole range, blocking this worker's event loop
    // behind one slow reader; the overlapped call completes through the
    // socket's IOCP instead and carries its file offset explicitly. Progress
    // is only observable when a call completes, so each call sends at most
    // one read-ahead slice, the same granularity at which the pooled reader
    // renews write_timeout. The head rides in the first call's
    // TRANSMIT_FILE_BUFFERS, saving a send and its completion.
    constexpr std::uint64_t max_transmit_file_bytes = response_file_read_ahead_bytes;
    std::uint64_t offset = file.offset();
    std::uint64_t remaining = file.length();
    while (remaining > 0 || !head.empty()) {
        // A zero byte count would mean "the whole file": a call that only
        // finishes the head passes no file at all. The final byte is sent
        // alone, after the handle was checked once more.
        auto next_send = std::min(remaining, max_transmit_file_bytes);
        if (next_send == remaining && remaining > 1) {
            next_send = remaining - 1;
        }
        TRANSMIT_FILE_BUFFERS buffers{};
        buffers.Head = const_cast<char*>(head.data());
        buffers.HeadLength = static_cast<DWORD>(head.size());
        const auto transmit_completion = co_await ruvia::async_asio<std::size_t>(
            [&socket, &buffers, input, offset, next_send = static_cast<DWORD>(next_send)](auto handler) {
                asio::windows::overlapped_ptr operation(socket.get_executor(), std::move(handler));
                operation.get()->Offset = static_cast<DWORD>(offset & UINT64_C(0xffffffff));
                operation.get()->OffsetHigh = static_cast<DWORD>(offset >> 32);
                const auto transmitted = ::TransmitFile(socket.native_handle(), next_send == 0 ? nullptr : input,
                    next_send, 0, operation.get(), buffers.HeadLength == 0 ? nullptr : &buffers, 0);
                const int socket_error = transmitted != FALSE ? 0 : ::WSAGetLastError();
                if (transmitted != FALSE || socket_error == WSA_IO_PENDING) {
                    static_cast<void>(operation.release());
                    return;
                }
                operation.complete(std::error_code(socket_error, std::system_category()), 0);
            });
        // A failed completion still reports the prefix it transferred, which
        // may cover the head: the response is committed once the head left.
        const auto sent = static_cast<std::uint64_t>(transmit_completion.result());
        if (auto* lifecycle = progress.lifecycle()) {
            lifecycle->note_write(transmit_completion.result());
            lifecycle->note_error(transmit_completion.error_code());
        }
        const auto head_sent = std::min<std::uint64_t>(sent, head.size());
        const auto file_sent = sent - head_sent;
        head.remove_prefix(static_cast<std::size_t>(head_sent));
        const auto transmit_error = transmit_completion.error_code();
        if (transmit_error || sent == 0 || file_sent > remaining) {
            result.error_ = transmit_error ? transmit_error : std::error_code(asio::error::operation_aborted);
            result.head_written_ = head.empty();
            co_return result;
        }
        offset += file_sent;
        remaining -= file_sent;
        progress.note_write_progress();
        if (remaining != 0) {
            if (const auto changed = validate_native_file(input, file)) {
                result.error_ = changed;
                result.head_written_ = head.empty();
                co_return result;
            }
        }
    }
    result.head_written_ = true;
    co_return result;
}
#endif

}  // namespace ruvia::detail

namespace ruvia::detail {

// A write commits the final status only once the entire head is out; it
// completes only when it also ends without error.
struct http1_buffered_response_write_result final {
    std::optional<http_status_code> committed_status_;
    bool completed_{false};
};

// `bytes_transferred` is the composed async-write prefix accepted before `error`.
[[nodiscard]] inline http1_buffered_response_write_result classify_http1_buffered_response_write(
    const http1_buffered_response_plan& plan, std::size_t response_head_bytes, std::error_code error,
    std::size_t bytes_transferred) noexcept {
    if (response_head_bytes == 0 || bytes_transferred < response_head_bytes) {
        return {};
    }
    return {plan.body_plan().response_status(), !error};
}
// Only for a final buffered response followed immediately by send shutdown.
// Linux can carry the response tail and FIN together; other platforms keep
// their ordinary send flags. Reusable and streaming writes must not use it.
#if defined(__linux__)
inline constexpr asio::socket_base::message_flags final_plain_tcp_send_flags = MSG_MORE;
#else
inline constexpr asio::socket_base::message_flags final_plain_tcp_send_flags = 0;
#endif

// Puts an accepted plain TCP socket into user non-blocking mode once, before
// its first I/O, so try_plain_tcp_sync_write() can offer bytes without
// blocking the worker. Asio's own internal non-blocking state is not enough:
// write_some() polls indefinitely unless the user mode is set. Asynchronous
// operations are unaffected on every platform: the reactor already runs the
// descriptor non-blocking, and on Windows asio issues overlapped
// WSASend/WSARecv/TransmitFile, which ignore FIONBIO, while a synchronous
// send() on a full send buffer returns WSAEWOULDBLOCK (asio::error::would_block)
// instead of blocking. A socket left blocking (the call failed) keeps every
// write on the asynchronous path.
inline void enable_plain_tcp_sync_write(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
#if defined(__linux__)
    // This socket is freshly accepted TCP, not an external descriptor. The
    // public command sets Asio's user mode as well as the kernel flag, without
    // the assigned-descriptor F_GETFL/F_SETFL round trip.
    struct non_blocking_command final {
        int enabled_{1};
        [[nodiscard]] int name() const noexcept {
            return FIONBIO;
        }
        [[nodiscard]] int* data() noexcept {
            return &enabled_;
        }
    } command;
    socket.io_control(command, ignored);
#else
    socket.non_blocking(true, ignored);
#endif
}

// Try one non-blocking plain TCP write before suspending. Returns true when
// the write completed or hit a hard error (outcome in ec/bytes_written), and
// false when the remainder must go through async_write.
template <typename socket_type, typename const_buffer_sequence_type>
[[nodiscard]] inline bool try_plain_tcp_sync_write(socket_type& socket,
    const const_buffer_sequence_type& buffers, std::size_t total_bytes,
    asio::socket_base::message_flags flags, std::error_code& ec,
    std::size_t& bytes_written) noexcept {
    ec.clear();
    bytes_written = 0;
    if (!socket.non_blocking()) {
        return false;
    }
    bytes_written = socket.send(buffers, flags, ec);
    if (!ec) {
        return bytes_written == total_bytes;
    }
    if (ec == asio::error::would_block || ec == asio::error::try_again ||
        ec == asio::error::interrupted) {
        ec.clear();
        return false;
    }
    return true;
}

// Appends `parts` to `output` so a TLS stream encrypts them as one record. A
// gather write of the same parts is equivalent on the wire, so failing to
// allocate only selects that fallback: false leaves `output` unchanged. The
// reservation is the only allocation; the appends then fit.
[[nodiscard]] inline bool try_coalesce_tls_frame(
    http_response_head_buffer& output, std::initializer_list<std::string_view> parts) noexcept {
    std::size_t total = 0;
    for (const auto part : parts) {
        total += part.size();
    }
    try {
        output.reserve_additional(total);
    } catch (const std::bad_alloc&) {
        return false;
    }
    for (const auto part : parts) {
        output.append(part);
    }
    return true;
}

// A write either accepts every byte or ends with an error.
struct connection_write_outcome final {
    std::error_code error_;
    // Prefix of the written bytes the transport accepted.
    std::size_t bytes_{};
};

// Initiates the async_write of what follows the first `written` bytes of
// `head` then `body`. Every partial transfer touches `progress`.
template <typename stream_type, typename handler_type>
void async_write_after(stream_type& stream, std::string_view head, std::string_view body, std::size_t written,
    connection_scanner::entry& progress, handler_type&& handler) {
    if (written >= head.size()) {
        asio::async_write(stream, asio::buffer(body.substr(written - head.size())), connection_write_progress(progress),
            std::forward<handler_type>(handler));
        return;
    }
    const std::array<asio::const_buffer, 2> remaining{asio::buffer(head.substr(written)), asio::buffer(body)};
    asio::async_write(stream, remaining, connection_write_progress(progress), std::forward<handler_type>(handler));
}

// Awaitable write of `head` then `body`. Plain TCP first offers both to one
// non-blocking send; only a remainder suspends, as an async_write awaited in
// place, so a write the socket accepts at once costs no coroutine frame.
// Every transfer, synchronous or not, touches `progress`. The views must stay
// valid until the co_await completes.
template <typename stream_type>
class connection_gather_write final {
public:
    connection_gather_write(stream_type& stream, std::string_view head, std::string_view body,
        connection_scanner::entry& progress) noexcept
        : stream_(&stream),
          head_(head),
          body_(body),
          progress_(&progress) {}

    connection_gather_write(const connection_gather_write&) = delete;
    connection_gather_write& operator=(const connection_gather_write&) = delete;
    connection_gather_write* operator&() = delete;
    [[nodiscard]] bool await_ready() noexcept {
        if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, inbound_socket_stream>) {
            const std::array<asio::const_buffer, 2> buffers{asio::buffer(head_), asio::buffer(body_)};
            const bool settled = try_plain_tcp_sync_write(
                *stream_, buffers, head_.size() + body_.size(), 0, written_.error_, written_.bytes_);
            if (written_.bytes_ != 0) {
                progress_->note_write_progress();
            }
            return settled;
        } else {
            return false;
        }
    }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) {
        return remainder_.emplace(remainder_initiation{this}).await_suspend(handle);
    }

    [[nodiscard]] connection_write_outcome await_resume() {
        if (remainder_) {
            const auto completion = remainder_->await_resume();
            written_.error_ = completion.error_code();
            written_.bytes_ += completion.result();
        }
        return written_;
    }

private:
    struct remainder_initiation final {
        connection_gather_write* owner_;

        template <typename handler_type>
        void operator()(handler_type&& handler) const {
            async_write_after(*owner_->stream_, owner_->head_, owner_->body_, owner_->written_.bytes_,
                *owner_->progress_, std::forward<handler_type>(handler));
        }
    };

    stream_type* stream_;
    std::string_view head_;
    std::string_view body_;
    connection_scanner::entry* progress_;
    connection_write_outcome written_;
    std::optional<asio_completion_awaiter<std::size_t, remainder_initiation>> remainder_;
};

// A buffered response serialized into the connection's output.
struct http1_response_write_start final {
    // Output offset at which this response's head is complete. Responses held
    // back before it precede it; without them this is the head's length.
    std::size_t head_bytes_{};
    // In-memory body bytes not coalesced into the output.
    std::string_view body_;
    // The response stays in the output for a later coalesced write.
    bool held_{false};
};

// Appends the response head to `output`, after any pipelined responses held
// back there, and coalesces an in-memory body that fits the inline head
// storage. A nonzero `hold_limit` means the connection already holds its next
// request: the body is then coalesced up to that many output bytes, and a
// response wholly in the output below the limit is held back instead of
// written. File bodies are never coalesced or held; the caller writes any
// held-back output first, then continues with http1_file_response_body.
[[nodiscard]] inline http1_response_write_start start_response_write(http_response_head_buffer& output,
    const http_response& response, const http1_buffered_response_plan& response_plan, std::size_t hold_limit,
    bool coalesce_for_tls = false) {
    append_http1_response_head(response, output, response_plan.head_plan());
    http1_response_write_start start;
    start.head_bytes_ = output.view().size();
    if (response.file_body() || response.has_multipart_file_body()) {
        return start;
    }

    auto body = response_plan.send_body() ? response.body_bytes() : std::string_view{};
    if (!body.empty()) {
        const bool can_hold = start.head_bytes_ < hold_limit && body.size() < hold_limit - start.head_bytes_;
        if (can_hold || (coalesce_for_tls && output.can_append_on_stack(body.size()))) {
            output.append(body);
            body = {};
        } else if (coalesce_for_tls && start.head_bytes_ <= response_head_retained_heap_bytes &&
                   body.size() <= response_head_retained_heap_bytes - start.head_bytes_ &&
                   try_coalesce_tls_frame(output, {body})) {
            body = {};
        }
    }
    start.body_ = body;
    start.held_ = body.empty() && output.view().size() < hold_limit;
    return start;
}

// The body of a file or multipart/byteranges response. start() runs before
// any byte of the head leaves: it reads the first chunk, which validates the
// first file range, or for a kernel transfer validates the response's
// handle. A missing, unreadable or changed file can then still be answered
// with an error response. finish() writes the head start_response_write()
// serialized into an otherwise empty `head`, with the first chunk in the same
// send, then the rest of the body. Every partial transfer touches `progress`.
template <typename stream_type>
class http1_file_response_body final {
public:
    // `response` is borrowed until finish() completes.
    http1_file_response_body(const http_response& response, const response_file_io& file_io)
        : source_(response, file_io)
#if defined(_WIN32)
          ,
          transmit_file_(transmit_candidate(response))
#endif
    {
    }

    http1_file_response_body(const http1_file_response_body&) = delete;
    http1_file_response_body& operator=(const http1_file_response_body&) = delete;
    http1_file_response_body(http1_file_response_body&&) = delete;
    http1_file_response_body& operator=(http1_file_response_body&&) = delete;

    // False when the body failed with nothing sent; failure() says why.
    [[nodiscard]] task<bool> start(const http1_buffered_response_plan& response_plan) {
        // HEAD and status-suppressed bodies transfer no representation.
        if (response_plan.body_plan().body_suppressed()) {
            co_return true;
        }
#if defined(_WIN32)
        if (transmit_file_) {
            // The framing handle stays valid across an in-place rewrite, which
            // must fail before any byte is sent.
            if (const auto error =
                    validate_native_file(borrowed_native_file(*transmit_file_->opened_file()), *transmit_file_)) {
                transmit_failure_.emplace(response_file_failure::read_failed(error));
                co_return false;
            }
            transmit_ = true;
            co_return true;
        }
#endif
        first_ = co_await source_.next();
        co_return first_.kind() != response_body_chunk::kind_type::failed;
    }

    [[nodiscard]] task<http1_buffered_response_write_result> finish(stream_type& stream,
        http_response_head_buffer& head, const http1_buffered_response_plan& response_plan,
        const http1_response_write_start& start, connection_scanner::entry& progress) {
        const auto response_head_bytes = start.head_bytes_;
#if defined(_WIN32)
        if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, inbound_socket_stream>) {
            if (transmit_) {
                const auto written = co_await write_http_response_file(stream.lowest_layer(), head.view(),
                    borrowed_native_file(*transmit_file_->opened_file()), *transmit_file_, progress);
                co_return classify_http1_buffered_response_write(response_plan, response_head_bytes,
                    written.error_, written.head_written_ ? response_head_bytes : 0);
            }
        }
#endif
        const auto first_written = co_await connection_gather_write(stream, head.view(), first_.bytes(), progress);
        if (first_written.error_ || !response_plan.send_body()) {
            co_return classify_http1_buffered_response_write(
                response_plan, response_head_bytes, first_written.error_, first_written.bytes_);
        }
        if (first_.kind() == response_body_chunk::kind_type::end) {
            co_return classify_http1_buffered_response_write(response_plan, response_head_bytes, {}, response_head_bytes);
        }
        for (;;) {
            const auto chunk = co_await source_.next();
            if (chunk.kind() == response_body_chunk::kind_type::end) {
                co_return classify_http1_buffered_response_write(response_plan, response_head_bytes, {}, response_head_bytes);
            }
            if (chunk.kind() == response_body_chunk::kind_type::failed) {
                co_return classify_http1_buffered_response_write(response_plan, response_head_bytes,
                    std::make_error_code(std::errc::io_error), response_head_bytes);
            }
            progress.note_write_progress();
            if (const auto written = co_await connection_gather_write(stream, chunk.bytes(), {}, progress); written.error_) {
                co_return classify_http1_buffered_response_write(
                    response_plan, response_head_bytes, written.error_, response_head_bytes);
            }
        }
    }

    // Why start() or finish() could not produce the body from the file.
    [[nodiscard]] const response_file_failure* failure() const noexcept {
        if (const auto* failure = source_.failure()) {
            return failure;
        }
#if defined(_WIN32)
        if (transmit_failure_) {
            return &*transmit_failure_;
        }
#endif
        return nullptr;
    }

private:
#if defined(_WIN32)
    // The single range a kernel transfer may send from the response's handle.
    [[nodiscard]] static std::optional<http_response_file_view> transmit_candidate(
        [[maybe_unused]] const http_response& response) noexcept {
        if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, inbound_socket_stream>) {
            if (!response.has_multipart_file_body()) {
                const auto file = response.file_body();
                if (file && file->length() > transmit_file_min_bytes && file->opened_file() != nullptr) {
                    return file;
                }
            }
        }
        return std::nullopt;
    }
#endif

    response_file_source source_;
    response_body_chunk first_{response_body_chunk::end()};
#if defined(_WIN32)
    std::optional<http_response_file_view> transmit_file_;
    std::optional<response_file_failure> transmit_failure_;
    bool transmit_{false};
#endif
};

}  // namespace ruvia::detail
