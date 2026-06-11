#include "HttpResponseWriter.h"

#include <algorithm>
#include <cerrno>
#include <string>

#include <zlib.h>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/sendfile.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <mswsock.h>
#include <windows.h>
#endif

#include "ruvia/http/HeaderUtils.h"
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

void ResponseHeadBuffer::append(char value) { append(std::string_view(&value, 1)); }

void ResponseHeadBuffer::appendUnsigned(std::uint64_t value) {
    std::array<char, 32> buffer;
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec == std::errc{}) {
        append(std::string_view(buffer.data(), static_cast<std::size_t>(ptr - buffer.data())));
    }
}

std::string_view ResponseHeadBuffer::view() const noexcept { return overflowed_ ? std::string_view(heap_) : std::string_view(stack_.data(), used_); }

bool ResponseHeadBuffer::canAppendOnStack(std::size_t size) const noexcept { return !overflowed_ && size <= stack_.size() - used_; }

namespace {

void setCompressedContentLength(HttpResponse& response, std::size_t size) {
    std::array<char, 32> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), size);
    if (ec != std::errc{}) {
        throw std::logic_error("failed to format compressed content length");
    }
    response.setHeader("Content-Length", std::string_view(buffer.data(), static_cast<std::size_t>(ptr - buffer.data())));
}

void addAcceptEncodingVary(HttpResponse& response) {
    const auto vary = response.header(HttpResponse::kKnownHeaderVary);
    if (vary.empty()) {
        response.setHeader("Vary", "Accept-Encoding");
        return;
    }
    if (httpHasToken(vary, "Accept-Encoding")) {
        return;
    }
    std::pmr::string updated(response.resource());
    updated.append(vary);
    updated.append(", Accept-Encoding");
    response.setHeader("Vary", updated);
}

bool gzipCompress(std::string_view input, std::pmr::string& output) {
    z_stream stream{};
    constexpr int kGzipWindowBits = 15 + 16;
    if (deflateInit2(
            &stream,
            Z_DEFAULT_COMPRESSION,
            Z_DEFLATED,
            kGzipWindowBits,
            8,
            Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }

    struct DeflateGuard final {
        z_stream* stream;
        ~DeflateGuard() { (void)deflateEnd(stream); }
    } guard{&stream};

    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    std::array<char, 8192> chunk{};
    int status = Z_OK;
    while (status == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
        stream.avail_out = static_cast<uInt>(chunk.size());
        status = deflate(&stream, Z_FINISH);
        if (status != Z_OK && status != Z_STREAM_END) {
            return false;
        }
        const auto produced = chunk.size() - stream.avail_out;
        output.append(chunk.data(), produced);
    }
    return status == Z_STREAM_END;
}

}  // namespace

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

bool compressResponseBodyIfAccepted(
    const HttpRequestFlags& requestFlags,
    HttpResponse& response,
    const HttpServerOptions::Compression& options,
    bool skipBody) {
    if (skipBody ||
        !options.enabled ||
        response.hasFileBody() ||
        response.bodySize() < options.minBytes ||
        response.hasKnownHeader(HttpResponse::kKnownHeaderContentEncoding) ||
        response.hasKnownHeader(HttpResponse::kKnownHeaderContentRange) ||
        httpHasToken(response.header(HttpResponse::kKnownHeaderCacheControl), "no-transform") ||
        !requestFlags.acceptsGzip) {
        return false;
    }
    if (response.statusCode() < 200 ||
        response.statusCode() == 206 ||
        response.statusCode() == 204 ||
        response.statusCode() == 205 ||
        response.statusCode() == 304) {
        return false;
    }

    const auto body = response.bodyBytes();
    std::pmr::string compressed(response.resource());
    compressed.reserve(body.size());
    if (!gzipCompress(body, compressed) || compressed.size() >= body.size()) {
        return false;
    }

    response.setHeader("Content-Encoding", "gzip");
    addAcceptEncodingVary(response);
    setCompressedContentLength(response, compressed.size());
    response.setBody(std::move(compressed));
    return true;
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
            if ((!flags.explicitContentLengthAllowed && header.knownBit == HttpResponse::kKnownHeaderContentLength) ||
                (!flags.transferEncodingAllowed && header.knownBit == HttpResponse::kKnownHeaderTransferEncoding)) {
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
        .filterForbiddenBodyHeaders =
            (!explicitContentLengthAllowed && (knownBits & HttpResponse::kKnownHeaderContentLength) != 0) ||
            (!transferEncodingAllowed && (knownBits & HttpResponse::kKnownHeaderTransferEncoding) != 0)};

    const auto statusLine = httpCachedStatusLine(response.statusCode(), response.statusText());
    const auto dateHeader = cachedDateHeader();

    // Upper bound on emitted bytes (filtered headers are counted anyway; the
    // numeric slots use the 20-digit std::uint64_t worst case).
    std::size_t bound = statusLine.empty()
        ? 9 + 20 + 1 + response.statusText().size() + 2
        : statusLine.size();
    for (const auto& header : response.headers()) {
        bound += static_cast<std::size_t>(header.nameSize) + header.valueSize + 4;
    }
    bound += 14 + dateHeader.size() + 16 + 20 + 2 + 2;

    if (char* cursor = head.stackCursor(bound); cursor != nullptr) {
        RawHeadSink sink{cursor};
        emitResponseHead(response, sink, statusLine, dateHeader, flags);
        head.commitStack(sink.out);
        return;
    }
    emitResponseHead(response, head, statusLine, dateHeader, flags);
}

#if defined(__linux__)
class NativeFileHandle final {
public:
    explicit NativeFileHandle(int fd = -1) noexcept : fd_(fd) {}
    ~NativeFileHandle() { if (fd_ >= 0) { ::close(fd_); } }
    NativeFileHandle(const NativeFileHandle&) = delete;
    NativeFileHandle& operator=(const NativeFileHandle&) = delete;
    [[nodiscard]] int get() const noexcept { return fd_; }
private:
    int fd_;
};

Task<void> writeFileZeroCopy(asio::ip::tcp::socket& socket, const FileBody& file, std::error_code& ec) {
    NativeFileHandle input(::open(fileTokenPath(file.file).string().c_str(), O_RDONLY | O_CLOEXEC));
    if (input.get() < 0) { ec = std::error_code(errno, std::system_category()); co_return; }
    auto offset = static_cast<off_t>(file.offset);
    std::uint64_t remaining = file.length;
    while (remaining > 0) {
        const auto nextSend = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, 0x7ffff000ULL));
        const auto sent = ::sendfile(socket.native_handle(), input.get(), &offset, nextSend);
        if (sent > 0) { remaining -= static_cast<std::uint64_t>(sent); continue; }
        if (sent == 0) { ec = asio::error::operation_aborted; co_return; }
        if (errno == EINTR) { continue; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            ec = co_await asyncError([&socket](auto handler) mutable { socket.async_wait(asio::ip::tcp::socket::wait_write, std::move(handler)); });
            if (ec) { co_return; }
            continue;
        }
        ec = std::error_code(errno, std::system_category());
        co_return;
    }
    ec = {};
}
#elif defined(_WIN32)
class NativeFileHandle final {
public:
    explicit NativeFileHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}
    ~NativeFileHandle() { if (handle_ != INVALID_HANDLE_VALUE) { ::CloseHandle(handle_); } }
    NativeFileHandle(const NativeFileHandle&) = delete;
    NativeFileHandle& operator=(const NativeFileHandle&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
private:
    HANDLE handle_;
};

Task<void> writeFileZeroCopy(asio::ip::tcp::socket& socket, const FileBody& file, std::error_code& ec) {
    NativeFileHandle input(::CreateFileW(fileTokenPath(file.file).wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (input.get() == INVALID_HANDLE_VALUE) { ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category()); co_return; }
    LARGE_INTEGER position; position.QuadPart = static_cast<LONGLONG>(file.offset);
    if (::SetFilePointerEx(input.get(), position, nullptr, FILE_BEGIN) == 0) { ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category()); co_return; }
    std::uint64_t remaining = file.length;
    while (remaining > 0) {
        const auto nextSend = static_cast<DWORD>(std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>((std::numeric_limits<DWORD>::max)())));
        if (::TransmitFile(socket.native_handle(), input.get(), nextSend, 0, nullptr, nullptr, 0) != FALSE) { remaining -= nextSend; continue; }
        const auto error = ::WSAGetLastError();
        if (error == WSAEWOULDBLOCK) {
            ec = co_await asyncError([&socket](auto handler) mutable { socket.async_wait(asio::ip::tcp::socket::wait_write, std::move(handler)); });
            if (ec) { co_return; }
            continue;
        }
        ec = std::error_code(error, std::system_category());
        co_return;
    }
    ec = {};
}
#else
Task<void> writeFileZeroCopy(asio::ip::tcp::socket&, const FileBody&, std::error_code& ec) { ec = asio::error::operation_not_supported; co_return; }
#endif

}  // namespace ruvia::detail
