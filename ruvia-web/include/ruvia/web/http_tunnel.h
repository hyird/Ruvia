#pragma once

#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/web/http_capsule_stream.h"
#include "ruvia/web/http_datagram_stream.h"

namespace ruvia {
namespace detail {
template <typename stream_type>
class http_tunnel_session;
}

// An established CONNECT byte stream. The session owns this facade and its
// allocator. Received chunks own their storage until destruction; it counts
// against the connection's and the worker's inbound buffer budgets
// (server_config::max_inbound_buffer_bytes_per_connection_ / _per_worker_), so
// destroy chunks before the tunnel handler returns. A read that would exceed
// a budget throws std::bad_alloc.
class http_tunnel final {
public:
    http_tunnel(const http_tunnel&) = delete;
    http_tunnel& operator=(const http_tunnel&) = delete;

    // One running read and one running output operation are allowed.
    // An operation occupies its lane when started; unstarted operations may coexist.
    [[nodiscard]] scoped_operation<std::optional<std::pmr::string>> read() &;
    scoped_operation<std::optional<std::pmr::string>> read() && = delete;
    [[nodiscard]] scoped_operation<void> write(std::string_view bytes) &;
    scoped_operation<void> write(std::string_view) && = delete;
    template <typename text_type>
        requires(!std::same_as<std::remove_cvref_t<text_type>, std::pmr::string> &&
                 std::constructible_from<std::string_view, text_type &&>)
    [[nodiscard]] scoped_operation<void> write(text_type&& bytes_value) & {
        return write(std::string_view(std::forward<text_type>(bytes_value)));
    }
    template <typename text_type>
        requires(!std::same_as<std::remove_cvref_t<text_type>, std::pmr::string> &&
                    std::constructible_from<std::string_view, text_type &&>)
    scoped_operation<void> write(text_type&&) && = delete;
    [[nodiscard]] scoped_operation<void> write(std::span<const std::byte> bytes) &;
    scoped_operation<void> write(std::span<const std::byte>) && = delete;
    [[nodiscard]] scoped_operation<void> write(std::pmr::string&& bytes) &;
    scoped_operation<void> write(std::pmr::string&&) && = delete;

    // Half-close the send direction; reading remains available until peer EOF.
    [[nodiscard]] scoped_operation<void> finish() &;
    scoped_operation<void> finish() && = delete;
    void abort() noexcept;
    // Borrow the established route tunnel for HTTP Datagrams. The route protocol
    // owns negotiation; use only in its negotiated datagram handler, exclusively
    // with this tunnel. Capsule Protocol limits apply to encoded datagrams.
    [[nodiscard]] http_datagram_stream datagrams(http_capsule_config config = {}) &;
    http_datagram_stream datagrams(http_capsule_config = {}) && = delete;
    [[nodiscard]] http_capsule_stream capsules(http_capsule_config config = {}) &;
    http_capsule_stream capsules(http_capsule_config = {}) && = delete;

private:
    template <typename stream_type>
    friend class detail::http_tunnel_session;
    friend class detail::http_capsule_stream_state;
    friend class http_capsule_stream;
    using read_type = task<std::optional<std::pmr::string>> (*)(void*);
    using write_type = task<void> (*)(void*, std::string_view);
    using finish_type = task<void> (*)(void*);
    using abort_type = void (*)(void*) noexcept;
    http_tunnel(std::pmr::memory_resource& resource, const worker_handle& worker_value,
        void* target, read_type read, write_type write, finish_type finish_value, abort_type abort) noexcept;
    void require_active() const;
    std::pmr::memory_resource& resource_;
    const worker_handle& worker_;
    void* target_;
    read_type read_;
    write_type write_;
    finish_type finish_;
    abort_type abort_;
    bool read_active_{false};
    bool write_active_{false};
    bool send_ended_{false};
    unsigned running_operations_{0};
    ::ruvia::operation_scope operations_;
};
}  // namespace ruvia
