#include "ruvia/http/http3_connection.h"

#include <algorithm>
#include <array>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

#include "http3/http3_connection_impl.h"

namespace ruvia {
namespace {

[[nodiscard]] http3_connection_error_code early_settings_error(const http3_settings& remembered,
    const http3_settings& actual) noexcept {
    // RFC 9204 section 3.2.3 overrides the general HTTP/3 limit rule: a
    // remembered nonzero maximum table capacity must be unchanged.
    if (remembered.qpack_max_table_capacity_ != 0 &&
        actual.qpack_max_table_capacity_ != remembered.qpack_max_table_capacity_) {
        return http3_connection_error_code::qpack_decoder_stream_error;
    }
    // RFC 9114 section 7.2.4.2 also requires remembered nondefault settings
    // to be present. Omitting a finite field limit is not a valid relaxation.
    const bool invalid_field_limit = remembered.max_field_section_size_
                                         ? !actual.max_field_section_size_ || *actual.max_field_section_size_ < *remembered.max_field_section_size_
                                         : actual.max_field_section_size_.has_value();
    if (invalid_field_limit || actual.qpack_blocked_streams_ < remembered.qpack_blocked_streams_ ||
        (remembered.enable_connect_protocol_ && !actual.enable_connect_protocol_) ||
        (remembered.h3_datagram_ && !actual.h3_datagram_)) {
        return http3_connection_error_code::settings_error;
    }
    return http3_connection_error_code::no_error;
}

}  // namespace

bool http3_connection::impl_type::prepare_peer_settings() {
    if (failed_.scope_ != http3_connection_error_scope::none) {
        return false;
    }
    if (!control_.peer_settings() || early_data_phase_ == early_data_phase::pending || transmit_qpack_configured_) {
        return true;
    }
    const auto& settings = *control_.peer_settings();
    if (early_data_phase_ == early_data_phase::accepted) {
        const auto error = early_settings_error(*remembered_peer_settings_, settings);
        if (error != http3_connection_error_code::no_error) {
            failed_ = connection_error(error);
            return false;
        }
    }
    transmit_qpack_->configure_peer_settings({.max_table_capacity_ = static_cast<std::size_t>(settings.qpack_max_table_capacity_),
        .max_blocked_streams_ = static_cast<std::size_t>(std::min<std::uint64_t>(settings.qpack_blocked_streams_, limits_.max_active_streams_)),
        .fields_ = {limits_.max_encoded_field_section_bytes_, limits_.max_field_section_size_, limits_.max_fields_},
        .table_capacity_ = static_cast<std::size_t>(std::min<std::uint64_t>(settings.qpack_max_table_capacity_, 4096))});
    transmit_qpack_configured_ = true;
    return true;
}

http3_connection::http3_connection(http3_peer_role local_role, std::pmr::memory_resource* resource,
    http3_connection_config limits)
    : resource_(resource),
      impl_(nullptr) {
    if (resource == nullptr || limits.max_active_streams_ == 0 || limits.max_peer_unidirectional_streams_ == 0 || limits.max_remembered_pushes_ == 0 ||
        (limits.max_push_id_ && *limits.max_push_id_ > http3_var_int_max)) {
        throw std::invalid_argument("HTTP/3 connection resource and limits must be valid");
    }
    std::pmr::polymorphic_allocator<impl_type> allocator(resource);
    impl_ = allocator.allocate(1);
    try {
        std::allocator_traits<decltype(allocator)>::construct(allocator, impl_, local_role, resource, limits);
    } catch (...) {
        allocator.deallocate(impl_, 1);
        impl_ = nullptr;
        throw;
    }
}

http3_connection::~http3_connection() {
    if (impl_ != nullptr && !retire()) {
        std::terminate();
    }
}

http3_connection::http3_connection(http3_connection&& other) noexcept
    : resource_(other.resource_),
      impl_(std::exchange(other.impl_, nullptr)) {}

http3_connection& http3_connection::operator=(http3_connection&& other) noexcept {
    if (this != &other) {
        http3_connection temporary(std::move(other));
        std::swap(resource_, temporary.resource_);
        std::swap(impl_, temporary.impl_);
    }
    return *this;
}

http3_connection_result http3_connection::begin_early_data(http3_settings remembered) {
    if (impl_ == nullptr || impl_->role_ != http3_peer_role::client || impl_->feeding_) {
        throw std::logic_error("begin_early_data() requires an open client connection outside feed");
    }
    auto& impl = *impl_;
    if (impl.failed_.scope_ != http3_connection_error_scope::none) {
        return impl.failed_;
    }
    if (impl.early_data_phase_ != impl_type::early_data_phase::inactive || impl.peer_feed_started_ || impl.field_encoding_started_) {
        throw std::logic_error("begin_early_data() must precede peer input and local field sections");
    }
    std::array<char, 80> encoded{};
    if (encode_http3_settings(encoded, remembered).index() != 0) {
        throw std::invalid_argument("remembered HTTP/3 SETTINGS exceed the QUIC varint range");
    }
    impl.remembered_peer_settings_ = remembered;
    impl.early_data_phase_ = impl_type::early_data_phase::pending;
    return {};
}

http3_connection_result http3_connection::complete_early_data(bool accepted) {
    if (impl_ == nullptr || impl_->role_ != http3_peer_role::client || impl_->feeding_) {
        throw std::logic_error("complete_early_data() requires an open client connection outside feed");
    }
    auto& impl = *impl_;
    if (impl.failed_.scope_ != http3_connection_error_scope::none) {
        return impl.failed_;
    }
    if (impl.early_data_phase_ != impl_type::early_data_phase::pending) {
        throw std::logic_error("complete_early_data() requires one pending early-data attempt");
    }
    impl.early_data_phase_ = accepted ? impl_type::early_data_phase::accepted : impl_type::early_data_phase::rejected;
    if (!accepted) {
        impl.remembered_peer_settings_.reset();
    }
    try {
        if (!impl.prepare_peer_settings()) {
            return impl.failed_;
        }
        return {};
    } catch (...) {
        impl.failed_ = impl_type::connection_error(http3_connection_error_code::internal_error);
        throw;
    }
}

http3_connection_result http3_connection::register_client_request(std::uint64_t stream_id, http_known_method method) {
    if (impl_ == nullptr || impl_->role_ != http3_peer_role::client ||
        !is_http3_request_stream_id(stream_id)) {
        return impl_type::stream_error(http3_connection_error_code::stream_creation_error);
    }
    auto& impl = *impl_;
    if (impl.feeding_) {
        throw std::logic_error("register_client_request() during HTTP/3 feed");
    }
    if (impl.failed_.scope_ == http3_connection_error_scope::connection) {
        return impl.failed_;
    }
    if (impl.control_.goaway_id()) {
        return impl_type::stream_error(http3_connection_error_code::request_rejected);
    }
    if (impl.client_requests_.contains(stream_id)) {
        return impl_type::stream_error(http3_connection_error_code::stream_creation_error);
    }
    if (impl.client_requests_.size() >= impl.limits_.max_active_streams_) {
        return impl_type::stream_error(http3_connection_error_code::excessive_load);
    }
    impl.client_requests_.try_emplace(stream_id, stream_id, method, impl.resource_, impl.limits_, impl.receive_qpack_, impl);
    return {};
}

http3_connection_result http3_connection::cancel_request(std::uint64_t stream_id) {
    if (!impl_ || impl_->feeding_) {
        return impl_type::stream_error(http3_connection_error_code::general_protocol_error);
    }
    auto& owner_value = *impl_;
    if (owner_value.failed_.scope_ != http3_connection_error_scope::none) {
        return owner_value.failed_;
    }
    const bool live = owner_value.requests_.contains(stream_id) || owner_value.client_requests_.contains(stream_id) || owner_value.push_streams_.contains(stream_id);
    if (!live) {
        return impl_type::stream_error(http3_connection_error_code::stream_creation_error);
    }
    try {
        if (const auto canceled = owner_value.receive_qpack_.cancel(stream_id); (canceled.index() != 0)) {
            return owner_value.failed_ = impl_type::connection_error(http3_connection_error_code::qpack_decompression_failed);
        }
        owner_value.requests_.erase(stream_id);
        owner_value.client_requests_.erase(stream_id);
        if (const auto found = owner_value.push_streams_.find(stream_id); found != owner_value.push_streams_.end()) {
            if (found->second.id_) {
                owner_value.pushes_.at(*found->second.id_).completed_ = true;
            }
            (void)owner_value.peer_streams_.retire_non_critical_stream(stream_id);
            owner_value.push_streams_.erase(found);
        }
        return {http3_connection_status::reset};
    } catch (...) {
        owner_value.failed_ = impl_type::connection_error(http3_connection_error_code::internal_error);
        throw;
    }
}

bool http3_connection::retire_client_request(std::uint64_t stream_id) noexcept {
    if (impl_ == nullptr || impl_->role_ != http3_peer_role::client || impl_->feeding_ ||
        !is_http3_request_stream_id(stream_id)) {
        return false;
    }
    return impl_->client_requests_.erase(stream_id) != 0;
}

bool http3_connection::retire_server_request(std::uint64_t stream_id) noexcept {
    if (impl_ == nullptr || impl_->role_ != http3_peer_role::server || impl_->feeding_ ||
        !is_http3_request_stream_id(stream_id)) {
        return false;
    }
    return impl_->requests_.erase(stream_id) != 0;
}

bool http3_connection::retire() noexcept {
    if (impl_ == nullptr || impl_->feeding_) {
        return false;
    }
    auto* storage = std::exchange(impl_, nullptr);
    std::pmr::polymorphic_allocator<impl_type> allocator(resource_);
    std::allocator_traits<decltype(allocator)>::destroy(allocator, storage);
    allocator.deallocate(storage, 1);
    return true;
}

std::optional<std::uint64_t> http3_connection::peer_max_push_id() const noexcept {
    return impl_ ? impl_->control_.max_push_id() : std::nullopt;
}
const http3_message_head* http3_connection::promised_request(std::uint64_t push_id) const& noexcept {
    if (!impl_) {
        return nullptr;
    }
    const auto found = impl_->pushes_.find(push_id);
    return found == impl_->pushes_.end() || !found->second.head_ ? nullptr : &*found->second.head_;
}
std::span<const char> http3_connection::pending_critical_output(http3_critical_stream_output::stream_kind kind) const& noexcept {
    if (impl_ == nullptr) {
        return {};
    }
    switch (kind) {
        case http3_critical_stream_output::stream_kind::control:
            return std::span<const char>(impl_->control_output_).subspan(impl_->control_consumed_);
        case http3_critical_stream_output::stream_kind::qpack_encoder:
            return impl_->transmit_qpack_->pending_encoder_output();
        case http3_critical_stream_output::stream_kind::qpack_decoder:
            return impl_->receive_qpack_.pending_decoder_output();
    }
    return {};
}
bool http3_connection::consume_critical_output(http3_critical_stream_output::stream_kind kind, std::size_t bytes_value) noexcept {
    if (impl_ == nullptr || impl_->feeding_) {
        return false;
    }
    switch (kind) {
        case http3_critical_stream_output::stream_kind::control: {
            auto& impl = *impl_;
            if (bytes_value > impl.control_output_.size() - impl.control_consumed_) {
                return false;
            }
            impl.control_consumed_ += bytes_value;
            if (impl.control_consumed_ >= impl.control_output_.size() - impl.control_consumed_) {
                // Reclaim history even when output never becomes empty. Moving
                // at most the consumed byte count amortizes partial consumption;
                // retain capacity for the next control frame without allocation.
                impl.compact_control_output();
            }
            return true;
        }
        case http3_critical_stream_output::stream_kind::qpack_encoder:
            return impl_->transmit_qpack_->consume_encoder_output(bytes_value);
        case http3_critical_stream_output::stream_kind::qpack_decoder:
            return impl_->receive_qpack_.consume_decoder_output(bytes_value);
    }
    return false;
}

std::size_t http3_connection::active_request_count() const noexcept {
    return impl_ == nullptr ? 0 : impl_->requests_.size() + impl_->client_requests_.size() + impl_->push_streams_.size();
}

http3_settings http3_connection::local_settings() const noexcept {
    if (impl_ == nullptr) {
        return {};
    }
    const auto& config = impl_->limits_;
    return {.qpack_max_table_capacity_ = config.qpack_max_table_capacity_,
        .max_field_section_size_ = config.max_field_section_size_,
        .qpack_blocked_streams_ = config.qpack_blocked_streams_,
        .enable_connect_protocol_ = config.enable_connect_protocol_,
        .h3_datagram_ = config.enable_datagrams_};
}

const std::optional<http3_settings>& http3_connection::peer_settings() const noexcept {
    static const std::optional<http3_settings> empty;
    return impl_ == nullptr ? empty : impl_->control_.peer_settings();
}

const std::optional<http3_settings>& http3_connection::effective_peer_settings() const noexcept {
    static const std::optional<http3_settings> empty;
    return impl_ == nullptr ? empty : impl_->effective_peer_settings();
}

bool http3_connection::peer_reports_unprocessed(std::uint64_t stream_id,
    std::optional<std::uint64_t> peer_reset_error_code) const noexcept {
    if (impl_ == nullptr || impl_->failed_.scope_ != http3_connection_error_scope::none) {
        return false;
    }
    const auto found = impl_->client_requests_.find(stream_id);
    if (found == impl_->client_requests_.end() || found->second.response_observed_) {
        return false;
    }
    const auto goaway = impl_->control_.goaway_id();
    return (goaway && stream_id >= *goaway) ||
           peer_reset_error_code == static_cast<std::uint64_t>(http3_connection_error_code::request_rejected);
}

std::optional<std::uint64_t> http3_connection::peer_goaway_id() const noexcept {
    return impl_ == nullptr ? std::nullopt : impl_->control_.goaway_id();
}
}  // namespace ruvia
