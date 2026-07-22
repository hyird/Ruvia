#include "ruvia/edge/detail/server/TlsContext.h"

#include <asio/buffer.hpp>

#include <openssl/ssl.h>  // ALPN selection

namespace ruvia::edge {

namespace {

// ALPN wire list the edge advertises, in server-preference order: h2, http/1.1.
constexpr unsigned char kAlpnProtocols[] = {
    2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

extern "C" inline int edgeAlpnSelect(
    SSL* /*ssl*/, const unsigned char** out, unsigned char* outlen,
    const unsigned char* in, unsigned int inlen, void* /*arg*/) {
    if (SSL_select_next_proto(const_cast<unsigned char**>(out), outlen, kAlpnProtocols,
                              sizeof(kAlpnProtocols), in, inlen) != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_NOACK;  // no shared protocol: fall back to HTTP/1.1
    }
    return SSL_TLSEXT_ERR_OK;
}

}  // namespace

std::shared_ptr<asio::ssl::context> makeTlsContext(const EdgeTlsConfig& config) {
    auto context = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_server);
    context->use_certificate_chain(asio::buffer(config.certificateChainPem));
    context->use_private_key(
        asio::buffer(config.privateKeyPem), asio::ssl::context::pem);
    SSL_CTX_set_alpn_select_cb(context->native_handle(), &edgeAlpnSelect, nullptr);
    return context;
}

}  // namespace ruvia::edge
