#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3LocalCriticalStreams.h"
#include "ruvia/web/detail/http3/Http3CriticalStreamDriver.h"

#include "test_harness.h"

namespace {

using Driver = ruvia::detail::Http3CriticalStreamDriver;
using Quic = ruvia::detail::Http3QuicStreamSet;

struct FakeQuic final {
    std::array<std::string, 3> accepted{};
    std::array<const char*, 3> retryAddress{};
    std::array<std::size_t, 3> retrySize{};
    std::array<int, 3> writes{};
    int opens{};
    bool noCredit{true};
    bool failDecoder{false};

    [[nodiscard]] Quic::OpenStream open(Driver::Kind kind) {
        ++opens;
        if (kind == Driver::Kind::kQpackEncoder && noCredit) {
            noCredit = false;
            return {.error = Quic::Error::kStreamLimitRetry};
        }
        return {.id = 2 + 4 * static_cast<std::uint64_t>(kind)};
    }

    [[nodiscard]] Quic::StreamWrite write(std::uint64_t id, std::span<const char> bytes) {
        const auto index = static_cast<std::size_t>((id - 2) / 4);
        if (index >= accepted.size()) {
            return {.status = Quic::StreamWrite::Status::kFatal};
        }
        ++writes[index];
        if (index == 0 && writes[index] == 1) {
            retryAddress[index] = bytes.data();
            retrySize[index] = bytes.size();
            return {.status = Quic::StreamWrite::Status::kWouldBlock};
        }
        if (index == 0 && writes[index] == 2 &&
            (bytes.data() != retryAddress[index] || bytes.size() != retrySize[index])) {
            return {.status = Quic::StreamWrite::Status::kRetryMismatch};
        }
        if (index == 2 && failDecoder) {
            return {.status = Quic::StreamWrite::Status::kClosed};
        }
        const auto count = index == 0 && writes[index] == 2 ? std::size_t{1} : bytes.size();
        accepted[index].append(bytes.data(), count);
        return {.status = Quic::StreamWrite::Status::kAccepted, .bytes = count};
    }
};

}  // namespace

RUVIA_TEST(http3CriticalStreamDriverRetriesCreditAndWantWithoutConcludingStreams) {
    const auto prefixes = ruvia::Http3LocalCriticalStreams::create();
    RUVIA_CHECK(prefixes.has_value());
    if (!prefixes) {
        return;
    }
    Driver driver(*prefixes);
    FakeQuic quic;
    RUVIA_CHECK(!driver.complete());
    bool wroteBeforeCredit = false;
    RUVIA_CHECK(driver.drive(
                    [](Driver::Kind) {
                        return Quic::OpenStream{.error = Quic::Error::kStreamLimitRetry};
                    },
                    [&](std::uint64_t, std::span<const char>) {
                        wroteBeforeCredit = true;
                        return Quic::StreamWrite{.status = Quic::StreamWrite::Status::kFatal};
                    }) == Driver::Result::kBlocked);
    RUVIA_CHECK(!wroteBeforeCredit);
    auto open = [&](Driver::Kind kind) { return quic.open(kind); };
    auto write = [&](std::uint64_t id, std::span<const char> bytes) {
        return quic.write(id, bytes);
    };
    for (int i = 0; i < 5; ++i) {
        if (driver.drive(open, write) == Driver::Result::kReady) {
            break;
        }
    }
    RUVIA_CHECK(driver.drive(open, write) == Driver::Result::kReady);
    RUVIA_CHECK(driver.complete());
    RUVIA_CHECK_EQ(quic.accepted[0], std::string(prefixes->controlPrefix().data(),
                                         prefixes->controlPrefix().size()));
    RUVIA_CHECK_EQ(quic.accepted[1], std::string(prefixes->qpackEncoderPrefix().data(),
                                         prefixes->qpackEncoderPrefix().size()));
    RUVIA_CHECK_EQ(quic.accepted[2], std::string(prefixes->qpackDecoderPrefix().data(),
                                         prefixes->qpackDecoderPrefix().size()));
    RUVIA_CHECK(driver.queueGoaway(12));
    RUVIA_CHECK(!driver.complete());
    RUVIA_CHECK(!driver.queueGoaway(16));
    RUVIA_CHECK(driver.drive(open, write) == Driver::Result::kReady);
    RUVIA_CHECK(driver.complete());
    const auto controlBytes = std::span<const char>(quic.accepted[0].data(),
        quic.accepted[0].size());
    const auto goaway = controlBytes.subspan(prefixes->controlPrefix().size());
    const auto decodedGoaway = ruvia::decodeHttp3Frame(goaway);
    RUVIA_CHECK(decodedGoaway && decodedGoaway->type ==
                                     static_cast<std::uint64_t>(ruvia::Http3FrameType::kGoaway));
    if (decodedGoaway) {
        const auto identifier = ruvia::decodeHttp3VarInt(decodedGoaway->payload);
        RUVIA_CHECK(identifier && identifier->value == 12);
        RUVIA_CHECK_EQ(decodedGoaway->encodedBytes, goaway.size());
    }
    RUVIA_CHECK(driver.streamId(Driver::Kind::kControl) == 2);
    RUVIA_CHECK(driver.streamId(Driver::Kind::kQpackEncoder) == 6);
    RUVIA_CHECK(driver.streamId(Driver::Kind::kQpackDecoder) == 10);
    RUVIA_CHECK_EQ(quic.opens, 4);
}

RUVIA_TEST(http3CriticalStreamDriverLatchesFailedCriticalStream) {
    const auto prefixes = ruvia::Http3LocalCriticalStreams::create();
    RUVIA_CHECK(prefixes.has_value());
    if (!prefixes) {
        return;
    }
    Driver driver(*prefixes);
    FakeQuic quic;
    quic.failDecoder = true;
    auto open = [&](Driver::Kind kind) { return quic.open(kind); };
    auto write = [&](std::uint64_t id, std::span<const char> bytes) {
        return quic.write(id, bytes);
    };
    RUVIA_CHECK(driver.drive(open, write) == Driver::Result::kFatal);
    const auto previousOpens = quic.opens;
    RUVIA_CHECK(driver.drive(open, write) == Driver::Result::kFatal);
    RUVIA_CHECK_EQ(quic.opens, previousOpens);
}
