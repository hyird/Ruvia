#include "HttpResponseHead.h"

#include <charconv>
#include <cstring>

#include "HttpDateCache.h"
#include "HttpResponseBodyAccess.h"
#include "HttpResponseHeaderAccess.h"
#include "HttpResponseHeaderState.h"
#include "ruvia/http/HttpStatus.h"

namespace ruvia::detail {

namespace {

struct ResponseHeadFlags {
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
    std::string_view statusLine,
    std::string_view dateHeader,
    ResponseHeadFlags flags) {
    if (!statusLine.empty()) {
        sink.append(statusLine);
    } else {
        sink.append(std::string_view("HTTP/1.1 "));
        sink.appendUnsigned(response.status());
        sink.append(' ');
        sink.append(response.statusText());
        sink.append(std::string_view("\r\n"));
    }

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
    if ((knownBits & kResponseHeaderServer) == 0) {
        sink.append(std::string_view("Server: ruvia\r\n"));
    }
    if ((knownBits & kResponseHeaderDate) == 0) {
        sink.append(dateHeader);
    }
    if (flags.autoContentLengthOwnedByWriter) {
        sink.append(std::string_view("Content-Length: "));
        sink.appendUnsigned(responseBodySize(response));
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
        !suppressAutoContentLength;
    const bool explicitContentLengthAllowed =
        policy.explicitContentLengthAllowed() &&
        !hasTransferEncoding &&
        !autoContentLengthOwnedByWriter;
    const ResponseHeadFlags flags{
        .transferEncodingAllowed = transferEncodingAllowed,
        .autoContentLengthOwnedByWriter = autoContentLengthOwnedByWriter,
        .explicitContentLengthAllowed = explicitContentLengthAllowed,
        .filterForbiddenBodyHeaders = responseHasForbiddenBodyFramingHeader(
            knownBits,
            explicitContentLengthAllowed,
            transferEncodingAllowed)};

    const auto statusLine = httpCachedStatusLine(response.status(), response.statusText());
    const auto dateHeader = cachedDateHeader();

    // Upper bound on emitted bytes (filtered headers are counted anyway; the
    // numeric slots use the 20-digit std::uint64_t worst case). The raw stack
    // sink below emits without per-append bounds checks, so this must never
    // undercount the actual output.
    std::size_t bound = statusLine.empty()
        ? 9 + 20 + 1 + response.statusText().size() + 2
        : statusLine.size();
    for (const auto& header : response.headers()) {
        bound += header.name().size() + header.value().size() + 4;
    }
    if ((knownBits & kResponseHeaderServer) == 0) {
        bound += 15;
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
        emitResponseHead(response, sink, statusLine, dateHeader, flags);
        head.commitStack(sink.out);
        return;
    }
    head.reserveAdditional(bound);
    emitResponseHead(response, head, statusLine, dateHeader, flags);
}

}  // namespace ruvia::detail
