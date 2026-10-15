#include "ruvia/web/http_datagram_stream.h"

#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/web/http_client_tunnel.h"
#include "ruvia/web/http_tunnel.h"

#include "http/http_capsule_stream_state.h"
namespace ruvia {
http_datagram_stream::http_datagram_stream(http_capsule_stream&& stream)
    : stream_(std::move(stream)) {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
}
http_datagram_stream http_tunnel::datagrams(http_capsule_config config) & {
    return http_datagram_stream(capsules(config));
}
http_datagram_stream http_client_tunnel::datagrams(http_capsule_config config) && {
    if (!response_.state_ || !response_.state_->tunnel_ || !response_.state_->tunnel_->config_.datagrams_ || !response_.state_->tunnel_->accepted_) {
        throw std::logic_error("HTTP Datagrams require an accepted datagram tunnel");
    }
    return http_datagram_stream(std::move(*this).capsules(config));
}
scoped_operation<std::optional<http_datagram>> http_datagram_stream::read() & {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
    stream_.state_->require();
    return ::ruvia::make_scoped_operation(stream_.state_->read_scope_, read_owned(detail::capsule_state_pin(*stream_.state_), false));
}
task<std::optional<http_datagram>> http_datagram_stream::read_owned(detail::capsule_state_pin pin, bool udp) {
    auto& state_value = pin.state();
    detail::running_capsule_operation running(state_value, false);
    for (;;) {
        if (state_value.receive_ended_) {
            co_return std::nullopt;
        }
        bool fin{};
        if (state_value.offset_ == state_value.input_.size()) {
            fin = !co_await state_value.read_input();
        }
        if (!state_value.poll_capsule(fin, detail::capsule_interest::datagrams)) {
            continue;
        }
        if (state_value.type_ != http_datagram_capsule_type) {
            continue;
        }
        auto& owned = state_value.payload_;
        auto bytes_value = std::span<const char>(owned);
        http_datagram_session datagrams;
        std::size_t offset{};
        if (udp) {
            auto decoded = datagrams.receive_udp_datagram(bytes_value);
            if ((decoded.index() != 0)) {
                state_value.abort();
                throw std::runtime_error("malformed CONNECT-UDP datagram");
            }
            if (!std::get<0>(decoded)) {
                owned.clear();
                continue;
            }
            offset = bytes_value.size() - (std::get<0>(decoded))->payload_.size();
        } else {
            auto decoded = datagrams.receive_datagram(bytes_value);
            if ((decoded.index() != 0)) {
                state_value.abort();
                throw std::runtime_error("malformed HTTP Datagram");
            }
            if (!std::get<0>(decoded)) {
                owned.clear();
                continue;
            }
            offset = bytes_value.size() - (*std::get<0>(decoded)).size();
        }
        auto result_value = std::move(owned);
        state_value.payload_ = std::pmr::string(state_value.resource_);
        co_return http_datagram(http_capsule(state_value, http_datagram_capsule_type, std::move(result_value)), offset);
    }
}
scoped_operation<void> http_datagram_stream::send(std::span<const std::byte> payload_value) & {
    return send(std::string_view(reinterpret_cast<const char*>(payload_value.data()), payload_value.size()));
}
scoped_operation<void> http_datagram_stream::send(std::string_view payload_value) & {
    return send_payload(payload_value, false);
}
scoped_operation<void> http_datagram_stream::send_payload(std::string_view payload_value, bool udp) {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
    auto& state_value = *stream_.state_;
    state_value.require();
    if (state_value.send_ended_) {
        throw std::logic_error("HTTP Datagram sending direction is closed");
    }
    http_datagram_session session_value;
    std::array<char, 24> prefix{};
    std::size_t size{};
    auto prepare = [&] {
        if (udp) {
            auto plan = session_value.prepare_udp_datagram(std::span(payload_value.data(), payload_value.size()));
            if ((plan.index() != 0)) {
                return false;
            }
            std::copy_n(std::get<0>(plan).prefix_.begin(), std::get<0>(plan).prefix_size_, prefix.begin());
            size = std::get<0>(plan).prefix_size_;
        } else {
            auto plan = session_value.prepare_datagram(std::span(payload_value.data(), payload_value.size()));
            if ((plan.index() != 0)) {
                return false;
            }
            std::copy_n(std::get<0>(plan).prefix_.begin(), std::get<0>(plan).prefix_size_, prefix.begin());
            size = std::get<0>(plan).prefix_size_;
        }
        return true;
    };
    if (!prepare()) {
        throw std::length_error("HTTP Datagram payload exceeds its bound");
    }
    if (payload_value.size() + (udp ? 1 : 0) > state_value.config_.max_capsule_length_) {
        throw std::length_error("HTTP Datagram exceeds capsule bound");
    }
    return stream_.write_frame(std::string_view(prefix.data(), size), payload_value);
}
}  // namespace ruvia
