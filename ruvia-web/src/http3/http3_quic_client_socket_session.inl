#pragma once

#include <stdexcept>
#include <variant>

#include <asio/error.hpp>

#include "http3/http3_quic_socket_address.h"

namespace ruvia::detail {
namespace {

ruvia::quic_address quic_address_from_endpoint(const asio::ip::udp::endpoint& endpoint) {
    const auto address = to_http3_quic_datagram_address(endpoint);
    if ((address.index() != 0)) {
        throw std::invalid_argument("QUIC UDP endpoint is not a supported concrete address");
    }
    return to_quic_address(std::get<0>(address));
}

}  // namespace

template <typename send_type>
bool http3_quic_client_socket_session::send_pending(pump_result_type& result_value, send_type& send) {
    if (pending_packet_size_ == 0) {
        return true;
    }
    asio::error_code error;
    auto* socket = pending_candidate_ && candidate_socket_ ? &*candidate_socket_ : &socket_;
    const auto size = send(*socket,
        asio::buffer(packet_buffer_.data(), pending_packet_size_), error);
    if (error == asio::error::would_block || error == asio::error::try_again) {
        result_value.output_backpressured_ = true;
        return true;
    }
    if (error || size != pending_packet_size_) {
        if (pending_candidate_ && migration_id_) {
            fail_candidate_migration();
            pending_packet_size_ = 0;
            pending_candidate_ = false;
            result_value.output_backpressured_ = false;
            return true;
        }
        result_value.status_ = pump_status_type::fatal;
        return false;
    }
    pending_packet_size_ = 0;
    pending_candidate_ = false;
    ++result_value.sent_;
    return true;
}

template <typename send_type>
http3_quic_client_socket_session::pump_result_type
http3_quic_client_socket_session::pump_with_send(send_type& send) {
    require_owner_thread();
    pump_result_type result;
    if (closed_ || stopping_) {
        result.status_ = pump_status_type::closed;
        return result;
    }

    if (!send_pending(result, send)) {
        return result;
    }

    auto& connection = transport_.connection();
    const auto receive_from = [&](asio::ip::udp::socket& socket,
                                  const asio::ip::udp::endpoint& local_endpoint,
                                  std::size_t& received_value, bool candidate_value) {
        for (std::size_t packet = 0; packet < batch_size; ++packet) {
            asio::ip::udp::endpoint sender;
            asio::error_code error;
            const auto size = socket.receive_from(asio::buffer(receive_buffer_), sender, 0, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                break;
            }
            if (error) {
                if (candidate_value) {
                    fail_candidate_migration();
                    return true;
                }
                result.status_ = pump_status_type::fatal;
                return false;
            }
            if (sender != peer_) {
                continue;
            }
            ++received_value;
            if (size == 0) {
                continue;
            }
            const auto datagram = ruvia::quic_datagram_view{
                std::span<const std::byte>(receive_buffer_.data(), size),
                quic_address_from_endpoint(local_endpoint),
                quic_address_from_endpoint(sender)};
            (void)connection.receive(datagram, std::chrono::steady_clock::now());
        }
        return true;
    };
    if (!receive_from(socket_, local_endpoint_, result.received_, false) ||
        (candidate_socket_ && !receive_from(*candidate_socket_, *candidate_local_endpoint_,
                                  result.received_, true))) {
        return result;
    }
    settle_migration();

    const auto now = std::chrono::steady_clock::now();
    (void)transport_.handle_expiry(now);
    settle_migration();
    if (connection.info().early_data_ == ruvia::quic_early_data_state::rejected) {
        reset_rejected_early_streams();
    }

    const auto connection_info = connection.info();
    if (connection_info.state_ == ruvia::quic_connection_state::failed ||
        connection_info.state_ == ruvia::quic_connection_state::retired) {
        result.status_ = pump_status_type::fatal;
        return result;
    }
    const bool early_streams_allowed = early_data_enabled_ &&
                                       connection_info.early_data_ ==
                                           ruvia::quic_early_data_state::available;
    if (connection_info.state_ == ruvia::quic_connection_state::ready || early_streams_allowed) {
        const auto critical = critical_streams_->drive(
            [&connection](http3_critical_stream_driver::kind_type) {
                return connection.open_stream(true);
            },
            [&connection](std::uint64_t id, std::span<const char> bytes_value) {
                return connection.write_stream(id, std::as_bytes(bytes_value));
            });
        if (critical == http3_critical_stream_driver::result_type::fatal) {
            result.status_ = pump_status_type::fatal;
            return result;
        }
        result.critical_streams_ready_ = critical == http3_critical_stream_driver::result_type::ready;
        result.critical_output_progress_ = critical == http3_critical_stream_driver::result_type::progress;
        for (std::size_t index = 0; index < 3; ++index) {
            const auto id = critical_streams_->stream_id(
                static_cast<http3_critical_stream_driver::kind_type>(index));
            if (!id) {
                continue;
            }
            const auto health = connection.write_health(*id);
            if (health != ruvia::quic_operation_status::accepted &&
                health != ruvia::quic_operation_status::would_block &&
                health != ruvia::quic_operation_status::need_input) {
                result.status_ = pump_status_type::fatal;
                return result;
            }
        }
    }

    for (std::size_t packet = 0; pending_packet_size_ == 0 && packet < batch_size; ++packet) {
        const auto output = transport_.write_packet(packet_buffer_, now);
        if (output.size_ == 0) {
            break;
        }
        if (output.size_ > packet_buffer_.size()) {
            result.status_ = pump_status_type::fatal;
            return result;
        }
        const auto target = to_udp_endpoint(from_quic_address(output.peer_));
        const auto local = to_udp_endpoint(from_quic_address(output.local_));
        if ((target.index() != 0) || std::get<0>(target) != peer_ || (local.index() != 0)) {
            result.status_ = pump_status_type::fatal;
            return result;
        }
        if (failed_migration_local_endpoint_ &&
            std::get<0>(local) == *failed_migration_local_endpoint_) {
            continue;
        }
        if (std::get<0>(local) == local_endpoint_) {
            pending_candidate_ = false;
        } else if (candidate_local_endpoint_ && std::get<0>(local) == *candidate_local_endpoint_) {
            pending_candidate_ = true;
        } else {
            result.status_ = pump_status_type::fatal;
            return result;
        }
        pending_packet_size_ = output.size_;
        if (!send_pending(result, send) || result.output_backpressured_) {
            break;
        }
        settle_migration();
    }

    if (connection.info().state_ == ruvia::quic_connection_state::ready) {
        result.critical_streams_ready_ = critical_streams_->complete();
    }
    if (const auto expiry = transport_.next_expiry()) {
        result.event_timeout_ = *expiry > now ? *expiry - now
                                              : std::chrono::steady_clock::duration::zero();
    }
    if (result.received_ == 0 && result.sent_ == 0 && !result.critical_output_progress_ &&
        result.status_ == pump_status_type::active) {
        result.status_ = pump_status_type::would_block;
    }
    return result;
}

}  // namespace ruvia::detail
