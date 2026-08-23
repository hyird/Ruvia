#include <cstdint>
#include <filesystem>
#include <fstream>
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
    app.listen({.address = "not-an-ip-address", .http = availablePort()})
        .server({.workerCount = 1, .memoryPool = {.requestInitialBufferBytes = ruvia::kRequestArenaInitialBytes}});

    bool preparationFailed = false;
    try {
        app.run();
    } catch (const std::exception&) {
        preparationFailed = true;
    }
    if (!preparationFailed || !app.workers().empty()) {
        return 1;
    }

    const auto staticRootPath = std::filesystem::temp_directory_path() / "ruvia_app_startup_retry_static_root";
    std::filesystem::remove_all(staticRootPath);
    std::filesystem::create_directories(staticRootPath);
    std::ofstream(staticRootPath / "index.html") << "ok";
    ruvia::DocumentRootConfig documentRoot;
    documentRoot.root = staticRootPath;
    documentRoot.staticOptions.fileTypes = ruvia::StaticFileTypePolicy{.kind = ruvia::StaticFileTypePolicy::Kind::kAll};
    documentRoot.staticOptions.mimeTypes.push_back(ruvia::StaticMimeType{
        .extension = ".custom",
        .contentType = "text/x-custom",
    });
    app.documentRoot(std::move(documentRoot));

    bool started = false;
    std::size_t stopCalls = 0;
    app.listen({.address = "127.0.0.1", .http = availablePort()})
        .server({.workerCount = 1, .memoryPool = {.requestInitialBufferBytes = ruvia::kRequestArenaInitialBytes * 2}})
        .onStart([&] {
            started = true;
            app.stop();
        })
        .onStop([&] { ++stopCalls; });
    app.run();

    std::filesystem::remove_all(staticRootPath);
    return started && stopCalls == 1 ? 0 : 2;
}
