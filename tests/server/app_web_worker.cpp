#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/web/App.h"
#include "ruvia/core/detail/WorkerSelection.h"

namespace {

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
            accepted = first.post(
                           [](ruvia::WebWorkerContext&) -> ruvia::Task<void> {
                               throw std::runtime_error("app worker task failed");
                               co_return;
                           }) == ruvia::PostResult::kAccepted;
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
    for (const auto& worker : workers) {
        if (worker.accepting()) {
            return 2;
        }
    }
    return 0;
}
