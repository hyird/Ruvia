#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/use_future.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/core/asio_task.h"

#include "http3/http3_quic_client_socket_session.h"
#include "http3/http3_quic_client_tls_context.h"
#include "http3_quic_udp_pair.h"
#include "server/http_server_options.h"
#include "test_harness.h"
#include "test_tls_crypto.h"

namespace ruvia::detail {
struct http3_quic_client_socket_session_test_access final {
    static std::size_t rejected_early_stream_capacity(
        const http3_quic_client_socket_session& session_value) noexcept {
        return session_value.rejected_early_streams_.capacity();
    }

    static void close_candidate_socket(http3_quic_client_socket_session& session_value) noexcept {
        if (session_value.candidate_socket_) {
            asio::error_code ignored;
            session_value.candidate_socket_->close(ignored);
        }
    }

    struct scripted_sender final {
        std::size_t calls_{};

        [[nodiscard]] std::size_t operator()(asio::ip::udp::socket&,
            asio::const_buffer, asio::error_code& error) noexcept {
            ++calls_;
            error = asio::error::would_block;
            return 0;
        }
    };

    static http3_quic_client_socket_session::pump_result_type pump_with_sender(
        http3_quic_client_socket_session& session_value, scripted_sender& sender) {
        return session_value.pump_with_send(sender);
    }

    static void queue_pending_packet(http3_quic_client_socket_session& session_value) noexcept {
        session_value.packet_buffer_[0] = std::byte{0};
        session_value.pending_packet_size_ = 1;
        session_value.pending_candidate_ = false;
    }
};
}  // namespace ruvia::detail

namespace {

template <typename t_type>
bool drive_until_ready(asio::io_context& io, std::future<t_type>& future,
    std::chrono::steady_clock::time_point deadline_value) {
    while (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline_value) {
        io.restart();
        const auto remaining = deadline_value - std::chrono::steady_clock::now();
        const auto maximum_slice = std::chrono::duration_cast<
            std::chrono::steady_clock::duration>(std::chrono::milliseconds(10));
        io.run_for(std::min(remaining, maximum_slice));
    }
    return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

class quic_test_identity final {
public:
    using tls_config = ruvia::detail::http_server_listener_definition::tls_type;

    quic_test_identity() {
        directory_ = std::filesystem::temp_directory_path() /
                     ("ruvia-quic-migration-" +
                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(directory_)) {
            throw std::runtime_error("failed to create QUIC test identity directory");
        }
        key_.reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1"));
        certificate_.reset(X509_new_ex(nullptr, nullptr));
        if (!key_ || !certificate_ || X509_set_version(certificate_.get(), 2) != 1 ||
            ASN1_INTEGER_set(X509_get_serialNumber(certificate_.get()), 1) != 1 ||
            X509_gmtime_adj(X509_getm_notBefore(certificate_.get()), -60) == nullptr ||
            X509_gmtime_adj(X509_getm_notAfter(certificate_.get()), 3600) == nullptr ||
            X509_set_pubkey(certificate_.get(), key_.get()) != 1) {
            throw std::runtime_error("failed to create QUIC test certificate");
        }
        const auto subject = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
        if (subject == nullptr ||
            X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) != 1 ||
            X509_set_subject_name(certificate_.get(), subject.get()) != 1 ||
            X509_set_issuer_name(certificate_.get(), subject.get()) != 1 ||
            ruvia::test::sign_tls_certificate(certificate_.get(), key_.get()) <= 0) {
            throw std::runtime_error("failed to sign QUIC test certificate");
        }
        certificate_path_ = directory_ / "certificate.pem";
        private_key_path_ = directory_ / "private-key.pem";
        std::unique_ptr<BIO, decltype(&BIO_free)> certificate_bio(
            BIO_new_file(certificate_path_.string().c_str(), "w"), BIO_free);
        std::unique_ptr<BIO, decltype(&BIO_free)> key_bio(
            BIO_new_file(private_key_path_.string().c_str(), "w"), BIO_free);
        if (!certificate_bio || !key_bio ||
            PEM_write_bio_X509(certificate_bio.get(), certificate_.get()) != 1 ||
            ruvia::test::write_tls_private_key(key_bio.get(), key_.get()) != 1) {
            throw std::runtime_error("failed to write QUIC test certificate");
        }
    }

    ~quic_test_identity() {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    [[nodiscard]] tls_config server_tls_config() const {
        tls_config config;
        config.identity_.certificate_chain_file_ = certificate_path_.string();
        config.identity_.private_key_file_ = private_key_path_.string();
        return config;
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path certificate_path_;
    std::filesystem::path private_key_path_;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key_{nullptr, EVP_PKEY_free};
    std::unique_ptr<X509, decltype(&X509_free)> certificate_{nullptr, X509_free};
};

}  // namespace

RUVIA_TEST(http3_quic_client_socket_session_owns_concrete_connected_socket) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peer_socket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(client_transport_config_view{});
    http3_quic_client_socket_session session_value(io, peer_socket.local_endpoint(), "localhost", tls,
        {.qpack_max_table_capacity_ = 4096, .qpack_blocked_streams_ = 16},
        ruvia::quic_version::v1, false, nullptr);
    RUVIA_CHECK_EQ(http3_quic_client_socket_session_test_access::
                       rejected_early_stream_capacity(session_value),
        0U);
    RUVIA_CHECK(session_value.local_endpoint().port() != 0);
    RUVIA_CHECK(session_value.local_endpoint().address().is_loopback());
    RUVIA_CHECK(session_value.peer_endpoint() == peer_socket.local_endpoint());
    const auto first = session_value.pump();
    RUVIA_CHECK(first.status_ == http3_quic_client_socket_session::pump_status_type::active ||
                first.status_ == http3_quic_client_socket_session::pump_status_type::would_block);
    const std::array<char, 0> empty{};
    asio::error_code error;
    (void)peer_socket.send_to(asio::buffer(empty), session_value.local_endpoint(), 0, error);
    RUVIA_CHECK(!error);
    const auto ignored = session_value.pump();
    RUVIA_CHECK(ignored.status_ != http3_quic_client_socket_session::pump_status_type::fatal);
    RUVIA_CHECK(ignored.received_ != 0);
    auto unstarted = session_value.wait_readable();
    session_value.close();
    RUVIA_CHECK(session_value.pump().status_ == http3_quic_client_socket_session::pump_status_type::closed);
    session_value.close();
}

RUVIA_TEST(http3_quic_client_socket_session_keeps_polling_during_send_backpressure) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peer(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(client_transport_config_view{});
    http3_quic_client_socket_session session_value(io, peer.local_endpoint(), "localhost", tls);
    (void)session_value.pump();
    http3_quic_client_socket_session_test_access::scripted_sender sender;
    http3_quic_client_socket_session_test_access::queue_pending_packet(session_value);
    const std::array<char, 1> input{'x'};
    (void)peer.send_to(asio::buffer(input), session_value.local_endpoint());
    const auto tick = http3_quic_client_socket_session_test_access::pump_with_sender(session_value, sender);
    RUVIA_CHECK_EQ(sender.calls_, 1U);
    RUVIA_CHECK(tick.output_backpressured_);
    RUVIA_CHECK(tick.received_ > 0);
    RUVIA_CHECK(tick.event_timeout_.has_value());
    session_value.close();
}

RUVIA_TEST(http3_quic_client_socket_session_migrates_with_two_live_udp_paths) {
    using namespace ruvia::detail;
    using udp = asio::ip::udp;
    quic_test_identity identity;
    ruvia::testing::http3_quic_udp_pair peer(
        identity.server_tls_config(), ruvia::testing::http3_quic_udp_pair::server_only_t{});
    asio::io_context io;
    http3_quic_client_tls_context tls(client_transport_config_view{
        .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification});
    http3_quic_client_socket_session session_value(io, peer.server_endpoint(), "localhost", tls);

    const auto handshake_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!session_value.transport().info().confirmed_ &&
           std::chrono::steady_clock::now() < handshake_deadline) {
        (void)session_value.pump();
        peer.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(session_value.transport().info().confirmed_);

    const auto old_endpoint = session_value.local_endpoint();
    // A connected UDP endpoint may be rebound on Windows. If the OS allows
    // the candidate socket, QUIC rejects migrating to its existing path.
    bool same_path_rejected = false;
    try {
        const auto same_path = session_value.start_path_migration(old_endpoint);
        same_path_rejected = same_path.status_ == ruvia::quic_migration_status::rejected;
    } catch (const std::system_error& error) {
        same_path_rejected = error.code() == asio::error::address_in_use;
    }
    RUVIA_CHECK(same_path_rejected);
    RUVIA_CHECK(session_value.local_endpoint() == old_endpoint);
    RUVIA_CHECK(!session_value.active_path_migration());

    const auto reserve_endpoint = [&] {
        udp::socket reservation(io, udp::endpoint(asio::ip::address_v4::loopback(), 0));
        return reservation.local_endpoint();
    };
    const auto candidate_endpoint = reserve_endpoint();
    const auto migration = session_value.start_path_migration(candidate_endpoint);
    RUVIA_CHECK(migration.status_ == ruvia::quic_migration_status::started);
    const auto validation_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    std::optional<ruvia::quic_path_migration> validated;
    while (std::chrono::steady_clock::now() < validation_deadline) {
        (void)session_value.pump();
        peer.pump();
        validated = session_value.path_migration(migration.id_);
        if (!validated || validated->status_ != ruvia::quic_migration_status::started) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(validated.has_value());
    RUVIA_CHECK(validated->status_ == ruvia::quic_migration_status::validated);
    RUVIA_CHECK(session_value.local_endpoint() == candidate_endpoint);

    const auto return_migration = session_value.start_path_migration(old_endpoint);
    RUVIA_CHECK(return_migration.status_ == ruvia::quic_migration_status::started ||
                return_migration.status_ == ruvia::quic_migration_status::validated);
    const auto return_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    std::optional<ruvia::quic_path_migration> returned;
    while (std::chrono::steady_clock::now() < return_deadline) {
        (void)session_value.pump();
        peer.pump();
        returned = session_value.path_migration(return_migration.id_);
        if (!returned || returned->status_ != ruvia::quic_migration_status::started) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(returned.has_value());
    RUVIA_CHECK(returned->status_ == ruvia::quic_migration_status::validated);
    RUVIA_CHECK(session_value.local_endpoint() == old_endpoint);

    const auto candidate_io_endpoint = reserve_endpoint();
    const auto candidate_io_migration = session_value.start_path_migration(candidate_io_endpoint);
    RUVIA_CHECK(candidate_io_migration.status_ == ruvia::quic_migration_status::started);
    RUVIA_CHECK(session_value.consume_work_notification());
    const auto candidate_activity = session_value.pump();
    auto candidate_wait = asio::co_spawn(io,
        ruvia::as_awaitable(session_value.wait_for_activity(candidate_activity)), asio::use_future);
    io.restart();
    RUVIA_CHECK(io.run_one_for(std::chrono::seconds(1)) != 0);
    RUVIA_CHECK(candidate_wait.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    http3_quic_client_socket_session_test_access::close_candidate_socket(session_value);
    const auto candidate_wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    RUVIA_CHECK(drive_until_ready(io, candidate_wait, candidate_wait_deadline));
    RUVIA_CHECK(candidate_wait.get() ==
                http3_quic_client_socket_session::wake_reason_type::candidate_failure);
    (void)session_value.pump();
    peer.pump();
    const auto candidate_io_failed = session_value.path_migration(candidate_io_migration.id_);
    RUVIA_CHECK(candidate_io_failed.has_value());
    RUVIA_CHECK(candidate_io_failed->status_ == ruvia::quic_migration_status::failed);
    RUVIA_CHECK(session_value.local_endpoint() == old_endpoint);
    RUVIA_CHECK(session_value.transport().info().state_ == ruvia::quic_connection_state::ready);

    const auto fallback_endpoint = session_value.local_endpoint();
    const auto failing_endpoint = reserve_endpoint();
    const auto pending_validation_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    std::optional<ruvia::quic_path_migration> failing;
    while (std::chrono::steady_clock::now() < pending_validation_deadline) {
        const auto attempt_value = session_value.start_path_migration(failing_endpoint);
        if (attempt_value.status_ == ruvia::quic_migration_status::started ||
            attempt_value.status_ == ruvia::quic_migration_status::validated) {
            failing = attempt_value;
            break;
        }
        (void)session_value.pump();
        peer.pump(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(failing.has_value());
    const auto failure_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    std::optional<ruvia::quic_path_migration> failed;
    while (failing && std::chrono::steady_clock::now() < failure_deadline) {
        (void)session_value.pump();
        peer.pump(true);
        failed = session_value.path_migration(failing->id_);
        if (!failed || failed->status_ != ruvia::quic_migration_status::started) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(failed.has_value());
    RUVIA_CHECK(failed->status_ == ruvia::quic_migration_status::failed);
    RUVIA_CHECK(session_value.local_endpoint() == fallback_endpoint);

    const auto cancel_endpoint = reserve_endpoint();
    const auto cancel = session_value.start_path_migration(cancel_endpoint);
    RUVIA_CHECK(cancel.status_ == ruvia::quic_migration_status::started);
    RUVIA_CHECK(session_value.consume_work_notification());
    const auto activity = session_value.pump();
    auto wait = asio::co_spawn(io, ruvia::as_awaitable(session_value.wait_for_activity(activity)),
        asio::use_future);
    io.restart();
    RUVIA_CHECK(io.run_one_for(std::chrono::seconds(1)) != 0);
    RUVIA_CHECK(wait.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    RUVIA_CHECK(session_value.cancel_path_migration(cancel.id_) == ruvia::quic_operation_status::accepted);
    session_value.close();
    const auto stop_wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    RUVIA_CHECK(drive_until_ready(io, wait, stop_wait_deadline));
    RUVIA_CHECK(wait.get() == http3_quic_client_socket_session::wake_reason_type::stopped);
    RUVIA_CHECK_EQ(io.poll(), 0U);
    RUVIA_CHECK(session_value.pump().status_ == http3_quic_client_socket_session::pump_status_type::closed);
    const auto cancelled = session_value.path_migration(cancel.id_);
    RUVIA_CHECK(cancelled.has_value());
    RUVIA_CHECK(cancelled->status_ == ruvia::quic_migration_status::aborted);
}

RUVIA_TEST(http3_quic_client_socket_session_write_wait_completes_when_socket_is_ready) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peer_socket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(client_transport_config_view{});
    http3_quic_client_socket_session session_value(io, peer_socket.local_endpoint(), "localhost", tls);
    auto future = asio::co_spawn(io, ruvia::as_awaitable(session_value.wait_writable()), asio::use_future);
    io.run();
    RUVIA_CHECK(!ruvia::testing::throws_on([&] { future.get(); }));
    session_value.close();
}

RUVIA_TEST(http3_quic_client_socket_session_activity_wait_wakes_on_readable_datagram) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peer_socket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(client_transport_config_view{});
    http3_quic_client_socket_session session_value(io, peer_socket.local_endpoint(), "localhost", tls);
    const std::array<char, 1> datagram{'x'};
    (void)peer_socket.send_to(asio::buffer(datagram), session_value.local_endpoint());
    auto future = asio::co_spawn(io, ruvia::as_awaitable(session_value.wait_for_activity({})),
        asio::use_future);
    io.run();
    RUVIA_CHECK(future.get() == http3_quic_client_socket_session::wake_reason_type::readable);
    session_value.close();
}

RUVIA_TEST(http3_quic_client_socket_session_activity_wait_retries_full_input_via_quic_timer) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peer_socket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(client_transport_config_view{});
    http3_quic_client_socket_session session_value(io, peer_socket.local_endpoint(), "localhost", tls);
    const std::array<char, 1> datagram{'x'};
    (void)peer_socket.send_to(asio::buffer(datagram), session_value.local_endpoint());
    http3_quic_client_socket_session::pump_result_type full_input;
    full_input.input_backpressured_ = true;
    auto future = asio::co_spawn(io, ruvia::as_awaitable(session_value.wait_for_activity(full_input)),
        asio::use_future);
    io.run();
    RUVIA_CHECK(future.get() == http3_quic_client_socket_session::wake_reason_type::quic_event);
    session_value.close();
}

RUVIA_TEST(http3_quic_client_socket_session_activity_wait_drains_read_and_timer_after_writable) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peer_socket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(client_transport_config_view{});
    http3_quic_client_socket_session session_value(io, peer_socket.local_endpoint(), "localhost", tls);
    http3_quic_client_socket_session::pump_result_type blocked_output;
    blocked_output.output_backpressured_ = true;
    blocked_output.event_timeout_ = std::chrono::milliseconds(25);
    auto future = asio::co_spawn(io, ruvia::as_awaitable(session_value.wait_for_activity(blocked_output)),
        asio::use_future);
    io.run();
    RUVIA_CHECK(future.get() == http3_quic_client_socket_session::wake_reason_type::writable);
    RUVIA_CHECK_EQ(io.poll(), 0U);
    session_value.close();
}

RUVIA_TEST(http3_quic_client_socket_session_activity_wait_preserves_absolute_deadline_across_ticks) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peer_socket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(client_transport_config_view{});
    http3_quic_client_socket_session session_value(io, peer_socket.local_endpoint(), "localhost", tls);
    http3_quic_client_socket_session::pump_result_type pending_input;
    pending_input.input_backpressured_ = true;
    pending_input.event_timeout_ = std::chrono::milliseconds(2);
    const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::milliseconds(18);
    bool expired{};
    int quic_events{};
    for (int i = 0; i < 32; ++i) {
        auto future = asio::co_spawn(io,
            ruvia::as_awaitable(session_value.wait_for_activity(pending_input, deadline_value)),
            asio::use_future);
        io.run();
        io.restart();
        const auto reason = future.get();
        if (reason == http3_quic_client_socket_session::wake_reason_type::deadline) {
            expired = true;
            break;
        }
        RUVIA_CHECK(reason == http3_quic_client_socket_session::wake_reason_type::quic_event);
        ++quic_events;
    }
    RUVIA_CHECK(expired && quic_events > 0);
    session_value.close();
}

RUVIA_TEST(http3_quic_client_socket_session_activity_wait_drains_read_write_timer_on_stop) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peer_socket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(client_transport_config_view{});
    http3_quic_client_socket_session session_value(io, peer_socket.local_endpoint(), "localhost", tls);
    http3_quic_client_socket_session::pump_result_type blocked_output;
    blocked_output.output_backpressured_ = true;
    blocked_output.event_timeout_ = std::chrono::seconds(1);
    auto future = asio::co_spawn(io, ruvia::as_awaitable(session_value.wait_for_activity(blocked_output)),
        asio::use_future);
    RUVIA_CHECK(io.poll_one() != 0);
    session_value.request_stop();
    io.restart();
    io.run();
    RUVIA_CHECK(future.get() == http3_quic_client_socket_session::wake_reason_type::stopped);
    RUVIA_CHECK_EQ(io.poll(), 0U);
    session_value.close();
}

RUVIA_TEST(http3_quic_client_socket_session_activity_wait_close_joins_all_pending_handlers) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peer_socket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(client_transport_config_view{});
    http3_quic_client_socket_session session_value(io, peer_socket.local_endpoint(), "localhost", tls);
    http3_quic_client_socket_session::pump_result_type blocked_output;
    blocked_output.output_backpressured_ = true;
    blocked_output.event_timeout_ = std::chrono::milliseconds(50);
    auto future = asio::co_spawn(io, ruvia::as_awaitable(session_value.wait_for_activity(blocked_output)),
        asio::use_future);
    RUVIA_CHECK(io.poll_one() != 0);
    session_value.close();
    io.restart();
    io.run();
    RUVIA_CHECK(future.get() == http3_quic_client_socket_session::wake_reason_type::stopped);
    RUVIA_CHECK_EQ(io.poll(), 0U);
}

RUVIA_TEST(http3_quic_client_socket_session_activity_wait_rejects_concurrent_cycles_without_losing_owner) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peer_socket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(client_transport_config_view{});
    http3_quic_client_socket_session session_value(io, peer_socket.local_endpoint(), "localhost", tls);
    auto first = asio::co_spawn(io, ruvia::as_awaitable(session_value.wait_for_activity({})),
        asio::use_future);
    RUVIA_CHECK(io.poll_one() != 0);
    auto duplicate = asio::co_spawn(io, ruvia::as_awaitable(session_value.wait_for_activity({})),
        asio::use_future);
    io.run_for(std::chrono::milliseconds(5));
    const bool duplicate_ready =
        duplicate.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    RUVIA_CHECK(duplicate_ready);
    session_value.request_stop();
    io.restart();
    io.run();
    RUVIA_CHECK(first.get() == http3_quic_client_socket_session::wake_reason_type::stopped);
    if (duplicate_ready) {
        RUVIA_CHECK(duplicate.get() == http3_quic_client_socket_session::wake_reason_type::fatal);
    }
    session_value.close();
}

RUVIA_TEST(http3_quic_client_socket_session_application_wake_is_latched_before_arming) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peer_socket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(client_transport_config_view{});
    http3_quic_client_socket_session session_value(io, peer_socket.local_endpoint(), "localhost", tls);
    session_value.notify_work();
    session_value.notify_work();
    auto future = asio::co_spawn(io, ruvia::as_awaitable(session_value.wait_for_activity({})),
        asio::use_future);
    io.run();
    RUVIA_CHECK(future.get() == http3_quic_client_socket_session::wake_reason_type::application);
    RUVIA_CHECK(session_value.consume_work_notification());
    RUVIA_CHECK(!session_value.consume_work_notification());
    http3_quic_client_socket_session::pump_result_type pending_input;
    pending_input.input_backpressured_ = true;
    pending_input.event_timeout_ = std::chrono::milliseconds(2);
    auto next_value = asio::co_spawn(io, ruvia::as_awaitable(session_value.wait_for_activity(pending_input)),
        asio::use_future);
    io.restart();
    io.run();
    RUVIA_CHECK(next_value.get() == http3_quic_client_socket_session::wake_reason_type::quic_event);
    session_value.close();
}

RUVIA_TEST(http3_quic_client_socket_session_application_wake_drains_all_armed_handlers) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peer_socket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(client_transport_config_view{});
    http3_quic_client_socket_session session_value(io, peer_socket.local_endpoint(), "localhost", tls);
    http3_quic_client_socket_session::pump_result_type pending;
    pending.event_timeout_ = std::chrono::seconds(1);
    auto future = asio::co_spawn(io, ruvia::as_awaitable(session_value.wait_for_activity(pending)),
        asio::use_future);
    RUVIA_CHECK(io.poll_one() != 0);
    session_value.notify_work();
    io.restart();
    io.run();
    RUVIA_CHECK(future.get() == http3_quic_client_socket_session::wake_reason_type::application);
    RUVIA_CHECK_EQ(io.poll(), 0U);
    RUVIA_CHECK(session_value.consume_work_notification());
    RUVIA_CHECK(!session_value.consume_work_notification());
    session_value.close();
}

RUVIA_TEST(http3_quic_client_socket_session_application_wake_cannot_override_stop) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peer_socket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(client_transport_config_view{});
    http3_quic_client_socket_session session_value(io, peer_socket.local_endpoint(), "localhost", tls);
    http3_quic_client_socket_session::pump_result_type pending;
    pending.event_timeout_ = std::chrono::seconds(1);
    auto future = asio::co_spawn(io, ruvia::as_awaitable(session_value.wait_for_activity(pending)),
        asio::use_future);
    RUVIA_CHECK(io.poll_one() != 0);
    session_value.notify_work();
    session_value.request_stop();
    io.restart();
    io.run();
    RUVIA_CHECK(future.get() == http3_quic_client_socket_session::wake_reason_type::stopped);
    RUVIA_CHECK_EQ(io.poll(), 0U);
    session_value.close();
}

RUVIA_TEST(http3_quic_client_socket_session_activity_wait_cold_drop_and_close_before_start) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peer_socket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(client_transport_config_view{});
    http3_quic_client_socket_session session_value(io, peer_socket.local_endpoint(), "localhost", tls);
    {
        auto cold = session_value.wait_for_activity({});
    }
    session_value.request_stop();
    auto future = asio::co_spawn(io, ruvia::as_awaitable(session_value.wait_for_activity({})),
        asio::use_future);
    io.run();
    RUVIA_CHECK(future.get() == http3_quic_client_socket_session::wake_reason_type::stopped);
    session_value.close();
}

RUVIA_TEST(http3_quic_client_socket_session_close_wakes_joined_read_wait) {
    using namespace ruvia::detail;
    asio::io_context io;
    asio::ip::udp::socket peer_socket(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    http3_quic_client_tls_context tls(client_transport_config_view{});
    http3_quic_client_socket_session session_value(io, peer_socket.local_endpoint(), "localhost", tls);
    auto future = asio::co_spawn(io, ruvia::as_awaitable(session_value.wait_readable()), asio::use_future);
    RUVIA_CHECK(io.poll() != 0);
    RUVIA_CHECK(future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    session_value.close();
    io.restart();
    io.run();
    RUVIA_CHECK(ruvia::testing::throws_on([&] { future.get(); }));
}
