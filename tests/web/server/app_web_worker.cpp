#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"
#include "ruvia/core/detail/worker/WorkerSelection.h"

namespace {

int controllerConstructed = 0;
int controllerDestroyed = 0;
int middlewareConstructed = 0;
int middlewareDestroyed = 0;

class InstanceProbeMiddleware final
    : public ruvia::Middleware<InstanceProbeMiddleware> {
public:
    InstanceProbeMiddleware() {
        ++middlewareConstructed;
    }

    ~InstanceProbeMiddleware() {
        ++middlewareDestroyed;
    }

    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next& next) {
        co_await next();
    }
};

class InstanceProbeController final
    : public ruvia::Controller<InstanceProbeController> {
public:
    InstanceProbeController() {
        ++controllerConstructed;
    }

    ~InstanceProbeController() {
        ++controllerDestroyed;
    }

    RUVIA_CONTROLLER_GROUP("", InstanceProbeMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/instance-probe", probe);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> probe(ruvia::Context& context) {
        co_return context.text("ok");
    }
};

std::uint16_t availablePort() {
    asio::io_context context;
    asio::ip::tcp::acceptor acceptor(
        context,
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    return acceptor.local_endpoint().port();
}

}  // namespace

int main() {
    auto& app = ruvia::app();
    bool rejectedZeroCapacity = false;
    try {
        app.setWorkerMailboxCapacity(0);
    } catch (const std::invalid_argument&) {
        rejectedZeroCapacity = true;
    }
    bool stableSelection = false;
    bool isolatedInstances = false;
    bool accepted = false;
    bool startHookFinished = false;
    bool stopHookAfterStart = false;
    std::size_t stopCalls = 0;
    std::vector<ruvia::WebWorkerHandle> workers;

    app.setListenAddress("127.0.0.1")
        .setServerTopology(ruvia::ServerTopology::http(availablePort()))
        .setWorkersPerListener(2)
        .setWorkerMailboxCapacity(8)
        .onStop([&] {
            ++stopCalls;
            stopHookAfterStart = startHookFinished;
        })
        .onStart([&] {
            workers = app.workers();
            constexpr std::string_view key = "device-42";
            const auto first = app.workerFor(key);
            const auto second =
                app.workerFor(ruvia::detail::workerSelectionHash(key));
            stableSelection = workers.size() == 2 && first.valid() &&
                              first.id() == second.id();
            isolatedInstances = controllerConstructed == 2 &&
                                middlewareConstructed == 2;
            accepted = first.post(
                           [](ruvia::WebWorkerContext&) -> ruvia::Task<void> {
                               throw std::runtime_error("app worker task failed");
                               co_return;
                           }) == ruvia::PostStatus::kAccepted;
            app.stop();
            startHookFinished = true;
        });

    bool propagated = false;
    try {
        app.run();
    } catch (const std::runtime_error& error) {
        propagated = std::string_view(error.what()) == "app worker task failed";
    }

    if (!rejectedZeroCapacity || !stableSelection || !accepted || !propagated ||
        !stopHookAfterStart || stopCalls != 1) {
        return 1;
    }
    if (!isolatedInstances) {
        return 2;
    }
    if (controllerDestroyed != 2 || middlewareDestroyed != 2) {
        return 3;
    }
    for (const auto& worker : workers) {
        if (worker.accepting()) {
            return 4;
        }
    }
    return 0;
}
