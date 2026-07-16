// Worker-local state and blocking work: useWorkerState<T> per-worker
// instances shared by the HTTP and dispatch paths, cross-worker task posting
// via App::workerFor / WebWorkerHandle::post, and the sanctioned pattern for
// blocking calls -- run them on your own thread (or pool) and hand the result
// back to the request coroutine through a OneShot, keeping the worker free.

#include <chrono>
#include <cstdint>
#include <string_view>
#include <thread>
#include <utility>

#include "ruvia/core/OneShot.h"
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

    // Blocking escape hatch: the framework never runs blocking code for you.
    // Run it on a thread you own and complete a OneShot bound to this
    // request's worker; complete() is safe from any thread and the coroutine
    // resumes on its worker, which stayed free to serve other requests.
    ruvia::Task<ruvia::HttpResponse> offload(ruvia::Context& c) {
        auto [completion, receiver] =
            ruvia::makeOneShot<std::uint64_t>(c.worker());
        // A real application submits to a long-lived pool instead of
        // spawning a thread per request.
        std::thread([completion = std::move(completion)]() mutable {
            (void)completion.complete(slowChecksum("expensive input"));
        }).detach();

        const auto result = co_await receiver.wait();
        if (const auto* value = result.value()) {
            std::pmr::string body(c.resource());
            body.append("checksum=");
            body.append(std::to_string(*value));
            body.push_back('\n');
            co_return c.text(std::move(body));
        }
        // closed()/stopping(): the worker is shutting down mid-wait.
        co_return c.error(
            ruvia::http_status::kServiceUnavailable,
            "shutting_down",
            "worker is stopping");
    }

    // Cross-worker dispatch: pick a worker by key and post a job onto its
    // event loop. The job sees the SAME per-worker state instance that
    // worker's HTTP requests see.
    ruvia::Task<ruvia::HttpResponse> fanout(ruvia::Context& c) {
        std::size_t accepted = 0;
        for (const auto& worker : ruvia::app().workers()) {
            const auto posted = worker.post(
                [](ruvia::WebWorkerContext& ctx) -> ruvia::Task<void> {
                    ++ctx.workerState<WorkerStats>().dispatched;
                    co_return;
                });
            if (posted == ruvia::PostResult::kAccepted) {
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
        .setServerTopology(ruvia::ServerTopology::http(8090))
        .setWorkersPerListener(2)
        // Each worker builds its own WorkerStats before serving; the factory
        // form (useWorkerState<T>(fn)) covers non-default-constructible types.
        .useWorkerState<WorkerStats>()
        .run();
}
