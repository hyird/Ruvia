#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory_resource>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/http2/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/Http2Hpack.h"

namespace {

using Clock = std::chrono::steady_clock;

#if defined(_MSC_VER)
#define RUVIA_BENCHMARK_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define RUVIA_BENCHMARK_NOINLINE __attribute__((noinline))
#else
#define RUVIA_BENCHMARK_NOINLINE
#endif

struct Options final {
    std::chrono::milliseconds warmup{100};
    std::chrono::milliseconds duration{500};
    std::string outputPath;
};

struct Result final {
    std::string_view name;
    std::uint64_t operations;
    std::uint64_t elapsedNanoseconds;
};

[[nodiscard]] std::uint64_t parseMilliseconds(
    std::string_view argument,
    std::string_view option) {
    std::uint64_t value = 0;
    const auto* begin = argument.data();
    const auto* end = begin + argument.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value == 0 ||
        value > 60'000) {
        throw std::invalid_argument(
            std::string(option) + " must be an integer in [1, 60000]");
    }
    return value;
}

[[nodiscard]] Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        const auto requireValue = [&](std::string_view option) {
            if (++i == argc) {
                throw std::invalid_argument(
                    std::string(option) + " requires a value");
            }
            return std::string_view(argv[i]);
        };
        if (argument == "--warmup-ms") {
            options.warmup = std::chrono::milliseconds(
                parseMilliseconds(requireValue(argument), argument));
        } else if (argument == "--duration-ms") {
            options.duration = std::chrono::milliseconds(
                parseMilliseconds(requireValue(argument), argument));
        } else if (argument == "--output") {
            options.outputPath = requireValue(argument);
            if (options.outputPath.empty()) {
                throw std::invalid_argument("--output path must not be empty");
            }
        } else {
            throw std::invalid_argument(
                "unknown benchmark argument: " + std::string(argument));
        }
    }
    return options;
}

void retain(std::uint64_t value) noexcept {
    // The benchmarked functions live in ruvia-http, but retaining their observed
    // results also prevents link-time optimization from deleting a future inline
    // benchmark body.
    static volatile std::uint64_t sink = 0;
    sink = sink ^ value;
}

template <typename Batch>
[[nodiscard]] Result runBenchmark(
    std::string_view name,
    std::size_t operationsPerBatch,
    std::chrono::milliseconds warmup,
    std::chrono::milliseconds duration,
    Batch&& batch) {
    const auto exerciseUntil = [&](Clock::time_point deadline) {
        std::uint64_t checksum = 0;
        std::uint64_t operations = 0;
        do {
            checksum ^= batch();
            operations += operationsPerBatch;
        } while (Clock::now() < deadline);
        retain(checksum);
        return operations;
    };

    (void)exerciseUntil(Clock::now() + warmup);
    const auto start = Clock::now();
    const auto operations = exerciseUntil(start + duration);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - start);
    return Result{
        .name = name,
        .operations = operations,
        .elapsedNanoseconds = static_cast<std::uint64_t>(elapsed.count())};
}

void beginHttp2ServerBenchmarkConnection(
    ruvia::detail::Http2Connection& connection) {
    connection.beginConnection();

    std::array<char,
        ruvia::detail::kHttp2ClientPreface.size() +
            ruvia::detail::kHttp2FrameHeaderBytes> handshake{};
    auto* cursor = std::copy(
        ruvia::detail::kHttp2ClientPreface.begin(),
        ruvia::detail::kHttp2ClientPreface.end(),
        handshake.data());
    ruvia::detail::http2EncodeFrameHeader(
        cursor,
        0,
        ruvia::detail::Http2FrameType::kSettings,
        0,
        0);
    if (connection.feed(
            std::string_view(handshake.data(), handshake.size())) !=
        ruvia::detail::Http2FeedResult::kAccepted) {
        throw std::runtime_error("HTTP/2 benchmark handshake failed");
    }
    if (connection.consumeOutput(connection.pendingOutput().size()) !=
        ruvia::detail::Http2OutputConsumeStatus::kDrained) {
        throw std::runtime_error(
            "HTTP/2 benchmark handshake output did not drain");
    }
}

class Http1ServerHeadParseBatch final {
public:
    static constexpr std::size_t kOperations = 128;

    [[nodiscard]] std::uint64_t operator()() {
        std::uint64_t checksum = 0;
        for (std::size_t i = 0; i < kOperations; ++i) {
            parser_.parseHead(request_, state_);
            const auto* ready = state_.headReady();
            if (ready == nullptr) {
                throw std::runtime_error(
                    "HTTP/1 benchmark request head did not parse");
            }
            checksum += ready->headerBytes();
            checksum += state_.request.headers().size();
        }
        return checksum;
    }

private:
    static constexpr std::string_view request_ =
        "GET /items/42?expand=owner HTTP/1.1\r\n"
        "Host: benchmark.example\r\n"
        "Accept: application/json\r\n"
        "Accept-Encoding: gzip, br\r\n"
        "User-Agent: ruvia-benchmark\r\n"
        "\r\n";
    ruvia::detail::Http1ServerRequestParser parser_;
    ruvia::detail::Http1ServerRequestParseState state_;
};

class Http2FrameCodecBatch final {
public:
    static constexpr std::size_t kOperations = 1024;

    [[nodiscard]] std::uint64_t operator()() noexcept {
        std::uint64_t checksum = 0;
        for (std::size_t i = 0; i < kOperations; ++i) {
            const auto streamId = static_cast<std::uint32_t>(
                ((sequence_++ & 0x3fffU) * 2U) + 1U);
            const auto parsed = roundTrip(bytes_, streamId);
            checksum += parsed.length;
            checksum += parsed.streamId;
        }
        return checksum;
    }

private:
    RUVIA_BENCHMARK_NOINLINE static ruvia::detail::Http2FrameHeader roundTrip(
        std::array<char, ruvia::detail::kHttp2FrameHeaderBytes>& bytes,
        std::uint32_t streamId) noexcept {
        ruvia::detail::http2EncodeFrameHeader(
            bytes.data(),
            16'384,
            ruvia::detail::Http2FrameType::kData,
            0,
            streamId);
        return ruvia::detail::http2ParseFrameHeader(
            std::string_view(bytes.data(), bytes.size()));
    }

    std::array<char, ruvia::detail::kHttp2FrameHeaderBytes> bytes_{};
    std::uint32_t sequence_{0};
};

class Http2PingFeedBatch final {
public:
    static constexpr std::size_t kOperations = 128;

    Http2PingFeedBatch()
        : connection_(&resource_, ruvia::detail::Http2Role::kServer) {
        beginHttp2ServerBenchmarkConnection(connection_);

        ruvia::detail::http2EncodeFrameHeader(
            ping_.data(),
            8,
            ruvia::detail::Http2FrameType::kPing,
            0,
            0);
    }

    [[nodiscard]] std::uint64_t operator()() {
        std::uint64_t checksum = 0;
        for (std::size_t i = 0; i < kOperations; ++i) {
            ping_[ruvia::detail::kHttp2FrameHeaderBytes] =
                static_cast<char>(sequence_++);
            const auto status = connection_.feed(
                std::string_view(ping_.data(), ping_.size()));
            if (status != ruvia::detail::Http2FeedResult::kAccepted) {
                throw std::runtime_error(
                    "HTTP/2 PING fast path was not accepted");
            }
            const auto output = connection_.pendingOutput();
            checksum += output.size();
            if (connection_.consumeOutput(output.size()) !=
                ruvia::detail::Http2OutputConsumeStatus::kDrained) {
                throw std::runtime_error(
                    "HTTP/2 benchmark PING output did not drain");
            }
        }
        return checksum;
    }

private:
    std::pmr::unsynchronized_pool_resource resource_;
    ruvia::detail::Http2Connection connection_;
    std::array<char, ruvia::detail::kHttp2FrameHeaderBytes + 8> ping_{};
    std::uint32_t sequence_{0};
};

class Http2RequestHeadFeedBatch final {
public:
    static constexpr std::size_t kOperations = 64;

    Http2RequestHeadFeedBatch()
        : connection_(&resource_, ruvia::detail::Http2Role::kServer),
          frame_(&resource_) {
        beginHttp2ServerBenchmarkConnection(connection_);

        std::pmr::string block(&resource_);
        ruvia::detail::HpackEncoder::encodeHeader(
            block, ":method", "GET");
        ruvia::detail::HpackEncoder::encodeHeader(
            block, ":scheme", "https");
        ruvia::detail::HpackEncoder::encodeHeader(
            block, ":path", "/items/42?expand=owner");
        ruvia::detail::HpackEncoder::encodeHeader(
            block, ":authority", "benchmark.example");
        ruvia::detail::HpackEncoder::encodeHeader(
            block, "accept", "application/json");
        ruvia::detail::HpackEncoder::encodeHeader(
            block, "accept-encoding", "gzip, br");
        ruvia::detail::HpackEncoder::encodeHeader(
            block, "user-agent", "ruvia-benchmark");

        frame_.resize(ruvia::detail::kHttp2FrameHeaderBytes);
        frame_.append(block.data(), block.size());
    }

    [[nodiscard]] std::uint64_t operator()() {
        std::uint64_t checksum = 0;
        for (std::size_t i = 0; i < kOperations; ++i) {
            const auto streamId = nextStreamId_;
            nextStreamId_ += 2;
            ruvia::detail::http2EncodeFrameHeader(
                frame_.data(),
                static_cast<std::uint32_t>(
                    frame_.size() - ruvia::detail::kHttp2FrameHeaderBytes),
                ruvia::detail::Http2FrameType::kHeaders,
                ruvia::detail::kHttp2FlagEndHeaders |
                    ruvia::detail::kHttp2FlagEndStream,
                streamId);
            if (connection_.feed(
                    std::string_view(frame_.data(), frame_.size())) !=
                ruvia::detail::Http2FeedResult::kAccepted) {
                throw std::runtime_error(
                    "HTTP/2 request-head benchmark feed failed");
            }

            std::size_t eventCount = 0;
            while (connection_.nextEvent().has_value()) {
                ++eventCount;
            }
            if (eventCount != 2 ||
                connection_.submitReset(
                    streamId,
                    ruvia::detail::Http2ErrorCode::kCancel) !=
                    ruvia::detail::Http2SubmitStatus::kAccepted) {
                throw std::runtime_error(
                    "HTTP/2 request-head benchmark cleanup failed");
            }
            const auto output = connection_.pendingOutput();
            checksum += streamId + eventCount + output.size();
            if (connection_.consumeOutput(output.size()) !=
                ruvia::detail::Http2OutputConsumeStatus::kDrained) {
                throw std::runtime_error(
                    "HTTP/2 request-head benchmark output did not drain");
            }
        }
        return checksum;
    }

private:
    std::pmr::unsynchronized_pool_resource resource_;
    ruvia::detail::Http2Connection connection_;
    std::pmr::string frame_;
    std::uint32_t nextStreamId_{1};
};

[[nodiscard]] std::string renderJson(
    const Options& options,
    const std::vector<Result>& results) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(3);
    output << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"environment\": {\"compiler_id\": \""
           << RUVIA_BENCHMARK_COMPILER_ID
           << "\", \"compiler_version\": \""
           << RUVIA_BENCHMARK_COMPILER_VERSION
           << "\", \"configuration\": \""
           << RUVIA_BENCHMARK_CONFIGURATION
           << "\", \"processor\": \""
           << RUVIA_BENCHMARK_PROCESSOR << "\"},\n"
           << "  \"warmup_ms\": " << options.warmup.count() << ",\n"
           << "  \"duration_ms\": " << options.duration.count() << ",\n"
           << "  \"benchmarks\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& result = results[i];
        const auto nanosecondsPerOperation =
            static_cast<double>(result.elapsedNanoseconds) /
            static_cast<double>(result.operations);
        const auto operationsPerSecond = 1'000'000'000.0 /
            nanosecondsPerOperation;
        output << "    {\"name\": \"" << result.name
               << "\", \"operations\": " << result.operations
               << ", \"elapsed_ns\": " << result.elapsedNanoseconds
               << ", \"ns_per_operation\": " << nanosecondsPerOperation
               << ", \"operations_per_second\": " << operationsPerSecond
               << "}";
        output << (i + 1 == results.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    return output.str();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parseOptions(argc, argv);
        Http1ServerHeadParseBatch http1;
        Http2FrameCodecBatch frameCodec;
        Http2PingFeedBatch pingFeed;
        Http2RequestHeadFeedBatch requestHeadFeed;

        std::vector<Result> results;
        results.reserve(4);
        results.push_back(runBenchmark(
            "http1_server_request_head_parse",
            Http1ServerHeadParseBatch::kOperations,
            options.warmup,
            options.duration,
            http1));
        results.push_back(runBenchmark(
            "http2_frame_header_round_trip",
            Http2FrameCodecBatch::kOperations,
            options.warmup,
            options.duration,
            frameCodec));
        results.push_back(runBenchmark(
            "http2_ping_feed",
            Http2PingFeedBatch::kOperations,
            options.warmup,
            options.duration,
            pingFeed));
        results.push_back(runBenchmark(
            "http2_request_head_feed",
            Http2RequestHeadFeedBatch::kOperations,
            options.warmup,
            options.duration,
            requestHeadFeed));

        const auto json = renderJson(options, results);
        std::fwrite(json.data(), 1, json.size(), stdout);
        if (!options.outputPath.empty()) {
            std::ofstream output(options.outputPath, std::ios::binary);
            output.exceptions(std::ios::failbit | std::ios::badbit);
            output << json;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "benchmark failed: %s\n", error.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
