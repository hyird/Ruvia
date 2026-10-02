#include <algorithm>
#include <array>
#include <future>
#include <string>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/Http1ServerRequestParser.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/server/http1/Http1InterimResponseSink.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
struct OutputStream final {
    asio::io_context& io;
    std::string wire;
    bool fail{};
    using executor_type = asio::io_context::executor_type;
    executor_type get_executor() noexcept {
        return io.get_executor();
    }
    template <typename Buffers, typename Handler>
    void async_write_some(const Buffers& buffers, Handler handler) {
        const auto count = std::min(asio::buffer_size(buffers), std::size_t{7});
        std::array<char, 7> bytes{};
        asio::buffer_copy(asio::buffer(bytes), buffers, count);
        if (!fail) {
            wire.append(bytes.data(), count);
        }
        asio::post(io, [handler = std::move(handler), count, fail = fail]() mutable {
            handler(fail ? std::make_error_code(std::errc::broken_pipe) : std::error_code{}, fail ? 0 : count);
        });
    }
};
}  // namespace

RUVIA_TEST(http1_context_interim_output_owns_fields_drops_cold_operations_and_rejects_final_overlap) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    ruvia::test::CountingMemoryResource upstream;
    {
        ruvia::WorkerMemory memory(upstream);
        ruvia::RequestMemory requestMemory(memory);
        const auto worker = attachment.loop().handle();
        ruvia::StopSource stop;
        const auto stopToken = stop.token();
        OutputStream stream{io};
        ruvia::detail::Http1InterimResponseSink sink(stream, memory.resource());
        const auto parsed = ruvia::Http1ServerRequestParser{}.parseMessage("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
        auto context = ruvia::detail::ContextAccess::make(requestMemory, parsed.request,
            ruvia::detail::ContextServices(worker, stopToken).withInterimOutput(sink.output()));
        auto exercise = [&]() -> ruvia::Task<void> {
            std::string field(512, 'h');
            const std::array headers{ruvia::HttpHeaderView("Link", field)};
            {
                auto cold = context.inform(ruvia::HttpInterimResponseHead(ruvia::http_status::kEarlyHints, headers));
            }
            RUVIA_CHECK(stream.wire.empty());
            auto operation = context.inform(ruvia::HttpInterimResponseHead(ruvia::http_status::kEarlyHints, headers));
            field.assign("changed");
            bool overlap = false;
            try {
                sink.output().commitFinal();
            } catch (const std::logic_error&) {
                overlap = true;
            }
            RUVIA_CHECK(overlap);
            co_await std::move(operation);
            RUVIA_CHECK_EQ(stream.wire, "HTTP/1.1 103 Early Hints\r\nLink: " + std::string(512, 'h') + "\r\n\r\n");
            const auto before = stream.wire;
            const std::array invalid{ruvia::HttpHeaderView("Content-Length", "0")};
            bool rejected = false;
            try {
                co_await context.inform(ruvia::HttpInterimResponseHead(ruvia::http_status::kContinue, invalid));
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            RUVIA_CHECK(rejected);
            RUVIA_CHECK_EQ(stream.wire, before);
            stream.fail = true;
            bool failed = false;
            try {
                co_await context.inform(ruvia::HttpInterimResponseHead(ruvia::http_status::kContinue));
            } catch (const std::system_error&) {
                failed = true;
            }
            RUVIA_CHECK(failed);
            sink.output().commitFinal();
            rejected = false;
            try {
                auto invalidAfterFinal = context.inform(ruvia::HttpInterimResponseHead(ruvia::http_status::kEarlyHints));
            } catch (const std::logic_error&) {
                rejected = true;
            }
            RUVIA_CHECK(rejected);
        };
        auto run = [&]() -> ruvia::Task<void> {
            try {
                co_await exercise();
            } catch (...) {
                attachment.stop();
                throw;
            }
            attachment.stop();
        };
        auto root = attachment.loop().start(run());
        attachment.run();
        root.get();
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
}
