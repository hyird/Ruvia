#include "ruvia/web/HttpDatagramStream.h"

#include <stdexcept>

#include "ruvia/web/HttpClientTunnel.h"
#include "ruvia/web/HttpTunnel.h"
#include "ruvia/web/detail/http/HttpCapsuleStreamState.h"
namespace ruvia {
HttpDatagramStream::HttpDatagramStream(HttpCapsuleStream stream, HttpDatagramSendPolicy policy)
    : stream_(std::move(stream)) {
    if (policy != HttpDatagramSendPolicy::kAutomatic && policy != HttpDatagramSendPolicy::kCapsule && policy != HttpDatagramSendPolicy::kQuic) {
        throw std::invalid_argument("invalid HTTP Datagram send policy");
    }
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
    stream_.state_->datagramPolicy = policy;
}
HttpDatagramStream HttpTunnel::datagrams(HttpDatagramConfig config) & {
    return HttpDatagramStream(capsules(config.capsules), config.sendPolicy);
}
HttpDatagramStream HttpClientTunnel::datagrams(HttpDatagramConfig config) && {
    if (!response_.state_ || !response_.state_->tunnel || !response_.state_->tunnel->config.datagrams || !response_.state_->tunnel->accepted) {
        throw std::logic_error("HTTP Datagrams require an accepted datagram tunnel");
    }
    return HttpDatagramStream(std::move(*this).capsules(config.capsules), config.sendPolicy);
}
ScopedOperation<std::optional<HttpDatagram>> HttpDatagramStream::read() & {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
    stream_.state_->require(false);
    return ::ruvia::make_scoped_operation(stream_.state_->readScope, readOwned(detail::CapsuleStatePin(*stream_.state_), false));
}
Task<std::optional<HttpDatagram>> HttpDatagramStream::readOwned(detail::CapsuleStatePin pin, bool udp) {
    auto& state = pin.state();
    detail::RunningCapsuleOperation running(state);
    for (;;) {
        if (state.receiveEnded) {
            co_return std::nullopt;
        }
        bool fin{};
        std::optional<std::pmr::string> native;
        if (state.offset == state.input.size()) {
            auto input = co_await state.readDatagramInput();
            if (!input) {
                fin = true;
            } else if (input->quic) {
                native.emplace(std::move(input->bytes));
            } else {
                state.input = std::move(input->bytes);
                state.offset = 0;
            }
        }
        const auto transport = native ? HttpDatagramTransport::kQuic : HttpDatagramTransport::kCapsule;
        if (!native) {
            if (!state.pollCapsule(fin, detail::CapsuleInterest::kDatagrams)) {
                continue;
            }
            if (state.type != kHttpDatagramCapsuleType) {
                continue;
            }
        }
        auto& owned = native ? *native : state.payload;
        auto bytes = std::span<const char>(owned);
        HttpDatagramSession datagrams(state.datagramConfig());
        std::size_t offset{};
        if (udp) {
            auto decoded = datagrams.receiveUdpDatagram(bytes, transport);
            if (!decoded) {
                state.abort();
                throw std::runtime_error("malformed CONNECT-UDP datagram");
            }
            if (!*decoded) {
                owned.clear();
                continue;
            }
            offset = bytes.size() - (*decoded)->payload.size();
        } else {
            auto decoded = datagrams.receiveDatagram(bytes, transport);
            if (!decoded) {
                state.abort();
                throw std::runtime_error("malformed HTTP Datagram");
            }
            if (!*decoded) {
                owned.clear();
                continue;
            }
            offset = bytes.size() - (**decoded).size();
        }
        auto result = std::move(owned);
        if (!native) {
            state.payload = std::pmr::string(state.resource);
        }
        co_return HttpDatagram(HttpCapsule(state, kHttpDatagramCapsuleType, std::move(result)), offset, transport);
    }
}
ScopedOperation<void> HttpDatagramStream::send(std::span<const std::byte> payload) & {
    return send(std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
}
ScopedOperation<void> HttpDatagramStream::send(std::string_view payload) & {
    return sendPayload(payload, false);
}
ScopedOperation<void> HttpDatagramStream::sendPayload(std::string_view payload, bool udp) {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
    auto& state = *stream_.state_;
    state.require(true);
    if (state.sendEnded) {
        throw std::logic_error("HTTP Datagram sending direction is closed");
    }
    HttpDatagramSession session(state.datagramConfig());
    auto transport = state.datagramPolicy == HttpDatagramSendPolicy::kCapsule ? HttpDatagramTransport::kCapsule : HttpDatagramTransport::kQuic;
    std::array<char, 24> prefix{};
    std::size_t size{};
    auto prepare = [&] {
        if (udp) {
            auto plan = session.prepareUdpDatagram(std::span(payload.data(), payload.size()), transport);
            if (!plan) {
                return false;
            }
            std::copy_n(plan->prefix.begin(), plan->prefixSize, prefix.begin());
            size = plan->prefixSize;
        } else {
            auto plan = session.prepareDatagram(std::span(payload.data(), payload.size()), transport);
            if (!plan) {
                return false;
            }
            std::copy_n(plan->prefix.begin(), plan->prefixSize, prefix.begin());
            size = plan->prefixSize;
        }
        return true;
    };
    if (!prepare()) {
        if (state.datagramPolicy != HttpDatagramSendPolicy::kAutomatic) {
            throw std::length_error("HTTP Datagram transport is unavailable or payload exceeds its bound");
        }
        transport = HttpDatagramTransport::kCapsule;
        if (!prepare()) {
            throw std::length_error("HTTP Datagram payload exceeds its bound");
        }
    }
    if (transport == HttpDatagramTransport::kCapsule) {
        if (payload.size() + (udp ? 1 : 0) > state.config.maxCapsuleLength) {
            throw std::length_error("HTTP Datagram exceeds capsule bound");
        }
        return stream_.writeFrame(std::string_view(prefix.data(), size), payload);
    }
    const auto send = +[](detail::CapsuleWriteInput input) -> Task<void> {
        auto& owner = input.pin.state();
        detail::RunningCapsuleOperation running(owner);
        co_await owner.sendDatagram(input.bytes);
    };
    std::pmr::string bytes(prefix.data(), size, state.resource);
    bytes.append(payload);
    return ::ruvia::make_scoped_operation(state.outputScope, send(detail::CapsuleWriteInput{detail::CapsuleStatePin(state), std::move(bytes)}));
}
}  // namespace ruvia
