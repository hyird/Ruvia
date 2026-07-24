#include <cstdint>
#include <stdexcept>

#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"

class StartupRetryController final : public ruvia::Controller<StartupRetryController> {
public:
    RUVIA_CONTROLLER_GROUP("")

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/startup-retry", handle);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> handle(ruvia::Context& context) {
        co_return context.text("ok");
    }
};

namespace {

std::uint16_t availablePort() {
    asio::io_context context;
    asio::ip::tcp::acceptor acceptor(context, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    return acceptor.local_endpoint().port();
}

}  // namespace

int main() {
    auto& app = ruvia::app();
    app.setListenAddress("not-an-ip-address").setWorkersPerListener(1).setMemoryPoolConfig({.requestInitialBufferBytes = ruvia::kRequestArenaInitialBytes});

    bool preparationFailed = false;
    try {
        app.run();
    } catch (const std::exception&) {
        preparationFailed = true;
    }
    if (!preparationFailed || !app.workers().empty()) {
        return 1;
    }

    bool started = false;
    std::size_t stopCalls = 0;
    app.setListenAddress("127.0.0.1")
        .setServerTopology(ruvia::ServerTopology::http(availablePort()))
        .setMemoryPoolConfig({.requestInitialBufferBytes = ruvia::kRequestArenaInitialBytes * 2})
        .onStart([&] {
            started = true;
            app.stop();
        })
        .onStop([&] { ++stopCalls; });
    app.run();

    return started && stopCalls == 1 ? 0 : 2;
}
