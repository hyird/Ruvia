#include "ruvia/web/ServerConfig.h"
#include "ruvia/web/detail/app/AppConfigMutation.h"
#include "ruvia/core/detail/config/ConfigValidation.h"
#include "ruvia/web/detail/http/static/StaticFileTypes.h"
#include "ruvia/web/detail/http/static/StaticRootOptionsValidation.h"
#include "ruvia/web/detail/router/PrefixFallback.h"
#include "ruvia/core/detail/util/NativePath.h"

#include <memory_resource>
#include <type_traits>
#include <utility>

namespace ruvia {

namespace {

[[nodiscard]] detail::AppStaticRootOptions copyStaticRootOptionsToAppResource(
    const StaticRootOptions& source) {
    auto* const resource = detail::appResource();
    detail::AppStaticRootOptions result(resource);
    result.cacheControl = source.cacheControl;
    result.indexFile = source.indexFile;
    result.defaultContentType = source.defaultContentType;
    result.mimeTypes.reserve(source.mimeTypes.size());
    for (const auto& mime : source.mimeTypes) {
        auto& stored = result.mimeTypes.emplace_back(resource);
        stored.extension = mime.extension;
        stored.contentType = mime.contentType;
    }

    result.fileTypeKind = source.fileTypes.kind;
    if (result.fileTypeKind == StaticFileTypePolicy::Kind::kOnly) {
        result.fileTypeExtensions.reserve(source.fileTypes.extensions.size());
        for (const auto& extension : source.fileTypes.extensions) {
            result.fileTypeExtensions.push_back(std::pmr::string(extension, resource));
        }
    }
    result.rangeRequests = source.rangeRequests;
    result.responseValidators = source.responseValidators;
    result.dotfiles = source.dotfiles;
    return result;
}

[[nodiscard]] detail::StaticRootPrecompressionOptions makeStaticRootPrecompressionOptions(
    const DocumentRootConfig& config) {
    detail::ensurePositiveSize(config.precompressMinBytes,
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
            detail::ensurePositiveSize(
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
            detail::ensurePositiveDuration(config.runtime.refreshInterval,
                "document root refresh interval must be greater than zero");
            if (config.staticOptions.indexFile.empty()) {
                config.staticOptions.indexFile = "index.html";
            }
            detail::normalizeMimeTypes(config.staticOptions.mimeTypes);
            detail::normalizeFileTypes(config.staticOptions.fileTypes.extensions);
            detail::validateStaticRootOptions(config.staticOptions);

            detail::AppDocumentRootConfig replacement(detail::appResource());
            detail::assignNativePath(replacement.root, config.root);
            replacement.staticOptions = copyStaticRootOptionsToAppResource(config.staticOptions);
            replacement.runtime = config.runtime;
            replacement.precompression = makeStaticRootPrecompressionOptions(config);

            state.documentRootConfig = std::move(replacement);
        });
}

App& App::documentRoot(std::nullptr_t) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change document root while app is running",
        [](detail::AppState& state) { state.documentRootConfig.reset(); });
}

App& App::useMiddleware(detail::ControllerMiddlewareDescriptor descriptor) {
    if (!descriptor.valid() || descriptor.create() == nullptr || descriptor.destroy() == nullptr) {
        throw std::invalid_argument("app middleware must be constructible and invocable");
    }
    if (descriptor.validatedModelTypeKey() != nullptr) {
        // A validator binds one model type to one route's body/fields; running
        // it for every route would fail requests that legitimately carry no
        // such payload. Attach RUVIA_VALIDATE_* middlewares per route instead.
        throw std::invalid_argument("validator middleware binds to a route and cannot be app-wide");
    }
    return detail::mutateStoppedApp(*this, *state_,
        "cannot add app middleware while app is running",
        [descriptor](detail::AppState& state) { state.globalMiddlewares.push_back(descriptor); });
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
            for (const auto& existing : state.workerStates) {
                if (existing.typeKey() == definition.typeKey()) {
                    // Two factories for one T cannot both win; the accessor
                    // returns one instance per type. Fail loudly instead of
                    // silently last-wins.
                    throw std::invalid_argument("worker state type is already registered");
                }
            }
            state.workerStates.push_back(std::move(definition));
        });
}

App& App::onError(HttpErrorHandler handler) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change error handler while app is running",
        [handler = std::move(handler)](
            detail::AppState& state) mutable { state.errorHandler = std::move(handler); });
}

App& App::onNotFound(HttpNotFoundHandler handler) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change not found handler while app is running",
        [handler = std::move(handler)](
            detail::AppState& state) mutable { state.notFoundHandler = std::move(handler); });
}

namespace {

// Storage differs from TestApp's -- these live on the app resource -- but the
// rule that decides what is accepted is shared.
template <typename Handlers, typename Handler>
void appendPrefixHandler(Handlers& handlers, std::string_view prefix, Handler handler) {
    const auto normalized = detail::validateFallbackPrefix(handlers, prefix, handler);
    handlers.emplace_back(std::pmr::string(normalized, detail::appResource()), std::move(handler));
}

}  // namespace

App& App::onError(ScopedErrorHandlerOptions options) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change error handler while app is running",
        [options = std::move(options)](detail::AppState& state) mutable {
            appendPrefixHandler(
                state.prefixErrorHandlers, options.prefix, std::move(options.handler));
        });
}

App& App::onNotFound(ScopedNotFoundHandlerOptions options) {
    return detail::mutateStoppedApp(*this, *state_,
        "cannot change not found handler while app is running",
        [options = std::move(options)](detail::AppState& state) mutable {
            appendPrefixHandler(
                state.prefixNotFoundHandlers, options.prefix, std::move(options.handler));
        });
}

}  // namespace ruvia
