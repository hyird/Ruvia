#include "ruvia/http/detail/http3/quic_connection_state.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "ruvia/http/quic_connection.h"

namespace ruvia::detail {
namespace {

std::pmr::memory_resource* require_resource(std::pmr::memory_resource* resource) {
    if (!resource) {
        throw std::invalid_argument("QUIC connection requires a memory resource");
    }
    return resource;
}

std::size_t level_index(quic_encryption_level level) {
    const auto index = static_cast<std::size_t>(level);
    if (index >= 3) {
        throw std::invalid_argument("invalid QUIC encryption level");
    }
    return index;
}

void validate_config(const quic_connection_config& config,
    const quic_crypto_provider_view& crypto, std::pmr::memory_resource* resource) {
    constexpr auto varint_max = (std::uint64_t{1} << 62) - 1;
    constexpr auto stream_count_max = (std::uint64_t{1} << 60) - 1;
    const auto& transport = config.local_transport_parameters;
    const bool client = config.role == quic_role::client;
    const bool server = config.role == quic_role::server;
    const bool role_valid = client || server;
    const bool cid_valid = config.source_connection_id.has_value() &&
                           (client ? (!config.original_destination_connection_id &&
                                         config.destination_connection_id.size() >= NGTCP2_MIN_INITIAL_DCIDLEN)
                                   : (config.original_destination_connection_id.has_value() &&
                                         config.original_destination_connection_id->size() >= NGTCP2_MIN_INITIAL_DCIDLEN));
    const bool stream_counts_valid =
        transport.initial_max_streams_bidi <= stream_count_max &&
        transport.initial_max_streams_uni <= stream_count_max &&
        transport.initial_max_streams_bidi <= config.limits.max_streams &&
        transport.initial_max_streams_uni <= config.limits.max_streams -
                                                 static_cast<std::size_t>(transport.initial_max_streams_bidi);
    const auto initial_stream_count = stream_counts_valid
                                          ? transport.initial_max_streams_bidi + transport.initial_max_streams_uni
                                          : std::uint64_t{};
    const bool transport_varints_valid =
        transport.initial_max_data <= varint_max &&
        transport.initial_max_stream_data_bidi_local <= varint_max &&
        transport.initial_max_stream_data_bidi_remote <= varint_max &&
        transport.initial_max_stream_data_uni <= varint_max &&
        transport.max_datagram_frame_size <= varint_max &&
        transport.active_connection_id_limit <= varint_max &&
        transport.active_connection_id_limit >= 2 &&
        transport.idle_timeout_ms <= varint_max;
    const bool invalid = !resource || !role_valid || config.version != quic_version::v1 ||
                         config.local_address.port == 0 || config.peer_address.port == 0 || !cid_valid ||
                         config.limits.max_crypto_buffer_size == 0 || config.limits.max_stream_buffer_size == 0 ||
                         config.limits.max_connection_buffer_size == 0 || config.limits.max_streams == 0 ||
                         config.limits.max_local_streams == 0 || config.limits.max_datagram_size < NGTCP2_MAX_UDP_PAYLOAD_SIZE ||
                         config.limits.max_datagram_size > 65527 || config.limits.max_datagrams == 0 ||
                         transport.max_udp_payload_size < NGTCP2_MAX_UDP_PAYLOAD_SIZE ||
                         transport.max_udp_payload_size > 65527 ||
                         config.limits.max_datagram_size < transport.max_udp_payload_size ||
                         transport.initial_max_stream_data_bidi_local > config.limits.max_stream_buffer_size ||
                         transport.initial_max_stream_data_bidi_remote > config.limits.max_stream_buffer_size ||
                         transport.initial_max_stream_data_uni > config.limits.max_stream_buffer_size ||
                         transport.initial_max_data > config.limits.max_connection_buffer_size ||
                         !transport_varints_valid || !stream_counts_valid ||
                         config.limits.max_local_streams > config.limits.max_streams -
                                                               static_cast<std::size_t>(initial_stream_count) ||
                         initial_stream_count > config.limits.max_lifetime_peer_streams ||
                         transport.idle_timeout_ms >
                             static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / 1'000'000;
    if (invalid) {
        throw std::invalid_argument("invalid QUIC connection limits, role, CIDs, or transport parameters");
    }
    crypto.validate();
}

}  // namespace

quic_connection_state::quic_connection_state(quic_connection_config config,
    quic_crypto_provider_view crypto, quic_tls_driver_view tls_driver,
    std::pmr::memory_resource* resource, quic_timestamp now)
    : config_(std::move(config)),
      crypto_(crypto),
      tls_driver_(tls_driver),
      resource_(require_resource(resource)),
      last_supplied_time_(now),
      tls_handshake_(this),
      inbound_crypto_{std::pmr::list<crypto_record>(resource_),
          std::pmr::list<crypto_record>(resource_),
          std::pmr::list<crypto_record>(resource_)},
      local_transport_parameters_(resource_),
      streams_(resource_),
      received_datagrams_(resource_),
      send_datagrams_(resource_),
      close_reason_(resource_) {
    validate_config(config_, crypto_, resource_);
    local_transport_parameters_.reserve(64);
    streams_.reserve(config_.limits.max_streams);
    tls_driver_.validate();
}

quic_connection_state::~quic_connection_state() noexcept {
    retire();
}

void quic_connection_state::retire() noexcept {
    if (tls_driver_retired_) {
        return;
    }
    if (tls_driver_retiring_ || tls_driver_active_) {
        std::terminate();
    }
    tls_driver_retiring_ = true;
    tls_driver_.retire(tls_driver_.context);
    if (crypto_lease_active_) {
        std::terminate();
    }
    tls_driver_retired_ = true;
    quic_retire_all_connection_ids(server_cid_registry_);
    ngtcp2_conn_del(connection_);
    connection_ = nullptr;
    server_cid_registry_ = {};
    tls_driver_retiring_ = false;
    state_ = ruvia::quic_connection_state::retired;
}

void quic_connection_state::append_crypto(quic_encryption_level level,
    std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return;
    }
    if (bytes.size() > config_.limits.max_crypto_buffer_size - retained_crypto_bytes_) {
        throw quic_error(quic_error_code::resource_limit, "QUIC CRYPTO buffer limit exceeded");
    }
    auto& records = inbound_crypto_[level_index(level)];
    records.emplace_back(resource_);
    try {
        records.back().bytes.assign(bytes.begin(), bytes.end());
    } catch (...) {
        records.pop_back();
        throw;
    }
    retained_crypto_bytes_ += bytes.size();
}

quic_crypto_record_lease quic_connection_state::take_crypto(quic_encryption_level level) {
    if (crypto_lease_active_) {
        throw std::logic_error("only one inbound QUIC CRYPTO lease may be outstanding");
    }
    auto& records = inbound_crypto_[level_index(level)];
    if (records.empty()) {
        return {};
    }
    auto& record = records.front();
    const auto remaining = record.bytes.size() - record.offset;
    if (remaining == 0) {
        std::terminate();
    }
    crypto_lease_active_ = true;
    leased_record_ = &record;
    leased_level_ = level;
    leased_crypto_bytes_ = remaining;
    return quic_crypto_record_lease(this, &consume_lease, level,
        std::span<const std::byte>(record.bytes).subspan(record.offset));
}

void quic_connection_state::consume_crypto(std::size_t size, bool release_lease) noexcept {
    if (!crypto_lease_active_ || leased_record_ == nullptr || size > leased_crypto_bytes_) {
        std::terminate();
    }
    auto& records = inbound_crypto_[static_cast<std::size_t>(leased_level_)];
    if (records.empty() || &records.front() != leased_record_) {
        std::terminate();
    }
    leased_record_->offset += size;
    retained_crypto_bytes_ -= size;
    leased_crypto_bytes_ -= size;
    if (leased_record_->offset == leased_record_->bytes.size()) {
        records.pop_front();
        leased_record_ = nullptr;
    }
    if (release_lease) {
        crypto_lease_active_ = false;
        leased_record_ = nullptr;
        leased_crypto_bytes_ = 0;
    } else if (leased_record_ == nullptr) {
        crypto_lease_active_ = false;
        leased_crypto_bytes_ = 0;
    }
}

std::size_t quic_connection_state::retained_crypto_bytes() const noexcept {
    return retained_crypto_bytes_;
}

std::size_t quic_connection_state::crypto_record_count(quic_encryption_level level) const noexcept {
    const auto index = static_cast<std::size_t>(level);
    return index < inbound_crypto_.size() ? inbound_crypto_[index].size() : 0;
}

void quic_connection_state::latch_close_reason(quic_close_reason_view reason) {
    if (close_reason_latched_) {
        return;
    }
    if (reason.reason.size() > 1024) {
        throw quic_error(quic_error_code::resource_limit, "QUIC close reason exceeds its bound");
    }
    std::pmr::string owned_reason(resource_);
    if (!reason.reason.empty()) {
        owned_reason.assign(reason.reason.data(), reason.reason.size());
    }
    close_reason_.swap(owned_reason);
    close_kind_ = reason.kind;
    close_error_code_ = reason.code;
    close_frame_type_ = reason.frame_type;
    close_reason_latched_ = true;
}

quic_close_reason_view quic_connection_state::close_reason() const noexcept {
    return {.kind = close_kind_,
        .code = close_error_code_,
        .frame_type = close_frame_type_,
        .reason = close_reason_};
}

void quic_connection_state::latch_failure(std::exception_ptr failure) noexcept {
    if (!latched_failure_) {
        latched_failure_ = std::move(failure);
        state_ = ruvia::quic_connection_state::failed;
    }
}

void quic_connection_state::rethrow_failure() const {
    if (latched_failure_) {
        std::rethrow_exception(latched_failure_);
    }
}

quic_connection_info quic_connection_state::info() const noexcept {
    return {.state = state_,
        .local_address = config_.local_address,
        .peer_address = config_.peer_address,
        .tls_handshake_complete = tls_handshake_complete_,
        .quic_handshake_complete = quic_handshake_complete_,
        .confirmed = confirmed_,
        .negotiated_idle_timeout_ms = config_.local_transport_parameters.idle_timeout_ms,
        .close_error_code = close_error_code_};
}

void quic_connection_state::complete_tls(quic_tls_info_view info) {
    if (!tls_driver_active_ || tls_handshake_complete_ || failed() ||
        info.negotiated_alpn.size() > negotiated_alpn_.size()) {
        throw std::invalid_argument("invalid QUIC TLS completion metadata");
    }
    std::copy(info.negotiated_alpn.begin(), info.negotiated_alpn.end(), negotiated_alpn_.begin());
    negotiated_alpn_size_ = info.negotiated_alpn.size();
    negotiated_cipher_suite_ = info.cipher_suite;
    tls_handshake_complete_ = true;
}

void quic_connection_state::fail_tls(quic_tls_alert alert) noexcept {
    if (!tls_driver_active_) {
        return;
    }
    failure_alert_ = alert;
    state_ = ruvia::quic_connection_state::failed;
}

bool quic_connection_state::failed() const noexcept {
    return state_ == ruvia::quic_connection_state::failed;
}

quic_tls_handshake& quic_connection_state::tls_handshake() noexcept {
    return tls_handshake_;
}

void quic_connection_state::consume_lease(void* context, std::size_t size,
    bool release_lease) noexcept {
    static_cast<quic_connection_state*>(context)->consume_crypto(size, release_lease);
}

}  // namespace ruvia::detail

namespace ruvia {

quic_crypto_record_lease::quic_crypto_record_lease(void* context, consume_fn consume,
    quic_encryption_level level, std::span<const std::byte> bytes) noexcept
    : context_(context),
      consume_(consume),
      level_(level),
      bytes_(bytes) {}

quic_crypto_record_lease::quic_crypto_record_lease(quic_crypto_record_lease&& other) noexcept
    : context_(std::exchange(other.context_, nullptr)),
      consume_(std::exchange(other.consume_, nullptr)),
      level_(other.level_),
      bytes_(std::exchange(other.bytes_, {})) {}

quic_crypto_record_lease& quic_crypto_record_lease::operator=(quic_crypto_record_lease&& other) noexcept {
    if (this != &other) {
        release();
        context_ = std::exchange(other.context_, nullptr);
        consume_ = std::exchange(other.consume_, nullptr);
        level_ = other.level_;
        bytes_ = std::exchange(other.bytes_, {});
    }
    return *this;
}

quic_crypto_record_lease::~quic_crypto_record_lease() noexcept {
    release();
}

quic_crypto_record_lease::operator bool() const noexcept {
    return context_ != nullptr;
}

quic_encryption_level quic_crypto_record_lease::level() const noexcept {
    return level_;
}

std::span<const std::byte> quic_crypto_record_lease::bytes() const noexcept {
    return bytes_;
}

void quic_crypto_record_lease::consume(std::size_t size) {
    if (!context_ || size > bytes_.size()) {
        throw std::out_of_range("QUIC CRYPTO lease consume exceeds its view");
    }
    if (size == 0) {
        return;
    }
    consume_(context_, size, false);
    bytes_ = bytes_.subspan(size);
    if (bytes_.empty()) {
        context_ = nullptr;
        consume_ = nullptr;
    }
}

void quic_crypto_record_lease::release() noexcept {
    if (context_) {
        consume_(context_, 0, true);
        context_ = nullptr;
        consume_ = nullptr;
        bytes_ = {};
    }
}

}  // namespace ruvia

namespace ruvia::detail {

void quic_connection_state_deleter::operator()(quic_connection_state* state) const noexcept {
    if (!state) {
        return;
    }
    std::destroy_at(state);
    resource->deallocate(state, sizeof(quic_connection_state), alignof(quic_connection_state));
}

}  // namespace ruvia::detail

namespace ruvia {

quic_tls_handshake::quic_tls_handshake(detail::quic_connection_state* state) noexcept
    : state_(state) {}

quic_tls_handshake::~quic_tls_handshake() noexcept = default;

quic_role quic_tls_handshake::role() const noexcept {
    return state_->config_.role;
}

quic_encryption_level quic_tls_handshake::current_read_level() const noexcept {
    return state_->current_read_level_;
}

quic_tls_info_view quic_tls_handshake::info() const noexcept {
    return {{state_->negotiated_alpn_.data(), state_->negotiated_alpn_size_},
        state_->negotiated_cipher_suite_};
}

quic_crypto_record_lease quic_tls_handshake::take_crypto_record() {
    if (!state_->tls_driver_active_ || state_->tls_driver_retiring_ ||
        state_->tls_driver_retired_) {
        throw std::logic_error("QUIC CRYPTO input is available only during active TLS drive");
    }
    return state_->take_crypto(state_->current_read_level_);
}

std::span<const std::byte> quic_tls_handshake::local_transport_parameters() const noexcept {
    return state_->local_transport_parameters_;
}

void quic_tls_handshake::complete(quic_tls_info_view info) {
    state_->complete_tls(info);
}

void quic_tls_handshake::fail(quic_tls_alert alert) noexcept {
    state_->fail_tls(alert);
}

bool quic_tls_handshake::completed() const noexcept {
    return state_->tls_handshake_complete_;
}

bool quic_tls_handshake::failed() const noexcept {
    return state_->failed();
}

quic_tls_alert quic_tls_handshake::failure_alert() const noexcept {
    return state_->failure_alert_;
}

void quic_tls_driver_view::validate() const {
    if (!drive || !retire) {
        throw std::invalid_argument("QUIC TLS driver view requires drive and retire callbacks");
    }
}

}  // namespace ruvia
