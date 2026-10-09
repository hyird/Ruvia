#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/error.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_local_critical_streams.h"
#include "ruvia/http/http3_var_int.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_client_types.h"

#include "http3/http3_quic_datagram_bridge.h"
#include "http3/http3_quic_server_transport.h"
#include "http3/http3_quic_socket_address.h"
#include "http3/http3_quic_tls_context.h"
#include "test_harness.h"
#include "test_io_context.h"
#include "test_tls_crypto.h"

namespace {
using namespace std::chrono_literals;
using udp_type = asio::ip::udp;

class quic_udp_blackhole final {
public:
    explicit quic_udp_blackhole(asio::io_context& io)
        : socket_(io, udp_type::endpoint(asio::ip::address_v4::loopback(), 0)),
          endpoint_(socket_.local_endpoint()) {
        socket_.non_blocking(true);
    }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return endpoint_.port();
    }

    [[nodiscard]] std::size_t datagrams() const noexcept {
        return datagrams_;
    }

    [[nodiscard]] std::size_t quic_long_headers() const noexcept {
        return quic_long_headers_;
    }

    [[nodiscard]] std::span<const udp_type::endpoint> sources() const noexcept {
        return sources_;
    }

    void receive_available() {
        std::array<std::byte, 65536> packet{};
        for (;;) {
            udp_type::endpoint source;
            asio::error_code error;
            const auto size = socket_.receive_from(asio::buffer(packet), source, 0, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                return;
            }
            if (error) {
                throw std::system_error(error, "receive HTTP/3 client UDP packet");
            }
            ++datagrams_;
            if (size >= 7 && (std::to_integer<unsigned char>(packet.front()) & 0x80U) != 0) {
                ++quic_long_headers_;
            }
            if (std::find(sources_.begin(), sources_.end(), source) == sources_.end()) {
                sources_.push_back(source);
            }
        }
    }

private:
    udp_type::socket socket_;
    udp_type::endpoint endpoint_;
    std::vector<udp_type::endpoint> sources_;
    std::size_t datagrams_{};
    std::size_t quic_long_headers_{};
};

struct send_result final {
    bool finished_{};
    bool returned_response_{};
    std::optional<ruvia::http_client_error::code_type> error_{};
};

ruvia::task<void> send_and_capture(ruvia::http_client_handle handle,
    const ruvia::http_client_request_view& request, send_result& result_value) {
    try {
        auto response = co_await handle.send(request);
        result_value.returned_response_ = true;
        static_cast<void>(response);
    } catch (const ruvia::http_client_error& error) {
        result_value.error_ = error.code();
    }
    result_value.finished_ = true;
}

ruvia::task<bool> wait_for_datagrams(const ruvia::worker_handle& worker_value,
    quic_udp_blackhole& peer, std::size_t target, std::chrono::milliseconds timeout) {
    const auto deadline_value = std::chrono::steady_clock::now() + timeout;
    while (peer.datagrams() < target) {
        peer.receive_available();
        if (peer.datagrams() >= target) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline_value) {
            co_return false;
        }
        if (co_await ruvia::sleep_for(worker_value, 1ms) != ruvia::timer_sleep_result::elapsed) {
            co_return false;
        }
    }
    peer.receive_available();
    co_return peer.datagrams() >= target;
}

ruvia::task<bool> wait_for_completion(const ruvia::worker_handle& worker_value,
    quic_udp_blackhole& peer, const send_result& result_value, std::chrono::milliseconds timeout) {
    const auto deadline_value = std::chrono::steady_clock::now() + timeout;
    while (!result_value.finished_) {
        peer.receive_available();
        if (std::chrono::steady_clock::now() >= deadline_value) {
            co_return false;
        }
        if (co_await ruvia::sleep_for(worker_value, 1ms) != ruvia::timer_sleep_result::elapsed) {
            co_return false;
        }
    }
    peer.receive_available();
    co_return true;
}

ruvia::task<void> exercise_cross_thread_constructed_http3_client(
    ruvia::event_loop_attachment& attachment, quic_udp_blackhole& peer,
    ruvia::http_client& client, send_result& result_value) {
    try {
        const auto worker_value = attachment.loop().handle();
        ruvia::stop_source stop;
        const ruvia::http_client_request_view request{
            .method_ = "GET", .target_ = "/cross-thread-owner"};
        {
            ruvia::task_scope task(worker_value);
            task.spawn(send_and_capture(client.with_options(
                                            {.timeout_ = 5s, .stop_token_ = stop.token()}),
                request, result_value));
            const bool dispatched = co_await wait_for_datagrams(worker_value, peer, 1, 2s);
            if (dispatched) {
                stop.request_stop();
                (void)co_await wait_for_completion(worker_value, peer, result_value, 2s);
            }
            task.request_stop();
            co_await task.join();
        }
        co_await client.shutdown();
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::task<void> exercise_public_http3_dispatch(
    ruvia::event_loop_attachment& attachment, quic_udp_blackhole& peer,
    ruvia::testing::test_context& ruvia_ctx) {
    try {
        const auto worker_value = attachment.loop().handle();
        ruvia::http_client client(attachment.loop(), ruvia::http_client_config{
                                                         .scheme_ = ruvia::http_scheme::https,
                                                         .host_ = "127.0.0.1",
                                                         .port_ = peer.port(),
                                                         .connection_count_ = 2,
                                                         .connect_timeout_ = 2s,
                                                         .request_timeout_ = 5s,
                                                         .acquire_timeout_ = 1s,
                                                         .max_response_bytes_ = 4096,
                                                         .protocol_ = ruvia::http_client_protocol::http3_only,
                                                         .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification,
                                                     });

        ruvia::stop_source first_stop;
        ruvia::stop_source second_stop;
        const auto first_handle = client.with_options(
            {.timeout_ = 5s, .stop_token_ = first_stop.token()});
        const auto second_handle = client.with_options(
            {.timeout_ = 5s, .stop_token_ = second_stop.token()});
        const ruvia::http_client_request_view first_request{.method_ = "GET", .target_ = "/slot-one"};
        const ruvia::http_client_request_view second_request{.method_ = "GET", .target_ = "/slot-two"};
        send_result first_result;
        send_result second_result;
        {
            ruvia::task_scope requests(worker_value);
            requests.spawn(send_and_capture(first_handle, first_request, first_result));
            requests.spawn(send_and_capture(second_handle, second_request, second_result));
            const auto source_deadline = std::chrono::steady_clock::now() + 2s;
            while (peer.sources().size() < 2 &&
                   std::chrono::steady_clock::now() < source_deadline) {
                peer.receive_available();
                if (peer.sources().size() < 2) {
                    (void)co_await ruvia::sleep_for(worker_value, 1ms);
                }
            }
            peer.receive_available();
            RUVIA_CHECK(peer.sources().size() >= 2);
            RUVIA_CHECK(peer.quic_long_headers() >= 2);

            first_stop.request_stop();
            second_stop.request_stop();
            const bool first_finished = co_await wait_for_completion(
                worker_value, peer, first_result, 2s);
            const bool second_finished = co_await wait_for_completion(
                worker_value, peer, second_result, 2s);
            RUVIA_CHECK(first_finished && second_finished);
            co_await requests.join();
        }
        RUVIA_CHECK(first_result.finished_ && second_result.finished_);
        RUVIA_CHECK(first_result.error_ == ruvia::http_client_error::code_type::cancelled);
        RUVIA_CHECK(second_result.error_ == ruvia::http_client_error::code_type::cancelled);
        RUVIA_CHECK_EQ(client.stats().in_flight_requests_, std::size_t{0});

        peer.receive_available();
        const auto deadline_datagram_count = peer.datagrams() + 1;
        send_result deadline_result;
        ruvia::stop_source deadline_safety_stop;
        const ruvia::http_client_request_view deadline_request{
            .method_ = "GET", .target_ = "/deadline"};
        {
            ruvia::task_scope request(worker_value);
            request.spawn(send_and_capture(client.with_options(
                                               {.timeout_ = 150ms, .stop_token_ = deadline_safety_stop.token()}),
                deadline_request, deadline_result));
            const bool dispatched = co_await wait_for_datagrams(
                worker_value, peer, deadline_datagram_count, 1s);
            RUVIA_CHECK(dispatched);
            const bool finished = co_await wait_for_completion(
                worker_value, peer, deadline_result, 2s);
            RUVIA_CHECK(finished);
            if (!deadline_result.finished_) {
                deadline_safety_stop.request_stop();
                (void)co_await wait_for_completion(
                    worker_value, peer, deadline_result, 1s);
            }
            co_await request.join();
        }
        RUVIA_CHECK(deadline_result.error_ == ruvia::http_client_error::code_type::timeout);
        RUVIA_CHECK_EQ(client.stats().in_flight_requests_, std::size_t{0});

        peer.receive_available();
        const auto rotated_datagram_count = peer.datagrams() + 1;
        ruvia::stop_source rotation_stop;
        send_result rotation_result;
        const ruvia::http_client_request_view rotation_request{
            .method_ = "GET", .target_ = "/rotated-connection"};
        {
            ruvia::task_scope request(worker_value);
            request.spawn(send_and_capture(client.with_options(
                                               {.timeout_ = 5s, .stop_token_ = rotation_stop.token()}),
                rotation_request, rotation_result));
            const bool rotated = co_await wait_for_datagrams(
                worker_value, peer, rotated_datagram_count, 2s);
            RUVIA_CHECK(rotated);
            rotation_stop.request_stop();
            const bool finished = co_await wait_for_completion(
                worker_value, peer, rotation_result, 2s);
            RUVIA_CHECK(finished);
            co_await request.join();
        }
        RUVIA_CHECK(rotation_result.error_ == ruvia::http_client_error::code_type::cancelled);
        RUVIA_CHECK_EQ(client.stats().in_flight_requests_, std::size_t{0});

        co_await client.shutdown();
        peer.receive_available();
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

using test_quic_address_type = ruvia::quic_address;

[[nodiscard]] test_quic_address_type test_quic_address(const udp_type::endpoint& endpoint) {
    if (!endpoint.address().is_v4()) {
        throw std::runtime_error("HTTP/3 GOAWAY peer requires IPv4");
    }
    test_quic_address_type address;
    const auto bytes_value = endpoint.address().to_v4().to_bytes();
    std::transform(bytes_value.begin(), bytes_value.end(), address.bytes_.begin(),
        [](unsigned char value) { return std::byte{value}; });
    address.port_ = endpoint.port();
    return address;
}

[[nodiscard]] udp_type::endpoint test_udp_endpoint(const test_quic_address_type& address) {
    if (address.family_ != ruvia::quic_address_family::ipv4) {
        throw std::runtime_error("HTTP/3 GOAWAY peer received a non-IPv4 destination");
    }
    asio::ip::address_v4::bytes_type bytes_value{};
    std::transform(address.bytes_.begin(), address.bytes_.begin() + bytes_value.size(), bytes_value.begin(),
        [](std::byte value) { return std::to_integer<unsigned char>(value); });
    return {asio::ip::address_v4(bytes_value), address.port_};
}

[[nodiscard]] std::vector<char> test_http3_frame(
    std::uint64_t type, std::span<const char> payload_value) {
    std::array<char, 16> header_value{};
    const auto type_size = ruvia::encode_http3_var_int(header_value, type);
    if ((type_size.index() != 0)) {
        throw std::runtime_error("failed to encode HTTP/3 GOAWAY test frame type");
    }
    const auto payload_size = ruvia::encode_http3_var_int(
        std::span<char>(header_value).subspan(std::get<0>(type_size)), payload_value.size());
    if ((payload_size.index() != 0)) {
        throw std::runtime_error("failed to encode HTTP/3 GOAWAY test frame length");
    }
    std::vector<char> output(header_value.begin(), header_value.begin() + std::get<0>(type_size) + std::get<0>(payload_size));
    output.insert(output.end(), payload_value.begin(), payload_value.end());
    return output;
}

[[nodiscard]] ruvia::http3_local_critical_streams make_test_critical_streams() {
    auto streams = ruvia::http3_local_critical_streams::create();
    if ((streams.index() != 0)) {
        throw std::runtime_error("failed to create HTTP/3 GOAWAY test stream prefixes");
    }
    return std::move(std::get<0>(streams));
}

class test_identity_files final {
public:
    test_identity_files() {
        std::random_device random;
        directory_ = std::filesystem::temp_directory_path() /
                     ("ruvia-h3-goaway-" + std::to_string(random()) + "-" +
                         std::to_string(random()));
        if (!std::filesystem::create_directory(directory_)) {
            throw std::runtime_error("failed to create HTTP/3 GOAWAY test identity directory");
        }
        try {
            EVP_PKEY_CTX* raw_context = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
            if (raw_context == nullptr) {
                throw std::runtime_error("failed to create HTTP/3 GOAWAY test key generator");
            }
            std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
                raw_context, EVP_PKEY_CTX_free);
            EVP_PKEY* raw_key = nullptr;
            if (EVP_PKEY_keygen_init(context.get()) <= 0 ||
                EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0 ||
                EVP_PKEY_generate(context.get(), &raw_key) <= 0) {
                throw std::runtime_error("failed to generate HTTP/3 GOAWAY test key");
            }
            std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
            std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new_ex(nullptr, nullptr), X509_free);
            if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
                ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
                X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0) == nullptr ||
                X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 86400) == nullptr ||
                X509_set_pubkey(certificate.get(), key.get()) != 1 ||
                X509_set_issuer_name(certificate.get(), X509_get_subject_name(certificate.get())) != 1 ||
                ruvia::test::sign_tls_certificate(certificate.get(), key.get()) <= 0) {
                throw std::runtime_error("failed to create HTTP/3 GOAWAY test certificate");
            }
            certificate_file_ = directory_ / "cert.pem";
            private_key_file_ = directory_ / "key.pem";
            std::unique_ptr<BIO, decltype(&BIO_free)> certificate_bio(
                BIO_new_file(certificate_file_.string().c_str(), "w"), BIO_free);
            std::unique_ptr<BIO, decltype(&BIO_free)> private_key_bio(
                BIO_new_file(private_key_file_.string().c_str(), "w"), BIO_free);
            if (!certificate_bio || !private_key_bio ||
                PEM_write_bio_X509(certificate_bio.get(), certificate.get()) != 1 ||
                ruvia::test::write_tls_private_key(private_key_bio.get(), key.get()) != 1) {
                throw std::runtime_error("failed to write HTTP/3 GOAWAY test identity");
            }
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove_all(directory_, ignored);
            throw;
        }
    }

    ~test_identity_files() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& certificate() const noexcept {
        return certificate_file_;
    }
    [[nodiscard]] const std::filesystem::path& private_key() const noexcept {
        return private_key_file_;
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path certificate_file_;
    std::filesystem::path private_key_file_;
};

class go_away_rotation_peer final {
    struct request_type final {
        std::uint64_t stream_{};
        std::size_t response_offset_{};
        bool request_finished_{};
        bool response_finished_{};
        bool rejection_reset_sent_{};
    };
    struct connection_type final {
        ruvia::quic_connection_token id_{};
        std::array<std::optional<std::uint64_t>, 3> critical_ids_{};
        std::array<std::size_t, 3> critical_offsets_{};
        std::vector<request_type> requests_;
        std::size_t go_away_offset_{};
    };

public:
    static constexpr std::uint64_t unobserved_stream =
        std::numeric_limits<std::uint64_t>::max();

    explicit go_away_rotation_peer(const test_identity_files& identity,
        bool reject_first_request_as_unprocessed = false)
        : socket_(io_, udp_type::endpoint(asio::ip::address_v4::loopback(), 0)),
          endpoint_(socket_.local_endpoint()),
          identity_(identity),
          reject_first_request_as_unprocessed_(reject_first_request_as_unprocessed),
          prefixes_(make_test_critical_streams()) {
        socket_.non_blocking(true);
        for (std::size_t index = 0; index < critical_bytes_.size(); ++index) {
            const std::array<std::span<const char>, 3> spans{
                prefixes_.control_prefix(), prefixes_.qpack_encoder_prefix(),
                prefixes_.qpack_decoder_prefix()};
            critical_bytes_[index].assign(spans[index].begin(), spans[index].end());
        }
        std::pmr::monotonic_buffer_resource temporary;
        const ruvia::http3_field_section_field_view fields_value[]{{":status", "200"},
            {"content-length", "2"}};
        const auto encoded_head = ruvia::encode_http3_field_section(fields_value, &temporary);
        if ((encoded_head.index() != 0)) {
            throw std::runtime_error("failed to encode HTTP/3 GOAWAY test response");
        }
        response_bytes_ = test_http3_frame(1, std::get<0>(encoded_head));
        const auto data = test_http3_frame(0, std::span<const char>("ok", 2));
        response_bytes_.insert(response_bytes_.end(), data.begin(), data.end());
        std::array<char, ruvia::http3_var_int_max_bytes> go_away_id{};
        const auto go_away_id_size = ruvia::encode_http3_var_int(
            go_away_id, reject_first_request_as_unprocessed_ ? 0 : 4);
        if ((go_away_id_size.index() != 0)) {
            throw std::runtime_error("failed to encode HTTP/3 GOAWAY test identifier");
        }
        go_away_bytes_ = test_http3_frame(7,
            std::span<const char>(go_away_id.data(), std::get<0>(go_away_id_size)));
        thread_ = std::thread([this] { run(); });
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, 5s,
                [this] { return started_ || failure_ != nullptr; })) {
            lock.unlock();
            stop_.store(true, std::memory_order_release);
            thread_.join();
            throw std::runtime_error("HTTP/3 GOAWAY peer startup watchdog expired");
        }
        const auto failure = failure_;
        lock.unlock();
        if (failure != nullptr) {
            thread_.join();
            std::rethrow_exception(failure);
        }
    }

    ~go_away_rotation_peer() {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    go_away_rotation_peer(const go_away_rotation_peer&) = delete;
    go_away_rotation_peer& operator=(const go_away_rotation_peer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return endpoint_.port();
    }
    [[nodiscard]] std::size_t accepted_connections() const noexcept {
        return accepted_connections_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t first_request_stream() const noexcept {
        return first_request_stream_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t second_request_stream() const noexcept {
        return second_request_stream_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool first_connection_saw_stream4() const noexcept {
        return first_connection_saw_stream4_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool go_away_sent() const noexcept {
        return go_away_sent_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool rejection_reset_sent() const noexcept {
        return rejection_reset_sent_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool first_connection_application_response_started() const noexcept {
        return first_connection_application_response_started_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool synchronize() {
        std::unique_lock lock(mutex_);
        const auto generation = ++synchronize_requested_;
        if (!condition_.wait_for(lock, 5s, [this, generation] {
                return synchronize_completed_ >= generation || failure_ != nullptr;
            })) {
            return false;
        }
        const auto failure = failure_;
        lock.unlock();
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        return true;
    }

    void rethrow_if_failed() const {
        std::exception_ptr failure;
        {
            std::lock_guard lock(mutex_);
            failure = failure_;
        }
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
    }

private:
    void run() noexcept {
        try {
            std::pmr::unsynchronized_pool_resource resource;
            ruvia::detail::http_server_listener_definition::tls_type tls;
            tls.identity_.certificate_chain_file_ = identity_.certificate().string();
            tls.identity_.private_key_file_ = identity_.private_key().string();
            ruvia::detail::http3_quic_tls_context tls_context(tls, &resource);
            ruvia::detail::http3_quic_server_transport server(tls_context, {}, &resource);
            {
                std::lock_guard lock(mutex_);
                started_ = true;
            }
            condition_.notify_all();

            std::vector<connection_type> connections;
            std::array<std::byte, 65536> packet{};
            std::array<char, 4096> request_bytes{};
            bool retired_rejected_connection = false;
            const auto watchdog_value = std::chrono::steady_clock::now() + 30s;
            while (!stop_.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() >= watchdog_value) {
                    throw std::runtime_error("HTTP/3 GOAWAY peer watchdog expired");
                }
                for (unsigned count = 0; count != 32; ++count) {
                    udp_type::endpoint source;
                    asio::error_code error;
                    const auto size = socket_.receive_from(asio::buffer(packet), source, 0, error);
                    if (error == asio::error::would_block || error == asio::error::try_again) {
                        break;
                    }
                    if (error) {
                        // The rejected connection can close while packets to its
                        // old UDP port are still in flight. Keep serving its replacement.
                        if (error == asio::error::connection_reset) {
                            continue;
                        }
                        throw std::system_error(error, "receive HTTP/3 GOAWAY test datagram");
                    }
                    if (size == 0) {
                        continue;
                    }
                    const auto bytes_value = std::span<const std::byte>(packet.data(), size);
                    const auto local = ruvia::detail::from_quic_address(test_quic_address(endpoint_));
                    const auto remote = ruvia::detail::from_quic_address(test_quic_address(source));
                    const auto routed = server.route_datagram(bytes_value, local, remote);
                    const auto now = std::chrono::steady_clock::now();
                    if (routed.kind_ == ruvia::quic_server_route_kind::initial_offer) {
                        const auto admitted = server.admit_initial(routed.offer_, now);
                        if (admitted.status_ == ruvia::quic_operation_status::accepted) {
                            connections.push_back(connection_type{.id_ = admitted.connection_});
                            accepted_connections_.store(connections.size(), std::memory_order_release);
                        }
                    } else if (routed.kind_ == ruvia::quic_server_route_kind::existing_connection) {
                        (void)server.server().receive(routed.connection_,
                            {bytes_value, test_quic_address(endpoint_), test_quic_address(source)}, now);
                    }
                }
                (void)server.server().handle_expiry(std::chrono::steady_clock::now());

                for (std::size_t connection_index = 0;
                    connection_index < connections.size(); ++connection_index) {
                    auto& connection = connections[connection_index];
                    if (connection_index == 0 && retired_rejected_connection) {
                        continue;
                    }
                    auto& transport = server.server().connection(connection.id_);
                    const auto info = transport.info();
                    if (!info.quic_handshake_complete_ || info.state_ != ruvia::quic_connection_state::ready) {
                        continue;
                    }
                    for (std::size_t index = 0; index < connection.critical_ids_.size(); ++index) {
                        if (!connection.critical_ids_[index]) {
                            const auto opened = transport.open_stream(true);
                            if (opened.status_ != ruvia::quic_operation_status::accepted) {
                                throw std::runtime_error("HTTP/3 GOAWAY peer critical stream open failed");
                            }
                            connection.critical_ids_[index] = opened.stream_id_;
                        }
                        auto& offset = connection.critical_offsets_[index];
                        if (offset < critical_bytes_[index].size()) {
                            const auto written = transport.write_stream(*connection.critical_ids_[index],
                                std::as_bytes(std::span<const char>(critical_bytes_[index]).subspan(offset)));
                            if (written.status_ == ruvia::quic_operation_status::accepted) {
                                offset += written.accepted_;
                            } else if (written.status_ != ruvia::quic_operation_status::would_block) {
                                throw std::runtime_error("HTTP/3 GOAWAY peer critical stream write failed");
                            }
                        }
                    }

                    const auto newly_accepted = transport.accept_streams();
                    if (newly_accepted.status_ != ruvia::quic_operation_status::accepted &&
                        newly_accepted.status_ != ruvia::quic_operation_status::need_input) {
                        throw std::runtime_error("HTTP/3 GOAWAY peer stream acceptance failed");
                    }
                    for (std::size_t index = 0; index < newly_accepted.size_; ++index) {
                        const auto& stream = newly_accepted.streams_[index];
                        if (!stream.readable_ || !stream.writable_) {
                            continue;
                        }
                        connection.requests_.push_back(request_type{.stream_ = stream.stream_id_});
                        if (connection_index == 0) {
                            std::uint64_t unobserved = unobserved_stream;
                            static_cast<void>(first_request_stream_.compare_exchange_strong(
                                unobserved, stream.stream_id_, std::memory_order_release,
                                std::memory_order_relaxed));
                            if (stream.stream_id_ == 4) {
                                first_connection_saw_stream4_.store(true, std::memory_order_release);
                            }
                        } else if (connection_index == 1) {
                            std::uint64_t unobserved = unobserved_stream;
                            static_cast<void>(second_request_stream_.compare_exchange_strong(
                                unobserved, stream.stream_id_, std::memory_order_release,
                                std::memory_order_relaxed));
                        }
                    }

                    for (auto& request : connection.requests_) {
                        if (!request.request_finished_) {
                            for (;;) {
                                const auto read = transport.read_stream(request.stream_, std::as_writable_bytes(std::span(request_bytes)));
                                if (read.status_ == ruvia::quic_stream_read_status::data) {
                                    continue;
                                }
                                if (read.status_ == ruvia::quic_stream_read_status::fin) {
                                    request.request_finished_ = true;
                                } else if (read.status_ != ruvia::quic_stream_read_status::would_block) {
                                    throw std::runtime_error("HTTP/3 GOAWAY peer request read failed");
                                }
                                break;
                            }
                        }
                        if (reject_first_request_as_unprocessed_ && connection_index == 0) {
                            if (request.stream_ != 0) {
                                throw std::runtime_error(
                                    "HTTP/3 GOAWAY replay peer expected request stream 0");
                            }
                            if (request.rejection_reset_sent_) {
                                continue;
                            }
                            if (!go_away_sent()) {
                                if (connection.critical_offsets_[0] != critical_bytes_[0].size()) {
                                    continue;
                                }
                                const auto written = transport.write_stream(*connection.critical_ids_[0],
                                    std::as_bytes(std::span<const char>(go_away_bytes_).subspan(connection.go_away_offset_)));
                                if (written.status_ == ruvia::quic_operation_status::accepted) {
                                    connection.go_away_offset_ += written.accepted_;
                                    if (connection.go_away_offset_ == go_away_bytes_.size()) {
                                        go_away_sent_.store(true, std::memory_order_release);
                                    }
                                } else if (written.status_ != ruvia::quic_operation_status::would_block) {
                                    throw std::runtime_error(
                                        "HTTP/3 GOAWAY replay control write failed");
                                }
                                if (!go_away_sent()) {
                                    continue;
                                }
                            }
                            const auto reset = transport.reset_stream(request.stream_,
                                static_cast<std::uint64_t>(
                                    ruvia::http3_connection_error_code::request_rejected));
                            if (reset != ruvia::quic_operation_status::accepted) {
                                throw std::runtime_error(
                                    "HTTP/3 GOAWAY replay request reset failed");
                            }
                            request.rejection_reset_sent_ = true;
                            rejection_reset_sent_.store(true, std::memory_order_release);
                            continue;
                        }
                        if (!request.request_finished_ || request.response_finished_) {
                            continue;
                        }
                        if (connection_index == 0 && !go_away_sent()) {
                            if (connection.critical_offsets_[0] != critical_bytes_[0].size()) {
                                continue;
                            }
                            const auto written = transport.write_stream(*connection.critical_ids_[0],
                                std::as_bytes(std::span<const char>(go_away_bytes_).subspan(connection.go_away_offset_)));
                            if (written.status_ == ruvia::quic_operation_status::accepted) {
                                connection.go_away_offset_ += written.accepted_;
                                if (connection.go_away_offset_ == go_away_bytes_.size()) {
                                    go_away_sent_.store(true, std::memory_order_release);
                                }
                            } else if (written.status_ != ruvia::quic_operation_status::would_block) {
                                throw std::runtime_error("HTTP/3 GOAWAY peer control write failed");
                            }
                            // Send the cutoff before releasing the first response.
                            // Completing that response must not race control delivery.
                            continue;
                        }
                        if (request.response_offset_ < response_bytes_.size()) {
                            const auto written = transport.write_stream(request.stream_,
                                std::as_bytes(std::span<const char>(response_bytes_).subspan(request.response_offset_)));
                            if (written.status_ == ruvia::quic_operation_status::accepted) {
                                request.response_offset_ += written.accepted_;
                                if (connection_index == 0 && written.accepted_ != 0) {
                                    first_connection_application_response_started_.store(
                                        true, std::memory_order_release);
                                }
                            } else if (written.status_ != ruvia::quic_operation_status::would_block) {
                                throw std::runtime_error("HTTP/3 GOAWAY peer response write failed");
                            }
                        }
                        if (request.response_offset_ != response_bytes_.size()) {
                            continue;
                        }
                        const auto finished = transport.finish_stream(request.stream_);
                        if (finished == ruvia::quic_operation_status::accepted) {
                            request.response_finished_ = true;
                        } else if (finished != ruvia::quic_operation_status::would_block) {
                            throw std::runtime_error("HTTP/3 GOAWAY peer response FIN failed");
                        }
                    }
                }

                for (std::size_t index = 0; index < connections.size(); ++index) {
                    if (index == 0 && retired_rejected_connection) {
                        continue;
                    }
                    auto& transport = server.server().connection(connections[index].id_);
                    for (unsigned count = 0; count != 64; ++count) {
                        const auto outbound = transport.write_packet(packet, std::chrono::steady_clock::now());
                        if (outbound.size_ == 0) {
                            break;
                        }
                        asio::error_code error;
                        const auto sent = socket_.send_to(
                            asio::buffer(packet.data(), outbound.size_), test_udp_endpoint(outbound.peer_), 0, error);
                        if (error || sent != outbound.size_) {
                            throw std::system_error(error ? error : std::make_error_code(std::errc::io_error),
                                "send HTTP/3 GOAWAY test datagram");
                        }
                    }
                }
                if (reject_first_request_as_unprocessed_ && rejection_reset_sent() &&
                    !retired_rejected_connection) {
                    // The reset datagrams are sent; release the rejected QUIC
                    // connection before admitting the replay on a new one.
                    server.retire(connections.front().id_);
                    retired_rejected_connection = true;
                }
                {
                    std::lock_guard lock(mutex_);
                    synchronize_completed_ = synchronize_requested_;
                }
                condition_.notify_all();
                std::this_thread::sleep_for(1ms);
            }
            for (const auto& connection : connections) {
                server.retire(connection.id_);
            }
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                failure_ = std::current_exception();
                started_ = true;
            }
            condition_.notify_all();
        }
    }

    asio::io_context io_;
    udp_type::socket socket_;
    udp_type::endpoint endpoint_;
    const test_identity_files& identity_;
    bool reject_first_request_as_unprocessed_{};
    ruvia::http3_local_critical_streams prefixes_;
    std::array<std::vector<char>, 3> critical_bytes_;
    std::vector<char> response_bytes_;
    std::vector<char> go_away_bytes_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::exception_ptr failure_;
    bool started_{};
    std::uint64_t synchronize_requested_{};
    std::uint64_t synchronize_completed_{};
    std::atomic<bool> stop_{};
    std::atomic<std::size_t> accepted_connections_{};
    std::atomic<std::uint64_t> first_request_stream_{unobserved_stream};
    std::atomic<std::uint64_t> second_request_stream_{unobserved_stream};
    std::atomic<bool> first_connection_saw_stream4_{};
    std::atomic<bool> go_away_sent_{};
    std::atomic<bool> rejection_reset_sent_{};
    std::atomic<bool> first_connection_application_response_started_{};
};

template <typename predicate_type>
ruvia::task<bool> wait_for_go_away_peer(const ruvia::worker_handle& worker_value,
    go_away_rotation_peer& peer, predicate_type predicate, std::chrono::milliseconds timeout) {
    const auto deadline_value = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        peer.rethrow_if_failed();
        if (std::chrono::steady_clock::now() >= deadline_value) {
            co_return false;
        }
        if (co_await ruvia::sleep_for(worker_value, 1ms) != ruvia::timer_sleep_result::elapsed) {
            co_return false;
        }
    }
    peer.rethrow_if_failed();
    co_return true;
}

class client_watchdog final {
    struct state_type final {
        ruvia::http_client* client_{};
        bool expired_{};
    };

public:
    client_watchdog(asio::io_context& io, ruvia::http_client& client)
        : timer_(io),
          state_(std::make_shared<state_type>(state_type{&client, false})) {
        timer_.expires_after(25s);
        timer_.async_wait([state = state_](const asio::error_code& error) noexcept {
            if (!error && state->client_ != nullptr) {
                state->expired_ = true;
                state->client_->close();
            }
        });
    }

    ~client_watchdog() {
        disarm();
    }

    void disarm() noexcept {
        state_->client_ = nullptr;
        asio::error_code ignored;
        timer_.cancel();
    }

    [[nodiscard]] bool expired() const noexcept {
        return state_->expired_;
    }

private:
    asio::steady_timer timer_;
    std::shared_ptr<state_type> state_;
};

ruvia::task<bool> wait_for_http_client_in_flight_zero(const ruvia::worker_handle& worker_value,
    ruvia::http_client& client, std::chrono::milliseconds timeout) {
    const auto deadline_value = std::chrono::steady_clock::now() + timeout;
    while (client.stats().in_flight_requests_ != 0) {
        if (std::chrono::steady_clock::now() >= deadline_value) {
            co_return false;
        }
        if (co_await ruvia::sleep_for(worker_value, 1ms) != ruvia::timer_sleep_result::elapsed) {
            co_return false;
        }
    }
    co_return true;
}

void check_go_away_rotation_response(ruvia::http_client_response& response,
    const ruvia::http_client_response_bytes& body, ruvia::testing::test_context& ruvia_ctx) {
    RUVIA_CHECK_EQ(response.status().value(), std::uint16_t{200});
    RUVIA_CHECK(response.protocol_version() == ruvia::http_protocol_version::http3);
    RUVIA_CHECK(response.body().complete());
    RUVIA_CHECK_EQ(body.size(), std::size_t{2});
    if (body.size() == 2) {
        const auto bytes_value = body.bytes();
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()),
            std::string_view("ok"));
    }
}

ruvia::task<void> exercise_public_http3_go_away_rotation(
    asio::io_context& io, ruvia::event_loop_attachment& attachment,
    go_away_rotation_peer& peer, ruvia::testing::test_context& ruvia_ctx) {
    std::exception_ptr failure;
    std::string_view stage = "client setup";
    bool shutdown_completed = false;
    bool in_flight_zero = false;
    try {
        ruvia::http_client client(attachment.loop(), ruvia::http_client_config{
                                                         .scheme_ = ruvia::http_scheme::https,
                                                         .host_ = "127.0.0.1",
                                                         .port_ = peer.port(),
                                                         .connection_count_ = 1,
                                                         .connect_timeout_ = 3s,
                                                         .request_timeout_ = 8s,
                                                         .acquire_timeout_ = 3s,
                                                         .max_response_bytes_ = 64,
                                                         .protocol_ = ruvia::http_client_protocol::http3_only,
                                                         .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification,
                                                     });
        client_watchdog watchdog(io, client);
        try {
            stage = "first response";
            const ruvia::http_client_request_view first_request{
                .method_ = "GET", .target_ = "/goaway-first"};
            {
                auto response = co_await client.send(first_request);
                auto body = co_await response.body().read_all(16);
                check_go_away_rotation_response(response, body, ruvia_ctx);
            }
            RUVIA_CHECK_EQ(peer.accepted_connections(), std::size_t{1});
            RUVIA_CHECK_EQ(peer.first_request_stream(), std::uint64_t{0});
            RUVIA_CHECK(peer.go_away_sent());

            stage = "second response";
            const ruvia::http_client_request_view second_request{
                .method_ = "GET", .target_ = "/goaway-second"};
            {
                auto response = co_await client.send(second_request);
                auto body = co_await response.body().read_all(16);
                check_go_away_rotation_response(response, body, ruvia_ctx);
            }
            stage = "peer synchronization";
            const auto worker_value = attachment.loop().handle();
            const bool second_connection_ready = co_await wait_for_go_away_peer(worker_value, peer, [&] { return peer.accepted_connections() >= 2 &&
                                                                                                                 peer.second_request_stream() != go_away_rotation_peer::unobserved_stream; }, 5s);
            RUVIA_CHECK(second_connection_ready);
            RUVIA_CHECK(peer.synchronize());
            RUVIA_CHECK(peer.accepted_connections() >= 2);
            RUVIA_CHECK_EQ(peer.second_request_stream(), std::uint64_t{0});
            RUVIA_CHECK(!peer.first_connection_saw_stream4());
            stage = "in-flight retirement";
            in_flight_zero = co_await wait_for_http_client_in_flight_zero(
                worker_value, client, 2s);
            RUVIA_CHECK(in_flight_zero);
        } catch (...) {
            failure = std::current_exception();
        }
        stage = "client shutdown";
        try {
            co_await client.shutdown();
            shutdown_completed = true;
        } catch (...) {
            if (failure == nullptr) {
                failure = std::current_exception();
            }
        }
        watchdog.disarm();
        RUVIA_CHECK(shutdown_completed);
        RUVIA_CHECK(!watchdog.expired());
        peer.rethrow_if_failed();
    } catch (...) {
        if (failure == nullptr) {
            failure = std::current_exception();
        }
    }
    attachment.stop();
    if (failure != nullptr) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "HTTP/3 GOAWAY rotation test failed during %.*s: %s (connections=%zu, first=%llu, second=%llu, goaway=%d, stream4=%d)\n",
                static_cast<int>(stage.size()), stage.data(), error.what(), peer.accepted_connections(),
                static_cast<unsigned long long>(peer.first_request_stream()),
                static_cast<unsigned long long>(peer.second_request_stream()),
                peer.go_away_sent(), peer.first_connection_saw_stream4());
        } catch (...) {
            std::fprintf(stderr, "HTTP/3 GOAWAY rotation test failed with a non-standard exception\n");
        }
        RUVIA_CHECK(false);
    }
}

ruvia::task<void> exercise_public_http3_go_away_replay(
    asio::io_context& io, ruvia::event_loop_attachment& attachment,
    go_away_rotation_peer& peer, ruvia::testing::test_context& ruvia_ctx) {
    std::exception_ptr failure;
    std::string_view stage = "client setup";
    bool shutdown_completed = false;
    bool in_flight_zero = false;
    std::size_t caller_successes = 0;
    try {
        ruvia::http_client client(attachment.loop(), ruvia::http_client_config{
                                                         .scheme_ = ruvia::http_scheme::https,
                                                         .host_ = "127.0.0.1",
                                                         .port_ = peer.port(),
                                                         .connection_count_ = 1,
                                                         .connect_timeout_ = 3s,
                                                         .request_timeout_ = 8s,
                                                         .acquire_timeout_ = 3s,
                                                         .max_response_bytes_ = 64,
                                                         .protocol_ = ruvia::http_client_protocol::http3_only,
                                                         .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification,
                                                     });
        client_watchdog watchdog(io, client);
        try {
            stage = "unprocessed request replay";
            const ruvia::http_client_request_view request{
                .method_ = "GET", .target_ = "/goaway-replay"};
            const auto request_started = std::chrono::steady_clock::now();
            {
                auto response = co_await client.send(request);
                ++caller_successes;
                auto body = co_await response.body().read_all(16);
                check_go_away_rotation_response(response, body, ruvia_ctx);
            }
            RUVIA_CHECK(std::chrono::steady_clock::now() - request_started < 8s);
            RUVIA_CHECK_EQ(caller_successes, std::size_t{1});

            stage = "peer replay synchronization";
            const auto worker_value = attachment.loop().handle();
            const bool replay_observed = co_await wait_for_go_away_peer(worker_value, peer, [&] { return peer.accepted_connections() >= 2 &&
                                                                                                         peer.second_request_stream() != go_away_rotation_peer::unobserved_stream &&
                                                                                                         peer.rejection_reset_sent(); }, 5s);
            RUVIA_CHECK(replay_observed);
            RUVIA_CHECK(peer.synchronize());
            RUVIA_CHECK_EQ(peer.first_request_stream(), std::uint64_t{0});
            RUVIA_CHECK_EQ(peer.second_request_stream(), std::uint64_t{0});
            RUVIA_CHECK(peer.accepted_connections() >= 2);
            RUVIA_CHECK(peer.go_away_sent());
            RUVIA_CHECK(peer.rejection_reset_sent());
            RUVIA_CHECK(!peer.first_connection_application_response_started());

            stage = "in-flight retirement";
            in_flight_zero = co_await wait_for_http_client_in_flight_zero(
                worker_value, client, 2s);
            RUVIA_CHECK(in_flight_zero);
            RUVIA_CHECK_EQ(client.stats().in_flight_requests_, std::size_t{0});
        } catch (...) {
            failure = std::current_exception();
        }
        stage = "client shutdown";
        try {
            co_await client.shutdown();
            shutdown_completed = true;
        } catch (...) {
            if (failure == nullptr) {
                failure = std::current_exception();
            }
        }
        watchdog.disarm();
        RUVIA_CHECK(shutdown_completed);
        RUVIA_CHECK(!watchdog.expired());
        peer.rethrow_if_failed();
    } catch (...) {
        if (failure == nullptr) {
            failure = std::current_exception();
        }
    }
    attachment.stop();
    if (failure != nullptr) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "HTTP/3 GOAWAY replay test failed during %.*s: %s (connections=%zu, first=%llu, second=%llu, goaway=%d, rejected=%d, response=%d, successes=%zu)\n",
                static_cast<int>(stage.size()), stage.data(), error.what(), peer.accepted_connections(),
                static_cast<unsigned long long>(peer.first_request_stream()),
                static_cast<unsigned long long>(peer.second_request_stream()),
                peer.go_away_sent(), peer.rejection_reset_sent(),
                peer.first_connection_application_response_started(), caller_successes);
        } catch (...) {
            std::fprintf(stderr, "HTTP/3 GOAWAY replay test failed with a non-standard exception\n");
        }
        RUVIA_CHECK(false);
    }
}
}  // namespace

RUVIA_TEST(http3_public_http_client_constructed_before_worker_launch_binds_quic_owner_on_worker) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    quic_udp_blackhole peer(io);
    ruvia::http_client client(attachment.loop(), ruvia::http_client_config{
                                                     .scheme_ = ruvia::http_scheme::https,
                                                     .host_ = "127.0.0.1",
                                                     .port_ = peer.port(),
                                                     .connection_count_ = 1,
                                                     .connect_timeout_ = 2s,
                                                     .request_timeout_ = 5s,
                                                     .acquire_timeout_ = 1s,
                                                     .max_response_bytes_ = 4096,
                                                     .protocol_ = ruvia::http_client_protocol::http3_only,
                                                     .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification,
                                                 });
    send_result result;
    auto root = attachment.loop().start(
        exercise_cross_thread_constructed_http3_client(attachment, peer, client, result));
    std::thread worker_value([&attachment] { attachment.run(); });
    worker_value.join();
    root.get();
    RUVIA_CHECK(result.finished_);
    RUVIA_CHECK(result.error_ == ruvia::http_client_error::code_type::cancelled);
    RUVIA_CHECK(peer.quic_long_headers() >= 1);
}

RUVIA_TEST(http3_public_http_client_handle_dispatches_udp_and_maps_pool_slots_to_connections) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    quic_udp_blackhole peer(io);
    auto root = attachment.loop().start(
        exercise_public_http3_dispatch(attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
}

RUVIA_TEST(http3_public_http_client_rotates_connection_after_peer_go_away) {
    test_identity_files identity;
    go_away_rotation_peer peer(identity);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    auto root = attachment.loop().start(
        exercise_public_http3_go_away_rotation(io, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
}

RUVIA_TEST(http3_public_http_client_retries_peer_reported_unprocessed_request) {
    test_identity_files identity;
    go_away_rotation_peer peer(identity, true);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    auto root = attachment.loop().start(
        exercise_public_http3_go_away_replay(io, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
}
