#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/buffer_pool.h"
#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_notification.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/core/worker_runtime_context.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/quic_server.h"

#include "client/client_transport.h"
#include "http3/http3_datagram_channel.h"
#include "http3/http3_quic_client_tls_context.h"
#include "http3/http3_quic_client_transport.h"
#include "http3/http3_quic_socket_address.h"
#include "http3/http3_quic_wire_owner.h"
#include "http3/http3_worker_runtime.h"
#include "http3/http3_worker_server.h"
#include "integration/worker_capabilities.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "server/http_server_listener.h"
#include "server/http_server_options.h"
#include "test_harness.h"
#include "test_tls_crypto.h"

namespace {
using namespace std::chrono_literals;
using udp_type = asio::ip::udp;
using ruvia::detail::http3_quic_client_tls_context;
using ruvia::detail::http3_quic_client_transport;
using ruvia::detail::http3_quic_datagram_address;
using ruvia::detail::http3_quic_server_transport;
using ruvia::detail::http3_quic_tls_context;
using ruvia::detail::http3_quic_wire_owner;
using ruvia::detail::http3_worker_datagram_endpoint;

http3_quic_wire_owner::protocol_pump_result_type fail_protocol_pump(
    void*, http3_quic_server_transport&, http3_worker_datagram_endpoint&) noexcept {
    return http3_quic_wire_owner::protocol_pump_result_type::fatal;
}

class fail_on_allocation_resource final : public std::pmr::memory_resource {
public:
    explicit fail_on_allocation_resource(std::size_t fail_at) noexcept
        : fail_at_(fail_at) {}
    [[nodiscard]] std::size_t allocation_attempts() const noexcept {
        return attempts_;
    }
    [[nodiscard]] std::size_t outstanding_allocations() const noexcept {
        return outstanding_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        // MSVC debug containers allocate small iterator proxies in noexcept
        // constructors. Inject failure into payload storage, not those proxies.
        if (bytes_value >= 32 && ++attempts_ == fail_at_) {
            throw std::bad_alloc();
        }
        ++outstanding_;
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }
    void do_deallocate(void* allocation, std::size_t bytes_value, std::size_t alignment) override {
        --outstanding_;
        std::pmr::new_delete_resource()->deallocate(allocation, bytes_value, alignment);
    }
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::size_t fail_at_{};
    std::size_t attempts_{};
    std::size_t outstanding_{};
};

struct version_negotiation_pump_state final {
    bool routed_{};
    bool sent_{};
    bool plan_consumed_{};
    std::exception_ptr failure_;
};

http3_quic_wire_owner::protocol_pump_result_type pump_version_negotiation(void* context_value,
    http3_quic_server_transport& transport, http3_worker_datagram_endpoint& endpoint) noexcept {
    auto& state_value = *static_cast<version_negotiation_pump_state*>(context_value);
    try {
        const auto received_value = endpoint.receive_slot();
        if (!received_value) {
            return http3_quic_wire_owner::protocol_pump_result_type::idle;
        }
        const auto local = ruvia::detail::to_http3_quic_datagram_address(
            received_value->local_destination_);
        const auto peer = ruvia::detail::to_http3_quic_datagram_address(received_value->peer_);
        if ((local.index() != 0) || (peer.index() != 0)) {
            throw std::runtime_error("test QUIC datagram has invalid endpoint addresses");
        }
        auto route = transport.route_datagram(received_value->bytes_, std::get<0>(local), std::get<0>(peer));
        state_value.routed_ = route.kind_ == ruvia::quic_server_route_kind::version_negotiation;
        if (state_value.routed_) {
            const auto result_value = ruvia::detail::send_http3_version_negotiation(
                transport.server(), route.version_negotiation_, endpoint);
            state_value.sent_ = result_value == http3_worker_datagram_endpoint::pump_result::pending;
            state_value.plan_consumed_ = !route.version_negotiation_.valid();
        }
        if (endpoint.consume_receive() == http3_worker_datagram_endpoint::pump_result::error) {
            throw std::system_error(endpoint.error(), "consume test QUIC datagram");
        }
        return http3_quic_wire_owner::protocol_pump_result_type::progress;
    } catch (...) {
        state_value.failure_ = std::current_exception();
        return http3_quic_wire_owner::protocol_pump_result_type::fatal;
    }
}

struct connection_pump_state final {
    std::optional<ruvia::quic_connection_token> connection_;
    std::size_t full_drives_{};
    std::size_t full_receives_{};
    std::size_t full_expirations_{};
    std::size_t full_expirations_with_deadline_{};
    std::optional<ruvia::quic_timestamp> last_full_expiry_;
    bool repeated_full_expiry_{};
    bool full_idle_retired_{};
    std::size_t packet_writes_{};
    std::size_t packets_sent_{};
    std::exception_ptr failure_;
};

http3_quic_wire_owner::protocol_pump_result_type pump_connection(void* context_value,
    http3_quic_server_transport& transport, http3_worker_datagram_endpoint& endpoint) noexcept {
    auto& state_value = *static_cast<connection_pump_state*>(context_value);
    try {
        const auto now = std::chrono::steady_clock::now();
        const bool full = endpoint.outbound_pending() && !endpoint.outbound_capacity();
        state_value.full_drives_ += full;
        const auto expiry = transport.server().next_expiry();
        const bool expired_while_full = full && expiry && *expiry <= now;
        (void)transport.server().handle_expiry(now);
        if (expired_while_full) {
            state_value.repeated_full_expiry_ |= state_value.last_full_expiry_ && *expiry <= *state_value.last_full_expiry_;
            state_value.last_full_expiry_ = expiry;
            ++state_value.full_expirations_;
            const auto next_value = transport.server().next_expiry();
            state_value.full_expirations_with_deadline_ += next_value.has_value();
            state_value.repeated_full_expiry_ |= next_value && *next_value <= *expiry;
            state_value.full_idle_retired_ |= state_value.connection_ &&
                                              transport.server().connection(*state_value.connection_).info().state_ == ruvia::quic_connection_state::retired;
        }
        bool progress_value{};
        if (const auto received = endpoint.receive_slot()) {
            const auto local = ruvia::detail::to_http3_quic_datagram_address(received->local_destination_);
            const auto peer = ruvia::detail::to_http3_quic_datagram_address(received->peer_);
            if ((local.index() != 0) || (peer.index() != 0)) {
                throw std::runtime_error("test QUIC datagram has invalid endpoint addresses");
            }
            auto route = transport.route_datagram(received->bytes_, std::get<0>(local), std::get<0>(peer));
            if (route.kind_ == ruvia::quic_server_route_kind::initial_offer && !state_value.connection_) {
                const auto admitted = transport.admit_initial(route.offer_, now);
                if (admitted.status_ == ruvia::quic_operation_status::accepted) {
                    state_value.connection_ = admitted.connection_;
                }
            } else if (route.kind_ == ruvia::quic_server_route_kind::existing_connection) {
                const auto status = transport.server().receive(route.connection_,
                    {received->bytes_, ruvia::detail::to_quic_address(std::get<0>(local)), ruvia::detail::to_quic_address(std::get<0>(peer))}, now);
                if (status == ruvia::quic_operation_status::accepted ||
                    status == ruvia::quic_operation_status::need_input) {
                    state_value.full_receives_ += full;
                }
            }
            if (endpoint.consume_receive() == http3_worker_datagram_endpoint::pump_result::error) {
                throw std::system_error(endpoint.error(), "consume test QUIC datagram");
            }
            progress_value = true;
        }
        if (state_value.connection_) {
            const auto output = endpoint.packet_buffer();
            if (!output.empty()) {
                ++state_value.packet_writes_;
                const auto packet = transport.server().connection(*state_value.connection_).write_packet(output, now);
                if (packet.size_ == 0) {
                    endpoint.cancel_packet();
                } else {
                    const auto source_value = ruvia::detail::to_udp_endpoint(ruvia::detail::from_quic_address(packet.local_));
                    const auto peer = ruvia::detail::to_udp_endpoint(ruvia::detail::from_quic_address(packet.peer_));
                    if ((source_value.index() != 0) || (peer.index() != 0)) {
                        throw std::runtime_error("test QUIC packet has invalid endpoint addresses");
                    }
                    if (endpoint.send_datagram(output.first(packet.size_), std::get<0>(source_value), std::get<0>(peer)) != http3_worker_datagram_endpoint::pump_result::pending) {
                        throw std::runtime_error("test QUIC endpoint did not accept reserved packet");
                    }
                    ++state_value.packets_sent_;
                    progress_value = true;
                }
            }
        }
        return progress_value ? http3_quic_wire_owner::protocol_pump_result_type::progress : http3_quic_wire_owner::protocol_pump_result_type::idle;
    } catch (...) {
        endpoint.cancel_packet();
        state_value.failure_ = std::current_exception();
        return http3_quic_wire_owner::protocol_pump_result_type::fatal;
    }
}

class wire_worker final {
public:
    wire_worker() {
        owner_.start();
    }
    ~wire_worker() {
        owner_.request_stop();
        owner_.join();
    }
    [[nodiscard]] ruvia::worker_runtime_context& runtime() noexcept {
        return owner_.context();
    }
    template <typename function>
    auto invoke(function operation) {
        using result = decltype(operation());
        auto task_value = std::make_shared<std::packaged_task<result()>>(std::move(operation));
        auto completion = task_value->get_future();
        if (!runtime().submission().post([task_value] { (*task_value)(); }).accepted() ||
            completion.wait_for(5s) != std::future_status::ready) {
            std::terminate();
        }
        return completion.get();
    }

private:
    ruvia::worker_runtime owner_{{.queue_capacity_ = 8}};
};

struct test_identity_files final {
    test_identity_files() {
        std::random_device random;
        directory_ = std::filesystem::temp_directory_path() /
                     ("ruvia-http3-network-wire-" + std::to_string(random()) + "-" +
                         std::to_string(random()));
        if (!std::filesystem::create_directory(directory_)) {
            throw std::runtime_error("could not create QUIC test TLS directory");
        }
        certificate_ = directory_ / "certificate.pem";
        private_key_ = directory_ / "private-key.pem";
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> key_context(
            EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr), EVP_PKEY_CTX_free);
        EVP_PKEY* raw_key = nullptr;
        if (!key_context || EVP_PKEY_keygen_init(key_context.get()) <= 0 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(key_context.get(), 2048) <= 0 ||
            EVP_PKEY_generate(key_context.get(), &raw_key) <= 0) {
            throw std::runtime_error("could not generate QUIC test TLS key");
        }
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new_ex(nullptr, nullptr), X509_free);
        if (!cert || X509_set_version(cert.get(), 2) != 1 ||
            ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) != 1 ||
            X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0) == nullptr ||
            X509_gmtime_adj(X509_getm_notAfter(cert.get()), 86400) == nullptr ||
            X509_set_pubkey(cert.get(), key.get()) != 1 ||
            X509_set_issuer_name(cert.get(), X509_get_subject_name(cert.get())) != 1 ||
            ruvia::test::sign_tls_certificate(cert.get(), key.get()) <= 0) {
            throw std::runtime_error("could not create QUIC test TLS certificate");
        }
        std::unique_ptr<BIO, decltype(&BIO_free)> cert_bio(
            BIO_new_file(certificate_.string().c_str(), "w"), BIO_free);
        std::unique_ptr<BIO, decltype(&BIO_free)> key_bio(
            BIO_new_file(private_key_.string().c_str(), "w"), BIO_free);
        if (!cert_bio || !key_bio || PEM_write_bio_X509(cert_bio.get(), cert.get()) != 1 ||
            ruvia::test::write_tls_private_key(key_bio.get(), key.get()) != 1) {
            throw std::runtime_error("could not write QUIC test TLS identity");
        }
    }
    ~test_identity_files() {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }
    std::filesystem::path directory_;
    std::filesystem::path certificate_;
    std::filesystem::path private_key_;
};

http3_quic_datagram_address quic_address(const udp_type::endpoint& endpoint) {
    const auto address = ruvia::detail::to_http3_quic_datagram_address(endpoint);
    if ((address.index() != 0)) {
        throw std::runtime_error("test UDP endpoint cannot be converted to QUIC address");
    }
    return std::get<0>(address);
}

bool is_long_header_type(std::span<const std::byte> packet, std::uint8_t type) {
    if (packet.size() < 5 || (std::to_integer<std::uint8_t>(packet[0]) & 0x80U) == 0) {
        return false;
    }
    const std::uint32_t version =
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(packet[1])) << 24U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(packet[2])) << 16U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(packet[3])) << 8U) |
        static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(packet[4]));
    return version == 1 && ((std::to_integer<std::uint8_t>(packet[0]) >> 4U) & 0x03U) == type;
}

ruvia::quic_connection_config client_config(udp_type::socket& peer, const udp_type::endpoint& server) {
    ruvia::quic_connection_config config;
    config.local_address_ = ruvia::detail::to_quic_address(quic_address(peer.local_endpoint()));
    config.peer_address_ = ruvia::detail::to_quic_address(quic_address(server));
    return config;
}

std::size_t pump_peer(http3_quic_client_transport& client, udp_type::socket& socket) {
    const auto now = std::chrono::steady_clock::now();
    const auto check_core_state = [&client] {
        const auto state_value = client.connection().info().state_;
        if (state_value == ruvia::quic_connection_state::failed ||
            state_value == ruvia::quic_connection_state::retired) {
            throw std::runtime_error("test QUIC client entered a terminal state");
        }
    };
    std::array<std::byte, 2048> output{};
    std::size_t sent{};
    for (std::size_t count = 0; count < 32; ++count) {
        const auto packet = client.write_packet(output, now);
        check_core_state();
        if (packet.size_ == 0) {
            break;
        }
        const auto destination = ruvia::detail::to_udp_endpoint(
            ruvia::detail::from_quic_address(packet.peer_));
        if ((destination.index() != 0)) {
            throw std::runtime_error("test QUIC client packet has invalid destination");
        }
        asio::error_code error;
        const auto size = socket.send_to(asio::buffer(output.data(), packet.size_),
            std::get<0>(destination), 0, error);
        if (error || size != packet.size_) {
            throw std::system_error(error ? error : std::make_error_code(std::errc::io_error),
                "send test QUIC client packet");
        }
        ++sent;
    }

    std::array<std::byte, 2048> input{};
    for (;;) {
        udp_type::endpoint source;
        asio::error_code error;
        const auto size = socket.receive_from(asio::buffer(input), source, 0, error);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            break;
        }
        if (error) {
            throw std::system_error(error, "receive test QUIC client packet");
        }
        const auto local = ruvia::detail::to_quic_address(quic_address(socket.local_endpoint()));
        const auto remote = ruvia::detail::to_quic_address(quic_address(source));
        const ruvia::quic_datagram_view datagram{
            .bytes_ = std::span<const std::byte>(input.data(), size),
            .local_ = local,
            .peer_ = remote,
        };
        (void)client.receive(datagram, std::chrono::steady_clock::now());
        check_core_state();
    }
    const auto expiry_now = std::chrono::steady_clock::now();
    if (const auto expiry = client.next_expiry(); expiry && *expiry <= expiry_now) {
        (void)client.handle_expiry(expiry_now);
        check_core_state();
    }
    return sent;
}

// Real Acceptor/channel/worker path shared by the wire-driver tests.
class native_wire_fixture final {
public:
    native_wire_fixture(ruvia::worker_runtime_context& runtime, asio::io_context& io, udp_type::endpoint bind, http3_quic_tls_context& tls,
        ruvia::quic_server_config config, std::pmr::memory_resource* resource,
        http3_quic_wire_owner::protocol_pump_type pump)
        : io_(io),
          runtime_(runtime),
          notification_(runtime_),
          pool_(1 + channel::default_input_capacity + channel::default_output_window, channel::packet_capacity),
          packets_(pool_, notification_),
          native_(io, bind, {nullptr, native_notification}, pool_),
          timer_(io) {
        packets_.stage_worker(runtime_);
        packets_.worker_start();
        native_.prepare();
        (void)native_.start();
        owner_ = std::make_unique<http3_quic_wire_owner>(io, packets_,
            udp_type::endpoint(bind.address(), native_.bound_port()), tls, config, resource, pump);
        arm();
    }
    ~native_wire_fixture() {
        owner_->request_stop();
        const auto deadline_value = std::chrono::steady_clock::now() + 2s;
        while ((!owner_->stop_status().complete() || !native_.endpoint_retired()) && std::chrono::steady_clock::now() < deadline_value) {
            poll();
            if (io_.stopped()) {
                io_.restart();
            }
            io_.run_for(1ms);
            owner_->poll_stop();
        }
        if (!owner_->stop_status().complete() || !native_.endpoint_retired()) {
            std::terminate();
        }
        owner_.reset();
        packets_.worker_close();
        packets_.acceptor_poll();
        if (!packets_.acceptor_finalize()) {
            std::terminate();
        }
        timer_.cancel();
        if (io_.stopped()) {
            io_.restart();
        }
        io_.poll();
        notification_.close();
    }
    http3_quic_wire_owner& owner() noexcept {
        return *owner_;
    }
    bool native_retired() const noexcept {
        return native_.endpoint_retired();
    }
    void poll() noexcept {
        packets_.acceptor_poll();
        if (owner_->stop_status().stopping_) {
            packets_.acceptor_close();
            native_.request_stop();
            while (packets_.acceptor_take_output()) {
            }
        } else {
            if (auto packet = native_.take_receive()) {
                (void)packets_.acceptor_push(std::move(*packet));
            }
            native_.poll_receive();
            if (native_.outbound_capacity()) {
                if (auto packet = packets_.acceptor_take_output()) {
                    (void)native_.send_owned_datagram(std::move(*packet));
                }
            }
        }
        owner_->poll_datagrams();
    }

private:
    using channel = ruvia::detail::http3_datagram_channel;
    static void native_notification(void*, ruvia::detail::http3_acceptor_datagram_endpoint::notification_kind) noexcept {}
    void arm() {
        timer_.expires_after(1ms);
        timer_.async_wait([this](const asio::error_code& error) {
            if (error) {
                return;
            }
            poll();
            arm();
        });
    }
    asio::io_context& io_;

    ruvia::worker_runtime_context& runtime_;
    ruvia::worker_notification notification_;
    ruvia::buffer_pool pool_;
    channel packets_;
    ruvia::detail::http3_acceptor_datagram_endpoint native_;
    asio::steady_timer timer_;
    std::unique_ptr<http3_quic_wire_owner> owner_;
};

void drain_owner(asio::io_context& io, native_wire_fixture& fixture_value) {
    auto& owner_value = fixture_value.owner();
    owner_value.request_stop();
    const auto deadline_value = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline_value) {
        fixture_value.poll();
        if (io.stopped()) {
            io.restart();
        }
        io.run_for(2ms);
        owner_value.poll_stop();
        if (owner_value.stop_status().complete() && fixture_value.native_retired()) {
            return;
        }
    }
    throw std::runtime_error("HTTP/3 network QUIC wire owner did not drain");
}

void exercise_owner_allocation_failure(ruvia::testing::test_context& ruvia_ctx,
    std::size_t fail_at, ruvia::worker_runtime_context& worker_context) {
    test_identity_files files;
    ruvia::detail::http_server_listener_definition::tls_type tls_config;
    tls_config.identity_.certificate_chain_file_ = files.certificate_.string();
    tls_config.identity_.private_key_file_ = files.private_key_.string();
    http3_quic_tls_context tls(tls_config, std::pmr::get_default_resource());
    http3_quic_client_tls_context client_tls({
        .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification,
    });
    fail_on_allocation_resource allocation_resource(fail_at);
    asio::io_context io;
    connection_pump_state pump;
    native_wire_fixture fixture_value(worker_context, io, udp_type::endpoint(asio::ip::address_v4::loopback(), 0), tls,
        {}, &allocation_resource, {&pump, &pump_connection});
    auto& owner_value = fixture_value.owner();
    try {
        owner_value.prepare();
        owner_value.start();
    } catch (...) {
        RUVIA_CHECK(owner_value.failure() != nullptr);
    }

    if (!owner_value.failure()) {
        udp_type::socket peer(io, udp_type::endpoint(asio::ip::address_v4::loopback(), 0));
        peer.non_blocking(true);
        const udp_type::endpoint server(asio::ip::address_v4::loopback(), owner_value.bound_port());
        http3_quic_client_transport client(client_tls, client_config(peer, server), "localhost",
            std::chrono::steady_clock::now());
        RUVIA_CHECK(pump_peer(client, peer) != 0);
        std::array<std::byte, 2048> packet{};
        udp_type::endpoint source;
        std::size_t packet_size{};
        bool received_retry{};
        const auto retry_deadline = std::chrono::steady_clock::now() + 2s;
        while (!received_retry && !owner_value.failure() &&
               std::chrono::steady_clock::now() < retry_deadline) {
            asio::error_code error;
            packet_size = peer.receive_from(asio::buffer(packet), source, 0, error);
            if (!error) {
                received_retry = true;
                break;
            }
            RUVIA_CHECK(error == asio::error::would_block || error == asio::error::try_again);
            if (error != asio::error::would_block && error != asio::error::try_again) {
                break;
            }
            if (io.stopped()) {
                io.restart();
            }
            io.run_for(2ms);
        }
        RUVIA_CHECK(received_retry || owner_value.failure() != nullptr);
        if (received_retry) {
            RUVIA_CHECK(source == server);
            const std::span<const std::byte> retry(packet.data(), packet_size);
            RUVIA_CHECK(is_long_header_type(retry, 0));
            const auto local = ruvia::detail::to_quic_address(quic_address(peer.local_endpoint()));
            const auto remote = ruvia::detail::to_quic_address(quic_address(source));
            (void)client.receive({retry, local, remote}, std::chrono::steady_clock::now());
            RUVIA_CHECK(client.connection().info().state_ != ruvia::quic_connection_state::failed);
            RUVIA_CHECK(pump_peer(client, peer) != 0);
        }
        const auto deadline_value = std::chrono::steady_clock::now() + 3s;
        while (!owner_value.failure() && std::chrono::steady_clock::now() < deadline_value) {
            if (io.stopped()) {
                io.restart();
            }
            io.run_for(2ms);
        }
    }
    RUVIA_CHECK(owner_value.failure() != nullptr);
    RUVIA_CHECK(allocation_resource.allocation_attempts() == fail_at);
    drain_owner(io, fixture_value);
    const auto done = owner_value.stop_status();
    RUVIA_CHECK(done.complete());
    RUVIA_CHECK(done.endpoint_retired_);
    RUVIA_CHECK(done.timer_handlers_retired_);
    RUVIA_CHECK(done.transport_destroyed_);
    RUVIA_CHECK(!done.outbound_pending_);
    RUVIA_CHECK(done.failed_);
    RUVIA_CHECK(allocation_resource.outstanding_allocations() == 0);
}
struct runtime_burst_observation final {
    std::optional<std::size_t> business_head_;
    std::optional<std::size_t> timer_head_;
    bool input_empty_{};
    bool output_empty_{};
    std::size_t active_{};
    std::size_t refused_{};
    std::exception_ptr failure_;
};

struct runtime_burst_fixture final {
    using channel = ruvia::detail::http3_datagram_channel;

    ruvia::worker_memory memory_;
    ruvia::detail::http_server_options options_;
    ruvia::detail::router router_;
    ruvia::detail::router_impl& routes_{ruvia::detail::router_impl::from(router_)};
    ruvia::detail::worker_capabilities capabilities_;
    ruvia::connection_scanner scanner_;
    ruvia::stop_source stop_;
    ruvia::stop_token stop_token_;
    std::atomic<std::size_t> active_{};
    std::atomic<std::size_t> refused_{};
    ruvia::detail::http3_worker_server server_;
    std::exception_ptr failure_;
    ruvia::detail::http3_worker_runtime protocol_;
    ruvia::task_scope runners_;
    ruvia::worker_signal business_ready_;
    asio::steady_timer business_timer_;
    asio::steady_timer observation_timer_;
    channel& packets_;
    std::optional<std::size_t> business_head_;
    std::optional<std::size_t> timer_head_;

    runtime_burst_fixture(ruvia::worker_runtime_context& worker_value, channel& input,
        const udp_type::endpoint& local, const ruvia::detail::http_server_listener_definition::tls_type& tls)
        : capabilities_(worker_value.io_context(), worker_value.handle(), memory_.resource(), {}, {}),
          scanner_(worker_value.handle(), {}),
          stop_token_(stop_.token()),
          server_(worker_value.handle(), memory_, finalize_routes(), capabilities_, scanner_,
              worker_value.io_context().get_executor(), options_, stop_token_, 1, 8, active_, refused_),
          protocol_(worker_value, local, tls, ruvia::http3_listen_config{},
              {.server_ = &server_, .max_connections_ = 1, .buffer_capacity_ = 8, .max_requests_per_connection_ = 8},
              input, {}, {this, [](void* context_value, std::exception_ptr error) noexcept {
                              static_cast<runtime_burst_fixture*>(context_value)->failure_ =
                                  std::move(error);
                          }}),
          runners_(worker_value.handle(), {.resource_ = memory_.resource()}),
          business_ready_(worker_value.handle()),
          business_timer_(worker_value.io_context()),
          observation_timer_(worker_value.io_context()),
          packets_(input) {
        protocol_.stage();
    }

    [[nodiscard]] const ruvia::detail::route_table& finalize_routes() {
        routes_.finalize();
        return routes_.route_table();
    }

    [[nodiscard]] std::optional<std::size_t> input_head() noexcept {
        const auto input = packets_.worker_input();
        if (!input) {
            return std::nullopt;
        }
        return std::to_integer<std::size_t>(input->bytes_[1]) |
               (std::to_integer<std::size_t>(input->bytes_[2]) << 8U);
    }

    [[nodiscard]] ruvia::task<void> run_business_task() {
        co_await business_ready_.wait();
        business_head_ = input_head();
    }

    void release(std::promise<runtime_burst_observation>& observed_value) {
        runners_.spawn(run_business_task());
        business_ready_.notify();
        business_timer_.expires_at(std::chrono::steady_clock::now());
        business_timer_.async_wait([this](const asio::error_code& error) {
            if (!error) {
                timer_head_ = input_head();
            }
        });
        // This read-only snapshot precedes the runtime's 10ms monitor. Neither
        // observer pumps protocol work or publishes another datagram edge.
        observation_timer_.expires_after(5ms);
        observation_timer_.async_wait([this, &observed_value](const asio::error_code& error) {
            if (error) {
                observed_value.set_exception(std::make_exception_ptr(std::system_error(error)));
                return;
            }
            observed_value.set_value({business_head_, timer_head_, !packets_.worker_input(),
                packets_.worker_outbound_count() == 0, active_.load(), refused_.load(), failure_});
        });
        runners_.spawn(server_.run());
        runners_.spawn(protocol_.run_datagrams());
        protocol_.start();
    }

    [[nodiscard]] ruvia::task<void> join_cold_retirement() {
        co_await protocol_.join();
        co_await runners_.join();
    }
};

}  // namespace

RUVIA_TEST(http3_worker_runtime_coalesced_drop_burst_drains_without_external_edges_and_yields) {
    using channel = ruvia::detail::http3_datagram_channel;
    constexpr std::size_t burst_size = 256;
    constexpr std::size_t output_window = 16;
    test_identity_files files;
    ruvia::detail::http_server_listener_definition::tls_type tls;
    tls.identity_.certificate_chain_file_ = files.certificate_.string();
    tls.identity_.private_key_file_ = files.private_key_.string();
    asio::io_context acceptor_io;
    ruvia::worker_runtime_context acceptor_runtime(acceptor_io, 8);
    ruvia::worker_notification acceptor_notification(acceptor_runtime);
    ruvia::buffer_pool pool(burst_size + output_window, channel::packet_capacity);
    wire_worker worker;
    channel packets(pool, acceptor_notification, nullptr, burst_size, output_window);
    packets.stage_worker(worker.runtime());
    const udp_type::endpoint local(asio::ip::address_v4::loopback(), 4433);
    const udp_type::endpoint peer(asio::ip::address_v4::loopback(), 43210);
    std::unique_ptr<runtime_burst_fixture> fixture;
    RUVIA_CHECK(worker.invoke([&] {
        fixture = std::make_unique<runtime_burst_fixture>(
            worker.runtime(), packets, local, tls);
        return fixture->server_.install();
    }));
    std::promise<runtime_burst_observation> observed;
    auto observation_value = observed.get_future();
    // No runner exists yet. Every notification therefore belongs to one
    // coalesced RX burst; there is no follow-up UDP or completion-credit edge.
    for (std::size_t index = 0; index < burst_size; ++index) {
        auto storage = pool.try_acquire();
        RUVIA_CHECK(storage.has_value());
        storage->bytes()[0] = std::byte{0};
        storage->bytes()[1] = static_cast<std::byte>(index & 0xffU);
        storage->bytes()[2] = static_cast<std::byte>(index >> 8U);
        RUVIA_CHECK(packets.acceptor_push({std::move(*storage), 3, local, peer}));
    }
    worker.invoke([&] { fixture->release(observed); });
    // Waiting is Acceptor-only. Do not poll credits or post worker commands
    // until the read-only owner snapshot proves the complete burst was consumed.
    const bool observed_in_time = observation_value.wait_for(2s) == std::future_status::ready;
    RUVIA_CHECK(observed_in_time);
    if (!observed_in_time) {
        std::terminate();
    }
    const auto snapshot = observation_value.get();
    RUVIA_CHECK(snapshot.business_head_.has_value());
    RUVIA_CHECK(snapshot.timer_head_.has_value());
    if (snapshot.business_head_) {
        RUVIA_CHECK(*snapshot.business_head_ > 0 && *snapshot.business_head_ < burst_size);
    }
    if (snapshot.timer_head_) {
        RUVIA_CHECK(*snapshot.timer_head_ > 0 && *snapshot.timer_head_ < burst_size);
    }
    RUVIA_CHECK(snapshot.input_empty_);
    RUVIA_CHECK(snapshot.output_empty_);
    RUVIA_CHECK_EQ(snapshot.active_, std::size_t{0});
    RUVIA_CHECK_EQ(snapshot.refused_, std::size_t{0});
    RUVIA_CHECK(!snapshot.failure_);
    RUVIA_CHECK(!packets.acceptor_take_output());
    RUVIA_CHECK_EQ(pool.outstanding(), burst_size + output_window);
    // Aggregate credit returns are bounded per poll, independently of the
    // already-drained RX queue. Only now may Acceptor replenish worker credits.
    for (std::size_t turn = 0;
        turn < burst_size && pool.outstanding() > output_window; ++turn) {
        packets.acceptor_poll();
    }
    RUVIA_CHECK_EQ(pool.outstanding(), output_window);
    packets.acceptor_close();
    std::promise<void> joined;
    auto completion = joined.get_future();
    worker.invoke([&] {
        fixture->protocol_.stop();
        asio::co_spawn(worker.runtime().io_context(),
            ruvia::as_awaitable(fixture->runners_.join()),
            [&joined](std::exception_ptr error) {
                if (error) {
                    joined.set_exception(std::move(error));
                } else {
                    joined.set_value();
                }
            });
    });
    const auto join_deadline = std::chrono::steady_clock::now() + 2s;
    while (completion.wait_for(0s) != std::future_status::ready &&
           std::chrono::steady_clock::now() < join_deadline) {
        // Keep the real Acceptor owner advancing unused TX returns and final
        // completion credits while the worker's structured runners retire.
        packets.acceptor_poll();
        (void)completion.wait_for(1ms);
    }
    const bool joined_in_time = completion.wait_for(0s) == std::future_status::ready;
    RUVIA_CHECK(joined_in_time);
    if (!joined_in_time) {
        std::terminate();
    }
    completion.get();
    packets.acceptor_poll();
    const bool drained = worker.invoke([&] {
        const bool done = fixture->server_.drained() && fixture->protocol_.drained() &&
                          !fixture->failure_;
        fixture.reset();
        return done;
    });
    RUVIA_CHECK(drained);
    RUVIA_CHECK(packets.worker_closed());
    RUVIA_CHECK(packets.acceptor_finalize());
    RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
}

RUVIA_TEST(http3_worker_server_cold_protocol_retirement_joins_started_handler_and_returns_channel_loans) {
    using channel = ruvia::detail::http3_datagram_channel;
    test_identity_files files;
    ruvia::detail::http_server_listener_definition::tls_type tls;
    tls.identity_.certificate_chain_file_ = files.certificate_.string();
    tls.identity_.private_key_file_ = files.private_key_.string();
    asio::io_context acceptor_io;
    ruvia::worker_runtime_context acceptor_runtime(acceptor_io, 8);
    ruvia::worker_notification acceptor_notification(acceptor_runtime);
    ruvia::buffer_pool pool(8, channel::packet_capacity);
    wire_worker worker;
    auto packets = std::make_unique<channel>(pool, acceptor_notification, nullptr, 2, 2);
    packets->stage_worker(worker.runtime());
    const udp_type::endpoint local(asio::ip::address_v4::loopback(), 4433);
    std::unique_ptr<runtime_burst_fixture> fixture;
    std::promise<void> joined;
    auto completion = joined.get_future();
    worker.invoke([&] {
        fixture = std::make_unique<runtime_burst_fixture>(
            worker.runtime(), *packets, local, tls);
        RUVIA_CHECK(fixture->server_.install());
        fixture->runners_.spawn(fixture->server_.run());
        RUVIA_CHECK(fixture->server_.run_started());
        RUVIA_CHECK(!fixture->protocol_.runner_started());
        fixture->protocol_.abandon_before_launch();
        asio::co_spawn(worker.runtime().io_context(),
            ruvia::as_awaitable(fixture->join_cold_retirement()),
            [&joined](std::exception_ptr error) {
                if (error) {
                    joined.set_exception(std::move(error));
                } else {
                    joined.set_value();
                }
            });
    });
    RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{2});
    packets->acceptor_close();
    const auto deadline_value = std::chrono::steady_clock::now() + 2s;
    while (completion.wait_for(0s) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline_value) {
        packets->acceptor_poll();
        (void)completion.wait_for(1ms);
    }
    const bool completed = completion.wait_for(0s) == std::future_status::ready;
    RUVIA_CHECK(completed);
    if (!completed) {
        std::terminate();
    }
    completion.get();
    RUVIA_CHECK(packets->worker_closed());
    RUVIA_CHECK(packets->acceptor_finalize());
    RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
    // application destroys Acceptor channels before phase-two worker finalization.
    // Repeated lifecycle requests and protocol destruction must not borrow them.
    packets.reset();
    RUVIA_CHECK(worker.invoke([&] {
        fixture->protocol_.stop();
        fixture->protocol_.abandon_before_launch();
        const bool drained = fixture->server_.drained() && fixture->protocol_.drained() &&
                             !fixture->failure_;
        fixture.reset();
        return drained;
    }));
    RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
}

RUVIA_TEST(http3_network_queue_deduplicates_initial_offers_and_preserves_capacity) {
    std::pmr::vector<ruvia::quic_initial_offer> pending;
    pending.reserve(2);
    ruvia::quic_initial_offer offer{.offer_id_ = 1};
    RUVIA_CHECK(ruvia::detail::queue_http3_initial_offer(pending, offer));
    RUVIA_CHECK(!ruvia::detail::queue_http3_initial_offer(pending, offer));
    RUVIA_CHECK_EQ(pending.size(), std::size_t{1});

    std::uint64_t next_id = 2;
    while (pending.size() < pending.capacity()) {
        offer.offer_id_ = next_id++;
        RUVIA_CHECK(ruvia::detail::queue_http3_initial_offer(pending, offer));
    }
    offer.offer_id_ = next_id;
    RUVIA_CHECK(!ruvia::detail::queue_http3_initial_offer(pending, offer));
    RUVIA_CHECK_EQ(pending.size(), pending.capacity());
}

RUVIA_TEST(http3_network_sends_version_negotiation_from_owned_udp_slot_without_admission) {
    wire_worker runner;
    runner.invoke([&] {
        auto& worker_context = runner.runtime();
        test_identity_files files;
        ruvia::detail::http_server_listener_definition::tls_type tls_config;
        tls_config.identity_.certificate_chain_file_ = files.certificate_.string();
        tls_config.identity_.private_key_file_ = files.private_key_.string();
        http3_quic_tls_context tls(tls_config, std::pmr::get_default_resource());
        asio::io_context io;
        version_negotiation_pump_state pump_state;
        native_wire_fixture fixture_value(worker_context, io, udp_type::endpoint(asio::ip::address_v4::loopback(), 0), tls,
            {}, nullptr, {&pump_state, &pump_version_negotiation});
        auto& owner_value = fixture_value.owner();
        owner_value.prepare();
        owner_value.start();

        udp_type::socket peer(io, udp_type::endpoint(asio::ip::address_v4::loopback(), 0));
        std::array<std::byte, ruvia::detail::http3_udp_socket::datagram_buffer_size> response{};
        udp_type::endpoint response_source;
        asio::error_code receive_error;
        std::size_t response_size{};
        bool receive_complete{};
        peer.async_receive_from(asio::buffer(response), response_source,
            [&](const asio::error_code& error, std::size_t size) noexcept {
                receive_error = error;
                response_size = size;
                receive_complete = true;
            });

        std::array<std::byte, 1200> unsupported_version{};
        unsupported_version[0] = std::byte{0xc0};
        unsupported_version[4] = std::byte{0x02};
        unsupported_version[5] = std::byte{8};
        unsupported_version[14] = std::byte{8};
        for (std::size_t i = 0; i < 8; ++i) {
            unsupported_version[6 + i] = static_cast<std::byte>(0x10 + i);
            unsupported_version[15 + i] = static_cast<std::byte>(0x20 + i);
        }
        const udp_type::endpoint server(asio::ip::address_v4::loopback(), owner_value.bound_port());
        asio::error_code send_error;
        RUVIA_CHECK(peer.send_to(asio::buffer(unsupported_version), server, 0, send_error) ==
                    unsupported_version.size());
        RUVIA_CHECK(!send_error);

        const auto deadline_value = std::chrono::steady_clock::now() + 2s;
        while (!receive_complete && std::chrono::steady_clock::now() < deadline_value) {
            if (io.stopped()) {
                io.restart();
            }
            io.run_for(2ms);
        }
        RUVIA_CHECK(receive_complete);
        RUVIA_CHECK(!receive_error);
        RUVIA_CHECK(pump_state.routed_);
        RUVIA_CHECK(pump_state.sent_);
        RUVIA_CHECK(pump_state.plan_consumed_);
        RUVIA_CHECK(!pump_state.failure_);
        RUVIA_CHECK(response_size > 5);
        if (response_size > 5) {
            RUVIA_CHECK_EQ(response[1], std::byte{});
            RUVIA_CHECK_EQ(response[2], std::byte{});
            RUVIA_CHECK_EQ(response[3], std::byte{});
            RUVIA_CHECK_EQ(response[4], std::byte{});
        }
        RUVIA_CHECK(response_source == server);
        RUVIA_CHECK(owner_value.transport() != nullptr);
        if (owner_value.transport() != nullptr) {
            RUVIA_CHECK_EQ(owner_value.transport()->server().connection_count(), std::size_t{0});
            RUVIA_CHECK_EQ(owner_value.transport()->server().pending_connection_count(), std::size_t{0});
        }
        RUVIA_CHECK(!owner_value.failure());
        drain_owner(io, fixture_value);
        RUVIA_CHECK(owner_value.stop_status().complete());
        RUVIA_CHECK(!owner_value.stop_status().failed_);
    });
}

RUVIA_TEST(http3_network_quic_wire_owner_handles_initial_and_bounds_timer_progress) {
    wire_worker runner;
    runner.invoke([&] {
        auto& worker_context = runner.runtime();
        test_identity_files files;
        ruvia::detail::http_server_listener_definition::tls_type tls_config;
        tls_config.identity_.certificate_chain_file_ = files.certificate_.string();
        tls_config.identity_.private_key_file_ = files.private_key_.string();
        http3_quic_tls_context tls(tls_config, std::pmr::get_default_resource());
        http3_quic_client_tls_context client_tls({
            .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification,
        });
        asio::io_context io;
        connection_pump_state pump;
        native_wire_fixture fixture_value(worker_context, io, udp_type::endpoint(asio::ip::address_v4::loopback(), 0), tls,
            {}, nullptr, {&pump, &pump_connection});
        auto& owner_value = fixture_value.owner();
        owner_value.prepare();
        RUVIA_CHECK(owner_value.bound_port() != 0);
        udp_type::socket peer(io, udp_type::endpoint(asio::ip::address_v4::loopback(), 0));
        peer.non_blocking(true);
        const udp_type::endpoint server(asio::ip::address_v4::loopback(), owner_value.bound_port());
        http3_quic_client_transport client(client_tls, client_config(peer, server), "localhost",
            std::chrono::steady_clock::now());
        RUVIA_CHECK(pump_peer(client, peer) != 0);
        RUVIA_CHECK(client.connection().info().state_ == ruvia::quic_connection_state::connecting);

        io.poll();
        std::array<std::byte, 2048> before_start{};
        udp_type::endpoint before_start_source;
        asio::error_code before_start_error;
        (void)peer.receive_from(asio::buffer(before_start), before_start_source, 0, before_start_error);
        RUVIA_CHECK(before_start_error == asio::error::would_block ||
                    before_start_error == asio::error::try_again);
        RUVIA_CHECK(owner_value.timer_expirations() == 0);
        io.restart();
        owner_value.start();

        std::array<std::byte, 2048> packet{};
        udp_type::endpoint source;
        std::size_t packet_size{};
        bool received_retry{};
        const auto receive_deadline = std::chrono::steady_clock::now() + 2s;
        while (!received_retry && std::chrono::steady_clock::now() < receive_deadline) {
            asio::error_code error;
            packet_size = peer.receive_from(asio::buffer(packet), source, 0, error);
            if (!error) {
                received_retry = true;
                break;
            }
            RUVIA_CHECK(error == asio::error::would_block || error == asio::error::try_again);
            if (error != asio::error::would_block && error != asio::error::try_again) {
                break;
            }
            io.run_for(2ms);
        }
        RUVIA_CHECK(received_retry);
        RUVIA_CHECK(source == server);
        const std::span<const std::byte> retry(packet.data(), packet_size);
        RUVIA_CHECK(is_long_header_type(retry, 0));
        if (received_retry && is_long_header_type(retry, 0)) {
            RUVIA_CHECK(packet_size > 21);
            const auto local = ruvia::detail::to_quic_address(quic_address(peer.local_endpoint()));
            const auto remote = ruvia::detail::to_quic_address(quic_address(source));
            const auto received_value = client.receive({retry, local, remote}, std::chrono::steady_clock::now());
            RUVIA_CHECK(received_value == ruvia::quic_operation_status::accepted || received_value == ruvia::quic_operation_status::need_input);
            RUVIA_CHECK(client.connection().info().state_ == ruvia::quic_connection_state::connecting);
            RUVIA_CHECK(pump_peer(client, peer) != 0);
            io.run_for(1200ms);
            RUVIA_CHECK(owner_value.timer_expirations() > 0);
            RUVIA_CHECK(owner_value.timer_expirations() < 1600);
            constexpr std::array<char, 4> close_reason{'d', 'o', 'n', 'e'};
            const auto close_status = client.connection().close({
                .kind_ = ruvia::quic_close_kind::application,
                .code_ = 0,
                .frame_type_ = 0,
                .reason_ = close_reason,
            });
            RUVIA_CHECK(close_status == ruvia::quic_operation_status::accepted);
            RUVIA_CHECK(pump_peer(client, peer) != 0);
            io.run_for(10ms);
        }
        RUVIA_CHECK(!owner_value.failure());
        drain_owner(io, fixture_value);
        RUVIA_CHECK(owner_value.stop_status().complete());
        RUVIA_CHECK(!owner_value.stop_status().failed_);
    });
}

RUVIA_TEST(http3_forwarded_wire_owner_drives_input_and_timer_while_udp_output_window_is_full) {
    using channel = ruvia::detail::http3_datagram_channel;
    test_identity_files files;
    ruvia::detail::http_server_listener_definition::tls_type tls_config;
    tls_config.identity_.certificate_chain_file_ = files.certificate_.string();
    tls_config.identity_.private_key_file_ = files.private_key_.string();
    http3_quic_tls_context tls(tls_config, std::pmr::get_default_resource());
    http3_quic_client_tls_context client_tls({
        .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification,
    });
    asio::io_context acceptor_io;
    ruvia::worker_runtime_context acceptor_runtime(acceptor_io, 8);
    ruvia::worker_notification acceptor_notification(acceptor_runtime);
    ruvia::buffer_pool pool(8, channel::packet_capacity);
    wire_worker worker;
    constexpr std::size_t output_window = 1;
    channel packets(pool, acceptor_notification, nullptr, 2, output_window);
    packets.stage_worker(worker.runtime());
    ruvia::detail::http3_acceptor_datagram_endpoint acceptor(acceptor_io,
        udp_type::endpoint(asio::ip::address_v4::loopback(), 0),
        {nullptr, [](void*, ruvia::detail::http3_acceptor_datagram_endpoint::notification_kind) noexcept {}}, pool);
    acceptor.prepare();
    RUVIA_CHECK(acceptor.start() == http3_worker_datagram_endpoint::pump_result::pending);
    const udp_type::endpoint server(asio::ip::address_v4::loopback(), acceptor.bound_port());
    connection_pump_state pump;
    std::unique_ptr<http3_quic_wire_owner> owner;
    ruvia::quic_server_config server_config;
    // ngtcp2 applies its authoritative max(idle_timeout, 3*PTO) minimum.
    // Even with TX held, its independent idle deadline must retire the peer.
    server_config.local_transport_parameters_.idle_timeout_ms_ = 1;
    worker.invoke([&] {
        packets.worker_start();
        owner = std::make_unique<http3_quic_wire_owner>(worker.runtime().io_context(), packets,
            server, tls, server_config, nullptr,
            http3_quic_wire_owner::protocol_pump_type{&pump, &pump_connection});
        owner->prepare();
        owner->start();
    });
    udp_type::socket peer(acceptor_io, udp_type::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_transport client(client_tls, client_config(peer, server), "localhost",
        std::chrono::steady_clock::now());
    std::array<std::byte, 2048> initial_bytes{};
    const auto initial_value = client.write_packet(initial_bytes, std::chrono::steady_clock::now());
    RUVIA_CHECK(initial_value.size_ != 0);
    RUVIA_CHECK(is_long_header_type(std::span<const std::byte>(initial_bytes).first(initial_value.size_), 0));
    const auto send_initial = [&] {
        asio::error_code error;
        const auto sent = peer.send_to(asio::buffer(initial_bytes.data(), initial_value.size_), server, 0, error);
        RUVIA_CHECK(!error);
        RUVIA_CHECK_EQ(sent, initial_value.size_);
    };
    const auto forward_input = [&] {
        const auto deadline_value = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline_value) {
            acceptor.poll_receive();
            if (acceptor_io.stopped()) {
                acceptor_io.restart();
            }
            acceptor_io.run_for(2ms);
            if (auto input = acceptor.take_receive()) {
                return packets.acceptor_push(std::move(*input));
            }
        }
        return false;
    };
    send_initial();
    RUVIA_CHECK(forward_input());
    worker.invoke([&] { owner->poll_datagrams(); });
    auto held_output = packets.acceptor_take_output();
    RUVIA_CHECK(held_output.has_value());
    if (held_output) {
        RUVIA_CHECK(is_long_header_type(held_output->view().bytes_, 0));
        RUVIA_CHECK(held_output->local_destination_ == server);
        RUVIA_CHECK(held_output->peer_ == peer.local_endpoint());
    }
    std::array<std::byte, channel::packet_capacity> held_bytes{};
    if (held_output) {
        std::ranges::copy(held_output->view().bytes_, held_bytes.begin());
    }
    const auto before_input = worker.invoke([&] {
        RUVIA_CHECK(pump.connection_.has_value());
        RUVIA_CHECK(owner->stop_status().outbound_pending_);
        RUVIA_CHECK_EQ(packets.worker_outbound_count(), output_window);
        RUVIA_CHECK(!packets.worker_outbound_capacity());
        RUVIA_CHECK(owner->transport()->server().next_expiry().has_value());
        return pump;
    });

    // Replay a real client Initial while Acceptor still holds the server's UDP
    // completion lease. RX routing/receive must run even without a TX credit.
    send_initial();
    RUVIA_CHECK(forward_input());
    const auto after_input = worker.invoke([&] {
        owner->request_drive();
        RUVIA_CHECK(!packets.worker_input());
        RUVIA_CHECK(owner->stop_status().outbound_pending_);
        RUVIA_CHECK(!packets.worker_outbound_capacity());
        return pump;
    });
    RUVIA_CHECK(after_input.full_drives_ > before_input.full_drives_);
    RUVIA_CHECK_EQ(after_input.full_receives_, before_input.full_receives_ + 1);
    RUVIA_CHECK_EQ(after_input.packet_writes_, before_input.packet_writes_);
    RUVIA_CHECK_EQ(after_input.packets_sent_, before_input.packets_sent_);
    packets.acceptor_poll();

    struct timer_observation final {
        connection_pump_state pump_;
        std::size_t expirations_{};
        std::chrono::steady_clock::time_point observed_at_{};
        bool deadline_{};
        bool full_{};
        bool failed_{};
    };
    const auto observe_timer = [&] {
        return worker.invoke([&] {
            return timer_observation{
                pump, owner->timer_expirations(), std::chrono::steady_clock::now(),
                owner->transport() && owner->transport()->server().next_expiry().has_value(),
                owner->stop_status().outbound_pending_ && !packets.worker_outbound_capacity(),
                owner->failure() != nullptr || pump.failure_ != nullptr};
        });
    };
    const auto before_timer = observe_timer();
    auto after_timer = before_timer;
    const auto timer_deadline = std::chrono::steady_clock::now() + 6s;
    // Only read worker state: no request_drive/poll_datagrams can masquerade
    // as a timer callback. Native PTO deadlines must advance while production
    // waits for credit, and the independent idle expiry must still retire.
    while (!after_timer.pump_.full_idle_retired_ &&
           std::chrono::steady_clock::now() < timer_deadline && !after_timer.failed_) {
        acceptor.poll_receive();
        if (acceptor_io.stopped()) {
            acceptor_io.restart();
        }
        acceptor_io.run_for(2ms);
        after_timer = observe_timer();
    }
    // RX can process the first due native deadline before before_timer.
    RUVIA_CHECK(after_timer.expirations_ > before_timer.expirations_);
    RUVIA_CHECK(after_timer.pump_.full_expirations_ >= 2);
    RUVIA_CHECK_EQ(after_timer.pump_.full_expirations_with_deadline_ + 1,
        after_timer.pump_.full_expirations_);
    RUVIA_CHECK(!after_timer.pump_.repeated_full_expiry_);
    RUVIA_CHECK(after_timer.pump_.full_idle_retired_);
    RUVIA_CHECK(!after_timer.deadline_);
    RUVIA_CHECK(after_timer.full_);
    RUVIA_CHECK(!after_timer.failed_);
    RUVIA_CHECK_EQ(after_timer.pump_.packet_writes_, before_input.packet_writes_);
    RUVIA_CHECK_EQ(after_timer.pump_.packets_sent_, before_input.packets_sent_);
    if (held_output) {
        RUVIA_CHECK(std::ranges::equal(held_output->view().bytes_,
            std::span<const std::byte>(held_bytes).first(held_output->size_)));
    }

    packets.acceptor_close();
    const auto stopping = worker.invoke([&] {
        owner->request_stop();
        owner->poll_stop();
        return owner->stop_status();
    });
    RUVIA_CHECK(stopping.stopping_);
    RUVIA_CHECK(stopping.outbound_pending_);
    RUVIA_CHECK(!stopping.transport_destroyed_);
    RUVIA_CHECK(!stopping.complete());
    RUVIA_CHECK(!packets.acceptor_finalize());
    // This models the Acceptor UDP completion, not a QUIC ACK: the client has
    // never received this output and sends no acknowledgement during the test.
    held_output.reset();
    packets.acceptor_poll();
    acceptor.request_stop();
    http3_quic_wire_owner::stop_status_type done;
    const auto stop_deadline = std::chrono::steady_clock::now() + 2s;
    do {
        packets.acceptor_poll();
        if (acceptor_io.stopped()) {
            acceptor_io.restart();
        }
        acceptor_io.run_for(2ms);
        done = worker.invoke([&] {
            owner->poll_datagrams();
            owner->poll_stop();
            return owner->stop_status();
        });
    } while ((!done.complete() || acceptor.status() == http3_worker_datagram_endpoint::stop_status::pending) &&
             std::chrono::steady_clock::now() < stop_deadline);
    RUVIA_CHECK(done.complete());
    RUVIA_CHECK(!done.outbound_pending_);
    RUVIA_CHECK(!done.failed_);
    RUVIA_CHECK(acceptor.status() == http3_worker_datagram_endpoint::stop_status::done);
    worker.invoke([&] {
        owner.reset();
        packets.worker_close();
    });
    RUVIA_CHECK(packets.acceptor_finalize());
    RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
}

RUVIA_TEST(http3_network_quic_wire_owner_async_wait_submission_failure_drains_handlers) {
    wire_worker runner;
    runner.invoke([&] {
        auto& worker_context = runner.runtime();
        exercise_owner_allocation_failure(ruvia_ctx, 1, worker_context);
    });
}

RUVIA_TEST(http3_network_quic_wire_owner_allocation_failure_drains_handlers) {
    wire_worker runner;
    runner.invoke([&] {
        auto& worker_context = runner.runtime();
        exercise_owner_allocation_failure(ruvia_ctx, 4, worker_context);
    });
}

RUVIA_TEST(http3_network_quic_wire_owner_fatal_failure_waits_for_network_retirement_gate) {
    wire_worker runner;
    runner.invoke([&] {
        auto& worker_context = runner.runtime();
        test_identity_files files;
        ruvia::detail::http_server_listener_definition::tls_type tls_config;
        tls_config.identity_.certificate_chain_file_ = files.certificate_.string();
        tls_config.identity_.private_key_file_ = files.private_key_.string();
        http3_quic_tls_context tls(tls_config, std::pmr::get_default_resource());
        asio::io_context io;
        native_wire_fixture fixture_value(worker_context, io, udp_type::endpoint(asio::ip::address_v4::loopback(), 0), tls,
            {}, nullptr, {&tls, &fail_protocol_pump});
        auto& owner_value = fixture_value.owner();
        owner_value.prepare();
        owner_value.defer_transport_retirement();
        try {
            owner_value.start();
        } catch (...) {
        }
        RUVIA_CHECK(owner_value.failure() != nullptr);
        const auto deadline_value = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline_value) {
            if (io.stopped()) {
                io.restart();
            }
            io.run_for(2ms);
            owner_value.poll_stop();
            const auto status = owner_value.stop_status();
            if (status.endpoint_retired_ && status.timer_handlers_retired_) {
                RUVIA_CHECK(!status.transport_destroyed_);
                RUVIA_CHECK(owner_value.transport() != nullptr);
                break;
            }
        }
        RUVIA_CHECK(!owner_value.stop_status().complete());
        owner_value.release_transport_retirement();
        drain_owner(io, fixture_value);
        RUVIA_CHECK(owner_value.stop_status().complete());
        RUVIA_CHECK(owner_value.stop_status().failed_);
    });
}

RUVIA_TEST(http3_network_quic_wire_owner_stop_waits_for_borrowed_datagram_send_and_handlers) {
    wire_worker runner;
    runner.invoke([&] {
        auto& worker_context = runner.runtime();
        test_identity_files files;
        ruvia::detail::http_server_listener_definition::tls_type tls_config;
        tls_config.identity_.certificate_chain_file_ = files.certificate_.string();
        tls_config.identity_.private_key_file_ = files.private_key_.string();
        http3_quic_tls_context tls(tls_config, std::pmr::get_default_resource());
        http3_quic_client_tls_context client_tls({
            .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification,
        });
        asio::io_context io;
        connection_pump_state pump;
        native_wire_fixture fixture_value(worker_context, io, udp_type::endpoint(asio::ip::address_v4::loopback(), 0), tls,
            {}, nullptr, {&pump, &pump_connection});
        auto& owner_value = fixture_value.owner();
        owner_value.prepare();
        owner_value.start();
        udp_type::socket peer(io, udp_type::endpoint(asio::ip::address_v4::loopback(), 0));
        peer.non_blocking(true);
        const udp_type::endpoint server(asio::ip::address_v4::loopback(), owner_value.bound_port());
        http3_quic_client_transport client(client_tls, client_config(peer, server), "localhost",
            std::chrono::steady_clock::now());
        RUVIA_CHECK(pump_peer(client, peer) != 0);
        bool stopped_during_send{};
        for (std::size_t turn = 0; turn < 64 && !stopped_during_send; ++turn) {
            if (io.stopped()) {
                io.restart();
            }
            if (io.run_one() == 0) {
                break;
            }
            stopped_during_send = owner_value.stop_status().outbound_pending_;
        }
        RUVIA_CHECK(stopped_during_send);
        if (stopped_during_send) {
            owner_value.request_stop();
            const auto pending = owner_value.stop_status();
            RUVIA_CHECK(pending.stopping_);
            RUVIA_CHECK(pending.outbound_pending_);
            RUVIA_CHECK(!pending.transport_destroyed_);
            owner_value.request_drive();
            RUVIA_CHECK(owner_value.stop_status().stopping_);
        }
        drain_owner(io, fixture_value);
        const auto done = owner_value.stop_status();
        RUVIA_CHECK(done.complete());
        RUVIA_CHECK(done.endpoint_retired_);
        RUVIA_CHECK(done.timer_handlers_retired_);
        RUVIA_CHECK(done.transport_destroyed_);
        RUVIA_CHECK(!done.outbound_pending_);
        RUVIA_CHECK(!done.failed_);
    });
}

RUVIA_TEST(http3_network_quic_wire_owner_construction_failure_drains_before_allocator_retires) {
    wire_worker runner;
    runner.invoke([&] {
        auto& worker_context = runner.runtime();
        test_identity_files files;
        ruvia::detail::http_server_listener_definition::tls_type tls_config;
        tls_config.identity_.certificate_chain_file_ = files.certificate_.string();
        tls_config.identity_.private_key_file_ = files.private_key_.string();
        http3_quic_tls_context tls(tls_config, std::pmr::get_default_resource());
        asio::io_context io;
        ruvia::quic_server_config invalid_config;
        invalid_config.local_transport_parameters_.max_udp_payload_size_ = 1199;
        connection_pump_state pump;
        native_wire_fixture fixture_value(worker_context, io, udp_type::endpoint(asio::ip::address_v4::loopback(), 0), tls,
            invalid_config, nullptr, {&pump, &pump_connection});
        auto& owner_value = fixture_value.owner();
        RUVIA_CHECK(ruvia::testing::throws_on([&] { owner_value.prepare(); }));
        drain_owner(io, fixture_value);
        RUVIA_CHECK(owner_value.stop_status().complete());
        RUVIA_CHECK(owner_value.stop_status().failed_);
        RUVIA_CHECK(owner_value.failure() != nullptr);
        RUVIA_CHECK(ruvia::testing::throws_on([&] { owner_value.rethrow_failure(); }));
    });
}

RUVIA_TEST(http3_network_quic_wire_owner_prepared_listener_stops_without_serving) {
    wire_worker runner;
    runner.invoke([&] {
        auto& worker_context = runner.runtime();
        test_identity_files files;
        ruvia::detail::http_server_listener_definition::tls_type tls_config;
        tls_config.identity_.certificate_chain_file_ = files.certificate_.string();
        tls_config.identity_.private_key_file_ = files.private_key_.string();
        http3_quic_tls_context tls(tls_config, std::pmr::get_default_resource());
        asio::io_context io;
        connection_pump_state pump;
        native_wire_fixture fixture_value(worker_context, io, udp_type::endpoint(asio::ip::address_v4::loopback(), 0), tls,
            {}, nullptr, {&pump, &pump_connection});
        auto& owner_value = fixture_value.owner();
        RUVIA_CHECK(ruvia::testing::throws_on([&] { owner_value.start(); }));
        RUVIA_CHECK(!owner_value.failure());
        owner_value.prepare();
        const auto port = owner_value.bound_port();
        RUVIA_CHECK(port != 0);
        RUVIA_CHECK(ruvia::testing::throws_on([&] { owner_value.prepare(); }));
        owner_value.request_stop();
        drain_owner(io, fixture_value);
        RUVIA_CHECK(owner_value.stop_status().complete());
        RUVIA_CHECK(!owner_value.failure());
        RUVIA_CHECK(ruvia::testing::throws_on([&] { owner_value.start(); }));
        udp_type::socket rebound(io, udp_type::endpoint(asio::ip::address_v4::loopback(), port));
        RUVIA_CHECK(rebound.local_endpoint().port() == port);
    });
}
