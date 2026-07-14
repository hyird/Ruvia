#include "ruvia/http/detail/server/HttpResponseHead.h"

#include <charconv>
#include <cstring>
#include <stdexcept>

#include "ruvia/http/detail/server/HttpDateCache.h"
#include "ruvia/http/detail/HttpResponseHeaderAccess.h"
#include "ruvia/http/detail/HttpResponseHeaderState.h"
#include "ruvia/http/HttpStatus.h"

namespace ruvia::detail {

namespace {

struct ResponseHeadFlags {
    HttpProtocolVersion protocolVersion{HttpProtocolVersion::kHttp11};
    bool emitChunkedTransferEncoding{false};
    bool autoContentLengthOwnedByWriter{false};
    bool explicitContentLengthAllowed{false};
    std::uint64_t canonicalContentLength{0};
};

inline constexpr std::string_view kChunkedTransferEncodingHeader =
    "Transfer-Encoding: chunked\r\n";

// Unchecked sink writing through a raw cursor; the caller guarantees capacity
// via ResponseHeadBuffer::stackCursor. Constant-size appends inline to stores.
struct RawHeadSink {
    char* out;

    void append(std::string_view value) noexcept {
        // A default-constructed empty string_view may carry a null data pointer.
        // libc annotates memcpy arguments as nonnull even when the byte count is
        // zero, so avoid passing that representation across the C boundary.
        if (value.empty()) {
            return;
        }
        std::memcpy(out, value.data(), value.size());
        out += value.size();
    }

    void append(char value) noexcept {
        *out++ = value;
    }

    void appendUnsigned(std::uint64_t value) noexcept {
        out = std::to_chars(out, out + 20, value).ptr;
    }
};

template <typename Sink>
void emitResponseHead(
    const HttpResponse& response,
    Sink& sink,
    std::uint16_t responseStatus,
    std::string_view reasonPhrase,
    std::string_view dateHeader,
    ResponseHeadFlags flags) {
    sink.append(
        flags.protocolVersion == HttpProtocolVersion::kHttp10
            ? std::string_view("HTTP/1.0 ")
            : std::string_view("HTTP/1.1 "));
    sink.appendUnsigned(responseStatus);
    // RFC 9112 requires this SP even when the optional reason phrase is empty.
    sink.append(' ');
    sink.append(reasonPhrase);
    sink.append(std::string_view("\r\n"));

    for (const auto& header : response.headers()) {
        const auto knownBit = responseHeaderKnownBit(header);
        // HTTP/1 framing belongs exclusively to Http1ResponseHeadPlan. A handler
        // cannot override canonical chunked framing, invent Transfer-Encoding on
        // an HTTP/1.0 response, or attach Content-Length to a body-open
        // close-delimited stream.
        if (knownBit == kResponseHeaderTransferEncoding ||
            (knownBit == kResponseHeaderContentLength &&
             !flags.explicitContentLengthAllowed)) {
            continue;
        }
        sink.append(header.name());
        sink.append(std::string_view(": "));
        sink.append(header.value());
        sink.append(std::string_view("\r\n"));
    }

    const auto knownBits = responseKnownHeaderBits(response);
    if ((knownBits & kResponseHeaderDate) == 0) {
        sink.append(dateHeader);
    }
    if (flags.emitChunkedTransferEncoding) {
        sink.append(kChunkedTransferEncodingHeader);
    }
    if (flags.autoContentLengthOwnedByWriter) {
        sink.append(std::string_view("Content-Length: "));
        sink.appendUnsigned(flags.canonicalContentLength);
        sink.append(std::string_view("\r\n"));
    }
    sink.append(std::string_view("\r\n"));
}

}  // namespace

void appendResponseHead(
    const HttpResponse& response,
    ResponseHeadBuffer& head,
    const Http1ResponseHeadPlan& plan) {
    const auto bodyPlan = plan.bodyPlan();
    if (response.status() != bodyPlan.responseStatus()) {
        throw std::invalid_argument(
            "HTTP/1 response plan status does not match response");
    }
    const auto* buffered = plan.buffered();
    if (buffered != nullptr &&
        buffered->contentLength() !=
            bodyPlan.bufferedRepresentationLength(response)) {
        throw std::invalid_argument(
            "HTTP/1 response plan representation does not match response");
    }
    const auto responseStatus = bodyPlan.responseStatus();
    const auto policy = bodyPlan.policy();
    const bool emitChunkedTransferEncoding =
        plan.chunkedStream() != nullptr && policy.transferEncodingAllowed();
    const bool autoContentLengthOwnedByWriter =
        policy.autoContentLengthAllowed() &&
        !emitChunkedTransferEncoding &&
        (plan.buffered() != nullptr || !policy.bodyAllowed());
    const bool explicitContentLengthAllowed =
        policy.explicitContentLengthAllowed() &&
        !emitChunkedTransferEncoding &&
        !autoContentLengthOwnedByWriter &&
        (plan.closeDelimitedStream() == nullptr || bodyPlan.bodySuppressed());
    const ResponseHeadFlags flags{
        .protocolVersion = plan.protocolVersion(),
        .emitChunkedTransferEncoding = emitChunkedTransferEncoding,
        .autoContentLengthOwnedByWriter = autoContentLengthOwnedByWriter,
        .explicitContentLengthAllowed = explicitContentLengthAllowed,
        // Buffered HEAD metadata retains the selected representation length.
        // A status-level no-content policy that still owns framing (205) is
        // canonicalized to zero for both buffered and streaming heads.
        .canonicalContentLength =
            buffered != nullptr && policy.bodyAllowed()
                ? buffered->contentLength()
                : std::uint64_t{0}};

    const auto knownBits = responseKnownHeaderBits(response);

    const auto reasonPhrase = httpReasonPhrase(responseStatus);
    const auto dateHeader = cachedDateHeader();

    // Upper bound on emitted bytes (filtered headers are counted anyway; the
    // numeric slots use the 20-digit std::uint64_t worst case). The raw stack
    // sink below emits without per-append bounds checks, so this must never
    // undercount the actual output.
    std::size_t bound = 9 + 20 + 1 + reasonPhrase.size() + 2;
    for (const auto& header : response.headers()) {
        bound += header.name().size() + header.value().size() + 4;
    }
    if ((knownBits & kResponseHeaderDate) == 0) {
        bound += dateHeader.size();
    }
    if (emitChunkedTransferEncoding) {
        bound += kChunkedTransferEncodingHeader.size();
    }
    if (autoContentLengthOwnedByWriter) {
        bound += 16 + 20 + 2;
    }
    bound += 2;

    if (char* cursor = head.stackCursor(bound); cursor != nullptr) {
        RawHeadSink sink{cursor};
        emitResponseHead(
            response,
            sink,
            responseStatus,
            reasonPhrase,
            dateHeader,
            flags);
        head.commitStack(sink.out);
        return;
    }
    head.reserveAdditional(bound);
    emitResponseHead(
        response,
        head,
        responseStatus,
        reasonPhrase,
        dateHeader,
        flags);
}

}  // namespace ruvia::detail
