#include <exception>
#include <optional>
#include <string>

#include "ruvia/core/EventLoopAttachment.h"

#include "http/HttpTunnelSession.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
struct TunnelTransportState {
    std::string output;
    unsigned reads{};
    unsigned finishes{};
    bool aborted{};
};
struct TunnelTransport {
    TunnelTransportState& state;
    ruvia::Task<ruvia::detail::HttpStreamReadResult> readMore(std::pmr::string& bytes) {
        if (state.aborted) {
            co_return ruvia::detail::HttpStreamReadResult::makeFailure(std::make_error_code(std::errc::operation_canceled));
        }
        if (++state.reads == 1) {
            bytes.append(512, 'r');
            co_return ruvia::detail::HttpStreamReadResult::makeData();
        }
        co_return ruvia::detail::HttpStreamReadResult::makeEnd();
    }
    ruvia::Task<std::error_code> writeBytes(std::string_view bytes, ruvia::detail::HttpStreamEnd end) {
        if (state.aborted) {
            co_return std::make_error_code(std::errc::operation_canceled);
        }
        state.output.append(bytes);
        if (end == ruvia::detail::HttpStreamEnd::kEnd) {
            ++state.finishes;
        }
        co_return std::error_code{};
    }
    void abort() noexcept {
        state.aborted = true;
    }
};
}  // namespace
RUVIA_TEST(httpTunnelOwnsColdWritesEnforcesDirectionLanesAndPreservesReadsAfterFinish) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    ruvia::test::CountingMemoryResource resource;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::Task<void> {
        try {
            TunnelTransportState state;
            const auto& worker = attachment.loop().handle();
            ruvia::detail::HttpTunnelSession session(TunnelTransport{state}, worker, resource);
            auto& tunnel = session.tunnel();
            {
                auto cold = tunnel.write("discarded");
            }
            RUVIA_CHECK(state.output.empty());
            std::string bytes(1024, 'w');
            auto write = tunnel.write(std::string_view(bytes));
            bytes.assign("mutated");
            bool overlap = false;
            try {
                auto busy = tunnel.finish();
            } catch (const std::logic_error&) {
                overlap = true;
            }
            RUVIA_CHECK(overlap);
            auto read = tunnel.read();
            co_await std::move(write);
            auto retained = co_await std::move(read);
            RUVIA_CHECK(retained && std::string_view(*retained) == std::string(512, 'r'));
            RUVIA_CHECK(state.output == std::string(1024, 'w'));
            co_await tunnel.finish();
            co_await tunnel.finish();
            RUVIA_CHECK_EQ(state.finishes, 1U);
            RUVIA_CHECK(!(co_await tunnel.read()));
            RUVIA_CHECK(std::string_view(*retained) == std::string(512, 'r'));
            bool afterFinish = false;
            try {
                auto invalid = tunnel.write("invalid");
            } catch (const std::logic_error&) {
                afterFinish = true;
            }
            RUVIA_CHECK(afterFinish);
            co_await session.join();
            bool expired = false;
            try {
                auto invalid = tunnel.read();
            } catch (const std::logic_error&) {
                expired = true;
            }
            RUVIA_CHECK(expired);
        } catch (...) {
            failure = std::current_exception();
        }
        attachment.stop();
    };
    auto task = attachment.loop().start(run());
    io.run();
    task.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}
