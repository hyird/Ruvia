#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3LocalCriticalStreams.h"

#include "http3/Http3CriticalStreamDriver.h"
#include "test_harness.h"

namespace {

using Driver = ruvia::detail::Http3CriticalStreamDriver;
using StreamOpen = ruvia::quic_stream_open_result;
using StreamWrite = ruvia::quic_stream_write_result;
using OperationStatus = ruvia::quic_operation_status;

struct FakeQuic final {
    std::array<std::string, 3> accepted{};
    std::array<const char*, 3> retryAddress{};
    std::array<std::size_t, 3> retrySize{};
    std::array<int, 3> writes{};
    int opens{};
    bool noCredit{true};
    bool failDecoder{false};

    [[nodiscard]] StreamOpen open(Driver::Kind kind) {
        ++opens;
        if (kind == Driver::Kind::qpack_encoder && noCredit) {
            noCredit = false;
            return {.status = OperationStatus::would_block};
        }
        return {.status = OperationStatus::accepted,
            .stream_id = 2 + 4 * static_cast<std::uint64_t>(kind)};
    }

    [[nodiscard]] StreamWrite write(std::uint64_t id, std::span<const char> bytes) {
        const auto index = static_cast<std::size_t>((id - 2) / 4);
        if (index >= accepted.size()) {
            return {.status = OperationStatus::closing};
        }
        ++writes[index];
        if (index == 0 && writes[index] == 1) {
            retryAddress[index] = bytes.data();
            retrySize[index] = bytes.size();
            return {.status = OperationStatus::would_block};
        }
        if (index == 0 && writes[index] == 2 &&
            (bytes.data() != retryAddress[index] || bytes.size() != retrySize[index])) {
            return {.status = OperationStatus::closing};
        }
        if (index == 2 && failDecoder) {
            return {.status = OperationStatus::closing};
        }
        const auto count = index == 0 && writes[index] == 2 ? std::size_t{1} : bytes.size();
        accepted[index].append(bytes.data(), count);
        return {.status = OperationStatus::accepted, .accepted = count};
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
                        return StreamOpen{.status = OperationStatus::would_block};
                    },
                    [&](std::uint64_t, std::span<const char>) {
                        wroteBeforeCredit = true;
                        return StreamWrite{.status = OperationStatus::closing};
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
    RUVIA_CHECK(driver.streamId(Driver::Kind::control) == 2);
    RUVIA_CHECK(driver.streamId(Driver::Kind::qpack_encoder) == 6);
    RUVIA_CHECK(driver.streamId(Driver::Kind::qpack_decoder) == 10);
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
