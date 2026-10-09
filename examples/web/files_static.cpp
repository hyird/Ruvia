// Static files: c.file(...), c.static_file(...), static_root, a document root,
// response-validator/range-request policies and gzip configuration.
//
// A standalone static_root is immutable. application document roots refresh every
// second; see the commented interval configuration below.
// Run ruvia_example_files_static, then GET http://127.0.0.1:8083/index.html
// or /files/assets/hello.txt. /files/download describes a file-backed response.
// Try Range: bytes=0-4 (206), If-None-Match with the returned ETag (304), and
// Accept-Encoding: gzip. Compression depends on type, size, and negotiation.
// The document root refreshes; the explicitly constructed static_root is a
// snapshot. Keep it alive until application::run() returns and all file responses retire.

#include <filesystem>
#include <memory>

#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/static_files.h"

namespace {

std::unique_ptr<ruvia::static_root> g_assets;

std::filesystem::path examples_root() {
    return std::filesystem::path(RUVIA_EXAMPLES_SOURCE_DIR);
}

}  // namespace

class files_controller final : public ruvia::controller<files_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/files")

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/download", download);
    RUVIA_GET("/assets/*", asset);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> download(ruvia::context& c) {
        co_return c.file({.path_ = examples_root() / "public" / "hello.txt",
            .content_type_ = "text/plain; charset=utf-8"});
    }

    ruvia::task<ruvia::http_response> asset(ruvia::context& c) {
        co_return c.static_file(
            *g_assets, {.relative_path_ = c.req().param("*").value_or("index.html")});
    }
};

int main() {
    g_assets = std::make_unique<ruvia::static_root>(examples_root() / "public",
        ruvia::static_root_options{
            .cache_control_ = "public, max-age=3600",
            .index_file_ = "index.html",
            .mime_types_ = {{"txt", "text/plain; charset=utf-8"}},
            .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind_type::only,
                .extensions_ = {"html", "txt"}},
            .range_requests_ = ruvia::static_range_request_policy::honor,
            .response_validators_ = ruvia::static_response_validator_policy::emit,
            .dotfiles_ = ruvia::static_dotfile_policy::deny,
        });

    auto document_root = ruvia::document_root_config{
        .root_ = examples_root() / "public",
        .static_options_ =
            {
                .cache_control_ = "public, max-age=3600",
                .index_file_ = "index.html",
            },
        .precompress_gzip_ = true,
    };
    // Document roots refresh every second by default. To tune the interval:
    // .runtime = {
    //     .refresh_interval_ = std::chrono::milliseconds(500),
    // },
    // .precompress_gzip_ refresh-builds gzip variants for changed text assets.
    // The application blocking pool is enabled by default.

    ruvia::app()
        .listen({.address_ = "0.0.0.0", .http_ = 8083})
        .server({.worker_count_ = 2,
            .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
        .compression({})
        .document_root(std::move(document_root))
        .run();
}
