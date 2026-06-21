#include "ruvia/app/App.h"

#include <stdexcept>
#include <utility>

#include "AppConfigGuards.h"

namespace ruvia {

App& App::useTls(TlsConfig config) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot configure TLS while app is running");
    if (config.certificateChainFile.empty()) {
        throw std::invalid_argument("TLS certificate chain file must not be empty");
    }
    if (config.privateKeyFile.empty()) {
        throw std::invalid_argument("TLS private key file must not be empty");
    }

    options_.tls.enabled = true;
    options_.tls.certificateChainFile = std::move(config.certificateChainFile);
    options_.tls.privateKeyFile = std::move(config.privateKeyFile);
    options_.tls.privateKeyPassword = std::move(config.privateKeyPassword);
    options_.tls.verifyFile = std::move(config.verifyFile);
    return *this;
}

App& App::setCompression(CompressionConfig config) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change compression config while app is running");

    options_.compression.enabled = config.enabled;
    options_.compression.minBytes = config.minBytes;
    return *this;
}

App& App::setCors(CorsConfig config) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change CORS config while app is running");
    if (config.enabled && config.allowOrigin.empty()) {
        throw std::invalid_argument("CORS allowOrigin must not be empty when CORS is enabled");
    }
    detail::ensureNonNegativeDuration(config.maxAge, "CORS maxAge must not be negative");

    options_.cors.enabled = config.enabled;
    options_.cors.allowOrigin = std::move(config.allowOrigin);
    options_.cors.allowHeaders = std::move(config.allowHeaders);
    options_.cors.exposeHeaders = std::move(config.exposeHeaders);
    options_.cors.maxAge = config.maxAge;
    options_.cors.allowCredentials = config.allowCredentials;
    return *this;
}

App& App::setDocumentRoot(DocumentRootConfig config) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change document root while app is running");
    if (config.root.empty()) {
        throw std::invalid_argument("document root must not be empty");
    }
    if (config.staticOptions.indexFile.empty()) {
        config.staticOptions.indexFile = "index.html";
    }

    documentRootConfig_ = std::move(config);
    return *this;
}

App& App::setDocumentRoot(const std::filesystem::path& root) {
    DocumentRootConfig config;
    config.root = root;
    return setDocumentRoot(std::move(config));
}

App& App::setErrorHandler(HttpErrorHandler handler) {
    std::lock_guard lock(mutex_);
    detail::ensureAppNotRunning(running_, "cannot change error handler while app is running");

    errorHandler_ = handler;
    return *this;
}

}  // namespace ruvia
