#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory_resource>
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

class ReleasableMemoryResource final : public std::pmr::memory_resource {
public:
    void release() noexcept {
        released_ = true;
    }

    [[nodiscard]] bool deallocatedAfterRelease() const noexcept {
        return deallocatedAfterRelease_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        deallocatedAfterRelease_ = deallocatedAfterRelease_ || released_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool released_{false};
    bool deallocatedAfterRelease_{false};
};

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

    const auto staticRootPath = std::filesystem::temp_directory_path() / "ruvia_app_startup_retry_static_root";
    std::filesystem::remove_all(staticRootPath);
    std::filesystem::create_directories(staticRootPath);
    std::ofstream(staticRootPath / "index.html") << "ok";
    ReleasableMemoryResource callerResource;
    ruvia::DocumentRootConfig documentRoot;
    documentRoot.root = staticRootPath;
    documentRoot.staticOptions.fileTypes = ruvia::StaticFileTypePolicy::all();
    documentRoot.staticOptions.mimeTypes = std::pmr::vector<ruvia::StaticMimeType>(&callerResource);
    documentRoot.staticOptions.mimeTypes.push_back(ruvia::StaticMimeType{
        .extension = std::pmr::string(".custom", &callerResource),
        .contentType = std::pmr::string("text/x-custom", &callerResource),
    });
    app.setDocumentRoot(std::move(documentRoot));
    callerResource.release();

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

    const bool configDidNotRetainCallerResource = !callerResource.deallocatedAfterRelease();
    std::filesystem::remove_all(staticRootPath);
    return started && stopCalls == 1 && configDidNotRetainCallerResource ? 0 : 2;
}
