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
#include "ruvia/web/http_client.h"
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

#include <cassert>
#include <variant>

#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/ssl/context.hpp>

#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/websocket_handshake.h"
#include "ruvia/web/context.h"
#include "ruvia/web/streaming.h"

#if defined(RUVIA_ENABLE_HTTP3)
#include <asio/ip/udp.hpp>
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
                const auto transmitted = ::TransmitFile(socket.native(), next_send == 0 ? nullptr : input,
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

#if defined(__unix__)
#endif

namespace ruvia::detail {

[[nodiscard]] inline bool is_windows_drive_path(std::string_view path) noexcept {
    if (path.size() < 2 || path[1] != ':') {
        return false;
    }
    const auto c = path.front();
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[noreturn]] inline void throw_invalid_static_file_path() {
    throw http_error({.status_ = ruvia::http_status::forbidden,
        .code_ = "forbidden",
        .message_ = "invalid static file path"});
}

// Percent-decodes `input` in one pass into `output`, which holds at least
// input.size() bytes, and returns the decoded size. Percent mode fails on a
// malformed escape; form mode maps '+' to a space and keeps a '%' that starts
// no valid escape as a literal byte (WHATWG percent-decode). With
// `reject_encoded_separators`, an escape revealing '/' or '\\' fails too.
[[nodiscard]] inline std::optional<std::size_t> decode_url_bytes(
    std::string_view input, char* output, url_decode_mode mode, bool reject_encoded_separators) noexcept {
    std::size_t size = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (mode == url_decode_mode::form && c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (const auto byte = http_try_decode_percent_byte(input, i)) {
                if (reject_encoded_separators && (*byte == '/' || *byte == '\\')) {
                    return std::nullopt;
                }
                c = static_cast<char>(*byte);
                i += 2;
            } else if (mode != url_decode_mode::form) {
                return std::nullopt;
            }
        }
        output[size++] = c;
    }
    return size;
}

// Percent-decodes an encoded request path, or a wildcard tail of one, exactly
// once into the file path it names: the path itself when it holds no escape,
// otherwise the decoded bytes in `storage`. Routing and path-scoped middleware
// split request paths on raw '/' only, so an escape revealing '/' or '\\'
// would forge a segment boundary they never saw; that, like a malformed
// escape, yields nullopt.
[[nodiscard]] inline std::optional<std::string_view> decode_request_path(
    std::string_view path, std::string& storage) {
    if (!has_url_encoding(path, url_decode_mode::percent)) {
        return path;
    }
    storage.resize(path.size());
    const auto size = decode_url_bytes(path, storage.data(), url_decode_mode::percent, true);
    if (!size) {
        return std::nullopt;
    }
    storage.resize(*size);
    return std::string_view(storage);
}

// Web static-file path policy: validate a decoded file path relative to the
// configured document root. '/' and '\\' both separate segments; a "." or ".."
// segment, an empty interior segment, a NUL, or an absolute or drive path is
// rejected (403) rather than resolved. Requests carrying a dot segment never
// reach a route, and a decoded parameter could otherwise hide one behind an
// encoded separator ("sub%2F..%2Fsecret") that no route or path scope saw, so
// every caller fails closed instead of resolving it. A final separator names
// the directory itself and is dropped. The input is borrowed when it is
// already canonical; only a '\\' separator rebuilds it in `storage`.
[[nodiscard]] inline std::optional<std::string_view> try_normalize_static_relative_path(
    std::string_view input, std::string& storage) {
    if (input.empty()) {
        return input;
    }
    if (input.front() == '/' || input.front() == '\\' || is_windows_drive_path(input)) {
        return std::nullopt;
    }

    constexpr std::string_view stop_bytes("/\\\0", 3);
    bool backslash = false;
    std::size_t cursor_value = 0;
    for (;;) {
        const auto stop = input.find_first_of(stop_bytes, cursor_value);
        const auto end = stop == std::string_view::npos ? input.size() : stop;
        const auto segment = input.substr(cursor_value, end - cursor_value);
        if (segment == "." || segment == "..") {
            return std::nullopt;
        }
        if (stop == std::string_view::npos) {
            break;
        }
        if (input[stop] == '\0' || segment.empty()) {
            return std::nullopt;
        }
        backslash = backslash || input[stop] == '\\';
        cursor_value = stop + 1;
    }

    auto canonical = input;
    if (canonical.back() == '/' || canonical.back() == '\\') {
        canonical.remove_suffix(1);
    }
    if (!backslash) {
        return canonical;
    }
    storage.assign(canonical.data(), canonical.size());
    std::ranges::replace(storage, '\\', '/');
    return storage;
}

[[nodiscard]] inline std::string_view normalize_static_relative_path(
    std::string_view input, std::string& storage) {
    const auto normalized = try_normalize_static_relative_path(input, storage);
    if (!normalized.has_value()) {
        throw_invalid_static_file_path();
    }
    return *normalized;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

struct static_root_mime_type_storage final {
    friend bool operator==(const static_root_mime_type_storage&, const static_root_mime_type_storage&) = default;

    std::string extension_;
    std::string content_type_;
};

// Validated, normalized, owning static-root policy.
struct static_root_config_storage final {
    static_root_config_storage() = default;
    static_root_config_storage(const static_root_config_storage&) = default;
    static_root_config_storage(static_root_config_storage&&) noexcept = default;
    static_root_config_storage& operator=(const static_root_config_storage&) = delete;
    static_root_config_storage& operator=(static_root_config_storage&&) noexcept = default;

    friend bool operator==(const static_root_config_storage&, const static_root_config_storage&) = default;

    std::string cache_control_;
    std::string index_file_;
    std::string default_content_type_{"application/octet-stream"};
    std::vector<static_root_mime_type_storage> mime_types_;
    static_file_type_policy::kind file_type_kind_{static_file_type_policy::kind::defaults};
    std::vector<std::string> file_type_extensions_;
    static_range_request_policy range_requests_{static_range_request_policy::honor};
    static_response_validator_policy response_validators_{static_response_validator_policy::emit};
    static_dotfile_policy dotfiles_{static_dotfile_policy::deny};
};

[[nodiscard]] static_root_config_storage store_validated_static_root_config(
    const static_root_options& source_value);

[[nodiscard]] bool is_valid_static_file_extension(std::string_view extension) noexcept;

[[nodiscard]] bool file_type_allowed(
    std::string_view extension, const static_root_config_storage& config);

[[nodiscard]] std::string content_type_for(std::string_view extension,
    const static_root_config_storage& config);
}  // namespace ruvia::detail

namespace ruvia::detail {

struct static_root_precompression_options final {
    bool gzip_{false};
    bool brotli_{false};
    bool zstd_{false};
    std::size_t min_bytes_{1024};
    std::size_t max_bytes_{std::size_t{256} * 1024};

    [[nodiscard]] constexpr bool enabled() const noexcept {
        return gzip_ || brotli_ || zstd_;
    }

    [[nodiscard]] constexpr bool enabled(http_content_coding coding) const noexcept {
        switch (coding) {
            case http_content_coding::gzip:
                return gzip_;
            case http_content_coding::brotli:
                return brotli_;
            case http_content_coding::zstd:
                return zstd_;
            default:
                return false;
        }
    }
};

// Document-root snapshots keep the bytes of files up to max_file_bytes_, within
// budget_bytes_ for the whole snapshot, so those responses never touch the
// filesystem. Zero for either disables it.
struct static_root_resident_options final {
    std::size_t max_file_bytes_{0};
    std::size_t budget_bytes_{0};

    [[nodiscard]] constexpr bool enabled() const noexcept {
        return max_file_bytes_ != 0 && budget_bytes_ != 0;
    }
};

// Snapshots built by one refresher reuse an unchanged file's encoded bytes
// instead of copying them: the buffer is immutable and shared by every
// snapshot that indexes the same file version.
struct static_root_memory_variant final {
    http_content_coding content_coding_{http_content_coding::identity};
    std::shared_ptr<const std::string> bytes_;
    std::uint64_t modified_token_{0};
    std::time_t modified_seconds_{0};
    std::string etag_;
    std::string last_modified_;
};

struct static_root_entry final {
    std::string relative_path_;
    http_native_path_string_type file_path_;
    std::string content_type_;
    std::uint64_t size_{0};
    http_response_file_identity identity_{http_response_file_identity::unchecked()};
    std::uint64_t modified_token_{0};
    std::time_t modified_seconds_{0};
    std::string etag_;
    std::string last_modified_;
    bool directly_servable_{true};
    std::vector<static_root_memory_variant> memory_variants_;
    // The file's bytes as indexed, when the snapshot keeps it resident. Shared
    // with later snapshots that index the same file version.
    std::shared_ptr<const std::string> resident_bytes_;
};

struct static_root_state final {
    http_native_path_string_type root_;
    static_root_config_storage config_;
    std::vector<static_root_entry> entries_;
    std::vector<std::string> directories_;
    std::uint64_t fingerprint_{0};
    bool has_sidecars_{false};
#if defined(__unix__)
    // The indexed root, opened once: the index snapshot and each request
    // open entries beneath it. Shared with every static_root_entry_open, so
    // an open still running on the blocking pool after its request gave up
    // never uses a descriptor this snapshot already closed.
    std::shared_ptr<const native_file_descriptor> root_directory_;
    // The directory instance root_directory_ names; the index was built from
    // the same one.
    native_directory_identity root_identity_{};
#endif

    explicit static_root_state(static_root_config_storage configured_policy) noexcept
        : config_(std::move(configured_policy)) {}

    static_root_state(const static_root_state&) = delete;
    static_root_state& operator=(const static_root_state&) = delete;
    static_root_state(static_root_state&&) = delete;
    static_root_state& operator=(static_root_state&&) = delete;
};

class static_root_memory_variant_view final {
public:
    [[nodiscard]] http_content_coding content_coding() const noexcept {
        return content_coding_;
    }

    [[nodiscard]] std::string_view bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] std::uint64_t size() const noexcept {
        return static_cast<std::uint64_t>(bytes_.size());
    }

    [[nodiscard]] std::uint64_t modified_token() const noexcept {
        return modified_token_;
    }

    [[nodiscard]] std::time_t modified_seconds() const noexcept {
        return modified_seconds_;
    }

    [[nodiscard]] std::string_view etag() const noexcept {
        return etag_;
    }

    [[nodiscard]] std::string_view last_modified() const noexcept {
        return last_modified_;
    }

private:
    friend class static_root_entry_view;

    static_root_memory_variant_view(http_content_coding content_coding, std::string_view bytes_value,
        std::uint64_t modified_token, std::time_t modified_seconds, std::string_view etag,
        std::string_view last_modified) noexcept
        : content_coding_(content_coding),
          bytes_(bytes_value),
          modified_token_(modified_token),
          modified_seconds_(modified_seconds),
          etag_(etag),
          last_modified_(last_modified) {}

    http_content_coding content_coding_;
    std::string_view bytes_;
    std::uint64_t modified_token_;
    std::time_t modified_seconds_;
    std::string_view etag_;
    std::string_view last_modified_;
};

class static_root_entry_view final {
public:
    [[nodiscard]] const http_native_path_char_type* file_path() const noexcept {
        return file_path_;
    }

    [[nodiscard]] std::string_view content_type() const noexcept {
        return content_type_;
    }

    [[nodiscard]] std::string_view get_cache_control() const noexcept {
        return cache_control_;
    }

    [[nodiscard]] std::string_view etag() const noexcept {
        return etag_;
    }

    [[nodiscard]] std::string_view last_modified() const noexcept {
        return last_modified_;
    }

    [[nodiscard]] std::uint64_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] http_response_file_identity identity() const noexcept {
        return identity_;
    }

    [[nodiscard]] std::uint64_t modified_token() const noexcept {
        return modified_token_;
    }

    [[nodiscard]] std::time_t modified_seconds() const noexcept {
        return modified_seconds_;
    }

    [[nodiscard]] static_range_request_policy range_requests() const noexcept {
        return range_requests_;
    }

    [[nodiscard]] static_response_validator_policy response_validators() const noexcept {
        return response_validators_;
    }

    [[nodiscard]] bool has_memory_variants() const noexcept {
        return memory_variants_ != nullptr && !memory_variants_->empty();
    }

    [[nodiscard]] std::optional<static_root_memory_variant_view> memory_variant(
        http_content_coding coding) const noexcept {
        if (memory_variants_ == nullptr) {
            return std::nullopt;
        }
        for (const auto& variant : *memory_variants_) {
            if (variant.content_coding_ == coding) {
                return static_root_memory_variant_view(variant.content_coding_, *variant.bytes_,
                    variant.modified_token_, variant.modified_seconds_, variant.etag_,
                    variant.last_modified_);
            }
        }
        return std::nullopt;
    }

    // The file's indexed bytes when the snapshot keeps them in memory.
    [[nodiscard]] const std::string* resident_bytes() const noexcept {
        return resident_bytes_;
    }

private:
    friend class static_root_access;

    static_root_entry_view(const http_native_path_char_type* file_path, std::string_view content_type_value,
        std::string_view cache_control_value, std::string_view etag, std::string_view last_modified,
        std::uint64_t size, http_response_file_identity identity, std::uint64_t modified_token,
        std::time_t modified_seconds, static_range_request_policy range_requests,
        static_response_validator_policy response_validators, bool directly_servable,
        const std::vector<static_root_memory_variant>* memory_variants,
        const std::string* resident_bytes) noexcept
        : file_path_(file_path),
          content_type_(content_type_value),
          cache_control_(cache_control_value),
          etag_(etag),
          last_modified_(last_modified),
          size_(size),
          identity_(identity),
          modified_token_(modified_token),
          modified_seconds_(modified_seconds),
          range_requests_(range_requests),
          response_validators_(response_validators),
          directly_servable_(directly_servable),
          memory_variants_(memory_variants),
          resident_bytes_(resident_bytes) {}

    const http_native_path_char_type* file_path_;
    std::string_view content_type_;
    std::string_view cache_control_;
    std::string_view etag_;
    std::string_view last_modified_;
    std::uint64_t size_;
    http_response_file_identity identity_;
    std::uint64_t modified_token_;
    std::time_t modified_seconds_;
    static_range_request_policy range_requests_;
    static_response_validator_policy response_validators_;
    bool directly_servable_;
    const std::vector<static_root_memory_variant>* memory_variants_;
    const std::string* resident_bytes_;
};

// One indexed file's open for a response, owning everything it reads: it runs
// on the blocking pool and may outlive the request -- and the snapshot -- that
// started it. On POSIX it opens beneath the snapshot's retained root directory
// without following a symlink in any component; on Windows it opens the
// canonical path, the identity check against the index being its guard.
class static_root_entry_open final {
public:
    // The opened handle; empty when the open failed or, for a checked open,
    // when the handle no longer holds the indexed identity and size.
    [[nodiscard]] http_response_file_descriptor operator()() const noexcept;

private:
    friend class static_root_access;

#if defined(__unix__)
    static_root_entry_open(std::shared_ptr<const native_file_descriptor> root_directory,
        http_native_path_string_type open_path, http_response_file_identity identity, std::uint64_t size,
        bool checked) noexcept
        : root_directory_(std::move(root_directory)),
          open_path_(std::move(open_path)),
          identity_(identity),
          size_(size),
          checked_(checked) {}
#else
    static_root_entry_open(http_native_path_string_type open_path, http_response_file_identity identity,
        std::uint64_t size, bool checked) noexcept
        : open_path_(std::move(open_path)),
          identity_(identity),
          size_(size),
          checked_(checked) {}
#endif

#if defined(__unix__)
    std::shared_ptr<const native_file_descriptor> root_directory_;
#endif
    // Beneath the root on POSIX, the canonical path on Windows; in the
    // synchronized process resource, since the pool thread may free it.
    http_native_path_string_type open_path_;
    http_response_file_identity identity_;
    std::uint64_t size_;
    bool checked_;
};

class static_root_access final {
public:
    [[nodiscard]] static std::unique_ptr<static_root> make(
        const std::filesystem::path& root, const static_root_config_storage& config);
    [[nodiscard]] static std::unique_ptr<static_root> make(
        const std::filesystem::path& root, static_root_config_storage&& config);
    [[nodiscard]] static static_root_config_storage copy_config(const static_root& root);
    [[nodiscard]] static std::string_view index_file(const static_root& root) noexcept;
    [[nodiscard]] static bool has_directory_index(const static_root& root) noexcept;
    [[nodiscard]] static bool has_sidecars(const static_root& root) noexcept;
    [[nodiscard]] static std::optional<static_root_entry_view> find(
        const static_root& root, std::string_view relative_path) noexcept;
    [[nodiscard]] static std::optional<static_root_entry_view> find_variant(
        const static_root& root, std::string_view relative_path) noexcept;
    [[nodiscard]] static bool is_indexed_directory(
        const static_root& root, std::string_view relative_path) noexcept;
    // Resolves a decoded relative file path the way a static file response
    // does: normalizes it, falls back to the directory index file, and leaves
    // the final relative path in `relative` (backed by `storage` when
    // rewritten). Throws the forbidden http_error for a path outside the root.
    [[nodiscard]] static std::optional<static_root_entry_view> resolve(
        const static_root& root, std::string_view& relative, std::string& storage);
    [[nodiscard]] static std::optional<static_root_entry_view> try_resolve(
        const static_root& root, std::string_view& relative, std::string& storage);
    [[nodiscard]] static std::uint64_t fingerprint(const static_root& root) noexcept;
    [[nodiscard]] static bool same_snapshot(
        const static_root& left, const static_root& right) noexcept;
    static void install_precompressed_variants(static_root& root, const static_root* previous,
        const static_root_precompression_options& options);
    // Reads eligible files into the snapshot, reusing `previous`'s bytes for
    // entries whose indexed metadata is unchanged. A file that cannot be read
    // as indexed stays file-backed and keeps its identity check.
    static void install_resident_bodies(static_root& root, const static_root* previous,
        const static_root_resident_options& options);
    // The open of the entry's file for one response, run by the caller on the
    // blocking pool. `checked` also verifies the opened handle against the
    // indexed identity and size, for a response that sends none of its bytes
    // and so is never checked by the transport.
    [[nodiscard]] static static_root_entry_open entry_open(
        const static_root& root, const static_root_entry_view& entry, bool checked);
};
}  // namespace ruvia::detail

namespace ruvia::detail {

// Above this many entries an index lookup binary-searches instead of scanning.
inline constexpr std::size_t static_root_linear_lookup_limit = 8;

// Looks a key up in an index vector sorted by `projection` (a string_view of
// each entry): a short vector is scanned, a longer one binary-searched.
template <typename entry_type, typename projection_type>
[[nodiscard]] const entry_type* find_static_index_entry(
    const std::vector<entry_type>& entries, std::string_view key, projection_type projection) noexcept {
    if (entries.size() <= static_root_linear_lookup_limit) {
        for (const auto& entry : entries) {
            if (projection(entry) == key) {
                return &entry;
            }
        }
        return nullptr;
    }

    const auto iter = std::ranges::lower_bound(entries, key, std::ranges::less{}, projection);
    if (iter == entries.end() || projection(*iter) != key) {
        return nullptr;
    }
    return &*iter;
}

// Precompressed sidecars (foo.js.br / .zst / .gz) in selection preference
// order: an Accept-Encoding tie resolves br > zstd > gzip. Indexing,
// precompression and representation selection all read this one table.
struct static_precompression_sidecar final {
    std::string_view suffix_;
    http_content_coding content_coding_;
};

inline constexpr std::array<static_precompression_sidecar, 3> static_precompression_sidecars{{
    {".br", http_content_coding::brotli},
    {".zst", http_content_coding::zstd},
    {".gz", http_content_coding::gzip},
}};

[[nodiscard]] constexpr bool is_precompressed_sidecar_extension(std::string_view extension) noexcept {
    for (const auto& sidecar : static_precompression_sidecars) {
        if (sidecar.suffix_ == extension) {
            return true;
        }
    }
    return false;
}

// Orders two indexed snapshots of one file by (mtime, change token). A sidecar
// older than its identity file is never served: presence alone cannot prove
// its decoded bytes still describe the current resource.
struct static_file_version final {
    std::time_t modified_seconds_;
    std::uint64_t modified_token_;

    [[nodiscard]] constexpr bool at_least_as_new_as(const static_file_version& identity) const noexcept {
        if (modified_seconds_ != identity.modified_seconds_) {
            return modified_seconds_ > identity.modified_seconds_;
        }
        return modified_token_ >= identity.modified_token_;
    }
};

// URL index keys and extension policy use UTF-8 on every platform rather than
// the system narrow encoding of filesystem::path::generic_string<char>().
[[nodiscard]] inline std::u8string static_file_utf8_path(
    const std::filesystem::path& path) {
    return path.generic_u8string();
}

[[nodiscard]] inline std::string static_file_utf8_string(
    const std::filesystem::path& path) {
    if constexpr (std::is_same_v<std::filesystem::path::value_type, char>) {
        return path.generic_string();
    } else {
        const auto utf8 = static_file_utf8_path(path);
        return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    }
}
template <typename char_value_type>
[[nodiscard]] inline std::basic_string_view<char_value_type> static_file_extension(
    std::basic_string_view<char_value_type> path) noexcept {
    for (std::size_t i = path.size(); i > 0; --i) {
        const auto c = path[i - 1];
        if (c == static_cast<char_value_type>('/') || c == static_cast<char_value_type>('\\')) {
            return {};
        }
        if (c == static_cast<char_value_type>('.')) {
            return path.substr(i - 1);
        }
    }
    return {};
}

[[nodiscard]] inline std::string lower_static_file_extension_from_utf8(
    std::string_view utf8_path) {
    const auto source_value = static_file_extension(utf8_path);
    if (source_value.empty()) {
        return {};
    }
    std::string extension;
    extension.reserve(source_value.size());
    for (const char c : source_value) {
        auto out = c;
        if (out >= 'A' && out <= 'Z') {
            out = static_cast<char>(out + ('a' - 'A'));
        }
        extension.push_back(out);
    }
    return extension;
}
[[nodiscard]] inline std::string_view guess_static_file_content_type_from_extension(
    std::string_view lower_extension) noexcept {
    switch (lower_extension.size()) {
        case 3U:
            if (lower_extension == ".js") {
                return "text/javascript; charset=utf-8";
            }
            break;
        case 4U:
            if (lower_extension == ".htm") {
                return "text/html; charset=utf-8";
            }
            if (lower_extension == ".css") {
                return "text/css; charset=utf-8";
            }
            if (lower_extension == ".mjs") {
                return "text/javascript; charset=utf-8";
            }
            if (lower_extension == ".txt" || lower_extension == ".log") {
                return "text/plain; charset=utf-8";
            }
            if (lower_extension == ".png") {
                return "image/png";
            }
            if (lower_extension == ".jpg") {
                return "image/jpeg";
            }
            if (lower_extension == ".gif") {
                return "image/gif";
            }
            if (lower_extension == ".svg") {
                return "image/svg+xml";
            }
            if (lower_extension == ".bmp") {
                return "image/bmp";
            }
            if (lower_extension == ".ico" || lower_extension == ".cur") {
                return "image/vnd.microsoft.icon";
            }
            if (lower_extension == ".ttf") {
                return "font/ttf";
            }
            if (lower_extension == ".otf") {
                return "font/otf";
            }
            if (lower_extension == ".eot") {
                return "application/vnd.ms-fontobject";
            }
            if (lower_extension == ".xml" || lower_extension == ".xsl") {
                return "text/xml; charset=utf-8";
            }
            if (lower_extension == ".map") {
                return "application/json; charset=utf-8";
            }
            break;
        case 5U:
            if (lower_extension == ".html") {
                return "text/html; charset=utf-8";
            }
            if (lower_extension == ".json") {
                return "application/json; charset=utf-8";
            }
            if (lower_extension == ".jpeg") {
                return "image/jpeg";
            }
            if (lower_extension == ".wasm") {
                return "application/wasm";
            }
            if (lower_extension == ".apng") {
                return "image/apng";
            }
            if (lower_extension == ".avif") {
                return "image/avif";
            }
            if (lower_extension == ".webp") {
                return "image/webp";
            }
            if (lower_extension == ".woff") {
                return "font/woff";
            }
            break;
        case 6U:
            if (lower_extension == ".woff2") {
                return "font/woff2";
            }
            break;
        case 12U:
            if (lower_extension == ".webmanifest") {
                return "application/manifest+json; charset=utf-8";
            }
            break;
        default:
            break;
    }
    return "application/octet-stream";
}

template <typename char_value_type>
[[nodiscard]] inline std::string_view guess_static_file_content_type_from_path_view(
    std::basic_string_view<char_value_type> path) noexcept {
    const auto extension = static_file_extension(path);
    std::array<char, 12> lower;
    if (extension.size() < 3U || extension.size() > lower.size()) {
        return "application/octet-stream";
    }
    for (std::size_t i = 0; i < extension.size(); ++i) {
        const auto c = extension[i];
        if (c < static_cast<char_value_type>(0) || c > static_cast<char_value_type>(0x7f)) {
            return "application/octet-stream";
        }
        auto out = static_cast<char>(c);
        if (out >= 'A' && out <= 'Z') {
            out = static_cast<char>(out + ('a' - 'A'));
        }
        lower[i] = out;
    }
    return guess_static_file_content_type_from_extension(
        std::string_view(lower.data(), extension.size()));
}

// Strong validator for one file snapshot: a quoted 64-bit FNV-1a digest of the
// size, modification token, file identity and (for a stored non-identity
// representation) content coding, so any change to one of them yields a new
// ETag. Hashing keeps the field short and does not disclose the volume and
// file index the identity words carry.
inline constexpr std::size_t static_file_etag_size = 2U + 2U * sizeof(std::uint64_t);

[[nodiscard]] inline std::array<char, static_file_etag_size> format_static_file_snapshot_etag(
    std::uint64_t size, std::uint64_t modified_token, http_response_file_identity identity,
    http_content_coding coding = http_content_coding::identity) noexcept {
    auto hash = fnv1a_accumulate_integer(fnv1a_offset_basis, size);
    hash = fnv1a_accumulate_integer(hash, modified_token);
    for (const auto word : identity.words()) {
        hash = fnv1a_accumulate_integer(hash, word);
    }
    if (coding != http_content_coding::identity) {
        hash = fnv1a_accumulate(hash, http_content_coding_token(coding));
    }
    std::array<unsigned char, sizeof(hash)> digest{};
    for (std::size_t i = 0; i < digest.size(); ++i) {
        digest[i] = static_cast<unsigned char>(hash >> ((digest.size() - 1 - i) * 8U));
    }
    std::array<char, static_file_etag_size> output{};
    output.front() = '"';
    http_encode_lower_hex(output.data() + 1, digest);
    output.back() = '"';
    return output;
}

[[nodiscard]] inline std::string make_static_file_snapshot_etag(
    std::uint64_t size, std::uint64_t modified_token,
    http_response_file_identity identity, http_content_coding coding = http_content_coding::identity) {
    const auto formatted = format_static_file_snapshot_etag(size, modified_token, identity, coding);
    return std::string(formatted.data(), formatted.size());
}

}  // namespace ruvia::detail

namespace ruvia::detail {

[[nodiscard]] inline bool static_file_extensions_equivalent(
    std::string_view left, std::string_view right) noexcept {
    if (left.starts_with('.')) {
        left.remove_prefix(1);
    }
    if (right.starts_with('.')) {
        right.remove_prefix(1);
    }
    return http_ascii_equals_ignore_case(left, right);
}

[[nodiscard]] inline bool static_root_serves_dotfiles(static_dotfile_policy policy) {
    switch (policy) {
        case static_dotfile_policy::deny:
            return false;
        case static_dotfile_policy::serve:
            return true;
        default:
            throw std::invalid_argument("invalid static dotfile policy");
    }
}

inline void validate_static_root_options(const static_root_options& options) {
    (void)static_root_serves_dotfiles(options.dotfiles_);
    switch (options.range_requests_) {
        case static_range_request_policy::ignore:
        case static_range_request_policy::honor:
            break;
        default:
            throw std::invalid_argument("invalid static range request policy");
    }
    switch (options.response_validators_) {
        case static_response_validator_policy::omit:
        case static_response_validator_policy::emit:
            break;
        default:
            throw std::invalid_argument("invalid static response validator policy");
    }
    if (!ruvia::is_valid_http_header_value(options.cache_control_.view()) ||
        (!options.default_content_type_.view().empty() &&
            !ruvia::is_valid_http_content_type_field_value(options.default_content_type_.view()))) {
        throw std::invalid_argument("invalid static file header value");
    }
    for (std::size_t i = 0; i < options.mime_types_.size(); ++i) {
        const auto& mime = options.mime_types_[i];
        if (!ruvia::detail::is_valid_static_file_extension(mime.extension_.view()) ||
            mime.content_type_.view().empty() ||
            !ruvia::is_valid_http_content_type_field_value(mime.content_type_.view())) {
            throw std::invalid_argument("invalid static file mime type");
        }
        for (std::size_t previous = 0; previous < i; ++previous) {
            if (static_file_extensions_equivalent(
                    options.mime_types_[previous].extension_.view(), mime.extension_.view())) {
                throw std::invalid_argument("duplicate static file mime extension");
            }
        }
    }
    switch (options.file_types_.kind_) {
        case static_file_type_policy::kind::defaults:
        case static_file_type_policy::kind::all:
            if (!options.file_types_.extensions_.empty()) {
                throw std::invalid_argument("static file type extensions require only mode");
            }
            break;
        case static_file_type_policy::kind::only:
            if (options.file_types_.extensions_.empty()) {
                throw std::invalid_argument("static file type allow-list must not be empty");
            }
            for (const auto& extension : options.file_types_.extensions_) {
                if (!is_valid_static_file_extension(extension)) {
                    throw std::invalid_argument("invalid static file type");
                }
            }
            break;
        default:
            throw std::invalid_argument("invalid static file type mode");
    }
    const auto index_file = options.index_file_.view();
    if ((index_file.find('/') != std::string_view::npos) || (index_file.find('\\') != std::string_view::npos) || index_file == "." ||
        index_file == "..") {
        throw std::invalid_argument("invalid static file index name");
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {
enum class static_file_selection_mode : std::uint8_t {
    identity_only,
    precompressed,
};

}  // namespace ruvia::detail

namespace ruvia {

// Which bytes a static route actually serves: the identity file, a
// precompressed sidecar, or a refresh-built in-memory variant the client
// accepts.
class static_file_representation final {
public:
    static_file_representation(
        detail::static_root_entry_view entry_value, http_content_coding content_coding) noexcept
        : entry_(entry_value),
          content_coding_(content_coding) {}

    static_file_representation(detail::static_root_entry_view entry_value,
        detail::static_root_memory_variant_view memory_variant, http_content_coding content_coding) noexcept
        : entry_(entry_value),
          memory_variant_(memory_variant),
          content_coding_(content_coding) {}

    [[nodiscard]] const detail::static_root_entry_view& entry() const& noexcept RUVIA_LIFETIMEBOUND {
        return entry_;
    }
    const detail::static_root_entry_view& entry() const&& = delete;

    [[nodiscard]] const detail::static_root_memory_variant_view* memory_variant() const& noexcept RUVIA_LIFETIMEBOUND {
        return memory_variant_.has_value() ? &*memory_variant_ : nullptr;
    }
    const detail::static_root_memory_variant_view* memory_variant() const&& = delete;
    [[nodiscard]] http_content_coding content_coding() const noexcept {
        return content_coding_;
    }

private:
    detail::static_root_entry_view entry_;
    std::optional<detail::static_root_memory_variant_view> memory_variant_;
    http_content_coding content_coding_;
};

// Selects the best precompressed sidecar (foo.js.br / .gz / .zst) or
// refresh-built in-memory variant the request's Accept-Encoding selection
// accepts -- highest q-value wins, ties resolve br > zstd > gzip. The served
// bytes are the variant's, so its size/etag/modified describe the wire
// representation; the caller keeps the original Content-Type. Index lookups
// only (no per-request filesystem stat). nullopt means no acceptable
// representation exists.
[[nodiscard]] std::optional<static_file_representation> select_static_file_representation(
    const static_root& root, std::string_view relative,
    const http_response_coding_selection& selection,
    detail::static_root_entry_view identity);

}  // namespace ruvia

namespace ruvia {

class http_client;
class http_client_ref;
class stop_token;
class operation_scope;

#ifdef RUVIA_ENABLE_DATABASE
class db_session;
#endif

#ifdef RUVIA_ENABLE_REDIS
class redis_session;
#endif
namespace detail {

class borrowed_clients;
class db_registry;
struct request_io_metrics;
class http_client_registry;
class redis_registry;

// A copyable view of the complete client registry set owned by one worker, or
// of the clients a router borrows for its dispatches. The only other state is
// detached: partial registry graphs are not representable. Detached views are
// only placeholders for contexts built without worker capabilities; an
// attached view never outlives its worker, which joins all posted work before
// destroying its registries, nor a router's dispatch.
class worker_client_registry_view final {
public:
    [[nodiscard]] static constexpr worker_client_registry_view detached() noexcept {
        return worker_client_registry_view(nullptr);
    }

    constexpr worker_client_registry_view(http_client_registry& http_clients
#ifdef RUVIA_ENABLE_DATABASE
        ,
        db_registry& databases
#endif
#ifdef RUVIA_ENABLE_REDIS
        ,
        redis_registry& redis
#endif
        ) noexcept
        : http_clients_(&http_clients)
#ifdef RUVIA_ENABLE_DATABASE
          ,
          databases_(&databases)
#endif
#ifdef RUVIA_ENABLE_REDIS
          ,
          redis_(&redis)
#endif
    {
    }

    explicit constexpr worker_client_registry_view(const borrowed_clients& borrowed) noexcept
        : borrowed_(&borrowed) {}

    [[nodiscard]] constexpr bool attached() const noexcept {
        return http_clients_ != nullptr || borrowed_ != nullptr;
    }

    // `io_metrics` is the request's counter block, or null outside a request.
#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] db_session db(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics,
        const stop_token& stop_token) const;
    [[nodiscard]] db_session db(std::string_view alias, ::ruvia::operation_scope& operation_scope,
        request_io_metrics* io_metrics, const stop_token& stop_token) const;
#endif

#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] redis_session redis(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics,
        const stop_token& stop_token) const;
    [[nodiscard]] redis_session redis(std::string_view alias, ::ruvia::operation_scope& operation_scope,
        request_io_metrics* io_metrics, const stop_token& stop_token) const;
#endif

    [[nodiscard]] http_client_ref client(
        ::ruvia::operation_scope& operation_scope, const stop_token& stop_token) const;
    [[nodiscard]] http_client_ref client(std::string_view alias,
        ::ruvia::operation_scope& operation_scope, const stop_token& stop_token) const;

private:
    explicit constexpr worker_client_registry_view(std::nullptr_t) noexcept {}

    [[nodiscard]] const http_client& borrowed_http_clients(std::string_view alias) const;

    const borrowed_clients* borrowed_{nullptr};
    http_client_registry* http_clients_{nullptr};
#ifdef RUVIA_ENABLE_DATABASE
    db_registry* databases_{nullptr};
#endif
#ifdef RUVIA_ENABLE_REDIS
    redis_registry* redis_{nullptr};
#endif
};

}  // namespace detail
}  // namespace ruvia

namespace ruvia {

class body_reader;
class response_stream_writer;
class websocket;
class http_tunnel;

namespace detail {

class request_body_loader;
class context_request_body_source;
class context_response_output;

// A context always has exactly one request-body source. The buffered alternative
// means the complete bytes are already exposed by http_request; the other two
// borrow non-null runtime facades for the duration of dispatch.
class context_buffered_request_body_source final {
private:
    friend class context_request_body_source;
    constexpr context_buffered_request_body_source() noexcept = default;
};

class context_lazy_request_body_source final {
public:
    [[nodiscard]] constexpr request_body_loader& loader() const noexcept {
        return *loader_;
    }

private:
    friend class context_request_body_source;

    explicit constexpr context_lazy_request_body_source(request_body_loader& loader) noexcept
        : loader_(&loader) {}

    request_body_loader* loader_;
};

class context_streaming_request_body_source final {
public:
    [[nodiscard]] constexpr body_reader& reader() const noexcept {
        return *reader_;
    }

private:
    friend class context_request_body_source;

    explicit constexpr context_streaming_request_body_source(body_reader& reader_value) noexcept
        : reader_(&reader_value) {}

    body_reader* reader_;
};

class context_request_body_source final {
public:
    constexpr context_request_body_source() noexcept
        : value_(context_buffered_request_body_source{}) {}

    [[nodiscard]] static constexpr context_request_body_source lazy(
        request_body_loader& loader) noexcept {
        return context_request_body_source(context_lazy_request_body_source(loader));
    }

    [[nodiscard]] static constexpr context_request_body_source streaming(body_reader& reader_value) noexcept {
        return context_request_body_source(context_streaming_request_body_source(reader_value));
    }

    [[nodiscard]] constexpr const context_lazy_request_body_source* lazy() const& noexcept {
        return std::get_if<context_lazy_request_body_source>(&value_);
    }
    [[nodiscard]] constexpr const context_lazy_request_body_source* lazy() const&& = delete;

    [[nodiscard]] constexpr const context_streaming_request_body_source* streaming() const& noexcept {
        return std::get_if<context_streaming_request_body_source>(&value_);
    }
    [[nodiscard]] constexpr const context_streaming_request_body_source* streaming() const&& = delete;

private:
    using value_type = std::variant<context_buffered_request_body_source, context_lazy_request_body_source,
        context_streaming_request_body_source>;

    template <typename source>
    explicit constexpr context_request_body_source(source source_value) noexcept
        : value_(source_value) {}

    value_type value_;
};

// A context likewise has exactly one response output. The buffered alternative
// uses the ordinary http_response return path; long-lived outputs borrow one
// non-null runtime facade and cannot coexist in the same context. Every
// alternative but buffered and an uncommitted response stream means the
// response head is on the wire and no HTTP response can follow.
class context_buffered_response_output final {
private:
    friend class context_response_output;
    constexpr context_buffered_response_output() noexcept = default;
};

class context_response_stream_output final {
public:
    [[nodiscard]] constexpr response_stream_writer& writer() const noexcept {
        return *writer_;
    }

private:
    friend class context_response_output;

    explicit constexpr context_response_stream_output(response_stream_writer& writer) noexcept
        : writer_(&writer) {}

    response_stream_writer* writer_;
};

class context_websocket_output final {
public:
    [[nodiscard]] constexpr websocket& get_websocket() const noexcept {
        return *websocket_;
    }

private:
    friend class context_response_output;

    explicit constexpr context_websocket_output(websocket& websocket_value) noexcept
        : websocket_(&websocket_value) {}

    websocket* websocket_;
};

class context_tunnel_output final {
public:
    [[nodiscard]] constexpr http_tunnel& tunnel() const noexcept {
        return *tunnel_;
    }

private:
    friend class context_response_output;
    explicit constexpr context_tunnel_output(http_tunnel& tunnel) noexcept
        : tunnel_(&tunnel) {}
    http_tunnel* tunnel_;
};

// The upgrade or CONNECT handshake head was handed to the transport. Holds
// while no websocket/tunnel facade is bound -- before the session binds one and
// after it restores this output -- so a buffered error built by middleware can
// never be mistaken for a sendable response.
class context_upgraded_response_output final {
private:
    friend class context_response_output;
    constexpr context_upgraded_response_output() noexcept = default;
};

class context_response_output final {
public:
    constexpr context_response_output() noexcept
        : value_(context_buffered_response_output{}) {}

    [[nodiscard]] static constexpr context_response_output response_stream(
        response_stream_writer& writer) noexcept {
        return context_response_output(context_response_stream_output(writer));
    }

    [[nodiscard]] static constexpr context_response_output websocket_value(websocket& websocket_value) noexcept {
        return context_response_output(context_websocket_output(websocket_value));
    }

    [[nodiscard]] static constexpr context_response_output tunnel(http_tunnel& tunnel) noexcept {
        return context_response_output(context_tunnel_output(tunnel));
    }

    [[nodiscard]] static constexpr context_response_output upgraded() noexcept {
        return context_response_output(context_upgraded_response_output{});
    }

    // Whether the response head already reached the transport.
    [[nodiscard]] bool committed() const noexcept;

    [[nodiscard]] constexpr const context_tunnel_output* tunnel() const& noexcept {
        return std::get_if<context_tunnel_output>(&value_);
    }
    const context_tunnel_output* tunnel() const&& = delete;
    [[nodiscard]] constexpr const context_buffered_response_output* buffered() const& noexcept {
        return std::get_if<context_buffered_response_output>(&value_);
    }
    [[nodiscard]] constexpr const context_buffered_response_output* buffered() const&& = delete;

    [[nodiscard]] constexpr const context_response_stream_output* response_stream() const& noexcept {
        return std::get_if<context_response_stream_output>(&value_);
    }
    [[nodiscard]] constexpr const context_response_stream_output* response_stream() const&& = delete;

    [[nodiscard]] constexpr const context_websocket_output* get_websocket() const& noexcept {
        return std::get_if<context_websocket_output>(&value_);
    }
    [[nodiscard]] constexpr const context_websocket_output* get_websocket() const&& = delete;

private:
    using value_type = std::variant<context_buffered_response_output, context_response_stream_output,
        context_websocket_output, context_tunnel_output, context_upgraded_response_output>;

    template <typename output_type>
    explicit constexpr context_response_output(output_type output) noexcept
        : value_(output) {}

    value_type value_;
};

}  // namespace detail
}  // namespace ruvia

namespace ruvia {

class http_request;
class http_request_trailers;
class env;
}  // namespace ruvia

namespace ruvia::detail {

class rate_limiter;
class http_interim_response_output;
class inbound_buffer_account;
class route_table;
struct connection_failure_sink;
class tls_served_certificate;
class worker_state_registry;
class borrowed_clients;

// Borrows the request's connection.
class http_push_output final {
public:
    using submit_type = bool (*)(void*, http_push_request_view);
    http_push_output(void* target, submit_type submit) noexcept
        : target_(target),
          submit_(submit) {}
    // Header-list bounds are enforced by the HTTP/2 submit path.
    [[nodiscard]] bool push(http_push_request_view request) {
        return submit_(target_, request);
    }

private:
    void* target_;
    submit_type submit_;
};

// Request-scoped typed access to the owning connection's advertisement lane.
class http_connection_advertisement_output final {
public:
    using origins_type = void (*)(void*, std::span<const std::string_view>);
    using alternative_service_type = void (*)(void*, std::string_view);
    http_connection_advertisement_output(
        void* target, origins_type origins, alternative_service_type service) noexcept
        : target_(target),
          origins_(origins),
          service_(service) {}
    void advertise_origins(std::span<const std::string_view> origins);
    void advertise_alternative_service(std::string_view value);

private:
    void* target_;
    origins_type origins_;
    alternative_service_type service_;
};

struct context_worker_services final {
    std::reference_wrapper<const worker_ref> worker_;
    worker_client_registry_view clients_{worker_client_registry_view::detached()};
    rate_limiter* rate_limiter_{};
    const ruvia::env* env_{};
    std::size_t max_decoded_body_bytes_{default_max_buffered_body_bytes};
    const worker_state_registry* states_{};
    bool precompressed_static_files_{};
    // Bounds each blocking-pool wait of a file response's open, as the body
    // reads the transport waits on are bounded: the server write_timeout_.
    std::optional<timeout_duration> file_io_timeout_{};
    // Where a file response's real faults (no blocking pool, a stalled open)
    // are reported; the worker's connection failure outlet, never null.
    const connection_failure_sink* connection_failure_{};
    const http_client_ip_extractor* trusted_proxies_{};
};

struct context_connection_services final {
    conn_info info_;
    inbound_buffer_account* inbound_buffers_{};
    const route_table* routes_{};
    rate_limiter* route_rate_limiter_{};
    // Whether the peer is a configured trusted proxy. Decided once per
    // connection: the peer address never changes, so its IP parse and block
    // match are not repeated per request.
    bool peer_trusted_{false};
    // The certificate a TLS connection presented; null for cleartext.
    tls_served_certificate* served_certificate_{};
    // The Alt-Svc field value (RFC 7838 §3) this connection's listener
    // advertises on every response; empty when it advertises none.
    std::string_view alternative_service_{};
    // Set for a listener_dispatch::request_stream connection: every request
    // goes to this callback instead of the routes.
    request_stream_callback_ref request_stream_{};
    connection_lifecycle* lifecycle_{};
    // A router's borrowed clients and worker states, bound for its dispatches
    // like routes_; null keeps the worker's own.
    const borrowed_clients* router_clients_{};
    const worker_state_registry* router_states_{};
};

// The code request_stream::reset() chose; monostate selects the protocol's
// cancellation code (HTTP/2 CANCEL, H3_REQUEST_CANCELLED).
using request_reset_code = std::variant<std::monostate, http2_error_code, http3_error_code>;

struct request_stream_control final {
    callback_ref<void(request_reset_code)> reset_;
    bool reset_requested_{false};
};

// Borrowed protocol channels outlive every context for this request.
struct context_request_channels final {
    http_interim_response_output* interim_output_{};
    http_connection_advertisement_output* connection_advertisements_{};
    http_push_output* push_output_{};
    const std::optional<http_priority>* priority_update_{};
};

struct context_request_services final {
    std::reference_wrapper<const stop_token> stop_token_;
    context_request_body_source body_source_{};
    const http_request_trailers* trailers_{};
    context_request_channels channels_{};
    context_response_output response_output_{};
    std::size_t dispatch_depth_{};
    // The matched route's declared body_limit; 0 when it declares none.
    std::size_t route_body_limit_{};
    request_io_metrics* io_metrics_{nullptr};
    // Admission's Accept-Encoding selection for this request; empty for a
    // context created outside admission.
    std::optional<http_response_coding_selection> response_coding_{};
    // The arrival instant the request's owner pinned for rate-limit decisions
    // (test_app); empty when those decisions read the steady clock.
    std::optional<std::chrono::steady_clock::time_point> pinned_arrival_{};
    bool early_data_{false};
    request_body_state body_state_{};
    request_stream_control* stream_control_{};
};

class context_services final {
public:
    context_services() = delete;

    context_services(const context_worker_services& worker_services, const stop_token& stop_token_value)
        : worker_services_(&worker_services),
          request_services_{.stop_token_ = stop_token_value} {
        if (!worker_services_->worker_.get().valid()) {
            throw std::invalid_argument("context services require a valid worker");
        }
    }
    context_services(context_worker_services&&, const stop_token&) = delete;
    context_services(const context_worker_services&, stop_token&&) = delete;

    [[nodiscard]] constexpr worker_client_registry_view client_registries() const noexcept {
        return connection_services_.router_clients_ != nullptr
                   ? worker_client_registry_view(*connection_services_.router_clients_)
                   : worker_services_->clients_;
    }

    // Either argument may be null to keep the worker's own capability.
    void bind_router_capabilities(const borrowed_clients* clients, const worker_state_registry* states) noexcept {
        connection_services_.router_clients_ = clients;
        connection_services_.router_states_ = states;
    }

    [[nodiscard]] detail::rate_limiter* rate_limiter() const noexcept {
        return connection_services_.route_rate_limiter_ != nullptr
                   ? connection_services_.route_rate_limiter_
                   : worker_services_->rate_limiter_;
    }

    [[nodiscard]] const ruvia::env* env() const noexcept {
        return worker_services_->env_;
    }

    [[nodiscard]] inbound_buffer_account* inbound_buffers() const noexcept {
        return connection_services_.inbound_buffers_;
    }

    [[nodiscard]] context_services with_inbound_buffers(inbound_buffer_account& account) const noexcept {
        auto services = *this;
        services.connection_services_.inbound_buffers_ = &account;
        return services;
    }
    context_services with_inbound_buffers(inbound_buffer_account&&) const = delete;
    [[nodiscard]] constexpr std::size_t max_decoded_body_bytes() const noexcept {
        return worker_services_->max_decoded_body_bytes_;
    }

    // Ceiling for this request's decoded Content-Encoding representation. A
    // route's body_limit bounds what the route reads, not only the wire
    // bytes: a small compressed body must not inflate past it up to the
    // worker-wide bound.
    [[nodiscard]] constexpr std::size_t max_decoded_request_body_bytes() const noexcept {
        const auto route_limit = request_services_.route_body_limit_;
        return route_limit != 0 && route_limit < worker_services_->max_decoded_body_bytes_
                   ? route_limit
                   : worker_services_->max_decoded_body_bytes_;
    }

    void bind_route_body_limit(std::size_t limit) noexcept {
        request_services_.route_body_limit_ = limit;
    }

    [[nodiscard]] request_io_metrics* io_metrics() const noexcept {
        return request_services_.io_metrics_;
    }

    void bind_io_metrics(request_io_metrics* metrics) noexcept {
        request_services_.io_metrics_ = metrics;
    }

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_services_->worker_.get();
    }

    [[nodiscard]] const stop_token& get_stop_token() const noexcept {
        return request_services_.stop_token_.get();
    }

    // A transport that can observe the client leaving binds a request stop
    // that also fires then, on a request-local copy; `token` must observe the previous one and
    // outlive every context dispatched with these services.
    void bind_stop_token(const stop_token& token) noexcept {
        request_services_.stop_token_ = token;
    }
    void bind_stop_token(stop_token&&) = delete;

    void bind_early_data(bool value) noexcept {
        request_services_.early_data_ = value;
    }
    [[nodiscard]] bool early_data() const noexcept {
        return request_services_.early_data_;
    }

    void bind_body_state(request_body_state state) noexcept {
        request_services_.body_state_ = state;
    }
    [[nodiscard]] request_body_state body_state() const noexcept {
        return request_services_.body_state_;
    }
    void bind_stream_control(request_stream_control& control) noexcept {
        request_services_.stream_control_ = &control;
    }
    [[nodiscard]] bool request_reset() const noexcept {
        const auto* control = request_services_.stream_control_;
        return control != nullptr && control->reset_requested_;
    }
    void reset_request(request_reset_code error) const {
        if (auto* control = request_services_.stream_control_) {
            if (!control->reset_requested_) {
                control->reset_requested_ = true;
                control->reset_(error);
            }
        } else {
            abort_connection();
        }
    }

    [[nodiscard]] const route_table* routes() const noexcept {
        return connection_services_.routes_;
    }

    // Every dispatch path hands the context the table it may re-enter, which
    // also selects the not-found handler for the request path when the
    // handler asks for one. The route table is server-owned and outlives every
    // dispatched request.
    void bind_routes(const route_table& routes_value) noexcept {
        connection_services_.routes_ = &routes_value;
    }
    void bind_routes(route_table&&) = delete;
    void bind_route_rate_limiter(detail::rate_limiter& limiter) noexcept {
        connection_services_.route_rate_limiter_ = &limiter;
    }

    [[nodiscard]] constexpr const context_request_body_source& request_body_source() const noexcept {
        return request_services_.body_source_;
    }

    // Bind on a request-local copy; the worker and connection views stay shared.
    void bind_streaming_request_body(body_reader& value) noexcept {
        streaming_access::bind_worker(value, worker());
        request_services_.body_source_ = context_request_body_source::streaming(value);
    }
    void bind_lazy_request_body(request_body_loader& value) noexcept {
        request_services_.body_source_ = context_request_body_source::lazy(value);
    }

    [[nodiscard]] const http_request_trailers* request_trailers() const noexcept {
        return request_services_.trailers_;
    }

    // Bound by whichever layer owns the request body, alongside its source.
    void bind_request_trailers(const http_request_trailers& trailers) noexcept {
        request_services_.trailers_ = &trailers;
    }
    void bind_request_trailers(http_request_trailers&&) = delete;

    [[nodiscard]] http_interim_response_output* interim_output() const noexcept {
        return request_services_.channels_.interim_output_;
    }

    [[nodiscard]] http_connection_advertisement_output* connection_advertisements() const noexcept {
        return request_services_.channels_.connection_advertisements_;
    }

    [[nodiscard]] http_push_output* push_output() const noexcept {
        return request_services_.channels_.push_output_;
    }

    [[nodiscard]] const std::optional<http_priority>* request_priority_update() const noexcept {
        return request_services_.channels_.priority_update_;
    }

    void bind_request_channels(const context_request_channels& channels) noexcept {
        request_services_.channels_ = channels;
    }

    void bind_interim_output(http_interim_response_output& output) noexcept {
        request_services_.channels_.interim_output_ = &output;
    }

    [[nodiscard]] constexpr context_response_output& response_output() noexcept {
        return request_services_.response_output_;
    }

    [[nodiscard]] constexpr const context_response_output& response_output() const noexcept {
        return request_services_.response_output_;
    }

    void bind_response_stream(response_stream_writer& value) noexcept {
        streaming_access::bind_worker(value, worker());
        request_services_.response_output_ = context_response_output::response_stream(value);
    }

    [[nodiscard]] std::size_t dispatch_depth() const noexcept {
        return request_services_.dispatch_depth_;
    }

    // Subrequests inherit the parent connection identity, one dispatch level deeper.
    [[nodiscard]] context_services for_subrequest(const stop_token& stop_token_value) const {
        context_services services(*worker_services_, stop_token_value);
        services.connection_services_.info_ = connection_services_.info_;
        services.connection_services_.inbound_buffers_ = connection_services_.inbound_buffers_;
        services.connection_services_.routes_ = connection_services_.routes_;
        services.connection_services_.route_rate_limiter_ = connection_services_.route_rate_limiter_;
        services.connection_services_.lifecycle_ = connection_services_.lifecycle_;
        services.connection_services_.router_clients_ = connection_services_.router_clients_;
        services.connection_services_.router_states_ = connection_services_.router_states_;
        services.request_services_.dispatch_depth_ = request_services_.dispatch_depth_ + 1;
        services.request_services_.io_metrics_ = request_services_.io_metrics_;
        services.request_services_.early_data_ = request_services_.early_data_;
        services.request_services_.pinned_arrival_ = request_services_.pinned_arrival_;
        return services;
    }
    context_services for_subrequest(stop_token&&) const = delete;

    // Server-owned static-file policy. When response compression is enabled,
    // static_file() may negotiate an indexed precompressed variant. It never
    // performs request-time file compression.
    [[nodiscard]] constexpr bool precompressed_static_files() const noexcept {
        return worker_services_->precompressed_static_files_;
    }

    [[nodiscard]] constexpr std::optional<timeout_duration> file_io_timeout() const noexcept {
        return worker_services_->file_io_timeout_;
    }

    [[nodiscard]] const connection_failure_sink& connection_failure() const noexcept {
        return *worker_services_->connection_failure_;
    }

    // The request's Accept-Encoding selection as admission folded it, or
    // null for a context created outside admission. A failed negotiation is a
    // selection whose acceptable() is false.
    [[nodiscard]] const http_response_coding_selection* response_coding() const noexcept {
        return request_services_.response_coding_ ? &*request_services_.response_coding_ : nullptr;
    }

    void bind_response_coding(const http_response_coding_selection& selection) noexcept {
        request_services_.response_coding_ = selection;
    }

    [[nodiscard]] const std::optional<std::chrono::steady_clock::time_point>& pinned_arrival() const noexcept {
        return request_services_.pinned_arrival_;
    }

    void pin_arrival(std::chrono::steady_clock::time_point arrival) noexcept {
        request_services_.pinned_arrival_ = arrival;
    }

    [[nodiscard]] const worker_state_registry* worker_states() const noexcept {
        return connection_services_.router_states_ != nullptr ? connection_services_.router_states_
                                                              : worker_services_->states_;
    }

    [[nodiscard]] constexpr const conn_info& get_conn_info() const noexcept {
        return connection_services_.info_;
    }

    // The connection metadata one request sees. Identical to get_conn_info() unless
    // the peer is a configured trusted proxy, in which case its forwarding
    // headers name the client. Resolved per request rather than per connection:
    // each keep-alive request may carry different forwarding headers. Admission
    // resolves it once and binds the result with bind_conn_info, so every
    // context created for the request reads get_conn_info().
    [[nodiscard]] conn_info resolve_conn_info(const http_request& request) const noexcept;

    void bind_conn_info(const conn_info& resolved) noexcept {
        connection_services_.info_ = resolved;
    }

    // The RFC 9110 §7.4 facts of the transport connection, never of a
    // forwarded hop: whether its peer is a trusted gateway, and the
    // certificate it presented (null for cleartext and for subrequests).
    [[nodiscard]] constexpr bool peer_trusted() const noexcept {
        return connection_services_.peer_trusted_;
    }

    [[nodiscard]] constexpr tls_served_certificate* served_certificate() const noexcept {
        return connection_services_.served_certificate_;
    }
    [[nodiscard]] connection_lifecycle* connection_lifecycle_value() const noexcept {
        return connection_services_.lifecycle_;
    }
    void note_connection_request() const noexcept {
        if (auto* lifecycle = connection_services_.lifecycle_) {
            lifecycle->note_request();
        }
    }
    void set_connection_close_reason(connection_close_reason reason) const noexcept {
        if (auto* lifecycle = connection_services_.lifecycle_) {
            lifecycle->set_reason(reason);
        }
    }
    [[nodiscard]] std::shared_ptr<void> connection_state() const noexcept {
        const auto* lifecycle = connection_services_.lifecycle_;
        return lifecycle == nullptr ? std::shared_ptr<void>{} : lifecycle->state();
    }
    void abort_connection() const noexcept {
        if (auto* lifecycle = connection_services_.lifecycle_) {
            lifecycle->abort();
        }
    }
    [[nodiscard]] bool connection_aborted() const noexcept {
        const auto* lifecycle = connection_services_.lifecycle_;
        return lifecycle != nullptr && lifecycle->aborted();
    }
    [[nodiscard]] std::string_view remote_address() const noexcept {
        return connection_services_.info_.remote_text_;
    }

    [[nodiscard]] context_services with_connection_lifecycle(connection_lifecycle& lifecycle) const noexcept {
        auto services = *this;
        services.connection_services_.lifecycle_ = &lifecycle;
        services.connection_services_.info_.local_ = lifecycle.local_endpoint();
        services.connection_services_.info_.remote_ = lifecycle.remote_endpoint();
        if (!services.connection_services_.info_.via_trusted_proxy_) {
            services.connection_services_.info_.client_ = lifecycle.remote_endpoint();
        }
        return services;
    }

    // Views borrow connection-owned storage and remain valid for every context
    // created while that connection is dispatched. `connection_id` is the
    // worker's conn_info::connection_id() for the connection, zero without one.
    [[nodiscard]] context_services with_plain_transport(
        std::uint64_t connection_id, std::string_view remote_address, std::uint16_t remote_port) const noexcept {
        auto services = *this;
        services.connection_services_.info_ = conn_info::plain(connection_id, remote_address);
        services.connection_services_.info_.set_remote_port(remote_port);
        services.connection_services_.peer_trusted_ =
            trusts_peer(services.connection_services_.info_.remote_.address());
        services.connection_services_.served_certificate_ = nullptr;
        return services;
    }

    template <typename traits_type, typename allocator_type>
    context_services with_plain_transport(
        std::uint64_t, std::basic_string<char, traits_type, allocator_type>&&, std::uint16_t) const = delete;

    // `alpn` and `server_name` borrow the connection's SSL, which outlives it.
    [[nodiscard]] context_services with_tls_transport(std::uint64_t connection_id, std::string_view remote_address,
        std::optional<std::string_view> client_certificate_subject, std::string_view alpn, std::string_view server_name,
        std::uint16_t remote_port, tls_served_certificate& served_certificate) const noexcept {
        auto services = *this;
        services.connection_services_.info_ =
            conn_info::tls(connection_id, remote_address, client_certificate_subject, alpn, server_name);
        services.connection_services_.info_.set_remote_port(remote_port);
        services.connection_services_.peer_trusted_ =
            trusts_peer(services.connection_services_.info_.remote_.address());
        services.connection_services_.served_certificate_ = &served_certificate;
        return services;
    }

    template <typename traits_type, typename allocator_type>
    context_services with_tls_transport(std::uint64_t, std::basic_string<char, traits_type, allocator_type>&&,
        std::optional<std::string_view>, std::string_view, std::string_view, std::uint16_t,
        tls_served_certificate&) const = delete;

    template <typename traits_type, typename allocator_type>
    context_services with_tls_transport(std::uint64_t, std::string_view, std::basic_string<char, traits_type, allocator_type>&&,
        std::string_view, std::string_view, std::uint16_t, tls_served_certificate&) const = delete;

    [[nodiscard]] constexpr std::string_view alternative_service() const noexcept {
        return connection_services_.alternative_service_;
    }

    // Empty unless the connection's listener is a listener_dispatch::request_stream one.
    [[nodiscard]] constexpr request_stream_callback_ref request_stream() const noexcept {
        return connection_services_.request_stream_;
    }

    [[nodiscard]] context_services with_request_stream(request_stream_callback_ref callback) const noexcept {
        auto services = *this;
        services.connection_services_.request_stream_ = callback;
        return services;
    }

    // `value` borrows listener-owned storage that outlives the connection.
    [[nodiscard]] context_services with_alternative_service(std::string_view value) const noexcept {
        auto services = *this;
        services.connection_services_.alternative_service_ = value;
        return services;
    }

    template <typename traits_type, typename allocator_type>
    context_services with_alternative_service(std::basic_string<char, traits_type, allocator_type>&&) const = delete;

private:
    [[nodiscard]] bool trusts_peer(const ip_address& remote_address) const noexcept {
        return worker_services_->trusted_proxies_ != nullptr &&
               worker_services_->trusted_proxies_->is_trusted_proxy(remote_address);
    }

    const context_worker_services* worker_services_;
    context_connection_services connection_services_{.info_ = conn_info::plain(0, {})};
    context_request_services request_services_;
};

// Adds the listener's Alt-Svc advertisement (RFC 7838 §3) to a final
// response. An application that set its own Alt-Svc field keeps it: the
// advertisement is a default, never a second, conflicting field. `value`
// borrows listener storage that outlives every response of the connection.
inline void apply_alternative_service(http_response& response, std::string_view value) {
    if (!value.empty() && !response.header("Alt-Svc").has_value()) {
        response.header_stable_view("Alt-Svc", value);
    }
}

}  // namespace ruvia::detail

namespace ruvia {
class session_middleware;
}

namespace ruvia::detail {

struct session_untouched final {};

struct session_unrecognized final {
    std::string id_;
};

struct session_loaded final {
    std::string id_;
    std::string data_;
};

struct session_persist_new final {
    std::string data_;
};

struct session_rotate final {
    std::string old_id_;
    std::string data_;
};

struct session_clear final {
    std::optional<std::string> old_id_;
};

class context_session_state final {
public:
    context_session_state() noexcept = default;

    void bind(const session_middleware* owner);
    [[nodiscard]] bool available() const noexcept {
        return available_;
    }
    [[nodiscard]] const session_middleware* owner() const noexcept {
        return owner_;
    }
    // The first commit freezes all mutation before any asynchronous storage I/O.
    // A failed commit is terminal: its external writes cannot safely be replayed.
    [[nodiscard]] bool begin_commit();
    void finish_commit() noexcept;
    void fail_commit(std::exception_ptr exception) noexcept;
    void observe_presented_id(std::string_view id);
    void load_recognized(std::string_view data);
    void set(std::string_view data);
    void clear();
    void regenerate();

    [[nodiscard]] std::string_view data() const& noexcept;
    [[nodiscard]] std::string_view data() const&& = delete;

    [[nodiscard]] const session_untouched* untouched() const& noexcept {
        return std::get_if<session_untouched>(&value_);
    }
    [[nodiscard]] const session_untouched* untouched() const&& = delete;

    [[nodiscard]] const session_unrecognized* unrecognized() const& noexcept {
        return std::get_if<session_unrecognized>(&value_);
    }
    [[nodiscard]] const session_unrecognized* unrecognized() const&& = delete;

    [[nodiscard]] const session_loaded* loaded() const& noexcept {
        return std::get_if<session_loaded>(&value_);
    }
    [[nodiscard]] const session_loaded* loaded() const&& = delete;

    [[nodiscard]] const session_persist_new* persist_new() const& noexcept {
        return std::get_if<session_persist_new>(&value_);
    }
    [[nodiscard]] const session_persist_new* persist_new() const&& = delete;

    [[nodiscard]] const session_rotate* rotate() const& noexcept {
        return std::get_if<session_rotate>(&value_);
    }
    [[nodiscard]] const session_rotate* rotate() const&& = delete;

    [[nodiscard]] const session_clear* cleared() const& noexcept {
        return std::get_if<session_clear>(&value_);
    }
    [[nodiscard]] const session_clear* cleared() const&& = delete;

private:
    enum class phase_type : unsigned char { active,
        committing,
        committed,
        failed };
    void require_mutable() const;

    bool available_{false};
    const session_middleware* owner_{nullptr};
    phase_type phase_{phase_type::active};
    std::exception_ptr failure_;
    std::variant<session_untouched, session_unrecognized, session_loaded, session_persist_new,
        session_rotate, session_clear>
        value_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Privileged access to a context's session slot, used by the session middleware
// to load the stored blob and read what the handler left behind.
struct session_access final {
    static void bind(context& context_value, const session_middleware* owner) {
        context_value.session_state().bind(owner);
    }

    [[nodiscard]] static bool available(const context& context_value) noexcept {
        return context_value.session_state().available();
    }

    // No callback chain: the bound session_middleware owns this capability's
    // single response-commit stage for both buffered and long-lived routes.
    [[nodiscard]] static task<void> commit(context& context);

    static void observe_presented_id(context& context_value, std::string_view id) {
        context_value.session_state().observe_presented_id(id);
    }

    static void load(context& context_value, std::string_view data) {
        context_value.session_state().load_recognized(data);
    }

    [[nodiscard]] static const context_session_state& state(const context& context_value) noexcept {
        return context_value.session_state();
    }
};

inline constexpr std::string_view session_rotation_script =
    "if redis.call('EXISTS', KEYS[1]) == 0 then return 0 end "
    "if not redis.call('SET', KEYS[2], ARGV[1], 'EX', ARGV[2], 'NX') then return 0 end "
    "redis.call('DEL', KEYS[1]) return 1";

[[nodiscard]] inline bool is_valid_session_id(std::string_view id) noexcept {
    if (id.size() != secure_token_size) {
        return false;
    }
    for (const char ch : id) {
        const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        if (!hex) {
            return false;
        }
    }
    return true;
}

inline void set_session_cookie(http_response_headers& headers, std::string_view cookie_name,
    std::string_view value, const cookie_options& options) {
    headers.set_cookie(set_cookie_plan(cookie_name, value, options));
}

inline void append_session_cookie_header(
    http_response_headers& headers, std::string_view cookie_name, std::string_view id, bool secure) {
    set_session_cookie(headers, cookie_name, id,
        {
            .same_site_ = cookie_same_site::lax,
            .http_only_ = cookie_attribute_policy::emit,
            .secure_ = secure ? cookie_attribute_policy::emit : cookie_attribute_policy::omit,
        });
}

inline void append_expired_session_cookie_header(
    http_response_headers& headers, std::string_view cookie_name, bool secure) {
    set_session_cookie(headers, cookie_name, "",
        {
            .same_site_ = cookie_same_site::lax,
            .expires_ = cookie_deletion_expires,
            .http_only_ = cookie_attribute_policy::emit,
            .secure_ = secure ? cookie_attribute_policy::emit : cookie_attribute_policy::omit,
        });
}

}  // namespace ruvia::detail

namespace ruvia::detail {

class request_body_loader final {
public:
    // `operation` bounds the wait for body bytes; an interrupted call keeps what
    // arrived so far and a later call resumes it.
    using read_all_type = task<std::string_view> (*)(void*, operation_options);
    using discard_type = task<void> (*)(void*, operation_options);

    constexpr request_body_loader(void* target, read_all_type read_all, discard_type discard) noexcept
        : target_(target),
          read_all_(read_all),
          discard_(discard) {}

    request_body_loader(const request_body_loader&) = delete;
    request_body_loader& operator=(const request_body_loader&) = delete;

    [[nodiscard]] task<std::string_view> read_all(operation_options operation) {
        switch (state_) {
            case state_type::available:
                state_ = state_type::consuming;
                break;
            case state_type::buffered:
                co_return buffered_body_;
            case state_type::consuming:
                throw std::logic_error("request body consumption is already in progress");
            case state_type::discarded:
                throw std::logic_error("request body was discarded");
            case state_type::failed:
                throw std::logic_error("request body consumption previously failed");
        }
        const operation_steps steps(operation);
        try {
            buffered_body_ = co_await read_all_(target_, std::move(operation));
        } catch (...) {
            settle_failure(steps);
            throw;
        }
        state_ = state_type::buffered;
        co_return buffered_body_;
    }

    task<void> discard(operation_options operation) {
        switch (state_) {
            case state_type::available:
                state_ = state_type::consuming;
                break;
            case state_type::buffered:
            case state_type::discarded:
                co_return;
            case state_type::consuming:
                throw std::logic_error("request body consumption is already in progress");
            case state_type::failed:
                throw std::logic_error("request body consumption previously failed");
        }
        const operation_steps steps(operation);
        try {
            co_await discard_(target_, std::move(operation));
        } catch (...) {
            settle_failure(steps);
            throw;
        }
        state_ = state_type::discarded;
    }

private:
    enum class state_type : std::uint8_t {
        available,
        consuming,
        buffered,
        discarded,
        failed,
    };

    // Called while handling a consumption failure: an interruption by the
    // operation's own bound returns the body to `available`, so a later call
    // resumes it; any other failure is final.
    void settle_failure(const operation_steps& steps) noexcept {
        state_ = state_type::failed;
        try {
            throw;
        } catch (const std::system_error& error) {
            if (steps.interrupted(error)) {
                state_ = state_type::available;
            }
        } catch (...) {
        }
    }

    void* target_;
    read_all_type read_all_;
    discard_type discard_;
    std::string_view buffered_body_;
    state_type state_{state_type::available};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

template <typename reader_type>
[[nodiscard]] task<std::optional<std::span<const std::byte>>> body_reader_read_thunk(void* target, operation_options operation) {
    return static_cast<reader_type*>(target)->read(std::move(operation));
}

template <typename reader_type>
[[nodiscard]] body_reader make_body_reader_facade(reader_type& reader_value) noexcept {
    return streaming_access::make_body_reader(&reader_value, &body_reader_read_thunk<reader_type>);
}

template <typename reader_type>
class body_reader_binding final {
public:
    template <typename... args_type>
    explicit body_reader_binding(args_type&&... args)
        : reader_(std::forward<args_type>(args)...),
          facade_(make_body_reader_facade(reader_)) {}

    body_reader_binding(const body_reader_binding&) = delete;
    body_reader_binding& operator=(const body_reader_binding&) = delete;
    body_reader_binding(body_reader_binding&&) = delete;
    body_reader_binding& operator=(body_reader_binding&&) = delete;

    [[nodiscard]] reader_type& reader() noexcept {
        return reader_;
    }
    [[nodiscard]] body_reader& facade() noexcept {
        return facade_;
    }

private:
    reader_type reader_;
    body_reader facade_;
};

template <typename loader_type>
[[nodiscard]] task<std::string_view> request_body_loader_read_all_thunk(void* target, operation_options operation) {
    return static_cast<loader_type*>(target)->read_all(std::move(operation));
}

template <typename loader_type>
task<void> request_body_loader_discard_thunk(void* target, operation_options operation) {
    return static_cast<loader_type*>(target)->discard(std::move(operation));
}

template <typename loader_type>
[[nodiscard]] request_body_loader make_request_body_loader_facade(loader_type& loader) noexcept {
    return request_body_loader(
        &loader, &request_body_loader_read_all_thunk<loader_type>, &request_body_loader_discard_thunk<loader_type>);
}

template <typename loader_type>
class request_body_loader_binding final {
public:
    template <typename... args_type>
    explicit request_body_loader_binding(args_type&&... args)
        : loader_(std::forward<args_type>(args)...),
          facade_(make_request_body_loader_facade(loader_)) {}

    request_body_loader_binding(const request_body_loader_binding&) = delete;
    request_body_loader_binding& operator=(const request_body_loader_binding&) = delete;
    request_body_loader_binding(request_body_loader_binding&&) = delete;
    request_body_loader_binding& operator=(request_body_loader_binding&&) = delete;

    [[nodiscard]] loader_type& loader() noexcept {
        return loader_;
    }
    [[nodiscard]] const loader_type& loader() const noexcept {
        return loader_;
    }
    [[nodiscard]] request_body_loader& facade() noexcept {
        return facade_;
    }

private:
    loader_type loader_;
    request_body_loader facade_;
};

}  // namespace ruvia::detail

namespace ruvia {
class context;
}

namespace ruvia::detail {

// One request's typed interim output lane. The protocol driver outlives this
// object; operations own their fields until completion.
class http_interim_response_output final {
public:
    using write_type = task<void> (*)(void*, const http_interim_response_head&);
    http_interim_response_output(void* target, write_type write) noexcept
        : target_(target),
          write_(write) {}

    // An interrupted inform() resets the request of `context_value`.
    [[nodiscard]] task<void> inform(
        const http_interim_response_head& response, operation_options operation, context& context_value);
    // Answers a 100-continue expectation before the body is read, on the same
    // lane as inform() so the two heads never interleave. A final response
    // that has started already answered the expectation, and RFC 9110 §15.2
    // forbids any 1xx after it, so then nothing is written.
    task<void> send_continue();
    void commit_final() {
        if (output_active_) {
            throw std::logic_error("interim response output is still active");
        }
        final_committed_ = true;
    }

private:
    [[nodiscard]] task<void> write_owned(
        http_status_code status, std::vector<http_header> fields, operation_options operation, context& context_value);
    void* target_;
    write_type write_;
    bool final_committed_{};
    bool output_active_{};
    operation_interrupt_source interrupts_;
    ::ruvia::operation_scope scope_;
};
}  // namespace ruvia::detail

namespace ruvia::detail {

// A lazily built index for queries(name). Values stay in request order within
// each name; groups are sorted only for lookup and are never exposed publicly.
class request_query_values final {
public:
    explicit request_query_values(const request_name_value_list& fields) {
        if (fields.empty()) {
            return;
        }
        static constexpr std::size_t inline_order_capacity = 32;
        std::array<std::size_t, inline_order_capacity> inline_order;
        std::vector<std::size_t> heap_order;
        std::span<std::size_t> order;
        if (fields.size() <= inline_order_capacity) {
            order = std::span<std::size_t>(inline_order.data(), fields.size());
        } else {
            heap_order.resize(fields.size());
            order = heap_order;
        }
        for (std::size_t i = 0; i < fields.size(); ++i) {
            order[i] = i;
        }
        std::ranges::sort(order, [&fields](std::size_t left, std::size_t right) noexcept {
            const auto left_name = fields[left].name();
            const auto right_name = fields[right].name();
            return left_name == right_name ? left < right : left_name < right_name;
        });
        std::size_t group_count = 1;
        for (std::size_t i = 1; i < order.size(); ++i) {
            if (fields[order[i]].name() != fields[order[i - 1]].name()) {
                ++group_count;
            }
        }
        groups_.reserve(group_count);
        values_.reserve(fields.size());
        for (const auto index : order) {
            const auto name = fields[index].name();
            if (groups_.empty() || groups_.back().name_ != name) {
                groups_.push_back({name, values_.size(), 0});
            }
            values_.push_back(fields[index].value());
            ++groups_.back().count_;
        }
    }

    [[nodiscard]] std::span<const std::string_view> values(
        std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND {
        const auto group = std::ranges::lower_bound(groups_, name, {}, &group_type::name_);
        if (group == groups_.end() || group->name_ != name) {
            return {};
        }
        return std::span<const std::string_view>(values_).subspan(group->begin_, group->count_);
    }
    std::span<const std::string_view> values(std::string_view) const&& = delete;

private:
    struct group_type final {
        std::string_view name_;
        std::size_t begin_;
        std::size_t count_;
    };
    std::vector<group_type> groups_;
    std::vector<std::string_view> values_;
};

// Field views borrow the request query string or decoded_bytes_, so neither
// moves once published.
class request_query_cache final {
public:
    request_query_cache() noexcept
        : fields_(request_name_value_list_access::make()) {}

    [[nodiscard]] std::string& decoded_bytes() noexcept {
        return decoded_bytes_;
    }
    [[nodiscard]] request_name_value_list& mutable_fields() noexcept {
        return fields_;
    }

    [[nodiscard]] const request_name_value_list& fields() const& noexcept {
        return fields_;
    }
    const request_name_value_list& fields() const&& = delete;

    [[nodiscard]] const request_query_values& values() & {
        if (!values_) {
            values_.emplace(fields_);
        }
        return *values_;
    }
    const request_query_values& values() && = delete;

private:
    std::string decoded_bytes_;
    request_name_value_list fields_;
    std::optional<request_query_values> values_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Draft and selected response have disjoint ownership. Building a response
// consumes the draft; selecting or replacing a response never merges metadata.
class context_response_state final {
public:
    context_response_state() noexcept = default;

    [[nodiscard]] bool has_draft() const noexcept {
        return draft_.has_value();
    }
    [[nodiscard]] http_status_code draft_status() const noexcept {
        return draft_ ? draft_->status() : http_status::ok;
    }
    void reset_draft() noexcept {
        draft_.reset();
    }
    [[nodiscard]] http_response take_draft_or_new() noexcept {
        if (!draft_) {
            return http_response{};
        }
        auto response = std::move(*draft_);
        draft_.reset();
        return response;
    }
    [[nodiscard]] http_response& draft() {
        if (!draft_) {
            draft_.emplace();
        }
        return *draft_;
    }
    [[nodiscard]] http_response* final() noexcept {
        return final_ ? &*final_ : nullptr;
    }
    [[nodiscard]] const http_response* final() const noexcept {
        return final_ ? &*final_ : nullptr;
    }
    [[nodiscard]] http_response& policy() {
        if (!policy_) {
            policy_.emplace();
        }
        return *policy_;
    }
    [[nodiscard]] http_response& defaults() {
        if (!defaults_) {
            defaults_.emplace();
        }
        return *defaults_;
    }
    void policy_status(http_status_code status) {
        policy_status_ = status;
    }
    void apply_policy(http_response& response) const {
        if (defaults_ && !defaults_->headers().empty()) {
            response.transfer_headers_from(*defaults_, http_response_header_transfer::merge);
        }
        if (policy_ && !policy_->headers().empty()) {
            response.transfer_headers_from(*policy_, http_response_header_transfer::apply);
        }
        if (policy_status_) {
            response.status(*policy_status_);
        }
    }

    void finalize(http_response&& response) {
        final_.emplace(std::move(response));
    }

    [[nodiscard]] http_response&& release_final() & {
        assert(final_);
        apply_policy(*final_);
        return std::move(*final_);
    }
    http_response&& release_final() && = delete;

private:
    std::optional<http_response> draft_;
    std::optional<http_response> policy_;
    std::optional<http_response> defaults_;
    std::optional<http_status_code> policy_status_;
    std::optional<http_response> final_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

struct aligned_storage_deleter final {
    std::size_t alignment_{alignof(std::max_align_t)};
    void operator()(std::byte* storage) const noexcept {
        ::operator delete[](storage, std::align_val_t{alignment_});
    }
};

// Owns every request-local context value that is not a borrowed pointer: lazy
// caches, response/session machines, typed bindings, and middleware next() hop
// storage. Synchronous construction borrows the input view, then retains one
// services value here before returning; no reference to the input survives.
class context_request_storage final {
public:
    explicit context_request_storage(const context_services& service_view) noexcept
        : services_(service_view),
          decoded_body_reservation_(services_.inbound_buffers()) {}

    context_request_storage(const context_request_storage&) = delete;
    context_request_storage& operator=(const context_request_storage&) = delete;

    [[nodiscard]] std::byte* allocate_raw(std::size_t size, std::size_t alignment) {
        auto* storage = static_cast<std::byte*>(::operator new[](size, std::align_val_t{alignment}));
        try {
            owned_blocks_.emplace_back(storage, aligned_storage_deleter{alignment});
        } catch (...) {
            ::operator delete[](storage, std::align_val_t{alignment});
            throw;
        }
        return storage;
    }

    context_services services_;
    context_response_state response_state_;
    context_session_state session_state_;
    request_bindings request_bindings_;

    std::optional<std::string> decoded_body_;
    inbound_buffer_reservation decoded_body_reservation_;
    std::optional<request_name_value_list> headers_;
    std::optional<request_query_cache> query_;
    std::optional<request_name_value_list> cookies_;
    std::string decoded_route_params_;
    // Views borrow route storage, the request path, or decoded_route_params_.
    std::optional<request_name_value_list> route_params_;
    std::vector<std::unique_ptr<std::byte[], aligned_storage_deleter>> owned_blocks_;
    bool route_params_invalid_{false};
    bool route_params_unencoded_{false};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class context_websocket_binding;
class context_tunnel_binding;

struct context_access final {
    // An interrupted output operation cannot withdraw what it wrote, so it
    // resets the request: the HTTP/2 or HTTP/3 stream, or the HTTP/1 connection.
    static void reset_interrupted_request(void* raw_context) noexcept {
        auto& request_services = static_cast<context*>(raw_context)->services();
        try {
            request_services.reset_request({});
        } catch (...) {
            request_services.abort_connection();
        }
    }

    // A request-stream router borrows the already-bound transport context.
    // Restore every borrowed route view before its resolution frame retires;
    // retain the output state, since a stream may have committed in that scope.
    class route_binding final {
    public:
        route_binding(context& value, std::string_view path, std::span<const std::string_view> names,
            std::span<const std::string_view> values, std::uintptr_t scope)
            : context_(value),
              services_(value.services()),
              path_(value.route_path_),
              names_(value.param_names_),
              values_(value.param_values_),
              count_(value.param_count_),
              scope_(value.route_rate_limit_scope_),
              worker_states_(value.capabilities_.worker_states()) {
            value.route_path_ = path;
            value.param_names_ = names.data();
            value.param_values_ = values.data();
            value.param_count_ = values.size();
            value.route_rate_limit_scope_ = scope;
        }
        route_binding(const route_binding&) = delete;
        route_binding& operator=(const route_binding&) = delete;
        ~route_binding() {
            const auto output = context_.services().response_output();
            context_.services() = services_;
            context_.services().response_output() = output;
            context_.route_path_ = path_;
            context_.param_names_ = names_;
            context_.param_values_ = values_;
            context_.param_count_ = count_;
            context_.route_rate_limit_scope_ = scope_;
            context_.capabilities_.bind_worker_states(worker_states_);
        }

        // Points the context's worker-state lookups at the registry its
        // services now name; the destructor restores the previous one.
        void bind_worker_states() noexcept {
            context_.capabilities_.bind_worker_states(context_.services().worker_states());
        }

    private:
        context& context_;
        context_services services_;
        std::string_view path_;
        const std::string_view* names_;
        const std::string_view* values_;
        std::size_t count_;
        std::uintptr_t scope_;
        const worker_state_registry* worker_states_;
    };

    [[nodiscard]] static context make(
        const http_request& request, const context_services& services) {
        return context(request, services);
    }

    [[nodiscard]] static context make(const http_request& request,
        std::string_view route_path, const std::string_view* param_names,
        const std::string_view* param_values, std::size_t param_count,
        std::uintptr_t route_rate_limit_scope, const context_services& services) {
        return context(request, route_path, param_names, param_values, param_count,
            route_rate_limit_scope, services);
    }

    [[nodiscard]] static std::byte* allocate_request_storage(
        context& context_value, std::size_t size, std::size_t alignment) {
        return context_value.request_storage().allocate_raw(size, alignment);
    }

    // The response for an indexed entry already resolved below `root`. The
    // representation is selected and the content type copied before the task
    // is returned, so `relative` and `content_type_value` are read only by
    // this call; `root` and `base_entry` (a view into it) must outlive the
    // task, which opens a file-backed entry on the blocking pool.
    [[nodiscard]] static task<http_response> static_file_resolved(
        context& context_value, const static_root& root, std::string_view relative,
        std::string_view content_type_value, static_root_entry_view base_entry,
        static_file_selection_mode mode);

    [[nodiscard]] static http_response make_static_json_response(
        context& context_value, std::string_view json_body) {
        auto response = context_value.response_state().take_draft_or_new();
        if (!response.header("Content-Type")) {
            response.content_type(http_content_type::application_json);
        }
        response.static_body(json_body);
        return response;
    }

    [[nodiscard]] static http_response make_owned_json_response(
        context& context_value, std::string& json_body) {
        return context_value.json_serialized(json_body);
    }

    [[nodiscard]] static http_interim_response_output* interim_output(context& context_value) noexcept {
        return context_value.services().interim_output();
    }

    [[nodiscard]] static const http_request& request(const context& context_value) noexcept {
        return context_value.request_;
    }

    [[nodiscard]] static const context_request_body_source& request_body_source(
        const context& context_value) noexcept {
        return context_value.request_body_source();
    }

    [[nodiscard]] static context_services& services(context& context_value) noexcept {
        return context_value.services();
    }

    [[nodiscard]] static const context_services& services(const context& context_value) noexcept {
        return context_value.services();
    }

    [[nodiscard]] static std::optional<std::string_view> decoded_body(
        const context& context_value) noexcept {
        if (!context_value.body_decoded_) {
            return std::nullopt;
        }
        return std::string_view(*context_value.request_storage_->decoded_body_);
    }

    [[nodiscard]] static bool body_identity(const context& context_value) noexcept {
        return context_value.body_identity_;
    }

    static void mark_body_identity(const context& context_value) noexcept {
        context_value.body_identity_ = true;
    }

    [[nodiscard]] static std::string_view store_decoded_body(
        const context& context_value, std::string&& decoded_content) {
        auto& storage = context_value.request_storage();
        storage.decoded_body_reservation_.resize_to(inbound_string_heap_bytes(decoded_content));
        auto& decoded = context_value.decoded_body();
        decoded = std::move(decoded_content);
        storage.decoded_body_reservation_.resize_to(inbound_string_heap_bytes(decoded));
        context_value.body_decoded_ = true;
        return std::string_view(decoded);
    }

    [[nodiscard]] static detail::rate_limiter* rate_limiter(context& context_value) noexcept {
        return context_value.services().rate_limiter();
    }

    [[nodiscard]] static const std::optional<std::chrono::steady_clock::time_point>& rate_limit_arrival(
        context& context_value) noexcept {
        return context_value.services().pinned_arrival();
    }

    [[nodiscard]] static std::uintptr_t route_rate_limit_scope(const context& context_value) noexcept {
        return context_value.route_rate_limit_scope_;
    }

    static void set_response(context& context_value, http_response&& response) {
        context_value.store_response(std::move(response));
    }

    [[nodiscard]] static const http_response_headers& handshake_headers(context& context_value) {
        auto& state = context_value.response_state();
        state.apply_policy(state.draft());
        return state.draft().headers();
    }
    [[nodiscard]] static http_response& response_policy(context& context_value) {
        return context_value.response_state().policy();
    }
    static void discard_draft(context& context_value) {
        context_value.response_state().reset_draft();
    }
    static void apply_response_policy(context& context_value, http_response& response) {
        context_value.response_state().apply_policy(response);
    }
    static void security_header(context& context_value, std::string_view name, std::string_view value, bool replace) {
        auto& state = context_value.response_state();
        auto& response = replace ? state.policy() : state.defaults();
        response.header_stable_view(name, value);
    }

    // The status the handler set; a file response sends it for the full body.
    [[nodiscard]] static http_status_code response_status(const context& context_value) noexcept {
        return context_value.response_state().draft_status();
    }

    static void apply_response_state(context& context_value, http_response& response,
        std::optional<http_status_code> status_code) {
        context_value.apply_response_state(response, status_code);
    }

    // Called by a driver right before it hands the upgrade/CONNECT handshake
    // head to the transport: from here on no HTTP response can follow, and no
    // 1xx head either -- the stream now carries the upgraded protocol. Throws,
    // before anything is written, while an interim head is still being written.
    static void commit_upgrade(context& context_value) {
        if (auto* interim = context_value.services().interim_output()) {
            interim->commit_final();
        }
        context_value.response_output() = context_response_output::upgraded();
    }

    [[nodiscard]] static bool output_committed(const context& context_value) noexcept {
        return context_value.response_output().committed();
    }

    static void set_error(context& context_value, std::exception_ptr exception) noexcept {
        context_value.store_error(std::move(exception));
    }

    [[nodiscard]] static bool has_response(const context& context_value) noexcept {
        return context_value.has_response();
    }

    // The final response, for the dispatcher to return; see
    // context_response_state::release_final.
    [[nodiscard]] static http_response&& release_response(context& context_value) {
        return context_value.response_state().release_final();
    }

    [[nodiscard]] static http_response streaming_head(context& context_value) {
        return context_value.streaming_head();
    }

    // Commits the bound session, then builds the head a response stream sends before its first byte.
    [[nodiscard]] static task<http_response> commit_streaming_head(context& context_value);

private:
    friend class context_websocket_binding;
    friend class context_tunnel_binding;
    [[nodiscard]] static context_response_output bind_tunnel(context& context_value, http_tunnel& tunnel) noexcept {
        auto previous = context_value.response_output();
        context_value.response_output() = context_response_output::tunnel(tunnel);
        return previous;
    }

    [[nodiscard]] static context_response_output bind_websocket(
        context& context_value, websocket& websocket_value) noexcept {
        auto previous = context_value.response_output();
        context_value.response_output() = context_response_output::websocket_value(websocket_value);
        return previous;
    }

    static void restore_response_output(context& context_value, context_response_output output) noexcept {
        context_value.response_output() = output;
    }
};

class context_tunnel_binding final {
public:
    context_tunnel_binding(context& context_value, http_tunnel& tunnel) noexcept
        : context_(context_value),
          previous_(context_access::bind_tunnel(context_value, tunnel)) {}
    context_tunnel_binding(const context_tunnel_binding&) = delete;
    context_tunnel_binding& operator=(const context_tunnel_binding&) = delete;
    ~context_tunnel_binding() {
        context_access::restore_response_output(context_, previous_);
    }

private:
    context& context_;
    context_response_output previous_;
};

// The facade borrowed by context is valid only while the established session
// owns its connection. Restoring the previous output capability on every exit
// prevents onion middleware post-processing from observing a dangling facade.
class context_websocket_binding final {
public:
    context_websocket_binding(context& context_value, websocket& websocket_value) noexcept
        : context_(&context_value),
          previous_(context_access::bind_websocket(context_value, websocket_value)) {}

    context_websocket_binding(const context_websocket_binding&) = delete;
    context_websocket_binding& operator=(const context_websocket_binding&) = delete;
    context_websocket_binding(context_websocket_binding&&) = delete;
    context_websocket_binding& operator=(context_websocket_binding&&) = delete;

    ~context_websocket_binding() {
        context_access::restore_response_output(*context_, previous_);
    }

private:
    context* context_;
    context_response_output previous_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

[[nodiscard]] http_response make_default_error_response(http_error_info error);

// The default 404 answer for a request no route matched.
[[nodiscard]] http_response make_route_not_found_response();

[[nodiscard]] task<http_response> invoke_error_handler(
    context& context_value, http_error_info error, http_error_handler_ref handler);

}  // namespace ruvia::detail

namespace ruvia::detail {

// Web-runtime signal for an otherwise valid request whose complete
// Content-Encoding stack cannot be decoded. router dispatch owns the 415 JSON
// mapping and the RFC 9110 Accept-Encoding response advertisement, which names
// what the failed read accepts: a streaming read decodes nothing.
class unsupported_request_content_coding final : public std::exception {
public:
    enum class read_type { decoded,
        streaming };

    explicit unsupported_request_content_coding(read_type read) noexcept
        : read_(read) {}

    [[nodiscard]] const char* what() const noexcept override {
        return "request Content-Encoding is not supported";
    }

    // An Accept-Encoding field value with static storage.
    [[nodiscard]] std::string_view accepted_codings() const noexcept {
        return read_ == read_type::decoded ? http_supported_request_content_codings() : "identity";
    }

private:
    read_type read_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

#if defined(_WIN32)
// INVALID_SOCKET.
inline constexpr native_socket invalid_native_socket = ~native_socket{0};
#else
inline constexpr native_socket invalid_native_socket = -1;
#endif

// Closes a socket the server owns; an invalid handle is ignored.
void close_native_socket(native_socket socket) noexcept;

// listen_config::inherited_sockets_ adoption: checks that `socket` is a
// listening TCP socket bound to exactly `endpoint` (std::invalid_argument
// with `message` otherwise), then marks it close-on-exec (non-inheritable on
// Windows).
void adopt_inherited_tcp_socket(native_socket socket, const asio::ip::tcp::endpoint& endpoint, const char* message, std::optional<bool> ipv6_only);
// The same for a UDP socket bound to exactly `endpoint`.
void adopt_inherited_udp_socket(native_socket socket, const asio::ip::udp::endpoint& endpoint, const char* message, std::optional<bool> ipv6_only);

#if !defined(_WIN32)
// A dup() of `socket`, without FD_CLOEXEC, that the caller owns.
[[nodiscard]] native_socket duplicate_native_socket(native_socket socket);
#endif

// The adopted sockets of one listener: its TCP service's and, for a TLS
// listener, its HTTP/3 service's, in worker order on POSIX and one each on
// Windows. Shared by the listener's definition and its clones until the run
// that prepares the listener claims them; whatever is still unclaimed closes
// with the last owner.
class inherited_listener_sockets final {
public:
    inherited_listener_sockets(std::vector<native_socket> tcp, std::vector<native_socket> udp) noexcept
        : tcp_(std::move(tcp)),
          udp_(std::move(udp)) {}
    ~inherited_listener_sockets();

    inherited_listener_sockets(const inherited_listener_sockets&) = delete;
    inherited_listener_sockets& operator=(const inherited_listener_sockets&) = delete;
    inherited_listener_sockets(inherited_listener_sockets&&) = delete;
    inherited_listener_sockets& operator=(inherited_listener_sockets&&) = delete;

    [[nodiscard]] std::span<const native_socket> tcp() const& noexcept RUVIA_LIFETIMEBOUND {
        return tcp_;
    }
    std::span<const native_socket> tcp() const&& = delete;
    [[nodiscard]] std::span<const native_socket> udp() const& noexcept RUVIA_LIFETIMEBOUND {
        return udp_;
    }
    std::span<const native_socket> udp() const&& = delete;

    // Takes TCP socket `index` (invalid when none): on POSIX on the thread of
    // worker `index`, so distinct indices are claimed concurrently; on
    // Windows on the acceptor's.
    [[nodiscard]] native_socket claim_tcp(std::size_t index) noexcept {
        return index < tcp_.size() ? std::exchange(tcp_[index], invalid_native_socket) : invalid_native_socket;
    }
    // Takes every UDP socket at once, for the one opener of the HTTP/3 service.
    [[nodiscard]] std::vector<native_socket> claim_udp() noexcept {
        std::vector<native_socket> claimed(udp_.get_allocator());
        claimed.swap(udp_);
        return claimed;
    }

private:
    std::vector<native_socket> tcp_;
    std::vector<native_socket> udp_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// RFC 9325 §3.4: ticket encryption keys change regularly and are destroyed at
// the end of their validity. A key encrypts tickets for one rotation interval,
// still decrypts them for the next, and is then destroyed. The interval equals
// the server session lifetime (also the TLS 1.3 ticket lifetime and TLS 1.2
// lifetime hint), so every ticket stays decryptable for its whole lifetime and
// no key outlives two intervals.
inline constexpr std::chrono::seconds tls_session_lifetime{std::chrono::hours{2}};

// Product limit: after a failed copy_to, a worker retries this soon instead of
// at the next rotation, so a transient random-generator failure (or a key
// another worker has since generated) costs seconds of ticketless handshakes,
// not an epoch.
inline constexpr std::chrono::seconds tls_ticket_key_retry_delay{5};

// One ticket key: 16-byte key name, HMAC-SHA256 key and AES-256-CBC key, the
// protection OpenSSL applies to its own tickets. Cleansed on destruction.
struct tls_session_ticket_key final {
    tls_session_ticket_key() noexcept = default;
    tls_session_ticket_key(const tls_session_ticket_key&) noexcept = default;
    tls_session_ticket_key& operator=(const tls_session_ticket_key&) noexcept = default;
    ~tls_session_ticket_key();

    std::array<unsigned char, 16> name_{};
    std::array<unsigned char, 32> hmac_key_{};
    std::array<unsigned char, 32> aes_key_{};
};

class tls_session_ticket_key_set;

// One TLS listener's authoritative ticket keys, shared by every worker so a
// ticket issued by one worker resumes on any worker of the same listener.
// Epochs are counted from a steady-clock origin; the first worker to reach a
// new epoch generates its key under the mutex. Handshakes never touch this
// object: each worker copies the keys into its own set once per epoch.
class tls_session_ticket_key_ring final {
public:
    // Generates the key of the epoch starting at `origin`; throws when the
    // random generator fails.
    explicit tls_session_ticket_key_ring(std::chrono::steady_clock::time_point origin);

    tls_session_ticket_key_ring(const tls_session_ticket_key_ring&) = delete;
    tls_session_ticket_key_ring& operator=(const tls_session_ticket_key_ring&) = delete;

    // Any thread. Advances to the epoch containing `now`, destroying keys
    // whose validity ended, and copies the keys still valid into `target`.
    // Returns false when the new epoch's key could not be generated: `target`
    // then issues no tickets and only accepts the previous key, and a later
    // call retries the generation.
    [[nodiscard]] bool copy_to(std::chrono::steady_clock::time_point now, tls_session_ticket_key_set& target);

    // When the epoch containing `now` ends.
    [[nodiscard]] std::chrono::steady_clock::time_point next_rotation(
        std::chrono::steady_clock::time_point now) const noexcept;

private:
    [[nodiscard]] std::uint64_t epoch_at(std::chrono::steady_clock::time_point now) const noexcept;

    const std::chrono::steady_clock::time_point origin_;
    std::mutex mutex_;
    // Guarded by mutex_.
    std::uint64_t epoch_{0};
    std::optional<tls_session_ticket_key> current_;
    std::optional<tls_session_ticket_key> previous_;
};

// One worker's copy of a listener's ticket keys, installed as the ticket key
// callback of every TLS context of that listener. Only the owning worker
// thread reads it (inside handshakes) and refreshes it (between handshakes),
// so the handshake path takes no lock and touches no shared state.
class tls_session_ticket_key_set final {
public:
    tls_session_ticket_key_set();
    ~tls_session_ticket_key_set();

    tls_session_ticket_key_set(const tls_session_ticket_key_set&) = delete;
    tls_session_ticket_key_set& operator=(const tls_session_ticket_key_set&) = delete;

    // Startup only, before `context` serves a connection: issues and accepts
    // tickets with this set's keys and sets the matching session lifetime.
    void install(SSL_CTX& context);

private:
    friend class tls_session_ticket_key_ring;

    static int protect_ticket(SSL* ssl, unsigned char* key_name, unsigned char* iv, EVP_CIPHER_CTX* cipher,
        EVP_MAC_CTX* mac, int encrypt) noexcept;

    EVP_CIPHER* aes_256_cbc_;
    std::optional<tls_session_ticket_key> current_;
    std::optional<tls_session_ticket_key> previous_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Fixed identities are immutable; a live library publishes its own snapshots.
struct http_server_tls_identities final {
    // Certificate and staple form one immutable selection.
    struct identity final {
        identity(tls_certificate certificate, std::span<const std::byte> ocsp_response)
            : certificate_(std::move(certificate)),
              ocsp_response_(ocsp_response.begin(), ocsp_response.end()) {}
        tls_certificate certificate_;
        std::vector<std::byte> ocsp_response_;
    };
    struct named_identity final {
        named_identity(std::string_view name, identity value)
            : name_(name),
              value_(std::move(value)) {}
        std::string name_;
        identity value_;
    };
    struct addressed_identity final {
        asio::ip::address address_;
        identity value_;
    };

    http_server_tls_identities(identity primary)
        : primary_(std::move(primary)) {}
    http_server_tls_identities() = default;

    [[nodiscard]] const identity& select(
        std::string_view server_name, const asio::ip::address* local_address) const noexcept;

    std::optional<identity> primary_;
    std::vector<named_identity> exact_names_;
    std::vector<named_identity> wildcard_names_;
    std::vector<addressed_identity> local_addresses_;
    std::shared_ptr<tls_certificate_store> certificate_store_;
    std::shared_ptr<const tls_certificate_selector> select_certificate_;
    tls_certificate_id default_certificate_{};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// A server-chosen QUIC connection ID carries its owning worker's index in two
// bytes, so packets can be steered to that worker.
inline constexpr std::size_t http3_max_worker_count = 65536;

struct http_server_listener_definition final {
    // Every TLS member is loaded when the listener is configured: no identity
    // file name, in-memory PEM or private-key password survives in a
    // definition or its clones.
    struct tls_client_certificate_policy final {
        tls_client_certificate_requirement requirement_{tls_client_certificate_requirement::optional};
        tls_trust_store client_ca_;
        // The configured bundle's name, only so replace_tls_identities() can
        // recognize the unchanged policy it requires.
        tls_file_name ca_file_;
    };

    struct redirect_http_to_https final {
        std::uint16_t https_port_;
    };

    // A mixed listener still owns exactly one TLS configuration and one TCP
    // socket. The worker chooses the stream type before parsing any HTTP.
    enum class tls_tcp_mode : std::uint8_t {
        tls_only,
        detect_plain_or_tls,
    };

    struct tls final {
        explicit tls(std::shared_ptr<const http_server_tls_identities> identities) noexcept
            : identities_(std::move(identities)) {}

        // Never null. A worker's copy is the set its handshakes select from
        // now; only that worker replaces it, between handshakes.
        std::shared_ptr<const http_server_tls_identities> identities_;
        std::uint64_t max_outgoing_records_{std::uint64_t{1} << 23};
        std::optional<tls_client_certificate_policy> client_certificates_;
        // Created once by server configuration validation and shared by every
        // worker, which copies its keys into its own TLS contexts' key set.
        std::shared_ptr<tls_session_ticket_key_ring> session_ticket_keys_;
        tls_tcp_mode tcp_mode_{tls_tcp_mode::tls_only};
        // Applies only after the accepted stream was identified as plaintext.
        std::optional<redirect_http_to_https> plaintext_redirect_;
    };

    struct plain_http final {};

    // The HTTP/3 service of a TLS listener: a QUIC endpoint on the same
    // address and UDP port_, serving the listener's TLS identities.
    struct http3_service final {
        std::uint16_t port_;
        std::chrono::seconds alt_svc_max_age_;
        bool early_data_{false};
        bool retry_{false};
        // Created once by server configuration validation and shared by every
        // worker: keys the connection IDs that steer packets between workers
        // and derives stateless reset tokens (RFC 9000 §10.3.2).
        std::shared_ptr<const std::array<std::uint8_t, 32>> secret_;
    };

    // listener_dispatch::connection: application::on_connection serves every
    // connection. A TLS listener offers alpn_ (tls_config::alpn_ or its
    // default) in server preference, encoded as an RFC 7301 §3.1
    // ProtocolNameList without its length prefix; empty for a cleartext one.
    struct connection_dispatch final {
        connection_dispatch(std::string_view alpn)
            : alpn_(alpn) {}

        std::string alpn_;
    };

    using transport_type = std::variant<plain_http, tls, redirect_http_to_https>;

    http_server_listener_definition(asio::ip::tcp::endpoint configured_endpoint,
        transport_type configured_transport = plain_http{},
        std::optional<http3_service> configured_http3 = std::nullopt)
        : endpoint_(std::move(configured_endpoint)),
          transport_(std::move(configured_transport)),
          http3_(std::move(configured_http3)) {}

    [[nodiscard]] http_server_listener_definition clone() const;

    asio::ip::tcp::endpoint endpoint_;
    std::optional<bool> ipv6_only_;
    bool proxy_protocol_{false};
    // listen_config::inherited_sockets_ of this listener, shared by its
    // clones; null when it binds. The run that prepares the listener claims
    // them; the stored definition drops them once a run took them.
    std::shared_ptr<inherited_listener_sockets> inherited_sockets_;
    transport_type transport_;
    // Only a TLS listener carries one.
    std::optional<http3_service> http3_;
    // Absent for listener_dispatch::routes.
    std::optional<connection_dispatch> connection_;
    // listener_dispatch::request_stream: application::on_request_stream serves
    // every request, on this listener and on its HTTP/3 service.
    bool request_stream_{false};
};

// Only validated configurations carry a key ring; an unvalidated definition
// must never reach a serving TLS context.
[[nodiscard]] inline tls_session_ticket_key_ring& tls_session_ticket_keys(
    const http_server_listener_definition::tls& tls) {
    if (tls.session_ticket_keys_ == nullptr) {
        throw std::logic_error("TLS listener session ticket keys were not generated");
    }
    return *tls.session_ticket_keys_;
}

class http_server_session_config final {
public:
    explicit http_server_session_config(const http_server_listener_definition& definition);

    http_server_session_config(const http_server_session_config&) = delete;
    http_server_session_config& operator=(const http_server_session_config&) = delete;
    http_server_session_config(http_server_session_config&&) = delete;
    http_server_session_config& operator=(http_server_session_config&&) = delete;

    [[nodiscard]] const http_server_listener_definition::tls* tls() const& noexcept {
        return std::get_if<http_server_listener_definition::tls>(&transport_);
    }
    const http_server_listener_definition::tls* tls() const&& = delete;

    [[nodiscard]] bool accepts_plaintext() const noexcept {
        const auto* configured = tls();
        return configured != nullptr &&
               configured->tcp_mode_ == http_server_listener_definition::tls_tcp_mode::detect_plain_or_tls;
    }

    [[nodiscard]] const http_server_listener_definition::redirect_http_to_https* redirect()
        const& noexcept {
        if (const auto* configured = tls(); configured != nullptr) {
            return configured->plaintext_redirect_ ? &*configured->plaintext_redirect_ : nullptr;
        }
        return std::get_if<http_server_listener_definition::redirect_http_to_https>(&transport_);
    }
    const http_server_listener_definition::redirect_http_to_https* redirect() const&& = delete;

    // Worker thread, between handshakes: every ClientHello this listener (TLS
    // over TCP and its HTTP/3 service) processes later selects from
    // `identities`. Handshakes keep only the identity they selected, so the
    // previous set may be released here.
    void adopt_tls_identities(std::shared_ptr<const http_server_tls_identities> identities) noexcept;

    http_server_listener_definition::transport_type transport_;
    std::optional<bool> ipv6_only_;
    bool proxy_protocol_{false};
    std::optional<http_server_listener_definition::http3_service> http3_;
    // Stable for the session's lifetime: the TLS contexts' ALPN selection
    // borrows connection_->alpn_.
    std::optional<http_server_listener_definition::connection_dispatch> connection_;
    bool request_stream_{false};
    // The Alt-Svc field value advertising http3_ (RFC 7838 §3), sent on every
    // response of this TLS listener; empty without an HTTP/3 service.
    std::string alternative_service_;
    // This worker's copy of the listener's ticket keys, refreshed on the
    // worker thread once per key epoch. The TLS contexts below point at it, so
    // it is declared before them and outlives them.
    std::optional<tls_session_ticket_key_set> ticket_keys_;
    // One context per family for the worker's whole run, carrying no
    // identity: replacing identities keeps its session cache and ticket keys,
    // and every handshake installs the identity it selects on its own SSL.
    // TLS over TCP:
    std::optional<asio::ssl::context> tls_context_;
    // TLS 1.3 with ALPN h3 for the HTTP/3 service:
    std::optional<asio::ssl::context> quic_tls_context_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// The existing listener transaction stays alive across the public prepare/commit
// boundary, retaining its serialization lease and rollback state.
class prepared_listener_state final {
public:
    explicit prepared_listener_state(worker_ref caller)
        : caller_(std::move(caller)),
          ready_(caller_),
          finished_(caller_),
          ready_receipt_(ready_.make_receipt()),
          finished_receipt_(finished_.make_receipt()) {}
    ~prepared_listener_state() {
        if (decision_ == false) {
            (void)finished_.failure();
        }
    }

    void decide(bool activate) noexcept {
        std::coroutine_handle<> continuation;
        {
            std::lock_guard lock(mutex_);
            if (decision_) {
                return;
            }
            decision_ = activate;
            continuation = std::exchange(continuation_, {});
        }
        if (continuation) {
            caller_.defer_resume(continuation);
        }
    }
    struct decision_wait final {
        prepared_listener_state& state_;
        bool await_ready() const noexcept {
            return false;
        }
        bool await_suspend(std::coroutine_handle<> continuation) noexcept {
            std::lock_guard lock(state_.mutex_);
            if (state_.decision_) {
                return false;
            }
            state_.continuation_ = continuation;
            return true;
        }
        void await_resume() const noexcept {}
        void operator&() const = delete;
    };
    task<void> checkpoint(stop_token stopping) {
        auto registration = stopping.register_callback([this] { decide(false); });
        if (stopping.stop_requested()) {
            throw std::logic_error("prepared listener host is stopping");
        }
        ready_receipt_->complete();
        ready_receipt_.reset();
        co_await decision_wait{*this};
        std::lock_guard lock(mutex_);
        if (!*decision_) {
            throw std::logic_error("prepared listener was cancelled or its host stopped");
        }
    }
    void complete(std::exception_ptr failure) noexcept {
        if (ready_receipt_) {
            ready_receipt_->complete(failure);
            ready_receipt_.reset();
        }
        finished_receipt_->complete(std::move(failure));
        finished_receipt_.reset();
    }
    worker_ref caller_;
    runtime_control_completion ready_;
    runtime_control_completion finished_;

private:
    std::optional<runtime_control_completion::receipt> ready_receipt_;
    std::optional<runtime_control_completion::receipt> finished_receipt_;
    std::mutex mutex_;
    std::optional<bool> decision_;
    std::coroutine_handle<> continuation_;
};

struct prepared_listener_access final {
    static task<void> add(task<prepared_listener> preparation) {
        auto prepared = co_await std::move(preparation);
        co_await std::move(prepared).activate();
    }
    static std::shared_ptr<prepared_listener_state> make(worker_ref caller) {
        return std::make_shared<prepared_listener_state>(std::move(caller));
    }
    static task<prepared_listener> launch(std::shared_ptr<prepared_listener_state> state, task<void> operation) {
        if (!state->caller_.is_current() || !state->caller_.accepting()) {
            throw std::logic_error("listener preparation must run on its live caller worker");
        }
        async_start(std::move(operation), asio::bind_executor(state->caller_.executor(), [state](std::exception_ptr failure) { state->complete(std::move(failure)); }));
        co_await state->ready_.wait();
        if (auto failure = state->ready_.failure()) {
            co_await state->finished_.wait();
            (void)state->finished_.failure();
            std::rethrow_exception(failure);
        }
        co_return prepared_listener(std::move(state));
    }
    static task<void> finish(prepared_listener owner, bool activate) {
        const auto state = owner.state_;
        if (!state || !state->caller_.is_current()) {
            throw std::logic_error("prepared listener control must run on its preparation worker");
        }
        state->decide(activate);
        co_await state->finished_.wait();
        const auto failure = state->finished_.failure();
        owner.state_.reset();
        if (activate && failure) {
            std::rethrow_exception(failure);
        }
    }
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// 在构造冷任务前接管整批 socket，并拥有全部借用地址及配置副本。
// load() 将全部 socket 一次性交给唯一 loader；任何失败只关闭每个句柄一次。
class owned_listen_configs final {
public:
    explicit owned_listen_configs(listen_config config);
    explicit owned_listen_configs(std::span<const listen_config> configs);
    owned_listen_configs(owned_listen_configs&& other) noexcept
        : configs_(std::move(other.configs_)),
          addresses_(std::move(other.addresses_)),
          inherited_owned_(std::exchange(other.inherited_owned_, false)) {}
    owned_listen_configs(const owned_listen_configs&) = delete;
    owned_listen_configs& operator=(const owned_listen_configs&) = delete;
    owned_listen_configs& operator=(owned_listen_configs&&) = delete;
    ~owned_listen_configs();

    [[nodiscard]] std::vector<http_server_listener_definition> load();
    [[nodiscard]] bool needs_tls_preparation() const noexcept;
    std::vector<listen_config> configs_{};

private:
    void own_addresses();
    std::vector<std::string> addresses_{};
    bool inherited_owned_{true};
};

}  // namespace ruvia::detail

namespace ruvia {

class static_root;

namespace detail {

// One immutable document-root snapshot. The application's refresher builds it
// once and every worker shares it; reference counting happens only when a
// snapshot is published, adopted, or retired, never per request.
using document_root_snapshot = std::shared_ptr<const static_root>;

// One worker's adopted snapshot plus that worker's request leases on it. Only
// the owning worker touches the counter, so leasing stays a plain increment.
// The worker keeps a superseded generation until its leases drain, which keeps
// the snapshot alive for requests that still borrow it.
class document_root_generation final {
public:
    explicit document_root_generation(document_root_snapshot root) noexcept
        : root_(std::move(root)) {}

    ~document_root_generation() {
        if (leases_ != 0) {
            // A binding still points into this generation. Destroying it would
            // leave the request with a dangling root and counter.
            std::terminate();
        }
    }

    document_root_generation(const document_root_generation&) = delete;
    document_root_generation& operator=(const document_root_generation&) = delete;

    [[nodiscard]] const static_root* root() const noexcept {
        return root_.get();
    }

    [[nodiscard]] bool leased() const noexcept {
        return leases_ != 0;
    }

private:
    friend class document_root_binding;

    document_root_snapshot root_;
    std::size_t leases_{0};
};

// A request-time view of the configured document root. The object is
// deliberately not default-constructible or aggregate-initializable: callers
// choose between no root and a worker's current generation. It is move-only
// because a configured binding is also the request's lease on that generation;
// copying the view would make that ownership boundary ambiguous and could
// reintroduce a dangling static_root after a refresh.
class document_root_binding final {
public:
    [[nodiscard]] static document_root_binding none() noexcept {
        return document_root_binding(nullptr);
    }

    [[nodiscard]] static document_root_binding configured(document_root_generation& generation) noexcept {
        ++generation.leases_;
        return document_root_binding(&generation);
    }

    ~document_root_binding() {
        reset();
    }

    document_root_binding(const document_root_binding&) = delete;
    document_root_binding& operator=(const document_root_binding&) = delete;

    document_root_binding(document_root_binding&& other) noexcept
        : generation_(std::exchange(other.generation_, nullptr)) {}

    document_root_binding& operator=(document_root_binding&& other) noexcept {
        if (this != &other) {
            reset();
            generation_ = std::exchange(other.generation_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] const static_root* root() const noexcept {
        return generation_ == nullptr ? nullptr : generation_->root();
    }

private:
    explicit document_root_binding(document_root_generation* generation) noexcept
        : generation_(generation) {}

    void reset() noexcept {
        auto* const generation = std::exchange(generation_, nullptr);
        if (generation != nullptr) {
            if (generation->leases_ == 0) {
                std::terminate();
            }
            --generation->leases_;
        }
    }

    document_root_generation* generation_;
};

}  // namespace detail
}  // namespace ruvia

namespace ruvia::detail {

using stream_encoder_lease = http_stream_encoder_lease;

class worker_codecs final {
public:
    worker_codecs() noexcept = default;
    ~worker_codecs() = default;
    worker_codecs(const worker_codecs&) = delete;
    worker_codecs& operator=(const worker_codecs&) = delete;

    [[nodiscard]] http_content_encode_context& encoder(http_content_coding coding) {
        return content_codecs_.encoder(coding);
    }

    [[nodiscard]] stream_encoder_lease acquire_stream_encoder(http_content_coding coding) {
        return content_codecs_.acquire_stream_encoder(coding);
    }

    [[nodiscard]] websocket_deflate_codecs& websocket_deflate() noexcept {
        return websocket_deflate_;
    }

private:
    http_worker_codecs content_codecs_;
    websocket_deflate_codecs websocket_deflate_;
};
}  // namespace ruvia::detail

namespace ruvia::detail {

// Validates every TLS setting of one listener before any credential is read,
// then parses each identity (primary, SNI and local-address identities) from
// its PEM files or in-memory PEM, and the client CA bundle, exactly once.
// Invalid settings throw std::invalid_argument; unreadable, mismatched or
// invalid credentials throw asio::system_error. File names reach OpenSSL as
// UTF-8, which its file BIOs decode (on Windows into _wfopen), so no path is
// narrowed through the ANSI code page. Passwords and PEM text are only
// borrowed while their identity is parsed and are never copied; an empty
// password uses the noninteractive default and terminal prompting is disabled.
[[nodiscard]] http_server_listener_definition::tls load_http_server_tls(
    const tls_config& config);

// The identities of `config` alone, validated then parsed as by
// load_http_server_tls(); for application::replace_tls_identities().
[[nodiscard]] std::shared_ptr<const http_server_tls_identities> load_http_server_tls_identities(
    const tls_config& config);

// Throws std::invalid_argument unless `config` keeps the client certificate
// policy (the same CA path and requirement) and the outgoing record limit
// `current` was loaded with: replacing identities never changes them.
void ensure_same_http_server_tls_policy(const http_server_listener_definition::tls& current, const tls_config& config);

// Installs client-certificate verification into a worker's TLS context.
// Never touches the file system.
void configure_http_server_client_certificates(SSL_CTX& context_value,
    const std::optional<http_server_listener_definition::tls_client_certificate_policy>& client_certificates);

}  // namespace ruvia::detail

namespace ruvia {
class env;
}

namespace ruvia::detail {

class document_root_publisher;
class inbound_buffer_budget;
class worker_codecs;

struct access_log_sink final {
    access_log_callback_ref callback_;

    void invoke(const access_log_record& record) const noexcept {
        callback_(record);
    }
};

// The one place access-log records are built: from a live request, or from
// fields a pipelined response kept after its request storage was reused.
struct access_log_record_access final {
    using fields_type = access_log_record::fields_type;

    [[nodiscard]] static constexpr access_log_record make(const fields_type& fields) noexcept {
        return access_log_record(fields);
    }

    // Microseconds from `start` to now, clamped at zero.
    [[nodiscard]] static std::uint64_t duration_since(std::chrono::steady_clock::time_point start) noexcept {
        const auto micros =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        return micros < 0 ? 0 : static_cast<std::uint64_t>(micros);
    }
};

struct connection_failure_record_access final {
    [[nodiscard]] static connection_failure_record make(
        ip_address remote_address, std::exception_ptr exception) noexcept {
        return connection_failure_record(remote_address, std::move(exception));
    }
};

// The one place server observation records are built.
struct server_observation_access final {
    using tls_client_hello_fields = tls_client_hello_record::fields_type;

    [[nodiscard]] static tls_client_hello_record tls_client_hello(
        const tls_client_hello_fields& fields) noexcept {
        return tls_client_hello_record(fields);
    }

    [[nodiscard]] static constexpr http2_input_record http2_input(
        std::uint64_t connection_id, std::span<const std::byte> bytes) noexcept {
        return http2_input_record(connection_id, bytes);
    }
#if defined(RUVIA_ENABLE_HTTP2)
    [[nodiscard]] static constexpr http2_frame_record http2_frame(
        std::uint64_t connection_id, const http2_frame_metadata& metadata) noexcept {
        return http2_frame_record(connection_id, metadata);
    }
    [[nodiscard]] static constexpr http2_headers_record http2_headers(
        std::uint64_t connection_id, const http2_header_metadata& metadata) noexcept {
        return http2_headers_record(connection_id, metadata);
    }
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    [[nodiscard]] static constexpr http3_frame_record http3_frame(
        std::uint64_t connection_id, const http3_frame_metadata& metadata) noexcept {
        return http3_frame_record(connection_id, metadata);
    }
    [[nodiscard]] static constexpr http3_field_record http3_field(
        std::uint64_t connection_id, const http3_field_metadata& metadata) noexcept {
        return http3_field_record(connection_id, metadata);
    }
#endif
};

// The connection level's failure outlet. An unset callback still reports: the
// failure goes to the shared last-resort reporter rather than being dropped
// with the connection that produced it.
//
// A connection that ends because the peer reset or closed it, or because this
// server cancelled its I/O (stop, drain, or a timeout that closes the socket),
// is a normal end of connection rather than a failure: browsers close SSE
// tabs with a reset, and drain aborts pending reads. Such endings are neither
// reported nor counted, so the sink stays reserved for real faults. Only an
// inbound_transport_error -- thrown by I/O on the connection's own transport,
// possibly propagated through a handler -- qualifies; any other exception is
// reported whatever error code it carries.
struct connection_failure_sink final {
    connection_failure_callback_ref callback_;
    // Owned by the web_worker_runtime this sink was configured for; null before one
    // claims it. Counting here rather than at each reporting site keeps the
    // count and the callback from drifting apart as new sites are added.
    std::atomic<std::size_t>* counter_{nullptr};

    void invoke(ip_address remote_address, std::exception_ptr exception) const noexcept;
    void invoke(std::string_view remote_address, std::exception_ptr exception) const noexcept;
};

struct worker_failure_sink final {
    using invoke_type = void (*)(void*, const std::exception_ptr&) noexcept;

    void* target_{nullptr};
    invoke_type invoke_{nullptr};

    void notify(const std::exception_ptr& failure) const noexcept {
        if (invoke_ != nullptr) {
            invoke_(target_, failure);
        }
    }
};

// Fully normalized, worker-owned runtime options. Public callers configure application
// with server_config.h values; only the Web runtime constructs this state.
struct http_server_options final {
    // The application's document root: one app-scoped publisher shared by
    // every worker, plus this worker's current generation. Validated
    // configuration carries only the publisher; each worker's
    // static_root_runtime publishes its own generation before serving.
    class document_root_state final {
    public:
        document_root_state() noexcept = default;

        [[nodiscard]] static document_root_state refreshing(document_root_publisher& publisher) noexcept {
            return document_root_state(&publisher);
        }

        [[nodiscard]] document_root_publisher* publisher() const noexcept {
            return publisher_;
        }

        // Points new request bindings at the worker's adopted generation. Only
        // the owning worker calls this; earlier bindings keep their own lease.
        void publish(document_root_generation& generation) noexcept {
            if (publisher_ == nullptr) {
                std::terminate();
            }
            current_ = &generation;
        }

        [[nodiscard]] document_root_binding binding() const noexcept {
            return current_ == nullptr ? document_root_binding::none()
                                       : document_root_binding::configured(*current_);
        }

    private:
        explicit document_root_state(document_root_publisher* publisher) noexcept
            : publisher_(publisher) {}

        document_root_publisher* publisher_{nullptr};
        document_root_generation* current_{nullptr};
    };

    // The validated application server configuration, one copy per worker.
    server_config server_{};
    // Borrowed from the worker runtime, whose shutdown joins every connection;
    // each connection charges its inbound_buffer_account against it.
    inbound_buffer_budget* inbound_budget_{nullptr};
    // Presence enables the policy; absence bypasses it without retaining an
    // inactive configuration state.
    std::optional<compression_config> compression_{};
    // Borrowed from the worker runtime, which always sets it and outlives every
    // request and connection. Inline response compression and server WebSocket
    // connections (shared no-context-takeover deflate) borrow it; worker-thread
    // synchronous use only.
    worker_codecs* codecs_{nullptr};
    std::optional<http_cors_policy> cors_{};
    document_root_state document_root_{};
    // Peers whose forwarding headers may be believed. Empty by default, so an
    // unconfigured server treats every direct peer as the client.
    http_client_ip_extractor trusted_proxies_{};
    access_log_sink access_log_{};
    const env* env_{nullptr};
    worker_failure_sink failure_{};
    connection_failure_sink connection_failure_{};
    // Empty observers keep the connection paths free of observation work.
    tls_client_hello_callback_ref tls_client_hello_{};
    http2_input_callback_ref http2_input_{};
#if defined(RUVIA_ENABLE_HTTP2)
    http2_frame_callback_ref http2_frame_{};
    http2_headers_callback_ref http2_headers_{};
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    http3_frame_callback_ref http3_frame_{};
    http3_field_callback_ref http3_field_{};
#endif
    connection_admission_callback_ref connection_admission_{};
    connection_open_callback_ref connection_open_{};
    connection_close_callback_ref connection_close_{};
    // Serves every listener_dispatch::connection listener; run() requires it
    // when one exists.
    connection_callback_ref connection_{};
    // Serves every listener_dispatch::request_stream listener; run() requires
    // it when one exists.
    request_stream_callback_ref request_stream_{};
    std::optional<rate_limit_rule> default_rate_limit_per_worker_{};
    std::size_t rate_limit_capacity_per_worker_{default_rate_limit_capacity_per_worker};

    // Validates the whole aggregate, including its compression and CORS
    // policies, before publishing any of it.
    void set_server(const server_config& config);
    void set_rate_limit(const rate_limit_config& config);
    void clear_rate_limit() noexcept {
        default_rate_limit_per_worker_.reset();
        rate_limit_capacity_per_worker_ = default_rate_limit_capacity_per_worker;
    }
    void set_trusted_proxies(const trusted_proxy_config& config);
    void clear_trusted_proxies() noexcept {
        trusted_proxies_ = http_client_ip_extractor();
    }
};

}  // namespace ruvia::detail

namespace ruvia {
struct listen_config;
}

namespace ruvia::detail {

// Owns every inherited socket and cleanses every TLS secret, including when
// configuration validation or allocation fails.
[[nodiscard]] std::vector<http_server_listener_definition> load_server_listeners(
    std::span<const listen_config> configs);

// Rejects collisions in their separate TCP and HTTP/3 UDP endpoint namespaces.
void ensure_new_listener_endpoints(std::span<const http_server_listener_definition> existing,
    std::span<const http_server_listener_definition> added);

void validate_server_limits(const server_config& config);

// `listener` prepared for a run with `options`: a clone sharing its loaded
// credentials and inherited sockets, with a ticket key ring whose epochs
// start at `key_origin`, the run's origin for every TLS listener, and with
// HTTP/3 its connection ID secret. Throws std::invalid_argument for a
// listener the run cannot serve, including a connection or request_stream
// listener whose application callback `options` lacks.
[[nodiscard]] http_server_listener_definition validate_run_listener(const http_server_listener_definition& listener,
    const http_server_options& options, std::chrono::steady_clock::time_point key_origin);

class validated_http_server_configuration final {
public:
    validated_http_server_configuration(const validated_http_server_configuration&) = delete;
    validated_http_server_configuration& operator=(const validated_http_server_configuration&) = delete;
    validated_http_server_configuration(validated_http_server_configuration&&) = default;
    validated_http_server_configuration& operator=(validated_http_server_configuration&&) = default;

    [[nodiscard]] std::span<const http_server_listener_definition> listeners() const& noexcept RUVIA_LIFETIMEBOUND {
        return listeners_;
    }
    std::span<const http_server_listener_definition> listeners() const&& = delete;

    [[nodiscard]] const http_server_options& options() const& noexcept RUVIA_LIFETIMEBOUND {
        return options_;
    }
    const http_server_options& options() const&& = delete;

    // Where the ticket key epochs of every TLS listener of the run start,
    // including listeners added while it runs.
    [[nodiscard]] std::chrono::steady_clock::time_point tls_key_origin() const noexcept {
        return tls_key_origin_;
    }

private:
    validated_http_server_configuration(
        std::span<const http_server_listener_definition> listeners, http_server_options&& options);

    friend validated_http_server_configuration validate_http_server_configuration(
        std::span<const http_server_listener_definition> listeners, http_server_options&& options);

    std::vector<http_server_listener_definition> listeners_;
    http_server_options options_;
    std::chrono::steady_clock::time_point tls_key_origin_;
};

[[nodiscard]] validated_http_server_configuration validate_http_server_configuration(
    std::span<const http_server_listener_definition> listeners, http_server_options&& options);

}  // namespace ruvia::detail

#if defined(RUVIA_ENABLE_HTTP3)
namespace ruvia {
class http3_quic_connection;
struct http3_connection_options;
}  // namespace ruvia
#endif

namespace ruvia::detail {

// Builds one worker's TLS contexts of a listener at startup, from its loaded
// definition: the TCP context (TLS 1.1..1.3, h2 only with TLS >= 1.2,
// http/1.1, http/1.0, or connection dispatch's custom ALPN) and, with an
// HTTP/3 service, the QUIC context (TLS 1.3 only, ALPN h3 only, opt-in early
// data). Neither carries an identity: each
// handshake selects one from the identity set the session holds when its
// server_name is known (exact SNI, wildcard SNI, the local address bound by
// bind_tls_local_address(), else the primary) and installs it on its own SSL.
// Both bind resumption to the offered server_name and share the worker's copy
// of the listener's ticket keys. Replacing identities never rebuilds them, so
// their session caches and ticket keys survive it. observe_client_hello
// installs the message callback that delivers each connection's ClientHello
// to its observe_tls_client_hello() observation; without it no observation
// branch is installed. A listener without TLS keeps no context.
void configure_http_server_tls_contexts(http_server_session_config& session, bool observe_client_hello);

using prepared_http_server_session =
    std::unique_ptr<http_server_session_config>;

// Builds a stable-address candidate off the owner loop. Its contexts borrow
// only its own fields; move the owning pointer, never the session itself.
[[nodiscard]] prepared_http_server_session prepare_http_server_session_config(
    const http_server_listener_definition& definition, bool observe_client_hello);

// Attaches the local address a connection arrived at, which identity
// selection matches against local-address identities. The caller keeps it
// alive and unchanged for the SSL's whole lifetime; a connection without one
// never matches them.
void bind_tls_local_address(SSL& tls, const asio::ip::address& local_address);

// A selector needs the same stable endpoint/connection binding as an observer,
// even when no observer is registered. Does not install message observation.
[[nodiscard]] bool tls_certificate_selection_enabled(const http_server_session_config& session) noexcept;

// One connection's on_tls_client_hello delivery: every field of the record
// except the message, which the ClientHello supplies.
struct tls_client_hello_observation final {
    tls_client_hello_callback_ref callback_;
    server_observation_access::tls_client_hello_fields fields_;
};

// Attaches `observation` to a connection's SSL created from a context that
// configure_http_server_tls_contexts() built with observe_client_hello. The
// caller keeps it alive and unchanged for the SSL's whole lifetime.
void observe_tls_client_hello(SSL& tls, const tls_client_hello_observation& observation);

#if defined(RUVIA_ENABLE_HTTP3)
// Called after binding the QUIC connection, before receiving its Initial.
// `version` is the QUIC version of the client's first Initial. The caller
// owns resumption_context for the entire SSL lifetime.
void configure_http3_server_tls_session(SSL& tls, const http3_quic_connection& connection,
    std::uint32_t version, const http3_connection_options& options, bool early_data,
    std::array<unsigned char, 32>& resumption_context);
#endif
}  // namespace ruvia::detail

#if defined(_WIN32)

namespace ruvia::detail {

class connection_count;
#if defined(RUVIA_ENABLE_HTTP3)
class http3_worker_directory;
#endif

// An accepted TCP SOCKET that no I/O completion port owns yet: the acceptor
// creates it for AcceptEx and hands it to a business worker, whose io_context
// binds it on adoption. Closes it unless released.
class windows_accepted_socket final {
public:
    explicit windows_accepted_socket(SOCKET handle) noexcept
        : handle_(handle) {}
    windows_accepted_socket(windows_accepted_socket&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_SOCKET)) {}
    windows_accepted_socket(const windows_accepted_socket&) = delete;
    windows_accepted_socket& operator=(const windows_accepted_socket&) = delete;
    windows_accepted_socket& operator=(windows_accepted_socket&&) = delete;
    ~windows_accepted_socket();

    [[nodiscard]] SOCKET get() const noexcept {
        return handle_;
    }
    // The caller now owns the handle.
    SOCKET release() noexcept {
        return std::exchange(handle_, INVALID_SOCKET);
    }

private:
    SOCKET handle_;
};

// The application's dedicated Windows acceptor (AGENTS.md §5): a lightweight
// worker with its own core runtime and thread that owns every TCP listener,
// and every HTTP/3 UDP socket, exclusively (SO_EXCLUSIVEADDRUSE), so the
// business workers stay alike.
//
// TCP: AcceptEx accepts a raw SOCKET that no completion port owns and hands
// it to the business worker holding the fewest connections (ties rotate),
// through that worker's bounded queue: admission is reserved first, and a
// socket the queue refuses is closed and counted as refused. The worker binds
// the socket to its own io_context and serves it like any connection.
//
// HTTP/3: the acceptor receives every datagram and hands it, through the
// HTTP/3 worker directory, to the worker its destination connection ID names,
// or for a long header naming none (a new connection's Initial) to the worker
// http3_initial_worker picks. Workers send through transmit(): the datagram
// is queued, bounded per worker, and sent from the listener's socket.
//
// Lifecycle (driven by the application run):
//   start()          binds (or adopts) every listener; the two-phase start's
//                    first phase
//   serve()          after every business worker serves: accepting begins
//   stop_admission() pending accepts and receives are cancelled; the UDP
//                    sockets stay open for the business workers' last
//                    datagrams (CONNECTION_CLOSE)
//   request_stop()   after the business workers joined: every socket closes
//   join()
// Listener slots match the business workers' and are never reused within a
// run; application::add_listener() and remove_listener() change them on the
// acceptor thread through post_control().
class windows_acceptor final {
public:
    // Business worker thread: adopts one accepted socket.
    using adopt_type = void (*)(void* target, std::size_t listener_index, windows_accepted_socket accepted);

    // One business worker as a hand-over target.
    struct business_worker final {
        worker_ref worker_;
        const connection_count* load_{nullptr};
        void* target_{nullptr};
        adopt_type adopt_{nullptr};
    };

    windows_acceptor(std::span<const http_server_listener_definition> listeners, std::size_t worker_count,
#if defined(RUVIA_ENABLE_HTTP3)
        http3_worker_directory* directory,
#endif
        worker_failure_sink failure);
    ~windows_acceptor();

    windows_acceptor(const windows_acceptor&) = delete;
    windows_acceptor& operator=(const windows_acceptor&) = delete;

    // Construction thread, once per business worker in worker order.
    void bind_worker(std::size_t index, business_worker target);
    // Lifecycle thread. False when a stop came first; rethrows the
    // acceptor's failure (a listener that cannot be bound).
    [[nodiscard]] bool start();
    [[nodiscard]] bool serve();
    [[nodiscard]] task<void> start(worker_ref caller);
    [[nodiscard]] task<void> serve(worker_ref caller);
    // Any thread.
    void stop_admission() noexcept;
    void request_stop() noexcept;
    // Lifecycle thread, after request_stop().
    void join();
    // Waits for the native thread HANDLE asynchronously; join() afterwards only
    // reclaims a thread whose exit has already been observed.
    [[nodiscard]] task<void> join(worker_ref caller, asio::io_context& caller_io);
    [[nodiscard]] std::exception_ptr failure() noexcept;
    // Any thread: sockets the chosen worker's queue refused, and accept
    // failures other than peer disconnects.
    [[nodiscard]] std::size_t refused() const noexcept {
        return refused_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t accept_failures() const noexcept {
        return accept_failures_.load(std::memory_order_relaxed);
    }
    // Any thread: runs `control` on the acceptor thread; false once stopping.
    [[nodiscard]] bool post_control(move_only_function<void()> control) noexcept {
        return runtime_.post_control(std::move(control));
    }
    // Acceptor thread, the controls of application::add_listener(): binds or
    // adopts `definition`'s TCP socket in slot `index` and its HTTP/3 UDP
    // socket in slot `http3_index`, accepting nothing yet, or throws with
    // both slots left empty.
    void prepare_listener(std::size_t index, std::optional<std::size_t> http3_index,
        const http_server_listener_definition& definition
#if defined(RUVIA_ENABLE_HTTP3)
        ,
        http3_worker_directory* directory
#endif
    );
    // Starts accepting for (and receiving datagrams of) a prepared listener.
    void open_listener(std::size_t index) noexcept;
    // Closes the sockets of a listener prepared but never opened.
    void discard_listener(std::size_t index, std::shared_ptr<void> receipt = {}) noexcept;
    // application::remove_listener(): stops accepting for listener `index`
    // and closes its TCP socket, holding `receipt` until its accepts ended.
    // Its UDP socket keeps receiving for the established QUIC connections
    // until every business worker released it.
    void remove_listener(std::size_t index, std::shared_ptr<void> receipt) noexcept;
#if defined(RUVIA_ENABLE_HTTP3)
    // Business worker `worker_index`'s thread: queues one datagram for the
    // socket of HTTP/3 listener `http3_listener_index`. False when it was
    // dropped (the worker's bound is full or the acceptor stopped).
    [[nodiscard]] bool transmit(std::size_t worker_index, std::size_t http3_listener_index,
        const asio::ip::udp::endpoint& remote, std::span<const std::byte> bytes,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    // Any thread: one business worker's endpoint of a removed HTTP/3
    // listener served its last connection. The last worker releases this
    // listener's claim; the socket closes once no listener still needs it.
    void release_http3_listener(std::size_t http3_listener_index) noexcept;
#endif

private:
    struct tcp_listener;
    struct worker_slot;
#if defined(RUVIA_ENABLE_HTTP3)
    struct udp_transport;
    struct udp_listener;
#endif

    void complete_phase(bool startup, std::exception_ptr failure = {}) noexcept;
    task<void> run();
    void open_listeners();
    void open_tcp_listener(tcp_listener& listener);
#if defined(RUVIA_ENABLE_HTTP3)
    void open_udp_listener(udp_listener& listener);
#endif
    void prepare_listener_tasks(tcp_listener& listener);
    void start_listener(tcp_listener& listener) noexcept;
    void serve_on_context();
    void close_admission() noexcept;
    task<void> run_accepts(tcp_listener& listener);
    void report_accept_error(const std::error_code& error);
    void dispatch(std::size_t listener_index, windows_accepted_socket accepted);
    [[nodiscard]] std::size_t select_worker() noexcept;
#if defined(RUVIA_ENABLE_HTTP3)
    task<void> run_receives(udp_listener& listener);
    void steer(const udp_listener& listener, std::span<const std::byte> bytes,
        const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local,
        std::uint32_t interface_index) noexcept;
    void send_datagram(std::size_t http3_listener_index, const asio::ip::udp::endpoint& remote,
        std::span<const std::byte> bytes, const asio::ip::udp::endpoint& local,
        std::uint32_t interface_index) noexcept;
#endif

    // Declared first: every socket below belongs to its io_context.
    worker_runtime runtime_;
    asio::io_context& io_;
    worker_failure_sink failure_sink_;
    worker_signal finalize_signal_;
    task_scope tasks_;
    std::vector<std::unique_ptr<worker_slot>> workers_;
    // Indexed by listener slot, the HTTP/3 ones by HTTP/3 slot; empty for a
    // listener whose addition was rolled back. A removed listener keeps its
    // closed socket object until the acceptor ends: its accepts reference it.
    std::vector<std::unique_ptr<tcp_listener>> tcp_listeners_;
#if defined(RUVIA_ENABLE_HTTP3)
    http3_worker_directory* directory_;
    std::vector<std::unique_ptr<udp_listener>> udp_listeners_;
#endif
    // Owner thread.
    std::size_t next_worker_{0};
    bool admitting_{false};
    bool stopping_{false};
    std::atomic<std::size_t> refused_{0};
    std::atomic<std::size_t> accept_failures_{0};
    std::mutex phase_mutex_;
    std::optional<runtime_control_completion::receipt> startup_receipt_;
    std::optional<runtime_control_completion::receipt> serve_receipt_;
};

}  // namespace ruvia::detail
#endif

namespace ruvia::detail {

// Runtime capability is deliberately separate from Accept-Encoding
// negotiation. A precompressed file sidecar can satisfy a selected coding
// without this process owning an incremental encoder.
enum class http_response_coding_availability : std::uint8_t {
    identity_only,
    identity_and_compression,
};

// Static-file representations remain owned by the static response path.
enum class http_response_compression_source : std::uint8_t {
    buffered,
    stream,
};

enum class http_response_compression_decision : std::uint8_t {
    fixed_representation,
    negotiated_identity,
    encode,
};

// Compression policy and encoder execution are separate outcomes. A response
// may intentionally remain identity (body too small while identity is
// acceptable, no-transform, integrity digest fields, an incompressible media
// type, or an unsupported runtime capability), or the encoder may actually
// fail. Those cases have different meanings when the client has forbidden
// identity and must not collapse back to a bool.
enum class http_response_compression_status : std::uint8_t {
    compressed,
    not_applicable,
    unavailable,
    failed,
};

// The 406 every coding-policy path answers when the client accepts none of the
// codings a response can be sent with. Options rather than http_error_info so
// a throw site can build an http_error from the same value.
[[nodiscard]] constexpr http_error_info_options http_not_acceptable_coding_error() noexcept {
    return {.status_ = http_status::not_acceptable,
        .code_ = "not_acceptable",
        .message_ = "no acceptable response content coding"};
}

// Return true when the response cannot satisfy the client's coding policy:
// the negotiation failed, a framework-selected coding fell back to forbidden
// identity, or a handler supplied a pre-encoded representation the client
// excluded. Bodyless statuses are representation-free and therefore never
// need a content-coding fallback.
[[nodiscard]] inline bool http_response_coding_fallback_forbidden(const http_response_coding_selection& selection,
    http_known_method request_method, const http_response& response, const http_request* request = nullptr) {
    if (!plan_http_response_body(request_method, response.status()).status_allows_body()) {
        return false;
    }
    // An empty 416 carries no content and so no coding to negotiate; its
    // Content-Range describes the selected representation (RFC 9110 15.5.17).
    if (response.status() == http_status::range_not_satisfiable && response.body_bytes().empty()) {
        return false;
    }
    switch (evaluate_http_response_content_coding(selection, response, request)) {
        case http_response_content_coding_status::accepted:
            return false;
        case http_response_content_coding_status::not_acceptable:
        case http_response_content_coding_status::invalid:
            return true;
        case http_response_content_coding_status::absent:
            break;
    }
    if (!selection.acceptable()) {
        return true;
    }
    if (selection.coding() == http_content_coding::identity || selection.identity_accepted()) {
        return false;
    }
    // This guard targets only the framework's silent identity fallback.
    return true;
}

// Outcome of the synchronous compression phase for an in-memory body. An
// eligible body above sync_bytes_ on a worker with a blocking pool is left as
// identity and flagged for offload; every other outcome is final here.
struct http_buffered_compression_step final {
    http_response_compression_status status_;
    bool offload_;
};

// Applies the buffered compression policy without suspending: eligibility,
// size bounds, and encoding up to sync_bytes_. sync_bytes_ is the inline
// ceiling: a larger body is flagged for offload when `offload_available`, and
// otherwise stays identity (not_applicable). Only an offload result requires
// compress_response_offloaded(), so identity and ineligible responses never
// create a coroutine frame. The inline encoder is the worker's reusable one:
// its state stays in worker and only the encoded body comes from the
// response's resource.
[[nodiscard]] http_buffered_compression_step apply_response_compression(
    const http_response_coding_selection& selection, http_known_method request_method,
    http_response& response, const compression_config& options, worker_codecs& codecs, bool offload_available);

// Encodes the body flagged by apply_response_compression() on the worker's
// bounded blocking pool with a codec of its own, since worker codecs never leave the
// worker. Rejection by the pool is an intentional identity fallback;
// encoder/commit failures remain typed failures so a client that forbids
// identity receives a terminal error.
[[nodiscard]] task<http_response_compression_status> compress_response_offloaded(
    http_response_coding_selection selection, http_response& response, const worker_ref& worker_value,
    const stop_token& stop_token_value = {});

// The single decimal Content-Length a handler declared, or nullopt.
[[nodiscard]] std::optional<std::uint64_t> declared_content_length(const http_response& response) noexcept;

// The size rule for a body of `declared` bytes, shared by buffered bodies,
// HEAD metadata and streams whose head declares a length: below min_bytes_
// (or empty) stays identity unless identity is refused, and above max_bytes_
// always stays identity.
[[nodiscard]] bool declared_length_permits_encoding(const http_response_coding_selection& selection,
    const compression_config& options, std::uint64_t declared) noexcept;

// Response compression policy. Fixed representations do not vary by coding,
// except an application-encoded body, whose 200-or-406 outcome does; eligible
// representations are annotated before deciding between identity and an
// encoder. Encoder availability never changes Accept-Encoding parsing.
[[nodiscard]] http_response_compression_decision prepare_response_compression(
    const http_response_coding_selection& selection, http_known_method request_method,
    http_response& response, http_response_compression_source source_value,
    http_response_coding_availability availability);

}  // namespace ruvia::detail

namespace ruvia::detail {

class http_response_coding_policy_disabled final {};

// The response lifecycle has a state that is different from HTTP negotiation
// failure: after a generated terminal error, coding application is deliberately
// disabled. That state stays distinct from the negotiation result, so "406
// negotiation" and "do not transform this error again" are never conflated.
class http_response_coding_policy final {
public:
    [[nodiscard]] static http_response_coding_policy disabled() noexcept {
        return http_response_coding_policy(http_response_coding_policy_disabled{});
    }

    // A failed negotiation stays alive in the selection until a buffered
    // handler has produced its response status: a 204/304 has no response
    // content, so rejecting it before dispatch would turn a representation-free
    // response into a false 406. The selection's identity coding is then only
    // a placeholder that never becomes a silent identity fallback for a
    // response that actually has a body.
    [[nodiscard]] static http_response_coding_policy selected(
        http_response_coding_selection selection) noexcept {
        return http_response_coding_policy(selection);
    }

    [[nodiscard]] const http_response_coding_selection* selection() const& noexcept {
        return std::get_if<http_response_coding_selection>(&value_);
    }
    const http_response_coding_selection* selection() const&& = delete;

private:
    explicit http_response_coding_policy(http_response_coding_policy_disabled disabled) noexcept
        : value_(disabled) {}

    explicit http_response_coding_policy(http_response_coding_selection selection) noexcept
        : value_(selection) {}

    using value_type = std::variant<http_response_coding_selection, http_response_coding_policy_disabled>;
    value_type value_;
};

// A selected non-identity coding is only a promise until the response policy
// actually installs Content-Encoding. In particular, no-transform,
// incompressible media, or an existing file body can leave the response as
// identity. If the client explicitly excluded identity, that policy fallback
// is a 406 outcome; an encoder failure is surfaced separately as a 500.
[[nodiscard]] inline bool http_response_needs_not_acceptable(const http_response_coding_policy& policy,
    const http_request& request, const http_response& response) noexcept {
    const auto* selection = policy.selection();
    if (selection == nullptr) {
        return false;
    }
    return http_response_coding_fallback_forbidden(*selection, request.known_method(), response, &request);
}

[[nodiscard]] inline std::optional<http_error_info> http_buffered_response_preparation_error(
    const http_response_coding_policy& policy, const http_request& request,
    const http_response& response, http_response_compression_status compression) noexcept {
    const auto* selection = policy.selection();
    if (selection != nullptr && selection->coding() != http_content_coding::identity &&
        !selection->identity_accepted() &&
        plan_http_response_body(request.known_method(), response.status()).status_allows_body()) {
        if (compression == http_response_compression_status::failed) {
            return http_error_info({.status_ = http_status::internal_server_error,
                .code_ = "response_compression_failed",
                .message_ = "response compression failed"});
        }
        if (compression == http_response_compression_status::unavailable) {
            return http_error_info({.status_ = http_status::service_unavailable,
                .code_ = "blocking_pool_unavailable",
                .message_ = "blocking pool cannot accept the operation"});
        }
    }
    if (http_response_needs_not_acceptable(policy, request, response)) {
        return http_error_info(http_not_acceptable_coding_error());
    }
    return std::nullopt;
}

enum class buffered_response_recovery_action { ready,
    handle_error,
    prepare_terminal };

struct buffered_response_recovery_step final {
    buffered_response_recovery_action action_{buffered_response_recovery_action::ready};
    std::optional<http_error_info> error_;
};

// A secondary failure preserves the error handler's representation and
// disables coding instead of invoking it again. An error response (4xx/5xx)
// goes straight to that terminal path: it is not a representation of the
// target resource (RFC 9110 15.5.7), so the server may disregard
// Accept-Encoding for it (RFC 9110 12.4.1). Replacing it with a 406 would
// disguise the original failure and run the error handler a second time.
class buffered_response_recovery final {
public:
    [[nodiscard]] bool recovered() const noexcept {
        return stage_ != stage::application;
    }

    [[nodiscard]] buffered_response_recovery_step advance(http_response_coding_policy& policy,
        const http_request& request, const http_response& response,
        http_response_compression_status compression) noexcept {
        if (stage_ == stage::terminal) {
            return {};
        }
        auto error = http_buffered_response_preparation_error(policy, request, response, compression);
        if (!error.has_value()) {
            return {};
        }
        if (stage_ == stage::application && !response.status().is_error()) {
            stage_ = stage::negotiated_error;
            return {buffered_response_recovery_action::handle_error, std::move(error)};
        }
        policy = http_response_coding_policy::disabled();
        stage_ = stage::terminal;
        return {buffered_response_recovery_action::prepare_terminal, std::nullopt};
    }

private:
    enum class stage { application,
        negotiated_error,
        terminal };
    stage stage_{stage::application};
};

struct prepared_application_response final {
    http_buffered_response_write_plan write_plan_;
    bool recovered_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class connection_lifecycle;

// Activated after the CONNECT head has been flushed and before tunnel I/O starts.
// Only the TLS write BIO changes: Asio retains its read BIO and buffered input.
// Every ciphertext producer, including TLS control messages generated by reads,
// feeds one bounded queue and one socket writer. SSL_shutdown sends close_notify
// once; flush() waits for that alert without consuming the peer's remaining data.
class tls_tunnel_output final {
public:
    tls_tunnel_output(SSL& ssl, asio::ip::tcp::socket& socket, const worker_ref& worker_value,
        connection_lifecycle* lifecycle = nullptr);
    tls_tunnel_output(SSL&, asio::ip::tcp::socket&, worker_ref&&, connection_lifecycle* = nullptr) = delete;
    ~tls_tunnel_output();
    tls_tunnel_output(const tls_tunnel_output&) = delete;
    tls_tunnel_output& operator=(const tls_tunnel_output&) = delete;
    void start();
    [[nodiscard]] task<std::error_code> flush();
    [[nodiscard]] task<std::error_code> finish();
    void abort() noexcept;
    [[nodiscard]] task<void> join();

private:
    static const std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> bio_method_;
    static int create_bio(BIO* bio) noexcept;
    static int destroy_bio(BIO* bio) noexcept;
    static int write_bio(BIO* bio, const char* bytes, std::size_t size, std::size_t* written) noexcept;
    static long control_bio(BIO*, int command, long, void*) noexcept;
    [[nodiscard]] task<void> run_writer();
    [[nodiscard]] task<void> run_writer_inner();
    void fail(std::error_code error) noexcept;

    SSL& ssl_;
    asio::ip::tcp::socket& socket_;
    const worker_ref& worker_;
    connection_lifecycle* lifecycle_;
    BIO* previous_{nullptr};
    std::vector<char> bytes_;
    std::size_t head_{0};
    std::size_t size_{0};
    std::error_code error_{};
    bool started_{false};
    bool stopping_{false};
    bool send_ended_{false};
    worker_signal available_;
    worker_signal flushed_;
    task_scope tasks_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

template <typename stream_type>
class http_socket_tunnel_transport final {
public:
    // `write_scanner_entry` is the server connection's scanner entry: every
    // write runs its write stall clock, so write_timeout closes a tunnel
    // whose peer stopped reading while the reader stays long_lived. Null for
    // an owner that bounds its writes itself (the client's operation timeout).
    http_socket_tunnel_transport(
        stream_type& stream, tls_tunnel_output* tls_output, connection_scanner::entry* write_scanner_entry)
        : stream_(stream),
          tls_output_(tls_output),
          write_scanner_entry_(write_scanner_entry) {
        if constexpr (requires { stream_.next_layer(); }) {
            if (tls_output_ == nullptr) {
                throw std::logic_error("TLS tunnel requires its ciphertext writer");
            }
        }
    }
    // Appends up to 4096 bytes read from the stream to `bytes_value`. A caller
    // that clears and reuses one string reads without allocating after its
    // first read.
    [[nodiscard]] task<bool> read_more(std::string& bytes_value, inbound_buffer_reservation* reservation = nullptr) {
        return read_more(bytes_value, []() noexcept {}, asio::cancellation_slot{}, reservation);
    }

    // A cancellation emitted on `slot` ends only this transport read and
    // appends nothing, after which `check` may throw to abandon it.
    template <typename check_type>
    [[nodiscard]] task<bool> read_more(std::string& bytes_value, check_type check, asio::cancellation_slot slot,
        inbound_buffer_reservation* reservation = nullptr) {
        constexpr std::size_t read_bytes = 4096;
        const auto old_size = bytes_value.size();
        if (reservation != nullptr) {
            inbound_resize(bytes_value, *reservation, old_size + read_bytes);
        } else {
            bytes_value.resize(old_size + read_bytes);
        }
        const auto result_value = co_await async_asio<std::size_t>([&](auto handler) {
            stream_.async_read_some(asio::buffer(bytes_value.data() + old_size, read_bytes),
                asio::bind_cancellation_slot(slot, std::move(handler)));
        });
        if (reservation != nullptr) {
            inbound_resize(bytes_value, *reservation, old_size + result_value.result());
        } else {
            bytes_value.resize(old_size + result_value.result());
        }
        // A TLS peer that closed TCP without close_notify ends the tunnel byte
        // stream exactly like an orderly FIN or close_notify.
        if (is_stream_read_end(result_value.error_code())) {
            co_return false;
        }
        if (result_value.error_code()) {
            if (result_value.error_code() == asio::error::operation_aborted) {
                check();
            }
            throw inbound_transport_error(result_value.error_code(), "HTTP tunnel read");
        }
        co_return result_value.result() != 0;
    }

    // A cancellation emitted on `slot` ends only this transport read (a TLS
    // stream shields its record writes), after which `check` may throw to
    // abandon it.
    template <typename check_type>
    [[nodiscard]] task<bool> read_more(http_io_buffer& bytes, check_type check, asio::cancellation_slot slot,
        inbound_buffer_reservation* reservation = nullptr) {
        const auto writable = reservation != nullptr ? inbound_prepare(bytes, *reservation, 4096) : bytes.prepare(4096);
        const auto result = co_await async_asio<std::size_t>([&](auto completion) {
            stream_.async_read_some(asio::buffer(writable.data(), writable.size()),
                asio::bind_cancellation_slot(slot, std::move(completion)));
        });
        bytes.commit(result.result());
        if (is_stream_read_end(result.error_code())) {
            co_return false;
        }
        if (result.error_code()) {
            if (result.error_code() == asio::error::operation_aborted) {
                check();
            }
            throw inbound_transport_error(result.error_code(), "WebSocket read");
        }
        co_return result.result() != 0;
    }
    // `completed_bytes` receives the plaintext prefix accepted by the stream,
    // including progress reported with the terminal error.
    [[nodiscard]] task<std::error_code> write_bytes(
        std::string_view bytes_value, http_stream_end end, std::size_t* completed_bytes = nullptr) {
        if (completed_bytes != nullptr) {
            *completed_bytes = 0;
        }
        std::optional<connection_scanner::entry::write_scope> write_stall_clock;
        if (write_scanner_entry_ != nullptr) {
            write_stall_clock.emplace(*write_scanner_entry_);
        }
        if constexpr (!requires { stream_.next_layer(); }) {
            if (!bytes_value.empty()) {
                const auto result_value = co_await async_asio<std::size_t>([&](auto handler) {
                    if (write_scanner_entry_ != nullptr) {
                        asio::async_write(stream_, asio::buffer(bytes_value),
                            connection_write_progress(*write_scanner_entry_), std::move(handler));
                    } else {
                        asio::async_write(stream_, asio::buffer(bytes_value), std::move(handler));
                    }
                });
                if (completed_bytes != nullptr) {
                    *completed_bytes = result_value.result();
                }
                if (result_value.error_code()) {
                    co_return result_value.error_code();
                }
            }
        } else {
            // The ciphertext writer drains each chunk before the next is
            // encrypted, so a flushed chunk is the unit of progress.
            while (!bytes_value.empty()) {
                const auto chunk = bytes_value.substr(0, 4096);
                const auto result_value = co_await async_asio<std::size_t>([&](auto handler) {
                    asio::async_write(stream_, asio::buffer(chunk), std::move(handler));
                });
                if (completed_bytes != nullptr) {
                    *completed_bytes += result_value.result();
                }
                if (result_value.error_code()) {
                    co_return result_value.error_code();
                }
                if (const auto error = co_await tls_output_->flush()) {
                    co_return error;
                }
                if (write_scanner_entry_ != nullptr) {
                    write_scanner_entry_->note_write_progress();
                }
                bytes_value.remove_prefix(chunk.size());
            }
        }
        if (end == http_stream_end::end) {
            if constexpr (requires { stream_.next_layer(); }) {
                co_return co_await tls_output_->finish();
            } else {
                std::error_code error;
                stream_.shutdown(asio::ip::tcp::socket::shutdown_send, error);
                co_return error;
            }
        }
        co_return std::error_code{};
    }
    // HTTP/1.1 has no per-message reset: malformed tunnel data closes the
    // connection like any other abort.
    void abort(bool /*malformed*/ = false) noexcept {
        if constexpr (requires { stream_.next_layer(); }) {
            tls_output_->abort();
        } else {
            close_socket(stream_.lowest_layer());
        }
    }

private:
    stream_type& stream_;
    tls_tunnel_output* tls_output_;
    connection_scanner::entry* write_scanner_entry_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {
// A tunnel over an HTTP/1 socket transport or one HTTP/2 or HTTP/3 stream. Received
// chunks charge the connection's and worker's inbound buffer budgets.
template <typename transport_type>
class http_tunnel_session final {
public:
    http_tunnel_session(transport_type transport, const worker_ref& worker_value,
        inbound_buffer_account* inbound, bool datagrams, std::string_view pending = {})
        : transport_(std::move(transport)),
          inbound_(inbound),
          read_reservation_(inbound),
          pending_reservation_(inbound),
          delivered_reservation_(inbound),
          facade_(inbound, worker_value, this, &read_thunk, &write_thunk, &finish_thunk, &abort_thunk, datagrams) {
        if (!pending.empty()) {
            inbound_append(pending_, pending_reservation_, pending);
        }
        if constexpr (requires(transport_type& value, std::string_view bytes) {
                          value.read_datagram([] {}, asio::cancellation_slot{});
                          value.write_datagram(bytes);
                      }) {
            facade_.read_datagram_ = &read_datagram_thunk;
            facade_.write_datagram_ = &write_datagram_thunk;
        }
    }
    http_tunnel_session(transport_type, worker_ref&&, inbound_buffer_account*, bool, std::string_view = {}) = delete;
    [[nodiscard]] http_tunnel& tunnel() noexcept {
        return facade_;
    }
    void abort(bool malformed = false) noexcept {
        aborted_ = true;
        transport_.abort(malformed);
    }
    [[nodiscard]] task<void> finish(operation_options operation = {}) {
        if (!ended_) {
            operation_interrupt interrupt(interrupts_, facade_.worker_, operation, this, &abort_thunk_default, "HTTP tunnel finish");
            const auto error = co_await transport_.write_bytes({}, http_stream_end::end);
            interrupt.throw_if_interrupted();
            if (error) {
                throw inbound_transport_error(error, "HTTP tunnel finish");
            }
            ended_ = true;
        }
    }
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_;
    }
    [[nodiscard]] task<void> drain() {
        delivered_reservation_.clear();
        while (!receive_ended_) {
            receive_ended_ = !(co_await read_some());
        }
    }
    [[nodiscard]] task<void> join() {
        if (facade_.read_active_ || facade_.write_active_) {
            abort();
        }
        co_await facade_.operations_.close_and_join();
    }

private:
    [[nodiscard]] task<bool> read_some() {
        if (!pending_.empty()) {
            inbound_discard(pending_, pending_reservation_);
            co_return true;
        }
        read_buffer_.clear();
        if constexpr (requires { transport_.read_more(read_buffer_, &read_reservation_); }) {
            co_return co_await transport_.read_more(read_buffer_, &read_reservation_);
        } else {
            co_return co_await transport_.read_more(read_buffer_);
        }
    }
    static void wake_read(void* raw) noexcept {
        static_cast<asio::cancellation_signal*>(raw)->emit(asio::cancellation_type::terminal);
    }
    static task<std::optional<std::string>> read_thunk(void* raw, operation_options operation) {
        auto& owner_value = *static_cast<http_tunnel_session*>(raw);
        owner_value.delivered_reservation_.clear();
        operation_interrupt interrupt(owner_value.interrupts_, owner_value.facade_.worker_, operation,
            &owner_value.read_cancellation_, &wake_read, "HTTP tunnel read");
        if (!owner_value.pending_.empty()) {
            owner_value.delivered_reservation_.swap(owner_value.pending_reservation_);
            co_return std::exchange(owner_value.pending_, std::string{});
        }
        if (owner_value.receive_ended_) {
            co_return std::nullopt;
        }
        owner_value.read_buffer_.clear();
        bool has_more = false;
        if constexpr (requires(transport_type& t, std::string& b, asio::cancellation_slot sl, inbound_buffer_reservation* r) { t.read_more(b, [] {}, sl, r); }) {
            has_more = co_await owner_value.transport_.read_more(
                owner_value.read_buffer_, [&interrupt] { interrupt.throw_if_interrupted(); },
                owner_value.read_cancellation_.slot(), &owner_value.read_reservation_);
        } else {
            has_more = co_await owner_value.transport_.read_more(
                owner_value.read_buffer_, [&interrupt] { interrupt.throw_if_interrupted(); },
                owner_value.read_cancellation_.slot());
        }
        if (!has_more) {
            owner_value.receive_ended_ = true;
            co_return std::nullopt;
        }
        std::string chunk;
        inbound_append(chunk, owner_value.delivered_reservation_, owner_value.read_buffer_);
        co_return chunk;
    }
    static task<void> write_thunk(void* raw, std::string_view bytes_value, operation_options operation) {
        auto& owner_value = *static_cast<http_tunnel_session*>(raw);
        if (owner_value.ended_) {
            throw std::logic_error("HTTP tunnel send direction has ended");
        }
        operation_interrupt interrupt(owner_value.interrupts_, owner_value.facade_.worker_, operation, raw,
            &abort_thunk_default, "HTTP tunnel write");
        const auto error = co_await owner_value.transport_.write_bytes(bytes_value, http_stream_end::keep_open);
        interrupt.throw_if_interrupted();
        if (error) {
            throw inbound_transport_error(error, "HTTP tunnel write");
        }
    }
    static task<std::optional<http_tunnel_input>> read_datagram_thunk(void* raw, operation_options operation) {
        auto& owner_value = *static_cast<http_tunnel_session*>(raw);
        owner_value.delivered_reservation_.clear();
        operation_interrupt interrupt(owner_value.interrupts_, owner_value.facade_.worker_, operation,
            &owner_value.read_cancellation_, &wake_read, "HTTP tunnel read");
        if (!owner_value.pending_.empty()) {
            owner_value.delivered_reservation_.swap(owner_value.pending_reservation_);
            co_return http_tunnel_input{std::exchange(owner_value.pending_, std::string{}), false};
        }
        if (owner_value.receive_ended_) {
            co_return std::nullopt;
        }
        auto input = co_await owner_value.transport_.read_datagram(
            [&interrupt] { interrupt.throw_if_interrupted(); }, owner_value.read_cancellation_.slot());
        if (!input) {
            owner_value.receive_ended_ = true;
        }
        co_return input;
    }
    static task<bool> write_datagram_thunk(void* raw, std::string_view bytes, operation_options operation) {
        auto& owner_value = *static_cast<http_tunnel_session*>(raw);
        if (owner_value.ended_) {
            throw std::logic_error("HTTP tunnel send direction has ended");
        }
        operation_interrupt interrupt(owner_value.interrupts_, owner_value.facade_.worker_, operation, raw,
            &abort_thunk_default, "HTTP tunnel write");
        try {
            const bool sent = co_await owner_value.transport_.write_datagram(bytes);
            interrupt.throw_if_interrupted();
            co_return sent;
        } catch (...) {
            interrupt.throw_if_interrupted();
            throw;
        }
    }
    static task<void> finish_thunk(void* raw, operation_options operation) {
        return static_cast<http_tunnel_session*>(raw)->finish(std::move(operation));
    }
    static void abort_thunk(void* raw, bool malformed) noexcept {
        static_cast<http_tunnel_session*>(raw)->abort(malformed);
    }
    static void abort_thunk_default(void* raw) noexcept {
        static_cast<http_tunnel_session*>(raw)->abort();
    }
    transport_type transport_;
    inbound_buffer_account* inbound_;
    std::string read_buffer_;
    inbound_buffer_reservation read_reservation_;
    std::string pending_;
    inbound_buffer_reservation pending_reservation_;
    inbound_buffer_reservation delivered_reservation_;
    bool ended_{false};
    bool receive_ended_{false};
    bool aborted_{false};
    asio::cancellation_signal read_cancellation_;
    operation_interrupt_source interrupts_;
    http_tunnel facade_;
};

template <typename transport_type>
[[nodiscard]] task<void> invoke_tunnel_handler(http_tunnel_session<transport_type>& session_value,
    connection_scanner::entry& entry_value, const route_stream_handler& handler, context& context_value) {
    context_tunnel_binding binding(context_value, session_value.tunnel());
    entry_value.enter_long_lived();
    co_await handler(context_value);
}

template <typename transport_type>
[[nodiscard]] task<void> finish_tunnel_session(http_tunnel_session<transport_type>& session_value,
    std::exception_ptr failure, const connection_failure_sink& sink_value, std::string_view remote,
    connection_scanner::entry& scanner, timeout_duration timeout) {
    if (failure != nullptr) {
        session_value.abort();
    } else if (!session_value.aborted()) {
        struct drain_deadline final {
            http_tunnel_session<transport_type>& session_;
            std::int64_t deadline_;
            connection_scanner::periodic_check_registration registration_;
            static void tick(void* raw, std::int64_t now) noexcept {
                auto& self = *static_cast<drain_deadline*>(raw);
                if (now >= self.deadline_) {
                    self.session_.abort();
                }
            }
        } deadline{session_value, steady_now_ms() + timeout.value().count(), {}};
        scanner.register_periodic_check(deadline.registration_, &deadline, &drain_deadline::tick);
        try {
            co_await session_value.finish();
            co_await session_value.drain();
        } catch (...) {
            session_value.abort();
        }
    }
    co_await session_value.join();
    if (failure != nullptr) {
        sink_value.invoke(remote, failure);
        std::rethrow_exception(failure);
    }
}
}  // namespace ruvia::detail

namespace ruvia::detail {

// The one lifecycle check of server routes and clients.
inline void validate_websocket_lifecycle_options(const websocket_lifecycle_options& options) {
    if (options.close_handshake_timeout_.has_value() && options.close_handshake_timeout_->value().count() <= 0) {
        throw std::invalid_argument("websocket close-handshake timeout must be greater than zero");
    }
    const auto& heartbeat = options.heartbeat_;
    if (!heartbeat.ping_interval_.has_value()) {
        if (heartbeat.pong_timeout_.has_value()) {
            throw std::invalid_argument("websocket pong timeout requires a ping interval");
        }
        return;
    }
    if (heartbeat.ping_interval_->value().count() <= 0 ||
        (heartbeat.pong_timeout_.has_value() && heartbeat.pong_timeout_->value().count() <= 0)) {
        throw std::invalid_argument("websocket heartbeat intervals must be greater than zero");
    }
}

// Validated options with an absent pong timeout defaulted to the ping interval.
[[nodiscard]] inline websocket_lifecycle_options normalize_websocket_lifecycle_options(
    websocket_lifecycle_options options) {
    validate_websocket_lifecycle_options(options);
    auto& heartbeat = options.heartbeat_;
    if (heartbeat.ping_interval_.has_value() && !heartbeat.pong_timeout_.has_value()) {
        heartbeat.pong_timeout_ = heartbeat.ping_interval_;
    }
    return options;
}

enum class websocket_liveness_decision : std::uint8_t {
    idle,
    send_ping,
    abort_transport,
};

class websocket_liveness_idle final {};

// The ping write and its Pong wait are separate states. A Pong may arrive as
// soon as the transport accepts the bytes and before the write coroutine is
// resumed, so the in-flight state must be visible to the reader without
// starting the timeout prematurely.
struct websocket_sending_ping final {
    std::uint64_t challenge_;
};

struct websocket_awaiting_pong final {
    std::int64_t sent_at_ms_;
    std::uint64_t challenge_;
};

struct websocket_awaiting_peer_close final {
    std::int64_t started_at_ms_;
};

using websocket_liveness_state_type = std::variant<websocket_liveness_idle, websocket_sending_ping,
    websocket_awaiting_pong, websocket_awaiting_peer_close>;

[[nodiscard]] inline std::array<char, 8> websocket_heartbeat_payload(std::uint64_t challenge) noexcept {
    std::array<char, 8> payload_value{};
    for (std::size_t i = 0; i < payload_value.size(); ++i) {
        payload_value[i] = static_cast<char>(challenge >> ((payload_value.size() - 1 - i) * 8));
    }
    return payload_value;
}

[[nodiscard]] inline bool websocket_heartbeat_pong_matches(
    const websocket_liveness_state_type& state_value, std::string_view payload_value) noexcept {
    std::uint64_t challenge;
    if (const auto* sending = std::get_if<websocket_sending_ping>(&state_value)) {
        challenge = sending->challenge_;
    } else if (const auto* awaiting = std::get_if<websocket_awaiting_pong>(&state_value)) {
        challenge = awaiting->challenge_;
    } else {
        return false;
    }
    const auto expected = websocket_heartbeat_payload(challenge);
    return payload_value == std::string_view(expected.data(), expected.size());
}

// The Pong wait is measured only while the reader can poll it. Restarting the
// window when that becomes true (or at every tick while it is false) keeps a
// Pong that was already buffered during a write from being timed out before
// the reader's next poll gets to it.
inline void restart_websocket_pong_window(
    websocket_liveness_state_type& state_value, std::int64_t now) noexcept {
    if (const auto* pong = std::get_if<websocket_awaiting_pong>(&state_value)) {
        state_value = websocket_awaiting_pong{now, pong->challenge_};
    }
}

inline void resume_websocket_pong_deadline(const websocket_lifecycle_options& options,
    websocket_liveness_state_type& state_value, bool pong_observable) noexcept {
    if (const auto* pong = std::get_if<websocket_awaiting_pong>(&state_value);
        pong != nullptr && pong_observable && options.heartbeat_.pong_timeout_.has_value()) {
        const auto now = steady_now_ms();
        if (now - pong->sent_at_ms_ >= options.heartbeat_.pong_timeout_->value().count()) {
            state_value = websocket_awaiting_pong{now, pong->challenge_};
        }
    }
}

// pong_observable: an application read is polling the core and no write holds
// the lane, so a Pong that arrived can actually be seen. Only then does the
// Pong wait count; a peer that answered during a long write, or while no read
// is pending, is not dead (RFC 6455 section 5.5.3 only asks the peer to answer
// promptly). A write that stops making progress is still caught below.
[[nodiscard]] inline websocket_liveness_decision evaluate_websocket_liveness(
    const websocket_lifecycle_options& options, websocket_liveness_mode liveness_mode,
    const websocket_liveness_state_type& state_value, bool write_active, bool pong_observable,
    std::int64_t last_active_ms, std::int64_t now) noexcept {
    if (liveness_mode != websocket_liveness_mode::open) {
        const auto* close = std::get_if<websocket_awaiting_peer_close>(&state_value);
        return (liveness_mode == websocket_liveness_mode::awaiting_peer_close ||
                   liveness_mode == websocket_liveness_mode::awaiting_peer_eof) &&
                       close != nullptr &&
                       options.close_handshake_timeout_.has_value() &&
                       now - close->started_at_ms_ >= options.close_handshake_timeout_->value().count()
                   ? websocket_liveness_decision::abort_transport
                   : websocket_liveness_decision::idle;
    }

    if (!options.heartbeat_.ping_interval_.has_value()) {
        return websocket_liveness_decision::idle;
    }

    const auto ping_interval = options.heartbeat_.ping_interval_->value().count();
    const auto pong_timeout = options.heartbeat_.pong_timeout_->value().count();
    const auto* pong = std::get_if<websocket_awaiting_pong>(&state_value);
    if (pong != nullptr && pong_observable) {
        return now - pong->sent_at_ms_ >= pong_timeout ? websocket_liveness_decision::abort_transport
                                                       : websocket_liveness_decision::idle;
    }
    // A write that holds the lane (including a heartbeat Ping) blocks the next
    // Ping, so a peer that neither reads nor sends would never be probed. Give
    // such a stall the same budget as an unanswered Ping. The two budgets are
    // compared in stages: their sum can overflow for long configured values.
    const auto idle_for = now - last_active_ms;
    if (write_active && idle_for >= ping_interval && idle_for - ping_interval >= pong_timeout) {
        return websocket_liveness_decision::abort_transport;
    }
    if (!std::holds_alternative<websocket_liveness_idle>(state_value)) {
        return websocket_liveness_decision::idle;
    }
    if (idle_for < ping_interval || write_active) {
        return websocket_liveness_decision::idle;
    }
    return websocket_liveness_decision::send_ping;
}

// A stalled writer's deadline is last activity + ping interval + pong timeout,
// without overflowing that sum or restarting the pong portion on intermediate
// timer ticks.
[[nodiscard]] inline std::chrono::milliseconds websocket_heartbeat_delay(
    const websocket_lifecycle_options& options, const websocket_liveness_state_type& state,
    bool writing, std::int64_t last_active_ms, std::int64_t now) noexcept {
    const auto remaining = [now](std::int64_t started, std::int64_t budget) {
        const auto elapsed = std::max<std::int64_t>(0, now - started);
        return elapsed >= budget ? std::int64_t{0} : budget - elapsed;
    };
    std::int64_t delay = 1;
    if (writing) {
        const auto elapsed = std::max<std::int64_t>(0, now - last_active_ms);
        const auto ping = options.heartbeat_.ping_interval_->value().count();
        const auto pong = options.heartbeat_.pong_timeout_->value().count();
        if (elapsed >= ping) {
            delay = elapsed - ping >= pong ? 0 : pong - (elapsed - ping);
        } else {
            const auto until_ping = ping - elapsed;
            delay = pong > std::numeric_limits<std::int64_t>::max() - until_ping
                        ? std::numeric_limits<std::int64_t>::max()
                        : until_ping + pong;
        }
    } else if (const auto* pong = std::get_if<websocket_awaiting_pong>(&state)) {
        delay = remaining(pong->sent_at_ms_, options.heartbeat_.pong_timeout_->value().count());
    } else if (std::holds_alternative<websocket_liveness_idle>(state)) {
        delay = remaining(last_active_ms, options.heartbeat_.ping_interval_->value().count());
    }
    return std::chrono::milliseconds(std::max<std::int64_t>(1, delay));
}

class websocket_peer_close_info final {
public:
    void record(const websocket_close_event& close_event) noexcept {
        code_ = close_event.close_code();
        const auto reason_value = close_event.reason();
        const auto count = (std::min)(reason_value.size(), reason_.size());
        if (count != 0) {
            std::char_traits<char>::copy(reason_.data(), reason_value.data(), count);
        }
        reason_size_ = static_cast<std::uint8_t>(count);
    }

    [[nodiscard]] std::optional<std::uint16_t> code() const noexcept {
        return code_;
    }

    [[nodiscard]] std::string_view reason() const& noexcept RUVIA_LIFETIMEBOUND {
        return {reason_.data(), reason_size_};
    }
    std::string_view reason() const&& = delete;

private:
    std::optional<std::uint16_t> code_{};
    std::array<char, 123> reason_{};
    std::uint8_t reason_size_{0};
};

class websocket_write_lane final {
public:
    explicit websocket_write_lane(const worker_ref& worker_value)
        : signal_(worker_value) {}
    websocket_write_lane(worker_ref&&) = delete;

    [[nodiscard]] bool write_in_progress() const noexcept {
        return write_in_progress_;
    }

    [[nodiscard]] bool reader_waiting() const noexcept {
        return reader_waiting_;
    }

    [[nodiscard]] bool blocked_for_writer() const noexcept {
        return write_in_progress_ || reader_waiting_;
    }

    void begin() noexcept {
        if (write_in_progress_) {
            std::terminate();
        }
        write_in_progress_ = true;
    }

    void release() noexcept {
        if (!write_in_progress_) {
            std::terminate();
        }
        write_in_progress_ = false;
    }

    void finish() noexcept {
        release();
        signal_.notify();
    }

    void notify() noexcept {
        signal_.notify();
    }

    [[nodiscard]] worker_signal::wait_operation wait() {
        return signal_.wait();
    }

    template <typename check_type>
    [[nodiscard]] task<void> wait_for_idle(bool for_reader, check_type check) {
        try {
            for (;;) {
                check();
                if (!write_in_progress_ && (for_reader || !reader_waiting_)) {
                    if (for_reader) {
                        // The caller claims the lane before any writer resumes.
                        reader_waiting_ = false;
                    }
                    co_return;
                }
                if (for_reader) {
                    reader_waiting_ = true;
                }
                co_await signal_.wait();
            }
        } catch (...) {
            if (for_reader) {
                reader_waiting_ = false;
                signal_.notify();
            }
            throw;
        }
    }

    [[nodiscard]] task<void> wait_for_idle(bool for_reader) {
        return wait_for_idle(for_reader, []() noexcept {});
    }

private:
    bool write_in_progress_{false};
    bool reader_waiting_{false};
    worker_signal signal_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Address-stable lane leases and capability scope shared by both endpoint roles.
// The client also retains its transport owner; server transports remain borrowed.
class websocket_state final : public std::enable_shared_from_this<websocket_state> {
public:
    using read_type = task<std::optional<websocket_message>> (*)(void*, operation_options);
    using write_type = task<void> (*)(void*, websocket_opcode, std::string_view, bool, websocket_write_extent, operation_options);
    using close_type = task<void> (*)(void*, websocket_close_options, operation_options);
    using abort_type = void (*)(void*) noexcept;
    using peer_close_code_type = std::optional<std::uint16_t> (*)(void*) noexcept;
    using peer_close_reason_type = std::string_view (*)(void*) noexcept;
    using protocol_type = const websocket_connection& (*)(void*) noexcept;

    websocket_state(const worker_ref& worker, void* target,
        std::string_view subprotocol, read_type read, write_type write, close_type close,
        abort_type abort, peer_close_code_type peer_close_code, peer_close_reason_type peer_close_reason,
        protocol_type protocol) noexcept
        : worker_storage_(worker),
          worker_(&worker_storage_),
          target_(target),
          subprotocol_(subprotocol),
          read_(read),
          write_(write),
          close_(close),
          abort_(abort),
          peer_close_code_(peer_close_code),
          peer_close_reason_(peer_close_reason),
          protocol_(protocol) {
        operation_scope_.bind_worker(worker_storage_);
    }
    websocket_state(const websocket_state&) = delete;
    websocket_state& operator=(const websocket_state&) = delete;
    ~websocket_state() {
        if (operation_scope_.has_pending_operations() && !worker_->is_current()) {
            std::terminate();
        }
    }

    void require_active() const {
        if (!worker_->is_current()) {
            std::terminate();
        }
        if (!operation_scope_.active()) {
            throw std::logic_error("websocket lifetime has expired");
        }
    }
    [[nodiscard]] task<std::optional<websocket_message>> read(operation_options) &;
    task<void> text(std::string_view, websocket_send_options, operation_options) &;
    task<void> binary(std::string_view, websocket_send_options, operation_options) &;
    task<void> binary(std::span<const std::byte>, websocket_send_options, operation_options) &;
    task<void> pong(std::string_view, operation_options) &;
    task<void> ping(std::string_view, operation_options) &;
    task<void> send(websocket_message&&, websocket_send_options, operation_options) &;
    task<void> close(websocket_close_options, operation_options) &;
    void abort() & noexcept;
    websocket_message_writer begin_text(websocket_send_options) &;
    websocket_message_writer begin_binary(websocket_send_options) &;
    websocket_message_writer begin_message(websocket_opcode, bool);
    task<void> write(websocket_opcode, std::string_view, bool, operation_options, websocket_write_extent = websocket_write_extent::message);
    task<void> write(websocket_opcode, std::string&&, bool, operation_options, websocket_write_extent = websocket_write_extent::message);

    [[nodiscard]] std::string_view subprotocol() const noexcept {
        if (!worker_->is_current()) {
            std::terminate();
        }
        return operation_scope_.active() ? subprotocol_ : std::string_view{};
    }
    [[nodiscard]] std::optional<std::uint16_t> peer_close_code() const noexcept;
    [[nodiscard]] std::string_view peer_close_reason() const& noexcept;
    [[nodiscard]] websocket_termination termination() const noexcept;
    [[nodiscard]] std::uint64_t bytes_received() const noexcept;
    [[nodiscard]] std::uint64_t bytes_sent() const noexcept;

    worker_ref worker_storage_;
    const worker_ref* worker_;
    void* target_;
    std::string_view subprotocol_;
    read_type read_;
    write_type write_;
    close_type close_;
    abort_type abort_;
    peer_close_code_type peer_close_code_;
    peer_close_reason_type peer_close_reason_;
    protocol_type protocol_;
    bool read_active_{false};
    bool write_active_{false};
    bool message_send_active_{false};
    bool fragmented_message_active_{false};
    std::uint64_t fragmented_message_sequence_{0};
    operation_options defaults_;
    std::optional<timeout_duration> read_timeout_;
    std::shared_ptr<void> owner_;
    void (*retire_)(void*) noexcept {nullptr};
    operation_scope operation_scope_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// RFC 6455 connection over an HTTP/1 socket or HTTP/2 stream transport.
// Frame reassembly, serialization, heartbeat and close policy are shared;
// stream transports provide executor/read_more/write_bytes/abort operations.
template <typename stream_type>
class websocket_session_connection final {
public:
    static constexpr bool native_stream = requires(stream_type& stream) { stream.get_executor(); };
    using stream_argument_type = std::conditional_t<native_stream, stream_type&, stream_type>;

    websocket_session_connection(stream_argument_type stream, const worker_ref& worker_value,
        connection_scanner::entry& scanner_entry, websocket_lifecycle_options lifecycle_options,
        protocol_byte_limit message_limit, std::string_view initial_bytes,
        websocket_connection_options protocol_options, std::string&& subprotocol_value,
        inbound_buffer_account* inbound = nullptr)
        : stream_(std::forward<stream_argument_type>(stream)),
          worker_(&worker_value),
          scanner_entry_(scanner_entry),
          lifecycle_options_(lifecycle_options),
          buffer_reservation_(inbound),
          protocol_(buffer_, [&] {
              protocol_options.message_limit_ = message_limit;
              return protocol_options;
          }()),
          subprotocol_(std::move(subprotocol_value)),
          write_lane_(worker_value),
          reader_done_signal_(worker_value),
          background_tasks_(worker_value) {
        try {
            inbound_append(buffer_, buffer_reservation_, initial_bytes);
        } catch (...) {
            close_stream();
            throw;
        }
        scanner_entry_.register_periodic_check(periodic_check_, this, &websocket_session_connection::heartbeat_tick_thunk);
    }

    websocket_session_connection(stream_argument_type, worker_ref&&, connection_scanner::entry&, websocket_lifecycle_options,
        protocol_byte_limit, std::string_view, websocket_connection_options,
        std::string&&, inbound_buffer_account* = nullptr) = delete;
    static void close_stream(stream_type& stream) noexcept {
        if constexpr (!native_stream) {
            stream.abort();
        } else {
            ruvia::close_socket(stream.lowest_layer());
        }
    }
    ~websocket_session_connection() {
        if (facade_) {
            facade_->operation_scope_.close();
        }
        if (cancellation_target_) {
            cancellation_target_->detach(*this);
        }
    }

    websocket_session_connection(const websocket_session_connection&) = delete;
    websocket_session_connection& operator=(const websocket_session_connection&) = delete;

    static void heartbeat_tick_thunk(void* target, std::int64_t now) noexcept {
        static_cast<websocket_session_connection*>(target)->heartbeat_tick(now);
    }

    // A cancelled or timed-out read ends only its own wait: buffered input,
    // the read lane and the session stay intact for the next read.
    [[nodiscard]] task<std::optional<websocket_message>> read(operation_options options = {});
    // A cancelled or timed-out output operation aborts the session, since a
    // partly written frame cannot be withdrawn.
    task<void> write(websocket_opcode opcode, std::string_view payload, bool compress, websocket_write_extent extent,
        operation_options options);
    // Claims only the write lane. A read already pending observes the peer
    // Close and returns nullopt; otherwise close() reads until it arrives.
    task<void> close(::ruvia::websocket_close_options options = {}, operation_options operation = {});

    // Delivered on this worker by an operation's stop token.
    void cancel_operation_by_id(std::uint64_t id) noexcept {
        if (id == 0) {
            return;
        }
        if (id == read_operation_id_) {
            interrupt(operation_lane::read, operation_interrupt::cancelled);
        } else if (id == write_operation_id_) {
            interrupt(operation_lane::write, operation_interrupt::cancelled);
        }
    }
    void abort() noexcept {
        if (!worker_->is_current()) {
            std::terminate();
        }
        abort_transport(websocket_termination::aborted);
    }

    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND {
        return *worker_;
    }
    const worker_ref& worker() const&& = delete;

    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] std::optional<std::uint16_t> peer_close_code() const noexcept {
        return peer_close_.code();
    }

    [[nodiscard]] std::string_view peer_close_reason() const& noexcept RUVIA_LIFETIMEBOUND {
        return peer_close_.reason();
    }
    std::string_view peer_close_reason() const&& = delete;

    // The protocol core is the single owner of the termination cause and the
    // wire byte counters.
    [[nodiscard]] const websocket_connection& protocol() const& noexcept RUVIA_LIFETIMEBOUND {
        return protocol_;
    }
    const websocket_connection& protocol() const&& = delete;

    void require_current_worker() const noexcept {
        if (!worker_->is_current()) {
            std::terminate();
        }
    }
    void bind_facade(const std::shared_ptr<websocket_state>& facade) noexcept {
        facade_ = facade;
    }
    [[nodiscard]] stream_type& transport() & noexcept {
        return stream_;
    }
    task<void> detach_and_drain_writes();
    // The failure of a heartbeat write. Those writes run detached
    // from any caller, so the session reports it once detach_and_drain_writes()
    // has aborted the transport and joined them.
    [[nodiscard]] std::exception_ptr take_background_failure() noexcept {
        return std::exchange(background_failure_, nullptr);
    }

private:
    enum class write_claim_type : std::uint8_t {
        acquire,
        adopt,
    };

    enum class read_phase_type : std::uint8_t {
        idle,
        reserved,
        active,
    };

    enum class operation_lane : std::uint8_t {
        read,
        write,
    };

    enum class operation_interrupt : std::uint8_t {
        cancelled,
        timed_out,
    };

    // Unwinds an interrupted operation to the frame that owns its lane; it
    // never leaves this connection.
    struct operation_interruption final {};

    using cancellation_target_type = worker_cancellation_target<websocket_session_connection>;

    // One operation's deadline and stop token. Both deliver on this worker
    // and only flag the interruption; the operation unwinds at its next wait.
    class operation_interrupt_registration final {
    public:
        operation_interrupt_registration(
            websocket_session_connection& connection, operation_lane lane, const operation_options& options) {
            if (options.timeout_.has_value()) {
                (void)connection.worker_->schedule_timer(timer_, options.timeout_->deadline_from(std::chrono::steady_clock::now()),
                    [&connection, lane](worker_timer_outcome outcome) noexcept {
                        if (outcome == worker_timer_outcome::expired) {
                            connection.interrupt(lane, operation_interrupt::timed_out);
                        }
                    });
            }
            if (options.stop_token_.stoppable()) {
                cancellation_.emplace(connection.cancellation_target(),
                    lane == operation_lane::read ? connection.read_operation_id_ : connection.write_operation_id_);
                cancellation_->arm(options.stop_token_);
            }
        }

        operation_interrupt_registration(const operation_interrupt_registration&) = delete;
        operation_interrupt_registration& operator=(const operation_interrupt_registration&) = delete;

    private:
        worker_timer_registration timer_;
        // Declared last: its reset joins a pending cancellation first.
        std::optional<worker_cancellation_registration<cancellation_target_type>> cancellation_;
    };

    class write_guard_type final {
    public:
        explicit write_guard_type(websocket_session_connection& connection,
            write_claim_type claim = write_claim_type::acquire, bool restart_pong = true)
            : connection_(connection),
              restart_pong_(restart_pong) {
            if (claim == write_claim_type::acquire) {
                if (connection_.write_lane_.write_in_progress()) {
                    throw std::logic_error("concurrent websocket writes are not supported");
                }
                connection_.write_lane_.begin();
            } else if (!connection_.write_lane_.write_in_progress()) {
                std::terminate();
            }
        }

        void note_write() noexcept {
            restart_pong_ = true;
        }

        ~write_guard_type() {
            connection_.finish_write(restart_pong_);
        }

        write_guard_type(const write_guard_type&) = delete;
        write_guard_type& operator=(const write_guard_type&) = delete;

    private:
        websocket_session_connection& connection_;
        bool restart_pong_{true};
    };
    class write_operation_lease_type final {
    public:
        explicit write_operation_lease_type(websocket_session_connection& connection)
            : connection_(&connection) {
            if (connection_->write_active_) {
                connection_ = nullptr;
                throw std::logic_error("concurrent websocket writes are not supported");
            }
            connection_->write_active_ = true;
        }

        write_operation_lease_type(const write_operation_lease_type&) = delete;
        write_operation_lease_type& operator=(const write_operation_lease_type&) = delete;
        write_operation_lease_type(write_operation_lease_type&& other) noexcept
            : connection_(std::exchange(other.connection_, nullptr)) {}
        write_operation_lease_type& operator=(write_operation_lease_type&&) = delete;

        ~write_operation_lease_type() {
            if (connection_ != nullptr) {
                if (!connection_->worker_->is_current()) {
                    std::terminate();
                }
                connection_->write_active_ = false;
                connection_->notify_write_idle();
            }
        }

    private:
        websocket_session_connection* connection_;
    };

    // Submits an application payload by reference. Its owner is the caller of
    // write_owned(), which releases it once that write ends, so unless
    // release() records that the flush took the payload off the protocol, the
    // guard aborts protocol and transport: every exit after the submit,
    // exceptions and frame destruction included, leaves no view the protocol
    // could still copy or send.
    class borrowed_frame_guard_type final {
    public:
        borrowed_frame_guard_type(websocket_session_connection& connection, websocket_opcode opcode,
            std::string_view payload, bool compress, websocket_write_extent extent)
            : connection_(&connection) {
            const websocket_frame_options options{
                .final_ = extent != websocket_write_extent::fragment, .compress_ = compress};
            const auto submit = [&](bool borrow) {
                return borrow ? connection.protocol_.submit_frame(opcode, websocket_borrowed_payload(payload), options)
                              : connection.protocol_.submit_frame(opcode, payload, options);
            };
            const auto status = [&] {
                if constexpr (native_stream) {
                    if constexpr (requires(stream_type& value) { value.next_layer(); }) {
                        if (payload.size() + 14 <= websocket_retained_buffer_bytes) {
                            return submit(false);
                        }
                    }
                    return submit(true);
                } else {
                    return submit(false);
                }
            }();
            if (!connection.accept_submitted_frame(status)) {
                connection_ = nullptr;
            }
        }

        borrowed_frame_guard_type(const borrowed_frame_guard_type&) = delete;
        borrowed_frame_guard_type& operator=(const borrowed_frame_guard_type&) = delete;

        ~borrowed_frame_guard_type() {
            if (connection_ != nullptr) {
                connection_->abort_transport(websocket_termination::aborted);
            }
        }

        // False when the protocol was no longer open and queued nothing.
        [[nodiscard]] bool submitted() const noexcept {
            return connection_ != nullptr;
        }

        // A completed flush drained the payload, or ended the protocol so it
        // never reads the view again.
        void release() noexcept {
            connection_ = nullptr;
        }

    private:
        websocket_session_connection* connection_;
    };

    class read_guard_type final {
    public:
        explicit read_guard_type(websocket_session_connection& connection)
            : connection_(&connection) {
            if (connection_->read_phase_ != read_phase_type::idle) {
                connection_ = nullptr;
                throw std::logic_error("concurrent websocket reads are not supported");
            }
            connection_->read_phase_ = read_phase_type::reserved;
        }

        read_guard_type(const read_guard_type&) = delete;
        read_guard_type& operator=(const read_guard_type&) = delete;
        read_guard_type(read_guard_type&& other) noexcept
            : connection_(std::exchange(other.connection_, nullptr)) {}
        read_guard_type& operator=(read_guard_type&&) = delete;

        void start() {
            if (connection_ == nullptr || connection_->read_phase_ != read_phase_type::reserved) {
                std::terminate();
            }
            connection_->read_phase_ = read_phase_type::active;
            connection_->resume_pong_window_on_read_start();
        }

        ~read_guard_type() {
            if (connection_ != nullptr) {
                if (!connection_->worker_->is_current()) {
                    std::terminate();
                }
                connection_->read_phase_ = read_phase_type::idle;
                connection_->read_interrupt_.reset();
                connection_->reader_done_signal_.notify();
            }
        }

    private:
        websocket_session_connection* connection_;
    };

    void finish_write(bool restart_pong = true) noexcept;
    void resume_pong_window_on_read_start() noexcept;
    [[nodiscard]] bool pong_observable() const noexcept;
    void heartbeat_tick(std::int64_t now) noexcept;
    task<std::optional<websocket_message>> read_owned(read_guard_type read_guard, operation_options options);
    task<void> write_owned(websocket_opcode opcode, std::string_view payload, write_operation_lease_type write_lease,
        bool compress, websocket_write_extent extent, operation_options options);
    task<void> close_owned(::ruvia::websocket_close_options options, operation_options operation,
        write_operation_lease_type write_lease);
    task<void> write_heartbeat_ping();
    // Maps a rejected submit to its exception; false when the protocol is no
    // longer open and nothing was queued.
    bool accept_submitted_frame(websocket_frame_submit_status status);
    task<void> flush_protocol_output_now();
    // Reads more transport bytes into buffer_. Between frames the read is
    // sized by recent traffic, so an idle connection holds little read storage.
    task<bool> read_more();
    [[nodiscard]] const std::shared_ptr<cancellation_target_type>& cancellation_target();
    // Flags the operation that owns `lane`. A read wakes at its next wait and
    // keeps the session; an output operation aborts the session.
    void interrupt(operation_lane lane, operation_interrupt reason) noexcept;
    void throw_if_read_interrupted() const {
        if (read_interrupt_.has_value()) {
            throw operation_interruption{};
        }
    }
    void throw_if_write_interrupted() const {
        if (write_interrupt_.has_value()) {
            throw operation_interruption{};
        }
    }
    [[noreturn]] static void throw_interrupted(operation_interrupt reason, const char* operation_name);
    // A termination is recorded: this side ended the session (abort, a
    // heartbeat, close or operation timeout) or its close handshake finished.
    [[nodiscard]] bool session_ended() const noexcept {
        return protocol_.termination() != websocket_termination::none;
    }
    task<std::error_code> write_adapted(const websocket_output_plan& plan);
    auto write_bytes(const websocket_output_plan& plan);
    void close_stream() noexcept;
    // `cause` is recorded unless an earlier termination cause was.
    void abort_transport(websocket_termination cause, bool force_transport = false) noexcept;
    // Aborts the transport and retains the first background write failure.
    void fail_background(std::exception_ptr failure) noexcept;
    void record_peer_close(const websocket_close_event& close_event) noexcept {
        peer_close_.record(close_event);
    }
    void notify_write_idle() noexcept;
    [[nodiscard]] bool has_operations_to_drain() const noexcept {
        return read_phase_ != read_phase_type::idle || write_active_ || write_lane_.write_in_progress();
    }

    std::shared_ptr<websocket_state> facade_;
    stream_argument_type stream_;
    const worker_ref* worker_;
    connection_scanner::entry& scanner_entry_;
    websocket_lifecycle_options lifecycle_options_{};
    http_io_buffer buffer_;
    inbound_buffer_reservation buffer_reservation_{};
    websocket_connection protocol_;
    std::string subprotocol_;
    websocket_peer_close_info peer_close_{};
    websocket_write_lane write_lane_;
    worker_signal reader_done_signal_;
    bool write_active_{false};
    read_phase_type read_phase_{read_phase_type::idle};
    std::optional<operation_interrupt> read_interrupt_;
    std::optional<operation_interrupt> write_interrupt_;
    std::uint64_t read_operation_id_{0};
    std::uint64_t write_operation_id_{0};
    // Cancels only the transport read of an interrupted read().
    asio::cancellation_signal read_cancellation_;
    // Created by the first operation with a stop token; detached on destruction.
    std::shared_ptr<cancellation_target_type> cancellation_target_;
    websocket_liveness_state_type liveness_state_{websocket_liveness_idle{}};
    std::exception_ptr background_failure_;
    std::uint64_t heartbeat_sequence_{0};
    // Whether the last read between frames received less than
    // websocket_idle_read_bytes: the next such read then waits in a buffer of
    // that size.
    bool small_frames_{true};
    task_scope background_tasks_;
    // Declared last so destruction unregisters before any callback target state
    // starts to disappear.
    connection_scanner::periodic_check_registration periodic_check_;
};

template <typename stream_type>
bool websocket_session_connection<stream_type>::pong_observable() const noexcept {
    return !write_lane_.write_in_progress() && read_phase_ == read_phase_type::active;
}

template <typename stream_type>
void websocket_session_connection<stream_type>::resume_pong_window_on_read_start() noexcept {
    resume_websocket_pong_deadline(lifecycle_options_, liveness_state_, pong_observable());
}

template <typename stream_type>
void websocket_session_connection<stream_type>::finish_write(bool restart_pong) noexcept {
    write_lane_.release();
    if (restart_pong && std::holds_alternative<websocket_awaiting_pong>(liveness_state_)) {
        restart_websocket_pong_window(liveness_state_, steady_now_ms());
    }
    write_lane_.notify();
}

template <typename stream_type>
void websocket_session_connection<stream_type>::heartbeat_tick(std::int64_t now) noexcept {
    const bool observable = pong_observable();
    if (!observable) {
        restart_websocket_pong_window(liveness_state_, now);
    }
    switch (evaluate_websocket_liveness(lifecycle_options_, protocol_.liveness_mode(), liveness_state_,
        write_lane_.write_in_progress(), observable, scanner_entry_.last_active_ms(), now)) {
        case websocket_liveness_decision::idle:
            return;
        case websocket_liveness_decision::abort_transport:
            // A heartbeat or close timeout closes the connection.
            abort_transport(protocol_.liveness_mode() == websocket_liveness_mode::awaiting_peer_eof
                                ? protocol_.termination()
                                : websocket_termination::timeout,
                true);
            return;
        case websocket_liveness_decision::send_ping:
            break;
    }
    if (write_lane_.reader_waiting()) {
        // Let an already blocked reader poll its buffered control frames
        // before a scanner heartbeat claims the next message boundary.
        return;
    }

    liveness_state_ = websocket_sending_ping{++heartbeat_sequence_};
    write_lane_.begin();
    try {
        background_tasks_.spawn(write_heartbeat_ping());
    } catch (...) {
        finish_write();
        fail_background(std::current_exception());
        return;
    }
}

template <typename stream_type>
task<void> websocket_session_connection<stream_type>::write_heartbeat_ping() {
    write_guard_type write_guard(*this, write_claim_type::adopt);
    try {
        const auto* sending = std::get_if<websocket_sending_ping>(&liveness_state_);
        if (sending == nullptr) {
            co_return;
        }
        const auto challenge = sending->challenge_;
        const auto payload_value = websocket_heartbeat_payload(challenge);
        if (accept_submitted_frame(protocol_.submit_frame(
                websocket_opcode::ping, std::string_view(payload_value.data(), payload_value.size())))) {
            co_await flush_protocol_output_now();
        }
        // Ordinary I/O shares the scanner activity timestamp and may update it
        // while this coroutine is suspended. The Pong deadline belongs to this
        // completed heartbeat write, so use its own timestamp.
        const auto ping_sent_at_ms = steady_now_ms();
        if (protocol_.liveness_mode() == websocket_liveness_mode::open &&
            std::holds_alternative<websocket_sending_ping>(liveness_state_)) {
            liveness_state_ = websocket_awaiting_pong{ping_sent_at_ms, challenge};
        }
    } catch (...) {
        fail_background(std::current_exception());
    }
}

template <typename stream_type>
void websocket_session_connection<stream_type>::fail_background(std::exception_ptr failure) noexcept {
    abort_transport(websocket_termination::aborted);
    if (background_failure_ == nullptr) {
        background_failure_ = std::move(failure);
    }
}

template <typename stream_type>
task<std::optional<websocket_message>> websocket_session_connection<stream_type>::read(operation_options options) {
    require_current_worker();
    return read_owned(read_guard_type(*this), std::move(options));
}

template <typename stream_type>
const std::shared_ptr<typename websocket_session_connection<stream_type>::cancellation_target_type>&
websocket_session_connection<stream_type>::cancellation_target() {
    if (!cancellation_target_) {
        cancellation_target_ = make_worker_cancellation_target(*this, *worker_);
    }
    return cancellation_target_;
}

template <typename stream_type>
void websocket_session_connection<stream_type>::interrupt(operation_lane lane, operation_interrupt reason) noexcept {
    if (lane == operation_lane::read) {
        if (read_phase_ != read_phase_type::active || read_interrupt_.has_value()) {
            return;
        }
        read_interrupt_ = reason;
        read_cancellation_.emit(asio::cancellation_type::terminal);
        // Wakes a reader waiting for its turn at the next message boundary.
        write_lane_.notify();
        return;
    }
    if (!write_active_ || write_interrupt_.has_value()) {
        return;
    }
    write_interrupt_ = reason;
    abort_transport(reason == operation_interrupt::timed_out ? websocket_termination::timeout
                                                             : websocket_termination::aborted,
        true);
}

template <typename stream_type>
void websocket_session_connection<stream_type>::throw_interrupted(operation_interrupt reason, const char* operation_name) {
    if (reason == operation_interrupt::timed_out) {
        throw std::system_error(std::make_error_code(std::errc::timed_out), operation_name);
    }
    throw std::system_error(std::make_error_code(std::errc::operation_canceled), operation_name);
}

template <typename stream_type>
task<std::optional<websocket_message>> websocket_session_connection<stream_type>::read_owned(
    read_guard_type read_guard_value, operation_options options) {
    require_current_worker();
    std::optional<operation_interrupt> interrupted;
    {
        read_guard_type active_read(std::move(read_guard_value));
        active_read.start();
        if (options.stop_token_.stop_requested()) {
            throw_interrupted(operation_interrupt::cancelled, "websocket read cancelled");
        }
        const operation_interrupt_registration interrupt_registration(*this, operation_lane::read, options);
        const auto check_interrupt = [this] { throw_if_read_interrupted(); };
        try {
            for (;;) {
                std::optional<websocket_event> event;
                {
                    // RFC 6455 sections 5.5.1/5.5.2: poll() may queue an
                    // automatic Close/Pong. Reserve the next message boundary
                    // and retain the lane through that control output flush.
                    if (write_lane_.write_in_progress()) {
                        co_await write_lane_.wait_for_idle(true, check_interrupt);
                    }
                    write_guard_type write_guard(*this, write_claim_type::acquire, false);
                    event = protocol_.next_event();
                    if (event.has_value()) {
                        if (event->ping() != nullptr) {
                            write_guard.note_write();
                            co_await flush_protocol_output_now();
                        } else if (event->close() != nullptr || event->protocol_error() != nullptr || event->transport_end() != nullptr) {
                            if (const auto* close_event = event->close()) {
                                record_peer_close(*close_event);
                            }
                            liveness_state_ = event->close() != nullptr
                                                  ? websocket_liveness_state_type(websocket_awaiting_peer_close{steady_now_ms()})
                                                  : websocket_liveness_state_type(websocket_liveness_idle{});
                            write_guard.note_write();
                            co_await flush_protocol_output_now();
                        }
                    }
                }
                if (!event.has_value()) {
                    if (!(co_await read_more())) {
                        // EOF is an abnormal websocket close when no peer Close was
                        // received. The core discards unsent WS output and plans the
                        // transport end.
                        if (write_lane_.write_in_progress()) {
                            co_await write_lane_.wait_for_idle(true, check_interrupt);
                        }
                        write_guard_type write_guard(*this);
                        protocol_.notify_transport_eof();
                        co_await flush_protocol_output_now();
                        close_stream();
                        co_return std::nullopt;
                    }
                    scanner_entry_.touch();
                    continue;
                }

                if (const auto* message = event->message()) {
                    co_return *message;
                }
                if (event->ping() != nullptr) {
                    continue;
                }
                if (event->pong() != nullptr) {
                    if (websocket_heartbeat_pong_matches(liveness_state_, event->pong()->payload())) {
                        liveness_state_ = websocket_liveness_idle{};
                        scanner_entry_.touch();
                    }
                    continue;
                }
                if (event->close() != nullptr || event->protocol_error() != nullptr || event->transport_end() != nullptr) {
                    // The poll's exclusive lane already flushed the protocol's
                    // Close/end-transport plan before this read completes.
                    if (protocol_.liveness_mode() == websocket_liveness_mode::awaiting_peer_eof) {
                        continue;
                    }
                    co_return std::nullopt;
                }
                throw std::logic_error("unexpected WebSocket protocol event");
            }
        } catch (const operation_interruption&) {
            interrupted = read_interrupt_;
        } catch (...) {
            abort_transport(websocket_termination::aborted, true);
            throw;
        }
    }
    throw_interrupted(*interrupted, "websocket read");
}

template <typename stream_type>
task<bool> websocket_session_connection<stream_type>::read_more() {
    throw_if_read_interrupted();
    if constexpr (!native_stream) {
        try {
            const bool more = co_await stream_.read_more(
                buffer_, [this] { throw_if_read_interrupted(); }, read_cancellation_.slot());
            buffer_reservation_.resize_to(buffer_.capacity());
            co_return more;
        } catch (const inbound_transport_error&) {
            if (protocol_.liveness_mode() != websocket_liveness_mode::inactive) {
                abort_transport(websocket_termination::io_error, true);
                throw;
            }
        }
        // This side already ended the session and aborted the stream.
        co_return false;
    } else {
        constexpr std::size_t min_read_bytes = 4096;
        constexpr std::size_t max_read_bytes = 64 * 1024;
        const bool between_frames = buffer_.empty();
        std::span<char> read_space;
        if (between_frames) {
            // Preserve the native socket's small idle read allocation and
            // retained storage for steady mid-size messages.
            if (buffer_.capacity() > websocket_retained_buffer_bytes) {
                inbound_shrink_to(buffer_, buffer_reservation_, websocket_retained_buffer_bytes);
            }
            const auto min_idle_bytes = small_frames_ ? websocket_idle_read_bytes : min_read_bytes;
            if (buffer_.capacity() < min_idle_bytes) {
                read_space = inbound_prepare(buffer_, buffer_reservation_, min_idle_bytes);
            } else {
                read_space = buffer_.prepare_spare();
                if (small_frames_ && read_space.size() > websocket_idle_read_bytes) {
                    read_space = read_space.first(websocket_idle_read_bytes);
                } else if (read_space.size() > max_read_bytes) {
                    read_space = read_space.first(max_read_bytes);
                }
            }
        } else {
            // Use the writable tail before growing an incomplete frame.
            read_space = buffer_.prepare_spare();
            if (read_space.empty()) {
                const auto needed = std::clamp(protocol_.needed_input_bytes(), min_read_bytes, max_read_bytes);
                read_space = inbound_prepare(buffer_, buffer_reservation_, needed);
            } else if (read_space.size() > max_read_bytes) {
                read_space = read_space.first(max_read_bytes);
            }
        }
        // Only this read is cancelled: a TLS stream shields its own record
        // writes from the slot, so the engine resumes cleanly on the next read.
        auto read_completion = co_await ruvia::async_asio<std::size_t>([this, read_space](auto handler) mutable {
            stream_.async_read_some(asio::buffer(read_space),
                asio::bind_cancellation_slot(read_cancellation_.slot(), std::move(handler)));
        });
        const auto ec = read_completion.error_code();
        const auto bytes_read = read_completion.result();
        // The protocol maps EOF without a Close frame to abnormal closure.
        if (is_stream_read_end(ec) || (!ec && bytes_read == 0)) {
            co_return false;
        }
        if (ec) {
            // This side already ended the session and closed the stream.
            if (protocol_.liveness_mode() == websocket_liveness_mode::inactive) {
                co_return false;
            }
            if (read_interrupt_.has_value() && ec == asio::error::operation_aborted) {
                throw operation_interruption{};
            }
            abort_transport(websocket_termination::io_error, true);
            throw inbound_transport_error(ec, "failed to read websocket bytes");
        }
        buffer_.commit(bytes_read);
        if (between_frames) {
            small_frames_ = bytes_read < websocket_idle_read_bytes;
        }
        co_return true;
    }
}

template <typename stream_type>
task<void> websocket_session_connection<stream_type>::write(websocket_opcode opcode, std::string_view payload_value,
    bool compress, websocket_write_extent extent, operation_options options) {
    require_current_worker();
    return write_owned(opcode, payload_value, write_operation_lease_type(*this), compress, extent, std::move(options));
}

template <typename stream_type>
task<void> websocket_session_connection<stream_type>::write_owned(websocket_opcode opcode,
    std::string_view payload_value, write_operation_lease_type write_lease, bool compress, websocket_write_extent extent,
    operation_options options) {
    require_current_worker();
    std::optional<operation_interrupt> interrupted;
    {
        write_operation_lease_type active_write(std::move(write_lease));
        static_cast<void>(active_write);
        if (options.stop_token_.stop_requested()) {
            throw_interrupted(operation_interrupt::cancelled, "websocket write");
        }
        write_interrupt_.reset();
        const operation_interrupt_registration interrupt_registration(*this, operation_lane::write, options);
        try {
            if (write_lane_.blocked_for_writer()) {
                co_await write_lane_.wait_for_idle(false, [this] { throw_if_write_interrupted(); });
            }
            write_guard_type write_guard(*this);
            // The caller keeps the payload alive until this write completes, so an
            // uncompressed frame is gathered from it instead of being copied.
            borrowed_frame_guard_type borrowed_frame(*this, opcode, payload_value, compress, extent);
            if (borrowed_frame.submitted()) {
                co_await flush_protocol_output_now();
                borrowed_frame.release();
            }
        } catch (...) {
            if (!write_interrupt_.has_value()) {
                throw;
            }
            interrupted = write_interrupt_;
        }
    }
    if (interrupted.has_value()) {
        throw_interrupted(*interrupted, "websocket write");
    }
}

template <typename stream_type>
task<void> websocket_session_connection<stream_type>::close(
    ::ruvia::websocket_close_options options, operation_options operation) {
    require_current_worker();
    return close_owned(options, std::move(operation), write_operation_lease_type(*this));
}

template <typename stream_type>
task<void> websocket_session_connection<stream_type>::close_owned(
    ::ruvia::websocket_close_options options, operation_options operation, write_operation_lease_type write_lease) {
    require_current_worker();
    std::optional<operation_interrupt> interrupted;
    bool handshake_timed_out = false;
    {
        write_operation_lease_type active_write(std::move(write_lease));
        static_cast<void>(active_write);
        if (operation.stop_token_.stop_requested()) {
            throw_interrupted(operation_interrupt::cancelled, "websocket close");
        }
        write_interrupt_.reset();
        const operation_interrupt_registration interrupt_registration(*this, operation_lane::write, operation);
        try {
            if (write_lane_.blocked_for_writer()) {
                co_await write_lane_.wait_for_idle(false, [this] { throw_if_write_interrupted(); });
            }
            const auto reason = options.reason_.view();
            bool flush_output = false;
            {
                write_guard_type write_guard(*this);
                switch (protocol_.submit_close(options.code_, reason)) {
                    case websocket_close_submit_status::accepted:
                        // The deadline covers the Close write too: a peer that stops
                        // reading must not hold the flush below open past the timeout.
                        liveness_state_ = websocket_awaiting_peer_close{steady_now_ms()};
                        flush_output = true;
                        break;
                    case websocket_close_submit_status::already_closing:
                        flush_output = true;
                        break;
                    case websocket_close_submit_status::closed:
                        break;
                    case websocket_close_submit_status::invalid_code:
                        throw std::invalid_argument("invalid websocket close code");
                    case websocket_close_submit_status::invalid_reason:
                        throw std::invalid_argument("invalid websocket close reason");
                    case websocket_close_submit_status::reason_too_large:
                        throw std::invalid_argument("websocket close reason is too large");
                }
                if (flush_output) {
                    co_await flush_protocol_output_now();
                }
            }

            // RFC 6455 section 7.1.1: sending Close starts, but does not complete,
            // the handshake. A pending read keeps parsing input until the peer
            // Close arrives (or EOF/timeout aborts this transport) and then
            // returns nullopt; without one, close() reads itself. The core
            // suppresses application messages in this phase while still
            // validating frames and handling control traffic.
            const bool awaiting_peer_close = protocol_.liveness_mode() == websocket_liveness_mode::awaiting_peer_close;
            while (protocol_.liveness_mode() == websocket_liveness_mode::awaiting_peer_close ||
                   protocol_.liveness_mode() == websocket_liveness_mode::awaiting_peer_eof) {
                if (read_phase_ != read_phase_type::idle) {
                    co_await reader_done_signal_.wait();
                } else {
                    (void)co_await read();
                }
            }
            handshake_timed_out = awaiting_peer_close && protocol_.termination() == websocket_termination::timeout;
        } catch (...) {
            if (!write_interrupt_.has_value()) {
                throw;
            }
        }
        interrupted = write_interrupt_;
    }
    if (interrupted.has_value()) {
        throw_interrupted(*interrupted, "websocket close");
    }
    if (handshake_timed_out) {
        // The peer never answered: an inbound connection end, not a handler fault.
        throw std::system_error(
            std::make_error_code(std::errc::timed_out), "websocket close handshake timed out");
    }
}

template <typename stream_type>
task<void> websocket_session_connection<stream_type>::detach_and_drain_writes() {
    require_current_worker();
    if (facade_ && !facade_->owner_) {
        facade_->operation_scope_.expire();
    }
    periodic_check_.reset();
    // Teardown first cancels the transport operation that owns a suspended
    // write, then joins that write before the connection storage disappears.
    // Application writes can outlive the handler through the public facade just
    // as heartbeat writes can outlive the scanner callback, so both phases are
    // part of the same structured drain. Force a transport abort when active
    // I/O remains even if the protocol already committed its normal close.
    const bool has_active_io = read_phase_ == read_phase_type::active || write_lane_.write_in_progress();
    abort_transport(websocket_termination::aborted, has_active_io);
    while (has_operations_to_drain()) {
        if (read_phase_ != read_phase_type::idle) {
            co_await reader_done_signal_.wait();
        } else {
            co_await write_lane_.wait();
        }
    }
    co_await background_tasks_.join();
    if (facade_ && !facade_->owner_) {
        auto facade = std::exchange(facade_, nullptr);
        co_await facade->operation_scope_.close_and_join();
    } else {
        facade_.reset();
    }
}

template <typename stream_type>
void websocket_session_connection<stream_type>::notify_write_idle() noexcept {
    write_lane_.notify();
}

template <typename stream_type>
bool websocket_session_connection<stream_type>::accept_submitted_frame(websocket_frame_submit_status status) {
    switch (status) {
        case websocket_frame_submit_status::accepted:
            return true;
        case websocket_frame_submit_status::not_open:
            return false;
        case websocket_frame_submit_status::invalid_opcode:
            throw std::logic_error("invalid outbound websocket opcode");
        case websocket_frame_submit_status::message_too_large:
            throw std::invalid_argument("websocket message is too large");
        case websocket_frame_submit_status::invalid_text_payload:
            throw std::invalid_argument("websocket text payload is not valid UTF-8");
        case websocket_frame_submit_status::control_frame_too_large:
            throw std::invalid_argument("websocket control frame is too large");
        case websocket_frame_submit_status::message_in_progress:
            throw std::logic_error("a fragmented websocket message is in progress");
    }
    throw std::logic_error("unexpected websocket frame submit status");
}

template <typename stream_type>
auto websocket_session_connection<stream_type>::write_bytes(const websocket_output_plan& plan) {
    if constexpr (!native_stream) {
        return write_adapted(plan);
    } else {
        // One gathered write: pending protocol output, then a borrowed payload.
        const std::array<asio::const_buffer, 2> buffers{
            asio::buffer(plan.bytes().data(), plan.bytes().size()),
            asio::buffer(plan.borrowed_bytes().data(), plan.borrowed_bytes().size())};
        return ruvia::async_asio([this, buffers](auto handler) mutable {
            // Partial progress refreshes heartbeat transport activity and the
            // write_timeout inactivity clock.
            asio::async_write(stream_, buffers, connection_write_progress(scanner_entry_), std::move(handler));
        });
    }
}

template <typename stream_type>
task<std::error_code> websocket_session_connection<stream_type>::write_adapted(const websocket_output_plan& plan) {
    // The websocket protocol's transport disposition maps onto the stream end
    // at this one boundary; the multiplexed transports only speak http_stream_end.
    const auto end = plan.disposition() == websocket_transport_disposition::end_transport ? http_stream_end::end
                                                                                          : http_stream_end::keep_open;
    if (plan.borrowed_bytes().empty()) {
        co_return co_await stream_.write_bytes(plan.bytes(), end);
    }
    if (const auto error = co_await stream_.write_bytes(plan.bytes(), http_stream_end::keep_open)) {
        co_return error;
    }
    co_return co_await stream_.write_bytes(plan.borrowed_bytes(), end);
}

template <typename stream_type>
task<void> websocket_session_connection<stream_type>::flush_protocol_output_now() {
    struct flush_abort_guard final {
        websocket_session_connection* connection_;
        websocket_termination cause_{websocket_termination::aborted};
        ~flush_abort_guard() noexcept {
            if (connection_ != nullptr) {
                connection_->abort_transport(cause_, true);
            }
        }
    } guard{this};
    for (;;) {
        const auto plan = protocol_.output_plan();
        const auto disposition = plan.disposition();
        const auto size = plan.size();
        if (size == 0 && disposition == websocket_transport_disposition::keep_open) {
            guard.connection_ = nullptr;
            co_return;
        }
        if (size != 0 || (!native_stream && disposition == websocket_transport_disposition::end_transport)) {
            std::error_code ec;
            if constexpr (native_stream) {
                // The server write_timeout_ bounds a native write stalled by a
                // peer that stopped reading, whatever the reader's phase. An
                // HTTP/2-adapted stream is bounded by its session's writer.
                const connection_scanner::entry::write_scope write_stall_clock(scanner_entry_);
                const auto write_result = co_await write_bytes(plan);
                ec = write_result.error_code();
            } else {
                ec = co_await write_bytes(plan);
            }
            if (ec) {
                guard.cause_ = websocket_termination::io_error;
                if (protocol_.termination() == websocket_termination::timeout) {
                    throw std::system_error(std::make_error_code(std::errc::timed_out), "websocket write timed out");
                }
                throw inbound_transport_error(ec, "failed to write websocket bytes");
            }
        }
        if (protocol_.consume_output(size) != websocket_output_consume_status::drained) {
            std::terminate();
        }
        scanner_entry_.touch();
        if (disposition == websocket_transport_disposition::end_transport) {
            protocol_.commit_transport_end();
            guard.connection_ = nullptr;
            co_return;
        }
    }
}

template <typename stream_type>
void websocket_session_connection<stream_type>::abort_transport(websocket_termination cause, bool force_transport) noexcept {
    liveness_state_ = websocket_liveness_idle{};
    const auto disposition = protocol_.abort(cause);
    if (force_transport || disposition == websocket_abort_disposition::abort_transport) {
        close_stream();
        notify_write_idle();
    }
}

template <typename stream_type>
void websocket_session_connection<stream_type>::close_stream() noexcept {
    close_stream(stream_);
}

}  // namespace ruvia::detail

namespace ruvia::detail {

template <typename connection_type>
[[nodiscard]] task<std::optional<websocket_message>> websocket_read_thunk(void* target, operation_options options) {
    return static_cast<connection_type*>(target)->read(std::move(options));
}

template <typename connection_type>
task<void> websocket_write_thunk(void* target, websocket_opcode opcode, std::string_view payload_value, bool compress,
    websocket_write_extent extent, operation_options options) {
    return static_cast<connection_type*>(target)->write(opcode, payload_value, compress, extent, std::move(options));
}

template <typename connection_type>
task<void> websocket_close_thunk(void* target, ::ruvia::websocket_close_options options, operation_options operation) {
    return static_cast<connection_type*>(target)->close(options, std::move(operation));
}

template <typename connection_type>
void websocket_abort_thunk(void* target) noexcept {
    static_cast<connection_type*>(target)->abort();
}

template <typename connection_type>
[[nodiscard]] std::optional<std::uint16_t> websocket_peer_close_code_thunk(void* target) noexcept {
    return static_cast<connection_type*>(target)->peer_close_code();
}

template <typename connection_type>
[[nodiscard]] std::string_view websocket_peer_close_reason_thunk(void* target) noexcept {
    return static_cast<connection_type*>(target)->peer_close_reason();
}

template <typename connection_type>
[[nodiscard]] const websocket_connection& websocket_protocol_thunk(void* target) noexcept {
    return static_cast<connection_type*>(target)->protocol();
}

template <typename connection_type>
[[nodiscard]] websocket make_websocket_facade(connection_type& connection) {
    auto state = std::make_shared<websocket_state>(connection.worker(), &connection, connection.subprotocol(),
        &websocket_read_thunk<connection_type>, &websocket_write_thunk<connection_type>,
        &websocket_close_thunk<connection_type>, &websocket_abort_thunk<connection_type>,
        &websocket_peer_close_code_thunk<connection_type>,
        &websocket_peer_close_reason_thunk<connection_type>,
        &websocket_protocol_thunk<connection_type>);
    connection.bind_facade(state);
    return websocket(std::move(state));
}

// The terminal handler borrows an established connection, but does not close it:
// onion middleware post-processing still belongs to the same websocket request
// and must be able to turn its own failure into the session's 1011 outcome.
template <typename stream_type>
task<void> invoke_websocket_handler(websocket_session_connection<stream_type>& connection,
    connection_scanner::entry& scanner_entry, const route_stream_handler& handler,
    context& context_value) {
    auto websocket_value = make_websocket_facade(connection);
    context_websocket_binding websocket_binding(context_value, websocket_value);

    scanner_entry.enter_long_lived();
    co_await handler(context_value);
}
// The session retains the connection until the complete route middleware
// chain finishes, then converge here. close() is idempotent through the protocol
// core's typed close phase, so a handler that already closed itself is safe.
//
// A handler that failed is already past the upgrade, so its exception can only
// become a 1011 close code -- which tells the peer that something went wrong
// but not what, and is the last thing that references the failure. It, a
// failure to close, and a failed heartbeat write are reported
// here, since nothing after this frame holds any of them. The sink classifies
// each: a peer that resets instead of answering the Close, or a close-handshake
// timeout that aborted the transport, ends the connection normally.
template <typename stream_type>
task<void> finish_websocket_session(websocket_session_connection<stream_type>& connection,
    std::exception_ptr exception, const connection_failure_sink& connection_failure,
    std::string_view remote_address) {
    connection_failure.invoke(remote_address, exception);
    try {
        if (exception != nullptr) {
            co_await connection.close({.code_ = websocket_close_status::internal_error, .reason_ = "internal server error"});
        } else {
            co_await connection.close();
        }
    } catch (...) {
        connection_failure.invoke(remote_address, std::current_exception());
    }
    co_await connection.detach_and_drain_writes();
    connection_failure.invoke(remote_address, connection.take_background_failure());
}

}  // namespace ruvia::detail
