#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <string_view>

#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia {

enum class HttpParseStatus {
    kComplete,
    kIncomplete,
    kError
};

enum class HttpParseError {
    kNone,
    kHeaderTooLarge,
    kBodyTooLarge,
    kInvalidRequestLine,
    kUnsupportedHttpVersion,
    kInvalidRequestTarget,
    kUnsupportedMethod,
    kInvalidHeader,
    kTooManyHeaders,
    kMissingHost,
    kInvalidHost,
    kInvalidContentLength,
    kConflictingContentLength,
    kInvalidTransferEncoding,
    kUnsupportedTransferEncoding,
    kExpectationFailed,
    kInvalidChunkSize,
    kChunkSizeOverflow,
    kInvalidChunkExtension,
    kInvalidChunkCrlf,
    kInvalidTrailer
};

enum class HttpBodyKind {
    kNone,
    kContentLength,
    kChunked,
    kUpgrade
};

enum class HttpTransferCoding : std::uint8_t {
    kGzip,
    kDeflate
};

// Ruvia supports one request transfer-coding before the required final
// "chunked" framing coding (for example "gzip, chunked" or
// "deflate, chunked"). Multiple stacked compression codings are rejected to
// keep request streaming bounded and avoid intermediate decompression bombs.
inline constexpr std::size_t kMaxTransferCodings = 1;

struct HttpTransferCodings {
    std::array<HttpTransferCoding, kMaxTransferCodings> values{};
    std::size_t count{0};

    [[nodiscard]] bool empty() const noexcept {
        return count == 0;
    }
};

struct HttpRequestFlags {
    bool connectionClose{false};
    bool connectionKeepAlive{false};
    bool expectContinue{false};
    bool upgrade{false};
    bool hasHost{false};
    bool acceptsGzip{false};
    bool transferGzip{false};
    bool transferDeflate{false};
    std::uint8_t secWebSocketKeyCount{0};
    std::uint8_t secWebSocketVersionCount{0};
    std::uint8_t secWebSocketProtocolCount{0};
};

struct HttpBodyPlan {
    HttpBodyKind kind{HttpBodyKind::kNone};
    std::size_t contentLength{0};
    bool expectContinue{false};
};

enum class HttpChunkScanStatus {
    kComplete,
    kIncomplete,
    kInvalidSize,
    kSizeOverflow,
    kInvalidExtension,
    kInvalidCrlf,
    kInvalidTrailer,
    kTooLarge
};

struct HttpChunkScanResult {
    HttpChunkScanStatus status{HttpChunkScanStatus::kIncomplete};
    std::size_t consumedBytes{0};
    std::size_t decodedBytes{0};
};

struct HttpParseResult {
    HttpParseStatus status{HttpParseStatus::kIncomplete};
    HttpParseError error{HttpParseError::kNone};
    HttpRequest request;
    std::size_t headerBytes{0};
    std::size_t contentLength{0};
    // For chunked bodies this is the byte count after removing chunk framing.
    // It is not application-decoded payload size when transferCodings is non-empty.
    std::size_t decodedBodyBytes{0};
    std::size_t consumedBytes{0};
    bool chunked{false};
    bool transferGzip{false};
    bool transferDeflate{false};
    HttpTransferCodings transferCodings;
    HttpRequestFlags flags;
    HttpBodyPlan bodyPlan;
};

[[nodiscard]] std::string_view httpParseErrorMessage(HttpParseError error) noexcept;
[[nodiscard]] std::uint16_t httpParseErrorStatus(HttpParseError error) noexcept;
[[nodiscard]] bool parseHttpChunkSize(std::string_view value, std::size_t& size) noexcept;
[[nodiscard]] HttpChunkScanStatus validateHttpChunkTrailers(std::string_view trailers) noexcept;
[[nodiscard]] HttpChunkScanResult scanHttpChunkedBody(std::string_view body) noexcept;

class HttpParser final {
public:
    // In-place parsing entry points: `result` is reset and reused across
    // calls so the request hot path never copies or re-zeroes the ~2.5KB
    // parse result per read iteration.
    void parseHeaders(
        std::string_view buffer,
        HttpParseResult& result,
        std::size_t headerSearchOffset = 0) const noexcept;
    void parseBody(std::string_view buffer, HttpParseResult& result) const noexcept;
    [[nodiscard]] HttpParseResult parse(std::string_view buffer) const noexcept;

private:
    static void parseRequestHead(
        std::string_view buffer,
        std::size_t headerSearchOffset,
        HttpParseResult& result) noexcept;
};

}  // namespace ruvia
