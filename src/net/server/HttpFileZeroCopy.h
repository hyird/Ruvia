#pragma once

#include "../../http/HttpResponseFileBody.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"

#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>
#include <openssl/ssl.h>
#include <system_error>
#include <type_traits>

namespace ruvia::detail {

Task<void> writeFileZeroCopy(asio::ip::tcp::socket& socket, ResponseFileBody file, std::error_code& ec);

// True when kTLS send offload is active on this TLS stream, so sendfile() on the
// underlying socket emits TLS records in the kernel (zero-copy file body).
[[nodiscard]] inline bool tlsKernelSendActive(
    asio::ssl::stream<asio::ip::tcp::socket&>& stream) noexcept {
#ifdef BIO_get_ktls_send
    BIO* const wbio = SSL_get_wbio(stream.native_handle());
    return wbio != nullptr && BIO_get_ktls_send(wbio) == 1;
#else
    (void)stream;
    return false;
#endif
}

// Sends the file body with the best zero-copy path the stream allows:
//   - plain tcp::socket  -> sendfile / TransmitFile
//   - TLS stream + kTLS   -> sendfile on the underlying socket (kernel encrypts)
// Returns true when the zero-copy path handled the body (ec holds its result),
// or false when zero-copy is unavailable and the caller must fall back to the
// buffered read+write path.
template <typename Stream>
Task<bool> tryWriteFileZeroCopy(Stream& stream, ResponseFileBody file, std::error_code& ec) {
    using Raw = std::remove_cvref_t<Stream>;
    if constexpr (std::is_same_v<Raw, asio::ip::tcp::socket>) {
        co_await writeFileZeroCopy(stream, file, ec);
        co_return ec != asio::error::operation_not_supported;
    } else if constexpr (std::is_same_v<Raw, asio::ssl::stream<asio::ip::tcp::socket&>>) {
        if (tlsKernelSendActive(stream)) {
            co_await writeFileZeroCopy(stream.next_layer(), file, ec);
            co_return ec != asio::error::operation_not_supported;
        }
        co_return false;
    } else {
        co_return false;
    }
}

}  // namespace ruvia::detail
