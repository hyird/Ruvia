#pragma once

#if defined(RUVIA_ENABLE_HTTP3)

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
#include <system_error>

#include <asio/error.hpp>
#include <asio/ip/udp.hpp>
#include <openssl/ssl.h>

#include "asio_ip_endpoint.h"
#include "crypto.h"
#include "event_loop.h"
#include "http3_quic_connection.h"
#include "worker_ref.h"

namespace ruvia::detail {

inline constexpr std::size_t http3_max_udp_payload_bytes = 65527;

[[nodiscard]] inline bool http3_ignorable_receive_error(const std::error_code& error) noexcept {
#if defined(_WIN32)
    if (error.category() == asio::error::get_system_category()) {
        switch (error.value()) {
            case ERROR_PORT_UNREACHABLE:
            case ERROR_HOST_UNREACHABLE:
            case ERROR_NETWORK_UNREACHABLE:
            case ERROR_CONNECTION_REFUSED:
            case ERROR_NETNAME_DELETED:
            case ERROR_MORE_DATA:
            case WSAENETRESET:
                return true;
            default:
                break;
        }
    }
#endif
    return error == asio::error::connection_refused || error == asio::error::connection_reset || error == asio::error::message_size || error == asio::error::network_unreachable || error == asio::error::host_unreachable;
}

struct worker_turn final {
    explicit worker_turn(const worker_ref& worker_value) noexcept :
        worker_(worker_value) {}
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
    if (!fill_random_bytes(std::as_writable_bytes(std::span<std::uint8_t>(destination, size)))) {
        std::terminate();
    }
}

[[nodiscard]] inline bool try_initialize_quic_crypto() noexcept {
    static const bool initialized = ngtcp2_crypto_ossl_init() == 0;
    return initialized;
}

inline void install_http3_quic_common_crypto_callbacks(ngtcp2_callbacks& callbacks) noexcept {
    callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
    callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
    callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
    callbacks.update_key = ngtcp2_crypto_update_key_cb;
    callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    callbacks.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
    callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    callbacks.rand = fill_random;
}

inline void install_http3_quic_client_crypto_callbacks(ngtcp2_callbacks& callbacks) noexcept {
    install_http3_quic_common_crypto_callbacks(callbacks);
    callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
    callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;
}

inline void install_http3_quic_server_crypto_callbacks(ngtcp2_callbacks& callbacks) noexcept {
    install_http3_quic_common_crypto_callbacks(callbacks);
    callbacks.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
}

[[nodiscard]] inline quic_timestamp http3_quic_now() noexcept {
    return static_cast<quic_timestamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

[[nodiscard]] inline ngtcp2_duration http3_ngtcp2_duration(timeout_duration value) noexcept {
    const auto milliseconds = static_cast<std::uint64_t>((std::max)(value.value().count(), std::int64_t{0}));
    return milliseconds > UINT64_MAX / NGTCP2_MILLISECONDS ? UINT64_MAX : milliseconds * NGTCP2_MILLISECONDS;
}

[[nodiscard]] inline std::chrono::nanoseconds http3_quic_duration(timeout_duration value) noexcept {
    const auto milliseconds = (std::max)(value.value().count(), std::int64_t{0});
    constexpr auto max_ms = (std::chrono::nanoseconds::max)().count() / 1'000'000;
    return milliseconds >= max_ms ? (std::chrono::nanoseconds::max)() : std::chrono::nanoseconds(milliseconds * 1'000'000);
}

[[nodiscard]] inline std::chrono::nanoseconds http3_quic_duration(const std::optional<timeout_duration>& value) noexcept {
    return value.has_value() ? http3_quic_duration(*value) : std::chrono::nanoseconds{0};
}

[[nodiscard]] inline std::chrono::steady_clock::time_point http3_quic_steady_time(quic_timestamp timestamp) noexcept {
    using nanoseconds = std::chrono::nanoseconds;
    if (timestamp >= static_cast<quic_timestamp>((std::numeric_limits<nanoseconds::rep>::max)())) {
        return (std::chrono::steady_clock::time_point::max)();
    }
    return std::chrono::steady_clock::time_point(std::chrono::ceil<std::chrono::steady_clock::duration>(nanoseconds(static_cast<nanoseconds::rep>(timestamp))));
}

[[nodiscard]] inline quic_cid to_quic_cid(const ngtcp2_cid& cid) noexcept {
    return quic_cid::try_from_bytes(byte_view(reinterpret_cast<const std::byte*>(cid.data), cid.datalen)).value_or(quic_cid{});
}

[[nodiscard]] inline http3_quic_path make_quic_path(const asio::ip::udp::endpoint& local, const asio::ip::udp::endpoint& remote, std::uintptr_t user_data = 0) noexcept {
    return {
        .local_ = to_ip_endpoint(local),
        .remote_ = to_ip_endpoint(remote),
        .user_data_ = user_data,
    };
}

}  // namespace ruvia::detail

#endif
