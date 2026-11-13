#pragma once

#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2_crypto.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/socket_base.hpp>

#include "http3_test_client.h"

namespace ruvia::test {

// Blocking HTTP/3 client for malformed-request wire regressions. It drives
// ngtcp2 and nghttp3 directly, so a request field section reaches the wire
// verbatim: ruvia's public HTTP/3 API validates every request head and never
// sends one. One request is in flight at a time; DATA credit returns at once.
class http3_raw_request_client final {
public:
    http3_raw_request_client(asio::ip::udp::endpoint server, const std::filesystem::path& ca_file,
        std::string_view server_name, std::chrono::milliseconds timeout)
        : timeout_(timeout),
          server_(server),
          socket_(io_),
          tls_(ca_file, server_name) {
        socket_.open(server_.protocol());
        socket_.connect(server_);
        socket_.non_blocking(true);
        local_ = socket_.local_endpoint();
        // This client owns ngtcp2 itself, so it installs the adapter's crypto
        // callbacks next to its own stream callbacks.
        ngtcp2_callbacks callbacks{};
        callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
        callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
        callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
        callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
        callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
        callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;
        callbacks.update_key = ngtcp2_crypto_update_key_cb;
        callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
        callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
        callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
        callbacks.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
        callbacks.rand = http3_test_fill_random;
        callbacks.get_new_connection_id2 = http3_test_new_connection_id;
        callbacks.handshake_completed = [](ngtcp2_conn*, void* user_data) noexcept {
            static_cast<http3_raw_request_client*>(user_data)->handshake_completed_ = true;
            return 0;
        };
        callbacks.recv_stream_data = receive_stream_data;
        callbacks.acked_stream_data_offset = acknowledge_stream_data;
        callbacks.stream_close2 = close_stream;
        callbacks.stream_reset = reset_stream;
        callbacks.extend_max_stream_data = extend_stream_data;
        ngtcp2_settings settings;
        ngtcp2_settings_default(&settings);
        settings.initial_ts = timestamp();
        settings.max_tx_udp_payload_size = max_udp_payload_bytes;
        ngtcp2_transport_params parameters;
        ngtcp2_transport_params_default(&parameters);
        parameters.initial_max_data = 16 * 1024 * 1024;
        parameters.initial_max_stream_data_bidi_local = 1024 * 1024;
        parameters.initial_max_stream_data_uni = 1024 * 1024;
        parameters.initial_max_streams_uni = 16;
        parameters.max_idle_timeout = 30 * NGTCP2_SECONDS;
        const auto source = http3_test_random_connection_id(16);
        const auto destination = http3_test_random_connection_id(18);
        const auto initial_path = path();
        if (ngtcp2_conn_client_new(&connection_, &destination, &source, &initial_path, NGTCP2_PROTO_VER_V1,
                &callbacks, &settings, &parameters, nullptr, this) != 0) {
            throw std::runtime_error("HTTP/3 raw client cannot create its QUIC connection");
        }
        tls_.bind(get_connection, this, connection_);
        if (!wait_until([this] { return handshake_completed_ && start_http3(); })) {
            throw std::runtime_error("HTTP/3 raw client handshake failed");
        }
    }

    ~http3_raw_request_client() {
        if (http3_ != nullptr) {
            nghttp3_conn_del(http3_);
        }
        ngtcp2_conn_del(connection_);
    }

    http3_raw_request_client(const http3_raw_request_client&) = delete;
    http3_raw_request_client& operator=(const http3_raw_request_client&) = delete;
    http3_raw_request_client(http3_raw_request_client&&) = delete;
    http3_raw_request_client& operator=(http3_raw_request_client&&) = delete;

    // Sends `fields` verbatim as the request field section, after the peer's
    // SETTINGS arrived. end_stream=false leaves the request send half open.
    // Returns once the response ended or the stream closed or was reset.
    [[nodiscard]] http3_test_response request(
        std::initializer_list<std::pair<std::string_view, std::string_view>> fields, bool end_stream = true) {
        wait_or_throw([this] { return settings_received_; }, "HTTP/3 raw client did not receive SETTINGS");
        std::int64_t stream_id = -1;
        wait_or_throw([&] { return ngtcp2_conn_open_bidi_stream(connection_, &stream_id, nullptr) == 0; },
            "HTTP/3 raw client cannot open a request stream");
        std::vector<nghttp3_nv> native;
        native.reserve(fields.size());
        for (const auto& [name, value] : fields) {
            native.push_back({
                .name = reinterpret_cast<std::uint8_t*>(const_cast<char*>(name.data())),
                .value = reinterpret_cast<std::uint8_t*>(const_cast<char*>(value.data())),
                .namelen = name.size(),
                .valuelen = value.size(),
                .flags = NGHTTP3_NV_FLAG_NONE,
            });
        }
        const nghttp3_data_reader open_send_half{.read_data = provide_no_content};
        if (nghttp3_conn_submit_request(http3_, stream_id, native.data(), native.size(),
                end_stream ? nullptr : &open_send_half, nullptr) != 0) {
            throw std::runtime_error("HTTP/3 raw client cannot submit the field section");
        }
        stream_id_ = stream_id;
        response_ = {};
        complete_ = false;
        wait_or_throw([this] { return complete_; }, "HTTP/3 raw response did not complete");
        stream_id_ = -1;
        return std::move(response_);
    }

    // The connection failed or entered its closing or draining period.
    [[nodiscard]] bool closed() const noexcept {
        return failed_;
    }

    // For assertions on the CONNECTION_CLOSE the peer sent.
    [[nodiscard]] const ngtcp2_conn* native_handle() const noexcept {
        return connection_;
    }

private:
    static constexpr std::size_t max_udp_payload_bytes = 1350;

    [[nodiscard]] static http3_raw_request_client& self(void* user_data) noexcept {
        return *static_cast<http3_raw_request_client*>(user_data);
    }

    // An open request send half has no content to provide yet.
    static nghttp3_ssize provide_no_content(nghttp3_conn*, std::int64_t, nghttp3_vec*, std::size_t, std::uint32_t*,
        void*, void*) noexcept {
        return NGHTTP3_ERR_WOULDBLOCK;
    }

    static ngtcp2_conn* get_connection(ngtcp2_crypto_conn_ref* reference) noexcept {
        return self(reference->user_data).connection_;
    }

    [[nodiscard]] static ngtcp2_tstamp timestamp() noexcept {
        return static_cast<ngtcp2_tstamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    [[nodiscard]] ngtcp2_path path() noexcept {
        return {
            .local = {reinterpret_cast<ngtcp2_sockaddr*>(local_.data()), static_cast<ngtcp2_socklen>(local_.size())},
            .remote = {reinterpret_cast<ngtcp2_sockaddr*>(server_.data()), static_cast<ngtcp2_socklen>(server_.size())},
            .user_data = nullptr,
        };
    }

    // Binds the HTTP/3 control and QPACK streams once QUIC permits them.
    [[nodiscard]] bool start_http3() {
        if (http3_ != nullptr) {
            return true;
        }
        if (ngtcp2_conn_get_streams_uni_left2(connection_) < 3) {
            return false;
        }
        nghttp3_callbacks callbacks{};
        callbacks.acked_stream_data = nullptr;
        callbacks.recv_header = receive_header;
        callbacks.recv_trailer = receive_header;
        callbacks.recv_data = [](nghttp3_conn*, std::int64_t stream_id, const std::uint8_t* data, std::size_t size,
                                  void* user_data, void*) noexcept {
            auto& client = self(user_data);
            if (stream_id == client.stream_id_) {
                client.response_.body_.append(reinterpret_cast<const char*>(data), size);
            }
            return 0;
        };
        callbacks.end_stream = [](nghttp3_conn*, std::int64_t stream_id, void* user_data, void*) noexcept {
            auto& client = self(user_data);
            client.complete_ = client.complete_ || stream_id == client.stream_id_;
            return 0;
        };
        callbacks.stop_sending = [](nghttp3_conn*, std::int64_t stream_id, std::uint64_t error, void* user_data,
                                     void*) noexcept {
            (void)ngtcp2_conn_shutdown_stream_read(self(user_data).connection_, 0, stream_id, error);
            return 0;
        };
        callbacks.reset_stream = [](nghttp3_conn*, std::int64_t stream_id, std::uint64_t error, void* user_data,
                                     void*) noexcept {
            (void)ngtcp2_conn_shutdown_stream_write(self(user_data).connection_, 0, stream_id, error);
            return 0;
        };
        callbacks.recv_settings2 = [](nghttp3_conn*, const nghttp3_proto_settings*, void* user_data) noexcept {
            self(user_data).settings_received_ = true;
            return 0;
        };
        nghttp3_settings settings;
        nghttp3_settings_default(&settings);
        if (nghttp3_conn_client_new(&http3_, &callbacks, &settings, nullptr, this) != 0) {
            throw std::runtime_error("HTTP/3 raw client cannot create its HTTP/3 connection");
        }
        std::array<std::int64_t, 3> streams{};
        for (auto& stream_id : streams) {
            if (ngtcp2_conn_open_uni_stream(connection_, &stream_id, nullptr) != 0) {
                throw std::runtime_error("HTTP/3 raw client cannot open its control streams");
            }
        }
        if (nghttp3_conn_bind_control_stream(http3_, streams[0]) != 0 ||
            nghttp3_conn_bind_qpack_streams(http3_, streams[1], streams[2]) != 0) {
            throw std::runtime_error("HTTP/3 raw client cannot bind its control streams");
        }
        return true;
    }

    static int receive_header(nghttp3_conn*, std::int64_t stream_id, std::int32_t, nghttp3_rcbuf* name,
        nghttp3_rcbuf* value, std::uint8_t, void* user_data, void*) noexcept {
        auto& client = self(user_data);
        if (stream_id != client.stream_id_) {
            return 0;
        }
        const auto name_buffer = nghttp3_rcbuf_get_buf(name);
        const auto value_buffer = nghttp3_rcbuf_get_buf(value);
        const std::string_view name_view(reinterpret_cast<const char*>(name_buffer.base), name_buffer.len);
        const std::string_view value_view(reinterpret_cast<const char*>(value_buffer.base), value_buffer.len);
        try {
            if (name_view == ":status") {
                client.response_.status_ = 0;
                for (const char digit : value_view) {
                    client.response_.status_ = client.response_.status_ * 10 + (digit - '0');
                }
                if (client.response_.status_ < 200) {
                    client.response_.interim_statuses_.push_back(client.response_.status_);
                    client.response_.status_ = 0;
                }
            } else if (client.response_.status_ != 0) {
                client.response_.headers_.emplace_back(name_view, value_view);
            }
        } catch (...) {
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
        return 0;
    }

    static int receive_stream_data(ngtcp2_conn* connection, std::uint32_t flags, std::int64_t stream_id,
        std::uint64_t, const std::uint8_t* data, std::size_t size, void* user_data, void*) noexcept {
        auto& client = self(user_data);
        if (client.http3_ == nullptr ||
            nghttp3_conn_read_stream2(client.http3_, stream_id, data, size, (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0,
                ngtcp2_conn_get_timestamp(connection)) < 0) {
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
        // Every received byte is consumed at once.
        ngtcp2_conn_extend_max_stream_offset(connection, stream_id, size);
        ngtcp2_conn_extend_max_offset(connection, size);
        return 0;
    }

    static int acknowledge_stream_data(ngtcp2_conn*, std::int64_t stream_id, std::uint64_t, std::uint64_t size,
        void* user_data, void*) noexcept {
        auto& client = self(user_data);
        return client.http3_ == nullptr || nghttp3_conn_add_ack_offset(client.http3_, stream_id, size) == 0
                   ? 0
                   : NGTCP2_ERR_CALLBACK_FAILURE;
    }

    static int close_stream(ngtcp2_conn*, std::uint32_t flags, std::int64_t stream_id, std::uint64_t receive_error,
        std::uint64_t send_error, void* user_data, void*) noexcept {
        auto& client = self(user_data);
        if (stream_id == client.stream_id_) {
            if ((flags & NGTCP2_STREAM_CLOSE2_FLAG_RX_APP_ERROR_CODE_SET) != 0 && receive_error != NGHTTP3_H3_NO_ERROR &&
                !client.response_.reset_error_) {
                client.response_.reset_error_ = static_cast<http3_error_code>(receive_error);
            }
            client.complete_ = true;
        }
        if (client.http3_ == nullptr) {
            return 0;
        }
        std::uint32_t native_flags = 0;
        if ((flags & NGTCP2_STREAM_CLOSE2_FLAG_RX_APP_ERROR_CODE_SET) != 0) {
            native_flags |= NGHTTP3_STREAM_CLOSE_FLAG_RX_APP_ERROR_CODE_SET;
        }
        if ((flags & NGTCP2_STREAM_CLOSE2_FLAG_TX_APP_ERROR_CODE_SET) != 0) {
            native_flags |= NGHTTP3_STREAM_CLOSE_FLAG_TX_APP_ERROR_CODE_SET;
        }
        const int result = nghttp3_conn_close_stream2(client.http3_, native_flags, stream_id, receive_error, send_error);
        return result == 0 || result == NGHTTP3_ERR_STREAM_NOT_FOUND ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
    }

    static int reset_stream(ngtcp2_conn*, std::int64_t stream_id, std::uint64_t, std::uint64_t error,
        void* user_data, void*) noexcept {
        auto& client = self(user_data);
        if (stream_id == client.stream_id_ && !client.response_.reset_error_) {
            client.response_.reset_error_ = static_cast<http3_error_code>(error);
        }
        return client.http3_ == nullptr || nghttp3_conn_shutdown_stream_read(client.http3_, stream_id) == 0
                   ? 0
                   : NGTCP2_ERR_CALLBACK_FAILURE;
    }

    static int extend_stream_data(ngtcp2_conn*, std::int64_t stream_id, std::uint64_t, void* user_data,
        void*) noexcept {
        auto& client = self(user_data);
        return client.http3_ == nullptr || nghttp3_conn_unblock_stream(client.http3_, stream_id) == 0
                   ? 0
                   : NGTCP2_ERR_CALLBACK_FAILURE;
    }

    template <typename predicate_type>
    [[nodiscard]] bool wait_until(predicate_type&& done) {
        const auto deadline = std::chrono::steady_clock::now() + timeout_;
        for (;;) {
            if (done()) {
                return true;
            }
            if (failed_ || std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            pump(deadline);
        }
    }

    template <typename predicate_type>
    void wait_or_throw(predicate_type&& done, const char* what) {
        if (!wait_until(std::forward<predicate_type>(done))) {
            throw std::runtime_error(what);
        }
    }

    void pump(std::chrono::steady_clock::time_point deadline) {
        flush();
        auto wake = deadline;
        const auto expiry = ngtcp2_conn_get_expiry(connection_);
        if (expiry < static_cast<ngtcp2_tstamp>((std::numeric_limits<std::int64_t>::max)())) {
            wake = (std::min)(wake, std::chrono::steady_clock::time_point(
                                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                            std::chrono::nanoseconds(static_cast<std::int64_t>(expiry)))));
        }
        if (wake > std::chrono::steady_clock::now()) {
            socket_.async_wait(asio::socket_base::wait_read, [](const std::error_code&) {});
            io_.restart();
            io_.run_one_until(wake);
            socket_.cancel();
            io_.restart();
            io_.run();
        }
        receive_available();
        if (!failed_ && timestamp() >= ngtcp2_conn_get_expiry(connection_)) {
            failed_ = ngtcp2_conn_handle_expiry(connection_, timestamp()) != 0;
        }
        flush();
    }

    void receive_available() {
        for (;;) {
            std::error_code error;
            const auto size = socket_.receive(asio::buffer(input_), 0, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                return;
            }
            if (error) {
                throw std::system_error(error, "HTTP/3 raw client UDP receive failed");
            }
            if (failed_) {
                continue;
            }
            const auto received_path = path();
            const ngtcp2_pkt_info info{};
            failed_ = ngtcp2_conn_read_pkt(connection_, &received_path, &info,
                          reinterpret_cast<const std::uint8_t*>(input_.data()), size, timestamp()) != 0;
        }
    }

    void flush() {
        if (failed_) {
            return;
        }
        for (;;) {
            std::array<nghttp3_vec, 16> vectors{};
            std::int64_t stream_id = -1;
            int fin = 0;
            nghttp3_ssize count = 0;
            if (http3_ != nullptr) {
                count = nghttp3_conn_writev_stream(http3_, &stream_id, &fin, vectors.data(), vectors.size());
                if (count < 0) {
                    failed_ = true;
                    return;
                }
            }
            std::uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
            if (fin != 0) {
                flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
            }
            ngtcp2_path_storage storage;
            ngtcp2_path_storage_zero(&storage);
            ngtcp2_pkt_info info{};
            ngtcp2_ssize written = -1;
            const auto size = ngtcp2_conn_writev_stream(connection_, &storage.path, &info,
                reinterpret_cast<std::uint8_t*>(output_.data()), output_.size(), &written, flags, stream_id,
                reinterpret_cast<const ngtcp2_vec*>(vectors.data()), static_cast<std::size_t>(count), timestamp());
            if (size < 0) {
                if (size == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
                    nghttp3_conn_block_stream(http3_, stream_id);
                    continue;
                }
                if (size == NGTCP2_ERR_STREAM_SHUT_WR) {
                    nghttp3_conn_shutdown_stream_write(http3_, stream_id);
                    continue;
                }
                if (size == NGTCP2_ERR_WRITE_MORE) {
                    if (nghttp3_conn_add_write_offset(http3_, stream_id, static_cast<std::size_t>(written)) != 0) {
                        failed_ = true;
                        return;
                    }
                    continue;
                }
                failed_ = true;
                return;
            }
            if (written >= 0 && nghttp3_conn_add_write_offset(http3_, stream_id, static_cast<std::size_t>(written)) != 0) {
                failed_ = true;
                return;
            }
            if (size == 0) {
                return;
            }
            std::error_code error;
            socket_.send(asio::buffer(output_.data(), static_cast<std::size_t>(size)), 0, error);
            if (error && error != asio::error::would_block && error != asio::error::try_again) {
                throw std::system_error(error, "HTTP/3 raw client UDP send failed");
            }
        }
    }

    std::chrono::milliseconds timeout_;
    asio::ip::udp::endpoint server_;
    asio::ip::udp::endpoint local_;
    asio::io_context io_;
    asio::ip::udp::socket socket_;
    http3_test_client_tls tls_;
    ngtcp2_conn* connection_{nullptr};
    nghttp3_conn* http3_{nullptr};
    std::array<std::byte, max_udp_payload_bytes> output_{};
    std::vector<std::byte> input_ = std::vector<std::byte>(64 * 1024);
    http3_test_response response_;
    std::int64_t stream_id_{-1};
    bool handshake_completed_{false};
    bool settings_received_{false};
    bool complete_{false};
    bool failed_{false};
};

}  // namespace ruvia::test
