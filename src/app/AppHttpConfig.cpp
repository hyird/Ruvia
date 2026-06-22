#include "AppConfigMutation.h"
#include "ruvia/detail/NativePath.h"
#include "../http/HttpCorsConfigValidation.h"

#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ruvia {
namespace {

template <typename NativeChar>
void assignTlsFileNameFromNative(std::pmr::string& output, std::basic_string_view<NativeChar> native) {
    if constexpr (std::is_same_v<NativeChar, char>) {
        output.assign(native.data(), native.size());
    } else {
        const auto name = std::filesystem::path(native.begin(), native.end()).string();
        output.assign(name.data(), name.size());
    }
}

void assignTlsFileName(std::pmr::string& output, const std::filesystem::path& path) {
    assignTlsFileNameFromNative(output, detail::nativePathView(path));
}

}  // namespace

App& App::useTls(TlsConfig config) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot configure TLS while app is running",
        [&config](detail::AppState& state) {
            if (config.certificateChainFile.empty()) {
                throw std::invalid_argument("TLS certificate chain file must not be empty");
            }
            if (config.privateKeyFile.empty()) {
                throw std::invalid_argument("TLS private key file must not be empty");
            }

            state.options.tls.enabled = true;
            assignTlsFileName(state.options.tls.certificateChainFile, config.certificateChainFile);
            assignTlsFileName(state.options.tls.privateKeyFile, config.privateKeyFile);
            state.options.tls.privateKeyPassword = std::move(config.privateKeyPassword);
            assignTlsFileName(state.options.tls.verifyFile, config.verifyFile);
        });
}

App& App::setCompression(CompressionConfig config) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change compression config while app is running",
        [config](detail::AppState& state) {
            state.options.compression.enabled = config.enabled;
            state.options.compression.minBytes = config.minBytes;
        });
}

App& App::setCors(CorsConfig config) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change CORS config while app is running",
        [&config](detail::AppState& state) {
            detail::validateCorsConfig(config);

            state.options.cors.enabled = config.enabled;
            state.options.cors.allowOrigin = std::move(config.allowOrigin);
            state.options.cors.allowHeaders = std::move(config.allowHeaders);
            state.options.cors.exposeHeaders = std::move(config.exposeHeaders);
            state.options.cors.maxAge = config.maxAge;
            state.options.cors.allowCredentials = config.allowCredentials;
        });
}

App& App::setDocumentRoot(DocumentRootConfig config) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change document root while app is running",
        [&config](detail::AppState& state) {
            if (config.root.empty()) {
                throw std::invalid_argument("document root must not be empty");
            }
            if (config.staticOptions.indexFile.empty()) {
                config.staticOptions.indexFile = "index.html";
            }

            auto& documentRootConfig =
                state.documentRootConfig.emplace(ProcessMemory::instance().upstreamResource());
            detail::assignNativePath(documentRootConfig.root, config.root);
            documentRootConfig.staticOptions = std::move(config.staticOptions);
        });
}

App& App::setDocumentRoot(const std::filesystem::path& root) {
    DocumentRootConfig config;
    config.root = root;
    return setDocumentRoot(std::move(config));
}

App& App::setErrorHandler(HttpErrorHandler handler) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change error handler while app is running",
        [handler](detail::AppState& state) {
            state.errorHandler = handler;
        });
}

}  // namespace ruvia
