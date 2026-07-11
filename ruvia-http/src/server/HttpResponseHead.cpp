#include "ruvia/http/detail/server/HttpResponseHead.h"

#include <charconv>
#include <cstring>

#include "ruvia/http/detail/server/HttpDateCache.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/HttpResponseHeaderAccess.h"
#include "ruvia/http/detail/HttpResponseHeaderState.h"
#include "ruvia/http/HttpStatus.h"

namespace ruvia::detail {

namespace {

struct ResponseHeadFlags {
    bool bodyAllowed{false};
    bool transferEncodingAllowed{false};
    bool autoContentLengthOwnedByWriter{false};
    bool explicitContentLengthAllowed{false};
    bool filterForbiddenBodyHeaders{false};
};

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
    std::string_view reasonPhrase,
    std::string_view dateHeader,
    ResponseHeadFlags flags) {
    sink.append(std::string_view("HTTP/1.1 "));
    sink.appendUnsigned(response.status());
    // RFC 9112 requires this SP even when the optional reason phrase is empty.
    sink.append(' ');
    sink.append(reasonPhrase);
    sink.append(std::string_view("\r\n"));

    for (const auto& header : response.headers()) {
        if (flags.filterForbiddenBodyHeaders) {
            if (responseBodyFramingHeaderForbidden(
                    responseHeaderKnownBit(header),
                    flags.explicitContentLengthAllowed,
                    flags.transferEncodingAllowed)) {
                continue;
            }
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
    if (flags.autoContentLengthOwnedByWriter) {
        sink.append(std::string_view("Content-Length: "));
        // HEAD metadata uses the selected representation size because its status
        // still allows content. A status-level no-content policy that nevertheless
        // owns framing (currently 205) must always declare the wire content length
        // as zero, even if the application supplied a body that will be suppressed.
        sink.appendUnsigned(flags.bodyAllowed ? responseBodySize(response) : 0);
        sink.append(std::string_view("\r\n"));
    }
    sink.append(std::string_view("\r\n"));
}

}  // namespace

void appendResponseHead(
    const HttpResponse& response,
    ResponseHeadBuffer& head,
    ResponseWritePolicy policy,
    bool suppressAutoContentLength) {
    const bool transferEncodingAllowed = policy.transferEncodingAllowed() && suppressAutoContentLength;
    const auto knownBits = responseKnownHeaderBits(response);
    const bool hasTransferEncoding = transferEncodingAllowed &&
        (knownBits & kResponseHeaderTransferEncoding) != 0;
    const bool autoContentLengthOwnedByWriter =
        policy.autoContentLengthAllowed() &&
        !hasTransferEncoding &&
        (!suppressAutoContentLength || !policy.bodyAllowed());
    const bool explicitContentLengthAllowed =
        policy.explicitContentLengthAllowed() &&
        !hasTransferEncoding &&
        !autoContentLengthOwnedByWriter;
    const ResponseHeadFlags flags{
        .bodyAllowed = policy.bodyAllowed(),
        .transferEncodingAllowed = transferEncodingAllowed,
        .autoContentLengthOwnedByWriter = autoContentLengthOwnedByWriter,
        .explicitContentLengthAllowed = explicitContentLengthAllowed,
        .filterForbiddenBodyHeaders = responseHasForbiddenBodyFramingHeader(
            knownBits,
            explicitContentLengthAllowed,
            transferEncodingAllowed)};

    const auto reasonPhrase = httpReasonPhrase(response.status());
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
    if (autoContentLengthOwnedByWriter) {
        bound += 16 + 20 + 2;
    }
    bound += 2;

    if (char* cursor = head.stackCursor(bound); cursor != nullptr) {
        RawHeadSink sink{cursor};
        emitResponseHead(response, sink, reasonPhrase, dateHeader, flags);
        head.commitStack(sink.out);
        return;
    }
    head.reserveAdditional(bound);
    emitResponseHead(response, head, reasonPhrase, dateHeader, flags);
}

}  // namespace ruvia::detail
