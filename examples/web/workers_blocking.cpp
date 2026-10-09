// Worker-local state and blocking work: use_worker_state<T> per-worker
// instances shared by the HTTP and dispatch paths, cross-worker task posting
// via application::worker_for / web_worker_handle::post, and context::run_blocking() --
// the escape hatch for calls that block, which run on application::get_blocking_pool()'s
// threads so the worker stays free to serve its other connections.
// Run ruvia_example_workers_blocking on port 8090; inspect its route macros
// for stats, awaited dispatch, fanout and overload shedding endpoints.
// Offloaded work takes owned input. Do not capture context or worker state
// in a blocking job: stopping a request does not stop that native thread.

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"

namespace {

// One instance per worker, touched only from that worker's thread: no locks.
struct worker_stats final {
    std::uint64_t served_{0};
    std::uint64_t dispatched_{0};
};

// Stand-in for a genuinely blocking call (sync SDK, file crypto, ...).
std::uint64_t slow_checksum(std::string_view input) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::uint64_t sum = 1469598103934665603ull;
    for (const char byte : input) {
        sum = (sum ^ static_cast<unsigned char>(byte)) * 1099511628211ull;
    }
    return sum;
}

}  // namespace

class worker_tools_controller final : public ruvia::controller<worker_tools_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/workers")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/stats", stats);
    RUVIA_GET("/offload", offload);
    RUVIA_GET("/offload-or-shed", offload_or_shed);
    RUVIA_GET("/fanout", fanout);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> stats(ruvia::context& c) {
        auto& state_value = c.worker_state<worker_stats>();
        ++state_value.served_;
        std::pmr::string body(c.arena());
        body.append("served=");
        body.append(std::to_string(state_value.served_));
        body.append(" dispatched=");
        body.append(std::to_string(state_value.dispatched_));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    // Blocking escape hatch. The callable runs on a pool thread, so it must own
    // what it touches: the query value is copied into the lambda, never
    // borrowed from the request. The handler suspends and resumes on its own
    // worker, which kept serving other connections meanwhile. A saturated pool
    // throws blocking_operation_rejected at the co_await, which the default error
    // path answers with 503.
    ruvia::task<ruvia::http_response> offload(ruvia::context& c) {
        const auto checksum = co_await c.run_blocking(
            [input = std::string(c.req().query("input").value_or("default"))] {
                return slow_checksum(input);
            });
        std::pmr::string body(c.arena());
        body.append("checksum=");
        body.append(std::to_string(checksum));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    // The same work, with the overload answered by this handler instead of by
    // the error path, and a deadline so one wedged call cannot pin the request
    // forever.
    ruvia::task<ruvia::http_response> offload_or_shed(ruvia::context& c) {
        auto result_value = co_await c.try_run_blocking(std::chrono::seconds(2),
            [input = std::string("expensive input")] { return slow_checksum(input); });
        if (!result_value.completed()) {
            co_return c.error({.status_ = ruvia::http_status::service_unavailable,
                .code_ = "busy",
                .message_ = ruvia::describe_blocking_status(result_value.status())});
        }
        std::pmr::string body(c.arena());
        body.append("checksum=");
        body.append(std::to_string(std::move(result_value).value()));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    // Cross-worker dispatch: pick a worker by key and post a job onto its
    // event loop. The job sees the SAME per-worker state instance that
    // worker's HTTP requests see.
    ruvia::task<ruvia::http_response> fanout(ruvia::context& c) {
        std::size_t accepted = 0;
        for (const auto& worker : ruvia::app().workers()) {
            const auto posted = worker.post([](ruvia::web_worker_context& ctx) -> ruvia::task<void> {
                ++ctx.worker_state<worker_stats>().dispatched_;
                co_return;
            });
            if (posted == ruvia::post_status::accepted) {
                ++accepted;
            }
        }
        std::pmr::string body(c.arena());
        body.append("posted=");
        body.append(std::to_string(accepted));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }
};

int main() {
    ruvia::app()
        .listen({.address_ = "0.0.0.0", .http_ = 8090})
        .server({.worker_count_ = 2,
            .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
        // Each worker builds its own worker_stats before serving; the factory
        // form (use_worker_state<T>(fn)) covers non-default-constructible types.
        .use_worker_state<worker_stats>()
        // One pool for the whole process, shared by every worker. application provides
        // a bounded pool by default; this overrides its size for the example.
        // application::get_blocking_pool_stats() reports queue depth and rejections for
        // production tuning.
        .get_blocking_pool({
            .thread_count_ = 4,
            .queue_capacity_ = 128,
        })
        .run();
}
