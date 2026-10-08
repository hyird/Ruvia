#include <array>
#include <chrono>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/http/Http1ServerRequestParser.h"

#include "client/HttpClientConfigValidation.h"
#include "context/ContextAccess.h"
#include "context/HttpPushOutput.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
struct PushSink final {
    const ruvia::WorkerHandle& worker;
    ruvia::WorkerSignal entered;
    ruvia::StopSource stop;
    bool delay{};
    bool fail{};
    unsigned writes{};
    std::string path;
    std::string header;
    explicit PushSink(const ruvia::WorkerHandle& handle)
        : worker(handle),
          entered(handle) {}
    static ruvia::Task<bool> submit(void* raw, ruvia::HttpPushRequestView request) {
        auto& sink = *static_cast<PushSink*>(raw);
        if (sink.delay) {
            sink.entered.notify();
            const auto result = co_await ruvia::sleepFor(sink.worker, std::chrono::seconds(10), sink.stop.token());
            if (result == ruvia::TimerSleepResult::kStopRequested) {
                throw std::runtime_error("cancelled");
            }
        }
        if (sink.fail) {
            throw std::runtime_error("push failed");
        }
        sink.path = request.path;
        sink.header = request.headers.front().value();
        ++sink.writes;
        co_return true;
    }
};
}  // namespace

RUVIA_TEST(context_push_owns_inputs_reclaims_each_operation_and_handles_cold_failure_cancel_expiry) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    auto run = [&]() -> ruvia::Task<void> {
        const auto& worker = attachment.loop().handle();
        ruvia::test::CountingMemoryResource memory;
        ruvia::WorkerMemory workerMemory;
        ruvia::RequestMemory requestMemory(workerMemory);
        const auto parsed = ruvia::Http1ServerRequestParser{}.parseMessage("GET / HTTP/1.1\r\nHost: example.test\r\n\r\n");
        const ruvia::StopToken stop;
        PushSink sink(worker);
        auto makeContext = [&](auto& output) {
            return ruvia::detail::ContextAccess::make(requestMemory, parsed.request,
                ruvia::detail::ContextServices(worker, stop).withPushOutput(output));
        };
        {
            ruvia::detail::HttpPushOutput output(&memory, &sink, &PushSink::submit);
            auto context = makeContext(output);
            std::string path = "/" + std::string(80, 'a');
            std::string value(80, 'h');
            const std::array<ruvia::HttpHeaderView, 1> fields{{{"x-push", value}}};
            const ruvia::HttpPushRequestView request{.authority = "example.test", .path = path, .headers = fields};
            {
                auto cold = context.push(request);
                RUVIA_CHECK(memory.liveAllocations() > 0);
            }
            RUVIA_CHECK_EQ(sink.writes, 0U);
            RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
            for (unsigned repeat = 0; repeat != 128; ++repeat) {
                path.assign("/" + std::string(80, 'a'));
                value.assign(80, 'h');
                const std::array<ruvia::HttpHeaderView, 1> inputs{{{"x-push", value}}};
                auto operation = context.push({.authority = "example.test", .path = path, .headers = inputs});
                bool busy = false;
                try {
                    auto overlap = context.push(request);
                } catch (const std::logic_error&) {
                    busy = true;
                }
                RUVIA_CHECK(busy);
                path.assign("changed");
                value.assign("changed");
                RUVIA_CHECK(co_await std::move(operation));
                RUVIA_CHECK(sink.path == "/" + std::string(80, 'a') && sink.header == std::string(80, 'h'));
                RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
            }
            const std::array<ruvia::HttpHeaderView, 1> validFields{{{"x-push", "failure"}}};
            sink.fail = true;
            bool failed = false;
            try {
                (void)co_await context.push({.authority = "example.test", .headers = validFields});
            } catch (const std::runtime_error&) {
                failed = true;
            }
            RUVIA_CHECK(failed);
            RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
            sink.fail = false;
            sink.delay = true;
            ruvia::TaskScope tasks(worker);
            bool cancelled = false;
            auto wait = [&]() -> ruvia::Task<void> {
                try {
                    (void)co_await context.push({.authority = "example.test", .headers = validFields});
                } catch (const std::runtime_error&) {
                    cancelled = true;
                }
            };
            tasks.spawn(wait());
            co_await sink.entered.wait();
            sink.stop.requestStop();
            co_await tasks.join();
            RUVIA_CHECK(cancelled);
            RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
        }
        {
            std::optional<ruvia::detail::HttpPushOutput> output;
            output.emplace(&memory, &sink, &PushSink::submit);
            auto context = makeContext(*output);
            auto expired = context.push({.authority = "example.test"});
            output.reset();
            bool rejected = false;
            try {
                (void)co_await std::move(expired);
            } catch (const std::logic_error&) {
                rejected = true;
            }
            RUVIA_CHECK(rejected);
        }
        RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
}

RUVIA_TEST(http_client_push_configuration_rejects_zero_bounds_timeout_and_unverified_https) {
    ruvia::HttpClientConfig config{.host = "example.test"};
    for (unsigned mode = 0; mode != 4; ++mode) {
        config.push = {.enabled = true};
        config.tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kVerify;
        if (mode == 0) {
            config.push.maxQueuedPushes = 0;
        }
        if (mode == 1) {
            config.push.maxConcurrentPushes = 0;
        }
        if (mode == 2) {
            config.push.timeout = std::chrono::milliseconds::zero();
        }
        if (mode == 3) {
            config.tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification;
        }
        bool rejected = false;
        try {
            ruvia::detail::validateHttpClientConfig(config);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    }
}
