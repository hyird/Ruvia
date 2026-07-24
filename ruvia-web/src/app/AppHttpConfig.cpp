#include "ruvia/web/ServerConfig.h"
#include "ruvia/web/detail/app/AppConfigMutation.h"
#include "ruvia/core/detail/util/NativePath.h"

#include <type_traits>
#include <utility>

namespace ruvia {

App& App::setCompression(std::optional<CompressionConfig> config) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change compression config while app is running", [&config](detail::AppState& state) { state.options.compression = std::move(config); });
}

App& App::setCors(std::optional<CorsConfig> config) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change CORS config while app is running", [&config](detail::AppState& state) { state.options.cors = std::move(config); });
}

App& App::setDocumentRoot(DocumentRootConfig config) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change document root while app is running", [&config](detail::AppState& state) {
        if (config.root.empty()) {
            throw std::invalid_argument("document root must not be empty");
        }
        if (config.staticOptions.indexFile.empty()) {
            config.staticOptions.indexFile = "index.html";
        }

        auto& documentRootConfig = state.documentRootConfig.emplace(detail::appResource());
        detail::assignNativePath(documentRootConfig.root, config.root);
        documentRootConfig.staticOptions = std::move(config.staticOptions);
    });
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
    return detail::mutateStoppedApp(*this, *state_, "cannot add app middleware while app is running", [descriptor](detail::AppState& state) { state.globalMiddlewares.push_back(descriptor); });
}

App& App::setBlockingPool(std::optional<BlockingPoolOptions> options) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change the blocking pool while app is running", [&options](detail::AppState& state) { state.blockingPool = options; });
}

App& App::useWorkerStateDefinition(detail::WorkerStateDefinition definition) {
    return detail::mutateStoppedApp(*this, *state_, "cannot register worker state while app is running", [&definition](detail::AppState& state) {
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
    return detail::mutateStoppedApp(*this, *state_, "cannot change error handler while app is running", [handler](detail::AppState& state) { state.errorHandler = handler; });
}

App& App::notFound(HttpNotFoundHandler handler) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change not found handler while app is running", [handler](detail::AppState& state) { state.notFoundHandler = handler; });
}

namespace {

// Shared registration shape for both prefix-scoped fallback kinds: prefixes
// are normalized ("/api/" == "/api") and re-registering one replaces its
// handler, mirroring how the prefix-less setters overwrite.
template <typename Handler>
void upsertPrefixHandler(std::pmr::vector<std::pair<std::pmr::string, Handler>>& handlers, std::string_view prefix, Handler handler) {
    if (handler == nullptr) {
        throw std::invalid_argument("fallback handler must not be null");
    }
    if (prefix.empty() || prefix.front() != '/') {
        throw std::invalid_argument("fallback prefix must start with '/'");
    }
    while (prefix.size() > 1 && prefix.back() == '/') {
        prefix.remove_suffix(1);
    }
    for (auto& [existingPrefix, existingHandler] : handlers) {
        if (std::string_view(existingPrefix) == prefix) {
            existingHandler = handler;
            return;
        }
    }
    handlers.emplace_back(std::pmr::string(prefix, detail::appResource()), handler);
}

}  // namespace

App& App::onError(std::string_view prefix, HttpErrorHandler handler) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change error handler while app is running", [prefix, handler](detail::AppState& state) { upsertPrefixHandler(state.prefixErrorHandlers, prefix, handler); });
}

App& App::notFound(std::string_view prefix, HttpNotFoundHandler handler) {
    return detail::mutateStoppedApp(*this, *state_, "cannot change not found handler while app is running", [prefix, handler](detail::AppState& state) { upsertPrefixHandler(state.prefixNotFoundHandlers, prefix, handler); });
}

}  // namespace ruvia
