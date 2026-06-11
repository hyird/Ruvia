#include <filesystem>
#include <memory>

#include "ruvia/app/App.h"
#include "ruvia/http/Controller.h"
#include "ruvia/http/StaticFiles.h"

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
        co_return c.staticFile(*gAssets, c.param("*").toStringView().value_or("index.html"));
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

    ruvia::app()
        .setListenAddress("0.0.0.0", 8083)
        .setThreadNum(2)
        .setCompression(ruvia::CompressionConfig{.enabled = true, .minBytes = 128})
        .setDocumentRoot(std::move(documentRoot))
        .run();
}
