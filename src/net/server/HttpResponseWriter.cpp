#include "HttpResponseWriter.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <string>

#include "HttpDateCache.h"
#include "ruvia/http/HttpStatus.h"

namespace ruvia::detail {

void ResponseHeadBuffer::reset() noexcept {
    if (heap_.capacity() > kResponseHeadRetainedHeapBytes) {
        std::pmr::string replacement(heap_.get_allocator());
        heap_.swap(replacement);
    } else {
        heap_.clear();
    }
    used_ = 0;
    overflowed_ = false;
}

void ResponseHeadBuffer::append(std::string_view value) {
    if (!overflowed_ && value.size() <= stack_.size() - used_) {
        std::memcpy(stack_.data() + used_, value.data(), value.size());
        used_ += value.size();
        return;
    }
    if (!overflowed_) {
        heap_.reserve(std::max(stack_.size() * 2, used_ + value.size()));
        heap_.assign(stack_.data(), used_);
        overflowed_ = true;
    }
    heap_.append(value);
}

void ResponseHeadBuffer::append(char value) {
    if (!overflowed_ && used_ < stack_.size()) {
        stack_[used_++] = value;
        return;
    }
    if (!overflowed_) {
        heap_.reserve(stack_.size() * 2);
        heap_.assign(stack_.data(), used_);
        overflowed_ = true;
    }
    heap_.push_back(value);
}

void ResponseHeadBuffer::appendUnsigned(std::uint64_t value) {
    std::array<char, 32> buffer;
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec == std::errc{}) {
        append(std::string_view(buffer.data(), static_cast<std::size_t>(ptr - buffer.data())));
    }
}

void ResponseHeadBuffer::reserveAdditional(std::size_t size) {
    if (overflowed_) {
        heap_.reserve(heap_.size() + size);
        return;
    }
    heap_.reserve(used_ + size);
    heap_.assign(stack_.data(), used_);
    overflowed_ = true;
}

std::string_view ResponseHeadBuffer::view() const noexcept { return overflowed_ ? std::string_view(heap_) : std::string_view(stack_.data(), used_); }

bool ResponseHeadBuffer::canAppendOnStack(std::size_t size) const noexcept { return !overflowed_ && size <= stack_.size() - used_; }

ResponseWritePolicy responseWritePolicy(std::uint16_t statusCode) noexcept {
    if (statusCode >= 100 && statusCode < 200) {
        return ResponseWritePolicy{
            .bodyAllowed = false,
            .autoContentLengthAllowed = false,
            .explicitContentLengthAllowed = false,
            .transferEncodingAllowed = false};
    }
    if (statusCode == 204 || statusCode == 205) {
        return ResponseWritePolicy{
            .bodyAllowed = false,
            .autoContentLengthAllowed = false,
            .explicitContentLengthAllowed = false,
            .transferEncodingAllowed = false};
    }
    if (statusCode == 304) {
        return ResponseWritePolicy{
            .bodyAllowed = false,
            .autoContentLengthAllowed = false,
            .explicitContentLengthAllowed = true,
            .transferEncodingAllowed = false};
    }
    return {};
}

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
        sink.appendUnsigned(response.statusCode());
        sink.append(' ');
        sink.append(response.statusText());
        sink.append(std::string_view("\r\n"));
    }

    for (const auto& header : response.headers()) {
        if (flags.filterForbiddenBodyHeaders) {
            if (responseBodyFramingHeaderForbidden(
                    header.knownBit,
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

    const auto knownBits = response.knownHeaderBits();
    if ((knownBits & HttpResponse::kKnownHeaderServer) == 0) {
        sink.append(std::string_view("Server: ruvia\r\n"));
    }
    if ((knownBits & HttpResponse::kKnownHeaderDate) == 0) {
        sink.append(dateHeader);
    }
    if (flags.autoContentLengthOwnedByWriter) {
        sink.append(std::string_view("Content-Length: "));
        sink.appendUnsigned(response.bodySize());
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
    const bool transferEncodingAllowed = policy.transferEncodingAllowed && suppressAutoContentLength;
    const auto knownBits = response.knownHeaderBits();
    const bool hasTransferEncoding = transferEncodingAllowed &&
        (knownBits & HttpResponse::kKnownHeaderTransferEncoding) != 0;
    const bool autoContentLengthOwnedByWriter =
        policy.autoContentLengthAllowed &&
        !hasTransferEncoding &&
        !suppressAutoContentLength;
    const bool explicitContentLengthAllowed =
        policy.explicitContentLengthAllowed &&
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

    const auto statusLine = httpCachedStatusLine(response.statusCode(), response.statusText());
    const auto dateHeader = cachedDateHeader();

    // Upper bound on emitted bytes (filtered headers are counted anyway; the
    // numeric slots use the 20-digit std::uint64_t worst case). The raw stack
    // sink below emits without per-append bounds checks, so this must never
    // undercount the actual output.
    std::size_t bound = statusLine.empty()
        ? 9 + 20 + 1 + response.statusText().size() + 2
        : statusLine.size();
    for (const auto& header : response.headers()) {
        bound += static_cast<std::size_t>(header.nameSize) + header.valueSize + 4;
    }
    if ((knownBits & HttpResponse::kKnownHeaderServer) == 0) {
        bound += 15;
    }
    if ((knownBits & HttpResponse::kKnownHeaderDate) == 0) {
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
