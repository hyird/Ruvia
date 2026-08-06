// Static files: c.file(...), c.staticFile(...), StaticRoot, a document root,
// validators/ranges and gzip configuration.
//
// A standalone StaticRoot is an immutable index by default. App document roots
// can opt into development polling; see the commented configuration below.

#include <filesystem>
#include <memory>

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/StaticFiles.h"

namespace {

std::unique_ptr<ruvia::StaticRoot> gAssets;

std::filesystem::path examplesRoot() {
    return std::filesystem::path(RUVIA_EXAMPLES_SOURCE_DIR);
}

}  // namespace

class FilesController final : public ruvia::Controller<FilesController> {
public:
    RUVIA_CONTROLLER_GROUP("/files")

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/download", download);
    RUVIA_GET("/assets/*", asset);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> download(ruvia::Context& c) {
        co_return c.file(examplesRoot() / "public" / "hello.txt", "text/plain; charset=utf-8");
    }

    ruvia::Task<ruvia::HttpResponse> asset(ruvia::Context& c) {
        co_return c.staticFile(*gAssets, c.req().param("*").value_or("index.html"));
    }
};

int main() {
    ruvia::StaticRootOptions staticOptions;
    staticOptions.indexFile = "index.html";
    staticOptions.cacheControl = "public, max-age=3600";
    staticOptions.enableRanges = true;
    staticOptions.enableValidators = true;

    gAssets = std::make_unique<ruvia::StaticRoot>(examplesRoot() / "public", std::move(staticOptions));

    ruvia::DocumentRootConfig documentRoot;
    documentRoot.root = examplesRoot() / "public";
    documentRoot.staticOptions.indexFile = "index.html";
    documentRoot.staticOptions.cacheControl = "public, max-age=3600";
    // Development-only document-root refresh and browser reload:
    // documentRoot.runtimeOptions.refreshMode = ruvia::DocumentRootRefreshMode::kPolling;
    // documentRoot.runtimeOptions.refreshInterval = std::chrono::milliseconds(500);
    // documentRoot.runtimeOptions.enableLiveReload = true;
    // documentRoot.runtimeOptions.onDemandCompressionMaxBytes = 2 * 1024 * 1024;
    // Polling requires an application blocking pool:
    // ruvia::app().setBlockingPool(ruvia::BlockingPoolOptions{.threadCount = 2});

    ruvia::app().setListeners({ruvia::ListenerConfig::http("0.0.0.0", 8083)}).setWorkersPerListener(2).setSignalShutdown(true).setCompression(ruvia::CompressionConfig{.minBytes = 128}).setDocumentRoot(std::move(documentRoot)).run();
}
