#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/WebSocketProtocolTypes.h"

namespace ruvia {

struct WebSocketServerProtocolOptions final {
    WebSocketCompression compression{WebSocketCompression::kDisabled};
    int compressionLevel{6};
};

// Sans-I/O WebSocket server protocol. The input buffer is borrowed and must
// outlive this object; its memory resource must also outlive this object.
// Event payload views remain valid until the next poll() or input mutation.
class WebSocketServerProtocol final {
public:
    explicit WebSocketServerProtocol(std::pmr::string& input,
        ProtocolByteLimit messageLimit = ProtocolByteLimit::unlimited(),
        WebSocketCompression compression = WebSocketCompression::kDisabled);
    WebSocketServerProtocol(std::pmr::string& input, ProtocolByteLimit messageLimit,
        WebSocketServerProtocolOptions options);
    ~WebSocketServerProtocol();

    WebSocketServerProtocol(const WebSocketServerProtocol&) = delete;
    WebSocketServerProtocol& operator=(const WebSocketServerProtocol&) = delete;
    WebSocketServerProtocol(WebSocketServerProtocol&&) = delete;
    WebSocketServerProtocol& operator=(WebSocketServerProtocol&&) = delete;

    [[nodiscard]] std::optional<WebSocketEvent> poll() &;
    std::optional<WebSocketEvent> poll() && = delete;
    [[nodiscard]] WebSocketOutputPlan outputPlan() const& noexcept;
    WebSocketOutputPlan outputPlan() const&& = delete;
    [[nodiscard]] WebSocketOutputConsumeStatus consumeOutput(std::size_t n) noexcept;
    void commitTransportEnd() noexcept;
    void notifyTransportEof() noexcept;
    [[nodiscard]] WebSocketAbortDisposition abort() noexcept;
    [[nodiscard]] WebSocketLivenessMode livenessMode() const noexcept;
    [[nodiscard]] WebSocketFrameSubmitStatus submitFrame(
        WebSocketOpcode opcode, std::string_view payload, bool compress = true);
    [[nodiscard]] WebSocketCloseSubmitStatus submitClose(
        std::uint16_t code, std::string_view reason);

private:
    struct Impl;
    struct ImplDeleter {
        void operator()(Impl* value) const noexcept;
    };
    std::unique_ptr<Impl, ImplDeleter> impl_;
};

}  // namespace ruvia
