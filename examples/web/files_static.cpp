// Static files: c.file(...), c.staticFile(...), StaticRoot, a document root,
// response-validator/range-request policies and gzip configuration.
//
// A standalone StaticRoot is immutable. App document roots refresh every
// second; see the commented interval configuration below.
// Run ruvia_example_files_static, then GET http://127.0.0.1:8083/index.html
// or /files/assets/hello.txt. /files/download describes a file-backed response.
// Try Range: bytes=0-4 (206), If-None-Match with the returned ETag (304), and
// Accept-Encoding: gzip. Compression depends on type, size, and negotiation.
// The document root refreshes; the explicitly constructed StaticRoot is a
// snapshot. Keep it alive until App::run() returns and all file responses retire.

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
        co_return c.file({.path = examplesRoot() / "public" / "hello.txt",
            .contentType = "text/plain; charset=utf-8"});
    }

    ruvia::Task<ruvia::HttpResponse> asset(ruvia::Context& c) {
        co_return c.staticFile(
            *gAssets, {.relativePath = c.req().param("*").value_or("index.html")});
    }
};

int main() {
    gAssets = std::make_unique<ruvia::StaticRoot>(examplesRoot() / "public",
        ruvia::StaticRootOptions{
            .cacheControl = "public, max-age=3600",
            .indexFile = "index.html",
            .mimeTypes = {{"txt", "text/plain; charset=utf-8"}},
            .fileTypes = {.kind = ruvia::StaticFileTypePolicy::Kind::kOnly,
                .extensions = {"html", "txt"}},
            .rangeRequests = ruvia::StaticRangeRequestPolicy::kHonor,
            .responseValidators = ruvia::StaticResponseValidatorPolicy::kEmit,
            .dotfiles = ruvia::StaticDotfilePolicy::kDeny,
        });

    auto documentRoot = ruvia::DocumentRootConfig{
        .root = examplesRoot() / "public",
        .staticOptions =
            {
                .cacheControl = "public, max-age=3600",
                .indexFile = "index.html",
            },
        .precompressGzip = true,
    };
    // Document roots refresh every second by default. To tune the interval:
    // .runtime = {
    //     .refreshInterval = std::chrono::milliseconds(500),
    // },
    // .precompressGzip refresh-builds gzip variants for changed text assets.
    // The application blocking pool is enabled by default.

    ruvia::app()
        .listen({.address = "0.0.0.0", .http = 8083})
        .server({.worker_count = 2,
            .process_signal_handlers = ruvia::process_signal_handler_policy::install})
        .compression({})
        .documentRoot(std::move(documentRoot))
        .run();
}
