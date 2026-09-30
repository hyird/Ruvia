#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string_view>

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/WebSocketProtocolTypes.h"
#include "ruvia/http/detail/util/BorrowedView.h"

namespace ruvia {

enum class WebSocketFeedStatus : std::uint8_t { kAccepted,
    kInactive };

struct WebSocketConnectionOptions final {
    // The resource must outlive the connection, including its address-stable
    // implementation and all protocol buffers. nullptr uses the default PMR.
    std::pmr::memory_resource* resource{nullptr};
    ProtocolByteLimit messageLimit{ProtocolByteLimit::unlimited()};
    WebSocketCompression compression{WebSocketCompression::kDisabled};
    WebSocketConnectionRole role{WebSocketConnectionRole::kServer};
    WebSocketMaskKeyGenerator maskKeyGenerator{nullptr};
    void* maskKeyContext{nullptr};
    int compressionLevel{6};
};

// Sans-I/O RFC 6455 driver for an already upgraded connection. The role fixes
// inbound mask validation and outbound masking, including automatic Pong/Close.
// Event views remain valid until the next feed() or nextEvent(). Output bytes
// remain valid until nextEvent(), submitFrame(), submitClose(), consumeOutput(),
// or destruction. feed(), EOF and abort do not invalidate an in-flight write.
class WebSocketConnection final {
public:
    explicit WebSocketConnection(WebSocketConnectionOptions options = {});
    ~WebSocketConnection();
    WebSocketConnection(const WebSocketConnection&) = delete;
    WebSocketConnection& operator=(const WebSocketConnection&) = delete;
    WebSocketConnection(WebSocketConnection&&) noexcept;
    WebSocketConnection& operator=(WebSocketConnection&&) noexcept;

    [[nodiscard]] WebSocketFeedStatus feed(std::string_view input);
    template <detail::HttpTemporaryOwningCharString Input>
    WebSocketFeedStatus feed(Input&&) = delete;
    [[nodiscard]] std::optional<WebSocketEvent> nextEvent() &;
    std::optional<WebSocketEvent> nextEvent() && = delete;
    [[nodiscard]] WebSocketOutputPlan outputPlan() const& noexcept;
    WebSocketOutputPlan outputPlan() const&& = delete;
    [[nodiscard]] WebSocketOutputConsumeStatus consumeOutput(std::size_t bytes) noexcept;
    void commitTransportEnd() noexcept;
    void notifyTransportEof() noexcept;
    [[nodiscard]] WebSocketAbortDisposition abort() noexcept;
    [[nodiscard]] WebSocketLivenessMode livenessMode() const noexcept;
    [[nodiscard]] WebSocketFrameSubmitStatus submitFrame(
        WebSocketOpcode opcode, std::string_view payload, bool compress = true);
    [[nodiscard]] WebSocketCloseSubmitStatus submitClose(
        std::uint16_t code, std::string_view reason);

private:
    class Impl;
    struct ImplDeleter {
        void operator()(Impl* value) const noexcept;
    };
    std::unique_ptr<Impl, ImplDeleter> impl_;
};

}  // namespace ruvia
