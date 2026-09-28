#include "ruvia/web/detail/http3/Http3QuicDatagramBridge.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <memory>
#include <stdexcept>

#include <openssl/err.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace ruvia::detail {
namespace {

std::size_t addressSize(const Http3QuicDatagramAddress& address) noexcept {
    switch (address.family) {
        case Http3QuicDatagramAddress::Family::kIPv4:
            return 4;
        case Http3QuicDatagramAddress::Family::kIPv6:
            return 16;
    }
    return 0;
}

bool validAddressShape(const Http3QuicDatagramAddress& address) noexcept {
    const std::size_t count = addressSize(address);
    if (address.port == 0 || address.scopeId != 0 || count == 0) {
        return false;
    }
    if (address.family == Http3QuicDatagramAddress::Family::kIPv6 &&
        address.address[0] == 0xfe && (address.address[1] & 0xc0) == 0x80) {
        return false;  // BIO_ADDR_rawmake cannot supply the scope required by IPv6 link-local.
    }
    return true;
}

bool wildcard(const Http3QuicDatagramAddress& address) noexcept {
    const std::size_t count = addressSize(address);
    return count != 0 && std::all_of(address.address.begin(), address.address.begin() + count,
                             [](std::uint8_t byte) { return byte == 0; });
}

bool concrete(const Http3QuicDatagramAddress& address) noexcept {
    return validAddressShape(address) && !wildcard(address);
}

bool validBindAddress(const Http3QuicDatagramAddress& address) noexcept {
    return validAddressShape(address);
}

bool sameAddress(const Http3QuicDatagramAddress& left,
    const Http3QuicDatagramAddress& right) noexcept {
    const std::size_t count = addressSize(left);
    return count != 0 && left.family == right.family && left.port == right.port &&
           std::equal(left.address.begin(), left.address.begin() + count, right.address.begin());
}

bool validLocalDestination(const Http3QuicDatagramAddress& destination,
    const Http3QuicDatagramAddress& bound) noexcept {
    return concrete(destination) && destination.family == bound.family &&
           destination.port == bound.port &&
           (wildcard(bound) || sameAddress(destination, bound));
}

std::uint16_t toNetworkPort(std::uint16_t port) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(port);
    }
    return port;
}

std::uint16_t fromNetworkPort(std::uint16_t port) noexcept {
    return toNetworkPort(port);
}

// OpenSSL cannot mark an empty error queue. In that case, errors produced by this
// call are the entire queue and can be cleared on non-fatal outcomes. With a prior
// error, mark/pop removes only errors added by this operation.
class BioErrorScope final {
public:
    BioErrorScope() noexcept
        : lastErrorBefore_(ERR_peek_last_error()),
          initiallyEmpty_(lastErrorBefore_ == 0) {
        marked_ = !initiallyEmpty_ && ERR_set_mark() == 1;
    }
    [[nodiscard]] unsigned long newLastError() const noexcept {
        const unsigned long current = ERR_peek_last_error();
        return current != lastErrorBefore_ ? current : 0;
    }
    void discard() noexcept {
        if (marked_) {
            (void)ERR_pop_to_mark();
            marked_ = false;
        } else if (initiallyEmpty_) {
            ERR_clear_error();
        }
    }
    void preserveFatal() noexcept {
        if (marked_) {
            (void)ERR_clear_last_mark();
            marked_ = false;
        }
    }
    ~BioErrorScope() {
        discard();
    }

private:
    unsigned long lastErrorBefore_{};
    bool initiallyEmpty_{};
    bool marked_{};
};

bool makeBioAddress(const Http3QuicDatagramAddress& source, BIO_ADDR* target) noexcept {
    if (!concrete(source) || target == nullptr) {
        return false;
    }
    const bool ipv4 = source.family == Http3QuicDatagramAddress::Family::kIPv4;
    BIO_ADDR_clear(target);
    return BIO_ADDR_rawmake(target, ipv4 ? AF_INET : AF_INET6, source.address.data(),
               ipv4 ? 4 : 16, toNetworkPort(source.port)) == 1;
}

bool readBioAddress(const BIO_ADDR* source, Http3QuicDatagramAddress& target) noexcept {
    if (source == nullptr) {
        return false;
    }
    const int family = BIO_ADDR_family(source);
    const std::size_t expected = family == AF_INET ? 4 : family == AF_INET6 ? 16
                                                                            : 0;
    if (expected == 0) {
        return false;
    }
    Http3QuicDatagramAddress parsed;
    parsed.family = family == AF_INET ? Http3QuicDatagramAddress::Family::kIPv4
                                      : Http3QuicDatagramAddress::Family::kIPv6;
    std::size_t length = parsed.address.size();
    if (BIO_ADDR_rawaddress(source, parsed.address.data(), &length) != 1 || length != expected) {
        return false;
    }
    parsed.port = fromNetworkPort(static_cast<std::uint16_t>(BIO_ADDR_rawport(source)));
    if (!concrete(parsed)) {
        return false;
    }
    target = parsed;
    return true;
}

bool bioAddressAbsent(const BIO_ADDR* address) noexcept {
    return address == nullptr || BIO_ADDR_family(address) == AF_UNSPEC;
}

}  // namespace

bool makeHttp3QuicBioAddress(const Http3QuicDatagramAddress& source, BIO_ADDR* target) noexcept {
    return makeBioAddress(source, target);
}

Http3QuicDatagramBridge::Http3QuicDatagramBridge(Http3QuicDatagramAddress boundAddress)
    : boundAddress_(boundAddress) {
    if (!validBindAddress(boundAddress_)) {
        throw std::invalid_argument(
            "QUIC datagram bridge requires an IPv4/IPv6 bind address with a nonzero port and no unsupported scope");
    }

    using BioAddressOwner = std::unique_ptr<BIO_ADDR, decltype(&BIO_ADDR_free)>;
    using BioOwner = std::unique_ptr<BIO, decltype(&BIO_free)>;
    BioAddressOwner injectSource(BIO_ADDR_new(), BIO_ADDR_free);
    BioAddressOwner injectDestination(BIO_ADDR_new(), BIO_ADDR_free);
    BioAddressOwner outboundLocal(BIO_ADDR_new(), BIO_ADDR_free);
    BioAddressOwner outboundPeer(BIO_ADDR_new(), BIO_ADDR_free);
    if (!injectSource || !injectDestination || !outboundLocal || !outboundPeer) {
        throw std::runtime_error("failed to allocate QUIC datagram BIO addresses");
    }

    BIO* sslBioRaw = nullptr;
    BIO* appBioRaw = nullptr;
    const int pairResult = BIO_new_bio_dgram_pair(
        &sslBioRaw, kBioQueueSize, &appBioRaw, kBioQueueSize);
    BioOwner sslBio(sslBioRaw, BIO_free);
    BioOwner appBio(appBioRaw, BIO_free);
    if (pairResult != 1 || !sslBio || !appBio) {
        throw std::runtime_error("failed to create QUIC datagram BIO pair");
    }

    // Both endpoints send and consume per-datagram source/destination metadata.
    constexpr std::uint32_t addressCaps = BIO_DGRAM_CAP_HANDLES_SRC_ADDR |
                                          BIO_DGRAM_CAP_HANDLES_DST_ADDR |
                                          BIO_DGRAM_CAP_PROVIDES_SRC_ADDR |
                                          BIO_DGRAM_CAP_PROVIDES_DST_ADDR;
    if (BIO_dgram_set_caps(appBio.get(), addressCaps) != 1 ||
        BIO_dgram_set_caps(sslBio.get(), addressCaps) != 1 ||
        BIO_dgram_set_local_addr_enable(appBio.get(), 1) != 1 ||
        BIO_dgram_set_local_addr_enable(sslBio.get(), 1) != 1) {
        throw std::runtime_error("failed to configure QUIC datagram BIO pair");
    }

    sslBio_ = sslBio.release();
    appBio_ = appBio.release();
    injectSource_ = injectSource.release();
    injectDestination_ = injectDestination.release();
    outboundLocal_ = outboundLocal.release();
    outboundPeer_ = outboundPeer.release();
}

Http3QuicDatagramBridge::~Http3QuicDatagramBridge() {
    BIO_free(sslBio_);
    BIO_free(appBio_);
    BIO_ADDR_free(injectSource_);
    BIO_ADDR_free(injectDestination_);
    BIO_ADDR_free(outboundLocal_);
    BIO_ADDR_free(outboundPeer_);
}

BIO* Http3QuicDatagramBridge::releaseSslBio() noexcept {
    BIO* const bio = sslBio_;
    sslBio_ = nullptr;
    return bio;
}

Http3QuicDatagramBridge::InjectResult Http3QuicDatagramBridge::inject(
    std::span<const std::byte> datagram, const Http3QuicDatagramAddress& peer) noexcept {
    if (wildcard(boundAddress_)) {
        return InjectResult::kFatal;
    }
    return inject(datagram, peer, boundAddress_);
}

Http3QuicDatagramBridge::InjectResult Http3QuicDatagramBridge::inject(
    std::span<const std::byte> datagram, const Http3QuicDatagramAddress& peer,
    Http3QuicDatagramAddress localDestination) noexcept {
    if (appBio_ == nullptr || datagram.size() > kMaxDatagramSize || !concrete(peer) ||
        !validLocalDestination(localDestination, boundAddress_) ||
        injectionGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
        return InjectResult::kFatal;
    }
    if (!makeHttp3QuicBioAddress(peer, injectSource_) ||
        !makeHttp3QuicBioAddress(localDestination, injectDestination_)) {
        return InjectResult::kFatal;
    }
    BioErrorScope errors;
    BIO_MSG message{};
    message.data = const_cast<std::byte*>(datagram.data());
    message.data_len = datagram.size();
    message.local = injectSource_;
    message.peer = injectDestination_;
    std::size_t processed = 0;
    const int result = BIO_sendmmsg(appBio_, &message, sizeof(message), 1, 0, &processed);
    if (result == 1 && processed == 1) {
        errors.discard();
        lastInjectedPeer_ = peer;
        ++injectionGeneration_;
        return InjectResult::kAccepted;
    }
    const unsigned long error = errors.newLastError();
    if (error != 0 && BIO_err_is_non_fatal(error)) {
        errors.discard();
        return InjectResult::kFull;
    }
    errors.preserveFatal();
    return InjectResult::kFatal;
}

Http3QuicDatagramBridge::OutboundResult Http3QuicDatagramBridge::takeOutbound(
    Http3QuicOutboundDatagram& outbound) noexcept {
    if (appBio_ == nullptr) {
        return OutboundResult::kFatal;
    }
    if (outboundInFlight_) {
        return OutboundResult::kBusy;
    }
    BIO_ADDR_clear(outboundLocal_);
    BIO_ADDR_clear(outboundPeer_);
    BioErrorScope errors;
    BIO_MSG message{};
    message.data = buffer_.data();
    message.data_len = buffer_.size();
    message.local = outboundLocal_;
    message.peer = outboundPeer_;
    std::size_t processed = 0;
    const int result = BIO_recvmmsg(appBio_, &message, sizeof(message), 1, 0, &processed);
    if (result != 1 || processed != 1) {
        const unsigned long error = errors.newLastError();
        if (error != 0 && BIO_err_is_non_fatal(error)) {
            errors.discard();
            return OutboundResult::kEmpty;
        }
        errors.preserveFatal();
        return OutboundResult::kFatal;
    }
    Http3QuicDatagramAddress destination;
    Http3QuicDatagramAddress source;
    const bool hasSource = !bioAddressAbsent(message.peer);
    if (!readBioAddress(message.local, destination) ||
        (hasSource && !readBioAddress(message.peer, source)) || message.data_len > buffer_.size()) {
        errors.preserveFatal();
        return OutboundResult::kFatal;
    }
    errors.discard();
    outboundSize_ = message.data_len;
    outboundDestination_ = destination;
    outboundSource_ = source;
    outboundHasSource_ = hasSource;
    outboundInFlight_ = true;
    outbound = {std::span<const std::byte>(buffer_.data(), outboundSize_), outboundDestination_,
        outboundHasSource_, outboundSource_};
    return OutboundResult::kReady;
}

void Http3QuicDatagramBridge::completeOutbound() noexcept {
    outboundInFlight_ = false;
    outboundSize_ = 0;
    outboundHasSource_ = false;
}

bool Http3QuicDatagramBridge::outboundQuiescent() const noexcept {
    return !outboundInFlight_ && appBio_ != nullptr && BIO_ctrl_pending(appBio_) == 0;
}

}  // namespace ruvia::detail
