#include "ruvia/http/detail/websocket/WsConnection.h"
#include "ruvia/http/detail/websocket/WsEvent.h"
#include "ruvia/http/ProtocolByteLimit.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

namespace {

void exerciseEvent(const ruvia::detail::WsEvent& event) noexcept {
    switch (event.kind()) {
        case ruvia::detail::WsEventKind::kMessage:
            if (const auto* message = event.message()) {
                (void)message->opcode();
                (void)message->payload();
            }
            break;
        case ruvia::detail::WsEventKind::kPing:
            if (const auto* ping = event.ping()) {
                (void)ping->payload();
            }
            break;
        case ruvia::detail::WsEventKind::kPong:
            if (const auto* pong = event.pong()) {
                (void)pong->payload();
            }
            break;
        case ruvia::detail::WsEventKind::kClose:
            if (const auto* close = event.close()) {
                (void)close->closeCode();
                (void)close->reason();
            }
            break;
        case ruvia::detail::WsEventKind::kProtocolError:
            if (const auto* error = event.protocolError()) {
                (void)error->closeCode();
            }
            break;
        case ruvia::detail::WsEventKind::kTransportEnd:
            (void)event.transportEnd();
            break;
    }
}

// Drives one server-side WebSocket core over arbitrary inbound bytes. The core
// unmasks and parses in place; poll() emits at most one event or nullopt when it
// needs more input. Since the input is fixed and finite and every event either
// consumes buffered bytes or is terminal, the loop terminates; an iteration cap
// backstops it. A bounded message limit turns a decompression/reassembly bomb
// into a protocol error instead of an allocation blowup.
void drive(
    std::string_view bytes,
    ruvia::detail::WebSocketDeflateNegotiation deflate,
    std::pmr::memory_resource* resource) {
    std::pmr::string input(bytes, resource);
    ruvia::detail::WsConnection connection(
        input,
        ruvia::ProtocolByteLimit::limited(1U << 20),
        deflate);

    const std::size_t pollBudget = bytes.size() + 64;
    for (std::size_t iteration = 0; iteration < pollBudget; ++iteration) {
        auto event = connection.poll();

        // Keep pending output bounded so a stream of ping/close replies cannot
        // accumulate, and exercise the output-plan accessors on every step.
        const auto plan = connection.outputPlan();
        if (!plan.bytes().empty()) {
            connection.consumeOutput(plan.bytes().size());
        }
        (void)connection.livenessMode();

        if (!event.has_value()) {
            break;
        }
        const bool transportEnded =
            event->transportEnd() != nullptr;
        exerciseEvent(*event);
        if (transportEnded) {
            break;
        }
    }
}

}  // namespace

// Fuzzes the server-side WebSocket frame decoder (RFC 6455) with permessage-
// deflate both disabled and negotiated (RFC 7692). Every borrowed payload/reason
// view must stay in-bounds and the core must terminate for arbitrary bytes,
// including unmasked frames, oversized length fields, fragmented sequences, and
// malformed control frames.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto bytes = std::string_view(
        reinterpret_cast<const char*>(data),
        size);

    std::pmr::monotonic_buffer_resource plainResource;
    drive(bytes, ruvia::detail::WebSocketDeflateNegotiation::kDisabled, &plainResource);

    std::pmr::monotonic_buffer_resource deflateResource;
    drive(bytes, ruvia::detail::WebSocketDeflateNegotiation::kAccepted, &deflateResource);

    return 0;
}
