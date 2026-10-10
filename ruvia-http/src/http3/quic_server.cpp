#include "ruvia/http/quic_server.h"

#include <ngtcp2/ngtcp2.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "http3/quic_cid_partition.h"
#include "http3/quic_cid_registry.h"

namespace ruvia {
namespace {

struct connection_deleter final {
    std::pmr::memory_resource* resource_{};
    void operator()(quic_connection* value) const noexcept {
        if (!value) {
            return;
        }
        std::destroy_at(value);
        std::pmr::polymorphic_allocator<quic_connection>(resource_).deallocate(value, 1);
    }
};
using connection_owner = std::unique_ptr<quic_connection, connection_deleter>;

struct cid_hash final {
    std::size_t operator()(const quic_connection_id& cid) const noexcept {
        std::size_t hash = 1469598103934665603ULL;
        for (const auto byte : cid.view()) {
            hash = (hash ^ std::to_integer<unsigned char>(byte)) * 1099511628211ULL;
        }
        return hash;
    }
};

std::uint64_t next_nonzero(std::uint64_t& value, const char* what) {
    if (value == 0 || value == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(what);
    }
    return value++;
}

quic_connection_id from_native_cid(const ngtcp2_cid& cid) {
    if (cid.datalen > quic_max_connection_id_size) {
        throw std::invalid_argument("QUIC connection ID exceeds 20 bytes");
    }
    return quic_connection_id(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(cid.data), cid.datalen));
}

ngtcp2_cid to_native_cid(const quic_connection_id& cid) noexcept {
    ngtcp2_cid result_value{};
    result_value.datalen = cid.size();
    if (cid.size()) {
        std::memcpy(result_value.data, cid.view().data(), cid.size());
    }
    return result_value;
}

bool valid_varint(std::uint64_t value) noexcept {
    return value < (std::uint64_t{1} << 62);
}

struct parsed_quic_header final {
    ngtcp2_version_cid ids_{};
    int decode_result_{};
    bool short_header_{};
};

std::optional<parsed_quic_header> parse_quic_header(
    std::span<const std::byte> packet) noexcept {
    if (packet.empty()) {
        return std::nullopt;
    }
    parsed_quic_header result;
    result.short_header_ = (std::to_integer<unsigned char>(packet.front()) & 0x80U) == 0;
    result.decode_result_ = ngtcp2_pkt_decode_version_cid(&result.ids_,
        reinterpret_cast<const std::uint8_t*>(packet.data()), packet.size(),
        detail::quic_server_connection_id_size);
    if ((result.decode_result_ != 0 && result.decode_result_ != NGTCP2_ERR_VERSION_NEGOTIATION) ||
        !result.ids_.dcid || result.ids_.dcidlen > quic_max_connection_id_size ||
        result.ids_.scidlen > quic_max_connection_id_size) {
        return std::nullopt;
    }
    return result;
}

bool same_address(const quic_address& left, const quic_address& right) noexcept {
    return left.family_ == right.family_ && left.port_ == right.port_ &&
           left.scope_id_ == right.scope_id_ && left.bytes_ == right.bytes_;
}

}  // namespace

std::optional<std::uint32_t> quic_datagram_partition(
    std::span<const std::byte> packet, std::uint32_t partition_count) noexcept {
    if (partition_count == 0) {
        return std::nullopt;
    }
    const auto header_value = parse_quic_header(packet);
    if (!header_value) {
        return std::nullopt;
    }
    return detail::quic_connection_id_partition(std::span<const std::byte>(
                                                    reinterpret_cast<const std::byte*>(header_value->ids_.dcid), header_value->ids_.dcidlen),
        partition_count);
}

quic_version_negotiation_plan::quic_version_negotiation_plan(
    quic_version_negotiation_plan&& other) noexcept
    : destination_connection_id_(other.destination_connection_id_),
      source_connection_id_(other.source_connection_id_),
      local_address_(other.local_address_),
      peer_address_(other.peer_address_),
      input_size_(other.input_size_),
      valid_(std::exchange(other.valid_, false)) {
    other.input_size_ = 0;
}

quic_version_negotiation_plan& quic_version_negotiation_plan::operator=(
    quic_version_negotiation_plan&& other) noexcept {
    if (this != &other) {
        destination_connection_id_ = other.destination_connection_id_;
        source_connection_id_ = other.source_connection_id_;
        local_address_ = other.local_address_;
        peer_address_ = other.peer_address_;
        input_size_ = other.input_size_;
        valid_ = std::exchange(other.valid_, false);
        other.input_size_ = 0;
    }
    return *this;
}

struct quic_server::impl final {
    struct pending final {
        explicit pending(std::pmr::memory_resource* resource)
            : bytes_(resource) {}
        quic_initial_offer offer_{};
        std::pmr::vector<std::byte> bytes_;
    };

    struct active final {
        active(impl* server, quic_connection_token connection_token,
            quic_connection* value, std::pmr::memory_resource* resource)
            : owner_(server),
              token_(connection_token),
              connection_(value, connection_deleter{resource}),
              ids_(resource) {}
        impl* owner_{};
        quic_connection_token token_{};
        connection_owner connection_;
        std::pmr::vector<quic_connection_id> ids_;
    };

    impl(quic_server_config source_value, quic_crypto_provider_view provider,
        std::pmr::memory_resource* memory)
        : config_(std::move(source_value)),
          crypto_(provider),
          resource_(memory),
          pending_by_id_(memory),
          pending_by_dcid_(memory),
          active_by_token_(memory),
          cid_to_token_(memory) {}

    quic_server_config config_;
    quic_crypto_provider_view crypto_;
    std::pmr::memory_resource* resource_;
    std::pmr::unordered_map<std::uint64_t, pending> pending_by_id_;
    std::pmr::unordered_map<quic_connection_id, std::uint64_t, cid_hash> pending_by_dcid_;
    std::pmr::unordered_map<std::uint64_t, active> active_by_token_;
    std::pmr::unordered_map<quic_connection_id, quic_connection_token, cid_hash> cid_to_token_;
    std::size_t pending_bytes_{};
    std::uint64_t next_offer_id_{1};
    std::uint64_t next_token_{1};

    // The offer must match its cached Initial exactly; a stale or forged offer
    // never selects another peer's pending state.
    [[nodiscard]] std::pmr::unordered_map<std::uint64_t, pending>::iterator find_pending(
        const quic_initial_offer& offer) noexcept {
        const auto found = pending_by_id_.find(offer.offer_id_);
        if (found == pending_by_id_.end()) {
            return found;
        }
        const auto& cached = found->second.offer_;
        const bool matches = cached.destination_connection_id_ == offer.destination_connection_id_ &&
                             cached.source_connection_id_ == offer.source_connection_id_ &&
                             cached.original_destination_connection_id_ == offer.original_destination_connection_id_ &&
                             same_address(cached.local_address_, offer.local_address_) &&
                             same_address(cached.peer_address_, offer.peer_address_) &&
                             cached.version_ == offer.version_;
        return matches ? found : pending_by_id_.end();
    }

    void consume_pending(std::pmr::unordered_map<std::uint64_t, pending>::iterator found) noexcept {
        pending_bytes_ -= found->second.bytes_.size();
        pending_by_dcid_.erase(found->second.offer_.destination_connection_id_);
        pending_by_id_.erase(found);
    }

    static void publish_cid(void* context_value, std::span<const std::byte> bytes_value) {
        auto& entry_value = *static_cast<active*>(context_value);
        if (bytes_value.size() > quic_max_connection_id_size) {
            throw std::invalid_argument("QUIC local connection ID exceeds 20 bytes");
        }
        const quic_connection_id cid(bytes_value);
        if (entry_value.owner_->cid_to_token_.contains(cid)) {
            throw std::runtime_error("QUIC local CID collision");
        }
        entry_value.owner_->cid_to_token_.emplace(cid, entry_value.token_);
        try {
            entry_value.ids_.push_back(cid);
        } catch (...) {
            entry_value.owner_->cid_to_token_.erase(cid);
            throw;
        }
    }

    static void retire_cid(void* context_value, std::span<const std::byte> bytes_value) noexcept {
        auto& entry_value = *static_cast<active*>(context_value);
        if (bytes_value.size() > quic_max_connection_id_size) {
            return;
        }
        const quic_connection_id cid(bytes_value);
        const auto found = entry_value.owner_->cid_to_token_.find(cid);
        if (found != entry_value.owner_->cid_to_token_.end() && found->second == entry_value.token_) {
            entry_value.owner_->cid_to_token_.erase(found);
        }
        const auto known = std::ranges::find(entry_value.ids_, cid);
        if (known != entry_value.ids_.end()) {
            entry_value.ids_.erase(known);
        }
    }

    static void retire_all_cids(void* context_value) noexcept {
        auto& entry_value = *static_cast<active*>(context_value);
        while (!entry_value.ids_.empty()) {
            const auto cid = entry_value.ids_.back();
            retire_cid(&entry_value, cid.view());
        }
    }

    static detail::quic_cid_registry_view cid_registry(active& entry_value) noexcept {
        return {.context_ = &entry_value, .publish_ = publish_cid, .retire_ = retire_cid, .retire_all_ = retire_all_cids};
    }
};

void quic_server::impl_deleter::operator()(impl* value) const noexcept {
    if (!value) {
        return;
    }
    std::destroy_at(value);
    std::pmr::polymorphic_allocator<impl>(resource_).deallocate(value, 1);
}

quic_server::quic_server(quic_server_config config, quic_crypto_provider_view crypto,
    std::pmr::memory_resource* resource) {
    if (!resource) {
        throw std::invalid_argument("QUIC server requires a memory resource");
    }
    crypto.validate();
    detail::validate_quic_cid_partition(config.cid_partition_);
    const auto& transport = config.local_transport_parameters_;
    const auto& limits = config.limits_;
    constexpr std::uint64_t stream_count_max = (std::uint64_t{1} << 60) - 1;
    const bool stream_counts_valid = transport.initial_max_streams_bidi_ <= stream_count_max &&
                                     transport.initial_max_streams_uni_ <= stream_count_max &&
                                     transport.initial_max_streams_bidi_ <= limits.max_streams_ &&
                                     transport.initial_max_streams_uni_ <= limits.max_streams_ -
                                                                               static_cast<std::size_t>(std::min<std::uint64_t>(
                                                                                   transport.initial_max_streams_bidi_, limits.max_streams_));
    const auto initial_stream_count = stream_counts_valid
                                          ? transport.initial_max_streams_bidi_ + transport.initial_max_streams_uni_
                                          : std::uint64_t{};
    if ((config.version_ != quic_version::v1 && config.version_ != quic_version::v2) ||
        transport.max_udp_payload_size_ < 1200 ||
        transport.max_udp_payload_size_ > 65527 || limits.max_datagram_size_ < transport.max_udp_payload_size_ ||
        limits.max_datagram_size_ > 65527 || limits.max_crypto_buffer_size_ == 0 ||
        limits.max_stream_buffer_size_ == 0 || limits.max_connection_buffer_size_ == 0 ||
        limits.max_streams_ == 0 || limits.max_local_streams_ == 0 || limits.max_datagrams_ == 0 ||
        transport.initial_max_data_ > limits.max_connection_buffer_size_ ||
        transport.initial_max_stream_data_bidi_local_ > limits.max_stream_buffer_size_ ||
        transport.initial_max_stream_data_bidi_remote_ > limits.max_stream_buffer_size_ ||
        transport.initial_max_stream_data_uni_ > limits.max_stream_buffer_size_ ||
        !stream_counts_valid || limits.max_local_streams_ > limits.max_streams_ - static_cast<std::size_t>(initial_stream_count) ||
        initial_stream_count > limits.max_lifetime_peer_streams_ ||
        transport.idle_timeout_ms_ > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / 1'000'000 ||
        transport.active_connection_id_limit_ < 2 ||
        !valid_varint(config.local_transport_parameters_.idle_timeout_ms_) ||
        !valid_varint(config.local_transport_parameters_.max_udp_payload_size_) ||
        !valid_varint(config.local_transport_parameters_.initial_max_data_) ||
        !valid_varint(config.local_transport_parameters_.initial_max_stream_data_bidi_local_) ||
        !valid_varint(config.local_transport_parameters_.initial_max_stream_data_bidi_remote_) ||
        !valid_varint(config.local_transport_parameters_.initial_max_stream_data_uni_) ||
        !valid_varint(config.local_transport_parameters_.initial_max_streams_bidi_) ||
        !valid_varint(config.local_transport_parameters_.initial_max_streams_uni_) ||
        !valid_varint(config.local_transport_parameters_.active_connection_id_limit_) ||
        !valid_varint(config.local_transport_parameters_.max_datagram_frame_size_)) {
        throw std::invalid_argument("invalid QUIC server version, transport parameters, or pending budget");
    }
    std::pmr::polymorphic_allocator<impl> allocator(resource);
    auto* value = allocator.allocate(1);
    try {
        std::construct_at(value, std::move(config), crypto, resource);
    } catch (...) {
        allocator.deallocate(value, 1);
        throw;
    }
    impl_ = std::unique_ptr<impl, impl_deleter>(value, impl_deleter{resource});
}

quic_server::~quic_server() noexcept {
    if (impl_) {
        for (auto& [token, active] : impl_->active_by_token_) {
            (void)token;
            active.connection_->retire_from_server();
        }
        impl_.reset();
    }
}

quic_server_route quic_server::route_datagram(const quic_datagram_view& datagram) {
    if (!impl_ || datagram.bytes_.empty()) {
        return {};
    }
    const auto parsed_header = parse_quic_header(datagram.bytes_);
    if (!parsed_header) {
        return {};
    }
    const auto& decoded = parsed_header->ids_;
    const auto decode_result = parsed_header->decode_result_;
    const auto* packet = reinterpret_cast<const std::uint8_t*>(datagram.bytes_.data());
    quic_connection_id dcid(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(decoded.dcid), decoded.dcidlen));
    if (detail::quic_connection_id_partition(dcid.view(), impl_->config_.cid_partition_.count_) !=
        impl_->config_.cid_partition_.index_) {
        return {};
    }
    // Short headers carry no version. Route their destination CID before
    // interpreting version zero as a long-header Version Negotiation packet.
    if (parsed_header->short_header_) {
        if (const auto found = impl_->cid_to_token_.find(dcid); found != impl_->cid_to_token_.end()) {
            return {.kind_ = quic_server_route_kind::existing_connection, .connection_ = found->second};
        }
        return {};
    }
    if (decoded.version == 0) {
        return {};
    }
    if (decoded.version != static_cast<std::uint32_t>(quic_version::v1) &&
        decoded.version != static_cast<std::uint32_t>(quic_version::v2)) {
        if (datagram.bytes_.size() < 1200 || decoded.dcidlen > quic_max_connection_id_size ||
            decoded.scidlen > quic_max_connection_id_size) {
            return {};
        }
        quic_version_negotiation_plan plan;
        plan.destination_connection_id_ = quic_connection_id(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(decoded.scid), decoded.scidlen));
        plan.source_connection_id_ = dcid;
        plan.local_address_ = datagram.local_;
        plan.peer_address_ = datagram.peer_;
        plan.input_size_ = datagram.bytes_.size();
        plan.valid_ = true;
        return {.kind_ = quic_server_route_kind::version_negotiation,
            .version_negotiation_ = std::move(plan)};
    }
    if (const auto found = impl_->cid_to_token_.find(dcid); found != impl_->cid_to_token_.end()) {
        return {.kind_ = quic_server_route_kind::existing_connection, .connection_ = found->second};
    }
    if (datagram.bytes_.size() < 1200 || decode_result != 0 || decoded.dcidlen < 8 ||
        decoded.dcidlen > quic_max_connection_id_size || decoded.scidlen > quic_max_connection_id_size) {
        return {};
    }

    ngtcp2_pkt_hd header_value{};
    if (ngtcp2_accept(&header_value, packet, datagram.bytes_.size()) != 0 ||
        header_value.type != NGTCP2_PKT_INITIAL ||
        (header_value.version != NGTCP2_PROTO_VER_V1 && header_value.version != NGTCP2_PROTO_VER_V2) ||
        header_value.tokenlen != 0 || header_value.dcid.datalen < 8 ||
        header_value.dcid.datalen > quic_max_connection_id_size ||
        header_value.scid.datalen > quic_max_connection_id_size) {
        return {};
    }
    const auto initial_dcid = from_native_cid(header_value.dcid);
    const auto initial_scid = from_native_cid(header_value.scid);
    if (const auto existing_pending = impl_->pending_by_dcid_.find(initial_dcid);
        existing_pending != impl_->pending_by_dcid_.end()) {
        const auto& existing = impl_->pending_by_id_.at(existing_pending->second).offer_;
        if (existing.source_connection_id_ != initial_scid) {
            return {};
        }
        return {.kind_ = quic_server_route_kind::initial_offer, .offer_ = existing};
    }
    if (impl_->pending_by_id_.size() >= impl_->config_.max_pending_connections_ ||
        datagram.bytes_.size() > impl_->config_.max_pending_datagram_bytes_ -
                                     std::min(impl_->pending_bytes_, impl_->config_.max_pending_datagram_bytes_)) {
        return {.kind_ = quic_server_route_kind::rejected,
            .status_ = quic_operation_status::would_block};
    }
    const auto offer_id = next_nonzero(impl_->next_offer_id_, "QUIC Initial offer ID exhausted");
    quic_initial_offer offer{.offer_id_ = offer_id,
        .local_address_ = datagram.local_,
        .peer_address_ = datagram.peer_,
        .destination_connection_id_ = initial_dcid,
        .source_connection_id_ = initial_scid,
        .original_destination_connection_id_ = initial_dcid,
        .version_ = static_cast<quic_version>(header_value.version)};
    impl::pending pending(impl_->resource_);
    pending.offer_ = offer;
    pending.bytes_.assign(datagram.bytes_.begin(), datagram.bytes_.end());
    auto [it, inserted] = impl_->pending_by_id_.emplace(offer_id, std::move(pending));
    if (!inserted) {
        throw std::logic_error("QUIC Initial offer ID collision");
    }
    try {
        impl_->pending_by_dcid_.emplace(initial_dcid, offer_id);
    } catch (...) {
        impl_->pending_by_id_.erase(it);
        throw;
    }
    impl_->pending_bytes_ += datagram.bytes_.size();
    return {.kind_ = quic_server_route_kind::initial_offer, .offer_ = offer};
}

quic_packet_result quic_server::write_version_negotiation(
    quic_version_negotiation_plan& plan, std::span<std::byte> output) {
    if (!plan.valid_) {
        throw std::invalid_argument("invalid or already consumed QUIC VN plan");
    }
    const auto amplification_limit = plan.input_size_ > std::numeric_limits<std::size_t>::max() / 3
                                         ? std::numeric_limits<std::size_t>::max()
                                         : plan.input_size_ * 3;
    if (output.size() > amplification_limit) {
        output = output.first(amplification_limit);
    }
    if (!impl_) {
        throw std::logic_error("QUIC server is not initialized");
    }
    const auto dcid = to_native_cid(plan.destination_connection_id_);
    const auto scid = to_native_cid(plan.source_connection_id_);
    const auto packet_size = 7 + dcid.datalen + scid.datalen + sizeof(std::uint32_t);
    if (output.size() < packet_size || packet_size > amplification_limit) {
        return {.status_ = quic_operation_status::would_block};
    }
    output = output.first(std::min(output.size(), amplification_limit));
    std::byte random{};
    impl_->crypto_.random_bytes_(impl_->crypto_.context_, std::span<std::byte>(&random, 1));
    constexpr std::array<std::uint32_t, 2> versions{NGTCP2_PROTO_VER_V1, NGTCP2_PROTO_VER_V2};
    const auto written = ngtcp2_pkt_write_version_negotiation(
        reinterpret_cast<std::uint8_t*>(output.data()), output.size(),
        std::to_integer<std::uint8_t>(random), dcid.data, dcid.datalen,
        scid.data, scid.datalen, versions.data(), versions.size());
    if (written == NGTCP2_ERR_NOBUF) {
        return {.status_ = quic_operation_status::would_block};
    }
    if (written < 0) {
        throw std::runtime_error("ngtcp2 failed to write Version Negotiation packet");
    }
    plan.valid_ = false;
    return {.status_ = quic_operation_status::accepted, .size_ = static_cast<std::size_t>(written), .local_ = plan.local_address_, .peer_ = plan.peer_address_};
}

quic_server_admit_result quic_server::admit_initial(const quic_initial_offer& offer,
    quic_tls_driver_view tls_driver, quic_timestamp now) {
    if (!impl_) {
        throw std::logic_error("QUIC server is not initialized");
    }
    const auto pending_it = impl_->find_pending(offer);
    if (pending_it == impl_->pending_by_id_.end() ||
        offer.local_address_.port_ == 0 || offer.peer_address_.port_ == 0 ||
        (offer.local_address_.family_ != quic_address_family::ipv4 &&
            offer.local_address_.family_ != quic_address_family::ipv6) ||
        (offer.peer_address_.family_ != quic_address_family::ipv4 &&
            offer.peer_address_.family_ != quic_address_family::ipv6) ||
        (offer.version_ != quic_version::v1 && offer.version_ != quic_version::v2)) {
        throw std::invalid_argument("invalid or stale QUIC Initial offer");
    }
    tls_driver.validate();
    if (impl_->active_by_token_.size() >= impl_->config_.max_active_connections_) {
        return {.status_ = quic_operation_status::would_block};
    }
    auto& saved = pending_it->second;
    quic_connection_token token{};
    try {
        quic_connection_config config{.role_ = quic_role::server,
            .version_ = offer.version_,
            .preferred_version_ = impl_->config_.version_,
            .local_address_ = offer.local_address_,
            .peer_address_ = offer.peer_address_,
            .destination_connection_id_ = offer.source_connection_id_,
            .original_destination_connection_id_ = offer.original_destination_connection_id_,
            .cid_partition_ = impl_->config_.cid_partition_,
            .local_transport_parameters_ = impl_->config_.local_transport_parameters_,
            .limits_ = impl_->config_.limits_};
        std::array<std::byte, detail::quic_server_connection_id_size> server_source_id_bytes{};
        for (std::size_t attempt_value = 0; attempt_value < 4; ++attempt_value) {
            detail::generate_quic_server_connection_id(
                impl_->crypto_, server_source_id_bytes, config.cid_partition_);
            const quic_connection_id candidate(server_source_id_bytes);
            if (candidate != offer.original_destination_connection_id_ &&
                !impl_->cid_to_token_.contains(candidate)) {
                config.source_connection_id_ = candidate;
                break;
            }
        }
        if (!config.source_connection_id_) {
            // Nothing was created and existing CID owners are untouched. A later
            // attempt draws fresh candidates, so the offer stays pending.
            return {.status_ = quic_operation_status::would_block};
        }
        const auto server_source_id = *config.source_connection_id_;
        token = quic_connection_token{next_nonzero(impl_->next_token_, "QUIC connection token exhausted")};
        std::pmr::polymorphic_allocator<quic_connection> allocator(impl_->resource_);
        auto* raw = allocator.allocate(1);
        try {
            ::new (static_cast<void*>(raw)) quic_connection(
                offer, std::move(config), impl_->crypto_, tls_driver, impl_->resource_, now);
        } catch (...) {
            allocator.deallocate(raw, 1);
            throw;
        }
        auto active_it = impl_->active_by_token_.end();
        try {
            auto insertion = impl_->active_by_token_.try_emplace(
                token.value_, impl_.get(), token, nullptr, impl_->resource_);
            active_it = insertion.first;
            if (!insertion.second) {
                throw std::logic_error("QUIC connection token collision");
            }
        } catch (...) {
            raw->retire_from_server();
            std::destroy_at(raw);
            allocator.deallocate(raw, 1);
            throw;
        }
        active_it->second.connection_.reset(raw);
        try {
            raw->bind_server_cid_registry(impl::cid_registry(active_it->second));
            impl::publish_cid(&active_it->second, server_source_id.view());
            impl::publish_cid(&active_it->second, offer.original_destination_connection_id_.view());
            raw->receive({.bytes_ = saved.bytes_, .local_ = offer.local_address_, .peer_ = offer.peer_address_}, now);
        } catch (...) {
            impl::retire_all_cids(&active_it->second);
            active_it->second.connection_->retire_from_server();
            impl_->active_by_token_.erase(active_it);
            throw;
        }
    } catch (...) {
        // The caller drops an offer whose admission failed: retrying the same
        // Initial (for example a rejected ClientHello) fails again. Release the
        // bounded pending slot with it; a retransmission becomes a fresh offer.
        impl_->consume_pending(pending_it);
        throw;
    }
    impl_->consume_pending(pending_it);
    return {.status_ = quic_operation_status::accepted, .connection_ = token};
}

bool quic_server::discard_initial(const quic_initial_offer& offer) noexcept {
    if (!impl_) {
        return false;
    }
    const auto pending_it = impl_->find_pending(offer);
    if (pending_it == impl_->pending_by_id_.end()) {
        return false;
    }
    impl_->consume_pending(pending_it);
    return true;
}

quic_connection& quic_server::connection(quic_connection_token token) {
    const auto found = impl_->active_by_token_.find(token.value_);
    if (token.value_ == 0 || found == impl_->active_by_token_.end()) {
        throw std::out_of_range("stale QUIC connection token");
    }
    return *found->second.connection_;
}

const quic_connection& quic_server::connection(quic_connection_token token) const {
    return const_cast<quic_server*>(this)->connection(token);
}

quic_operation_status quic_server::receive(quic_connection_token token,
    const quic_datagram_view& datagram, quic_timestamp now) {
    return connection(token).receive(datagram, now);
}

std::optional<quic_timestamp> quic_server::next_expiry() const noexcept {
    std::optional<quic_timestamp> earliest;
    if (!impl_) {
        return earliest;
    }
    for (const auto& [token, active] : impl_->active_by_token_) {
        (void)token;
        const auto expiry = active.connection_->next_expiry();
        if (expiry && (!earliest || *expiry < *earliest)) {
            earliest = expiry;
        }
    }
    return earliest;
}

quic_operation_status quic_server::handle_expiry(quic_timestamp now) {
    if (!impl_) {
        return quic_operation_status::retired;
    }
    auto status = quic_operation_status::need_input;
    for (auto& [token, active] : impl_->active_by_token_) {
        (void)token;
        const auto current = active.connection_->handle_expiry(now);
        if (current != quic_operation_status::need_input && current != quic_operation_status::accepted) {
            status = current;
        }
    }
    return status;
}

quic_operation_status quic_server::retire(quic_connection_token token) noexcept {
    if (!impl_ || token.value_ == 0) {
        return quic_operation_status::retired;
    }
    const auto found = impl_->active_by_token_.find(token.value_);
    if (found == impl_->active_by_token_.end()) {
        return quic_operation_status::retired;
    }
    auto& connection = *found->second.connection_;
    impl::retire_all_cids(&found->second);
    const auto status = connection.retire_from_server();
    impl_->active_by_token_.erase(found);
    return status;
}

std::size_t quic_server::connection_count() const noexcept {
    return impl_ ? impl_->active_by_token_.size() : 0;
}

std::size_t quic_server::pending_connection_count() const noexcept {
    return impl_ ? impl_->pending_by_id_.size() : 0;
}

}  // namespace ruvia
