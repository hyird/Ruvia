#include "ruvia/web/detail/app/AppConfigMutation.h"
#include "ruvia/core/detail/NativePath.h"
#include "ruvia/web/detail/http/HttpCorsConfigValidation.h"

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

App& App::addTlsCertificate(std::string_view host, TlsConfig config) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot configure TLS while app is running",
        [host, &config](detail::AppState& state) {
            if (host.empty()) {
                throw std::invalid_argument("SNI host must not be empty");
            }
            if (config.certificateChainFile.empty()) {
                throw std::invalid_argument("TLS certificate chain file must not be empty");
            }
            if (config.privateKeyFile.empty()) {
                throw std::invalid_argument("TLS private key file must not be empty");
            }

            detail::HttpServerOptions::Tls::SniCertificate cert;
            cert.host.assign(host.data(), host.size());
            assignTlsFileName(cert.certificateChainFile, config.certificateChainFile);
            assignTlsFileName(cert.privateKeyFile, config.privateKeyFile);
            cert.privateKeyPassword = std::move(config.privateKeyPassword);
            state.options.tls.sniCertificates.push_back(std::move(cert));
        });
}

App& App::setCompression(std::optional<CompressionConfig> config) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change compression config while app is running",
        [&config](detail::AppState& state) {
            state.options.compression = std::move(config);
        });
}

App& App::setCors(std::optional<CorsConfig> config) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change CORS config while app is running",
        [&config](detail::AppState& state) {
            if (config.has_value()) {
                detail::validateCorsConfig(*config);
            }
            state.options.cors = std::move(config);
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
                state.documentRootConfig.emplace(detail::appResource());
            detail::assignNativePath(documentRootConfig.root, config.root);
            documentRootConfig.staticOptions = std::move(config.staticOptions);
        });
}

App& App::onError(HttpErrorHandler handler) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change error handler while app is running",
        [handler](detail::AppState& state) {
            state.errorHandler = handler;
        });
}

App& App::notFound(HttpNotFoundHandler handler) {
    return detail::mutateStoppedApp(
        *this,
        *state_,
        "cannot change not found handler while app is running",
        [handler](detail::AppState& state) {
            state.notFoundHandler = handler;
        });
}

}  // namespace ruvia
