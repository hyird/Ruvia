#pragma once

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <span>

#include <asio/ip/udp.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http3_quic_connection.h"

#include "tls/tls_random.h"

namespace ruvia::detail {

// Resumes the coroutine on a later turn of its worker, letting other
// connections run between bursts of one connection's output.
struct worker_turn final {
    explicit worker_turn(const worker_ref& worker_value) noexcept
        : worker_(worker_value) {}
    worker_turn(worker_ref&&) = delete;
    const worker_turn* operator&() const = delete;
    worker_turn* operator&() = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> continuation) const noexcept {
        worker_.defer_resume(continuation);
    }
    void await_resume() const noexcept {}

    const worker_ref& worker_;
};

struct free_tls final {
    void operator()(SSL* tls) const noexcept {
        SSL_free(tls);
    }
};

struct free_crypto final {
    void operator()(ngtcp2_crypto_ossl_ctx* crypto) const noexcept {
        ngtcp2_crypto_ossl_ctx_del(crypto);
    }
};

inline void fill_random(std::uint8_t* destination, std::size_t size, const ngtcp2_rand_ctx*) noexcept {
    // ngtcp2 cannot fail this callback; packet protection without randomness
    // must not continue.
    if (!fill_random_bytes(std::span<unsigned char>(destination, size))) {
        std::terminate();
    }
}

// The ngtcp2_crypto_ossl adapter callbacks both roles share; the client adds
// client_initial_ and recv_retry_, the server recv_client_initial_.
[[nodiscard]] inline http3_quic_crypto_callbacks make_http3_quic_crypto_callbacks() noexcept {
    return {
        .recv_crypto_data_ = ngtcp2_crypto_recv_crypto_data_cb,
        .encrypt_ = ngtcp2_crypto_encrypt_cb,
        .decrypt_ = ngtcp2_crypto_decrypt_cb,
        .hp_mask_ = ngtcp2_crypto_hp_mask_cb,
        .update_key_ = ngtcp2_crypto_update_key_cb,
        .delete_crypto_aead_ctx_ = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
        .delete_crypto_cipher_ctx_ = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
        .get_path_challenge_data_ = ngtcp2_crypto_get_path_challenge_data2_cb,
        .version_negotiation_ = ngtcp2_crypto_version_negotiation_cb,
        .rand_ = fill_random,
    };
}

[[nodiscard]] inline ngtcp2_tstamp http3_quic_now() noexcept {
    return static_cast<ngtcp2_tstamp>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

[[nodiscard]] inline ngtcp2_duration http3_quic_duration(timeout_duration value) noexcept {
    const auto milliseconds = static_cast<std::uint64_t>((std::max)(value.value().count(), std::int64_t{0}));
    return milliseconds > UINT64_MAX / NGTCP2_MILLISECONDS ? UINT64_MAX : milliseconds * NGTCP2_MILLISECONDS;
}

[[nodiscard]] inline ngtcp2_duration http3_quic_duration(const std::optional<timeout_duration>& value) noexcept {
    return value.has_value() ? http3_quic_duration(*value) : 0;
}

[[nodiscard]] inline std::chrono::steady_clock::time_point http3_quic_steady_time(ngtcp2_tstamp timestamp) noexcept {
    using nanoseconds = std::chrono::nanoseconds;
    if (timestamp >= static_cast<ngtcp2_tstamp>((std::numeric_limits<nanoseconds::rep>::max)())) {
        return (std::chrono::steady_clock::time_point::max)();
    }
    return std::chrono::steady_clock::time_point(
        std::chrono::ceil<std::chrono::steady_clock::duration>(nanoseconds(static_cast<nanoseconds::rep>(timestamp))));
}

[[nodiscard]] inline std::chrono::nanoseconds http3_quic_nanoseconds(ngtcp2_duration duration) noexcept {
    return std::chrono::nanoseconds(
        static_cast<std::int64_t>((std::min)(duration, static_cast<ngtcp2_duration>(INT64_MAX))));
}

[[nodiscard]] inline ngtcp2_path http3_quic_path(
    asio::ip::udp::endpoint& local, asio::ip::udp::endpoint& remote) noexcept {
    return {
        .local = {local.data(), static_cast<ngtcp2_socklen>(local.size())},
        .remote = {remote.data(), static_cast<ngtcp2_socklen>(remote.size())},
        .user_data = nullptr,
    };
}

[[nodiscard]] inline std::optional<asio::ip::udp::endpoint> http3_udp_endpoint(
    const ngtcp2_addr& address) noexcept {
    asio::ip::udp::endpoint endpoint;
    if (address.addr == nullptr || address.addrlen == 0 || address.addrlen > endpoint.capacity()) {
        return std::nullopt;
    }
    std::memcpy(endpoint.data(), address.addr, address.addrlen);
    endpoint.resize(address.addrlen);
    return endpoint;
}
}  // namespace ruvia::detail
