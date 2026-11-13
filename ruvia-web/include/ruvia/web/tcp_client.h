#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/port_number.h"
#include "ruvia/web/http_client_types.h"
#include "ruvia/web/tls_config.h"

namespace ruvia {

class event_loop;
namespace detail {
class tcp_client_state;
}

struct tcp_client_config final {
    // An unbracketed IP literal or ASCII DNS name. DNS requires a blocking pool
    // on the bound event loop. Configuration is owned before construction returns.
    std::string host_;
    port_number port_{};
    client_tls_config tls_{};
    client_tcp_config tcp_{};
    // RFC 7301 opaque protocol names, in preference order: 1..255 bytes each,
    // at most 65535 encoded bytes in total. Non-empty offers require TLS.
    std::vector<std::string> alpn_;
    std::optional<timeout_duration> connect_timeout_{std::chrono::seconds(30)};
    std::optional<timeout_duration> read_timeout_{};
    std::optional<timeout_duration> write_timeout_{};
};

// Address-stable owner of one outbound byte stream, with the shared DNS,
// Happy Eyeballs and SAN-authenticating TLS/mTLS transport.
// Operations run on worker(); construction, close and destruction may run on
// another thread. Connect once; one read and one write may run concurrently,
// but overlapping operations in the same direction throw std::logic_error.
// Timeouts begin when awaited; per-operation options may tighten the configured
// timeout. Cancellation or I/O failure closes both directions, never permitting
// reuse of a partially transmitted TLS record. I/O failures throw std::system_error
// (timed_out / operation_canceled for deadlines / cancellation).
// Cold tasks retain their input and owner; shutdown and loop stop retire cold
// read/write operations and join running ones before retiring the transport.
class tcp_client final {
public:
    tcp_client(event_loop loop, const tcp_client_config& config);
    ~tcp_client();
    tcp_client(const tcp_client&) = delete;
    tcp_client& operator=(const tcp_client&) = delete;
    tcp_client(tcp_client&&) = delete;
    tcp_client& operator=(tcp_client&&) = delete;

    [[nodiscard]] task<void> connect(operation_options options = {}) &;
    task<void> connect(operation_options = {}) && = delete;
    // Owner loop, after connect(). Empty for TCP or if the peer skipped ALPN.
    // The view borrows this connection until close, shutdown or I/O failure.
    [[nodiscard]] std::string_view alpn() const& RUVIA_LIFETIMEBOUND;
    std::string_view alpn() const&& = delete;
    // At most 4 KiB. nullopt is EOF (TCP FIN, TLS close_notify or truncation).
    // Borrowed bytes remain valid until the next read starts or shutdown joins;
    // retain this owner while using the view. EOF does not end the write lane.
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options options = {}) &;
    task<std::optional<std::span<const std::byte>>> read(operation_options = {}) && = delete;
    // Owns the bytes before returning the cold task; completes after all bytes
    // are written. Failure may have sent a prefix and always retires the stream.
    [[nodiscard]] task<void> write(std::span<const std::byte> bytes, operation_options options = {}) &;
    task<void> write(std::span<const std::byte>, operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::string_view bytes, operation_options options = {}) &;
    task<void> write(std::string_view, operation_options = {}) && = delete;
    // Exact std::string rvalues transfer storage directly without copying.
    template <typename string_type>
        requires std::same_as<string_type, std::string>
    [[nodiscard]] task<void> write(string_type&& bytes, operation_options options = {}) & {
        return write_owned(std::move(bytes), std::move(options));
    }
    template <typename string_type>
        requires std::same_as<string_type, std::string>
    task<void> write(string_type&&, operation_options = {}) && = delete;

    // Immediate whole-connection cancellation, not a send half-close. TLS uses
    // the shared transport's bounded best-effort close_notify before closing.
    void close() noexcept;
    // Closes immediately and waits for operation and socket retirement. This
    // is not a TLS graceful shutdown or a send half-close. Run on worker().
    [[nodiscard]] task<void> shutdown() &;
    task<void> shutdown() && = delete;
    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND;
    const worker_ref& worker() const&& = delete;

private:
    [[nodiscard]] task<void> write_owned(std::string bytes, operation_options options);
    std::shared_ptr<detail::tcp_client_state> state_;
};

}  // namespace ruvia
