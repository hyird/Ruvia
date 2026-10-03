#include "ruvia/http/quic_connection.h"

#include <ngtcp2/ngtcp2.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <limits>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include "ruvia/http/detail/http3/quic_address_codec.h"
#include "ruvia/http/detail/http3/quic_connection_state.h"
#include "ruvia/http/detail/http3/quic_crypto_bridge.h"
#include "ruvia/http/detail/http3/quic_stream.h"

namespace ruvia {
namespace {

using state_owner = std::unique_ptr<detail::quic_connection_state,
    detail::quic_connection_state_deleter>;

class server_cid_publication_transaction final {
public:
    explicit server_cid_publication_transaction(detail::quic_connection_state& state) noexcept
        : state_(state),
          registry_(state.server_cid_registry_),
          first_(state.server_cid_publication_journal_.ids.size()) {}

    server_cid_publication_transaction(const server_cid_publication_transaction&) = delete;
    server_cid_publication_transaction& operator=(const server_cid_publication_transaction&) = delete;

    ~server_cid_publication_transaction() noexcept {
        auto& ids = state_.server_cid_publication_journal_.ids;
        for (auto it = ids.begin() + static_cast<std::ptrdiff_t>(first_); it != ids.end(); ++it) {
            detail::quic_retire_connection_id(registry_, it->view());
        }
        ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(first_), ids.end());
    }

    void commit() noexcept {
        auto& ids = state_.server_cid_publication_journal_.ids;
        ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(first_), ids.end());
    }

private:
    detail::quic_connection_state& state_;
    detail::quic_cid_registry_view registry_;
    std::size_t first_{};
};

std::uint64_t timestamp_value(quic_timestamp value) {
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
    if (nanos < 0) {
        throw std::invalid_argument("QUIC timestamp must not precede the steady-clock epoch");
    }
    return static_cast<std::uint64_t>(nanos);
}

ngtcp2_cid native_cid(const quic_connection_id& id) {
    ngtcp2_cid result{};
    result.datalen = id.size();
    if (id.size() != 0) {
        std::memcpy(result.data, id.view().data(), id.size());
    }
    return result;
}

void fill_transport_params(ngtcp2_transport_params& params,
    const detail::quic_connection_state& state) {
    ngtcp2_transport_params_default(&params);
    const auto& source = state.config_.local_transport_parameters;
    params.initial_max_data = source.initial_max_data;
    params.initial_max_stream_data_bidi_local = source.initial_max_stream_data_bidi_local;
    params.initial_max_stream_data_bidi_remote = source.initial_max_stream_data_bidi_remote;
    params.initial_max_stream_data_uni = source.initial_max_stream_data_uni;
    params.initial_max_streams_bidi = source.initial_max_streams_bidi;
    params.initial_max_streams_uni = source.initial_max_streams_uni;
    params.max_idle_timeout = source.idle_timeout_ms * NGTCP2_MILLISECONDS;
    params.max_udp_payload_size = source.max_udp_payload_size;
    params.max_datagram_frame_size = source.max_datagram_frame_size;
    params.active_connection_id_limit = source.active_connection_id_limit;
    params.disable_active_migration = source.disable_active_migration;
    if (state.config_.role == quic_role::server) {
        // ngtcp2 derives initial_scid from the scid argument and requires this input field unset.
        const auto original_dcid = native_cid(*state.config_.original_destination_connection_id);
        params.original_dcid = original_dcid;
        params.original_dcid_present = 1;
    }
}

struct alignas(std::max_align_t) allocation_header {
    std::size_t size;
};

detail::quic_connection_state* allocator_state(void* user_data) noexcept {
    return static_cast<detail::quic_connection_state*>(user_data);
}

void* native_allocate(std::size_t size, void* user_data) noexcept {
    if (size > std::numeric_limits<std::size_t>::max() - sizeof(allocation_header)) {
        return nullptr;
    }
    auto* state = allocator_state(user_data);
    try {
        auto* header = static_cast<allocation_header*>(state->resource_->allocate(
            sizeof(allocation_header) + size, alignof(std::max_align_t)));
        header->size = size;
        return header + 1;
    } catch (...) {
        state->latch_failure(std::current_exception());
        return nullptr;
    }
}

void native_free(void* pointer, void* user_data) noexcept {
    if (!pointer) {
        return;
    }
    auto* state = allocator_state(user_data);
    auto* header = static_cast<allocation_header*>(pointer) - 1;
    state->resource_->deallocate(header, sizeof(allocation_header) + header->size,
        alignof(std::max_align_t));
}

void* native_calloc(std::size_t count, std::size_t size, void* user_data) noexcept {
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        return nullptr;
    }
    const auto bytes = count * size;
    auto* memory = native_allocate(bytes, user_data);
    if (memory && bytes) {
        std::memset(memory, 0, bytes);
    }
    return memory;
}

void* native_realloc(void* pointer, std::size_t size, void* user_data) noexcept {
    if (!pointer) {
        return native_allocate(size, user_data);
    }
    if (size == 0) {
        native_free(pointer, user_data);
        return nullptr;
    }
    auto* old_header = static_cast<allocation_header*>(pointer) - 1;
    const auto old_size = old_header->size;
    auto* replacement = native_allocate(size, user_data);
    if (!replacement) {
        return nullptr;
    }
    std::memcpy(replacement, pointer, std::min(old_size, size));
    native_free(pointer, user_data);
    return replacement;
}

int path_challenge_callback(ngtcp2_conn*, ngtcp2_path_challenge_data* data,
    void* user_data) noexcept {
    auto* state = allocator_state(user_data);
    try {
        state->crypto_.random_bytes(state->crypto_.context,
            std::span<std::byte>(reinterpret_cast<std::byte*>(data->data), sizeof(data->data)));
        return 0;
    } catch (...) {
        state->latch_failure(std::current_exception());
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
}

int get_new_cid_callback(ngtcp2_conn*, ngtcp2_cid* cid,
    ngtcp2_stateless_reset_token* token, std::size_t cid_length,
    void* user_data) noexcept {
    auto* state = allocator_state(user_data);
    try {
        if (cid_length > sizeof(cid->data)) {
            throw quic_error(quic_error_code::resource_limit, "ngtcp2 requested an oversized connection ID");
        }
        state->crypto_.random_bytes(state->crypto_.context,
            std::span<std::byte>(reinterpret_cast<std::byte*>(cid->data), cid_length));
        state->crypto_.random_bytes(state->crypto_.context,
            std::span<std::byte>(reinterpret_cast<std::byte*>(token->data), sizeof(token->data)));
        cid->datalen = cid_length;
        if (state->config_.role == quic_role::server && state->server_cid_registry_) {
            const auto bytes = std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(cid->data), cid->datalen);
            detail::quic_publish_connection_id(state->server_cid_registry_, bytes);
            try {
                state->server_cid_publication_journal_.ids.emplace_back(bytes);
            } catch (...) {
                detail::quic_retire_connection_id(state->server_cid_registry_, bytes);
                throw;
            }
        }
        return 0;
    } catch (...) {
        state->latch_failure(std::current_exception());
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
}

int remove_cid_callback(ngtcp2_conn*, const ngtcp2_cid* cid, void* user_data) noexcept {
    auto* state = allocator_state(user_data);
    try {
        if (!state || !cid || cid->datalen > quic_max_connection_id_size) {
            throw std::invalid_argument("ngtcp2 supplied an invalid retired local CID");
        }
        if (state->config_.role == quic_role::server && state->server_cid_registry_) {
            detail::quic_retire_connection_id(state->server_cid_registry_,
                {reinterpret_cast<const std::byte*>(cid->data), cid->datalen});
        }
        return 0;
    } catch (...) {
        if (state) {
            state->latch_failure(std::current_exception());
        }
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
}

state_owner make_state(quic_connection_config config, quic_crypto_provider_view crypto,
    quic_tls_driver_view driver, std::pmr::memory_resource* resource, quic_timestamp now,
    std::span<const std::byte> early_transport_parameters = {}) {
    crypto.validate();
    if (!resource) {
        throw std::invalid_argument("QUIC connection requires a memory resource");
    }
    auto* selected = resource;
    std::pmr::polymorphic_allocator<detail::quic_connection_state> allocator(selected);
    auto* state = allocator.allocate(1);
    try {
        std::construct_at(state, std::move(config), crypto, driver, selected, now,
            early_transport_parameters);
    } catch (...) {
        allocator.deallocate(state, 1);
        throw;
    }
    return state_owner(state, detail::quic_connection_state_deleter{selected});
}

int path_validation_callback(ngtcp2_conn*, uint32_t, const ngtcp2_path* path,
    const ngtcp2_path*, ngtcp2_path_validation_result result, void* user_data) {
    auto& state = *static_cast<detail::quic_connection_state*>(user_data);
    if (path != nullptr) {
        state.on_path_validation(*path, result);
    }
    return 0;
}

void initialize_native_connection(detail::quic_connection_state& state, quic_timestamp now) {
    const auto ts = timestamp_value(now);
    ngtcp2_callbacks callbacks{};
    detail::fill_quic_crypto_callbacks(callbacks);
    detail::configure_quic_stream_callbacks(callbacks);
    callbacks.get_new_connection_id2 = get_new_cid_callback;
    callbacks.remove_connection_id = remove_cid_callback;
    callbacks.get_path_challenge_data2 = path_challenge_callback;
    callbacks.path_validation = path_validation_callback;

    ngtcp2_settings settings{};
    ngtcp2_settings_default(&settings);
    const auto preferred_version = static_cast<std::uint32_t>(state.config_.preferred_version);
    const auto initial_version = static_cast<std::uint32_t>(state.config_.version);
    // RFC 9368 compatible versions can negotiate in-place; incompatible versions require
    // a fresh TLS/QUIC connection and are intentionally not advertised here.
    static constexpr std::array<std::uint32_t, 2> v1_first_versions{
        NGTCP2_PROTO_VER_V1, NGTCP2_PROTO_VER_V2};
    static constexpr std::array<std::uint32_t, 2> v2_first_versions{
        NGTCP2_PROTO_VER_V2, NGTCP2_PROTO_VER_V1};
    const auto& preferred_versions = preferred_version == NGTCP2_PROTO_VER_V1
                                         ? v1_first_versions
                                         : v2_first_versions;
    static constexpr std::array<std::uint32_t, 2> available_versions{
        NGTCP2_PROTO_VER_V1, NGTCP2_PROTO_VER_V2};
    settings.preferred_versions = preferred_versions.data();
    settings.preferred_versionslen = state.config_.role == quic_role::client ||
                                             preferred_version != initial_version
                                         ? preferred_versions.size()
                                         : 1;
    settings.available_versions = available_versions.data();
    settings.available_versionslen = available_versions.size();
    settings.original_version = state.config_.role == quic_role::client
                                    ? static_cast<std::uint32_t>(state.config_.version)
                                    : 0;
    settings.initial_ts = ts;
    settings.max_tx_udp_payload_size = state.config_.local_transport_parameters.max_udp_payload_size;
    settings.max_window = state.config_.limits.max_connection_buffer_size;
    settings.max_stream_window = state.config_.limits.max_stream_buffer_size;
    detail::initialize_quic_random_context(settings.rand_ctx, state);

    ngtcp2_transport_params params{};
    fill_transport_params(params, state);
    detail::fill_quic_path(state.path_, state.config_.local_address, state.config_.peer_address);
    auto dcid = native_cid(state.config_.destination_connection_id);
    auto scid = native_cid(*state.config_.source_connection_id);
    state.ngtcp_memory_ = {&state, native_allocate, native_free, native_calloc, native_realloc};
    int result{};
    if (state.config_.role == quic_role::client) {
        result = ngtcp2_conn_client_new(&state.connection_, &dcid, &scid,
            &state.path_.path, static_cast<std::uint32_t>(state.config_.version),
            &callbacks, &settings, &params, &state.ngtcp_memory_, &state);
    } else {
        result = ngtcp2_conn_server_new(&state.connection_, &dcid, &scid,
            &state.path_.path, static_cast<std::uint32_t>(state.config_.version),
            &callbacks, &settings, &params, &state.ngtcp_memory_, &state);
    }
    if (result != 0) {
        detail::rethrow_quic_callback_failure(state);
        throw quic_error(result == NGTCP2_ERR_NOMEM ? quic_error_code::resource_limit
                                                    : quic_error_code::protocol_failure,
            std::string("failed to initialize ngtcp2 connection: ") + ngtcp2_strerror(result));
    }
    if (state.config_.role == quic_role::client) {
        detail::install_quic_initial_keys(state, state.config_.destination_connection_id.view());
        detail::encode_quic_local_transport_parameters(state);
    } else {
        // Seed the TLS capability before ngtcp2 commits server parameters at Handshake keys.
        auto local_params = params;
        local_params.initial_scid = scid;
        local_params.initial_scid_present = 1;
        std::array<std::uint8_t, 4096> encoded{};
        const auto encoded_size = ngtcp2_transport_params_encode(
            encoded.data(), encoded.size(), &local_params);
        if (encoded_size < 0) {
            throw quic_error(quic_error_code::protocol_failure,
                "failed to encode provisional server QUIC transport parameters");
        }
        std::pmr::vector<std::byte> owned(state.resource_);
        owned.reserve(static_cast<std::size_t>(encoded_size));
        for (ngtcp2_ssize index = 0; index < encoded_size; ++index) {
            owned.push_back(static_cast<std::byte>(encoded[static_cast<std::size_t>(index)]));
        }
        state.local_transport_parameters_.swap(owned);
    }
}

quic_operation_status idle_status(const detail::quic_connection_state& state) noexcept {
    switch (state.state_) {
        case quic_connection_state::closing:
            return quic_operation_status::closing;
        case quic_connection_state::draining:
            return quic_operation_status::draining;
        case quic_connection_state::retired:
            return quic_operation_status::retired;
        case quic_connection_state::connecting:
        case quic_connection_state::ready:
        case quic_connection_state::failed:
            return quic_operation_status::need_input;
    }
    return quic_operation_status::need_input;
}

quic_error_code native_error_category(int result) noexcept {
    if (result == NGTCP2_ERR_NOMEM || result == NGTCP2_ERR_CRYPTO_BUFFER_EXCEEDED) {
        return quic_error_code::resource_limit;
    }
    if (result == NGTCP2_ERR_CRYPTO || result == NGTCP2_ERR_AEAD_LIMIT_REACHED) {
        return quic_error_code::crypto_failure;
    }
    return quic_error_code::protocol_failure;
}

void check_native_result(detail::quic_connection_state& state, int result,
    const char* operation) {
    detail::rethrow_quic_callback_failure(state);
    if (result == 0) {
        return;
    }
    if (result == NGTCP2_ERR_DRAINING) {
        state.close_error_code_ = ngtcp2_conn_get_ccerr2(state.connection_)->error_code;
        state.state_ = quic_connection_state::draining;
        return;
    }
    if (result == NGTCP2_ERR_CLOSING) {
        state.state_ = quic_connection_state::closing;
        return;
    }
    if (result == NGTCP2_ERR_DROP_CONN || result == NGTCP2_ERR_IDLE_CLOSE) {
        state.retire();
        return;
    }
    state.latch_failure(std::make_exception_ptr(quic_error(native_error_category(result),
        std::string(operation) + ": " + ngtcp2_strerror(result))));
    state.rethrow_failure();
}

state_owner make_connection(quic_connection_config config, quic_crypto_provider_view crypto,
    quic_tls_driver_view tls_driver, std::pmr::memory_resource* resource, quic_timestamp now,
    std::span<const std::byte> early_transport_parameters = {}) {
    crypto.validate();
    tls_driver.validate();
    switch (config.role) {
        case quic_role::client:
            if (config.original_destination_connection_id) {
                throw std::invalid_argument("client QUIC configuration must not specify an Original Destination CID");
            }
            if (config.destination_connection_id.size() == 0) {
                std::array<std::byte, NGTCP2_MIN_INITIAL_DCIDLEN> random{};
                crypto.random_bytes(crypto.context, random);
                config.destination_connection_id = quic_connection_id(random);
            } else if (config.destination_connection_id.size() < NGTCP2_MIN_INITIAL_DCIDLEN) {
                throw std::invalid_argument("client QUIC Initial destination connection ID must be at least 8 bytes");
            }
            break;
        case quic_role::server:
            if (!config.original_destination_connection_id) {
                throw std::invalid_argument("server QUIC configuration requires an Original Destination CID");
            }
            break;
        default:
            throw std::invalid_argument("invalid QUIC connection role");
    }
    if (!config.source_connection_id) {
        std::array<std::byte, 16> random{};
        crypto.random_bytes(crypto.context, random);
        config.source_connection_id = quic_connection_id(random);
    }
    auto state = make_state(std::move(config), crypto, tls_driver, resource, now,
        early_transport_parameters);
    initialize_native_connection(*state, now);
    return state;
}

}  // namespace

quic_connection::quic_connection(quic_connection_config config,
    quic_crypto_provider_view crypto, quic_tls_driver_view tls_driver,
    std::pmr::memory_resource* resource, quic_timestamp now,
    std::span<const std::byte> early_transport_parameters)
    : impl_(make_connection(std::move(config), crypto, tls_driver, resource, now,
          early_transport_parameters)) {}

quic_connection::quic_connection(quic_initial_offer offer,
    quic_connection_config config, quic_crypto_provider_view crypto,
    quic_tls_driver_view tls_driver, std::pmr::memory_resource* resource,
    quic_timestamp now) {
    config.role = quic_role::server;
    config.version = offer.version;
    config.local_address = offer.local_address;
    config.peer_address = offer.peer_address;
    config.destination_connection_id = offer.source_connection_id;
    config.original_destination_connection_id = offer.original_destination_connection_id;
    impl_ = make_connection(std::move(config), crypto, tls_driver, resource, now);
}

quic_connection::~quic_connection() noexcept = default;
quic_tls_handshake& quic_connection::tls_handshake() noexcept {
    return impl_->tls_handshake();
}
quic_connection_info quic_connection::info() const noexcept {
    return impl_->info();
}

std::size_t quic_connection::encode_early_transport_parameters(
    std::span<std::byte> output) const {
    const auto& state = *impl_;
    if (state.config_.role != quic_role::client || !state.connection_ ||
        !state.tls_handshake_complete_ || output.empty()) {
        throw quic_error(quic_error_code::invalid_state,
            "remembered QUIC transport parameters require a completed client handshake");
    }
    const auto result = ngtcp2_conn_encode_0rtt_transport_params2(
        state.connection_, reinterpret_cast<uint8_t*>(output.data()), output.size());
    if (result < 0) {
        throw quic_error(result == NGTCP2_ERR_NOBUF ? quic_error_code::resource_limit
                                                    : quic_error_code::protocol_failure,
            "ngtcp2 failed to encode remembered QUIC transport parameters");
    }
    return static_cast<std::size_t>(result);
}

quic_path_migration quic_connection::start_path_migration(const quic_address& local_address) {
    return impl_->start_path_migration(local_address,
        static_cast<ngtcp2_tstamp>(timestamp_value(impl_->last_supplied_time_)));
}

std::optional<quic_path_migration> quic_connection::path_migration(std::uint64_t id) const noexcept {
    return impl_->path_migration(id);
}

quic_operation_status quic_connection::cancel_path_migration(std::uint64_t id) {
    return impl_->cancel_path_migration(id);
}

quic_operation_status quic_connection::fail_path_migration(std::uint64_t id) noexcept {
    return impl_->fail_path_migration(id);
}

quic_operation_status quic_connection::receive(const quic_datagram_view& datagram,
    quic_timestamp now) {
    impl_->rethrow_failure();
    if (!impl_->connection_ || impl_->state_ == quic_connection_state::retired) {
        return quic_operation_status::retired;
    }
    if (timestamp_value(now) < timestamp_value(impl_->last_supplied_time_)) {
        throw std::invalid_argument("QUIC timestamps must be monotonic");
    }
    impl_->last_supplied_time_ = now;
    detail::fill_quic_path(impl_->path_, datagram.local, datagram.peer);
    server_cid_publication_transaction cid_transaction(*impl_);
    const int result = ngtcp2_conn_read_pkt(impl_->connection_, &impl_->path_.path, nullptr,
        reinterpret_cast<const std::uint8_t*>(datagram.bytes.data()), datagram.bytes.size(), timestamp_value(now));
    if (result == NGTCP2_ERR_DECRYPT || result == NGTCP2_ERR_DISCARD_PKT) {
        detail::rethrow_quic_callback_failure(*impl_);
        return quic_operation_status::need_input;
    }
    check_native_result(*impl_, result, "ngtcp2 packet receive failed");
    if (result == 0) {
        cid_transaction.commit();
        impl_->quic_handshake_complete_ = ngtcp2_conn_get_handshake_completed2(impl_->connection_) != 0;
    }
    const auto status = idle_status(*impl_);
    return status == quic_operation_status::need_input ? quic_operation_status::accepted : status;
}

quic_packet_result quic_connection::write_packet(std::span<std::byte> output, quic_timestamp now) {
    impl_->rethrow_failure();
    if (!impl_->connection_ || impl_->state_ == quic_connection_state::retired) {
        return {.status = quic_operation_status::retired};
    }
    const auto ts = timestamp_value(now);
    if (ts < timestamp_value(impl_->last_supplied_time_)) {
        throw std::invalid_argument("QUIC timestamps must be monotonic");
    }
    impl_->last_supplied_time_ = now;
    if (output.empty()) {
        return {.status = idle_status(*impl_)};
    }
    server_cid_publication_transaction cid_transaction(*impl_);
    ngtcp2_pkt_info packet_info{};
    ngtcp2_ssize written{};
    if (impl_->state_ == quic_connection_state::closing) {
        ngtcp2_ccerr error{};
        const auto reason = impl_->close_reason();
        if (reason.kind == quic_close_kind::application) {
            ngtcp2_ccerr_set_application_error(&error, reason.code,
                reinterpret_cast<const uint8_t*>(reason.reason.data()), reason.reason.size());
        } else {
            ngtcp2_ccerr_set_transport_error(&error, reason.code,
                reinterpret_cast<const uint8_t*>(reason.reason.data()), reason.reason.size());
            error.frame_type = reason.frame_type;
        }
        written = ngtcp2_conn_write_connection_close(impl_->connection_, &impl_->path_.path,
            &packet_info, reinterpret_cast<uint8_t*>(output.data()), output.size(), &error, ts);
        detail::rethrow_quic_callback_failure(*impl_);
        if (written >= 0) {
            cid_transaction.commit();
        }
        if (written == NGTCP2_ERR_INVALID_STATE || written == NGTCP2_ERR_NOBUF) {
            written = 0;
        } else if (written < 0) {
            check_native_result(*impl_, static_cast<int>(written), "ngtcp2 close packet write failed");
        }
        if (!impl_->connection_) {
            return {.status = quic_operation_status::retired};
        }
    } else {
        const auto blocked = [](ngtcp2_ssize result) noexcept {
            return result == NGTCP2_ERR_STREAM_DATA_BLOCKED ||
                   result == NGTCP2_ERR_STREAM_NOT_FOUND ||
                   result == NGTCP2_ERR_STREAM_SHUT_WR || result == NGTCP2_ERR_NOBUF;
        };
        const auto try_datagram = [&]() {
            if (!impl_->connection_) {
                return false;
            }
            const auto datagram = detail::next_datagram_write(*impl_);
            if (!datagram) {
                return false;
            }
            const ngtcp2_vec vector{
                reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(datagram.bytes.data())),
                datagram.bytes.size()};
            int accepted{};
            written = ngtcp2_conn_writev_datagram(impl_->connection_, &impl_->path_.path,
                &packet_info, reinterpret_cast<uint8_t*>(output.data()), output.size(), &accepted,
                NGTCP2_WRITE_DATAGRAM_FLAG_NONE, datagram.id, &vector, 1, ts);
            detail::rethrow_quic_callback_failure(*impl_);
            if (written >= 0) {
                cid_transaction.commit();
            }
            detail::commit_datagram_write(*impl_, datagram.id, accepted != 0);
            if (written < 0 && !blocked(written)) {
                check_native_result(*impl_, static_cast<int>(written), "ngtcp2 DATAGRAM write failed");
            }
            return true;
        };
        const auto try_stream = [&]() {
            if (!impl_->connection_) {
                return false;
            }
            const auto stream = detail::next_stream_write(*impl_);
            if (!stream) {
                return false;
            }
            std::array<ngtcp2_vec, detail::quic_stream_write_range_count> vectors{};
            std::size_t offered_size{};
            for (std::size_t i = 0; i < stream.range_count; ++i) {
                vectors[i] = {
                    reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(stream.ranges[i].data())),
                    stream.ranges[i].size()};
                offered_size += stream.ranges[i].size();
            }
            ngtcp2_ssize accepted{};
            const auto flags = stream.fin ? NGTCP2_WRITE_STREAM_FLAG_FIN : NGTCP2_WRITE_STREAM_FLAG_NONE;
            written = ngtcp2_conn_writev_stream(impl_->connection_, &impl_->path_.path,
                &packet_info, reinterpret_cast<uint8_t*>(output.data()), output.size(), &accepted,
                flags, static_cast<std::int64_t>(stream.stream_id), vectors.data(), stream.range_count, ts);
            detail::rethrow_quic_callback_failure(*impl_);
            if (written >= 0) {
                cid_transaction.commit();
            }
            const auto accepted_bytes = accepted < 0 ? 0U : static_cast<std::size_t>(accepted);
            const bool fin_submitted = stream.fin && accepted >= 0 && accepted_bytes == offered_size && written > 0;
            detail::commit_stream_write(*impl_, stream, accepted_bytes, fin_submitted);
            if (written < 0 && !blocked(written)) {
                check_native_result(*impl_, static_cast<int>(written), "ngtcp2 STREAM write failed");
            }
            return true;
        };
        bool attempted_datagram{};
        bool attempted_stream{};
        if (impl_->prefer_datagram_) {
            attempted_datagram = try_datagram();
        }
        if (written <= 0) {
            attempted_stream = try_stream();
        }
        if (written <= 0 && !attempted_datagram) {
            attempted_datagram = try_datagram();
        }
        if (written <= 0 && impl_->connection_) {
            written = ngtcp2_conn_write_pkt(impl_->connection_, &impl_->path_.path,
                &packet_info, reinterpret_cast<uint8_t*>(output.data()), output.size(), ts);
            detail::rethrow_quic_callback_failure(*impl_);
            if (written >= 0) {
                cid_transaction.commit();
            }
            if (written < 0 && !blocked(written)) {
                check_native_result(*impl_, static_cast<int>(written), "ngtcp2 control packet write failed");
            }
        }
        (void)attempted_stream;
        if (!impl_->connection_) {
            return {.status = quic_operation_status::retired};
        }
        if (written > 0) {
            impl_->prefer_datagram_ = !impl_->prefer_datagram_;
        } else if (impl_->send_datagram_bytes_ != 0) {
            impl_->prefer_datagram_ = true;
        }
    }
    if (!impl_->connection_) {
        return {.status = quic_operation_status::retired};
    }
    ngtcp2_conn_update_pkt_tx_time(impl_->connection_, ts);
    detail::rethrow_quic_callback_failure(*impl_);
    if (written < 0) {
        if (written == NGTCP2_ERR_STREAM_DATA_BLOCKED || written == NGTCP2_ERR_STREAM_NOT_FOUND ||
            written == NGTCP2_ERR_STREAM_SHUT_WR || written == NGTCP2_ERR_NOBUF) {
            if (impl_->send_datagram_bytes_ != 0) {
                impl_->prefer_datagram_ = true;
            }
            return {.status = idle_status(*impl_)};
        }
        check_native_result(*impl_, static_cast<int>(written), "ngtcp2 packet write failed");
        return {.status = idle_status(*impl_)};
    }
    if (written == 0) {
        return {.status = idle_status(*impl_)};
    }
    return {.status = quic_operation_status::accepted, .size = static_cast<std::size_t>(written), .local = detail::decode_quic_address(impl_->path_.path.local), .peer = detail::decode_quic_address(impl_->path_.path.remote)};
}

std::optional<quic_timestamp> quic_connection::next_expiry() const noexcept {
    if (!impl_->connection_ || impl_->state_ == quic_connection_state::retired) {
        return std::nullopt;
    }
    const auto expiry = ngtcp2_conn_get_expiry2(impl_->connection_);
    if (expiry == std::numeric_limits<ngtcp2_tstamp>::max()) {
        return std::nullopt;
    }
    const auto max = static_cast<std::uint64_t>(std::numeric_limits<quic_timestamp::duration::rep>::max());
    if (expiry > max) {
        return quic_timestamp::max();
    }
    return quic_timestamp(std::chrono::duration_cast<quic_timestamp::duration>(std::chrono::nanoseconds(expiry)));
}

quic_operation_status quic_connection::handle_expiry(quic_timestamp now) {
    impl_->rethrow_failure();
    if (!impl_->connection_ || impl_->state_ == quic_connection_state::retired) {
        return quic_operation_status::retired;
    }
    const auto ts = timestamp_value(now);
    if (ts < timestamp_value(impl_->last_supplied_time_)) {
        throw std::invalid_argument("QUIC timestamps must be monotonic");
    }
    impl_->last_supplied_time_ = now;
    server_cid_publication_transaction cid_transaction(*impl_);
    const auto result = ngtcp2_conn_handle_expiry(impl_->connection_, ts);
    check_native_result(*impl_, result, "ngtcp2 expiry handling failed");
    if (result == 0) {
        cid_transaction.commit();
    }
    return idle_status(*impl_) == quic_operation_status::need_input
               ? quic_operation_status::accepted
               : idle_status(*impl_);
}

quic_operation_status quic_connection::update_key(quic_timestamp now) {
    impl_->rethrow_failure();
    if (!impl_->connection_ || impl_->state_ == quic_connection_state::retired) {
        return quic_operation_status::retired;
    }
    const auto ts = timestamp_value(now);
    if (ts < timestamp_value(impl_->last_supplied_time_)) {
        throw std::invalid_argument("QUIC timestamps must be monotonic");
    }
    impl_->last_supplied_time_ = now;
    if (!impl_->confirmed_) {
        return quic_operation_status::would_block;
    }
    server_cid_publication_transaction cid_transaction(*impl_);
    const int result = ngtcp2_conn_initiate_key_update(impl_->connection_, ts);
    detail::rethrow_quic_callback_failure(*impl_);
    if (result == NGTCP2_ERR_INVALID_STATE) {
        return quic_operation_status::would_block;
    }
    check_native_result(*impl_, result, "ngtcp2 key update failed");
    cid_transaction.commit();
    return impl_->connection_ ? quic_operation_status::accepted
                              : quic_operation_status::retired;
}

quic_operation_status quic_connection::close(quic_close_reason_view reason) {
    if (impl_->state_ == quic_connection_state::retired || impl_->state_ == quic_connection_state::draining) {
        return quic_operation_status::retired;
    }
    impl_->latch_close_reason(reason);
    impl_->latched_failure_ = nullptr;
    impl_->state_ = quic_connection_state::closing;
    return quic_operation_status::accepted;
}

quic_operation_status quic_connection::retire_from_server() noexcept {
    if (impl_) {
        impl_->retire();
    }
    return quic_operation_status::retired;
}

void quic_connection::bind_server_cid_registry(detail::quic_cid_registry_view registry) {
    if (!impl_ || !impl_->connection_ || impl_->config_.role != quic_role::server) {
        throw std::logic_error("server CID registry requires an initialized server connection");
    }
    if (!registry) {
        throw std::invalid_argument("server CID registry view is incomplete");
    }
    if (impl_->server_cid_registry_) {
        throw std::logic_error("server CID registry is already bound");
    }
    impl_->server_cid_registry_ = registry;
}

}  // namespace ruvia
