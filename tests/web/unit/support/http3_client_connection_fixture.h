#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
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

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/http/http3_client_request_head.h"
#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_local_critical_streams.h"
#include "ruvia/http/http3_var_int.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_client_types.h"
#include "ruvia/web/http_udp_tunnel.h"
#include "ruvia/web/websocket_client.h"

#include "client/http_client_pool.h"
#include "client/http_client_response_state.h"
#include "http3/http3_client_connection.h"
#include "http3/http3_quic_client_tls_context.h"
#include "http3/http3_quic_client_transport.h"
#include "http3/http3_quic_socket_address.h"
#include "http3_quic_udp_pair.h"
#include "router/router_impl.h"
#include "server/acceptor.h"
#include "server/http_server_options_validation.h"
#include "server/web_worker_runtime.h"
#include "test_harness.h"
#include "test_io_context.h"
#include "test_tls_crypto.h"

namespace ruvia::detail {
struct http3_client_connection_test_access final {
    using http3_client_connection = ruvia::detail::http3_client_connection;

    static std::optional<std::uint64_t> request_stream_id(
        const http3_client_connection& connection,
        http3_client_connection::request_id_type id) noexcept {
        const auto request = std::find_if(connection.requests_.begin(), connection.requests_.end(),
            [id](const auto& candidate_value) { return candidate_value.id_ == id; });
        return request == connection.requests_.end() ? std::nullopt : request->writer_.stream_id();
    }

    static bool has_pending_priority_update(
        const http3_client_connection& connection,
        http3_client_connection::request_id_type id) noexcept {
        const auto request = std::find_if(connection.requests_.begin(), connection.requests_.end(),
            [id](const auto& candidate_value) { return candidate_value.id_ == id; });
        return request != connection.requests_.end() && request->pending_priority_update_.has_value();
    }

    static bool early_request_finished(const http3_client_connection& connection,
        http3_client_connection::request_id_type id) noexcept {
        if (!connection.session_) {
            return false;
        }
        const auto request = std::find_if(connection.requests_.begin(), connection.requests_.end(),
            [id](const auto& candidate_value) { return candidate_value.id_ == id; });
        if (request == connection.requests_.end()) {
            return false;
        }
        const auto info = connection.session_->transport().info();
        return connection.session_->early_data_enabled() &&
               info.early_data_ == ruvia::quic_early_data_state::available &&
               !info.quic_handshake_complete_ && request->response_parser_registered_ &&
               request->writer_.finished();
    }
};
}  // namespace ruvia::detail

namespace http3_client_connection_test {

using connection_type = ruvia::detail::http3_client_connection;
using namespace std::chrono_literals;

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_{};
    std::size_t returns_{};
    std::size_t live_bytes_{};
    std::size_t rejected_allocations_{};
    struct allocation_type final {
        std::size_t bytes_{};
        std::size_t alignment_{};
        friend bool operator==(const allocation_type&, const allocation_type&) = default;
    };

    void begin_trace() noexcept {
        first_allocation_.reset();
    }
    [[nodiscard]] std::optional<allocation_type> first_allocation() const noexcept {
        return first_allocation_;
    }
    void fail_first_allocation(allocation_type allocation) noexcept {
        failed_allocation_ = allocation;
        fail_matching_allocation_ = true;
    }
    [[nodiscard]] allocation_type last_rejected_allocation() const noexcept {
        return last_rejected_allocation_;
    }
    [[nodiscard]] std::size_t matching_allocation_attempts() const noexcept {
        return matching_allocation_attempts_;
    }
    void reject_allocations(bool reject = true) noexcept {
        rejecting_ = reject;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        const allocation_type allocation{bytes_value, alignment};
        if (!first_allocation_) {
            first_allocation_ = allocation;
        }
        if (failed_allocation_ && allocation == *failed_allocation_) {
            ++matching_allocation_attempts_;
            if (fail_matching_allocation_) {
                fail_matching_allocation_ = false;
                ++rejected_allocations_;
                last_rejected_allocation_ = allocation;
                throw std::bad_alloc();
            }
        }
        if (rejecting_) {
            ++rejected_allocations_;
            throw std::bad_alloc();
        }
        auto* address = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++allocations_;
        live_bytes_ += bytes_value;
        return address;
    }
    void do_deallocate(void* address, std::size_t bytes_value, std::size_t alignment) override {
        ++returns_;
        live_bytes_ -= bytes_value;
        std::pmr::new_delete_resource()->deallocate(address, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return &other == this;
    }

    std::optional<allocation_type> first_allocation_;
    std::optional<allocation_type> failed_allocation_;
    allocation_type last_rejected_allocation_{};
    std::size_t matching_allocation_attempts_{};
    bool fail_matching_allocation_{};
    bool rejecting_{};
};

class test_identity_files final {
public:
    test_identity_files() {
        std::random_device random;
        directory_ = std::filesystem::temp_directory_path() /
                     ("ruvia-h3-client-" + std::to_string(random()) + "-" +
                         std::to_string(random()));
        if (!std::filesystem::create_directory(directory_)) {
            throw std::runtime_error("failed to create HTTP/3 test identity directory");
        }
        try {
            EVP_PKEY_CTX* raw_context = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
            if (raw_context == nullptr) {
                throw std::runtime_error("failed to create test key generator");
            }
            std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
                raw_context, EVP_PKEY_CTX_free);
            EVP_PKEY* raw_key = nullptr;
            if (EVP_PKEY_keygen_init(context.get()) <= 0 ||
                EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0 ||
                EVP_PKEY_generate(context.get(), &raw_key) <= 0) {
                throw std::runtime_error("failed to generate HTTP/3 test key");
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
                throw std::runtime_error("failed to create HTTP/3 test certificate");
            }
            const auto subject = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
            const auto* name = reinterpret_cast<const unsigned char*>("localhost");
            if (!subject || X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_ASC, name, -1, -1, 0) != 1 ||
                X509_set_subject_name(certificate.get(), subject.get()) != 1 ||
                X509_set_issuer_name(certificate.get(), subject.get()) != 1) {
                throw std::runtime_error("failed to name HTTP/3 test identity");
            }
            const std::array extensions{std::pair{NID_subject_alt_name, "IP:127.0.0.1,DNS:localhost"},
                std::pair{NID_basic_constraints, "critical,CA:TRUE"}};
            for (const auto& [nid, value] : extensions) {
                std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
                    X509V3_EXT_nconf_nid(nullptr, nullptr, nid, value), X509_EXTENSION_free);
                if (!extension || X509_add_ext(certificate.get(), extension.get(), -1) != 1) {
                    throw std::runtime_error("failed to authenticate HTTP/3 test identity");
                }
            }
            if (ruvia::test::sign_tls_certificate(certificate.get(), key.get()) <= 0) {
                throw std::runtime_error("failed to sign HTTP/3 test identity");
            }
            certificate_file_ = directory_ / "cert.pem";
            private_key_file_ = directory_ / "key.pem";
            std::unique_ptr<BIO, decltype(&BIO_free)> cert_bio(
                BIO_new_file(certificate_file_.string().c_str(), "w"), BIO_free);
            std::unique_ptr<BIO, decltype(&BIO_free)> key_bio(
                BIO_new_file(private_key_file_.string().c_str(), "w"), BIO_free);
            if (!cert_bio || !key_bio || PEM_write_bio_X509(cert_bio.get(), certificate.get()) != 1 ||
                ruvia::test::write_tls_private_key(key_bio.get(), key.get()) != 1) {
                throw std::runtime_error("failed to write HTTP/3 test identity");
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

[[nodiscard]] inline ruvia::quic_stream_write_result write_quic_stream(
    ruvia::quic_connection& connection, std::uint64_t stream_id,
    std::span<const char> bytes_value, bool fin = false) {
    return connection.write_stream(stream_id, std::as_bytes(bytes_value), fin);
}

[[nodiscard]] inline ruvia::quic_stream_read_result read_quic_stream(
    ruvia::quic_connection& connection, std::uint64_t stream_id,
    std::span<char> bytes_value) {
    return connection.read_stream(stream_id, std::as_writable_bytes(bytes_value));
}

[[nodiscard]] inline bool is_quic_stream_closed(ruvia::quic_operation_status status) noexcept {
    return status == ruvia::quic_operation_status::stream_closed ||
           status == ruvia::quic_operation_status::closing ||
           status == ruvia::quic_operation_status::draining ||
           status == ruvia::quic_operation_status::retired;
}

[[nodiscard]] inline std::vector<char> test_http3_frame(
    std::uint64_t type, std::span<const char> payload_value) {
    std::array<char, 16> header_value{};
    const auto type_size = ruvia::encode_http3_var_int(header_value, type);
    if ((type_size.index() != 0)) {
        throw std::runtime_error("failed to encode HTTP/3 test frame type");
    }
    const auto payload_size = ruvia::encode_http3_var_int(
        std::span<char>(header_value).subspan(std::get<0>(type_size)), payload_value.size());
    if ((payload_size.index() != 0)) {
        throw std::runtime_error("failed to encode HTTP/3 test frame length");
    }
    std::vector<char> output(header_value.begin(), header_value.begin() + std::get<0>(type_size) + std::get<0>(payload_size));
    output.insert(output.end(), payload_value.begin(), payload_value.end());
    return output;
}

struct quic_push_scenario final {
    std::size_t count_{1};
    std::size_t body_bytes_{5};
    bool stream_before_promise_{false};
    bool promise_only_{false};
    bool cancel_before_promise_{false};
    bool malformed_response_{false};
    bool cross_origin_{false};
};

class local_quic_response_peer final {
public:
    using stream_id = std::uint64_t;

    explicit local_quic_response_peer(const test_identity_files& identity,
        bool malformed_tail = false, bool consume_request = true, bool websocket = false,
        bool advertise_origins = false, std::optional<quic_push_scenario> push = {},
        bool tunnel = false, bool udp = false, bool early_data = false,
        ruvia::detail::http3_quic_tls_context* shared_server_tls = nullptr,
        bool hold_handshake_on_early_decision = false)
        : consume_request_(consume_request),
          websocket_(websocket),
          advertise_origins_(advertise_origins),
          push_(push),
          tunnel_(tunnel || udp),
          udp_(udp),
          early_data_(early_data),
          shared_server_tls_(shared_server_tls),
          hold_handshake_on_early_decision_(hold_handshake_on_early_decision) {
        ruvia::detail::http_server_listener_definition::tls_type tls;
        tls.identity_.certificate_chain_file_ = identity.certificate().string();
        tls.identity_.private_key_file_ = identity.private_key().string();
        tls.http3_early_data_ = early_data_;
        const std::string_view content_length = malformed_tail ? "67" : "6";
        ruvia::http3_field_section_field_view fields_value[]{{":status", "200"}, {"content-length", content_length}};
        std::pmr::monotonic_buffer_resource temporary;
        const auto encoded_head = ruvia::encode_http3_field_section(fields_value, &temporary);
        if ((encoded_head.index() != 0)) {
            throw std::runtime_error("failed to encode HTTP/3 test response head");
        }
        first_part_ = test_http3_frame(1, std::get<0>(encoded_head));
        const auto first_data = test_http3_frame(0, std::span<const char>("abc", 3));
        first_part_.insert(first_part_.end(), first_data.begin(), first_data.end());
        if (malformed_tail) {
            const std::string body(64, 'x');
            final_part_ = test_http3_frame(0, std::span<const char>(body.data(), body.size()));
            const auto unexpected = test_http3_frame(4, {});
            final_part_.insert(final_part_.end(), unexpected.begin(), unexpected.end());
        } else {
            final_part_ = test_http3_frame(0, std::span<const char>("def", 3));
        }
        if (websocket_) {
            const ruvia::http3_field_section_field_view ws_fields[]{
                {":status", "200"}, {"sec-websocket-protocol", "chat"},
                {"sec-websocket-extensions", "permessage-deflate; server_no_context_takeover; client_no_context_takeover"}};
            const auto ws_head = ruvia::encode_http3_field_section(ws_fields, &temporary);
            if ((ws_head.index() != 0)) {
                throw std::runtime_error("failed to encode WebSocket test response");
            }
            first_part_ = test_http3_frame(1, std::get<0>(ws_head));
            ruvia::websocket_connection websocket_value({.resource_ = &temporary});
            const std::string greeting(100000, 'w');
            if (websocket_value.submit_frame(ruvia::websocket_opcode::binary, greeting, false) != ruvia::websocket_frame_submit_status::accepted) {
                throw std::runtime_error("failed to encode WebSocket test greeting");
            }
            const auto output = websocket_value.output_plan();
            const auto data = test_http3_frame(0, std::span<const char>(output.bytes().data(), output.bytes().size()));
            first_part_.insert(first_part_.end(), data.begin(), data.end());
            (void)websocket_value.consume_output(output.bytes().size());
            if (websocket_value.submit_close(1000, {}) != ruvia::websocket_close_submit_status::accepted) {
                throw std::runtime_error("failed to encode WebSocket test Close");
            }
            const auto close = websocket_value.output_plan();
            final_part_ = test_http3_frame(0, std::span<const char>(close.bytes().data(), close.bytes().size()));
        }
        if (tunnel_) {
            const ruvia::http3_field_section_field_view tunnel_fields[]{{":status", "200"}, {"x-tunnel", "owned-metadata"}};
            const auto head = ruvia::encode_http3_field_section(tunnel_fields, &temporary);
            if ((head.index() != 0)) {
                throw std::runtime_error("CONNECT test head encoding failed");
            }
            first_part_ = test_http3_frame(1, std::get<0>(head));
            const std::string greeting(100003, 's');
            const auto data = test_http3_frame(0, std::span(greeting.data(), greeting.size()));
            first_part_.insert(first_part_.end(), data.begin(), data.end());
            final_part_ = test_http3_frame(0, std::span<const char>("ended", 5));
        }
        if (udp_) {
            const ruvia::http3_field_section_field_view udp_fields[]{{":status", "200"}, {"capsule-protocol", "?1"}};
            const auto head = ruvia::encode_http3_field_section(udp_fields, &temporary);
            if ((head.index() != 0)) {
                throw std::runtime_error("UDP test head encoding failed");
            }
            first_part_ = test_http3_frame(1, std::get<0>(head));
            std::string payload_value(1, '\0');
            payload_value.append(16003, 's');
            std::array<char, 16> header;
            const auto encoded = ruvia::encode_http_capsule_header(header, 0, payload_value.size());
            std::string capsule(header.data(), std::get<0>(encoded));
            capsule.append(payload_value);
            const auto data = test_http3_frame(0, std::span(capsule.data(), capsule.size()));
            first_part_.insert(first_part_.end(), data.begin(), data.end());
            final_part_ = test_http3_frame(0, std::span<const char>("\0\1\0", 3));
        }
        thread_ = std::thread([this, tls = std::move(tls)]() mutable { run(std::move(tls)); });
        std::unique_lock lock(mutex_);
        if (!started_condition_.wait_for(lock, 5s, [this] { return started_ || failure_ != nullptr; })) {
            lock.unlock();
            stop_.store(true, std::memory_order_release);
            thread_.join();
            throw std::runtime_error("HTTP/3 local peer startup watchdog expired");
        }
        const auto failure = failure_;
        lock.unlock();
        if (failure != nullptr) {
            thread_.join();
            std::rethrow_exception(failure);
        }
    }

    ~local_quic_response_peer() {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
        pair_.reset();
    }

    local_quic_response_peer(const local_quic_response_peer&) = delete;
    local_quic_response_peer& operator=(const local_quic_response_peer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool first_part_ready() const noexcept {
        return first_part_ready_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool final_part_sent() const noexcept {
        return final_part_sent_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool handshake_observed() const noexcept {
        return handshake_observed_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool early_data_rejected() const noexcept {
        return early_data_rejected_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool early_data_accepted() const noexcept {
        return early_data_accepted_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool handshake_paused() const noexcept {
        return handshake_paused_.load(std::memory_order_acquire);
    }
    void allow_handshake() noexcept {
        allow_handshake_.store(true, std::memory_order_release);
    }
    [[nodiscard]] std::size_t request_payload_bytes() const noexcept {
        return request_payload_bytes_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool synchronize() {
        std::unique_lock lock(mutex_);
        const auto generation = ++synchronization_requested_;
        if (!started_condition_.wait_for(lock, 5s, [this, generation] {
                return synchronization_completed_ >= generation || failure_ != nullptr;
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
    [[nodiscard]] unsigned websocket_messages() const noexcept {
        return websocket_messages_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool client_end_observed() const noexcept {
        return client_end_observed_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t cancelled_pushes() const noexcept {
        return cancelled_pushes_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t promised_pushes() const noexcept {
        return promised_pushes_.load(std::memory_order_acquire);
    }
    [[nodiscard]] int push_priority_observed() const noexcept {
        return push_priority_observed_.load(std::memory_order_acquire);
    }
    [[nodiscard]] int priority_observed() const noexcept {
        return priority_observed_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::optional<std::uint64_t> peer_max_push_id() const noexcept {
        if (!peer_max_push_id_observed_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        return peer_max_push_id_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::optional<std::uint64_t> priority_element_id() const noexcept {
        if (!priority_element_id_observed_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        return priority_element_id_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t tunnel_bytes() const noexcept {
        return tunnel_bytes_.load(std::memory_order_acquire);
    }
    void allow_final_part() noexcept {
        allow_final_part_.store(true, std::memory_order_release);
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
    void run(ruvia::detail::http_server_listener_definition::tls_type tls) noexcept {
        try {
            if (shared_server_tls_ != nullptr) {
                pair_ = std::make_unique<ruvia::testing::http3_quic_udp_pair>(
                    *shared_server_tls_, ruvia::testing::http3_quic_udp_pair::server_only_t{}, &resource_);
            } else {
                pair_ = std::make_unique<ruvia::testing::http3_quic_udp_pair>(tls,
                    ruvia::testing::http3_quic_udp_pair::server_only_t{}, &resource_);
            }
            auto& pair = *pair_;
            port_.store(pair.server_endpoint().port(), std::memory_order_release);
            auto prefixes = ruvia::http3_local_critical_streams::create({.enable_connect_protocol_ = websocket_ || tunnel_});
            if ((prefixes.index() != 0)) {
                throw std::runtime_error("failed to create local HTTP/3 critical stream prefixes");
            }
            const auto control_prefix = std::get<0>(prefixes).control_prefix();
            std::string server_control(control_prefix.data(), control_prefix.size());
            if (advertise_origins_) {
                const std::array<std::string_view, 2> origins{"https://127.0.0.1", "https://localhost"};
                const auto frame = ruvia::encode_http3_origin_frame(origins);
                if ((frame.index() != 0)) {
                    throw std::runtime_error("failed to encode HTTP/3 ORIGIN fixture");
                }
                server_control.append(std::get<0>(frame).data(), std::get<0>(frame).size());
            }
            {
                std::lock_guard lock(mutex_);
                started_ = true;
            }
            started_condition_.notify_all();

            std::optional<ruvia::quic_connection_token> connection_id;
            std::array<std::optional<stream_id>, 3> local_critical_ids;
            std::array<std::size_t, 3> local_critical_offsets{};
            std::array<std::span<const char>, 3> local_critical_bytes{
                std::span<const char>(server_control), std::get<0>(prefixes).qpack_encoder_prefix(), std::get<0>(prefixes).qpack_decoder_prefix()};
            std::optional<stream_id> request_stream;
            struct push_wire final {
                stream_id stream_{};
                std::vector<char> bytes_;
                std::size_t offset_{};
                std::vector<char> promise_;
                std::chrono::steady_clock::time_point promise_release_{};
                bool finished_{};
            };
            std::deque<push_wire> push_streams;
            std::deque<std::vector<char>> promises;
            std::size_t promise_offset{};
            std::size_t next_push{};
            std::optional<std::chrono::steady_clock::time_point> request_head_at;
            bool request_finished = false;
            bool first_prepared = false;
            bool dynamic_head_prepared = false;
            std::vector<stream_id> peer_critical_streams;
            std::array<std::string, 2> qpack_bytes;
            std::array<std::size_t, 2> qpack_offsets{};
            std::optional<std::chrono::steady_clock::time_point> encoder_release;
            std::size_t first_offset = 0;
            std::size_t response_head_bytes = 0;
            std::size_t final_offset = 0;
            bool final_fin_sent = false;
            std::array<char, 4096> request_bytes{};
            ruvia::http3_connection ws_request(ruvia::http3_peer_role::server, &resource_, {.enable_connect_protocol_ = websocket_ || tunnel_});
            struct ws_receive final {
                std::pmr::memory_resource* resource_;
                std::atomic<int>* priority_;
                std::atomic<int>* push_priority_;
                std::atomic<std::uint64_t>* priority_element_id_;
                std::atomic<bool>* priority_element_id_observed_;
                std::atomic<std::size_t>* cancelled_;
                bool websocket_enabled_;
                bool tunnel_;
                bool udp_;
                std::atomic<std::size_t>* tunnel_bytes_;

                ruvia::http_capsule_decoder capsules_{};
                std::pmr::string capsule_payload_{resource_};
                std::optional<ruvia::websocket_connection> websocket_{};
                bool head_ready_{false};
                unsigned messages_{0};
            } ws_receive_value{&resource_, &priority_observed_, &push_priority_observed_,
                &priority_element_id_, &priority_element_id_observed_, &cancelled_pushes_, websocket_,
                tunnel_, udp_, &tunnel_bytes_};
            const auto on_ws_event = [](void* context_value, const ruvia::http3_connection_event& event) {
                auto& state_value = *static_cast<ws_receive*>(context_value);
                if (event.priority_update_) {
                    const auto priority = event.priority_update_->fields_.request_priority();
                    (event.priority_update_->push_ ? state_value.push_priority_ : state_value.priority_)->store(priority.urgency_ | (priority.incremental_ ? 0x100 : 0), std::memory_order_release);
                    if (!event.priority_update_->push_) {
                        state_value.priority_element_id_->store(event.priority_update_->element_id_, std::memory_order_release);
                        state_value.priority_element_id_observed_->store(true, std::memory_order_release);
                    }
                } else if (event.kind_ == ruvia::http3_connection_event_kind::push_canceled) {
                    state_value.cancelled_->fetch_add(1, std::memory_order_release);
                } else if (event.kind_ == ruvia::http3_connection_event_kind::request_head) {
                    if (!state_value.websocket_enabled_) {
                        state_value.head_ready_ = true;
                        return;
                    }
                    if (!event.head_ || event.head_->method_ != "CONNECT" || event.head_->protocol_ != "websocket") {
                        throw std::runtime_error("invalid WebSocket Extended CONNECT");
                    }
                    state_value.head_ready_ = true;
                    state_value.websocket_.emplace(ruvia::websocket_connection_options{.resource_ = state_value.resource_, .compression_ = {.enabled_ = true}});
                } else if (event.kind_ == ruvia::http3_connection_event_kind::tunnel_data) {
                    if (state_value.udp_) {
                        const auto collect = [](void* raw, ruvia::http_capsule_event capsule) {
                            auto& target = *static_cast<ws_receive*>(raw);
                            if (!capsule.payload_.empty()) {
                                target.capsule_payload_.append(capsule.payload_.data(), capsule.payload_.size());
                            }
                            if (!capsule.end_capsule_) {
                                return;
                            }
                            const auto datagram = ruvia::decode_http_udp_datagram(std::span(target.capsule_payload_.data(), target.capsule_payload_.size()));
                            if (capsule.type_ != 0 || (datagram.index() != 0) || std::get<0>(datagram).context_id_ != 0 ||
                                !std::ranges::all_of(std::get<0>(datagram).payload_, [](char ch) { return ch == 't'; })) {
                                throw std::runtime_error("invalid CONNECT-UDP client packet");
                            }
                            target.tunnel_bytes_->fetch_add(std::get<0>(datagram).payload_.size(), std::memory_order_release);
                            target.capsule_payload_.clear();
                        };
                        const auto decoded = state_value.capsules_.feed(event.body_, false, collect, &state_value);
                        if (decoded != ruvia::http_capsule_status::need_more_data) {
                            throw std::runtime_error("invalid client capsule stream");
                        }
                        return;
                    }
                    if (state_value.tunnel_) {
                        if (!std::ranges::all_of(event.body_, [](char ch) { return ch == 't'; })) {
                            throw std::runtime_error("invalid CONNECT client payload");
                        }
                        state_value.tunnel_bytes_->fetch_add(event.body_.size(), std::memory_order_release);
                        return;
                    }
                    (void)state_value.websocket_->feed(std::string_view(event.body_.data(), event.body_.size()));
                    while (auto message_value = state_value.websocket_->next_event()) {
                        if (auto* payload_value = message_value->message()) {
                            if (payload_value->payload() != std::string(100000, 'c')) {
                                throw std::runtime_error("invalid WebSocket client payload");
                            }
                            ++state_value.messages_;
                        } else if (message_value->protocol_error()) {
                            throw std::runtime_error("invalid WebSocket client frame");
                        } else if (message_value->close() || message_value->transport_end()) {
                            break;
                        }
                    }
                }
            };

            bool handshake_gate_released = false;
            while (!stop_.load(std::memory_order_acquire)) {
                handshake_gate_released |= allow_handshake_.load(std::memory_order_acquire);
                const bool drop_handshake_packets =
                    hold_handshake_on_early_decision_ && !handshake_gate_released;
                pair.pump(drop_handshake_packets, drop_handshake_packets ? 1 : 32);
                if (!connection_id) {
                    connection_id = pair.maybe_connection_token();
                }
                if (connection_id) {
                    auto& server = pair.server();
                    const auto info = server.info();
                    if (info.early_data_ == ruvia::quic_early_data_state::rejected) {
                        early_data_rejected_.store(true, std::memory_order_release);
                    } else if (info.early_data_ == ruvia::quic_early_data_state::accepted) {
                        early_data_accepted_.store(true, std::memory_order_release);
                    }
                    if (hold_handshake_on_early_decision_ && !handshake_gate_released &&
                        !info.quic_handshake_complete_) {
                        handshake_paused_.store(true, std::memory_order_release);
                    }
                    if (info.quic_handshake_complete_ && info.state_ == ruvia::quic_connection_state::ready) {
                        handshake_observed_.store(true, std::memory_order_release);
                        for (std::size_t i = 0; i < local_critical_ids.size(); ++i) {
                            if (!local_critical_ids[i]) {
                                const auto opened = server.open_stream(true);
                                if (opened.status_ != ruvia::quic_operation_status::accepted) {
                                    throw std::runtime_error("HTTP/3 local peer critical stream open failed");
                                }
                                local_critical_ids[i] = opened.stream_id_;
                            }
                            auto& offset = local_critical_offsets[i];
                            if (offset < local_critical_bytes[i].size()) {
                                const auto written = write_quic_stream(server, *local_critical_ids[i],
                                    local_critical_bytes[i].subspan(offset));
                                if (written.status_ == ruvia::quic_operation_status::accepted) {
                                    offset += written.accepted_;
                                } else if (written.status_ != ruvia::quic_operation_status::would_block) {
                                    throw std::runtime_error("HTTP/3 local peer critical stream write failed");
                                }
                            }
                        }
                        auto accepted = server.accept_streams();
                        if (accepted.status_ != ruvia::quic_operation_status::accepted &&
                            accepted.status_ != ruvia::quic_operation_status::need_input &&
                            accepted.status_ != ruvia::quic_operation_status::would_block) {
                            throw std::runtime_error("HTTP/3 local peer stream acceptance failed");
                        }
                        for (std::size_t i = 0; i < accepted.size_; ++i) {
                            const auto& stream = accepted.streams_[i];
                            if (stream.readable_ && stream.writable_) {
                                request_stream = stream.stream_id_;
                            } else if (stream.readable_) {
                                peer_critical_streams.push_back(stream.stream_id_);
                            }
                        }
                        if (!final_fin_sent) {
                            for (const auto id : peer_critical_streams) {
                                const auto read = read_quic_stream(server, id, request_bytes);
                                if (read.status_ == ruvia::quic_stream_read_status::data) {
                                    const auto parsed_value = ws_request.feed(id, std::span<const char>(request_bytes.data(), read.size_), false, false, on_ws_event, &ws_receive_value);
                                    if (parsed_value.scope_ != ruvia::http3_connection_error_scope::none) {
                                        throw std::runtime_error("invalid WebSocket peer critical input");
                                    }
                                    if (const auto max_push_id = ws_request.peer_max_push_id()) {
                                        peer_max_push_id_.store(*max_push_id, std::memory_order_relaxed);
                                        peer_max_push_id_observed_.store(true, std::memory_order_release);
                                    }
                                } else if (read.status_ != ruvia::quic_stream_read_status::would_block && !final_fin_sent) {
                                    throw std::runtime_error("WebSocket peer critical transport failed");
                                }
                            }
                        }
                        if (request_stream && !request_finished && consume_request_) {
                            for (;;) {
                                const auto read = read_quic_stream(server, *request_stream, request_bytes);
                                if (read.status_ == ruvia::quic_stream_read_status::data) {
                                    request_payload_bytes_.fetch_add(read.size_, std::memory_order_release);
                                    if (websocket_ || push_ || tunnel_) {
                                        const auto parsed_value = ws_request.feed(*request_stream,
                                            std::span<const char>(request_bytes.data(), read.size_), false, false, on_ws_event, &ws_receive_value);
                                        if (parsed_value.status_ == ruvia::http3_connection_status::connection_error || parsed_value.status_ == ruvia::http3_connection_status::stream_error) {
                                            throw std::runtime_error("invalid HTTP/3 WebSocket test input");
                                        }
                                        first_prepared = ws_receive_value.head_ready_;
                                        if (first_prepared && !request_head_at) {
                                            request_head_at = std::chrono::steady_clock::now();
                                        }
                                        websocket_messages_.store(ws_receive_value.messages_, std::memory_order_release);
                                        if (ws_receive_value.messages_ == 2) {
                                            allow_final_part_.store(true, std::memory_order_release);
                                        }
                                    }
                                    continue;
                                }
                                if (read.status_ == ruvia::quic_stream_read_status::fin) {
                                    request_finished = true;
                                    client_end_observed_.store(true, std::memory_order_release);
                                } else if (tunnel_ && final_fin_sent && read.status_ == ruvia::quic_stream_read_status::reset &&
                                           read.peer_reset_error_code_ == static_cast<std::uint64_t>(ruvia::http3_connection_error_code::request_cancelled)) {
                                    request_finished = true;
                                } else if (read.status_ != ruvia::quic_stream_read_status::would_block) {
                                    throw std::runtime_error("HTTP/3 local peer request stream read failed");
                                }
                                break;
                            }
                        }
                        if (request_finished && !first_prepared) {
                            first_prepared = true;
                        }
                        if (websocket_ && first_prepared && !dynamic_head_prepared) {
                            ruvia::http_response response({.resource_ = &resource_});
                            response.header("sec-websocket-protocol", "chat");
                            response.header("sec-websocket-extensions", "permessage-deflate; server_no_context_takeover; client_no_context_takeover");
                            const auto head = ws_request.encode_response_head(*request_stream, response,
                                ruvia::plan_buffered_http_response_write(ruvia::http_known_method::connect, response));
                            if ((head.index() != 0)) {
                                throw std::runtime_error("failed to encode dynamic WebSocket response");
                            }
                            const auto old_header = ruvia::decode_http3_frame_header(first_part_);
                            if ((old_header.index() != 0)) {
                                throw std::runtime_error("invalid static WebSocket response fixture");
                            }
                            const auto prefix = test_http3_frame(1, std::get<0>(head).field_section_.field_section_);
                            first_part_.erase(first_part_.begin(), first_part_.begin() + static_cast<std::ptrdiff_t>(std::get<0>(old_header).encoded_bytes_ + std::get<0>(old_header).length_));
                            first_part_.insert(first_part_.begin(), prefix.begin(), prefix.end());
                            response_head_bytes = prefix.size();
                            dynamic_head_prepared = true;
                        }
                        if (push_ && request_finished && ws_request.peer_max_push_id() && next_push < push_->count_ && next_push <= *ws_request.peer_max_push_id()) {
                            const auto prepare_next_push = [&] {
                                std::optional<stream_id> opened_push;
                                if (!push_->promise_only_ && !push_->cancel_before_promise_) {
                                    const auto opened = server.open_stream(true);
                                    if (opened.status_ == ruvia::quic_operation_status::would_block || opened.status_ == ruvia::quic_operation_status::need_input) {
                                        return;
                                    }
                                    if (opened.status_ != ruvia::quic_operation_status::accepted) {
                                        throw std::runtime_error("push fixture stream open failed: " + std::to_string(static_cast<unsigned>(opened.status_)));
                                    }
                                    opened_push = opened.stream_id_;
                                }
                                const auto push_id = next_push++;
                                const std::string path = "/push/" + std::to_string(push_id);
                                const std::string authority = push_->cross_origin_ ? "other.test" : "127.0.0.1:" + std::to_string(port());
                                const std::array<ruvia::http_header_view, 1> headers{ruvia::http_header_view("x-promise", "owned")};
                                auto promise = ws_request.prepare_push_promise(*request_stream, push_id, {.authority_ = authority, .path_ = path, .headers_ = headers});
                                if ((promise.index() != 0)) {
                                    throw std::runtime_error("push fixture promise failed");
                                }
                                if (push_->cancel_before_promise_) {
                                    auto cancel = ws_request.prepare_cancel_push(push_id);
                                    // The critical initial prefix has already completed; use the stable critical output lane below.
                                    qpack_bytes[1].assign(std::get<0>(cancel).data(), std::get<0>(cancel).size());
                                    qpack_offsets[1] = 0;
                                }
                                if (push_->promise_only_ || push_->cancel_before_promise_) {
                                    promises.emplace_back(std::get<0>(promise).begin(), std::get<0>(promise).end());
                                } else {
                                    auto prefix = ws_request.prepare_push_stream(*opened_push, push_id);
                                    if ((prefix.index() != 0)) {
                                        throw std::runtime_error("push fixture stream prefix failed");
                                    }
                                    push_wire wire{*opened_push, std::vector<char>(std::get<0>(prefix).begin(), std::get<0>(prefix).end())};
                                    const std::string length = std::to_string(push_->body_bytes_ + (push_->malformed_response_ ? 1 : 0));
                                    const std::array<ruvia::http3_field_section_field_view, 2> fields_value{{{":status", "200"}, {"content-length", length}}};
                                    const auto encoded = ruvia::encode_http3_field_section(fields_value, &resource_);
                                    auto head = test_http3_frame(1, std::get<0>(encoded));
                                    wire.bytes_.insert(wire.bytes_.end(), head.begin(), head.end());
                                    const std::string content(push_->body_bytes_, 'p');
                                    auto body = test_http3_frame(0, content);
                                    wire.bytes_.insert(wire.bytes_.end(), body.begin(), body.end());
                                    if (push_->stream_before_promise_) {
                                        wire.promise_.assign(std::get<0>(promise).begin(), std::get<0>(promise).end());
                                        wire.promise_release_ = std::chrono::steady_clock::now() + 30ms;
                                    } else {
                                        promises.emplace_back(std::get<0>(promise).begin(), std::get<0>(promise).end());
                                    }
                                    push_streams.push_back(std::move(wire));
                                }
                                promised_pushes_.store(next_push, std::memory_order_release);
                            };
                            prepare_next_push();
                        }
                        for (auto it = push_streams.begin(); it != push_streams.end();) {
                            if (!it->promise_.empty() && std::chrono::steady_clock::now() >= it->promise_release_) {
                                promises.push_back(std::move(it->promise_));
                                it->promise_.clear();
                            }
                            bool ended = it->finished_;
                            if (it->offset_ < it->bytes_.size()) {
                                const auto written = write_quic_stream(server, it->stream_, std::span<const char>(it->bytes_).subspan(it->offset_));
                                if (written.status_ == ruvia::quic_operation_status::accepted) {
                                    it->offset_ += written.accepted_;
                                } else if (is_quic_stream_closed(written.status_)) {
                                    cancelled_pushes_.fetch_add(1, std::memory_order_release);
                                    ended = true;
                                } else if (written.status_ != ruvia::quic_operation_status::would_block) {
                                    throw std::runtime_error("push fixture response write failed");
                                }
                            }
                            if (it->offset_ == it->bytes_.size() && !it->finished_) {
                                const auto fin = server.finish_stream(it->stream_);
                                if (fin == ruvia::quic_operation_status::accepted) {
                                    ended = true;
                                    it->finished_ = true;
                                } else if (fin != ruvia::quic_operation_status::would_block && !is_quic_stream_closed(fin)) {
                                    throw std::runtime_error("push fixture FIN failed");
                                }
                            }
                            if (ended && it->finished_ && it->promise_.empty()) {
                                // FIN acceptance only queues it. Retire after submission;
                                // ngtcp2 retains the output until ACK without an abortive reset.
                                ended = server.retire_completed_stream(it->stream_) == ruvia::quic_operation_status::accepted;
                            }
                            if (ended && it->promise_.empty()) {
                                if (!it->finished_) {
                                    (void)server.close_stream(it->stream_);
                                }
                                it = push_streams.erase(it);
                            } else {
                                ++it;
                            }
                        }
                        if (push_ && !qpack_bytes[1].empty()) {
                            auto& bytes_value = qpack_bytes[1];
                            auto& offset = qpack_offsets[1];
                            const auto written = write_quic_stream(server, *local_critical_ids[0], std::span<const char>(bytes_value).subspan(offset));
                            if (written.status_ == ruvia::quic_operation_status::accepted) {
                                offset += written.accepted_;
                                if (offset == bytes_value.size()) {
                                    bytes_value.clear();
                                }
                            } else if (written.status_ != ruvia::quic_operation_status::would_block) {
                                throw std::runtime_error("push fixture cancellation failed");
                            }
                        }
                        if (push_ && !promises.empty() && (first_offset == 0 || first_offset == first_part_.size())) {
                            const auto written = write_quic_stream(server, *request_stream, std::span<const char>(promises.front()).subspan(promise_offset));
                            if (written.status_ == ruvia::quic_operation_status::accepted) {
                                promise_offset += written.accepted_;
                                if (promise_offset == promises.front().size()) {
                                    promises.pop_front();
                                    promise_offset = 0;
                                }
                            } else if (written.status_ != ruvia::quic_operation_status::would_block) {
                                throw std::runtime_error("push fixture promise write failed");
                            }
                        }
                        const bool push_prepared = !push_ || next_push != 0 || (request_head_at && std::chrono::steady_clock::now() >= *request_head_at + 100ms);
                        if (first_prepared && push_prepared && promises.empty() && first_offset < first_part_.size()) {
                            const auto offered = std::span<const char>(first_part_).subspan(first_offset, std::min(request_bytes.size(), first_part_.size() - first_offset));
                            const auto written = write_quic_stream(server, *request_stream, offered);
                            if (written.status_ == ruvia::quic_operation_status::accepted) {
                                first_offset += written.accepted_;
                                if (websocket_ && !encoder_release && first_offset >= response_head_bytes) {
                                    encoder_release = std::chrono::steady_clock::now() + 30ms;
                                }
                                if (first_offset == first_part_.size()) {
                                    first_part_ready_.store(true, std::memory_order_release);
                                }
                            } else if (written.status_ != ruvia::quic_operation_status::would_block) {
                                throw std::runtime_error("HTTP/3 local peer initial response write failed");
                            }
                        }
                        if (websocket_ && encoder_release && std::chrono::steady_clock::now() >= *encoder_release) {
                            for (std::size_t index = 0; index != qpack_bytes.size(); ++index) {
                                auto& queued = qpack_bytes[index];
                                auto& offset = qpack_offsets[index];
                                const auto pending = index == 0 ? ws_request.pending_qpack_encoder_output() : ws_request.pending_qpack_decoder_output();
                                if (queued.empty() && !pending.empty()) {
                                    queued.assign(pending.data(), pending.size());
                                    offset = 0;
                                }
                                if (queued.empty()) {
                                    continue;
                                }
                                const auto written = write_quic_stream(server, *local_critical_ids[index + 1], std::span<const char>(queued.data() + offset, queued.size() - offset));
                                if (written.status_ == ruvia::quic_operation_status::accepted) {
                                    const bool consumed = index == 0 ? ws_request.consume_qpack_encoder_output(written.accepted_) : ws_request.consume_qpack_decoder_output(written.accepted_);
                                    if (!consumed) {
                                        throw std::runtime_error("invalid WebSocket QPACK output credit");
                                    }
                                    offset += written.accepted_;
                                    if (offset == queued.size()) {
                                        queued.clear();
                                    }
                                } else if (written.status_ != ruvia::quic_operation_status::would_block) {
                                    throw std::runtime_error("WebSocket QPACK output failed");
                                }
                            }
                        }
                        if (first_part_ready() && promises.empty() && allow_final_part_.load(std::memory_order_acquire) &&
                            final_offset < final_part_.size()) {
                            const auto written = write_quic_stream(server, *request_stream,
                                std::span<const char>(final_part_).subspan(final_offset));
                            if (written.status_ == ruvia::quic_operation_status::accepted) {
                                final_offset += written.accepted_;
                            } else if (written.status_ != ruvia::quic_operation_status::would_block) {
                                throw std::runtime_error("HTTP/3 local peer final response write failed");
                            }
                        }
                        if (final_offset == final_part_.size() && !final_fin_sent) {
                            const auto finished = server.finish_stream(*request_stream);
                            if (finished == ruvia::quic_operation_status::accepted) {
                                final_fin_sent = true;
                                final_part_sent_.store(true, std::memory_order_release);
                            } else if (finished != ruvia::quic_operation_status::would_block) {
                                throw std::runtime_error("HTTP/3 local peer response FIN failed");
                            }
                        }
                    }
                }
                const bool drop_handshake_packets_at_end =
                    hold_handshake_on_early_decision_ && !handshake_gate_released;
                pair.pump(drop_handshake_packets_at_end,
                    drop_handshake_packets_at_end ? 1 : 32);
                {
                    std::lock_guard lock(mutex_);
                    synchronization_completed_ = synchronization_requested_;
                }
                started_condition_.notify_all();
                std::this_thread::sleep_for(1ms);
            }
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                failure_ = std::current_exception();
                started_ = true;
            }
            started_condition_.notify_all();
        }
    }

    std::atomic<std::uint16_t> port_{};
    std::pmr::unsynchronized_pool_resource resource_;
    std::unique_ptr<ruvia::testing::http3_quic_udp_pair> pair_;
    std::vector<char> first_part_;
    std::vector<char> final_part_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable started_condition_;
    std::exception_ptr failure_;
    bool started_{};
    std::uint64_t synchronization_requested_{};
    std::uint64_t synchronization_completed_{};
    std::atomic<bool> stop_{};
    std::atomic<bool> handshake_observed_{};
    std::atomic<bool> early_data_rejected_{};
    std::atomic<bool> early_data_accepted_{};
    std::atomic<bool> handshake_paused_{};
    std::atomic<bool> allow_handshake_{};
    std::atomic<std::size_t> request_payload_bytes_{};
    std::atomic<bool> first_part_ready_{};
    std::atomic<bool> allow_final_part_{};
    std::atomic<bool> final_part_sent_{};
    bool consume_request_{};
    bool websocket_{};
    bool advertise_origins_{};
    std::optional<quic_push_scenario> push_{};
    bool tunnel_{};
    bool udp_{};
    bool early_data_{};
    ruvia::detail::http3_quic_tls_context* shared_server_tls_{};
    bool hold_handshake_on_early_decision_{};
    std::atomic<std::size_t> tunnel_bytes_{};
    std::atomic<std::size_t> cancelled_pushes_{};
    std::atomic<std::size_t> promised_pushes_{};
    std::atomic<int> push_priority_observed_{-1};
    std::atomic<unsigned> websocket_messages_{};
    std::atomic<bool> client_end_observed_{};
    std::atomic<int> priority_observed_{-1};
    std::atomic<std::uint64_t> priority_element_id_{};
    std::atomic<bool> priority_element_id_observed_{};
    std::atomic<std::uint64_t> peer_max_push_id_{};
    std::atomic<bool> peer_max_push_id_observed_{};
};

template <typename predicate_type>
inline ruvia::task<bool> wait_for_peer(const ruvia::worker_handle& worker_value, local_quic_response_peer& peer,
    predicate_type predicate, std::chrono::milliseconds timeout) {
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

class connection_watchdog final {
    struct state_type final {
        connection_type* connection_{};
        bool expired_{};
    };

public:
    connection_watchdog(asio::io_context& io, connection_type& connection)
        : timer_(io),
          state_(std::make_shared<state_type>(state_type{&connection, false})) {
        timer_.expires_after(20s);
        timer_.async_wait([state = state_](const asio::error_code& error) noexcept {
            if (!error && state->connection_ != nullptr) {
                state->expired_ = true;
                state->connection_->request_stop();
            }
        });
    }

    ~connection_watchdog() {
        disarm();
    }

    connection_watchdog(const connection_watchdog&) = delete;
    connection_watchdog& operator=(const connection_watchdog&) = delete;

    void disarm() noexcept {
        state_->connection_ = nullptr;
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

inline ruvia::task<void> exercise_public_http3_client_pool(asio::io_context& io,
    const ruvia::worker_handle& worker_value, ruvia::event_loop_attachment& attachment,
    local_quic_response_peer& peer, ruvia::testing::test_context& ruvia_ctx,
    bool observe_origins = false, std::string_view ca_file = {}, bool migrate_quic_path = false) {
    counting_resource memory;
    std::exception_ptr failure;
    std::optional<ruvia::http_client_advertisement> retained_advertisement;
    std::optional<std::uint64_t> migration_id;
    {
        ruvia::http_client_config config{
            .scheme_ = ruvia::http_scheme::https,
            .host_ = "127.0.0.1",
            .port_ = peer.port(),
            .connection_count_ = 1,
            .connect_timeout_ = 10s,
            .request_timeout_ = 12s,
            .acquire_timeout_ = 2s,
            .max_response_bytes_ = 64,
            .protocol_ = ruvia::http_client_protocol::http3_only,
            .advertisements_ = {.receive_origins_ = observe_origins},
            .tls_peer_verification_ = observe_origins ? ruvia::tls_peer_verification_policy::verify : ruvia::tls_peer_verification_policy::skip_verification,
            .ca_file_ = std::string(ca_file),
            .user_agent_ = "Ruvia-H3-Test",
        };
        ruvia::detail::http_client_pool pool(io, worker_value,
            ruvia::detail::http_client_config_storage(config, &memory),
            ruvia::http_client_result_budget_config{}, &memory);
        try {
            {
                ruvia::detail::http_client_request_storage request("GET", "/public-pool", &memory);
                auto response = co_await pool.execute(std::move(request), {});
                RUVIA_CHECK(response.protocol_version() == ruvia::http_protocol_version::http3);
                RUVIA_CHECK_EQ(response.status().value(), 200);
                RUVIA_CHECK(peer.first_part_ready());
                RUVIA_CHECK(!peer.final_part_sent());

                if (observe_origins) {
                    RUVIA_CHECK(co_await wait_for_peer(worker_value, peer, [&] {
                        retained_advertisement = pool.next_advertisement();
                        return retained_advertisement && retained_advertisement->origins(); }, 2s));
                    if (retained_advertisement && retained_advertisement->origins()) {
                        RUVIA_CHECK(retained_advertisement->origins()->origins_.size() == 2);
                        RUVIA_CHECK(retained_advertisement->origins()->origins_[0] == "https://127.0.0.1");
                    }
                }

                response.reprioritize({.urgency_ = 1, .incremental_ = true});
                const bool priority_received = co_await wait_for_peer(worker_value, peer, [&] { return peer.priority_observed() == 0x101; }, 2s);
                RUVIA_CHECK(priority_received);

                auto first = co_await response.body().text();
                RUVIA_CHECK(first.has_value());
                if (first) {
                    RUVIA_CHECK_EQ(*first, std::string_view("abc"));
                }
                peer.allow_final_part();
                auto second = co_await response.body().text();
                RUVIA_CHECK(second.has_value());
                if (second) {
                    RUVIA_CHECK_EQ(*second, std::string_view("def"));
                }
                auto end = co_await response.body().text();
                RUVIA_CHECK(!end.has_value());
                RUVIA_CHECK(response.body().complete());
                bool retired_priority_rejected = false;
                try {
                    response.reprioritize({});
                } catch (const ruvia::http_client_error& error) {
                    retired_priority_rejected = error.code() == ruvia::http_client_error::code_type::closing;
                }
                RUVIA_CHECK(retired_priority_rejected);
            }
            const bool completed = co_await wait_for_peer(worker_value, peer, [&] { return pool.stats().completed_requests_ == 1; }, 2s);
            RUVIA_CHECK(completed);
            const auto stats = pool.stats();
            RUVIA_CHECK_EQ(stats.completed_requests_, std::size_t{1});
            RUVIA_CHECK_EQ(stats.failed_requests_, std::size_t{0});
            RUVIA_CHECK_EQ(stats.in_flight_requests_, std::size_t{0});
            if (migrate_quic_path) {
                asio::ip::udp::socket reservation(
                    io, {asio::ip::address_v4::loopback(), 0});
                const auto local_endpoint = reservation.local_endpoint();
                reservation.close();
                const auto migration = pool.start_quic_path_migration(local_endpoint);
                RUVIA_CHECK(migration.status_ == ruvia::quic_migration_status::started ||
                            migration.status_ == ruvia::quic_migration_status::validated);
                if (migration.status_ == ruvia::quic_migration_status::started ||
                    migration.status_ == ruvia::quic_migration_status::validated) {
                    migration_id = migration.id_;
                    const bool settled = co_await wait_for_peer(worker_value, peer, [&] {
                        const auto status = pool.path_migration(*migration_id);
                        return status && status->status_ != ruvia::quic_migration_status::started; }, 8s);
                    RUVIA_CHECK(settled);
                    const auto status = pool.path_migration(*migration_id);
                    RUVIA_CHECK(status.has_value());
                    if (status) {
                        RUVIA_CHECK(status->status_ == ruvia::quic_migration_status::validated);
                    }
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        pool.close_now();
        co_await pool.join();
        if (migration_id) {
            const auto retired = pool.path_migration(*migration_id);
            RUVIA_CHECK(retired.has_value());
            if (retired) {
                RUVIA_CHECK(retired->status_ == ruvia::quic_migration_status::validated);
            }
        }
    }
    RUVIA_CHECK_EQ(memory.allocations_, memory.returns_);
    RUVIA_CHECK_EQ(memory.live_bytes_, std::size_t{0});
    if (observe_origins && retained_advertisement && retained_advertisement->origins()) {
        RUVIA_CHECK(retained_advertisement->origins()->origins_[1] == "https://localhost");
        RUVIA_CHECK(retained_advertisement->protocol_version() == ruvia::http_protocol_version::http3);
    }
    retained_advertisement.reset();
    attachment.stop();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

}  // namespace http3_client_connection_test

using namespace http3_client_connection_test;  // NOLINT(google-build-using-namespace)
