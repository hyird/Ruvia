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

#include "ruvia/http/detail/http3/quic_cid_registry.h"

namespace ruvia {
namespace {

struct connection_deleter final {
    std::pmr::memory_resource* resource{};
    void operator()(quic_connection* value) const noexcept {
        if (!value) {
            return;
        }
        std::destroy_at(value);
        std::pmr::polymorphic_allocator<quic_connection>(resource).deallocate(value, 1);
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
    ngtcp2_cid result{};
    result.datalen = cid.size();
    if (cid.size()) {
        std::memcpy(result.data, cid.view().data(), cid.size());
    }
    return result;
}

bool valid_varint(std::uint64_t value) noexcept {
    return value < (std::uint64_t{1} << 62);
}

}  // namespace

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
            : bytes(resource) {}
        quic_initial_offer offer{};
        std::pmr::vector<std::byte> bytes;
    };

    struct active final {
        active(impl* server, quic_connection_token connection_token,
            quic_connection* value, std::pmr::memory_resource* resource)
            : owner(server),
              token(connection_token),
              connection(value, connection_deleter{resource}),
              ids(resource) {}
        impl* owner{};
        quic_connection_token token{};
        connection_owner connection;
        std::pmr::vector<quic_connection_id> ids;
    };

    impl(quic_server_config source, quic_crypto_provider_view provider,
        std::pmr::memory_resource* memory)
        : config(std::move(source)),
          crypto(provider),
          resource(memory),
          pending_by_id(memory),
          pending_by_dcid(memory),
          active_by_token(memory),
          cid_to_token(memory) {}

    quic_server_config config;
    quic_crypto_provider_view crypto;
    std::pmr::memory_resource* resource;
    std::pmr::unordered_map<std::uint64_t, pending> pending_by_id;
    std::pmr::unordered_map<quic_connection_id, std::uint64_t, cid_hash> pending_by_dcid;
    std::pmr::unordered_map<std::uint64_t, active> active_by_token;
    std::pmr::unordered_map<quic_connection_id, quic_connection_token, cid_hash> cid_to_token;
    std::size_t pending_bytes{};
    std::uint64_t next_offer_id{1};
    std::uint64_t next_token{1};

    static void publish_cid(void* context, std::span<const std::byte> bytes) {
        auto& entry = *static_cast<active*>(context);
        if (bytes.size() > quic_max_connection_id_size) {
            throw std::invalid_argument("QUIC local connection ID exceeds 20 bytes");
        }
        const quic_connection_id cid(bytes);
        if (entry.owner->cid_to_token.contains(cid)) {
            throw std::runtime_error("QUIC local CID collision");
        }
        entry.owner->cid_to_token.emplace(cid, entry.token);
        try {
            entry.ids.push_back(cid);
        } catch (...) {
            entry.owner->cid_to_token.erase(cid);
            throw;
        }
    }

    static void retire_cid(void* context, std::span<const std::byte> bytes) noexcept {
        auto& entry = *static_cast<active*>(context);
        if (bytes.size() > quic_max_connection_id_size) {
            return;
        }
        const quic_connection_id cid(bytes);
        const auto found = entry.owner->cid_to_token.find(cid);
        if (found != entry.owner->cid_to_token.end() && found->second == entry.token) {
            entry.owner->cid_to_token.erase(found);
        }
        const auto known = std::ranges::find(entry.ids, cid);
        if (known != entry.ids.end()) {
            entry.ids.erase(known);
        }
    }

    static void retire_all_cids(void* context) noexcept {
        auto& entry = *static_cast<active*>(context);
        while (!entry.ids.empty()) {
            const auto cid = entry.ids.back();
            retire_cid(&entry, cid.view());
        }
    }

    static detail::quic_cid_registry_view cid_registry(active& entry) noexcept {
        return {.context = &entry, .publish = publish_cid, .retire = retire_cid, .retire_all = retire_all_cids};
    }
};

void quic_server::impl_deleter::operator()(impl* value) const noexcept {
    if (!value) {
        return;
    }
    std::destroy_at(value);
    std::pmr::polymorphic_allocator<impl>(resource).deallocate(value, 1);
}

quic_server::quic_server(quic_server_config config, quic_crypto_provider_view crypto,
    std::pmr::memory_resource* resource) {
    if (!resource) {
        throw std::invalid_argument("QUIC server requires a memory resource");
    }
    crypto.validate();
    const auto& transport = config.local_transport_parameters;
    const auto& limits = config.limits;
    constexpr std::uint64_t stream_count_max = (std::uint64_t{1} << 60) - 1;
    const bool stream_counts_valid = transport.initial_max_streams_bidi <= stream_count_max &&
                                     transport.initial_max_streams_uni <= stream_count_max &&
                                     transport.initial_max_streams_bidi <= limits.max_streams &&
                                     transport.initial_max_streams_uni <= limits.max_streams -
                                                                              static_cast<std::size_t>(std::min<std::uint64_t>(
                                                                                  transport.initial_max_streams_bidi, limits.max_streams));
    const auto initial_stream_count = stream_counts_valid
                                          ? transport.initial_max_streams_bidi + transport.initial_max_streams_uni
                                          : std::uint64_t{};
    if ((config.version != quic_version::v1 && config.version != quic_version::v2) ||
        transport.max_udp_payload_size < 1200 ||
        transport.max_udp_payload_size > 65527 || limits.max_datagram_size < transport.max_udp_payload_size ||
        limits.max_datagram_size > 65527 || limits.max_crypto_buffer_size == 0 ||
        limits.max_stream_buffer_size == 0 || limits.max_connection_buffer_size == 0 ||
        limits.max_streams == 0 || limits.max_local_streams == 0 || limits.max_datagrams == 0 ||
        transport.initial_max_data > limits.max_connection_buffer_size ||
        transport.initial_max_stream_data_bidi_local > limits.max_stream_buffer_size ||
        transport.initial_max_stream_data_bidi_remote > limits.max_stream_buffer_size ||
        transport.initial_max_stream_data_uni > limits.max_stream_buffer_size ||
        !stream_counts_valid || limits.max_local_streams > limits.max_streams - static_cast<std::size_t>(initial_stream_count) ||
        initial_stream_count > limits.max_lifetime_peer_streams ||
        transport.idle_timeout_ms > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / 1'000'000 ||
        transport.active_connection_id_limit < 2 ||
        !valid_varint(config.local_transport_parameters.idle_timeout_ms) ||
        !valid_varint(config.local_transport_parameters.max_udp_payload_size) ||
        !valid_varint(config.local_transport_parameters.initial_max_data) ||
        !valid_varint(config.local_transport_parameters.initial_max_stream_data_bidi_local) ||
        !valid_varint(config.local_transport_parameters.initial_max_stream_data_bidi_remote) ||
        !valid_varint(config.local_transport_parameters.initial_max_stream_data_uni) ||
        !valid_varint(config.local_transport_parameters.initial_max_streams_bidi) ||
        !valid_varint(config.local_transport_parameters.initial_max_streams_uni) ||
        !valid_varint(config.local_transport_parameters.active_connection_id_limit) ||
        !valid_varint(config.local_transport_parameters.max_datagram_frame_size)) {
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
        for (auto& [token, active] : impl_->active_by_token) {
            (void)token;
            active.connection->retire_from_server();
        }
        impl_.reset();
    }
}

quic_server_route quic_server::route_datagram(const quic_datagram_view& datagram) {
    if (!impl_ || datagram.bytes.empty()) {
        return {};
    }
    ngtcp2_version_cid decoded{};
    const auto* packet = reinterpret_cast<const std::uint8_t*>(datagram.bytes.data());
    const int decode_result = ngtcp2_pkt_decode_version_cid(&decoded, packet,
        datagram.bytes.size(), 16);
    if (decode_result != 0 && decode_result != NGTCP2_ERR_VERSION_NEGOTIATION) {
        return {};
    }

    if (!decoded.dcid || decoded.dcidlen > quic_max_connection_id_size ||
        decoded.scidlen > quic_max_connection_id_size) {
        return {};
    }
    quic_connection_id dcid(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(decoded.dcid), decoded.dcidlen));
    // Short headers carry no version. Route their destination CID before
    // interpreting version zero as a long-header Version Negotiation packet.
    if ((std::to_integer<unsigned char>(datagram.bytes.front()) & 0x80U) == 0) {
        if (const auto found = impl_->cid_to_token.find(dcid); found != impl_->cid_to_token.end()) {
            return {.kind = quic_server_route_kind::existing_connection, .connection = found->second};
        }
        return {};
    }
    if (decoded.version == 0) {
        return {};
    }
    if (decoded.version != static_cast<std::uint32_t>(quic_version::v1) &&
        decoded.version != static_cast<std::uint32_t>(quic_version::v2)) {
        if (datagram.bytes.size() < 1200 || decoded.dcidlen > quic_max_connection_id_size ||
            decoded.scidlen > quic_max_connection_id_size) {
            return {};
        }
        quic_version_negotiation_plan plan;
        plan.destination_connection_id_ = quic_connection_id(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(decoded.scid), decoded.scidlen));
        plan.source_connection_id_ = dcid;
        plan.local_address_ = datagram.local;
        plan.peer_address_ = datagram.peer;
        plan.input_size_ = datagram.bytes.size();
        plan.valid_ = true;
        return {.kind = quic_server_route_kind::version_negotiation,
            .version_negotiation = std::move(plan)};
    }
    if (const auto found = impl_->cid_to_token.find(dcid); found != impl_->cid_to_token.end()) {
        return {.kind = quic_server_route_kind::existing_connection, .connection = found->second};
    }
    if (datagram.bytes.size() < 1200 || decode_result != 0 || decoded.dcidlen < 8 ||
        decoded.dcidlen > quic_max_connection_id_size || decoded.scidlen > quic_max_connection_id_size) {
        return {};
    }

    ngtcp2_pkt_hd header{};
    if (ngtcp2_accept(&header, packet, datagram.bytes.size()) != 0 ||
        header.type != NGTCP2_PKT_INITIAL ||
        (header.version != NGTCP2_PROTO_VER_V1 && header.version != NGTCP2_PROTO_VER_V2) ||
        header.tokenlen != 0 || header.dcid.datalen < 8 ||
        header.dcid.datalen > quic_max_connection_id_size ||
        header.scid.datalen > quic_max_connection_id_size) {
        return {};
    }
    const auto initial_dcid = from_native_cid(header.dcid);
    const auto initial_scid = from_native_cid(header.scid);
    if (const auto existing_pending = impl_->pending_by_dcid.find(initial_dcid);
        existing_pending != impl_->pending_by_dcid.end()) {
        const auto& existing = impl_->pending_by_id.at(existing_pending->second).offer;
        if (existing.source_connection_id != initial_scid) {
            return {};
        }
        return {.kind = quic_server_route_kind::initial_offer, .offer = existing};
    }
    if (impl_->pending_by_id.size() >= impl_->config.max_pending_connections ||
        datagram.bytes.size() > impl_->config.max_pending_datagram_bytes -
                                    std::min(impl_->pending_bytes, impl_->config.max_pending_datagram_bytes)) {
        return {.kind = quic_server_route_kind::rejected,
            .status = quic_operation_status::would_block};
    }
    const auto offer_id = next_nonzero(impl_->next_offer_id, "QUIC Initial offer ID exhausted");
    quic_initial_offer offer{.offer_id = offer_id,
        .local_address = datagram.local,
        .peer_address = datagram.peer,
        .destination_connection_id = initial_dcid,
        .source_connection_id = initial_scid,
        .original_destination_connection_id = initial_dcid,
        .version = static_cast<quic_version>(header.version)};
    impl::pending pending(impl_->resource);
    pending.offer = offer;
    pending.bytes.assign(datagram.bytes.begin(), datagram.bytes.end());
    auto [it, inserted] = impl_->pending_by_id.emplace(offer_id, std::move(pending));
    if (!inserted) {
        throw std::logic_error("QUIC Initial offer ID collision");
    }
    try {
        impl_->pending_by_dcid.emplace(initial_dcid, offer_id);
    } catch (...) {
        impl_->pending_by_id.erase(it);
        throw;
    }
    impl_->pending_bytes += datagram.bytes.size();
    return {.kind = quic_server_route_kind::initial_offer, .offer = offer};
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
        return {.status = quic_operation_status::would_block};
    }
    output = output.first(std::min(output.size(), amplification_limit));
    std::byte random{};
    impl_->crypto.random_bytes(impl_->crypto.context, std::span<std::byte>(&random, 1));
    constexpr std::array<std::uint32_t, 2> versions{NGTCP2_PROTO_VER_V1, NGTCP2_PROTO_VER_V2};
    const auto written = ngtcp2_pkt_write_version_negotiation(
        reinterpret_cast<std::uint8_t*>(output.data()), output.size(),
        std::to_integer<std::uint8_t>(random), dcid.data, dcid.datalen,
        scid.data, scid.datalen, versions.data(), versions.size());
    if (written == NGTCP2_ERR_NOBUF) {
        return {.status = quic_operation_status::would_block};
    }
    if (written < 0) {
        throw std::runtime_error("ngtcp2 failed to write Version Negotiation packet");
    }
    plan.valid_ = false;
    return {.status = quic_operation_status::accepted, .size = static_cast<std::size_t>(written), .local = plan.local_address_, .peer = plan.peer_address_};
}

quic_server_admit_result quic_server::admit_initial(const quic_initial_offer& offer,
    quic_tls_driver_view tls_driver, quic_timestamp now) {
    if (!impl_) {
        throw std::logic_error("QUIC server is not initialized");
    }
    const auto pending_it = impl_->pending_by_id.find(offer.offer_id);
    if (pending_it == impl_->pending_by_id.end() ||
        pending_it->second.offer.destination_connection_id != offer.destination_connection_id ||
        pending_it->second.offer.source_connection_id != offer.source_connection_id ||
        pending_it->second.offer.original_destination_connection_id != offer.original_destination_connection_id ||
        pending_it->second.offer.local_address.bytes != offer.local_address.bytes ||
        pending_it->second.offer.local_address.port != offer.local_address.port ||
        pending_it->second.offer.local_address.scope_id != offer.local_address.scope_id ||
        pending_it->second.offer.local_address.family != offer.local_address.family ||
        pending_it->second.offer.peer_address.bytes != offer.peer_address.bytes ||
        pending_it->second.offer.peer_address.port != offer.peer_address.port ||
        pending_it->second.offer.peer_address.scope_id != offer.peer_address.scope_id ||
        pending_it->second.offer.peer_address.family != offer.peer_address.family ||
        offer.local_address.port == 0 || offer.peer_address.port == 0 ||
        (offer.local_address.family != quic_address_family::ipv4 &&
            offer.local_address.family != quic_address_family::ipv6) ||
        (offer.peer_address.family != quic_address_family::ipv4 &&
            offer.peer_address.family != quic_address_family::ipv6) ||
        (offer.version != quic_version::v1 && offer.version != quic_version::v2)) {
        throw std::invalid_argument("invalid or stale QUIC Initial offer");
    }
    tls_driver.validate();
    if (impl_->active_by_token.size() >= impl_->config.max_active_connections) {
        return {.status = quic_operation_status::would_block};
    }
    auto& saved = pending_it->second;
    quic_connection_config config{.role = quic_role::server,
        .version = offer.version,
        .preferred_version = impl_->config.version,
        .local_address = offer.local_address,
        .peer_address = offer.peer_address,
        .destination_connection_id = offer.source_connection_id,
        .original_destination_connection_id = offer.original_destination_connection_id,
        .local_transport_parameters = impl_->config.local_transport_parameters,
        .limits = impl_->config.limits};
    std::array<std::byte, 16> server_source_id_bytes{};
    bool source_id_selected{};
    for (std::size_t attempt = 0; attempt < 4; ++attempt) {
        impl_->crypto.random_bytes(impl_->crypto.context, server_source_id_bytes);
        const quic_connection_id candidate(server_source_id_bytes);
        if (candidate != offer.original_destination_connection_id &&
            !impl_->cid_to_token.contains(candidate)) {
            config.source_connection_id = candidate;
            source_id_selected = true;
            break;
        }
    }
    if (!source_id_selected) {
        throw std::runtime_error("QUIC server could not generate a collision-free source CID");
    }
    const auto server_source_id = *config.source_connection_id;
    const quic_connection_token token{next_nonzero(impl_->next_token, "QUIC connection token exhausted")};
    std::pmr::polymorphic_allocator<quic_connection> allocator(impl_->resource);
    auto* raw = allocator.allocate(1);
    try {
        ::new (static_cast<void*>(raw)) quic_connection(
            offer, std::move(config), impl_->crypto, tls_driver, impl_->resource, now);
    } catch (...) {
        allocator.deallocate(raw, 1);
        throw;
    }
    auto active_it = impl_->active_by_token.end();
    try {
        auto insertion = impl_->active_by_token.try_emplace(
            token.value, impl_.get(), token, nullptr, impl_->resource);
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
    active_it->second.connection.reset(raw);
    try {
        raw->bind_server_cid_registry(impl::cid_registry(active_it->second));
        impl::publish_cid(&active_it->second, server_source_id.view());
        impl::publish_cid(&active_it->second, offer.original_destination_connection_id.view());
        const auto datagram = quic_datagram_view{.bytes = saved.bytes,
            .local = offer.local_address,
            .peer = offer.peer_address};
        raw->receive(datagram, now);
    } catch (...) {
        while (!active_it->second.ids.empty()) {
            const auto cid = active_it->second.ids.back();
            impl::retire_cid(&active_it->second, cid.view());
        }
        active_it->second.connection->retire_from_server();
        impl_->active_by_token.erase(active_it);
        throw;
    }
    impl_->pending_bytes -= saved.bytes.size();
    impl_->pending_by_dcid.erase(offer.destination_connection_id);
    impl_->pending_by_id.erase(pending_it);
    return {.status = quic_operation_status::accepted, .connection = token};
}

quic_connection& quic_server::connection(quic_connection_token token) {
    const auto found = impl_->active_by_token.find(token.value);
    if (token.value == 0 || found == impl_->active_by_token.end()) {
        throw std::out_of_range("stale QUIC connection token");
    }
    return *found->second.connection;
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
    for (const auto& [token, active] : impl_->active_by_token) {
        (void)token;
        const auto expiry = active.connection->next_expiry();
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
    for (auto& [token, active] : impl_->active_by_token) {
        (void)token;
        const auto current = active.connection->handle_expiry(now);
        if (current != quic_operation_status::need_input && current != quic_operation_status::accepted) {
            status = current;
        }
    }
    return status;
}

quic_operation_status quic_server::retire(quic_connection_token token) noexcept {
    if (!impl_ || token.value == 0) {
        return quic_operation_status::retired;
    }
    const auto found = impl_->active_by_token.find(token.value);
    if (found == impl_->active_by_token.end()) {
        return quic_operation_status::retired;
    }
    auto& connection = *found->second.connection;
    impl::retire_all_cids(&found->second);
    const auto status = connection.retire_from_server();
    impl_->active_by_token.erase(found);
    return status;
}

std::size_t quic_server::connection_count() const noexcept {
    return impl_ ? impl_->active_by_token.size() : 0;
}

std::size_t quic_server::pending_connection_count() const noexcept {
    return impl_ ? impl_->pending_by_id.size() : 0;
}

}  // namespace ruvia
