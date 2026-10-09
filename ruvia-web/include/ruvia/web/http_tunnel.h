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
#include "ruvia/web/detail/http/http_datagram_input.h"
#include "ruvia/web/http_capsule_stream.h"
#include "ruvia/web/http_datagram_stream.h"

namespace ruvia {
namespace detail {
struct http_tunnel_access;
}

// An established CONNECT byte stream. The session owns this facade and its
// allocator; received chunks own their storage until destruction and must be
// destroyed before that session's memory owner retires.
class http_tunnel final {
public:
    http_tunnel(const http_tunnel&) = delete;
    http_tunnel& operator=(const http_tunnel&) = delete;

    // One outstanding read and one outstanding output operation are allowed.
    // A cold operation also reserves its lane until it is awaited or discarded.
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
    [[nodiscard]] http_datagram_stream datagrams(http_datagram_config config = {}) &;
    http_datagram_stream datagrams(http_datagram_config = {}) && = delete;
    [[nodiscard]] http_capsule_stream capsules(http_capsule_config config = {}) &;
    http_capsule_stream capsules(http_capsule_config = {}) && = delete;

private:
    friend struct detail::http_tunnel_access;
    friend class detail::http_capsule_stream_state;
    friend class http_capsule_stream;
    using read_type = task<std::optional<std::pmr::string>> (*)(void*);
    using write_type = task<void> (*)(void*, std::string_view);
    using finish_type = task<void> (*)(void*);
    using abort_type = void (*)(void*) noexcept;
    http_tunnel(std::pmr::memory_resource& resource, const worker_handle& worker_value,
        void* target, read_type read, write_type write, finish_type finish_value, abort_type abort) noexcept;
    void require_active() const;
    using read_datagram_input_type = task<std::optional<detail::http_datagram_input>> (*)(void*);
    using send_datagram_type = void (*)(void*, std::span<const std::byte>);
    using datagram_config_type = http_datagram_session_config (*)(void*);
    [[nodiscard]] scoped_operation<std::optional<detail::http_datagram_input>> read_datagram_input();
    [[nodiscard]] scoped_operation<void> send_datagram(std::string_view bytes);
    read_datagram_input_type read_datagram_input_{};
    send_datagram_type send_datagram_{};
    datagram_config_type datagram_config_{};
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
