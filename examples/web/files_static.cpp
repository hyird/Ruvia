// Static files: c.file(...), c.staticFile(...), StaticRoot, a document root,
// response-validator/range-request policies and gzip configuration.
//
// A standalone StaticRoot is immutable. App document roots refresh every
// second; see the commented interval configuration below.

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
        co_return c.file({.path = examplesRoot() / "public" / "hello.txt", .contentType = "text/plain; charset=utf-8"});
    }

    ruvia::Task<ruvia::HttpResponse> asset(ruvia::Context& c) {
        co_return c.staticFile(*gAssets, {.relativePath = c.req().param("*").value_or("index.html")});
    }
};

int main() {
    gAssets = std::make_unique<ruvia::StaticRoot>(
        examplesRoot() / "public",
        ruvia::StaticRootOptions{
            .cacheControl = "public, max-age=3600",
            .indexFile = "index.html",
            .rangeRequests = ruvia::StaticRangeRequestPolicy::kHonor,
            .responseValidators = ruvia::StaticResponseValidatorPolicy::kEmit,
        });

    auto documentRoot = ruvia::DocumentRootConfig{
        .root = examplesRoot() / "public",
        .staticOptions = {
            .cacheControl = "public, max-age=3600",
            .indexFile = "index.html",
        },
    };
    // Document roots refresh every second by default. To tune the interval:
    // .runtimeOptions = {
    //     .refreshInterval = std::chrono::milliseconds(500),
    // },
    // The application blocking pool is enabled by default.

    ruvia::app().listen({.address = "0.0.0.0", .http = 8083}).setWorkerCount(2).setProcessSignalHandlers(ruvia::ProcessSignalHandlerPolicy::kInstall).setCompression(ruvia::CompressionConfig{}).setDocumentRoot(std::move(documentRoot)).run();
}
