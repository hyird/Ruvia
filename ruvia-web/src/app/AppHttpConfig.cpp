#include <memory_resource>
#include <type_traits>
#include <utility>

#include "ruvia/core/ConfigValidation.h"
#include "ruvia/core/NativePath.h"
#include "ruvia/web/ServerConfig.h"
#include "ruvia/web/detail/app/AppConfigMutation.h"
#include "ruvia/web/detail/http/static/StaticFileTypes.h"
#include "ruvia/web/detail/http/static/StaticRootOptionsValidation.h"

namespace ruvia {

namespace {

[[nodiscard]] detail::StaticRootPrecompressionOptions makeStaticRootPrecompressionOptions(
    const DocumentRootConfig& config) {
    ruvia::ensurePositiveSize(config.precompressMinBytes,
        "document root precompression minimum size must be greater than zero");
    if (config.precompressMaxBytes < config.precompressMinBytes) {
        throw std::invalid_argument(
            "document root precompression maximum size must not be smaller than the minimum size");
    }
    return detail::StaticRootPrecompressionOptions{
        .gzip = config.precompressGzip,
        .brotli = config.precompressBrotli,
        .zstd = config.precompressZstd,
        .minBytes = config.precompressMinBytes,
        .maxBytes = config.precompressMaxBytes,
    };
}

}  // namespace

App& App::compression(CompressionConfig config) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change compression config while app is running",
        [config = std::move(config)](detail::AppState& state) mutable {
            ruvia::ensurePositiveSize(
                config.minBytes, "compression minimum size must be greater than zero");
            if (config.syncBytes < config.minBytes) {
                throw std::invalid_argument(
                    "compression synchronous size must not be smaller than the minimum size");
            }
            if (config.maxBytes < config.syncBytes) {
                throw std::invalid_argument(
                    "compression maximum size must not be smaller than the synchronous size");
            }
            state.options.compression = std::move(config);
        });
}

App& App::compression(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change compression config while app is running",
        [](detail::AppState& state) { state.options.compression.reset(); });
}

App& App::cors(CorsConfig config) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change CORS config while app is running", [&config](detail::AppState& state) {
            state.options.cors = detail::makeCorsOptions(config, detail::appResource());
        });
}

App& App::cors(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change CORS config while app is running",
        [](detail::AppState& state) { state.options.cors.reset(); });
}

App& App::documentRoot(DocumentRootConfig config) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change document root while app is running", [&config](detail::AppState& state) {
            if (config.root.empty()) {
                throw std::invalid_argument("document root must not be empty");
            }
            ruvia::ensurePositiveDuration(config.runtime.refreshInterval,
                "document root refresh interval must be greater than zero");
            if (config.staticOptions.indexFile.empty()) {
                config.staticOptions.indexFile = "index.html";
            }
            detail::validateStaticRootOptions(config.staticOptions);
            const auto precompression = makeStaticRootPrecompressionOptions(config);

            detail::AppDocumentRootConfig replacement(
                detail::appResource(), detail::storeValidatedStaticRootConfig(
                                           config.staticOptions, detail::appResource()));
            ruvia::assignNativePath(replacement.root, config.root);
            replacement.runtime = config.runtime;
            replacement.precompression = precompression;

            state.documentRootConfig = std::move(replacement);
        });
}

App& App::documentRoot(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change document root while app is running",
        [](detail::AppState& state) { state.documentRootConfig.reset(); });
}

App& App::useMiddleware(detail::ControllerMiddlewareDescriptor descriptor) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot add app middleware while app is running",
        [descriptor](detail::AppState& state) { state.configuration.add_middleware(descriptor); });
}

App& App::blockingPool(BlockingPoolOptions config) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change the blocking pool while app is running",
        [&config](detail::AppState& state) { state.blockingPool = config; });
}

App& App::blockingPool(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change the blocking pool while app is running",
        [](detail::AppState& state) { state.blockingPool.reset(); });
}

App& App::useWorkerStateDefinition(detail::WorkerStateDefinition definition) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot register worker state while app is running",
        [&definition](detail::AppState& state) {
            state.configuration.add_worker_state(std::move(definition));
        });
}

App& App::onError(HttpErrorHandler handler) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change error handler while app is running",
        [handler = std::move(handler)](
            detail::AppState& state) mutable { state.configuration.on_error(std::move(handler)); });
}

App& App::onNotFound(HttpNotFoundHandler handler) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change not found handler while app is running",
        [handler = std::move(handler)](
            detail::AppState& state) mutable { state.configuration.on_not_found(std::move(handler)); });
}

App& App::onError(ScopedErrorHandlerOptions options) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change error handler while app is running",
        [options = std::move(options)](detail::AppState& state) mutable {
            state.configuration.on_error(std::move(options));
        });
}

App& App::onNotFound(ScopedNotFoundHandlerOptions options) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change not found handler while app is running",
        [options = std::move(options)](detail::AppState& state) mutable {
            state.configuration.on_not_found(std::move(options));
        });
}

}  // namespace ruvia
