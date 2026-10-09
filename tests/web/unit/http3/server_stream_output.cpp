#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/timer.h"
#include "ruvia/http/http3_buffered_response_cursor.h"
#include "ruvia/http/http3_client_request_head.h"
#include "ruvia/http/http3_client_response.h"
#include "ruvia/web/context.h"

#include "http3/http3_server_connection.h"
#include "http3/http3_server_stream_output.h"
#include "http3_quic_udp_pair.h"
#include "memory_resource_fixture.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "routing_fixture.h"
#include "server/http_server_options.h"
#include "test_harness.h"
#include "test_io_context.h"
#include "test_tls_crypto.h"

namespace {

using namespace ruvia::detail;
using output_type = http3_server_stream_output;
using buffer = http3_stream_buffer;
using borrowed_block_type = http3_stream_buffer::borrowed_block;

inline constexpr std::uint64_t epoch = 17;
inline constexpr std::uint64_t generation = 29;

class identity_files final {
public:
    identity_files() {
        std::random_device random;
        directory_ = std::filesystem::temp_directory_path() /
                     ("ruvia-http3-output-" + std::to_string(random()) + "-" +
                         std::to_string(random()));
        if (!std::filesystem::create_directory(directory_)) {
            throw std::runtime_error("failed to create temporary TLS directory");
        }

        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context_value(
            EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr), EVP_PKEY_CTX_free);
        EVP_PKEY* raw_key = nullptr;
        if (!context_value || EVP_PKEY_keygen_init(context_value.get()) <= 0 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(context_value.get(), 2048) <= 0 ||
            EVP_PKEY_generate(context_value.get(), &raw_key) <= 0) {
            throw std::runtime_error("failed to generate TLS key");
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
            throw std::runtime_error("failed to create self-signed test certificate");
        }

        certificate_file_ = directory_ / "cert.pem";
        private_key_file_ = directory_ / "key.pem";
        std::unique_ptr<BIO, decltype(&BIO_free)> certificate_bio(
            BIO_new_file(certificate_file_.string().c_str(), "w"), BIO_free);
        std::unique_ptr<BIO, decltype(&BIO_free)> key_bio(
            BIO_new_file(private_key_file_.string().c_str(), "w"), BIO_free);
        if (!certificate_bio || !key_bio ||
            PEM_write_bio_X509(certificate_bio.get(), certificate.get()) != 1 ||
            ruvia::test::write_tls_private_key(key_bio.get(), key.get()) != 1) {
            throw std::runtime_error("failed to write test certificate");
        }
    }

    ~identity_files() {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    [[nodiscard]] const std::filesystem::path& certificate_file() const noexcept {
        return certificate_file_;
    }
    [[nodiscard]] const std::filesystem::path& private_key_file() const noexcept {
        return private_key_file_;
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path certificate_file_;
    std::filesystem::path private_key_file_;
};

http_server_listener_definition::tls_type server_tls_config(const identity_files& files) {
    http_server_listener_definition::tls_type config;
    config.identity_.certificate_chain_file_ = files.certificate_file().string();
    config.identity_.private_key_file_ = files.private_key_file().string();
    return config;
}

class quic_pair final {
public:
    using stream_id_type = std::uint64_t;
    using read_status_type = ruvia::quic_stream_read_status;
    using read_result = ruvia::quic_stream_read_result;

    quic_pair()
        : files_(),
          pair_(std::make_unique<ruvia::testing::http3_quic_udp_pair>(
              server_tls_config(files_))) {}

    quic_pair(const quic_pair&) = delete;
    quic_pair& operator=(const quic_pair&) = delete;

    void connect() {
        if (!pair_->run_until([this] { return pair_->connected(); })) {
            throw std::runtime_error("QUIC test handshake deadline");
        }
    }

    [[nodiscard]] stream_id_type open_request_stream(bool finish_request = true,
        std::span<const char> request_bytes = {}) {
        request_streams_requested_ = true;
        const auto opened = pair_->client().open_stream(false);
        if (opened.status_ != ruvia::quic_operation_status::accepted) {
            throw std::runtime_error("failed to open client request stream");
        }
        constexpr std::array<char, 1> default_trigger{'x'};
        const auto trigger = request_bytes.empty() ? std::span<const char>(default_trigger) : request_bytes;
        const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        bool written = false;
        while (!written && std::chrono::steady_clock::now() < deadline_value) {
            const auto result_value = pair_->client().write_stream(opened.stream_id_, std::as_bytes(std::span(trigger)));
            if (result_value.status_ == ruvia::quic_operation_status::accepted &&
                result_value.accepted_ == trigger.size()) {
                written = true;
            } else if (result_value.status_ != ruvia::quic_operation_status::would_block &&
                       result_value.status_ != ruvia::quic_operation_status::need_input) {
                throw std::runtime_error("failed to write client request stream trigger");
            }
            if (!written) {
                pair_->pump();
            }
        }
        if (!written || (finish_request &&
                            pair_->client().finish_stream(opened.stream_id_) !=
                                ruvia::quic_operation_status::accepted)) {
            throw std::runtime_error("failed to finish client request stream");
        }
        while (!has_accepted_stream(opened.stream_id_) && std::chrono::steady_clock::now() < deadline_value) {
            pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!has_accepted_stream(opened.stream_id_)) {
            throw std::runtime_error("server did not accept client bidi stream");
        }
        if (finish_request) {
            const auto received_value = read_server_to_terminal(opened.stream_id_);
            if (received_value.status_ != read_status_type::fin ||
                received_value.bytes_ != std::string_view(trigger.data(), trigger.size())) {
                throw std::runtime_error("server did not drain the finished request stream");
            }
        }
        return opened.stream_id_;
    }

    struct server_receive_type final {
        std::string bytes_;
        read_status_type status_{read_status_type::would_block};
        std::optional<std::uint64_t> peer_reset_error_code_{};
    };

    [[nodiscard]] server_receive_type read_server_to_terminal(stream_id_type stream_id) {
        server_receive_type received;
        std::array<char, 256> buffer{};
        const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < deadline_value) {
            const auto read = pair_->server().read_stream(stream_id, std::as_writable_bytes(std::span(buffer)));
            if (read.status_ == read_status_type::data) {
                received.bytes_.append(buffer.data(), read.size_);
            } else if (read.status_ == read_status_type::fin || read.status_ == read_status_type::reset) {
                received.status_ = read.status_;
                received.peer_reset_error_code_ = read.peer_reset_error_code_;
                return received;
            } else if (read.status_ == read_status_type::would_block) {
                pair_->pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else {
                throw std::runtime_error("server request stream read failed");
            }
        }
        throw std::runtime_error("server request stream terminal watchdog");
    }

    void write_client_stream(stream_id_type stream_id, std::span<const char> bytes_value) {
        std::size_t offset{};
        const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (offset != bytes_value.size() && std::chrono::steady_clock::now() < deadline_value) {
            const auto write = pair_->client().write_stream(
                stream_id, std::as_bytes(bytes_value.subspan(offset)));
            if (write.status_ == ruvia::quic_operation_status::accepted &&
                write.accepted_ != 0 && write.accepted_ <= bytes_value.size() - offset) {
                offset += write.accepted_;
            } else if (write.status_ == ruvia::quic_operation_status::would_block ||
                       write.status_ == ruvia::quic_operation_status::need_input) {
                pair_->pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else {
                throw std::runtime_error("client request tail write failed");
            }
        }
        if (offset != bytes_value.size()) {
            throw std::runtime_error("client request tail write watchdog");
        }
    }

    void finish_client_stream(stream_id_type stream_id) {
        if (pair_->client().finish_stream(stream_id) != ruvia::quic_operation_status::accepted) {
            throw std::runtime_error("client request tail FIN failed");
        }
    }

    [[nodiscard]] ruvia::quic_connection& server() {
        return pair_->server();
    }
    [[nodiscard]] ruvia::quic_connection& client() noexcept {
        return pair_->client();
    }
    [[nodiscard]] const ruvia::quic_stream_metadata* accepted_stream(stream_id_type stream_id) const noexcept {
        for (std::size_t i = 0; i < accepted_count_; ++i) {
            if (accepted_[i].stream_id_ == stream_id) {
                return &accepted_[i];
            }
        }
        return nullptr;
    }

    void pump() {
        pair_->pump();
        if (!request_streams_requested_) {
            return;
        }
        const auto accepted = pair_->server().accept_streams();
        if (accepted.status_ != ruvia::quic_operation_status::accepted &&
            accepted.status_ != ruvia::quic_operation_status::need_input &&
            accepted.status_ != ruvia::quic_operation_status::would_block) {
            throw std::runtime_error("QUIC test server stream acceptance failed");
        }
        for (std::size_t i = 0; i < accepted.size_; ++i) {
            if (accepted_count_ >= accepted_.size()) {
                throw std::runtime_error("QUIC test accepted-stream fixture is full");
            }
            accepted_[accepted_count_++] = accepted.streams_[i];
        }
    }

private:
    [[nodiscard]] bool has_accepted_stream(stream_id_type stream_id) const noexcept {
        return accepted_stream(stream_id) != nullptr;
    }

    identity_files files_;
    std::unique_ptr<ruvia::testing::http3_quic_udp_pair> pair_;
    std::array<ruvia::quic_stream_metadata, 32> accepted_{};
    std::size_t accepted_count_{};
    bool request_streams_requested_{};
};

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_{};
    std::size_t deallocations_{};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        ++allocations_;
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        ++deallocations_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

[[nodiscard]] http3_stream_id message_id(std::uint64_t stream_id,
    std::uint64_t connection_epoch = epoch, std::uint64_t connection_generation = generation) noexcept {
    return {.epoch_ = connection_epoch, .connection_generation_ = connection_generation, .stream_id_ = stream_id};
}

[[nodiscard]] borrowed_block_type enqueue_block(buffer& buffer, http3_stream_id id,
    std::span<const char> bytes_value) {
    const auto input = std::as_bytes(bytes_value);
    const auto sent = buffer.try_send(id, input);
    if (sent != buffer::send_result::sent) {
        throw std::runtime_error("failed to enqueue HTTP/3 test block");
    }
    borrowed_block_type block;
    if (!buffer.try_receive(block)) {
        throw std::runtime_error("failed to receive HTTP/3 test block");
    }
    return block;
}

[[nodiscard]] std::size_t available_blocks(buffer& buffer) {
    std::vector<borrowed_block_type> retained;
    retained.reserve(buffer.block_capacity());
    const std::array<std::byte, 1> bytes_value{std::byte{0}};
    while (true) {
        const auto result_value = buffer.try_send(message_id(0), bytes_value);
        if (result_value == buffer::send_result::no_block) {
            return retained.size();
        }
        if (result_value != buffer::send_result::sent) {
            throw std::runtime_error("buffer credit probe could not publish");
        }
        borrowed_block_type block;
        if (!buffer.try_receive(block)) {
            throw std::runtime_error("buffer credit probe could not consume");
        }
        retained.push_back(std::move(block));
    }
}

[[nodiscard]] std::pmr::vector<char> encode_response(std::string_view body,
    std::pmr::memory_resource* resource) {
    ruvia::http_response response;
    response.body(body);
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    auto created = ruvia::http3_buffered_response_cursor::create(response, plan, resource);
    if ((created.index() != 0)) {
        throw std::runtime_error("failed to create HTTP/3 buffered response write cursor");
    }
    auto cursor_value = std::move(std::get<0>(created));
    std::pmr::vector<char> bytes(resource);
    for (std::size_t iteration = 0; iteration < 16; ++iteration) {
        const auto segment = cursor_value.next();
        if (segment.index() != 0) {
            throw std::runtime_error("failed to read HTTP/3 response segment");
        }
        if (std::get<0>(segment).empty()) {
            break;
        }
        bytes.insert(bytes.end(), std::get<0>(segment).begin(), std::get<0>(segment).end());
        if (cursor_value.acknowledge(std::get<0>(segment).size()).index() != 0) {
            throw std::runtime_error("failed to acknowledge HTTP/3 response segment");
        }
    }
    if (!cursor_value.fin_ready() || (cursor_value.acknowledge_fin(true).index() != 0) || !cursor_value.finished()) {
        throw std::runtime_error("HTTP/3 response cursor did not finish");
    }
    return bytes;
}

struct decoded_response final {
    std::uint16_t status_{};
    std::string body_;
    std::size_t final_heads_{};
    std::size_t message_ends_{};
};

void on_decoded_response(void* context_value, const ruvia::http3_client_response_event& event) {
    auto& response = *static_cast<decoded_response*>(context_value);
    if (event.kind_ == ruvia::http3_client_response_event_kind::final_head) {
        response.status_ = event.head_->status_;
        ++response.final_heads_;
    } else if (event.kind_ == ruvia::http3_client_response_event_kind::body) {
        response.body_.append(event.body_.data(), event.body_.size());
    } else if (event.kind_ == ruvia::http3_client_response_event_kind::message_end) {
        ++response.message_ends_;
    }
}

struct received_wire final {
    std::string bytes_;
    bool fin_{};
    std::optional<std::uint64_t> peer_reset_error_code_;
};

void read_available(quic_pair& pair, std::uint64_t stream_id, received_wire& received_value) {
    std::array<char, 2048> buffer{};
    for (unsigned attempt_value = 0; attempt_value < 16; ++attempt_value) {
        const auto read = pair.client().read_stream(stream_id, std::as_writable_bytes(std::span(buffer)));
        if (read.status_ == ruvia::quic_stream_read_status::data) {
            received_value.bytes_.append(buffer.data(), read.size_);
        } else if (read.status_ == ruvia::quic_stream_read_status::fin) {
            received_value.fin_ = true;
            return;
        } else if (read.status_ == ruvia::quic_stream_read_status::reset) {
            received_value.peer_reset_error_code_ = read.peer_reset_error_code_;
            return;
        } else if (read.status_ == ruvia::quic_stream_read_status::would_block) {
            return;
        } else {
            throw std::runtime_error("QUIC test response stream read failed");
        }
    }
}

[[nodiscard]] decoded_response decode_response(std::uint64_t stream_id,
    const received_wire& wire, std::pmr::memory_resource* resource) {
    ruvia::http3_client_response decoder(stream_id, ruvia::http_known_method::get, resource);
    decoded_response response;
    const auto result_value = decoder.feed(wire.bytes_, wire.fin_, false, on_decoded_response, &response);
    if (result_value.status_ != ruvia::http3_client_response_status::message_end) {
        throw std::runtime_error("QUIC client did not decode a complete HTTP/3 response");
    }
    return response;
}

[[nodiscard]] char pattern_byte(std::uint64_t offset) noexcept {
    return static_cast<char>((offset * 131U + (offset >> 8U) + 17U) & 0xffU);
}

void fill_pattern(std::span<char> output, std::uint64_t offset) noexcept {
    for (std::size_t i = 0; i < output.size(); ++i) {
        output[i] = pattern_byte(offset + i);
    }
}

[[nodiscard]] bool matches_pattern(std::string_view bytes_value, std::uint64_t offset) noexcept {
    for (std::size_t i = 0; i < bytes_value.size(); ++i) {
        if (bytes_value[i] != pattern_byte(offset + i)) {
            return false;
        }
    }
    return true;
}

struct pattern_supply final {
    std::uint64_t bytes_{};
    const char* last_address_{};
    std::size_t last_size_{};
};

[[nodiscard]] bool offer_pattern_block(output_type& output, buffer& buffer, std::uint64_t stream_id,
    pattern_supply& supply, std::uint64_t limit) {
    if (supply.bytes_ >= limit || output.queued_block_count() >= 2) {
        return false;
    }
    const auto info = output.stream_info(stream_id);
    if (info && info->queued_blocks_ != 0) {
        return false;
    }
    std::array<char, buffer::max_block_bytes> bytes_value{};
    const auto size = static_cast<std::size_t>(
        std::min<std::uint64_t>(bytes_value.size(), limit - supply.bytes_));
    fill_pattern(std::span<char>(bytes_value.data(), size), supply.bytes_);
    auto block = enqueue_block(buffer, message_id(stream_id), std::span<const char>(bytes_value.data(), size));
    supply.last_address_ = reinterpret_cast<const char*>(block.bytes().data());
    supply.last_size_ = block.bytes().size();
    const auto accepted = output.accept_data(block);
    if (accepted.status_ != output_type::status_type::accepted) {
        return false;
    }
    supply.bytes_ += size;
    return true;
}

}  // namespace

RUVIA_TEST(http3_server_stream_output_writes_fairly_and_client_decodes_response) {
    quic_pair pair;
    pair.connect();
    const auto first_id = pair.open_request_stream();
    const auto second_id = pair.open_request_stream();
    const auto first_wire = encode_response("first response", std::pmr::get_default_resource());
    const auto second_wire = encode_response("second response", std::pmr::get_default_resource());

    ruvia::worker_memory worker;
    buffer buffer(8, 8, 8);
    output_type output(pair.server(), worker, epoch, generation,
        {.max_tracked_streams_ = 8, .max_queued_blocks_ = 8, .max_drive_work_items_ = 1});

    auto foreign = enqueue_block(buffer, message_id(first_id, epoch + 1), first_wire);
    RUVIA_CHECK(output.accept_data(foreign).status_ == output_type::status_type::foreign_epoch);
    RUVIA_CHECK(foreign);
    foreign.release();
    RUVIA_CHECK_EQ(output.tracked_stream_count(), std::size_t{0});
    RUVIA_CHECK(pair.server().info().state_ != ruvia::quic_connection_state::retired);

    auto first_block = enqueue_block(buffer, message_id(first_id), first_wire);
    auto second_block = enqueue_block(buffer, message_id(second_id), second_wire);
    const http3_stream_control first_fin{.kind_ = http3_stream_control::kind::stream_fin,
        .id_ = message_id(first_id),
        .value_ = first_wire.size()};
    RUVIA_CHECK(output.accept_control(first_fin).status_ == output_type::status_type::fin_deferred);
    RUVIA_CHECK(output.accept_control(first_fin).status_ == output_type::status_type::duplicate_fin);
    RUVIA_CHECK(output.accept_data(first_block).status_ == output_type::status_type::accepted);
    RUVIA_CHECK(!first_block);
    RUVIA_CHECK(output.accept_data(second_block).status_ == output_type::status_type::accepted);
    RUVIA_CHECK(!second_block);
    RUVIA_CHECK(output.accept_control({.kind_ = http3_stream_control::kind::stream_fin,
                                          .id_ = message_id(second_id),
                                          .value_ = second_wire.size()})
                    .status_ == output_type::status_type::fin_deferred);

    received_wire first_received;
    received_wire second_received;
    std::array<bool, 2> attempted{};
    auto first_turn = output.drive();
    RUVIA_CHECK_EQ(first_turn.write_calls_, std::size_t{1});
    RUVIA_CHECK(first_turn.needs_reschedule_);
    if (first_turn.write_calls_ != 0) {
        attempted[first_turn.last_stream_id_ == first_id ? 0 : 1] = true;
    }
    pair.pump();
    output.notify_transport_activity();
    read_available(pair, first_id, first_received);
    read_available(pair, second_id, second_received);
    for (std::size_t turn_index = 0; turn_index < 128 && (!attempted[0] || !attempted[1]); ++turn_index) {
        const auto turn = output.drive();
        if (turn.write_calls_ != 0) {
            attempted[turn.last_stream_id_ == first_id ? 0 : 1] = true;
        }
        pair.pump();
        output.notify_transport_activity();
        read_available(pair, first_id, first_received);
        read_available(pair, second_id, second_received);
    }
    RUVIA_CHECK(attempted[0] && attempted[1]);

    const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline_value &&
           (!first_received.fin_ || !second_received.fin_)) {
        (void)output.drive();
        pair.pump();
        read_available(pair, first_id, first_received);
        read_available(pair, second_id, second_received);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(first_received.fin_ && second_received.fin_);
    output.notify_transport_activity();
    for (unsigned attempt_value = 0; attempt_value < 16; ++attempt_value) {
        (void)output.drive();
    }
    RUVIA_CHECK_EQ(output.queued_block_count(), std::size_t{0});
    RUVIA_CHECK_EQ(available_blocks(buffer), buffer.block_capacity() - output.queued_block_count());
    const auto first_info = output.stream_info(first_id);
    const auto second_info = output.stream_info(second_id);
    RUVIA_CHECK(first_info && first_info->state_ == output_type::stream_state_type::finished);
    RUVIA_CHECK(second_info && second_info->state_ == output_type::stream_state_type::finished);
    RUVIA_CHECK(first_info && first_info->send_fin_accepted_);
    RUVIA_CHECK(second_info && second_info->send_fin_accepted_);
    RUVIA_CHECK(first_info && first_info->accepted_wire_bytes_ == first_wire.size());
    RUVIA_CHECK(second_info && second_info->accepted_wire_bytes_ == second_wire.size());

    const auto decoded_first = decode_response(first_id, first_received, worker.resource());
    const auto decoded_second = decode_response(second_id, second_received, worker.resource());
    RUVIA_CHECK_EQ(decoded_first.status_, std::uint16_t{200});
    RUVIA_CHECK_EQ(decoded_first.body_, std::string("first response"));
    RUVIA_CHECK_EQ(decoded_first.final_heads_, std::size_t{1});
    RUVIA_CHECK_EQ(decoded_first.message_ends_, std::size_t{1});
    RUVIA_CHECK_EQ(decoded_second.status_, std::uint16_t{200});
    RUVIA_CHECK_EQ(decoded_second.body_, std::string("second response"));
    RUVIA_CHECK_EQ(decoded_second.final_heads_, std::size_t{1});
    RUVIA_CHECK_EQ(decoded_second.message_ends_, std::size_t{1});

    const auto conflict = output.accept_control({.kind_ = http3_stream_control::kind::stream_fin,
        .id_ = message_id(first_id),
        .value_ = first_wire.size() + 1});
    RUVIA_CHECK(conflict.status_ == output_type::status_type::final_size_error);
    RUVIA_CHECK(output.connection_retired());
    RUVIA_CHECK(output.stop().status_ == output_type::status_type::connection_closed);
    RUVIA_CHECK(buffer.stop());
}

RUVIA_TEST(http3_server_stream_output_idle_pump_does_not_request_continuation) {
    quic_pair pair;
    pair.connect();
    ruvia::worker_memory worker;
    output_type output(pair.server(), worker, epoch, generation,
        {.max_tracked_streams_ = 8, .max_drive_work_items_ = 2, .write_timeout_ = std::chrono::milliseconds(5)});
    for (unsigned turn = 0; turn < 100; ++turn) {
        const auto result_value = output.drive();
        RUVIA_CHECK_EQ(result_value.operations_, std::size_t{0});
        RUVIA_CHECK_EQ(result_value.scanned_slots_, std::size_t{0});
        RUVIA_CHECK(!result_value.needs_reschedule_);
    }
    RUVIA_CHECK(output.stop().status_ == output_type::status_type::stopped);
}

RUVIA_TEST(http3_server_stream_output_does_not_timeout_a_completed_tombstone) {
    quic_pair pair;
    pair.connect();
    const auto stream_id = pair.open_request_stream();
    ruvia::worker_memory worker;
    output_type output(pair.server(), worker, epoch, generation,
        {.write_timeout_ = std::chrono::milliseconds(5)});
    RUVIA_CHECK(output.accept_control({.kind_ = http3_stream_control::kind::stream_fin,
                                          .id_ = message_id(stream_id),
                                          .value_ = 0})
                    .status_ == output_type::status_type::accepted);
    for (unsigned attempt_value = 0; attempt_value < 16; ++attempt_value) {
        (void)output.drive();
        pair.pump();
        output.notify_transport_activity();
        const auto info = output.stream_info(stream_id);
        if (info && info->state_ == output_type::stream_state_type::finished) {
            break;
        }
    }
    const auto info = output.stream_info(stream_id);
    RUVIA_CHECK(info && info->state_ == output_type::stream_state_type::finished);
    RUVIA_CHECK_EQ(output.tracked_stream_count(), std::size_t{1});
    RUVIA_CHECK_EQ(output.live_stream_count(), std::size_t{0});
    RUVIA_CHECK_EQ(output.pending_stream_count(), std::size_t{0});
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto after_timeout = output.drive();
    RUVIA_CHECK_EQ(after_timeout.timed_out_streams_, std::size_t{0});
    RUVIA_CHECK(!output.connection_retired());
    RUVIA_CHECK(pair.server().info().state_ != ruvia::quic_connection_state::retired);
    RUVIA_CHECK(output.accept_control({.kind_ = http3_stream_control::kind::stream_fin,
                                          .id_ = message_id(stream_id),
                                          .value_ = 0})
                    .status_ == output_type::status_type::duplicate_fin);
    RUVIA_CHECK(output.stop().status_ == output_type::status_type::stopped);
}

RUVIA_TEST(http3_server_stream_output_does_not_timeout_a_deferred_fin_without_pending_bytes) {
    quic_pair pair;
    pair.connect();
    const auto stream_id = pair.open_request_stream(false);
    ruvia::worker_memory worker;
    output_type output(pair.server(), worker, epoch, generation,
        {.write_timeout_ = std::chrono::milliseconds(5)});
    RUVIA_CHECK(output.accept_control({.kind_ = http3_stream_control::kind::stream_fin,
                                          .id_ = message_id(stream_id),
                                          .value_ = 1})
                    .status_ == output_type::status_type::fin_deferred);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto idle = output.drive();
    RUVIA_CHECK_EQ(idle.timed_out_streams_, std::size_t{0});
    RUVIA_CHECK(!output.connection_retired());
    RUVIA_CHECK_EQ(output.pending_stream_count(), std::size_t{0});
    RUVIA_CHECK(output.stop().status_ == output_type::status_type::stopped);
}

RUVIA_TEST(http3_server_stream_output_keeps_receive_half_open_after_response_fin) {
    quic_pair pair;
    pair.connect();
    const auto stream_id = pair.open_request_stream(false);
    ruvia::worker_memory worker;
    buffer buffer(4, 4, 4);
    output_type output(pair.server(), worker, epoch, generation);
    const auto response_wire = encode_response("response before request end", worker.resource());
    auto block = enqueue_block(buffer, message_id(stream_id), response_wire);
    RUVIA_CHECK(output.accept_data(block).status_ == output_type::status_type::accepted);
    RUVIA_CHECK(!block);
    RUVIA_CHECK(output.accept_control({.kind_ = http3_stream_control::kind::stream_fin,
                                          .id_ = message_id(stream_id),
                                          .value_ = response_wire.size()})
                    .status_ == output_type::status_type::fin_deferred);

    received_wire response;
    const auto response_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!response.fin_ && std::chrono::steady_clock::now() < response_deadline) {
        (void)output.drive();
        pair.pump();
        output.notify_transport_activity();
        read_available(pair, stream_id, response);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(response.fin_);
    RUVIA_CHECK_EQ(response.bytes_.size(), response_wire.size());
    auto info = output.stream_info(stream_id);
    RUVIA_CHECK(info && info->send_fin_accepted_);
    RUVIA_CHECK(info && info->state_ == output_type::stream_state_type::fin_pending);
    RUVIA_CHECK(pair.server().retire_completed_stream(stream_id) ==
                ruvia::quic_operation_status::would_block);

    constexpr std::array<char, 9> tail{'-', 't', 'a', 'i', 'l', '-', 'o', 'k', '!'};
    pair.write_client_stream(stream_id, tail);
    pair.finish_client_stream(stream_id);
    const auto request = pair.read_server_to_terminal(stream_id);
    RUVIA_CHECK(request.status_ == ruvia::quic_stream_read_status::fin);
    RUVIA_CHECK_EQ(request.bytes_, std::string("x-tail-ok!"));
    output.notify_transport_activity();
    for (unsigned attempt_value = 0; attempt_value < 16; ++attempt_value) {
        (void)output.drive();
        info = output.stream_info(stream_id);
        if (info && info->state_ == output_type::stream_state_type::finished) {
            break;
        }
    }
    info = output.stream_info(stream_id);
    RUVIA_CHECK(info && info->state_ == output_type::stream_state_type::finished);
    RUVIA_CHECK_EQ(pair.server().read_health(stream_id).status_,
        ruvia::quic_stream_read_status::closed);
    RUVIA_CHECK(output.accept_control({.kind_ = http3_stream_control::kind::stream_fin,
                                          .id_ = message_id(stream_id),
                                          .value_ = response_wire.size()})
                    .status_ == output_type::status_type::duplicate_fin);
    const auto repeated = output.drive();
    RUVIA_CHECK_EQ(repeated.finished_streams_, std::size_t{0});
    RUVIA_CHECK(output.stop().status_ == output_type::status_type::stopped);
    RUVIA_CHECK(buffer.stop());
}

RUVIA_TEST(http3_server_stream_output_drains_buffered_request_before_normal_retirement) {
    quic_pair pair;
    pair.connect();
    const auto stream_id = pair.open_request_stream(false);
    pair.finish_client_stream(stream_id);
    pair.pump();

    ruvia::worker_memory worker;
    output_type output(pair.server(), worker, epoch, generation);
    RUVIA_CHECK(output.accept_control({.kind_ = http3_stream_control::kind::stream_fin,
                                          .id_ = message_id(stream_id),
                                          .value_ = 0})
                    .status_ == output_type::status_type::accepted);
    (void)output.drive();
    auto info = output.stream_info(stream_id);
    RUVIA_CHECK(info && info->send_fin_accepted_);
    RUVIA_CHECK(info && info->state_ == output_type::stream_state_type::fin_pending);
    RUVIA_CHECK(pair.server().retire_completed_stream(stream_id) ==
                ruvia::quic_operation_status::would_block);

    const auto request = pair.read_server_to_terminal(stream_id);
    RUVIA_CHECK(request.status_ == ruvia::quic_stream_read_status::fin);
    RUVIA_CHECK_EQ(request.bytes_, std::string("x"));
    pair.pump();
    output.notify_transport_activity();
    for (unsigned attempt_value = 0; attempt_value < 16; ++attempt_value) {
        (void)output.drive();
        info = output.stream_info(stream_id);
        if (info && info->state_ == output_type::stream_state_type::finished) {
            break;
        }
    }
    info = output.stream_info(stream_id);
    RUVIA_CHECK(info && info->state_ == output_type::stream_state_type::finished);
    RUVIA_CHECK(output.stop().status_ == output_type::status_type::stopped);
}

RUVIA_TEST(http3_server_stream_output_retires_after_peer_reset) {
    quic_pair pair;
    pair.connect();
    const auto stream_id = pair.open_request_stream(false);
    ruvia::worker_memory worker;
    output_type output(pair.server(), worker, epoch, generation);
    const auto fin = output.accept_control({.kind_ = http3_stream_control::kind::stream_fin,
        .id_ = message_id(stream_id),
        .value_ = 0});
    RUVIA_CHECK(fin.status_ == output_type::status_type::accepted);
    (void)output.drive();
    auto info = output.stream_info(stream_id);
    RUVIA_CHECK(info && info->send_fin_accepted_);
    RUVIA_CHECK(info && info->state_ == output_type::stream_state_type::fin_pending);

    received_wire response;
    const auto response_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!response.fin_ && std::chrono::steady_clock::now() < response_deadline) {
        pair.pump();
        read_available(pair, stream_id, response);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(response.fin_);
    constexpr auto reset_code = ruvia::http3_connection_error_code::message_error;
    RUVIA_CHECK(pair.client().reset_stream(stream_id, static_cast<std::uint64_t>(reset_code)) ==
                ruvia::quic_operation_status::accepted);
    const auto request = pair.read_server_to_terminal(stream_id);
    RUVIA_CHECK(request.status_ == ruvia::quic_stream_read_status::reset);
    RUVIA_CHECK(request.peer_reset_error_code_ == static_cast<std::uint64_t>(reset_code));
    output.notify_transport_activity();
    for (unsigned attempt_value = 0; attempt_value < 16; ++attempt_value) {
        (void)output.drive();
        info = output.stream_info(stream_id);
        if (info && info->state_ == output_type::stream_state_type::finished) {
            break;
        }
    }
    info = output.stream_info(stream_id);
    RUVIA_CHECK(info && info->state_ == output_type::stream_state_type::finished);
    RUVIA_CHECK_EQ(pair.server().read_health(stream_id).status_,
        ruvia::quic_stream_read_status::closed);
    RUVIA_CHECK(output.stop().status_ == output_type::status_type::stopped);
}

RUVIA_TEST(http3_server_stream_output_stop_force_closes_an_open_receive_half) {
    quic_pair pair;
    pair.connect();
    const auto stream_id = pair.open_request_stream(false);
    ruvia::worker_memory worker;
    output_type output(pair.server(), worker, epoch, generation);
    RUVIA_CHECK(output.accept_control({.kind_ = http3_stream_control::kind::stream_fin,
                                          .id_ = message_id(stream_id),
                                          .value_ = 0})
                    .status_ == output_type::status_type::accepted);
    (void)output.drive();
    auto info = output.stream_info(stream_id);
    RUVIA_CHECK(info && info->send_fin_accepted_);
    RUVIA_CHECK(info && info->state_ == output_type::stream_state_type::fin_pending);
    RUVIA_CHECK(output.stop().status_ == output_type::status_type::stopped);
    info = output.stream_info(stream_id);
    RUVIA_CHECK(info && info->state_ == output_type::stream_state_type::cancelled);
    RUVIA_CHECK(output.stop().status_ == output_type::status_type::stopped);
    RUVIA_CHECK_EQ(pair.server().read_health(stream_id).status_,
        ruvia::quic_stream_read_status::closed);
}

RUVIA_TEST(http3_server_stream_output_sends_typed_peer_reset_code) {
    quic_pair pair;
    pair.connect();
    const auto stream_id = pair.open_request_stream();
    ruvia::worker_memory worker;
    output_type output(pair.server(), worker, epoch, generation);
    constexpr auto reset_code = ruvia::http3_connection_error_code::message_error;

    const auto reset = output.accept_control({.kind_ = http3_stream_control::kind::stream_reset,
        .id_ = message_id(stream_id),
        .stream_reset_error_code_ = reset_code});
    RUVIA_CHECK(reset.status_ == output_type::status_type::reset);
    RUVIA_CHECK(reset.termination_.send_ == ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(reset.termination_.close_ == ruvia::quic_operation_status::accepted);

    ruvia::quic_stream_read_result peer_reset;
    std::array<char, 16> buffer{};
    const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline_value &&
           peer_reset.status_ != ruvia::quic_stream_read_status::reset) {
        pair.pump();
        peer_reset = pair.client().read_stream(stream_id, std::as_writable_bytes(std::span(buffer)));
        if (peer_reset.status_ == ruvia::quic_stream_read_status::would_block) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    RUVIA_CHECK(peer_reset.status_ == ruvia::quic_stream_read_status::reset);
    RUVIA_CHECK(peer_reset.peer_reset_error_code_.has_value());
    RUVIA_CHECK(peer_reset.peer_reset_error_code_ ==
                static_cast<std::uint64_t>(ruvia::http3_connection_error_code::message_error));
    const auto info = output.stream_info(stream_id);
    RUVIA_CHECK(info && info->state_ == output_type::stream_state_type::reset);
    RUVIA_CHECK(output.stop().status_ == output_type::status_type::stopped);
}

RUVIA_TEST(http3_server_stream_output_cancellation_terminates_unfinished_bidirectional_stream) {
    quic_pair pair;
    pair.connect();
    const auto stream_id = pair.open_request_stream(false);
    ruvia::worker_memory worker;
    output_type output(pair.server(), worker, epoch, generation);
    constexpr auto cancel_code = ruvia::http3_connection_error_code::request_cancelled;
    const auto cancelled = output.cancel_stream(stream_id,
        static_cast<std::uint64_t>(cancel_code));
    RUVIA_CHECK(cancelled.status_ == output_type::status_type::cancelled);
    RUVIA_CHECK(cancelled.termination_.send_ == ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(cancelled.termination_.close_ == ruvia::quic_operation_status::accepted);

    bool peer_received_reset{};
    bool peer_send_stopped{};
    std::uint64_t peer_reset_code{};
    std::array<char, 16> read_buffer{};
    constexpr std::array<char, 0> empty_write{};
    const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline_value &&
           (!peer_received_reset || !peer_send_stopped)) {
        pair.pump();
        if (!peer_received_reset) {
            const auto read = pair.client().read_stream(stream_id, std::as_writable_bytes(std::span(read_buffer)));
            if (read.status_ == ruvia::quic_stream_read_status::reset) {
                peer_received_reset = true;
                peer_reset_code = read.peer_reset_error_code_.value_or(0);
            }
        }
        if (!peer_send_stopped) {
            const auto write = pair.client().write_stream(stream_id, std::as_bytes(std::span(empty_write)));
            peer_send_stopped =
                write.status_ == ruvia::quic_operation_status::stream_closed;
        }
        if (!peer_received_reset || !peer_send_stopped) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    RUVIA_CHECK(peer_received_reset);
    RUVIA_CHECK_EQ(peer_reset_code, static_cast<std::uint64_t>(cancel_code));
    RUVIA_CHECK(peer_send_stopped);
    RUVIA_CHECK(output.stop().status_ == output_type::status_type::stopped);
}

RUVIA_TEST(http3_peer_response_cancel_releases_only_its_blocks_and_preserves_sibling_response) {
    quic_pair pair;
    pair.connect();
    const auto cancelled_id = pair.open_request_stream();
    const auto sibling_id = pair.open_request_stream();
    std::array<std::byte, 32> request_bytes{};
    const auto request_deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    for (const auto stream_id : {cancelled_id, sibling_id}) {
        bool request_finished{};
        while (!request_finished && std::chrono::steady_clock::now() < request_deadline_value) {
            pair.pump();
            const auto result_value = pair.server().read_stream(stream_id, request_bytes);
            request_finished = result_value.status_ == ruvia::quic_stream_read_status::fin;
            if (!request_finished && result_value.status_ != ruvia::quic_stream_read_status::data &&
                result_value.status_ != ruvia::quic_stream_read_status::would_block) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        RUVIA_CHECK(request_finished);
    }
    buffer buffer(4, 2, 2);
    ruvia::worker_memory worker;
    output_type output(pair.server(), worker, epoch, generation,
        {.max_tracked_streams_ = 4, .max_queued_blocks_ = 2});
    const std::array<char, 4> cancelled_bytes{'d', 'r', 'o', 'p'};
    const std::array<char, 4> sibling_bytes{'s', 'a', 'f', 'e'};
    auto cancelled_block = enqueue_block(buffer, message_id(cancelled_id), cancelled_bytes);
    auto sibling_block = enqueue_block(buffer, message_id(sibling_id), sibling_bytes);
    RUVIA_CHECK(output.accept_data(cancelled_block).status_ == output_type::status_type::accepted);
    RUVIA_CHECK(output.accept_data(sibling_block).status_ == output_type::status_type::accepted);
    RUVIA_CHECK(output.accept_control(
                          {.kind_ = http3_stream_control::kind::stream_fin,
                              .id_ = message_id(cancelled_id),
                              .value_ = cancelled_bytes.size()})
                    .status_ == output_type::status_type::fin_deferred);
    RUVIA_CHECK(output.accept_control(
                          {.kind_ = http3_stream_control::kind::stream_fin,
                              .id_ = message_id(sibling_id),
                              .value_ = sibling_bytes.size()})
                    .status_ == output_type::status_type::fin_deferred);
    RUVIA_CHECK(pair.client().stop_sending(cancelled_id,
                    static_cast<std::uint64_t>(ruvia::http3_connection_error_code::request_cancelled)) ==
                ruvia::quic_operation_status::accepted);

    const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (pair.server().write_health(cancelled_id) == ruvia::quic_operation_status::accepted &&
           std::chrono::steady_clock::now() < deadline_value) {
        pair.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(pair.server().write_health(cancelled_id) != ruvia::quic_operation_status::accepted);
    bool connection_failure{};
    received_wire received;
    while ((output.stream_info(cancelled_id)->state_ != output_type::stream_state_type::cancelled ||
               !received.fin_) &&
           std::chrono::steady_clock::now() < deadline_value) {
        const auto result_value = output.drive();
        connection_failure = result_value.status_ == output_type::status_type::transport_error ||
                             result_value.status_ == output_type::status_type::unsafe_to_release ||
                             result_value.status_ == output_type::status_type::connection_closed;
        if (connection_failure) {
            break;
        }
        pair.pump();
        read_available(pair, sibling_id, received);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(!connection_failure);
    RUVIA_CHECK(output.stream_info(cancelled_id)->state_ == output_type::stream_state_type::cancelled);
    RUVIA_CHECK(received.bytes_ == std::string_view(sibling_bytes.data(), sibling_bytes.size()));
    RUVIA_CHECK(received.fin_);
    RUVIA_CHECK(output.stop().status_ != output_type::status_type::unsafe_to_release);
    RUVIA_CHECK(available_blocks(buffer) == buffer.block_capacity());
}

RUVIA_TEST(http3_server_stream_output_backpressure_cancel_stop_and_pmr_lifetime) {
    quic_pair pair;
    pair.connect();
    const auto first_id = pair.open_request_stream();
    const auto second_id = pair.open_request_stream();
    counting_resource upstream;
    {
        ruvia::worker_memory worker(upstream);
        buffer buffer(4, 4, 4);
        {
            output_type output(pair.server(), worker, epoch, generation,
                {.max_tracked_streams_ = 4, .max_queued_blocks_ = 1, .max_drive_work_items_ = 2});
            constexpr std::array<char, 4> bytes_value{'d', 'a', 't', 'a'};
            auto queued = enqueue_block(buffer, message_id(first_id), bytes_value);
            RUVIA_CHECK(output.accept_data(queued).status_ == output_type::status_type::accepted);
            RUVIA_CHECK(!queued);
            auto backpressured = enqueue_block(buffer, message_id(first_id), bytes_value);
            RUVIA_CHECK(output.accept_data(backpressured).status_ == output_type::status_type::backpressured);
            RUVIA_CHECK(backpressured);
            RUVIA_CHECK_EQ(output.queued_block_count(), std::size_t{1});

            const auto cancelled = output.cancel_stream(first_id);
            RUVIA_CHECK(cancelled.status_ == output_type::status_type::cancelled);
            RUVIA_CHECK(cancelled.termination_.send_ == ruvia::quic_operation_status::accepted);
            RUVIA_CHECK(cancelled.termination_.close_ == ruvia::quic_operation_status::accepted);
            RUVIA_CHECK_EQ(output.queued_block_count(), std::size_t{0});
            RUVIA_CHECK(!output.stream_info(first_id)->queued_blocks_);
            RUVIA_CHECK(backpressured);
            RUVIA_CHECK_EQ(available_blocks(buffer), buffer.block_capacity() - output.queued_block_count() - 1);
            backpressured.release();
            RUVIA_CHECK_EQ(available_blocks(buffer), buffer.block_capacity() - output.queued_block_count());

            auto stop_block = enqueue_block(buffer, message_id(second_id), bytes_value);
            RUVIA_CHECK(output.accept_data(stop_block).status_ == output_type::status_type::accepted);
            RUVIA_CHECK(!stop_block);
            RUVIA_CHECK(output.stop().status_ == output_type::status_type::stopped);
            RUVIA_CHECK(output.stopped());
            RUVIA_CHECK_EQ(output.queued_block_count(), std::size_t{0});
            RUVIA_CHECK(output.stop().status_ == output_type::status_type::stopped);
            RUVIA_CHECK_EQ(available_blocks(buffer), buffer.block_capacity() - output.queued_block_count());
        }
        RUVIA_CHECK(buffer.stop());
    }
    RUVIA_CHECK_EQ(upstream.allocations_, upstream.deallocations_);
}

RUVIA_TEST(http3_server_stream_output_times_out_only_the_flow_controlled_stream) {
    quic_pair pair;
    pair.connect();
    const auto blocked_id = pair.open_request_stream();
    const auto sibling_id = pair.open_request_stream();
    counting_resource upstream;
    {
        ruvia::worker_memory worker(upstream);
        buffer buffer(4, 4, 4);
        {
            constexpr auto write_timeout = std::chrono::milliseconds(100);
            output_type output(pair.server(), worker, epoch, generation,
                {.max_tracked_streams_ = 4,
                    .max_queued_blocks_ = 1,
                    .max_drive_work_items_ = 16,
                    .write_timeout_ = write_timeout});
            constexpr std::uint64_t max_bytes = 64U * 1024U * 1024U;
            constexpr std::size_t max_steps = 8192;
            const auto blocked_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            pattern_supply supply;
            bool would_block{};
            for (std::size_t step = 0; step < max_steps &&
                                       std::chrono::steady_clock::now() < blocked_deadline && !would_block &&
                                       supply.bytes_ < max_bytes;
                ++step) {
                if (output.queued_block_count() == 0) {
                    (void)offer_pattern_block(output, buffer, blocked_id, supply, max_bytes);
                }
                const auto turn = output.drive();
                would_block = turn.would_block_writes_ != 0 && turn.last_stream_id_ == blocked_id;
                if (!would_block) {
                    pair.pump();
                    output.notify_transport_activity();
                }
            }
            RUVIA_CHECK(would_block);
            RUVIA_CHECK(supply.last_address_ != nullptr && supply.last_size_ != 0);
            RUVIA_CHECK_EQ(output.live_stream_count(), std::size_t{1});
            RUVIA_CHECK_EQ(output.pending_stream_count(), std::size_t{1});
            RUVIA_CHECK_EQ(output.queued_block_count(), std::size_t{1});
            RUVIA_CHECK_EQ(available_blocks(buffer), buffer.block_capacity() - output.queued_block_count());

            // The WANT retry still borrows the exact queued block; no replacement or
            // return is allowed before stream cancellation retires its SSL wrapper.
            const auto borrowed_address = supply.last_address_;
            const auto borrowed_size = supply.last_size_;
            auto settled = output.drive();
            for (std::size_t retry = 0; settled.needs_reschedule_ && retry < 32; ++retry) {
                settled = output.drive();
            }
            const auto before_retry = output.stream_info(blocked_id);
            RUVIA_CHECK(before_retry.has_value());
            const auto accepted_before_retry = before_retry ? before_retry->accepted_wire_bytes_ : 0;
            output.notify_transport_activity();
            const auto retry = output.drive();
            RUVIA_CHECK_EQ(retry.would_block_writes_, std::size_t{1});
            RUVIA_CHECK_EQ(supply.last_address_, borrowed_address);
            RUVIA_CHECK_EQ(supply.last_size_, borrowed_size);
            const auto after_retry = output.stream_info(blocked_id);
            RUVIA_CHECK(after_retry.has_value());
            RUVIA_CHECK(after_retry && after_retry->accepted_wire_bytes_ == accepted_before_retry);
            RUVIA_CHECK_EQ(output.queued_block_count(), std::size_t{1});
            RUVIA_CHECK_EQ(available_blocks(buffer), buffer.block_capacity() - output.queued_block_count());

            std::this_thread::sleep_for(write_timeout + std::chrono::milliseconds(10));
            std::size_t timed_out{};
            for (std::size_t attempt_value = 0; attempt_value < 32 && timed_out == 0; ++attempt_value) {
                timed_out += output.drive().timed_out_streams_;
            }
            RUVIA_CHECK_EQ(timed_out, std::size_t{1});
            const auto timed_out_info = output.stream_info(blocked_id);
            RUVIA_CHECK(timed_out_info && timed_out_info->state_ == output_type::stream_state_type::cancelled);
            RUVIA_CHECK_EQ(output.queued_block_count(), std::size_t{0});
            RUVIA_CHECK_EQ(available_blocks(buffer), buffer.block_capacity() - output.queued_block_count());
            RUVIA_CHECK(!output.connection_retired());
            RUVIA_CHECK(pair.server().info().state_ != ruvia::quic_connection_state::retired);

            received_wire timed_out_peer;
            const auto reset_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!timed_out_peer.peer_reset_error_code_ &&
                   std::chrono::steady_clock::now() < reset_deadline) {
                pair.pump();
                read_available(pair, blocked_id, timed_out_peer);
                if (!timed_out_peer.peer_reset_error_code_) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            RUVIA_CHECK(timed_out_peer.peer_reset_error_code_.has_value());
            RUVIA_CHECK(timed_out_peer.peer_reset_error_code_ ==
                        static_cast<std::uint64_t>(ruvia::http3_connection_error_code::request_cancelled));

            const auto sibling_wire = encode_response("sibling survives timeout", worker.resource());
            auto sibling_block = enqueue_block(
                buffer, message_id(sibling_id), sibling_wire);
            RUVIA_CHECK(output.accept_data(sibling_block).status_ == output_type::status_type::accepted);
            RUVIA_CHECK(!sibling_block);
            RUVIA_CHECK(output.accept_control({.kind_ = http3_stream_control::kind::stream_fin,
                                                  .id_ = message_id(sibling_id),
                                                  .value_ = sibling_wire.size()})
                            .status_ == output_type::status_type::fin_deferred);

            const auto late_data_bytes = std::as_bytes(std::span<const char>("late", 4));
            const auto late_sent = buffer.try_send(message_id(blocked_id), late_data_bytes);
            RUVIA_CHECK(late_sent == buffer::send_result::sent);
            borrowed_block_type late_block;
            RUVIA_CHECK(buffer.try_receive(late_block));
            RUVIA_CHECK(output.accept_data(late_block).status_ == output_type::status_type::closed_stream);
            RUVIA_CHECK(late_block);
            late_block.release();
            RUVIA_CHECK_EQ(available_blocks(buffer), buffer.block_capacity() - output.queued_block_count());
            RUVIA_CHECK(output.accept_control({.kind_ = http3_stream_control::kind::stream_fin,
                                                  .id_ = message_id(blocked_id),
                                                  .value_ = supply.bytes_})
                            .status_ == output_type::status_type::closed_stream);

            received_wire sibling_received;
            const auto response_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < response_deadline && !sibling_received.fin_) {
                const auto turn = output.drive();
                timed_out += turn.timed_out_streams_;
                pair.pump();
                output.notify_transport_activity();
                read_available(pair, sibling_id, sibling_received);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            output.notify_transport_activity();
            for (unsigned attempt_value = 0; attempt_value < 16; ++attempt_value) {
                (void)output.drive();
            }
            const auto blocked_info = output.stream_info(blocked_id);
            const auto sibling_info = output.stream_info(sibling_id);
            RUVIA_CHECK_EQ(timed_out, std::size_t{1});
            RUVIA_CHECK(blocked_info && blocked_info->state_ == output_type::stream_state_type::cancelled);
            RUVIA_CHECK(blocked_info &&
                        blocked_info->termination_.send_ == ruvia::quic_operation_status::accepted);
            RUVIA_CHECK(sibling_info && sibling_info->state_ == output_type::stream_state_type::finished);
            RUVIA_CHECK(sibling_received.fin_);
            RUVIA_CHECK_EQ(sibling_received.bytes_.size(), sibling_wire.size());
            RUVIA_CHECK(!output.connection_retired());
            RUVIA_CHECK(pair.server().info().state_ != ruvia::quic_connection_state::retired);
            RUVIA_CHECK_EQ(output.tracked_stream_count(), std::size_t{2});
            RUVIA_CHECK_EQ(output.live_stream_count(), std::size_t{0});
            RUVIA_CHECK_EQ(output.pending_stream_count(), std::size_t{0});
            RUVIA_CHECK_EQ(output.queued_block_count(), std::size_t{0});
            RUVIA_CHECK_EQ(available_blocks(buffer), buffer.block_capacity() - output.queued_block_count());

            const auto fresh_id = pair.open_request_stream();
            std::this_thread::sleep_for(write_timeout + std::chrono::milliseconds(10));
            const auto idle_fresh = output.drive();
            RUVIA_CHECK_EQ(idle_fresh.timed_out_streams_, std::size_t{0});
            // Opening a QUIC stream does not register it with the response writer.
            // An idle, unregistered stream must not inherit the old stream's deadline.
            RUVIA_CHECK(!output.stream_info(fresh_id));
            const auto fresh_wire = encode_response("fresh write gets a fresh deadline", worker.resource());
            auto fresh_block = enqueue_block(buffer, message_id(fresh_id), fresh_wire);
            RUVIA_CHECK(output.accept_data(fresh_block).status_ == output_type::status_type::accepted);
            RUVIA_CHECK(!fresh_block);
            RUVIA_CHECK(output.stream_info(fresh_id)->state_ == output_type::stream_state_type::open);
            RUVIA_CHECK(output.accept_control({.kind_ = http3_stream_control::kind::stream_fin,
                                                  .id_ = message_id(fresh_id),
                                                  .value_ = fresh_wire.size()})
                            .status_ == output_type::status_type::fin_deferred);
            received_wire fresh_received;
            const auto fresh_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < fresh_deadline && !fresh_received.fin_) {
                const auto turn = output.drive();
                timed_out += turn.timed_out_streams_;
                pair.pump();
                output.notify_transport_activity();
                read_available(pair, fresh_id, fresh_received);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            RUVIA_CHECK(fresh_received.fin_);
            output.notify_transport_activity();
            for (unsigned attempt_value = 0; attempt_value < 16; ++attempt_value) {
                (void)output.drive();
            }
            RUVIA_CHECK_EQ(fresh_received.bytes_.size(), fresh_wire.size());
            RUVIA_CHECK_EQ(timed_out, std::size_t{1});
            RUVIA_CHECK(output.stream_info(fresh_id)->state_ == output_type::stream_state_type::finished);
            RUVIA_CHECK(output.stop().status_ == output_type::status_type::stopped);
        }
        RUVIA_CHECK(buffer.stop());
    }
    RUVIA_CHECK_EQ(upstream.allocations_, upstream.deallocations_);
}

RUVIA_TEST(http3_server_stream_output_keeps_sibling_alive_after_external_stream_close) {
    quic_pair pair;
    pair.connect();
    const auto closed_id = pair.open_request_stream();
    const auto sibling_id = pair.open_request_stream();
    ruvia::worker_memory worker;
    buffer buffer(4, 4, 4);
    output_type output(pair.server(), worker, epoch, generation,
        {.max_tracked_streams_ = 4, .max_queued_blocks_ = 2, .max_drive_work_items_ = 1});
    constexpr std::array<char, 4> bytes_value{'d', 'a', 't', 'a'};

    auto closed_block = enqueue_block(buffer, message_id(closed_id), bytes_value);
    RUVIA_CHECK(output.accept_data(closed_block).status_ == output_type::status_type::accepted);
    RUVIA_CHECK(!closed_block);
    RUVIA_CHECK(pair.server().close_stream(closed_id) ==
                ruvia::quic_operation_status::accepted);
    const auto cancelled = output.cancel_stream(closed_id);
    RUVIA_CHECK(cancelled.status_ == output_type::status_type::cancelled);
    RUVIA_CHECK(cancelled.termination_.close_ == ruvia::quic_operation_status::retired);
    RUVIA_CHECK_EQ(output.queued_block_count(), std::size_t{0});
    RUVIA_CHECK_EQ(available_blocks(buffer), buffer.block_capacity() - output.queued_block_count());
    RUVIA_CHECK(pair.server().info().state_ != ruvia::quic_connection_state::retired);

    auto sibling_block = enqueue_block(buffer, message_id(sibling_id), bytes_value);
    RUVIA_CHECK(output.accept_data(sibling_block).status_ == output_type::status_type::accepted);
    RUVIA_CHECK(!sibling_block);
    const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline_value &&
           output.stream_info(sibling_id)->accepted_wire_bytes_ == 0) {
        (void)output.drive();
        pair.pump();
        output.notify_transport_activity();
    }
    const auto sibling_info = output.stream_info(sibling_id);
    RUVIA_CHECK(sibling_info && sibling_info->accepted_wire_bytes_ == bytes_value.size());
    RUVIA_CHECK(pair.server().info().state_ != ruvia::quic_connection_state::retired);
    RUVIA_CHECK(!output.connection_retired());
    RUVIA_CHECK(output.cancel_stream(sibling_id).status_ == output_type::status_type::cancelled);
    RUVIA_CHECK_EQ(output.queued_block_count(), std::size_t{0});
    RUVIA_CHECK_EQ(available_blocks(buffer), buffer.block_capacity() - output.queued_block_count());
    RUVIA_CHECK(output.stop().status_ == output_type::status_type::stopped);
    RUVIA_CHECK(buffer.stop());
}

RUVIA_TEST(http3_server_stream_output_parks_after_bounded_would_block_scans_and_cancels_safely) {
    quic_pair pair;
    pair.connect();
    const auto first_id = pair.open_request_stream();
    const auto second_id = pair.open_request_stream();
    ruvia::worker_memory worker;
    buffer buffer(2, 2, 4);
    output_type output(pair.server(), worker, epoch, generation,
        {.max_tracked_streams_ = 4, .max_queued_blocks_ = 2, .max_drive_work_items_ = 1});
    constexpr std::uint64_t max_supplied_bytes = 64U * 1024U * 1024U;
    constexpr std::size_t max_steps = 8192;
    const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    pattern_supply first_supply;
    pattern_supply second_supply;
    std::size_t steps{};
    bool first_would_block{};
    bool second_would_block{};

    while (steps < max_steps && std::chrono::steady_clock::now() < deadline_value &&
           !first_would_block && first_supply.bytes_ < max_supplied_bytes) {
        if (output.queued_block_count() == 0) {
            (void)offer_pattern_block(output, buffer, first_id, first_supply, max_supplied_bytes);
        }
        const auto turn = output.drive();
        first_would_block = turn.would_block_writes_ != 0 && turn.last_stream_id_ == first_id;
        if (!first_would_block) {
            pair.pump();
            output.notify_transport_activity();
        }
        ++steps;
    }

    while (steps < max_steps && std::chrono::steady_clock::now() < deadline_value &&
           first_would_block && !second_would_block &&
           first_supply.bytes_ + second_supply.bytes_ < max_supplied_bytes) {
        if (output.queued_block_count() < 2) {
            (void)offer_pattern_block(output, buffer, second_id, second_supply,
                max_supplied_bytes - first_supply.bytes_);
        }
        const auto turn = output.drive();
        second_would_block = turn.would_block_writes_ != 0 && turn.last_stream_id_ == second_id;
        if (!second_would_block) {
            pair.pump();
            output.notify_transport_activity();
        }
        ++steps;
    }

    RUVIA_CHECK(first_would_block);
    RUVIA_CHECK(second_would_block);
    const auto first_info = output.stream_info(first_id);
    const auto second_info = output.stream_info(second_id);
    RUVIA_CHECK(first_info && first_info->last_write_status_ == ruvia::quic_operation_status::would_block);
    RUVIA_CHECK(second_info && second_info->last_write_status_ == ruvia::quic_operation_status::would_block);
    RUVIA_CHECK(first_supply.last_address_ != nullptr && first_supply.last_size_ != 0);
    RUVIA_CHECK(second_supply.last_address_ != nullptr && second_supply.last_size_ != 0);
    RUVIA_CHECK(first_info && first_info->queued_blocks_ == 1);
    RUVIA_CHECK(second_info && second_info->queued_blocks_ == 1);
    RUVIA_CHECK(second_info && second_info->accepted_wire_bytes_ != 0);

    // Drain the persistent, budget-1 scan to a park point without new transport
    // events. This loop is bounded even when every attempted write is WANT.
    auto settled = output.drive();
    std::size_t settle_calls{};
    while (settled.needs_reschedule_ && settle_calls < 64) {
        settled = output.drive();
        ++settle_calls;
    }
    RUVIA_CHECK(!settled.needs_reschedule_);
    auto idle = output.drive();
    RUVIA_CHECK_EQ(idle.operations_, std::size_t{0});
    RUVIA_CHECK_EQ(idle.write_calls_, std::size_t{0});
    RUVIA_CHECK(!idle.needs_reschedule_);

    output.notify_transport_activity();
    const auto first_blocked_turn = output.drive();
    RUVIA_CHECK_EQ(first_blocked_turn.operations_, std::size_t{1});
    RUVIA_CHECK_EQ(first_blocked_turn.write_calls_, std::size_t{1});
    RUVIA_CHECK_EQ(first_blocked_turn.would_block_writes_, std::size_t{1});
    RUVIA_CHECK(first_blocked_turn.status_ == output_type::status_type::idle);
    RUVIA_CHECK(!first_blocked_turn.made_progress_);
    RUVIA_CHECK(first_blocked_turn.needs_reschedule_);
    const auto unvisited_stream = first_blocked_turn.last_stream_id_ == first_id ? second_id : first_id;
    const auto second_blocked_turn = output.drive();
    RUVIA_CHECK_EQ(second_blocked_turn.operations_, std::size_t{1});
    RUVIA_CHECK_EQ(second_blocked_turn.write_calls_, std::size_t{1});
    RUVIA_CHECK_EQ(second_blocked_turn.would_block_writes_, std::size_t{1});
    RUVIA_CHECK(!second_blocked_turn.made_progress_);
    RUVIA_CHECK_EQ(second_blocked_turn.last_stream_id_, unvisited_stream);

    settled = output.drive();
    settle_calls = 0;
    while (settled.needs_reschedule_ && settle_calls < 64) {
        settled = output.drive();
        ++settle_calls;
    }
    RUVIA_CHECK(!settled.needs_reschedule_);
    output.notify_transport_activity();
    bool retried_after_activity{};
    auto activity_turn = output.drive();
    std::size_t activity_calls{};
    while (activity_calls < 32) {
        retried_after_activity = retried_after_activity || activity_turn.would_block_writes_ != 0;
        if (!activity_turn.needs_reschedule_) {
            break;
        }
        activity_turn = output.drive();
        ++activity_calls;
    }
    RUVIA_CHECK(retried_after_activity);
    RUVIA_CHECK(!activity_turn.needs_reschedule_);
    const auto first_retried_info = output.stream_info(first_id);
    const auto second_retried_info = output.stream_info(second_id);
    RUVIA_CHECK(first_retried_info &&
                first_retried_info->last_write_status_ == ruvia::quic_operation_status::would_block &&
                first_retried_info->state_ == output_type::stream_state_type::open);
    RUVIA_CHECK(second_retried_info &&
                second_retried_info->last_write_status_ == ruvia::quic_operation_status::would_block &&
                second_retried_info->state_ == output_type::stream_state_type::open);
    idle = output.drive();
    RUVIA_CHECK_EQ(idle.operations_, std::size_t{0});
    RUVIA_CHECK(!idle.needs_reschedule_);

    RUVIA_CHECK_EQ(available_blocks(buffer), buffer.block_capacity() - output.queued_block_count());
    RUVIA_CHECK(output.cancel_stream(first_id).status_ == output_type::status_type::cancelled);
    RUVIA_CHECK_EQ(output.queued_block_count(), std::size_t{1});
    RUVIA_CHECK_EQ(available_blocks(buffer), buffer.block_capacity() - output.queued_block_count());
    RUVIA_CHECK(output.cancel_stream(second_id).status_ == output_type::status_type::cancelled);
    RUVIA_CHECK_EQ(output.queued_block_count(), std::size_t{0});
    RUVIA_CHECK_EQ(available_blocks(buffer), buffer.block_capacity() - output.queued_block_count());
    RUVIA_CHECK(!output.connection_retired());
    RUVIA_CHECK(output.stop().status_ == output_type::status_type::stopped);
    RUVIA_CHECK(buffer.stop());
}

RUVIA_TEST(http3_server_stream_output_sparse_capacity_resumes_pressure_and_fresh_responses) {
    quic_pair pair;
    pair.connect();
    const auto stream_id = pair.open_request_stream();
    ruvia::worker_memory worker;
    buffer buffer(2, 2, 4);
    output_type output(pair.server(), worker, epoch, generation,
        {.max_tracked_streams_ = 1024, .max_queued_blocks_ = 2, .max_drive_work_items_ = 1});
    constexpr std::uint64_t max_supplied_bytes = 64U * 1024U * 1024U;
    constexpr std::size_t max_steps = 8192;
    const auto blocked_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    pattern_supply supply;
    bool saw_would_block{};

    // QUIC flow control and packetization determine whether a write is partial;
    // this checks real WANT retries and does not assume any particular partial size.
    for (std::size_t step = 0; step < max_steps &&
                               std::chrono::steady_clock::now() < blocked_deadline && !saw_would_block &&
                               supply.bytes_ < max_supplied_bytes;
        ++step) {
        if (output.queued_block_count() == 0) {
            (void)offer_pattern_block(output, buffer, stream_id, supply, max_supplied_bytes);
        }
        const auto turn = output.drive();
        saw_would_block = turn.would_block_writes_ != 0 && turn.last_stream_id_ == stream_id;
        if (!saw_would_block) {
            pair.pump();
            output.notify_transport_activity();
        }
    }
    RUVIA_CHECK(saw_would_block);
    RUVIA_CHECK(supply.last_address_ != nullptr && supply.last_size_ != 0);
    RUVIA_CHECK_EQ(output.queued_block_count(), std::size_t{1});
    RUVIA_CHECK_EQ(available_blocks(buffer), buffer.block_capacity() - output.queued_block_count());
    const auto blocked_info = output.stream_info(stream_id);
    RUVIA_CHECK(blocked_info && blocked_info->last_write_status_ == ruvia::quic_operation_status::would_block);

    const auto fin = output.accept_control({.kind_ = http3_stream_control::kind::stream_fin,
        .id_ = message_id(stream_id),
        .value_ = supply.bytes_});
    RUVIA_CHECK(fin.status_ == output_type::status_type::fin_deferred);
    // Replay neither the same transport edge nor blocked writes while draining
    // local continuations. A fresh packet/credit edge below resumes output.
    output.notify_transport_activity();
    auto parked_turn = output.drive();
    for (std::size_t turn = 0; parked_turn.needs_reschedule_ && turn != 8; ++turn) {
        parked_turn = output.drive();
    }
    RUVIA_CHECK(!parked_turn.made_progress_);
    RUVIA_CHECK(!parked_turn.needs_reschedule_);
    const auto parked_info = output.stream_info(stream_id);
    RUVIA_CHECK(parked_info && blocked_info &&
                parked_info->accepted_wire_bytes_ == blocked_info->accepted_wire_bytes_);
    const auto idle_turn = output.drive();
    RUVIA_CHECK_EQ(idle_turn.write_calls_, std::size_t{0});
    RUVIA_CHECK(!idle_turn.needs_reschedule_);

    received_wire received;
    const auto recovery_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    bool complete_value{};
    for (std::size_t step = 0; step < max_steps &&
                               std::chrono::steady_clock::now() < recovery_deadline && !complete_value;
        ++step) {
        read_available(pair, stream_id, received);
        pair.pump();
        output.notify_transport_activity();
        (void)output.drive();
        read_available(pair, stream_id, received);
        const auto info = output.stream_info(stream_id);
        complete_value = info && info->state_ == output_type::stream_state_type::finished && received.fin_;
    }
    RUVIA_CHECK(complete_value);
    RUVIA_CHECK_EQ(received.bytes_.size(), static_cast<std::size_t>(supply.bytes_));
    RUVIA_CHECK(matches_pattern(received.bytes_, 0));
    RUVIA_CHECK_EQ(output.queued_block_count(), std::size_t{0});

    auto idle = output.drive();
    for (unsigned turn = 0; idle.needs_reschedule_ && turn != 8; ++turn) {
        idle = output.drive();
    }
    RUVIA_CHECK(!idle.needs_reschedule_);
    RUVIA_CHECK_EQ(output.drive().operations_, std::size_t{0});

    const auto first_fresh = pair.open_request_stream();
    const auto second_fresh = pair.open_request_stream();
    const std::array<std::uint64_t, 2> fresh_ids{first_fresh, second_fresh};
    const std::array<std::string_view, 2> fresh_bodies{"fresh after pressure", "fair sibling"};
    const auto first_wire = encode_response(fresh_bodies[0], worker.resource());
    const auto second_wire = encode_response(fresh_bodies[1], worker.resource());
    const std::array<std::span<const char>, 2> fresh_wire{first_wire, second_wire};
    output.notify_transport_activity();
    auto first_block = enqueue_block(buffer, message_id(first_fresh), fresh_wire[0]);
    RUVIA_CHECK(output.accept_data(first_block).status_ == output_type::status_type::accepted);
    RUVIA_CHECK(output.accept_control({.kind_ = http3_stream_control::kind::stream_fin,
                                          .id_ = message_id(first_fresh),
                                          .value_ = fresh_wire[0].size()})
                    .status_ == output_type::status_type::fin_deferred);
    // Add the sibling between bounded turns, before the first fresh FIN retires.
    auto first_turn = output.drive();
    for (unsigned turn = 0; first_turn.write_calls_ == 0 && turn != 4; ++turn) {
        first_turn = output.drive();
    }
    RUVIA_CHECK_EQ(first_turn.write_calls_, std::size_t{1});
    auto second_block = enqueue_block(buffer, message_id(second_fresh), fresh_wire[1]);
    RUVIA_CHECK(output.accept_data(second_block).status_ == output_type::status_type::accepted);
    RUVIA_CHECK(output.accept_control({.kind_ = http3_stream_control::kind::stream_fin,
                                          .id_ = message_id(second_fresh),
                                          .value_ = fresh_wire[1].size()})
                    .status_ == output_type::status_type::fin_deferred);
    bool sibling_serviced{};
    for (unsigned turn = 0; turn != 8 && !sibling_serviced; ++turn) {
        const auto result_value = output.drive();
        RUVIA_CHECK(result_value.operations_ <= 1);
        sibling_serviced = result_value.write_calls_ != 0 && result_value.last_stream_id_ == second_fresh;
    }
    RUVIA_CHECK(sibling_serviced);

    std::array<received_wire, 2> fresh_received;
    const auto fresh_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while ((!fresh_received[0].fin_ || !fresh_received[1].fin_) &&
           std::chrono::steady_clock::now() < fresh_deadline) {
        pair.pump();
        output.notify_transport_activity();
        (void)output.drive();
        for (std::size_t index = 0; index != fresh_ids.size(); ++index) {
            read_available(pair, fresh_ids[index], fresh_received[index]);
        }
    }
    for (std::size_t index = 0; index != fresh_ids.size(); ++index) {
        RUVIA_CHECK(fresh_received[index].fin_);
        const auto decoded = decode_response(fresh_ids[index], fresh_received[index], worker.resource());
        RUVIA_CHECK_EQ(decoded.status_, std::uint16_t{200});
        RUVIA_CHECK_EQ(decoded.body_, fresh_bodies[index]);
        RUVIA_CHECK_EQ(decoded.message_ends_, std::size_t{1});
    }
    RUVIA_CHECK_EQ(output.queued_block_count(), std::size_t{0});
    RUVIA_CHECK_EQ(available_blocks(buffer), buffer.block_capacity());
    RUVIA_CHECK(output.stop().status_ == output_type::status_type::stopped);
    RUVIA_CHECK(buffer.stop());
}

RUVIA_TEST(http3_server_stream_output_closes_on_tracking_exhaustion_and_rejects_invalid_ids) {
    {
        quic_pair pair;
        pair.connect();
        const auto stream_id = pair.open_request_stream();
        ruvia::worker_memory worker;
        buffer buffer(4, 4, 4);
        output_type output(pair.server(), worker, epoch, generation,
            {.max_tracked_streams_ = 1, .max_queued_blocks_ = 2, .max_drive_work_items_ = 2});
        constexpr std::array<char, 2> bytes_value{'o', 'k'};
        auto stale = enqueue_block(buffer, message_id(stream_id, epoch, generation + 1), bytes_value);
        RUVIA_CHECK(output.accept_data(stale).status_ == output_type::status_type::stale_connection);
        RUVIA_CHECK(stale);
        stale.release();

        auto accepted = enqueue_block(buffer, message_id(stream_id), bytes_value);
        RUVIA_CHECK(output.accept_data(accepted).status_ == output_type::status_type::accepted);
        RUVIA_CHECK(!accepted);
        const auto exhausted = output.accept_control({.kind_ = http3_stream_control::kind::writable,
            .id_ = message_id(stream_id + 4)});
        RUVIA_CHECK(exhausted.status_ == output_type::status_type::capacity_exhausted);
        RUVIA_CHECK(output.connection_retired());
        RUVIA_CHECK_EQ(output.queued_block_count(), std::size_t{0});
        RUVIA_CHECK_EQ(available_blocks(buffer), buffer.block_capacity() - output.queued_block_count());
        RUVIA_CHECK(output.stop().status_ == output_type::status_type::connection_closed);
        RUVIA_CHECK(buffer.stop());
    }

    {
        quic_pair pair;
        pair.connect();
        (void)pair.open_request_stream();
        ruvia::worker_memory worker;
        output_type output(pair.server(), worker, epoch, generation);
        const auto invalid = output.accept_control({.kind_ = http3_stream_control::kind::writable,
            .id_ = message_id(2)});
        RUVIA_CHECK(invalid.status_ == output_type::status_type::invalid_stream_id);
        RUVIA_CHECK(output.connection_retired());
        RUVIA_CHECK(output.stop().status_ == output_type::status_type::connection_closed);
    }
}

RUVIA_TEST(http3_server_stream_output_publishes_typed_critical_stream_without_fin_and_returns_credit) {
    quic_pair pair;
    pair.connect();
    const auto opened = pair.server().open_stream(true);
    RUVIA_CHECK(opened.status_ == ruvia::quic_operation_status::accepted);
    ruvia::worker_memory worker;
    buffer buffer(1, 1, 1);
    output_type output(pair.server(), worker, epoch, generation,
        {.max_tracked_streams_ = 3, .max_queued_blocks_ = 1, .max_drive_work_items_ = 2});
    constexpr std::array<char, 3> instructions{3, '\x80', 1};
    const auto sent = buffer.try_send_critical({epoch, generation, ruvia::http3_critical_stream_output::stream_kind::qpack_decoder}, std::as_bytes(std::span(instructions)));
    RUVIA_CHECK(sent == buffer::send_result::sent);
    borrowed_block_type block;
    RUVIA_CHECK(buffer.try_receive(block));
    RUVIA_CHECK(block.critical() != nullptr);
    RUVIA_CHECK(output.accept_data(block).status_ == output_type::status_type::invalid_input);
    RUVIA_CHECK(block);
    RUVIA_CHECK(output.accept_critical_data(block, opened.stream_id_).status_ == output_type::status_type::accepted);
    RUVIA_CHECK(!block);
    received_wire wire;
    const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    bool accepted_by_peer = false;
    while (wire.bytes_.size() < instructions.size() && std::chrono::steady_clock::now() < deadline_value) {
        (void)output.drive();
        pair.pump();
        const auto streams = pair.client().accept_streams();
        for (std::size_t i = 0; i < streams.size_; ++i) {
            accepted_by_peer |= streams.streams_[i].stream_id_ == opened.stream_id_;
        }
        if (accepted_by_peer) {
            read_available(pair, opened.stream_id_, wire);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK_EQ(wire.bytes_, std::string(instructions.data(), instructions.size()));
    RUVIA_CHECK(!wire.fin_);
    RUVIA_CHECK_EQ(output.queued_block_count(), std::size_t{0});
    RUVIA_CHECK_EQ(available_blocks(buffer), buffer.block_capacity() - output.queued_block_count());
    RUVIA_CHECK(output.stop().status_ == output_type::status_type::connection_closed);
    RUVIA_CHECK(buffer.stop());
}

RUVIA_TEST(http3_server_stream_output_binds_push_streams_finishes_and_cancels_without_closing_parent) {
    quic_pair pair;
    pair.connect();
    const auto parent_value = pair.open_request_stream();
    const auto first = pair.server().open_stream(true);
    const auto second = pair.server().open_stream(true);
    RUVIA_CHECK(first.status_ == ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(second.status_ == ruvia::quic_operation_status::accepted);
    ruvia::worker_memory worker;
    buffer buffer(3, 3, 3);
    output_type output(pair.server(), worker, epoch, generation,
        {.max_tracked_streams_ = 3, .max_queued_blocks_ = 3, .max_drive_work_items_ = 4});
    RUVIA_CHECK(output.register_push_stream(parent_value, 0).status_ == output_type::status_type::invalid_input);
    RUVIA_CHECK(output.register_push_stream(first.stream_id_, 0).status_ == output_type::status_type::accepted);
    RUVIA_CHECK(output.register_push_stream(second.stream_id_, 1).status_ == output_type::status_type::accepted);
    RUVIA_CHECK(output.register_push_stream(first.stream_id_, 2).status_ == output_type::status_type::invalid_input);
    auto first_id = message_id(first.stream_id_);
    first_id.push_id_ = 0;
    auto second_id = message_id(second.stream_id_);
    second_id.push_id_ = 1;
    constexpr std::array<char, 4> push_wire_value{1, 0, 'o', 'k'};
    auto first_block = enqueue_block(buffer, first_id, push_wire_value);
    auto cancelled_block = enqueue_block(buffer, second_id, push_wire_value);
    RUVIA_CHECK(output.accept_data(first_block).status_ == output_type::status_type::accepted);
    RUVIA_CHECK(output.accept_data(cancelled_block).status_ == output_type::status_type::accepted);
    RUVIA_CHECK(output.accept_control({.kind_ = http3_stream_control::kind::stream_fin,
                                          .id_ = first_id,
                                          .value_ = push_wire_value.size()})
                    .status_ == output_type::status_type::fin_deferred);
    RUVIA_CHECK(output.cancel_stream(second.stream_id_).status_ == output_type::status_type::cancelled);
    RUVIA_CHECK(!output.connection_retired());
    auto parent_wire = encode_response("parent", worker.resource());
    auto parent_block = enqueue_block(buffer, message_id(parent_value), parent_wire);
    RUVIA_CHECK(output.accept_data(parent_block).status_ == output_type::status_type::accepted);
    RUVIA_CHECK(output.accept_control({.kind_ = http3_stream_control::kind::stream_fin,
                                          .id_ = message_id(parent_value),
                                          .value_ = parent_wire.size()})
                    .status_ == output_type::status_type::fin_deferred);
    received_wire push;
    received_wire response;
    bool accepted_push = false;
    const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while ((!push.fin_ || !response.fin_) && std::chrono::steady_clock::now() < deadline_value) {
        (void)output.drive();
        pair.pump();
        const auto accepted = pair.client().accept_streams();
        for (std::size_t i = 0; i < accepted.size_; ++i) {
            accepted_push |= accepted.streams_[i].stream_id_ == first.stream_id_;
        }
        if (accepted_push && !push.fin_) {
            read_available(pair, first.stream_id_, push);
        }
        if (!response.fin_) {
            read_available(pair, parent_value, response);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(push.fin_);
    RUVIA_CHECK(response.fin_);
    output.notify_transport_activity();
    for (unsigned attempt_value = 0; attempt_value < 16; ++attempt_value) {
        (void)output.drive();
    }
    RUVIA_CHECK_EQ(push.bytes_, std::string(push_wire_value.data(), push_wire_value.size()));
    RUVIA_CHECK_EQ(response.bytes_, std::string(parent_wire.data(), parent_wire.size()));
    RUVIA_CHECK(output.stream_info(first.stream_id_)->state_ == output_type::stream_state_type::finished);
    RUVIA_CHECK(output.stream_info(second.stream_id_)->state_ == output_type::stream_state_type::cancelled);
    RUVIA_CHECK_EQ(output.queued_block_count(), std::size_t{0});
    RUVIA_CHECK_EQ(available_blocks(buffer), buffer.block_capacity() - output.queued_block_count());
    RUVIA_CHECK(output.stop().status_ == output_type::status_type::stopped);
    RUVIA_CHECK(buffer.stop());
}

RUVIA_TEST(http3_quic_client_stream_read_health_observes_peer_reset_before_reading_buffered_bytes) {
    quic_pair pair;
    pair.connect();
    const auto id = pair.open_request_stream();
    const std::array<char, 4> bytes_value{'d', 'a', 't', 'a'};
    RUVIA_CHECK(pair.server().write_stream(id, std::as_bytes(std::span(bytes_value))).status_ == ruvia::quic_operation_status::accepted);
    const auto code = static_cast<std::uint64_t>(ruvia::http3_connection_error_code::request_cancelled);
    RUVIA_CHECK(pair.server().reset_stream(id, code) == ruvia::quic_operation_status::accepted);
    const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    ruvia::quic_stream_read_result health;
    while (std::chrono::steady_clock::now() < deadline_value) {
        pair.pump();
        health = pair.client().read_health(id);
        if (health.status_ == ruvia::quic_stream_read_status::reset) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK(health.status_ == ruvia::quic_stream_read_status::reset);
    RUVIA_CHECK_EQ(health.peer_reset_error_code_, std::optional{code});
    std::array<char, 1> output{};
    RUVIA_CHECK(pair.client().read_stream(id, std::as_writable_bytes(std::span(output))).status_ == ruvia::quic_stream_read_status::reset);
}

namespace {
struct push_route_state final {
    std::filesystem::path file_path_;
    std::string body_;
    std::size_t accepted_{};
    bool metadata_owned_{true};
};
ruvia::task<ruvia::http_response> produce_push_routes(void* raw, ruvia::context& context_value) {
    auto& state_value = *static_cast<push_route_state*>(raw);
    if (context_value.req().path() == "/push") {
        for (auto path : {"/asset", "/stream", "/file"}) {
            std::string authority(context_value.req().authority());
            std::string target = path;
            std::string value(512, 'v');
            const std::array fields_value{ruvia::http_header_view("x-pushed", std::string_view(value))};
            auto operation = context_value.push({.authority_ = authority, .path_ = target, .headers_ = fields_value});
            authority.assign("changed");
            target.assign("changed");
            value.assign("changed");
            if (co_await std::move(operation)) {
                ++state_value.accepted_;
            }
        }
        co_return context_value.text("parent");
    }
    state_value.metadata_owned_ = state_value.metadata_owned_ && context_value.req().header("x-pushed") == std::string(512, 'v');
    if (context_value.req().path() == "/file") {
        ruvia::http_response response({.resource_ = context_value.arena()});
        response.file_body(state_value.file_path_, state_value.body_.size(), 0, state_value.body_.size(), ruvia::http_response_file_identity::unchecked());
        co_return response;
    }
    co_return context_value.text("asset");
}
ruvia::task<void> produce_push_stream(void* raw, ruvia::context& context_value) {
    auto& state_value = *static_cast<push_route_state*>(raw);
    state_value.metadata_owned_ = state_value.metadata_owned_ && context_value.req().header("x-pushed") == std::string(512, 'v');
    co_await context_value.stream().write(std::string_view(state_value.body_));
    const std::array trailers{ruvia::http_header_view("x-push-end", "done")};
    co_await context_value.stream().end(trailers);
}

ruvia::task<void> exercise_quic_push_routes(ruvia::event_loop_attachment& attachment, quic_pair& pair,
    std::uint64_t parent_id, std::span<const char> request, std::span<const char> peer_control,
    push_route_state& state_value, ruvia::blocking_pool& blocking_pool_value,
    ruvia::test::counting_memory_resource& upstream, ruvia::testing::test_context& ruvia_ctx) {
    using owner = ruvia::detail::http3_server_connection;
    const auto& worker_handle_value = attachment.loop().handle();
    ruvia::worker_memory worker(upstream);
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    for (auto path : {"/push", "/asset", "/file"}) {
        implementation.register_route(ruvia::http_known_method::get, routing_test::path(path),
            ruvia::detail::route_handler_type(&state_value, &produce_push_routes), ruvia::detail::request_body_mode::buffered, {}, {});
    }
    implementation.register_response_stream_route(ruvia::http_known_method::get, routing_test::path("/stream"),
        ruvia::detail::route_stream_handler_type(&state_value, &produce_push_stream), {}, {});
    implementation.finalize();
    ruvia::stop_source stopping;
    const auto stop_token_value = stopping.token();
    ruvia::detail::context_services services(worker_handle_value, stop_token_value);
    ruvia::detail::http_server_options options;
    options.blocking_pool_ = &blocking_pool_value;
    ruvia::connection_scanner scanner(worker_handle_value, {.scan_interval_ = std::chrono::seconds(1)});
    buffer inbound(2, 2, 2, worker.resource());
    buffer outbound(2, 2, 2, worker.resource());
    unsigned activations{};
    owner owner_value(implementation.route_table(), worker, services, options, outbound,
        {.context_ = &activations, .activate_ = [](void* raw, std::uint64_t, std::uint64_t, std::uint64_t, const owner::worker_activation_type&) noexcept { ++*static_cast<unsigned*>(raw); }, .slot_generation_ = 1},
        {.epoch_ = epoch, .connection_generation_ = generation, .max_tracked_streams_ = 32, .connection_scanner_ = &scanner, .executor_ = attachment.loop().executor()});
    output_type output(pair.server(), worker, epoch, generation,
        {.max_tracked_streams_ = 4, .max_queued_blocks_ = 2, .max_drive_work_items_ = 4});
    std::vector<std::uint64_t> pushed_ids;
    std::array<received_wire, 3> pushed;
    received_wire parent;
    std::exception_ptr failure;
    auto submit_input = [&](http3_stream_id id, std::span<const char> bytes_value, bool fin) {
        auto block = enqueue_block(inbound, id, bytes_value);
        RUVIA_CHECK(owner_value.accept_data(block).status_ == owner::event_status_type::accepted);
        block.release();
        if (fin) {
            RUVIA_CHECK(owner_value.accept_control({.kind_ = http3_stream_control::kind::stream_fin,
                                                       .id_ = id,
                                                       .value_ = bytes_value.size()})
                            .status_ == owner::event_status_type::dispatched);
        }
    };
    try {
        submit_input(message_id(2), peer_control, false);
        submit_input(message_id(parent_id), request, true);
        const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        std::vector<std::uint64_t> accepted_ids;
        while (std::chrono::steady_clock::now() < deadline_value) {
            if (const auto intent = owner_value.peek_transport_intent()) {
                if (intent->token_.kind_ == owner::transport_intent_kind_type::open_push_stream) {
                    const auto opened = pair.server().open_stream(true);
                    RUVIA_CHECK(opened.status_ == ruvia::quic_operation_status::accepted);
                    RUVIA_CHECK(output.register_push_stream(opened.stream_id_, *intent->token_.id_.push_id_).status_ == output_type::status_type::accepted);
                    pushed_ids.push_back(opened.stream_id_);
                    RUVIA_CHECK(owner_value.ack_transport_intent(intent->token_, owner::push_stream_open_result_type{.status_ = owner::push_stream_open_result_type::status_type::opened, .stream_id_ = opened.stream_id_}));
                } else {
                    RUVIA_CHECK(owner_value.ack_transport_intent(intent->token_));
                }
            }
            (void)owner_value.publish_one({.data_ = true, .control_ = true, .local_ = true});
            http3_stream_control control;
            while (outbound.try_receive_control(control)) {
                const auto accepted = output.accept_control(control);
                RUVIA_CHECK(accepted.status_ == output_type::status_type::accepted || accepted.status_ == output_type::status_type::fin_deferred);
            }
            buffer::borrowed_block block;
            while (outbound.try_receive(block)) {
                RUVIA_CHECK(output.accept_data(block).status_ == output_type::status_type::accepted);
            }
            (void)output.drive();
            pair.pump();
            const auto streams = pair.client().accept_streams();
            for (std::size_t i = 0; i < streams.size_; ++i) {
                accepted_ids.push_back(streams.streams_[i].stream_id_);
            }
            if (!parent.fin_) {
                read_available(pair, parent_id, parent);
            }
            for (std::size_t i = 0; i < pushed_ids.size(); ++i) {
                if (!pushed[i].fin_ && std::ranges::find(accepted_ids, pushed_ids[i]) != accepted_ids.end()) {
                    read_available(pair, pushed_ids[i], pushed[i]);
                }
            }
            (void)owner_value.reactivate_blocked({.data_ = true, .control_ = true, .local_ = true});
            if (owner_value.active_task_count() == 0 && parent.fin_ && pushed_ids.size() == 3 && pushed[0].fin_ && pushed[1].fin_ && pushed[2].fin_) {
                break;
            }
            co_await ruvia::sleep_for(worker_handle_value, std::chrono::milliseconds(1));
        }
        RUVIA_CHECK(parent.fin_);
        RUVIA_CHECK_EQ(state_value.accepted_, std::size_t{3});
        RUVIA_CHECK(state_value.metadata_owned_);
        RUVIA_CHECK_EQ(owner_value.active_session_stream_count(), std::size_t{0});
        ruvia::http3_connection client(ruvia::http3_peer_role::client, worker.resource(), {.max_push_id_ = 2});
        RUVIA_CHECK(client.register_client_request(parent_id, ruvia::http_known_method::get).scope_ == ruvia::http3_connection_error_scope::none);
        struct response_observation final {
            std::unordered_map<std::uint64_t, std::string> bodies_;
            std::string trailer_;
        } responses;
        auto capture_value = +[](void* raw, const ruvia::http3_connection_event& event) {
            auto& captured_value = *static_cast<response_observation*>(raw);
            if (event.kind_ == ruvia::http3_connection_event_kind::body) {
                captured_value.bodies_[event.stream_id_].append(event.body_.data(), event.body_.size());
            } else if (event.kind_ == ruvia::http3_connection_event_kind::trailer_field) {
                captured_value.trailer_ = event.trailer_.value_;
            }
        };
        RUVIA_CHECK(client.feed(parent_id, std::span(parent.bytes_), true, false, capture_value, &responses).scope_ == ruvia::http3_connection_error_scope::none);
        for (std::size_t i = 0; i < pushed_ids.size(); ++i) {
            RUVIA_CHECK(pushed[i].fin_);
            RUVIA_CHECK(client.feed(pushed_ids[i], std::span(pushed[i].bytes_), true, false, capture_value, &responses).scope_ == ruvia::http3_connection_error_scope::none);
        }
        RUVIA_CHECK_EQ(responses.bodies_[parent_id], "parent");
        RUVIA_CHECK_EQ(responses.bodies_[pushed_ids[0]], "asset");
        RUVIA_CHECK_EQ(responses.bodies_[pushed_ids[1]], state_value.body_);
        RUVIA_CHECK_EQ(responses.bodies_[pushed_ids[2]], state_value.body_);
        RUVIA_CHECK_EQ(responses.trailer_, "done");
    } catch (...) {
        failure = std::current_exception();
    }
    (void)owner_value.request_stop();
    while (const auto intent = owner_value.peek_transport_intent()) {
        if (intent->token_.kind_ == owner::transport_intent_kind_type::open_push_stream) {
            (void)owner_value.ack_transport_intent(intent->token_, owner::push_stream_open_result_type{.status_ = owner::push_stream_open_result_type::status_type::stopped});
        } else {
            (void)owner_value.ack_transport_intent(intent->token_);
        }
    }
    co_await owner_value.join();
    RUVIA_CHECK(output.stop().status_ != output_type::status_type::unsafe_to_release);
    RUVIA_CHECK(inbound.stop());
    RUVIA_CHECK(outbound.stop());
    attachment.stop();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
}  // namespace

RUVIA_TEST(http3_server_push_buffered_streaming_and_file_routes_use_real_quic_unidirectional_streams) {
    quic_pair pair;
    pair.connect();
    auto request_head = ruvia::encode_http3_client_request_head({.method_ = "GET", .scheme_ = "https", .authority_ = "example.test", .path_ = "/push"});
    RUVIA_CHECK((request_head.index() == 0));
    std::array<char, ruvia::http3_frame_header_max_bytes> frame{};
    const auto prefix = ruvia::encode_http3_frame_header(frame, 1, std::get<0>(request_head).field_section_.size());
    std::vector<char> request(frame.begin(), frame.begin() + std::get<0>(prefix));
    request.insert(request.end(), std::get<0>(request_head).field_section_.begin(), std::get<0>(request_head).field_section_.end());
    const auto parent_id = pair.open_request_stream(true, request);
    const auto control = pair.client().open_stream(true);
    RUVIA_CHECK_EQ(control.stream_id_, std::uint64_t{2});
    constexpr std::array<char, 6> settings_and_max{0, 4, 0, 0xd, 1, 2};
    pair.write_client_stream(control.stream_id_, settings_and_max);
    const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    std::string received_control;
    while (received_control.size() < settings_and_max.size() && std::chrono::steady_clock::now() < deadline_value) {
        pair.pump();
        if (pair.accepted_stream(control.stream_id_)) {
            std::array<char, 64> buffer{};
            const auto read = pair.server().read_stream(control.stream_id_, std::as_writable_bytes(std::span(buffer)));
            if (read.status_ == ruvia::quic_stream_read_status::data) {
                received_control.append(buffer.data(), read.size_);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RUVIA_CHECK_EQ(received_control, std::string(settings_and_max.data(), settings_and_max.size()));
    push_route_state state;
    state.body_.assign(65539, 's');
    state.file_path_ = std::filesystem::temp_directory_path() / ("ruvia-h3-push-file-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        std::ofstream file(state.file_path_, std::ios::binary);
        file << state.body_;
    }
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 32});
    ruvia::test::counting_memory_resource upstream;
    ruvia::blocking_pool pool({.thread_count_ = 1, .queue_capacity_ = 8});
    auto root = attachment.loop().start(exercise_quic_push_routes(attachment, pair, parent_id, request, settings_and_max, state, pool, upstream, ruvia_ctx));
    attachment.run();
    root.get();
    pool.stop();
    pool.join();
    std::filesystem::remove(state.file_path_);
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}
