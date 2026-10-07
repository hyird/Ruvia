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

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/web/HttpCapsuleStream.h"
#include "ruvia/web/HttpDatagramStream.h"
#include "ruvia/web/detail/http/HttpDatagramInput.h"

namespace ruvia {
namespace detail {
struct HttpTunnelAccess;
}

// An established CONNECT byte stream. The session owns this facade and its
// allocator; received chunks own their storage until destruction and must be
// destroyed before that session's memory owner retires.
class HttpTunnel final {
public:
    HttpTunnel(const HttpTunnel&) = delete;
    HttpTunnel& operator=(const HttpTunnel&) = delete;

    // One outstanding read and one outstanding output operation are allowed.
    // A cold operation also reserves its lane until it is awaited or discarded.
    [[nodiscard]] ScopedOperation<std::optional<std::pmr::string>> read() &;
    ScopedOperation<std::optional<std::pmr::string>> read() && = delete;
    [[nodiscard]] ScopedOperation<void> write(std::string_view bytes) &;
    ScopedOperation<void> write(std::string_view) && = delete;
    template <typename Text>
        requires(!std::same_as<std::remove_cvref_t<Text>, std::pmr::string> &&
                 std::constructible_from<std::string_view, Text &&>)
    [[nodiscard]] ScopedOperation<void> write(Text&& bytes) & {
        return write(std::string_view(std::forward<Text>(bytes)));
    }
    template <typename Text>
        requires(!std::same_as<std::remove_cvref_t<Text>, std::pmr::string> &&
                    std::constructible_from<std::string_view, Text &&>)
    ScopedOperation<void> write(Text&&) && = delete;
    [[nodiscard]] ScopedOperation<void> write(std::span<const std::byte> bytes) &;
    ScopedOperation<void> write(std::span<const std::byte>) && = delete;
    [[nodiscard]] ScopedOperation<void> write(std::pmr::string&& bytes) &;
    ScopedOperation<void> write(std::pmr::string&&) && = delete;

    // Half-close the send direction; reading remains available until peer EOF.
    [[nodiscard]] ScopedOperation<void> finish() &;
    ScopedOperation<void> finish() && = delete;
    void abort() noexcept;
    [[nodiscard]] HttpDatagramStream datagrams(HttpDatagramConfig config = {}) &;
    HttpDatagramStream datagrams(HttpDatagramConfig = {}) && = delete;
    [[nodiscard]] HttpCapsuleStream capsules(HttpCapsuleConfig config = {}) &;
    HttpCapsuleStream capsules(HttpCapsuleConfig = {}) && = delete;

private:
    friend struct detail::HttpTunnelAccess;
    friend class detail::HttpCapsuleStreamState;
    friend class HttpCapsuleStream;
    using Read = Task<std::optional<std::pmr::string>> (*)(void*);
    using Write = Task<void> (*)(void*, std::string_view);
    using Finish = Task<void> (*)(void*);
    using Abort = void (*)(void*) noexcept;
    HttpTunnel(std::pmr::memory_resource& resource, const WorkerHandle& worker,
        void* target, Read read, Write write, Finish finish, Abort abort) noexcept;
    void requireActive() const;
    using ReadDatagramInput = Task<std::optional<detail::HttpDatagramInput>> (*)(void*);
    using SendDatagram = void (*)(void*, std::span<const std::byte>);
    using DatagramConfig = HttpDatagramSessionConfig (*)(void*);
    [[nodiscard]] ScopedOperation<std::optional<detail::HttpDatagramInput>> readDatagramInput();
    [[nodiscard]] ScopedOperation<void> sendDatagram(std::string_view bytes);
    ReadDatagramInput readDatagramInput_{};
    SendDatagram sendDatagram_{};
    DatagramConfig datagramConfig_{};
    std::pmr::memory_resource& resource_;
    const WorkerHandle& worker_;
    void* target_;
    Read read_;
    Write write_;
    Finish finish_;
    Abort abort_;
    bool readActive_{false};
    bool writeActive_{false};
    bool sendEnded_{false};
    unsigned runningOperations_{0};
    ::ruvia::operation_scope operations_;
};
}  // namespace ruvia
