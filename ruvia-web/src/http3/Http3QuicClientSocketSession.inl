#pragma once

#include <stdexcept>

#include <asio/error.hpp>

#include "http3/Http3QuicSocketAddress.h"

namespace ruvia::detail {
namespace {

ruvia::quic_address quic_address_from_endpoint(const asio::ip::udp::endpoint& endpoint) {
    const auto address = to_http3_quic_datagram_address(endpoint);
    if (!address) {
        throw std::invalid_argument("QUIC UDP endpoint is not a supported concrete address");
    }
    return to_quic_address(*address);
}

}  // namespace

template <typename Send>
bool Http3QuicClientSocketSession::send_pending(PumpResult& result, Send& send) {
    if (pending_packet_size_ == 0) {
        return true;
    }
    asio::error_code error;
    auto* socket = pending_candidate_ && candidate_socket_ ? &*candidate_socket_ : &socket_;
    const auto size = send(*socket,
        asio::buffer(packetBuffer_.data(), pending_packet_size_), error);
    if (error == asio::error::would_block || error == asio::error::try_again) {
        result.outputBackpressured = true;
        return true;
    }
    if (error || size != pending_packet_size_) {
        if (pending_candidate_ && migration_id_) {
            fail_candidate_migration();
            pending_packet_size_ = 0;
            pending_candidate_ = false;
            result.outputBackpressured = false;
            return true;
        }
        result.status = PumpStatus::kFatal;
        return false;
    }
    pending_packet_size_ = 0;
    pending_candidate_ = false;
    ++result.sent;
    return true;
}

template <typename Send>
Http3QuicClientSocketSession::PumpResult
Http3QuicClientSocketSession::pump_with_send(Send& send) {
    requireOwnerThread();
    PumpResult result;
    if (closed_ || stopping_) {
        result.status = PumpStatus::kClosed;
        return result;
    }

    if (!send_pending(result, send)) {
        return result;
    }

    auto& connection = transport_.connection();
    const auto receiveFrom = [&](asio::ip::udp::socket& socket,
                                 const asio::ip::udp::endpoint& local_endpoint,
                                 std::size_t& received, bool candidate) {
        for (std::size_t packet = 0; packet < kBatchSize; ++packet) {
            asio::ip::udp::endpoint sender;
            asio::error_code error;
            const auto size = socket.receive_from(asio::buffer(receiveBuffer_), sender, 0, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                break;
            }
            if (error) {
                if (candidate) {
                    fail_candidate_migration();
                    return true;
                }
                result.status = PumpStatus::kFatal;
                return false;
            }
            if (sender != peer_) {
                continue;
            }
            ++received;
            if (size == 0) {
                continue;
            }
            const auto datagram = ruvia::quic_datagram_view{
                std::span<const std::byte>(receiveBuffer_.data(), size),
                quic_address_from_endpoint(local_endpoint),
                quic_address_from_endpoint(sender)};
            (void)connection.receive(datagram, std::chrono::steady_clock::now());
        }
        return true;
    };
    if (!receiveFrom(socket_, localEndpoint_, result.received, false) ||
        (candidate_socket_ && !receiveFrom(*candidate_socket_, *candidate_local_endpoint_,
                                  result.received, true))) {
        return result;
    }
    settle_migration();

    const auto now = std::chrono::steady_clock::now();
    (void)transport_.handle_expiry(now);
    settle_migration();
    if (connection.info().early_data == ruvia::quic_early_data_state::rejected) {
        reset_rejected_early_streams();
    }

    const auto connection_info = connection.info();
    if (connection_info.state == ruvia::quic_connection_state::failed ||
        connection_info.state == ruvia::quic_connection_state::retired) {
        result.status = PumpStatus::kFatal;
        return result;
    }
    const bool earlyStreamsAllowed = early_data_enabled_ &&
                                     connection_info.early_data ==
                                         ruvia::quic_early_data_state::available;
    if (connection_info.state == ruvia::quic_connection_state::ready || earlyStreamsAllowed) {
        const auto critical = criticalStreams_->drive(
            [&connection](Http3CriticalStreamDriver::Kind) {
                return connection.open_stream(true);
            },
            [&connection](std::uint64_t id, std::span<const char> bytes) {
                return connection.write_stream(id, std::as_bytes(bytes));
            });
        if (critical == Http3CriticalStreamDriver::Result::kFatal) {
            result.status = PumpStatus::kFatal;
            return result;
        }
        result.criticalStreamsReady = critical == Http3CriticalStreamDriver::Result::kReady;
        result.criticalOutputProgress = critical == Http3CriticalStreamDriver::Result::kProgress;
        for (std::size_t index = 0; index < 3; ++index) {
            const auto id = criticalStreams_->streamId(
                static_cast<Http3CriticalStreamDriver::Kind>(index));
            if (!id) {
                continue;
            }
            const auto health = connection.write_health(*id);
            if (health != ruvia::quic_operation_status::accepted &&
                health != ruvia::quic_operation_status::would_block &&
                health != ruvia::quic_operation_status::need_input) {
                result.status = PumpStatus::kFatal;
                return result;
            }
        }
    }

    for (std::size_t packet = 0; pending_packet_size_ == 0 && packet < kBatchSize; ++packet) {
        const auto output = transport_.write_packet(packetBuffer_, now);
        if (output.size == 0) {
            break;
        }
        if (output.size > packetBuffer_.size()) {
            result.status = PumpStatus::kFatal;
            return result;
        }
        const auto target = to_udp_endpoint(from_quic_address(output.peer));
        const auto local = to_udp_endpoint(from_quic_address(output.local));
        if (!target || *target != peer_ || !local) {
            result.status = PumpStatus::kFatal;
            return result;
        }
        if (failed_migration_local_endpoint_ &&
            *local == *failed_migration_local_endpoint_) {
            continue;
        }
        if (*local == localEndpoint_) {
            pending_candidate_ = false;
        } else if (candidate_local_endpoint_ && *local == *candidate_local_endpoint_) {
            pending_candidate_ = true;
        } else {
            result.status = PumpStatus::kFatal;
            return result;
        }
        pending_packet_size_ = output.size;
        if (!send_pending(result, send) || result.outputBackpressured) {
            break;
        }
        settle_migration();
    }

    if (connection.info().state == ruvia::quic_connection_state::ready) {
        result.criticalStreamsReady = criticalStreams_->complete();
    }
    if (const auto expiry = transport_.next_expiry()) {
        result.eventTimeout = *expiry > now ? *expiry - now
                                            : std::chrono::steady_clock::duration::zero();
    }
    if (result.received == 0 && result.sent == 0 && !result.criticalOutputProgress &&
        result.status == PumpStatus::kActive) {
        result.status = PumpStatus::kWouldBlock;
    }
    return result;
}

}  // namespace ruvia::detail
