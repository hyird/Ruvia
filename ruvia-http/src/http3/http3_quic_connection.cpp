#include "ruvia/http/http3_quic_connection.h"

#include <nghttp3/nghttp3.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>
namespace ruvia {
namespace {

[[nodiscard]] std::pmr::memory_resource* resolve_resource(
    std::pmr::memory_resource* resource) noexcept {
    return resource == nullptr ? std::pmr::get_default_resource() : resource;
}

[[nodiscard]] http3_connection_options resolve_http3_options(
    const http3_quic_connection_options& options) {
    auto http_options = options.http3_;
    if (http_options.resource_ == nullptr) {
        http_options.resource_ = resolve_resource(options.resource_);
    }
    http_options.own_data_events_ = true;
    return http_options;
}

}  // namespace

http3_quic_datagram::http3_quic_datagram(
    std::pmr::memory_resource* resource)
    : bytes_(resolve_resource(resource)) {
    ngtcp2_path_storage_zero(&path_);
}

class http3_quic_connection::impl final {
public:
    // RFC 9000 §10.2. close_pending: a CONNECTION_CLOSE is owed and the next
    // next_datagram() writes it; closing: it was sent; draining: the peer
    // closed, the connection idled out, or ngtcp2 dropped it, so nothing is
    // sent any more.
    enum class close_state : std::uint8_t {
        open,
        close_pending,
        closing,
        draining,
    };
    struct queued_datagram final {
        std::int64_t stream_id_;
        std::pmr::vector<std::byte> bytes_;
    };

    impl(http3_role role, const http3_quic_connection_options& options)
        : role_(role),
          callback_user_data_(options.callback_user_data_),
          max_udp_payload_size_(options.max_udp_payload_size_),
          max_pending_datagram_bytes_(options.max_pending_datagram_bytes_),
          http3_options_(resolve_http3_options(options)),
          http3_(role == http3_role::client ? http3_connection::client(http3_options_) : http3_connection::server(http3_options_)) {
        ngtcp2_ccerr_default(&close_error_);
        const bool server = role == http3_role::server;
        // A client may choose a zero-length Source Connection ID (RFC 9000
        // §5.1), which a server then addresses its packets to.
        if (options.max_udp_payload_size_ == 0 ||
            options.max_pending_datagram_bytes_ == 0 ||
            options.initial_path_.local.addr == nullptr ||
            options.initial_path_.remote.addr == nullptr ||
            options.source_connection_id_.datalen == 0 ||
            (!server && options.destination_connection_id_.datalen == 0) ||
            options.active_connection_id_limit_ < 2) {
            throw std::invalid_argument("invalid HTTP/3 QUIC connection options");
        }
        if (server != options.original_destination_connection_id_.has_value() ||
            (!server && (options.retry_source_connection_id_.has_value() ||
                            !options.retry_token_.empty() ||
                            options.stateless_reset_token_.has_value() ||
                            options.disable_active_migration_))) {
            throw std::invalid_argument("QUIC server-only transport parameters are mismatched to the role");
        }
        if (server &&
            (options.retry_source_connection_id_.has_value() != !options.retry_token_.empty() ||
                (options.retry_source_connection_id_.has_value() &&
                    options.retry_source_connection_id_->datalen == 0))) {
            throw std::invalid_argument("invalid HTTP/3 QUIC connection options");
        }
        callbacks_ = options.callbacks_;
        get_new_connection_id_ = options.callbacks_.get_new_connection_id2;
        remove_connection_id_ = options.callbacks_.remove_connection_id;
        handshake_completed_ = options.callbacks_.handshake_completed;
        handshake_confirmed_ = options.callbacks_.handshake_confirmed;
        recv_retry_ = options.callbacks_.recv_retry;
        recv_stateless_reset_ = options.callbacks_.recv_stateless_reset2;
        early_data_rejected_ = options.callbacks_.tls_early_data_rejected;
        callbacks_.get_new_connection_id2 =
            get_new_connection_id_ != nullptr ? get_new_connection_id_callback : nullptr;
        callbacks_.remove_connection_id =
            remove_connection_id_ != nullptr ? remove_connection_id_callback : nullptr;
        callbacks_.handshake_completed =
            handshake_completed_ != nullptr ? handshake_completed_callback : nullptr;
        callbacks_.handshake_confirmed =
            handshake_confirmed_ != nullptr ? handshake_confirmed_callback : nullptr;
        callbacks_.recv_retry =
            recv_retry_ != nullptr ? recv_retry_callback : nullptr;
        callbacks_.recv_stateless_reset = nullptr;
        callbacks_.recv_stateless_reset2 = recv_stateless_reset_callback;
        callbacks_.recv_stream_data = recv_stream_data_callback;
        callbacks_.acked_stream_data_offset = acked_stream_data_callback;
        callbacks_.extend_max_stream_data = extend_max_stream_data_callback;
        callbacks_.extend_max_remote_streams_bidi = extend_max_remote_streams_bidi_callback;
        callbacks_.stream_reset = stream_reset_callback;
        callbacks_.stream_stop_sending = stream_stop_sending_callback;
        callbacks_.stream_close = nullptr;
        callbacks_.stream_close2 = stream_close_callback;
        callbacks_.recv_datagram = recv_datagram_callback;
        callbacks_.tls_early_data_rejected = early_data_rejected_callback;

        ngtcp2_settings settings;
        ngtcp2_settings_default(&settings);
        settings.initial_ts = options.initial_timestamp_;
        settings.handshake_timeout = options.handshake_timeout_;
        settings.max_tx_udp_payload_size = options.max_udp_payload_size_;

        ngtcp2_transport_params transport_params;
        ngtcp2_transport_params_default(&transport_params);
        transport_params.initial_max_data = options.initial_max_data_;
        transport_params.initial_max_stream_data_bidi_local =
            options.initial_max_stream_data_;
        transport_params.initial_max_stream_data_bidi_remote =
            options.initial_max_stream_data_;
        transport_params.initial_max_stream_data_uni =
            options.initial_max_stream_data_;
        transport_params.initial_max_streams_bidi =
            options.initial_max_streams_bidi_;
        transport_params.initial_max_streams_uni = options.initial_max_streams_uni_;
        transport_params.max_idle_timeout = options.idle_timeout_;
        transport_params.active_connection_id_limit = options.active_connection_id_limit_;
        transport_params.max_datagram_frame_size = options.http3_.enable_h3_datagram_ ? options.max_udp_payload_size_ : 0;
        if (server) {
            transport_params.original_dcid = *options.original_destination_connection_id_;
            transport_params.original_dcid_present = 1;
            if (options.retry_source_connection_id_) {
                transport_params.retry_scid = *options.retry_source_connection_id_;
                transport_params.retry_scid_present = 1;
                settings.token = options.retry_token_.data();
                settings.tokenlen = options.retry_token_.size();
                settings.token_type = NGTCP2_TOKEN_TYPE_RETRY;
            }
            if (options.stateless_reset_token_) {
                std::memcpy(transport_params.stateless_reset_token,
                    options.stateless_reset_token_->data(), options.stateless_reset_token_->size());
                transport_params.stateless_reset_token_present = 1;
            }
            transport_params.disable_active_migration = options.disable_active_migration_ ? 1 : 0;
        }

        const int result = server
                               ? ngtcp2_conn_server_new(&connection_,
                                     &options.destination_connection_id_,
                                     &options.source_connection_id_,
                                     &options.initial_path_, options.version_,
                                     &callbacks_, &settings, &transport_params,
                                     nullptr, this)
                               : ngtcp2_conn_client_new(&connection_,
                                     &options.destination_connection_id_,
                                     &options.source_connection_id_,
                                     &options.initial_path_, options.version_,
                                     &callbacks_, &settings, &transport_params,
                                     nullptr, this);
        if (result != 0) {
            throw std::invalid_argument("ngtcp2 rejected HTTP/3 connection options");
        }
    }

    ~impl() {
        if (connection_ != nullptr) {
            ngtcp2_conn_del(connection_);
        }
    }

    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;

    [[nodiscard]] http3_quic_result error_result() const noexcept {
        return {application_error_ ? http3_quic_status::application_error
                                   : http3_quic_status::transport_error,
            connection_error_.value_or(0)};
    }

    http3_quic_result fail_transport(int error) noexcept {
        if (state_ != close_state::open) {
            return closed_result();
        }
        connection_error_ =
            static_cast<std::uint64_t>(-static_cast<std::int64_t>(error));
        application_error_ = false;
        if (error == NGTCP2_ERR_CRYPTO) {
            ngtcp2_ccerr_set_tls_alert(&close_error_, ngtcp2_conn_get_tls_alert2(connection_), nullptr, 0);
        } else {
            ngtcp2_ccerr_set_liberr(&close_error_, error, nullptr, 0);
        }
        state_ = close_state::close_pending;
        return error_result();
    }

    http3_quic_result fail_application(std::uint64_t error) noexcept {
        if (state_ != close_state::open) {
            return closed_result();
        }
        connection_error_ = error;
        application_error_ = true;
        ngtcp2_ccerr_set_application_error(&close_error_, error, nullptr, 0);
        state_ = close_state::close_pending;
        return error_result();
    }

    // A read or timer error that ends the connection without a
    // CONNECTION_CLOSE of ours (RFC 9000 §10.1, §10.2.2).
    http3_quic_result enter_draining() noexcept {
        state_ = close_state::draining;
        return closed_result();
    }

    [[nodiscard]] http3_quic_result closed_result() const noexcept {
        return {http3_quic_status::closed, connection_error_.value_or(0)};
    }

    [[nodiscard]] static impl& self(void* user_data) noexcept {
        return *static_cast<impl*>(user_data);
    }

    static int get_new_connection_id_callback(ngtcp2_conn* connection, ngtcp2_cid* cid,
        ngtcp2_stateless_reset_token* token, std::size_t size, void* user_data) noexcept {
        auto& value = self(user_data);
        return value.get_new_connection_id_(connection, cid, token, size, value.callback_user_data_);
    }

    static int remove_connection_id_callback(
        ngtcp2_conn* connection, const ngtcp2_cid* cid, void* user_data) noexcept {
        auto& value = self(user_data);
        return value.remove_connection_id_(connection, cid, value.callback_user_data_);
    }

    static int handshake_completed_callback(ngtcp2_conn* connection, void* user_data) noexcept {
        auto& value = self(user_data);
        return value.handshake_completed_(connection, value.callback_user_data_);
    }

    static int handshake_confirmed_callback(ngtcp2_conn* connection, void* user_data) noexcept {
        auto& value = self(user_data);
        return value.handshake_confirmed_(connection, value.callback_user_data_);
    }

    static int recv_retry_callback(
        ngtcp2_conn* connection, const ngtcp2_pkt_hd* header, void* user_data) noexcept {
        auto& value = self(user_data);
        return value.recv_retry_(connection, header, value.callback_user_data_);
    }

    static int recv_stateless_reset_callback(
        ngtcp2_conn* connection, const ngtcp2_pkt_stateless_reset2* reset, void* user_data) noexcept {
        auto& value = self(user_data);
        value.stateless_reset_received_ = true;
        return value.recv_stateless_reset_ == nullptr
                   ? 0
                   : value.recv_stateless_reset_(connection, reset, value.callback_user_data_);
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

    static int early_data_rejected_callback(ngtcp2_conn* connection, void* user_data) noexcept {
        auto& value = self(user_data);
        try {
            // ngtcp2 has discarded its 0-RTT streams. Reset nghttp3 now,
            // before the same input packet can deliver 1-RTT control streams.
            value.discard_early_http3();
            return value.early_data_rejected_ == nullptr ? 0
                                                         : value.early_data_rejected_(connection, value.callback_user_data_);
        } catch (...) {
            value.callback_error_ = std::current_exception();
            value.fail_application(NGHTTP3_H3_INTERNAL_ERROR);
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
    }

    static int recv_stream_data_callback(ngtcp2_conn* connection,
        std::uint32_t flags, std::int64_t stream_id, std::uint64_t,
        const std::uint8_t* data, std::size_t size, void* user_data,
        void*) noexcept {
        auto& value = self(user_data);
        try {
            if (value.role_ == http3_role::server && (stream_id & 3) == 0 &&
                (flags & NGTCP2_STREAM_DATA_FLAG_0RTT) != 0) {
                value.http3_.mark_early_data(stream_id);
            }
            const auto result = value.http3_.read_stream(stream_id,
                {reinterpret_cast<const std::byte*>(data), size},
                (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0,
                ngtcp2_conn_get_timestamp(connection));
            if (result.status_ != http3_status::accepted) {
                value.fail_application(result.error_code_);
                return NGTCP2_ERR_CALLBACK_FAILURE;
            }
            ngtcp2_conn_extend_max_stream_offset(
                connection, stream_id, result.consumed_bytes_);
            ngtcp2_conn_extend_max_offset(connection, result.consumed_bytes_);
            return 0;
        } catch (...) {
            value.callback_error_ = std::current_exception();
            value.fail_application(NGHTTP3_H3_INTERNAL_ERROR);
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
    }

    static int recv_datagram_callback(ngtcp2_conn*, std::uint32_t,
        const std::uint8_t* data, std::size_t size, void* user_data) noexcept {
        auto& value = self(user_data);
        try {
            const auto result = value.http3_.read_datagram(
                {reinterpret_cast<const std::byte*>(data), size});
            if (result.status_ != http3_status::accepted) {
                value.fail_application(result.error_code_);
                return NGTCP2_ERR_CALLBACK_FAILURE;
            }
            return 0;
        } catch (...) {
            value.callback_error_ = std::current_exception();
            value.fail_application(NGHTTP3_H3_INTERNAL_ERROR);
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
    }

    int fail_from_http3() noexcept {
        fail_application(http3_.connection_error().value_or(NGHTTP3_H3_INTERNAL_ERROR));
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }

    static int acked_stream_data_callback(ngtcp2_conn*, std::int64_t stream_id,
        std::uint64_t, std::uint64_t size, void* user_data, void*) noexcept {
        auto& value = self(user_data);
        return value.http3_.acknowledge_output(stream_id, size) == http3_status::accepted
                   ? 0
                   : value.fail_from_http3();
    }

    static int extend_max_stream_data_callback(ngtcp2_conn*,
        std::int64_t stream_id, std::uint64_t, void* user_data,
        void*) noexcept {
        auto& value = self(user_data);
        return value.http3_.unblock_stream(stream_id) == http3_status::accepted
                   ? 0
                   : value.fail_from_http3();
    }

    static int extend_max_remote_streams_bidi_callback(
        ngtcp2_conn*, std::uint64_t max_streams, void* user_data) noexcept {
        self(user_data).http3_.set_max_client_streams_bidi(max_streams);
        return 0;
    }

    static int stream_reset_callback(ngtcp2_conn*, std::int64_t stream_id,
        std::uint64_t, std::uint64_t, void* user_data, void*) noexcept {
        auto& value = self(user_data);
        return value.http3_.shutdown_stream_read(stream_id) == http3_status::accepted
                   ? 0
                   : value.fail_from_http3();
    }

    static int stream_stop_sending_callback(ngtcp2_conn*,
        std::int64_t stream_id, std::uint64_t, void* user_data,
        void*) noexcept {
        auto& value = self(user_data);
        return value.http3_.shutdown_stream_write(stream_id) == http3_status::accepted
                   ? 0
                   : value.fail_from_http3();
    }

    static int stream_close_callback(ngtcp2_conn* connection, std::uint32_t flags,
        std::int64_t stream_id, std::uint64_t receive_error,
        std::uint64_t send_error, void* user_data, void*) noexcept {
        auto& value = self(user_data);
        const auto status = value.http3_.close_stream(stream_id,
            (flags & NGTCP2_STREAM_CLOSE2_FLAG_RX_APP_ERROR_CODE_SET) != 0
                ? std::optional{receive_error}
                : std::nullopt,
            (flags & NGTCP2_STREAM_CLOSE2_FLAG_TX_APP_ERROR_CODE_SET) != 0
                ? std::optional{send_error}
                : std::nullopt);
        if (status != http3_status::accepted) {
            return value.fail_from_http3();
        }
        // RFC 9000 §4.6: a closed client request stream frees one slot of
        // the peer's bidirectional stream limit.
        if (value.role_ == http3_role::server && ngtcp2_is_bidi_stream(stream_id) != 0 &&
            ngtcp2_conn_is_local_stream2(connection, stream_id) == 0) {
            ngtcp2_conn_extend_max_streams_bidi(connection, 1);
        }
        return 0;
    }

    http3_role role_;
    void* callback_user_data_;
    std::size_t max_udp_payload_size_;
    std::size_t max_pending_datagram_bytes_;
    ngtcp2_callbacks callbacks_{};
    ngtcp2_get_new_connection_id2 get_new_connection_id_{nullptr};
    ngtcp2_remove_connection_id remove_connection_id_{nullptr};
    ngtcp2_handshake_completed handshake_completed_{nullptr};
    ngtcp2_handshake_confirmed handshake_confirmed_{nullptr};
    ngtcp2_recv_retry recv_retry_{nullptr};
    ngtcp2_recv_stateless_reset2 recv_stateless_reset_{nullptr};
    ngtcp2_tls_early_data_rejected early_data_rejected_{nullptr};
    ngtcp2_conn* connection_{nullptr};
    ngtcp2_ccerr close_error_{};
    http3_connection_options http3_options_;
    http3_connection http3_;
    std::optional<std::pmr::deque<queued_datagram>> datagrams_;
    std::size_t pending_datagram_bytes_{0};
    bool datagram_turn_{true};
    bool early_http3_{false};
    bool packet_input_{false};
    bool stateless_reset_received_{false};
    std::array<std::int64_t, 3> control_stream_ids_{};
    std::size_t control_stream_count_{0};
    bool http3_started_{false};
    std::optional<std::uint64_t> connection_error_;
    bool application_error_{false};
    close_state state_{close_state::open};
    std::exception_ptr callback_error_;
};

http3_quic_connection::http3_quic_connection(http3_role role,
    const http3_quic_connection_options& options)
    : impl_(std::make_unique<impl>(role, options)) {}

http3_quic_connection http3_quic_connection::client(
    const http3_quic_connection_options& options) {
    return http3_quic_connection(http3_role::client, options);
}

http3_quic_connection http3_quic_connection::server(
    const http3_quic_connection_options& options) {
    return http3_quic_connection(http3_role::server, options);
}

http3_quic_connection::~http3_quic_connection() = default;
http3_quic_connection::http3_quic_connection(
    http3_quic_connection&&) noexcept = default;
http3_quic_connection& http3_quic_connection::operator=(
    http3_quic_connection&&) noexcept = default;

http3_role http3_quic_connection::role() const noexcept {
    return impl_->role_;
}

ngtcp2_conn* http3_quic_connection::native_handle() noexcept {
    return impl_->connection_;
}

const ngtcp2_conn* http3_quic_connection::native_handle() const noexcept {
    return impl_->connection_;
}

void* http3_quic_connection::callback_user_data() const noexcept {
    return impl_->callback_user_data_;
}

void http3_quic_connection::set_tls_native_handle(
    void* tls_native_handle) noexcept {
    ngtcp2_conn_set_tls_native_handle(impl_->connection_, tls_native_handle);
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
    const auto needed = impl_->control_stream_ids_.size() -
                        impl_->control_stream_count_;
    if (ngtcp2_conn_get_streams_uni_left2(impl_->connection_) < needed) {
        return http3_quic_status::would_block;
    }
    while (impl_->control_stream_count_ < impl_->control_stream_ids_.size()) {
        auto& stream_id =
            impl_->control_stream_ids_[impl_->control_stream_count_];
        const int result = ngtcp2_conn_open_uni_stream(
            impl_->connection_, &stream_id, nullptr);
        if (result != 0) {
            impl_->fail_transport(result);
            return http3_quic_status::transport_error;
        }
        ++impl_->control_stream_count_;
    }
    const auto status = impl_->http3_.bind_control_streams(
        impl_->control_stream_ids_[0], impl_->control_stream_ids_[1],
        impl_->control_stream_ids_[2]);
    if (status != http3_status::accepted) {
        impl_->fail_application(impl_->http3_.connection_error().value_or(
            NGHTTP3_H3_INTERNAL_ERROR));
        return http3_quic_status::application_error;
    }
    impl_->http3_started_ = true;
    return http3_quic_status::accepted;
}

bool http3_quic_connection::http3_started() const noexcept {
    return impl_->http3_started_;
}

std::optional<std::int64_t> http3_quic_connection::open_request_stream() {
    if (impl_->role_ != http3_role::client || !impl_->http3_started_ ||
        impl_->state_ != impl::close_state::open ||
        ngtcp2_conn_get_streams_bidi_left2(impl_->connection_) == 0) {
        return std::nullopt;
    }
    std::int64_t stream_id = -1;
    const int result =
        ngtcp2_conn_open_bidi_stream(impl_->connection_, &stream_id, nullptr);
    if (result == NGTCP2_ERR_STREAM_ID_BLOCKED) {
        return std::nullopt;
    }
    if (result != 0) {
        (void)impl_->fail_transport(result);
        return std::nullopt;
    }
    return stream_id;
}

http3_quic_status http3_quic_connection::start_early_http3(
    std::span<const std::byte> transport_parameters, http3_peer_settings settings) {
    if (impl_->role_ != http3_role::client || impl_->http3_started_ || handshake_completed() ||
        impl_->state_ != impl::close_state::open || transport_parameters.empty()) {
        return http3_quic_status::invalid_state;
    }
    const auto result = ngtcp2_conn_decode_and_set_0rtt_transport_params(impl_->connection_,
        reinterpret_cast<const std::uint8_t*>(transport_parameters.data()), transport_parameters.size());
    if (result != 0) {
        return http3_quic_status::invalid_state;
    }
    impl_->http3_.remember_peer_settings(settings);
    impl_->early_http3_ = true;
    return start_http3();
}

http3_quic_status http3_quic_connection::reject_early_data() {
    if (impl_->role_ != http3_role::client || impl_->state_ != impl::close_state::open) {
        return http3_quic_status::invalid_state;
    }
    if (!impl_->early_http3_) {
        return http3_quic_status::accepted;
    }
    if (!ngtcp2_conn_get_tls_early_data_rejected2(impl_->connection_)) {
        const auto result = ngtcp2_conn_tls_early_data_rejected(impl_->connection_);
        if (result != 0) {
            return impl_->fail_transport(result).status_;
        }
    }
    impl_->discard_early_http3();
    return http3_quic_status::accepted;
}

http3_quic_result http3_quic_connection::migrate(const ngtcp2_path& path, ngtcp2_tstamp timestamp) {
    if (impl_->role_ != http3_role::client || impl_->state_ != impl::close_state::open ||
        path.local.addr == nullptr || path.remote.addr == nullptr) {
        return {http3_quic_status::invalid_state, 0};
    }
    const auto result = ngtcp2_conn_initiate_migration(impl_->connection_, &path, timestamp);
    if (result == 0) {
        return {};
    }
    if (result == NGTCP2_ERR_CONN_ID_BLOCKED) {
        return {http3_quic_status::would_block, 0};
    }
    if (result == NGTCP2_ERR_INVALID_STATE || result == NGTCP2_ERR_INVALID_ARGUMENT) {
        return {http3_quic_status::invalid_state, 0};
    }
    return impl_->fail_transport(result);
}

std::size_t http3_quic_connection::max_datagram_payload_size(std::int64_t stream_id) const noexcept {
    const auto prefix = impl_->http3_.datagram_prefix(stream_id);
    const auto* remote = ngtcp2_conn_get_remote_transport_params2(impl_->connection_);
    // Reserve packet protection, the largest connection ID and DATAGRAM
    // framing. Keeping this below the configured UDP limit avoids fragmentation.
    constexpr std::size_t packet_overhead = 96;
    constexpr std::uint64_t frame_overhead = 9;
    if (!prefix || remote == nullptr || impl_->state_ != impl::close_state::open ||
        impl_->max_udp_payload_size_ <= packet_overhead + prefix->size_ ||
        remote->max_datagram_frame_size <= frame_overhead + prefix->size_) {
        return 0;
    }
    return static_cast<std::size_t>((std::min)(static_cast<std::uint64_t>(impl_->max_udp_payload_size_ - packet_overhead - prefix->size_),
        remote->max_datagram_frame_size - frame_overhead - prefix->size_));
}

http3_quic_status http3_quic_connection::submit_datagram(
    std::int64_t stream_id, std::span<const std::byte> payload) {
    if (impl_->state_ != impl::close_state::open) {
        return http3_quic_status::closed;
    }
    const auto maximum = max_datagram_payload_size(stream_id);
    if (maximum == 0 || payload.size() > maximum) {
        return http3_quic_status::invalid_state;
    }
    const auto prefix = *impl_->http3_.datagram_prefix(stream_id);
    const auto bytes = prefix.size_ + payload.size();
    if ((impl_->datagrams_ && impl_->datagrams_->size() >= 64) ||
        bytes > impl_->max_pending_datagram_bytes_ - impl_->pending_datagram_bytes_) {
        return http3_quic_status::would_block;
    }
    if (!impl_->datagrams_) {
        impl_->datagrams_.emplace(impl_->http3_options_.resource_);
    }
    std::pmr::vector<std::byte> owned(impl_->http3_options_.resource_);
    owned.reserve(bytes);
    owned.insert(owned.end(), prefix.bytes().begin(), prefix.bytes().end());
    owned.insert(owned.end(), payload.begin(), payload.end());
    impl_->datagrams_->push_back({stream_id, std::move(owned)});
    impl_->pending_datagram_bytes_ += bytes;
    return http3_quic_status::accepted;
}

http3_quic_result http3_quic_connection::receive_datagram(
    const ngtcp2_path& path, std::uint32_t ecn,
    std::span<const std::byte> bytes, ngtcp2_tstamp timestamp) {
    if (impl_->state_ == impl::close_state::close_pending) {
        return impl_->error_result();
    }
    if (impl_->state_ != impl::close_state::open) {
        return impl_->closed_result();
    }
    if (impl_->http3_.events_pending()) {
        return {http3_quic_status::invalid_state, 0};
    }
    ngtcp2_pkt_info packet_info{};
    packet_info.ecn = static_cast<std::uint8_t>(ecn);
    impl_->packet_input_ = true;
    impl_->http3_.begin_packet_input();
    const int result = ngtcp2_conn_read_pkt(impl_->connection_, &path,
        &packet_info, reinterpret_cast<const std::uint8_t*>(bytes.data()),
        bytes.size(), timestamp);
    impl_->http3_.end_packet_input();
    impl_->packet_input_ = false;
    if (impl_->callback_error_) {
        auto error = std::exchange(impl_->callback_error_, {});
        std::rethrow_exception(error);
    }
    switch (result) {
        case 0:
            if (const auto settings = impl_->http3_.peer_settings(); settings && settings->h3_datagram_) {
                const auto* params = ngtcp2_conn_get_remote_transport_params2(impl_->connection_);
                if (params == nullptr || params->max_datagram_frame_size == 0) {
                    return impl_->fail_application(NGHTTP3_H3_SETTINGS_ERROR);
                }
            }
            return {http3_quic_status::accepted, 0};
        case NGTCP2_ERR_DRAINING:
        case NGTCP2_ERR_CLOSING:
        case NGTCP2_ERR_DROP_CONN:
            return impl_->enter_draining();
        default:
            break;
    }
    if (impl_->state_ == impl::close_state::close_pending) {
        return impl_->error_result();
    }
    return impl_->fail_transport(result);
}

http3_quic_result http3_quic_connection::next_datagram(
    std::size_t capacity, ngtcp2_tstamp timestamp,
    http3_quic_datagram& output) {
    capacity = (std::min)(capacity, impl_->max_udp_payload_size_);
    if (capacity == 0) {
        return {http3_quic_status::invalid_state, 0};
    }
    output.bytes_.resize(capacity);
    output.ecn_ = NGTCP2_ECN_NOT_ECT;
    ngtcp2_path_storage_zero(&output.path_);

    if (impl_->state_ == impl::close_state::closing ||
        impl_->state_ == impl::close_state::draining) {
        output.bytes_.clear();
        return impl_->closed_result();
    }
    if (impl_->state_ == impl::close_state::close_pending) {
        ngtcp2_pkt_info packet_info{};
        const auto written = ngtcp2_conn_write_connection_close(
            impl_->connection_, &output.path_.path, &packet_info,
            reinterpret_cast<std::uint8_t*>(output.bytes_.data()),
            output.bytes_.size(), &impl_->close_error_, timestamp);
        impl_->state_ = impl::close_state::closing;
        if (written <= 0) {
            output.bytes_.clear();
            return impl_->closed_result();
        }
        output.bytes_.resize(static_cast<std::size_t>(written));
        return {http3_quic_status::accepted, impl_->connection_error_.value_or(0)};
    }

    for (;;) {
        http3_stream_output stream_output;
        const auto pending = impl_->http3_.peek_output(capacity, stream_output);
        ngtcp2_pkt_info packet_info{};
        ngtcp2_ssize written = 0;

        while (impl_->datagrams_ && !impl_->datagrams_->empty() &&
               !impl_->http3_.datagram_prefix(impl_->datagrams_->front().stream_id_)) {
            impl_->pending_datagram_bytes_ -= impl_->datagrams_->front().bytes_.size();
            impl_->datagrams_->pop_front();
        }
        if (impl_->datagrams_ && !impl_->datagrams_->empty() &&
            (impl_->datagram_turn_ || pending.status_ == http3_status::would_block)) {
            auto& datagram = impl_->datagrams_->front().bytes_;
            const ngtcp2_vec vector{reinterpret_cast<std::uint8_t*>(datagram.data()), datagram.size()};
            int accepted = 0;
            written = ngtcp2_conn_writev_datagram(impl_->connection_, &output.path_.path, &packet_info,
                reinterpret_cast<std::uint8_t*>(output.bytes_.data()), output.bytes_.size(), &accepted,
                NGTCP2_WRITE_DATAGRAM_FLAG_NONE, 0, &vector, 1, timestamp);
            if (accepted != 0) {
                impl_->pending_datagram_bytes_ -= datagram.size();
                impl_->datagrams_->pop_front();
            }
            impl_->datagram_turn_ = false;
            if (written < 0) {
                output.bytes_.clear();
                return impl_->fail_transport(static_cast<int>(written));
            }
            if (written > 0) {
                ngtcp2_conn_update_pkt_tx_time(impl_->connection_, timestamp);
                output.ecn_ = packet_info.ecn;
                output.bytes_.resize(static_cast<std::size_t>(written));
                return {};
            }
        }
        impl_->datagram_turn_ = true;
        if (pending.status_ == http3_status::accepted) {
            std::array<ngtcp2_vec, 16> vectors{};
            for (std::size_t index = 0; index < stream_output.buffer_count_;
                ++index) {
                vectors[index] = {
                    const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(
                        stream_output.buffers_[index].data())),
                    stream_output.buffers_[index].size(),
                };
            }
            ngtcp2_ssize accepted = -1;
            const std::uint32_t flags = stream_output.fin_
                                            ? NGTCP2_WRITE_STREAM_FLAG_FIN
                                            : NGTCP2_WRITE_STREAM_FLAG_NONE;
            written = ngtcp2_conn_writev_stream(impl_->connection_,
                &output.path_.path, &packet_info,
                reinterpret_cast<std::uint8_t*>(output.bytes_.data()),
                output.bytes_.size(), &accepted, flags,
                stream_output.stream_id_, vectors.data(),
                stream_output.buffer_count_, timestamp);
            if (written == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
                if (impl_->http3_.block_stream(stream_output.stream_id_) !=
                    http3_status::accepted) {
                    output.bytes_.clear();
                    return impl_->fail_application(
                        impl_->http3_.connection_error().value_or(
                            NGHTTP3_H3_INTERNAL_ERROR));
                }
                continue;
            }
            if (written == NGTCP2_ERR_STREAM_SHUT_WR ||
                written == NGTCP2_ERR_STREAM_NOT_FOUND) {
                if (impl_->http3_.shutdown_stream_write(
                        stream_output.stream_id_) != http3_status::accepted) {
                    output.bytes_.clear();
                    return impl_->fail_application(
                        impl_->http3_.connection_error().value_or(
                            NGHTTP3_H3_INTERNAL_ERROR));
                }
                continue;
            }
            if (written < 0) {
                output.bytes_.clear();
                return impl_->fail_transport(static_cast<int>(written));
            }
            if (accepted >= 0 &&
                impl_->http3_.commit_output(stream_output.stream_id_,
                    static_cast<std::size_t>(accepted)) !=
                    http3_status::accepted) {
                output.bytes_.clear();
                return impl_->fail_application(
                    impl_->http3_.connection_error().value_or(
                        NGHTTP3_H3_INTERNAL_ERROR));
            }
        } else if (pending.status_ == http3_status::would_block) {
            written = ngtcp2_conn_write_pkt(impl_->connection_,
                &output.path_.path, &packet_info,
                reinterpret_cast<std::uint8_t*>(output.bytes_.data()),
                output.bytes_.size(), timestamp);
            if (written < 0) {
                output.bytes_.clear();
                return impl_->fail_transport(static_cast<int>(written));
            }
        } else {
            output.bytes_.clear();
            return impl_->fail_application(pending.error_code_);
        }

        if (written == 0) {
            output.bytes_.clear();
            return {http3_quic_status::would_block, 0};
        }
        ngtcp2_conn_update_pkt_tx_time(impl_->connection_, timestamp);
        output.ecn_ = packet_info.ecn;
        output.bytes_.resize(static_cast<std::size_t>(written));
        return {http3_quic_status::accepted, 0};
    }
}

http3_quic_result http3_quic_connection::handle_expiry(
    ngtcp2_tstamp timestamp) {
    if (impl_->state_ == impl::close_state::close_pending) {
        return impl_->error_result();
    }
    if (impl_->state_ != impl::close_state::open) {
        return impl_->closed_result();
    }
    const int result = ngtcp2_conn_handle_expiry(impl_->connection_, timestamp);
    if (result == 0) {
        return {};
    }
    // RFC 9000 §10.1: an idle timeout closes silently, as does a handshake
    // that never completed within its bound.
    if (result == NGTCP2_ERR_IDLE_CLOSE || result == NGTCP2_ERR_HANDSHAKE_TIMEOUT) {
        return impl_->enter_draining();
    }
    return impl_->fail_transport(result);
}

ngtcp2_tstamp http3_quic_connection::expiry() const noexcept {
    return ngtcp2_conn_get_expiry(impl_->connection_);
}

void http3_quic_connection::consume_stream_data(
    std::int64_t stream_id, std::size_t bytes) noexcept {
    if (bytes == 0 || impl_->state_ != impl::close_state::open) {
        return;
    }
    ngtcp2_conn_extend_max_stream_offset(impl_->connection_, stream_id, bytes);
    ngtcp2_conn_extend_max_offset(impl_->connection_, bytes);
}

void http3_quic_connection::reset_stream(
    std::int64_t stream_id, std::uint64_t application_error_code) noexcept {
    if (impl_->state_ != impl::close_state::open) {
        return;
    }
    // nghttp3 stops producing the stream first, so no frame of it is written
    // after the RESET_STREAM.
    (void)impl_->http3_.shutdown_stream_write(stream_id);
    const int result = ngtcp2_conn_shutdown_stream(impl_->connection_, 0, stream_id, application_error_code);
    if (result != 0 && result != NGTCP2_ERR_STREAM_NOT_FOUND) {
        (void)impl_->fail_transport(result);
    }
}

void http3_quic_connection::stop_receiving(
    std::int64_t stream_id, std::uint64_t application_error_code) noexcept {
    if (impl_->state_ != impl::close_state::open) {
        return;
    }
    const int result = ngtcp2_conn_shutdown_stream_read(impl_->connection_, 0, stream_id, application_error_code);
    if (result != 0 && result != NGTCP2_ERR_STREAM_NOT_FOUND) {
        (void)impl_->fail_transport(result);
        return;
    }
    (void)impl_->http3_.shutdown_stream_read(stream_id);
}

void http3_quic_connection::close(std::uint64_t application_error_code) noexcept {
    if (impl_->state_ != impl::close_state::open) {
        return;
    }
    ngtcp2_ccerr_set_application_error(&impl_->close_error_, application_error_code, nullptr, 0);
    impl_->state_ = impl::close_state::close_pending;
}

bool http3_quic_connection::closed() const noexcept {
    return impl_->state_ != impl::close_state::open;
}

ngtcp2_duration http3_quic_connection::close_period() const noexcept {
    return 3 * ngtcp2_conn_get_pto2(impl_->connection_);
}

std::optional<http3_event> http3_quic_connection::next_event() {
    for (;;) {
        auto event = impl_->http3_.next_event();
        if (!event) {
            return std::nullopt;
        }
        if (const auto* deferred =
                std::get_if<http3_deferred_consume_event>(&*event)) {
            consume_stream_data(deferred->stream_id_, deferred->bytes_);
            continue;
        }
        if (const auto* action =
                std::get_if<http3_transport_action_event>(&*event)) {
            const int result = action->action_ ==
                                       http3_transport_action::stop_sending
                                   ? ngtcp2_conn_shutdown_stream_read(
                                         impl_->connection_, 0,
                                         action->stream_id_,
                                         action->application_error_code_)
                                   : ngtcp2_conn_shutdown_stream_write(
                                         impl_->connection_, 0,
                                         action->stream_id_,
                                         action->application_error_code_);
            if (result != 0 && result != NGTCP2_ERR_STREAM_NOT_FOUND) {
                impl_->fail_transport(result);
            }
        }
        return event;
    }
}

http3_connection& http3_quic_connection::http3() noexcept {
    return impl_->http3_;
}

const http3_connection& http3_quic_connection::http3() const noexcept {
    return impl_->http3_;
}

std::optional<std::uint64_t>
http3_quic_connection::connection_error() const noexcept {
    if (impl_->connection_error_) {
        return impl_->connection_error_;
    }
    return impl_->http3_.connection_error();
}

std::size_t write_http3_quic_stateless_reset(
    const http3_quic_stateless_reset_token& token,
    std::span<const std::uint8_t> random_bytes,
    std::span<std::byte> output,
    std::size_t max_packet_bytes) noexcept {
    constexpr std::size_t min_packet_bytes =
        1 + NGTCP2_MIN_STATELESS_RESET_RANDLEN + NGTCP2_STATELESS_RESET_TOKENLEN;
    const std::size_t bound = (std::min)(output.size(), max_packet_bytes);
    if (bound < min_packet_bytes ||
        random_bytes.size() < NGTCP2_MIN_STATELESS_RESET_RANDLEN) {
        return 0;
    }
    const std::size_t random_len =
        (std::min)(random_bytes.size(), bound - 1 - NGTCP2_STATELESS_RESET_TOKENLEN);
    if (random_len < NGTCP2_MIN_STATELESS_RESET_RANDLEN) {
        return 0;
    }
    ngtcp2_stateless_reset_token native_token{};
    std::memcpy(native_token.data, token.data(), token.size());
    const auto written = ngtcp2_pkt_write_stateless_reset2(
        reinterpret_cast<std::uint8_t*>(output.data()), bound, &native_token,
        random_bytes.data(), random_len);
    return written > 0 ? static_cast<std::size_t>(written) : 0;
}

}  // namespace ruvia
