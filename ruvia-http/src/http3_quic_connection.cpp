#include "http3_quic_connection.h"

#include <ngtcp2/ngtcp2.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "http_c_library_memory.h"

namespace ruvia {

static_assert(quic_version_v1 == NGTCP2_PROTO_VER_V1);
static_assert(quic_version_v2 == NGTCP2_PROTO_VER_V2);
static_assert(quic_max_cid_bytes == NGTCP2_MAX_CIDLEN);
static_assert(quic_min_initial_dcid_bytes == NGTCP2_MIN_INITIAL_DCIDLEN);
static_assert(quic_stateless_reset_token_bytes == NGTCP2_STATELESS_RESET_TOKENLEN);
static_assert(quic_min_stateless_reset_random_bytes == NGTCP2_MIN_STATELESS_RESET_RANDLEN);

namespace {

[[nodiscard]] ngtcp2_cid to_native_cid(const quic_cid& cid) noexcept {
    ngtcp2_cid native{};
    native.datalen = cid.size();
    if (native.datalen != 0) {
        std::memcpy(native.data, cid.data(), native.datalen);
    }
    return native;
}

[[nodiscard]] quic_cid from_native_cid(const ngtcp2_cid& cid) noexcept {
    return quic_cid::try_from_bytes(byte_view(reinterpret_cast<const std::byte*>(cid.data), cid.datalen)).value_or(quic_cid{});
}

[[nodiscard]] ngtcp2_duration to_native_duration(std::chrono::nanoseconds value) noexcept {
    if (value.count() <= 0) {
        return 0;
    }
    return static_cast<ngtcp2_duration>(value.count());
}

struct native_path_buffer final {
    sockaddr_storage local_storage_{};
    sockaddr_storage remote_storage_{};
    ngtcp2_path path_{};

    [[nodiscard]] static ngtcp2_socklen encode_endpoint(const ip_endpoint& endpoint, sockaddr_storage& storage) noexcept {
        std::memset(&storage, 0, sizeof(storage));
        const auto addr = endpoint.address();
        const auto bytes = addr.bytes();
        if (addr.family() == ip_address_family::ipv4) {
            auto* sin = reinterpret_cast<sockaddr_in*>(&storage);
            sin->sin_family = AF_INET;
            sin->sin_port = htons(endpoint.port().value());
            std::memcpy(&sin->sin_addr, bytes.data(), bytes.size());
            return static_cast<ngtcp2_socklen>(sizeof(sockaddr_in));
        }
        auto* sin6 = reinterpret_cast<sockaddr_in6*>(&storage);
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(endpoint.port().value());
        sin6->sin6_scope_id = addr.scope_id();
        std::memcpy(&sin6->sin6_addr, bytes.data(), bytes.size());
        return static_cast<ngtcp2_socklen>(sizeof(sockaddr_in6));
    }

    explicit native_path_buffer(const http3_quic_path& path) noexcept {
        const auto local_len = encode_endpoint(path.local_, local_storage_);
        const auto remote_len = encode_endpoint(path.remote_, remote_storage_);
        path_.local.addr = reinterpret_cast<ngtcp2_sockaddr*>(&local_storage_);
        path_.local.addrlen = local_len;
        path_.remote.addr = reinterpret_cast<ngtcp2_sockaddr*>(&remote_storage_);
        path_.remote.addrlen = remote_len;
        path_.user_data = reinterpret_cast<void*>(path.user_data_);
    }
};

[[nodiscard]] ip_endpoint decode_native_endpoint(const ngtcp2_addr& address) noexcept {
    if (address.addr == nullptr || address.addrlen == 0) {
        return {};
    }
    if (address.addr->sa_family == AF_INET && address.addrlen >= sizeof(sockaddr_in)) {
        sockaddr_in sin{};
        std::memcpy(&sin, address.addr, sizeof(sin));
        std::array<std::uint8_t, 4> octets{};
        std::memcpy(octets.data(), &sin.sin_addr, octets.size());
        return ip_endpoint{ip_address::ipv4(octets), port_number{ntohs(sin.sin_port)}};
    }
    if (address.addr->sa_family == AF_INET6 && address.addrlen >= sizeof(sockaddr_in6)) {
        sockaddr_in6 sin6{};
        std::memcpy(&sin6, address.addr, sizeof(sin6));
        std::array<std::uint8_t, 16> octets{};
        std::memcpy(octets.data(), &sin6.sin6_addr, octets.size());
        return ip_endpoint{ip_address::ipv6(octets, sin6.sin6_scope_id), port_number{ntohs(sin6.sin6_port)}};
    }
    return {};
}

[[nodiscard]] http3_quic_path from_native_path(const ngtcp2_path& path) noexcept {
    return {
        .local_ = decode_native_endpoint(path.local),
        .remote_ = decode_native_endpoint(path.remote),
        .user_data_ = reinterpret_cast<std::uintptr_t>(path.user_data),
    };
}

[[nodiscard]] http3_connection_options resolve_http3_options(const http3_quic_connection_options& options, http3_received_data_credit_sink_type sink, void* context) {
    if (options.http3_.received_data_credit_sink_ != nullptr || options.http3_.received_data_credit_context_ != nullptr) {
        throw std::invalid_argument("HTTP/3 QUIC connection owns the received DATA credit sink");
    }
    auto http_options = options.http3_;
    http_options.own_data_events_ = true;
    http_options.received_data_credit_sink_ = sink;
    http_options.received_data_credit_context_ = context;
    return http_options;
}

}  // namespace

class http3_quic_connection::impl final {
public:
    enum class close_state : std::uint8_t {
        open,
        close_pending,
        closing,
        draining,
    };
    struct queued_datagram final {
        std::int64_t stream_id_;
        std::vector<std::byte> bytes_;
    };

    impl(http_connection_role role, const http3_quic_connection_options& options) :
        role_(role),
        callback_context_(options.callback_context_),
        max_udp_payload_size_(options.max_udp_payload_size_),
        max_pending_datagram_bytes_(options.max_pending_datagram_bytes_),
        memory_table_(detail::make_c_library_mem_table<ngtcp2_mem>(memory_)),
        http3_options_(resolve_http3_options(options, return_received_data_credit, this)),
        http3_(role == http_connection_role::client ? http3_connection::client(http3_options_) : http3_connection::server(http3_options_)) {
        ngtcp2_ccerr_default(&close_error_);
        const bool server = role == http_connection_role::server;
        if (options.max_udp_payload_size_ == 0 || options.max_pending_datagram_bytes_ == 0 || options.source_connection_id_.empty() || (!server && options.destination_connection_id_.empty()) || options.active_connection_id_limit_ < 2) {
            throw std::invalid_argument("invalid HTTP/3 QUIC connection options");
        }
        if (server != options.original_destination_connection_id_.has_value() || (!server && (options.retry_source_connection_id_.has_value() || !options.retry_token_.empty() || options.stateless_reset_token_.has_value() || options.disable_active_migration_))) {
            throw std::invalid_argument("QUIC server-only transport parameters are mismatched to the role");
        }
        if (server && (options.retry_source_connection_id_.has_value() != !options.retry_token_.empty() || (options.retry_source_connection_id_.has_value() && options.retry_source_connection_id_->empty()))) {
            throw std::invalid_argument("invalid HTTP/3 QUIC connection options");
        }
        if (options.configure_crypto_callbacks_ != nullptr) {
            options.configure_crypto_callbacks_(callbacks_);
        }
        owner_callbacks_ = options.owner_callbacks_;
        callbacks_.get_new_connection_id2 = owner_callbacks_.get_new_connection_id_ != nullptr ? +[](ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token, std::size_t cidlen, void* u) noexcept -> int {
            auto& v = self(u);
            quic_cid issued;
            http3_quic_stateless_reset_token reset_token{};
            if (!v.owner_callbacks_.get_new_connection_id_(issued, reset_token, cidlen, v.callback_context_) || issued.size() != cidlen) {
                return NGTCP2_ERR_CALLBACK_FAILURE;
            }
            *cid = to_native_cid(issued);
            std::memcpy(token->data, reset_token.data(), reset_token.size());
            return 0;
        }                                                                                   : nullptr;
        callbacks_.remove_connection_id = owner_callbacks_.remove_connection_id_ != nullptr ? +[](ngtcp2_conn*, const ngtcp2_cid* cid, void* u) noexcept -> int {
            auto& v = self(u);
            const auto converted = from_native_cid(*cid);
            return v.owner_callbacks_.remove_connection_id_(converted, v.callback_context_) ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
        }                                                                                 : nullptr;
        callbacks_.handshake_completed = owner_callbacks_.handshake_completed_ != nullptr ? +[](ngtcp2_conn*, void* u) noexcept -> int {
            auto& v = self(u);
            return v.owner_callbacks_.handshake_completed_(v.callback_context_) ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
        }                                                                                 : nullptr;
        callbacks_.handshake_confirmed = owner_callbacks_.handshake_confirmed_ != nullptr ? +[](ngtcp2_conn*, void* u) noexcept -> int {
            auto& v = self(u);
            return v.owner_callbacks_.handshake_confirmed_(v.callback_context_) ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
        } : nullptr;
        callbacks_.recv_stateless_reset2 = recv_stateless_reset_callback;
        callbacks_.recv_stream_data = recv_stream_data_callback;
        callbacks_.acked_stream_data_offset = acked_stream_data_callback;
        callbacks_.extend_max_stream_data = extend_max_stream_data_callback;
        callbacks_.extend_max_remote_streams_bidi = extend_max_remote_streams_bidi_callback;
        callbacks_.stream_reset = stream_reset_callback;
        callbacks_.recv_stop_sending = stream_stop_sending_callback;
        callbacks_.stream_close2 = stream_close_callback;
        callbacks_.recv_datagram = recv_datagram_callback;
        callbacks_.tls_early_data_rejected = early_data_rejected_callback;

        ngtcp2_settings settings;
        ngtcp2_settings_default(&settings);
        settings.initial_ts = options.initial_timestamp_;
        settings.handshake_timeout = to_native_duration(options.handshake_timeout_);
        settings.max_tx_udp_payload_size = options.max_udp_payload_size_;

        ngtcp2_transport_params transport_params;
        ngtcp2_transport_params_default(&transport_params);
        transport_params.initial_max_data = options.initial_max_data_;
        transport_params.initial_max_stream_data_bidi_local = options.initial_max_stream_data_;
        transport_params.initial_max_stream_data_bidi_remote = options.initial_max_stream_data_;
        transport_params.initial_max_stream_data_uni = options.initial_max_stream_data_;
        transport_params.initial_max_streams_bidi = options.initial_max_streams_bidi_;
        transport_params.initial_max_streams_uni = options.initial_max_streams_uni_;
        transport_params.max_idle_timeout = to_native_duration(options.idle_timeout_);
        transport_params.active_connection_id_limit = options.active_connection_id_limit_;
        transport_params.max_datagram_frame_size = options.http3_.enable_h3_datagram_ ? options.max_udp_payload_size_ : 0;
        if (server) {
            transport_params.original_dcid = to_native_cid(*options.original_destination_connection_id_);
            transport_params.original_dcid_present = 1;
            if (options.retry_source_connection_id_) {
                transport_params.retry_scid = to_native_cid(*options.retry_source_connection_id_);
                transport_params.retry_scid_present = 1;
                settings.token = reinterpret_cast<const std::uint8_t*>(options.retry_token_.data());
                settings.tokenlen = options.retry_token_.size();
                settings.token_type = NGTCP2_TOKEN_TYPE_RETRY;
            }
            if (options.stateless_reset_token_) {
                std::memcpy(transport_params.stateless_reset_token, options.stateless_reset_token_->data(), options.stateless_reset_token_->size());
                transport_params.stateless_reset_token_present = 1;
            }
            transport_params.disable_active_migration = options.disable_active_migration_ ? 1 : 0;
        }

        const auto native_scid = to_native_cid(options.source_connection_id_);
        const auto native_dcid = to_native_cid(options.destination_connection_id_);
        const native_path_buffer initial_path(options.initial_path_);
        const int result = server ? ngtcp2_conn_server_new(&connection_, &native_dcid, &native_scid, &initial_path.path_, options.version_, &callbacks_, &settings, &transport_params, &memory_table_, this) : ngtcp2_conn_client_new(&connection_, &native_dcid, &native_scid, &initial_path.path_, options.version_, &callbacks_, &settings, &transport_params, &memory_table_, this);
        if (result != 0) {
            if (result == NGTCP2_ERR_NOMEM) {
                memory_.rethrow_allocation_failure();
            }
            memory_.rethrow_if_failed();
            throw std::invalid_argument("ngtcp2 rejected HTTP/3 connection options");
        }
    }

    ~impl() {
        if (connection_ != nullptr) {
            ngtcp2_conn_del(std::exchange(connection_, nullptr));
        }
    }

    void extend_receive_windows(std::int64_t stream_id, std::size_t bytes) noexcept {
        if (bytes == 0 || state_ != close_state::open || connection_ == nullptr) {
            return;
        }
        ngtcp2_conn_extend_max_stream_offset(connection_, stream_id, bytes);
        ngtcp2_conn_extend_max_offset(connection_, bytes);
    }

    static void return_received_data_credit(void* context, std::int64_t stream_id, std::size_t bytes) noexcept {
        static_cast<impl*>(context)->extend_receive_windows(stream_id, bytes);
    }

    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;

    [[nodiscard]] http3_quic_result error_result() const noexcept {
        return {.status_ = application_error_ ? http3_quic_status::application_error : http3_quic_status::transport_error, .transport_error_code_ = transport_error_, .application_error_code_ = application_error_};
    }

    http3_quic_result fail_transport(int error) noexcept {
        if (state_ == close_state::close_pending) {
            return error_result();
        }
        if (state_ != close_state::open) {
            return closed_result();
        }
        if (error == NGTCP2_ERR_CRYPTO) {
            ngtcp2_ccerr_set_tls_alert(&close_error_, ngtcp2_conn_get_tls_alert2(connection_), nullptr, 0);
        } else {
            ngtcp2_ccerr_set_liberr(&close_error_, error, nullptr, 0);
        }
        transport_error_ = close_error_.error_code;
        state_ = close_state::close_pending;
        return error_result();
    }

    http3_quic_result fail_application(http3_error_code error) noexcept {
        if (state_ == close_state::close_pending) {
            return error_result();
        }
        if (state_ != close_state::open) {
            return closed_result();
        }
        application_error_ = error;
        ngtcp2_ccerr_set_application_error(&close_error_, static_cast<std::uint64_t>(error), nullptr, 0);
        state_ = close_state::close_pending;
        return error_result();
    }

    http3_quic_result enter_draining() noexcept {
        state_ = close_state::draining;
        return closed_result();
    }

    http3_quic_result enter_peer_draining() noexcept {
        if (!transport_error_ && !application_error_ && !stateless_reset_received_) {
            const auto* received = ngtcp2_conn_get_ccerr2(connection_);
            if (received->type == NGTCP2_CCERR_TYPE_APPLICATION) {
                application_error_ = static_cast<http3_error_code>(received->error_code);
            } else if (received->type == NGTCP2_CCERR_TYPE_TRANSPORT) {
                transport_error_ = received->error_code;
            }
        }
        return enter_draining();
    }

    [[nodiscard]] http3_quic_result closed_result() const noexcept {
        return {.status_ = http3_quic_status::closed, .transport_error_code_ = transport_error_, .application_error_code_ = application_error_};
    }
    void rethrow_if_failed(int result = 0) {
        if (callback_error_) {
            auto error = std::exchange(callback_error_, {});
            std::rethrow_exception(error);
        }
        http3_.rethrow_if_failed();
        if (result == NGTCP2_ERR_NOMEM) {
            memory_.rethrow_allocation_failure();
        }
        memory_.rethrow_if_failed();
    }

    [[nodiscard]] static impl& self(void* user_data) noexcept {
        return *static_cast<impl*>(user_data);
    }
    [[nodiscard]] std::optional<http3_quic_result> check_open() const noexcept {
        if (state_ == close_state::close_pending) {
            return error_result();
        }
        if (state_ != close_state::open) {
            return closed_result();
        }
        return std::nullopt;
    }

    static int recv_stateless_reset_callback(ngtcp2_conn*, const ngtcp2_pkt_stateless_reset2* reset, void* user_data) noexcept {
        auto& value = self(user_data);
        value.stateless_reset_received_ = true;
        if (value.owner_callbacks_.recv_stateless_reset_ == nullptr) {
            return 0;
        }
        http3_quic_stateless_reset_token token{};
        std::memcpy(token.data(), reset->token.data, token.size());
        return value.owner_callbacks_.recv_stateless_reset_(token, value.callback_context_) ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
    }

    void discard_early_http3() {
        if (!early_http3_) {
            return;
        }
        auto replacement = http3_connection::client(http3_options_);
        if (packet_input_) {
            replacement.begin_packet_input();
        }
        http3_ = std::move(replacement);
        control_stream_count_ = 0;
        http3_started_ = false;
        early_http3_ = false;
        datagrams_.reset();
        pending_datagram_bytes_ = 0;
    }

    static int early_data_rejected_callback(ngtcp2_conn*, void* user_data) noexcept {
        auto& value = self(user_data);
        try {
            value.discard_early_http3();
            return value.owner_callbacks_.tls_early_data_rejected_ == nullptr || value.owner_callbacks_.tls_early_data_rejected_(value.callback_context_) ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
        } catch (...) {
            value.callback_error_ = std::current_exception();
            value.fail_application(http3_error_code::internal_error);
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
    }

    static int recv_stream_data_callback(ngtcp2_conn* connection, std::uint32_t flags, std::int64_t stream_id, std::uint64_t, const std::uint8_t* data, std::size_t size, void* user_data, void*) noexcept {
        auto& value = self(user_data);
        try {
            if (value.role_ == http_connection_role::server && (stream_id & 3) == 0 && (flags & NGTCP2_STREAM_DATA_FLAG_0RTT) != 0) {
                value.http3_.mark_early_data(stream_id);
            }
            const auto result = value.http3_.read_stream(stream_id, {reinterpret_cast<const std::byte*>(data), size}, (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0, ngtcp2_conn_get_timestamp(connection));
            if (result.status_ != http3_transport_status::accepted) {
                value.fail_application(result.error_code_.value_or(http3_error_code::internal_error));
                return NGTCP2_ERR_CALLBACK_FAILURE;
            }
            ngtcp2_conn_extend_max_stream_offset(connection, stream_id, result.credit_bytes_);
            ngtcp2_conn_extend_max_offset(connection, result.credit_bytes_);
            return 0;
        } catch (...) {
            value.callback_error_ = std::current_exception();
            value.fail_application(http3_error_code::internal_error);
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
    }

    static int recv_datagram_callback(ngtcp2_conn*, std::uint32_t, const std::uint8_t* data, std::size_t size, void* user_data) noexcept {
        auto& value = self(user_data);
        try {
            const auto result = value.http3_.read_datagram({reinterpret_cast<const std::byte*>(data), size});
            if (result.status_ != http3_transport_status::accepted) {
                value.fail_application(result.error_code_.value_or(http3_error_code::internal_error));
                return NGTCP2_ERR_CALLBACK_FAILURE;
            }
            return 0;
        } catch (...) {
            value.callback_error_ = std::current_exception();
            value.fail_application(http3_error_code::internal_error);
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
    }

    int fail_from_http3() noexcept {
        fail_application(http3_.connection_error().value_or(http3_error_code::internal_error));
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    static int acked_stream_data_callback(ngtcp2_conn*, std::int64_t stream_id, std::uint64_t, std::uint64_t size, void* user_data, void*) noexcept {
        auto& value = self(user_data);
        return value.http3_.acknowledge_output(stream_id, size) == http3_transport_status::accepted ? 0 : value.fail_from_http3();
    }

    static int extend_max_stream_data_callback(ngtcp2_conn*, std::int64_t stream_id, std::uint64_t, void* user_data, void*) noexcept {
        auto& value = self(user_data);
        return value.http3_.unblock_stream(stream_id) == http3_transport_status::accepted ? 0 : value.fail_from_http3();
    }

    static int extend_max_remote_streams_bidi_callback(ngtcp2_conn*, std::uint64_t max_streams, void* user_data) noexcept {
        self(user_data).http3_.set_max_client_streams_bidi(max_streams);
        return 0;
    }

    static int stream_reset_callback(ngtcp2_conn*, std::int64_t stream_id, std::uint64_t, std::uint64_t app_error_code, void* user_data, void*) noexcept {
        auto& value = self(user_data);
        return value.http3_.shutdown_stream_read(stream_id, static_cast<http3_error_code>(app_error_code)) == http3_transport_status::accepted ? 0 : value.fail_from_http3();
    }

    static int stream_stop_sending_callback(ngtcp2_conn* connection, std::int64_t stream_id, std::uint64_t app_error_code, void* user_data, void*) noexcept {
        auto& value = self(user_data);
        const int result = ngtcp2_conn_shutdown_stream_write(connection, 0, stream_id, app_error_code);
        if (result != 0 && result != NGTCP2_ERR_STREAM_NOT_FOUND) {
            value.fail_transport(result);
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
        return value.http3_.shutdown_stream_write(stream_id, static_cast<http3_error_code>(app_error_code)) == http3_transport_status::accepted ? 0 : value.fail_from_http3();
    }

    static int stream_close_callback(ngtcp2_conn* connection, std::uint32_t flags, std::int64_t stream_id, std::uint64_t receive_error, std::uint64_t send_error, void* user_data, void*) noexcept {
        auto& value = self(user_data);
        const auto status = value.http3_.close_stream(stream_id, (flags & NGTCP2_STREAM_CLOSE2_FLAG_RX_APP_ERROR_CODE_SET) != 0 ? std::optional{static_cast<http3_error_code>(receive_error)} : std::nullopt, (flags & NGTCP2_STREAM_CLOSE2_FLAG_TX_APP_ERROR_CODE_SET) != 0 ? std::optional{static_cast<http3_error_code>(send_error)} : std::nullopt);
        if (status != http3_transport_status::accepted) {
            return value.fail_from_http3();
        }
        if (value.role_ == http_connection_role::server && ngtcp2_is_bidi_stream(stream_id) != 0 && ngtcp2_conn_is_local_stream2(connection, stream_id) == 0) {
            ngtcp2_conn_extend_max_streams_bidi(connection, 1);
        }
        return 0;
    }

    http_connection_role role_;
    void* callback_context_;
    std::size_t max_udp_payload_size_;
    std::size_t max_pending_datagram_bytes_;
    detail::http_c_library_memory memory_;
    ngtcp2_mem memory_table_;
    ngtcp2_callbacks callbacks_{};
    http3_quic_owner_callbacks owner_callbacks_{};
    ngtcp2_conn* connection_{nullptr};
    ngtcp2_ccerr close_error_{};
    http3_connection_options http3_options_;
    http3_connection http3_;
    std::optional<std::deque<queued_datagram>> datagrams_;
    std::size_t pending_datagram_bytes_{0};
    bool datagram_turn_{true};
    bool early_http3_{false};
    bool packet_input_{false};
    bool stateless_reset_received_{false};
    std::array<std::int64_t, 3> control_stream_ids_{};
    std::size_t control_stream_count_{0};
    bool http3_started_{false};
    std::optional<std::uint64_t> transport_error_;
    std::optional<http3_error_code> application_error_;
    close_state state_{close_state::open};
    std::exception_ptr callback_error_;
};

http3_quic_connection::http3_quic_connection(http_connection_role role, const http3_quic_connection_options& options) :
    impl_(std::make_unique<impl>(role, options)) {}

http3_quic_connection http3_quic_connection::client(const http3_quic_connection_options& options) {
    return http3_quic_connection(http_connection_role::client, options);
}

http3_quic_connection http3_quic_connection::server(const http3_quic_connection_options& options) {
    return http3_quic_connection(http_connection_role::server, options);
}

http3_quic_connection::~http3_quic_connection() = default;
http3_quic_connection::http3_quic_connection(http3_quic_connection&&) noexcept = default;
http3_quic_connection& http3_quic_connection::operator=(http3_quic_connection&&) noexcept = default;

http_connection_role http3_quic_connection::role() const noexcept {
    return impl_->role_;
}

ngtcp2_conn* http3_quic_connection::native() & noexcept {
    return impl_->connection_;
}

const ngtcp2_conn* http3_quic_connection::native() const& noexcept {
    return impl_->connection_;
}

void* http3_quic_connection::callback_context() const noexcept {
    return impl_->callback_context_;
}

void http3_quic_connection::set_tls_session(void* tls_session) noexcept {
    ngtcp2_conn_set_tls_native_handle(impl_->connection_, tls_session);
}

bool http3_quic_connection::handshake_completed() const noexcept {
    return ngtcp2_conn_get_handshake_completed2(impl_->connection_) != 0;
}

bool http3_quic_connection::after_retry() const noexcept {
    return ngtcp2_conn_after_retry2(impl_->connection_) != 0;
}

bool http3_quic_connection::stateless_reset_received() const noexcept {
    return impl_->stateless_reset_received_;
}

http3_quic_status http3_quic_connection::start_http3() {
    if (impl_->state_ != impl::close_state::open) {
        return http3_quic_status::closed;
    }
    if (impl_->http3_started_) {
        return http3_quic_status::accepted;
    }
    const auto needed = impl_->control_stream_ids_.size() - impl_->control_stream_count_;
    if (ngtcp2_conn_get_streams_uni_left2(impl_->connection_) < needed) {
        return http3_quic_status::would_block;
    }
    while (impl_->control_stream_count_ < impl_->control_stream_ids_.size()) {
        auto& stream_id = impl_->control_stream_ids_[impl_->control_stream_count_];
        const int result = ngtcp2_conn_open_uni_stream(impl_->connection_, &stream_id, nullptr);
        if (result != 0) {
            impl_->rethrow_if_failed(result);
            impl_->fail_transport(result);
            return http3_quic_status::transport_error;
        }
        ++impl_->control_stream_count_;
    }
    const auto status = impl_->http3_.bind_control_streams(impl_->control_stream_ids_[0], impl_->control_stream_ids_[1], impl_->control_stream_ids_[2]);
    if (status != http3_transport_status::accepted) {
        impl_->fail_application(impl_->http3_.connection_error().value_or(http3_error_code::internal_error));
        return http3_quic_status::application_error;
    }
    impl_->http3_started_ = true;
    return http3_quic_status::accepted;
}

bool http3_quic_connection::http3_started() const noexcept {
    return impl_->http3_started_;
}

std::optional<std::int64_t> http3_quic_connection::open_request_stream() {
    if (impl_->role_ != http_connection_role::client || !impl_->http3_started_ || impl_->state_ != impl::close_state::open || ngtcp2_conn_get_streams_bidi_left2(impl_->connection_) == 0) {
        return std::nullopt;
    }
    std::int64_t stream_id = -1;
    const int result = ngtcp2_conn_open_bidi_stream(impl_->connection_, &stream_id, nullptr);
    if (result == NGTCP2_ERR_STREAM_ID_BLOCKED) {
        return std::nullopt;
    }
    if (result != 0) {
        impl_->rethrow_if_failed(result);
        (void)impl_->fail_transport(result);
        return std::nullopt;
    }
    return stream_id;
}

http3_quic_status http3_quic_connection::start_early_http3(byte_view transport_parameters, http3_peer_settings settings) {
    if (impl_->role_ != http_connection_role::client || impl_->http3_started_ || handshake_completed() || impl_->state_ != impl::close_state::open || transport_parameters.empty()) {
        return http3_quic_status::invalid_state;
    }
    const auto result = ngtcp2_conn_decode_and_set_0rtt_transport_params(impl_->connection_, reinterpret_cast<const std::uint8_t*>(transport_parameters.data()), transport_parameters.size());
    if (result != 0) {
        impl_->rethrow_if_failed(result);
        return http3_quic_status::invalid_state;
    }
    const auto status = start_http3();
    if (status == http3_quic_status::accepted) {
        impl_->http3_.remember_peer_settings(settings);
        impl_->early_http3_ = true;
    }
    return status;
}

http3_quic_status http3_quic_connection::reject_early_data() {
    if (impl_->role_ != http_connection_role::client || impl_->state_ != impl::close_state::open) {
        return http3_quic_status::invalid_state;
    }
    if (!impl_->early_http3_) {
        return http3_quic_status::accepted;
    }
    if (!ngtcp2_conn_get_tls_early_data_rejected2(impl_->connection_)) {
        const auto result = ngtcp2_conn_tls_early_data_rejected(impl_->connection_);
        if (result != 0) {
            impl_->rethrow_if_failed(result);
            return impl_->fail_transport(result).status_;
        }
    }
    impl_->discard_early_http3();
    return http3_quic_status::accepted;
}

bool http3_quic_connection::early_data_rejected() const noexcept {
    return ngtcp2_conn_get_tls_early_data_rejected2(impl_->connection_) != 0;
}

std::optional<std::size_t> http3_quic_connection::encode_early_transport_parameters(mutable_byte_view output) const noexcept {
    if (impl_->role_ == http_connection_role::client && ngtcp2_conn_get_remote_transport_params2(impl_->connection_) == nullptr) {
        return std::nullopt;
    }
    const auto size = ngtcp2_conn_encode_0rtt_transport_params2(impl_->connection_, reinterpret_cast<std::uint8_t*>(output.data()), output.size());
    if (size < 0) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(size);
}

http3_quic_path http3_quic_connection::current_path() const noexcept {
    return from_native_path(*ngtcp2_conn_get_path2(impl_->connection_));
}

http3_quic_result http3_quic_connection::migrate(const http3_quic_path& path, quic_timestamp timestamp) {
    if (impl_->role_ != http_connection_role::client || impl_->state_ != impl::close_state::open) {
        return {.status_ = http3_quic_status::invalid_state};
    }
    const native_path_buffer native_path(path);
    const auto result = ngtcp2_conn_initiate_migration(impl_->connection_, &native_path.path_, timestamp);
    impl_->rethrow_if_failed(result);
    if (result == 0) {
        return {};
    }
    if (result == NGTCP2_ERR_CONN_ID_BLOCKED) {
        return {.status_ = http3_quic_status::would_block};
    }
    if (result == NGTCP2_ERR_INVALID_STATE || result == NGTCP2_ERR_INVALID_ARGUMENT) {
        return {.status_ = http3_quic_status::invalid_state};
    }
    return impl_->fail_transport(result);
}

std::size_t http3_quic_connection::max_datagram_payload_size(std::int64_t stream_id) const noexcept {
    const auto prefix = impl_->http3_.datagram_prefix(stream_id);
    const auto* remote = ngtcp2_conn_get_remote_transport_params2(impl_->connection_);
    constexpr std::size_t packet_overhead = 96;
    constexpr std::uint64_t frame_overhead = 9;
    if (!prefix || remote == nullptr || impl_->state_ != impl::close_state::open || impl_->max_udp_payload_size_ <= packet_overhead + prefix->size_ || remote->max_datagram_frame_size <= frame_overhead + prefix->size_) {
        return 0;
    }
    return static_cast<std::size_t>((std::min)(static_cast<std::uint64_t>(impl_->max_udp_payload_size_ - packet_overhead - prefix->size_), remote->max_datagram_frame_size - frame_overhead - prefix->size_));
}

http3_quic_status http3_quic_connection::submit_datagram(std::int64_t stream_id, byte_view payload) {
    if (impl_->state_ != impl::close_state::open) {
        return http3_quic_status::closed;
    }
    const auto maximum = max_datagram_payload_size(stream_id);
    if (maximum == 0 || payload.size() > maximum) {
        return http3_quic_status::invalid_state;
    }
    const auto prefix = *impl_->http3_.datagram_prefix(stream_id);
    const auto bytes = prefix.size_ + payload.size();
    if ((impl_->datagrams_ && impl_->datagrams_->size() >= 64) || bytes > impl_->max_pending_datagram_bytes_ - impl_->pending_datagram_bytes_) {
        return http3_quic_status::would_block;
    }
    if (!impl_->datagrams_) {
        impl_->datagrams_.emplace();
    }
    std::vector<std::byte> owned;
    owned.reserve(bytes);
    owned.insert(owned.end(), prefix.bytes().begin(), prefix.bytes().end());
    owned.insert(owned.end(), payload.begin(), payload.end());
    impl_->datagrams_->push_back({stream_id, std::move(owned)});
    impl_->pending_datagram_bytes_ += bytes;
    return http3_quic_status::accepted;
}

http3_quic_result http3_quic_connection::receive_datagram(const http3_quic_path& path, std::uint32_t ecn, byte_view bytes, quic_timestamp timestamp) {
    if (const auto closed = impl_->check_open()) {
        return *closed;
    }
    if (impl_->http3_.events_pending()) {
        return {.status_ = http3_quic_status::invalid_state};
    }
    const native_path_buffer native_path(path);
    ngtcp2_pkt_info packet_info{};
    packet_info.ecn = static_cast<std::uint8_t>(ecn);
    impl_->packet_input_ = true;
    impl_->http3_.begin_packet_input();
    const int result = ngtcp2_conn_read_pkt(impl_->connection_, &native_path.path_, &packet_info, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), timestamp);
    impl_->http3_.end_packet_input();
    impl_->packet_input_ = false;
    impl_->rethrow_if_failed(result);
    switch (result) {
        case 0:
            if (const auto settings = impl_->http3_.peer_settings(); settings && settings->h3_datagram_) {
                const auto* params = ngtcp2_conn_get_remote_transport_params2(impl_->connection_);
                if (params == nullptr || params->max_datagram_frame_size == 0) {
                    return impl_->fail_application(http3_error_code::settings_error);
                }
            }
            return {};
        case NGTCP2_ERR_DRAINING:
            return impl_->enter_peer_draining();
        case NGTCP2_ERR_CLOSING:
        case NGTCP2_ERR_DROP_CONN:
            return impl_->enter_draining();
        default:
            break;
    }
    return impl_->fail_transport(result);
}

http3_quic_result http3_quic_connection::next_datagram(std::size_t capacity, quic_timestamp timestamp, http3_quic_datagram& output) {
    capacity = (std::min)(capacity, impl_->max_udp_payload_size_);
    if (capacity == 0) {
        return {.status_ = http3_quic_status::invalid_state};
    }
    output.bytes_.clear();
    output.ecn_ = NGTCP2_ECN_NOT_ECT;
    output.path_ = {};

    if (impl_->state_ == impl::close_state::closing || impl_->state_ == impl::close_state::draining) {
        return impl_->closed_result();
    }
    ngtcp2_path_storage path_storage{};
    ngtcp2_path_storage_zero(&path_storage);
    output.bytes_.resize(capacity);
    try {
        if (impl_->state_ == impl::close_state::close_pending) {
            ngtcp2_pkt_info packet_info{};
            const auto written = ngtcp2_conn_write_connection_close(impl_->connection_, &path_storage.path, &packet_info, reinterpret_cast<std::uint8_t*>(output.bytes_.data()), output.bytes_.size(), &impl_->close_error_, timestamp);
            impl_->state_ = impl::close_state::closing;
            impl_->rethrow_if_failed(written < 0 ? static_cast<int>(written) : 0);
            if (written <= 0) {
                output.bytes_.clear();
                return impl_->closed_result();
            }
            output.ecn_ = packet_info.ecn;
            output.path_ = from_native_path(path_storage.path);
            output.bytes_.resize(static_cast<std::size_t>(written));
            return {.transport_error_code_ = impl_->transport_error_, .application_error_code_ = impl_->application_error_};
        }

        const auto fail_transport_pkt = [&](int error) noexcept {
            output.bytes_.clear();
            return impl_->fail_transport(error);
        };
        const auto fail_app_pkt = [&](std::optional<http3_error_code> error = std::nullopt) noexcept {
            output.bytes_.clear();
            return impl_->fail_application(error.value_or(impl_->http3_.connection_error().value_or(http3_error_code::internal_error)));
        };

        for (;;) {
            http3_stream_output stream_output;
            const auto pending = impl_->http3_started_ ? impl_->http3_.peek_output(capacity, stream_output) : http3_transport_result{.status_ = http3_transport_status::would_block};
            ngtcp2_pkt_info packet_info{};
            ngtcp2_ssize written = 0;

            if (impl_->http3_started_) {
                while (impl_->datagrams_ && !impl_->datagrams_->empty() && !impl_->http3_.datagram_prefix(impl_->datagrams_->front().stream_id_)) {
                    impl_->pending_datagram_bytes_ -= impl_->datagrams_->front().bytes_.size();
                    impl_->datagrams_->pop_front();
                }
            }
            if (impl_->http3_started_ && impl_->datagrams_ && !impl_->datagrams_->empty() && (impl_->datagram_turn_ || pending.status_ == http3_transport_status::would_block)) {
                auto& datagram = impl_->datagrams_->front().bytes_;
                const ngtcp2_vec vector{reinterpret_cast<std::uint8_t*>(datagram.data()), datagram.size()};
                int accepted = 0;
                written = ngtcp2_conn_writev_datagram(impl_->connection_, &path_storage.path, &packet_info, reinterpret_cast<std::uint8_t*>(output.bytes_.data()), output.bytes_.size(), &accepted, NGTCP2_WRITE_DATAGRAM_FLAG_NONE, 0, &vector, 1, timestamp);
                if (accepted != 0) {
                    impl_->pending_datagram_bytes_ -= datagram.size();
                    impl_->datagrams_->pop_front();
                }
                impl_->datagram_turn_ = false;
                impl_->rethrow_if_failed(written < 0 ? static_cast<int>(written) : 0);
                if (written < 0) {
                    return fail_transport_pkt(static_cast<int>(written));
                }
                if (written > 0) {
                    ngtcp2_conn_update_pkt_tx_time(impl_->connection_, timestamp);
                    output.ecn_ = packet_info.ecn;
                    output.path_ = from_native_path(path_storage.path);
                    output.bytes_.resize(static_cast<std::size_t>(written));
                    return {};
                }
            }
            impl_->datagram_turn_ = true;
            if (pending.status_ == http3_transport_status::accepted) {
                std::array<ngtcp2_vec, 16> vectors{};
                for (std::size_t index = 0; index < stream_output.buffer_count_; ++index) {
                    vectors[index] = {
                        const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(stream_output.buffers_[index].data())),
                        stream_output.buffers_[index].size(),
                    };
                }
                ngtcp2_ssize accepted = -1;
                const std::uint32_t flags = stream_output.fin_ ? NGTCP2_WRITE_STREAM_FLAG_FIN : NGTCP2_WRITE_STREAM_FLAG_NONE;
                written = ngtcp2_conn_writev_stream(impl_->connection_, &path_storage.path, &packet_info, reinterpret_cast<std::uint8_t*>(output.bytes_.data()), output.bytes_.size(), &accepted, flags, stream_output.stream_id_, vectors.data(), stream_output.buffer_count_, timestamp);
                impl_->rethrow_if_failed(written < 0 ? static_cast<int>(written) : 0);
                if (written == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
                    if (impl_->http3_.block_stream(stream_output.stream_id_) != http3_transport_status::accepted) {
                        return fail_app_pkt();
                    }
                    continue;
                }
                if (written == NGTCP2_ERR_STREAM_SHUT_WR || written == NGTCP2_ERR_STREAM_NOT_FOUND) {
                    if (impl_->http3_.shutdown_stream_write(stream_output.stream_id_) != http3_transport_status::accepted) {
                        return fail_app_pkt();
                    }
                    continue;
                }
                if (written < 0) {
                    return fail_transport_pkt(static_cast<int>(written));
                }
                if (accepted >= 0 && impl_->http3_.commit_output(stream_output.stream_id_, static_cast<std::size_t>(accepted)) != http3_transport_status::accepted) {
                    return fail_app_pkt();
                }
            } else if (pending.status_ == http3_transport_status::would_block) {
                written = ngtcp2_conn_write_pkt(impl_->connection_, &path_storage.path, &packet_info, reinterpret_cast<std::uint8_t*>(output.bytes_.data()), output.bytes_.size(), timestamp);
                impl_->rethrow_if_failed(written < 0 ? static_cast<int>(written) : 0);
                if (written < 0) {
                    return fail_transport_pkt(static_cast<int>(written));
                }
            } else {
                return fail_app_pkt(pending.error_code_);
            }

            if (written == 0) {
                output.bytes_.clear();
                return {.status_ = http3_quic_status::would_block};
            }
            ngtcp2_conn_update_pkt_tx_time(impl_->connection_, timestamp);
            output.ecn_ = packet_info.ecn;
            output.path_ = from_native_path(path_storage.path);
            output.bytes_.resize(static_cast<std::size_t>(written));
            return {};
        }
    } catch (...) {
        output.bytes_.clear();
        throw;
    }
}

http3_quic_result http3_quic_connection::expire_timers(quic_timestamp timestamp) {
    if (const auto closed = impl_->check_open()) {
        return *closed;
    }
    const int result = ngtcp2_conn_handle_expiry(impl_->connection_, timestamp);
    impl_->rethrow_if_failed(result);
    if (result == 0) {
        return {};
    }
    if (result == NGTCP2_ERR_IDLE_CLOSE || result == NGTCP2_ERR_HANDSHAKE_TIMEOUT) {
        return impl_->enter_draining();
    }
    return impl_->fail_transport(result);
}

quic_timestamp http3_quic_connection::expiry() const noexcept {
    return ngtcp2_conn_get_expiry(impl_->connection_);
}

void http3_quic_connection::reset_stream(std::int64_t stream_id, http3_error_code error) noexcept {
    if (impl_->state_ != impl::close_state::open) {
        return;
    }
    (void)impl_->http3_.shutdown_stream_write(stream_id);
    (void)impl_->http3_.shutdown_stream_read(stream_id);
    const int result = ngtcp2_conn_shutdown_stream(impl_->connection_, 0, stream_id, static_cast<std::uint64_t>(error));
    if (result != 0 && result != NGTCP2_ERR_STREAM_NOT_FOUND) {
        (void)impl_->fail_transport(result);
    }
}

void http3_quic_connection::reset_send_stream(std::int64_t stream_id, http3_error_code error) noexcept {
    if (impl_->state_ != impl::close_state::open) {
        return;
    }
    (void)impl_->http3_.shutdown_stream_write(stream_id);
    const int result = ngtcp2_conn_shutdown_stream_write(impl_->connection_, 0, stream_id, static_cast<std::uint64_t>(error));
    if (result != 0 && result != NGTCP2_ERR_STREAM_NOT_FOUND) {
        (void)impl_->fail_transport(result);
    }
}

void http3_quic_connection::stop_receiving(std::int64_t stream_id, http3_error_code error) noexcept {
    if (impl_->state_ != impl::close_state::open) {
        return;
    }
    const int result = ngtcp2_conn_shutdown_stream_read(impl_->connection_, 0, stream_id, static_cast<std::uint64_t>(error));
    if (result != 0 && result != NGTCP2_ERR_STREAM_NOT_FOUND) {
        (void)impl_->fail_transport(result);
        return;
    }
    (void)impl_->http3_.shutdown_stream_read(stream_id);
}

void http3_quic_connection::close(http3_error_code error) noexcept {
    if (impl_->state_ != impl::close_state::open) {
        return;
    }
    impl_->application_error_ = error;
    ngtcp2_ccerr_set_application_error(&impl_->close_error_, static_cast<std::uint64_t>(error), nullptr, 0);
    impl_->state_ = impl::close_state::close_pending;
}

bool http3_quic_connection::closed() const noexcept {
    return impl_->state_ != impl::close_state::open;
}

std::chrono::nanoseconds http3_quic_connection::close_period() const noexcept {
    const auto pto = ngtcp2_conn_get_pto2(impl_->connection_);
    const auto clamped = pto > UINT64_MAX / 3 ? UINT64_MAX : 3 * pto;
    return std::chrono::nanoseconds(static_cast<std::int64_t>((std::min)(clamped, static_cast<ngtcp2_duration>(INT64_MAX))));
}

std::optional<http3_event> http3_quic_connection::next_event() & {
    for (;;) {
        auto event = impl_->http3_.next_event();
        if (!event) {
            return std::nullopt;
        }
        if (const auto* deferred = event->deferred_consume()) {
            impl_->extend_receive_windows(deferred->stream_id(), deferred->consumed_bytes());
            continue;
        }
        if (const auto* action = event->transport_action(); action != nullptr && action->origin() == http3_transport_action_origin::local) {
            const auto error = static_cast<std::uint64_t>(action->error());
            const int result = action->action() == http3_transport_action::stop_sending ? ngtcp2_conn_shutdown_stream_read(impl_->connection_, 0, action->stream_id(), error) : ngtcp2_conn_shutdown_stream_write(impl_->connection_, 0, action->stream_id(), error);
            if (result != 0 && result != NGTCP2_ERR_STREAM_NOT_FOUND) {
                impl_->fail_transport(result);
            }
        }
        return event;
    }
}

http3_connection& http3_quic_connection::http3() & noexcept {
    return impl_->http3_;
}

const http3_connection& http3_quic_connection::http3() const& noexcept {
    return impl_->http3_;
}

std::optional<http3_error_code> http3_quic_connection::application_error() const noexcept {
    if (impl_->application_error_) {
        return impl_->application_error_;
    }
    return impl_->http3_.connection_error();
}

std::optional<std::uint64_t> http3_quic_connection::transport_error() const noexcept {
    return impl_->transport_error_;
}

quic_packet_version_cid decode_quic_packet_version_cid(byte_view packet, std::size_t short_dcid_size) noexcept {
    ngtcp2_version_cid header{};
    const auto decoded = ngtcp2_pkt_decode_version_cid(&header, reinterpret_cast<const std::uint8_t*>(packet.data()), packet.size(), short_dcid_size);
    if ((decoded != 0 && decoded != NGTCP2_ERR_VERSION_NEGOTIATION) || header.dcidlen > NGTCP2_MAX_CIDLEN || header.scidlen > NGTCP2_MAX_CIDLEN) {
        return {};
    }
    return {
        .status_ = decoded == NGTCP2_ERR_VERSION_NEGOTIATION ? quic_packet_version_status::version_negotiation : quic_packet_version_status::ok,
        .version_ = header.version,
        .dcid_ = byte_view(reinterpret_cast<const std::byte*>(header.dcid), header.dcidlen),
        .scid_ = byte_view(reinterpret_cast<const std::byte*>(header.scid), header.scidlen),
    };
}

std::optional<quic_initial_packet_header> decode_quic_initial_packet(byte_view packet) noexcept {
    ngtcp2_pkt_hd header{};
    if (ngtcp2_accept(&header, reinterpret_cast<const std::uint8_t*>(packet.data()), packet.size()) != 0) {
        return std::nullopt;
    }
    return quic_initial_packet_header{
        .version_ = header.version,
        .dcid_ = from_native_cid(header.dcid),
        .scid_ = from_native_cid(header.scid),
        .token_ = header.token != nullptr ? byte_view(reinterpret_cast<const std::byte*>(header.token), header.tokenlen) : byte_view{},
    };
}

std::size_t write_quic_version_negotiation_packet(mutable_byte_view output, std::uint8_t unused_random, byte_view dcid, byte_view scid, std::span<const std::uint32_t> supported_versions) noexcept {
    const auto written = ngtcp2_pkt_write_version_negotiation(reinterpret_cast<std::uint8_t*>(output.data()), output.size(), unused_random, reinterpret_cast<const std::uint8_t*>(dcid.data()), dcid.size(), reinterpret_cast<const std::uint8_t*>(scid.data()), scid.size(), supported_versions.data(), supported_versions.size());
    return written > 0 ? static_cast<std::size_t>(written) : 0;
}

std::size_t write_http3_quic_stateless_reset(const http3_quic_stateless_reset_token& token, byte_view random_bytes, mutable_byte_view output, std::size_t max_packet_bytes) noexcept {
    constexpr std::size_t min_packet_bytes = 1 + NGTCP2_MIN_STATELESS_RESET_RANDLEN + NGTCP2_STATELESS_RESET_TOKENLEN;
    const std::size_t bound = (std::min)(output.size(), max_packet_bytes);
    if (bound < min_packet_bytes || random_bytes.size() < NGTCP2_MIN_STATELESS_RESET_RANDLEN) {
        return 0;
    }
    const std::size_t random_len = (std::min)(random_bytes.size(), bound - 1 - NGTCP2_STATELESS_RESET_TOKENLEN);
    if (random_len < NGTCP2_MIN_STATELESS_RESET_RANDLEN) {
        return 0;
    }
    ngtcp2_stateless_reset_token native_token{};
    std::memcpy(native_token.data, token.data(), token.size());
    const auto written = ngtcp2_pkt_write_stateless_reset2(reinterpret_cast<std::uint8_t*>(output.data()), bound, &native_token, reinterpret_cast<const std::uint8_t*>(random_bytes.data()), random_len);
    return written > 0 ? static_cast<std::size_t>(written) : 0;
}

}  // namespace ruvia
