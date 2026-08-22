// Worker-local state and blocking work: useWorkerState<T> per-worker
// instances shared by the HTTP and dispatch paths, cross-worker task posting
// via App::workerFor / WebWorkerHandle::post, and Context::runBlocking() --
// the escape hatch for calls that block, which run on App::setBlockingPool()'s
// threads so the worker stays free to serve its other connections.

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"

namespace {

// One instance per worker, touched only from that worker's thread: no locks.
struct WorkerStats final {
    std::uint64_t served{0};
    std::uint64_t dispatched{0};
};

// Stand-in for a genuinely blocking call (sync SDK, file crypto, ...).
std::uint64_t slowChecksum(std::string_view input) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::uint64_t sum = 1469598103934665603ull;
    for (const char byte : input) {
        sum = (sum ^ static_cast<unsigned char>(byte)) * 1099511628211ull;
    }
    return sum;
}

}  // namespace

class WorkerToolsController final : public ruvia::Controller<WorkerToolsController> {
public:
    RUVIA_CONTROLLER_GROUP("/workers")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/stats", stats);
    RUVIA_GET("/offload", offload);
    RUVIA_GET("/offload-or-shed", offloadOrShed);
    RUVIA_GET("/fanout", fanout);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> stats(ruvia::Context& c) {
        auto& state = c.workerState<WorkerStats>();
        ++state.served;
        std::pmr::string body(c.resource());
        body.append("served=");
        body.append(std::to_string(state.served));
        body.append(" dispatched=");
        body.append(std::to_string(state.dispatched));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    // Blocking escape hatch. The callable runs on a pool thread, so it must own
    // what it touches: the query value is copied into the lambda, never
    // borrowed from the request. The handler suspends and resumes on its own
    // worker, which kept serving other connections meanwhile. A saturated pool
    // throws BlockingOperationRejected at the co_await, which the default error
    // path answers with 503.
    ruvia::Task<ruvia::HttpResponse> offload(ruvia::Context& c) {
        const auto checksum = co_await c.runBlocking([input = std::string(c.req().query("input").value_or("default"))] { return slowChecksum(input); });
        std::pmr::string body(c.resource());
        body.append("checksum=");
        body.append(std::to_string(checksum));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    // The same work, with the overload answered by this handler instead of by
    // the error path, and a deadline so one wedged call cannot pin the request
    // forever.
    ruvia::Task<ruvia::HttpResponse> offloadOrShed(ruvia::Context& c) {
        auto result = co_await c.tryRunBlocking(std::chrono::seconds(2), [input = std::string("expensive input")] { return slowChecksum(input); });
        if (!result.completed()) {
            co_return c.error({.status = ruvia::http_status::kServiceUnavailable, .code = "busy", .message = ruvia::describeBlockingStatus(result.status())});
        }
        std::pmr::string body(c.resource());
        body.append("checksum=");
        body.append(std::to_string(std::move(result).value()));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    // Cross-worker dispatch: pick a worker by key and post a job onto its
    // event loop. The job sees the SAME per-worker state instance that
    // worker's HTTP requests see.
    ruvia::Task<ruvia::HttpResponse> fanout(ruvia::Context& c) {
        std::size_t accepted = 0;
        for (const auto& worker : ruvia::app().workers()) {
            const auto posted = worker.post([](ruvia::WebWorkerContext& ctx) -> ruvia::Task<void> {
                ++ctx.workerState<WorkerStats>().dispatched;
                co_return;
            });
            if (posted == ruvia::PostStatus::kAccepted) {
                ++accepted;
            }
        }
        std::pmr::string body(c.resource());
        body.append("posted=");
        body.append(std::to_string(accepted));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }
};

int main() {
    ruvia::app()
        .setListeners({ruvia::ListenerConfig::http(ruvia::ListenerId{1}, {.address = "0.0.0.0", .port = 8090})})
        .setWorkerCount(2)
        .setProcessSignalHandlers(ruvia::ProcessSignalHandlerPolicy::kInstall)
        // Each worker builds its own WorkerStats before serving; the factory
        // form (useWorkerState<T>(fn)) covers non-default-constructible types.
        .useWorkerState<WorkerStats>()
        // One pool for the whole process, shared by every worker. App provides
        // a bounded pool by default; this overrides its size for the example.
        // App::blockingPoolStats() reports queue depth and rejections for
        // production tuning.
        .setBlockingPool(ruvia::BlockingPoolOptions{
            .threadCount = 4,
            .queueCapacity = 128,
        })
        .run();
}
