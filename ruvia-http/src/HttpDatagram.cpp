#include "ruvia/http/HttpDatagram.h"

#include <algorithm>
#include <stdexcept>

#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3PeerStreams.h"
#include "ruvia/http/detail/field/HttpStructuredFields.h"

namespace ruvia {
std::expected<bool, Http3CodecError> parseHttpCapsuleProtocol(std::string_view value) noexcept {
    detail::HttpStructuredParser parser{value};
    parser.spaces();
    detail::HttpStructuredItem item;
    if (!parser.item(item) || !parser.parameters()) {
        return std::unexpected(Http3CodecError::kValueOutOfRange);
    }
    parser.ows();
    if (parser.at != value.size() || item.kind != detail::HttpStructuredItem::Kind::kBoolean) {
        return std::unexpected(Http3CodecError::kValueOutOfRange);
    }
    return item.boolean;
}

std::expected<Http3DatagramView, Http3CodecError> decodeHttp3Datagram(std::span<const char> input) noexcept {
    auto id = decodeHttp3VarInt(input);
    if (!id) {
        return std::unexpected(id.error());
    }
    if (id->value > kHttp3VarIntMax / 4) {
        return std::unexpected(Http3CodecError::kValueOutOfRange);
    }
    return Http3DatagramView{id->value * 4, input.subspan(id->encodedBytes)};
}
std::expected<std::size_t, Http3CodecError> encodeHttp3DatagramPrefix(std::span<char> output, std::uint64_t streamId) noexcept {
    if (!isHttp3RequestStreamId(streamId)) {
        return std::unexpected(Http3CodecError::kValueOutOfRange);
    }
    return encodeHttp3VarInt(output, streamId / 4);
}
std::expected<HttpUdpDatagramView, Http3CodecError> decodeHttpUdpDatagram(std::span<const char> input) noexcept {
    auto id = decodeHttp3VarInt(input);
    if (!id) {
        return std::unexpected(id.error());
    }
    return HttpUdpDatagramView{id->value, input.subspan(id->encodedBytes)};
}
std::expected<std::size_t, Http3CodecError> encodeHttpUdpDatagramPrefix(std::span<char> output, std::uint64_t contextId) noexcept {
    return encodeHttp3VarInt(output, contextId);
}
std::expected<std::size_t, Http3CodecError> encodeHttpCapsuleHeader(std::span<char> output, std::uint64_t type, std::uint64_t length) noexcept {
    return encodeHttp3FrameHeader(output, type, length);
}
HttpCapsuleDecoder::HttpCapsuleDecoder(HttpCapsuleConfig config)
    : config_(config) {
    if (config.maxCapsuleLength > kHttp3VarIntMax) {
        throw std::invalid_argument("capsule length exceeds varint range");
    }
}
HttpCapsuleStatus HttpCapsuleDecoder::feed(std::span<const char> input, bool fin, HttpCapsuleCallback callback, void* context) {
    return feedImpl(input, fin, callback, context, false).status;
}
HttpCapsuleFeedResult HttpCapsuleDecoder::feedOne(std::span<const char> input, bool fin, HttpCapsuleCallback callback, void* context) {
    return feedImpl(input, fin, callback, context, true);
}
HttpCapsuleFeedResult HttpCapsuleDecoder::feedImpl(std::span<const char> input, bool fin, HttpCapsuleCallback callback, void* context, bool one) {
    if (feeding_) {
        throw std::logic_error("recursive capsule feed");
    }
    if (status_ != HttpCapsuleStatus::kNeedMoreData) {
        return {.status = status_};
    }
    struct Guard {
        bool& value;
        Guard(bool& v)
            : value(v) {
            value = true;
        }
        ~Guard() {
            value = false;
        }
    } guard(feeding_);
    const auto initialSize = input.size();
    bool completed{};
    try {
        while (!input.empty()) {
            if (!payload_) {
                header_[headerSize_++] = input.front();
                input = input.subspan(1);
                if (typeSize_ == 0) {
                    auto type = decodeHttp3VarInt(std::span<const char>(header_).first(headerSize_));
                    if (!type) {
                        continue;
                    }
                    type_ = type->value;
                    typeSize_ = type->encodedBytes;
                }
                auto length = decodeHttp3VarInt(std::span<const char>(header_).subspan(typeSize_, headerSize_ - typeSize_));
                if (!length) {
                    continue;
                }
                if (length->value > config_.maxCapsuleLength) {
                    status_ = HttpCapsuleStatus::kLimit;
                    return {.status = status_, .consumedBytes = initialSize - input.size()};
                }
                remaining_ = length->value;
                payload_ = remaining_ != 0;
                headerSize_ = 0;
                typeSize_ = 0;
                if (!payload_) {
                    if (callback) {
                        callback(context, {type_, {}, true});
                    }
                    completed = true;
                    if (one) {
                        break;
                    }
                }
            } else {
                auto count = static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), remaining_));
                auto part = input.first(count);
                input = input.subspan(count);
                remaining_ -= count;
                payload_ = remaining_ != 0;
                if (callback) {
                    callback(context, {type_, part, !payload_});
                }
                if (!payload_) {
                    completed = true;
                    if (one) {
                        break;
                    }
                }
            }
        }
        if (fin && input.empty()) {
            status_ = payload_ || headerSize_ ? HttpCapsuleStatus::kTruncated : HttpCapsuleStatus::kEnd;
        }
        return {.status = status_, .consumedBytes = initialSize - input.size(), .capsuleComplete = completed};
    } catch (...) {
        status_ = HttpCapsuleStatus::kTruncated;
        throw;
    }
}
Http3DatagramReceiveStatus planHttp3DatagramReceive(Http3DatagramView datagram,
    Http3DatagramReceiveContext context) noexcept {
    if (!isHttp3RequestStreamId(datagram.streamId) || !context.localH3Datagram) {
        return Http3DatagramReceiveStatus::kConnectionError;
    }
    if (!context.streamExists || !context.receiveOpen) {
        return Http3DatagramReceiveStatus::kDrop;
    }
    return context.supportsDatagrams ? Http3DatagramReceiveStatus::kDeliver : Http3DatagramReceiveStatus::kStreamError;
}

HttpDatagramSession::HttpDatagramSession(HttpDatagramSessionConfig config)
    : config_(config) {
    if (config.http3StreamId && !isHttp3RequestStreamId(*config.http3StreamId)) {
        throw std::invalid_argument("HTTP datagram stream must be a client bidirectional stream");
    }
}
bool HttpDatagramSession::quicDatagramsEnabled() const noexcept {
    return config_.http3StreamId && config_.localH3Datagram && config_.peerH3Datagram &&
           config_.quicDatagram && config_.maxQuicPayloadBytes > 0;
}
std::expected<HttpDatagramWritePlan, HttpDatagramError> HttpDatagramSession::prepareDatagram(
    std::span<const char> payload, HttpDatagramTransport transport) const noexcept {
    if (!sendOpen_) {
        return std::unexpected(HttpDatagramError::kSendClosed);
    }
    HttpDatagramWritePlan plan{.payload = payload, .transport = transport};
    if (transport == HttpDatagramTransport::kQuic) {
        if (!quicDatagramsEnabled()) {
            return std::unexpected(HttpDatagramError::kNotNegotiated);
        }
        const auto prefix = encodeHttp3DatagramPrefix(plan.prefix, *config_.http3StreamId);
        plan.prefixSize = *prefix;
        if (plan.prefixSize > config_.maxQuicPayloadBytes || payload.size() > config_.maxQuicPayloadBytes - plan.prefixSize) {
            return std::unexpected(HttpDatagramError::kPayloadTooLarge);
        }
    } else {
        const auto prefix = encodeHttpCapsuleHeader(plan.prefix, kHttpDatagramCapsuleType, payload.size());
        if (!prefix) {
            return std::unexpected(HttpDatagramError::kPayloadTooLarge);
        }
        plan.prefixSize = *prefix;
    }
    return plan;
}
std::expected<std::optional<std::span<const char>>, HttpDatagramError> HttpDatagramSession::receiveDatagram(
    std::span<const char> input, HttpDatagramTransport transport) const noexcept {
    if (transport == HttpDatagramTransport::kQuic) {
        if (!config_.http3StreamId || !config_.localH3Datagram) {
            return std::unexpected(HttpDatagramError::kNotNegotiated);
        }
        const auto decoded = decodeHttp3Datagram(input);
        if (!decoded) {
            return std::unexpected(HttpDatagramError::kMalformed);
        }
        if (decoded->streamId != *config_.http3StreamId) {
            return std::unexpected(HttpDatagramError::kWrongStream);
        }
        input = decoded->payload;
    }
    if (!receiveOpen_) {
        return std::nullopt;
    }
    return input;
}

std::expected<HttpUdpDatagramWritePlan, HttpDatagramError> HttpDatagramSession::prepareUdpDatagram(
    std::span<const char> payload, HttpDatagramTransport transport) const noexcept {
    if (!sendOpen_) {
        return std::unexpected(HttpDatagramError::kSendClosed);
    }
    if (payload.size() > 65527) {
        return std::unexpected(HttpDatagramError::kPayloadTooLarge);
    }
    HttpUdpDatagramWritePlan plan{.payload = payload, .transport = transport};
    if (transport == HttpDatagramTransport::kQuic) {
        if (!quicDatagramsEnabled()) {
            return std::unexpected(HttpDatagramError::kNotNegotiated);
        }
        const auto prefix = encodeHttp3DatagramPrefix(plan.prefix, *config_.http3StreamId);
        plan.prefixSize = *prefix;
        plan.prefix[plan.prefixSize++] = 0;  // UDP Context ID.
        if (plan.prefixSize > config_.maxQuicPayloadBytes || payload.size() > config_.maxQuicPayloadBytes - plan.prefixSize) {
            return std::unexpected(HttpDatagramError::kPayloadTooLarge);
        }
    } else {
        const auto prefix = encodeHttpCapsuleHeader(plan.prefix, kHttpDatagramCapsuleType, payload.size() + 1);
        plan.prefixSize = *prefix;
        plan.prefix[plan.prefixSize++] = 0;
    }
    return plan;
}
std::expected<std::optional<HttpUdpDatagramView>, HttpDatagramError> HttpDatagramSession::receiveUdpDatagram(
    std::span<const char> input, HttpDatagramTransport transport) const noexcept {
    if (transport == HttpDatagramTransport::kQuic) {
        if (!config_.http3StreamId || !config_.localH3Datagram) {
            return std::unexpected(HttpDatagramError::kNotNegotiated);
        }
        const auto datagram = decodeHttp3Datagram(input);
        if (!datagram) {
            return std::unexpected(HttpDatagramError::kMalformed);
        }
        if (datagram->streamId != *config_.http3StreamId) {
            return std::unexpected(HttpDatagramError::kWrongStream);
        }
        input = datagram->payload;
    }
    if (!receiveOpen_) {
        return std::nullopt;
    }
    const auto udp = decodeHttpUdpDatagram(input);
    if (!udp) {
        return std::unexpected(HttpDatagramError::kMalformed);
    }
    if (udp->contextId != 0) {
        return std::nullopt;
    }
    if (udp->payload.size() > 65527) {
        return std::unexpected(HttpDatagramError::kPayloadTooLarge);
    }
    return *udp;
}
}  // namespace ruvia
