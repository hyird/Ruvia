#include <array>
#include <chrono>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/Http1ServerRequestParser.h"
#include "ruvia/http/Http2Framing.h"

#include "context/ContextAccess.h"
#include "context/HttpConnectionAdvertisementOutput.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
using Output = ruvia::detail::HttpConnectionAdvertisementOutput;
struct Sink final {
    const ruvia::WorkerHandle& worker;
    std::pmr::memory_resource* resource;
    ruvia::WorkerSignal entered;
    ruvia::StopSource stop;
    std::string wire;
    bool fail{};
    bool delay{};
    Sink(const ruvia::WorkerHandle& worker, std::pmr::memory_resource* resource)
        : worker(worker),
          resource(resource),
          entered(worker) {}
    static ruvia::Task<void> origins(void* raw, std::span<const std::string_view> origins) {
        auto& sink = *static_cast<Sink*>(raw);
        if (sink.delay) {
            sink.entered.notify();
            const auto result = co_await ruvia::sleepFor(sink.worker, std::chrono::seconds(10), sink.stop.token());
            if (result == ruvia::TimerSleepResult::kStopRequested) {
                throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            }
        }
        if (sink.fail) {
            throw std::system_error(std::make_error_code(std::errc::broken_pipe));
        }
        const auto encoded = ruvia::encodeHttp2OriginFrame(origins, 16384, sink.resource);
        if (!encoded) {
            throw std::invalid_argument("invalid advertised origin");
        }
        sink.wire.append(encoded->data(), encoded->size());
    }
    static ruvia::Task<void> service(void* raw, std::string_view value) {
        auto& sink = *static_cast<Sink*>(raw);
        const auto encoded = ruvia::encodeHttp2AlternativeServiceFrame(1, {}, value, 16384, sink.resource);
        if (!encoded) {
            throw std::invalid_argument("invalid alternative service");
        }
        sink.wire.append(encoded->data(), encoded->size());
        co_return;
    }
};
}  // namespace

RUVIA_TEST(context_connection_advertisements_own_inputs_reclaim_storage_and_handle_cancel_failure_and_expiry) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto run = [&]() -> ruvia::Task<void> {
        ruvia::test::CountingMemoryResource memory;
        ruvia::WorkerMemory workerMemory;
        ruvia::RequestMemory requestMemory(workerMemory);
        const auto parsed = ruvia::Http1ServerRequestParser{}.parseMessage("GET / HTTP/1.1\r\nHost: example.test\r\n\r\n");
        ruvia::StopSource workerStop;
        const auto token = workerStop.token();
        Sink sink(worker, &memory);
        Output output(&memory, &sink, Sink::origins, Sink::service);
        auto context = ruvia::detail::ContextAccess::make(requestMemory, parsed.request,
            ruvia::detail::ContextServices(worker, token).withTlsTransport("127.0.0.1", {}).withConnectionAdvertisements(output));
        std::string value = "https://" + std::string(60, 'a') + ".example.test";
        const std::array<std::string_view, 1> origins{value};
        {
            auto cold = context.advertiseOrigins(origins);
            RUVIA_CHECK(memory.liveAllocations() > 0);
        }
        RUVIA_CHECK(sink.wire.empty());
        RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
        {
            auto operation = context.advertiseOrigins(origins);
            value.assign("changed");
            bool busy = false;
            try {
                auto overlap = context.advertiseAlternativeService("clear");
            } catch (const std::logic_error&) {
                busy = true;
            }
            RUVIA_CHECK(busy);
            co_await std::move(operation);
        }
        const auto frame = ruvia::parseHttp2FrameHeader(std::span<const char>(sink.wire));
        RUVIA_CHECK(frame.has_value());
        const auto decoded = ruvia::decodeHttpOriginAdvertisement(std::span<const char>(sink.wire).subspan(9, frame->length));
        const auto expected = "https://" + std::string(60, 'a') + ".example.test";
        RUVIA_CHECK(decoded && decoded->origins.size() == 1 && std::string_view(decoded->origins[0]) == expected);
        RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
        for (unsigned repeat = 0; repeat != 128; ++repeat) {
            std::string service = "h3=\":443\"; ma=86400";
            auto operation = context.advertiseAlternativeService(service);
            service.assign("changed");
            co_await std::move(operation);
            RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
        }
        const std::array<std::string_view, 1> valid{"https://example.test"};
        sink.fail = true;
        bool failed = false;
        try {
            co_await context.advertiseOrigins(valid);
        } catch (const std::system_error&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
        sink.fail = false;
        sink.delay = true;
        bool cancelled = false;
        {
            ruvia::TaskScope tasks(worker);
            auto cancelling = [&]() -> ruvia::Task<void> {
                try {
                    co_await context.advertiseOrigins(valid);
                } catch (const std::system_error& error) {
                    cancelled = error.code() == std::errc::operation_canceled;
                }
            };
            tasks.spawn(cancelling());
            co_await sink.entered.wait();
            sink.stop.requestStop();
            co_await tasks.join();
        }
        RUVIA_CHECK(cancelled);
        RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
        {
            std::optional<Output> expiring;
            expiring.emplace(&memory, &sink, Sink::origins, nullptr);
            auto stale = expiring->advertiseOrigins(valid);
            expiring.reset();
            bool expired = false;
            try {
                co_await std::move(stale);
            } catch (const std::logic_error&) {
                expired = true;
            }
            RUVIA_CHECK(expired);
        }
        RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
}
