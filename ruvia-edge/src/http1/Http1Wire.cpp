#include "ruvia/edge/detail/http1/Http1Wire.h"

#include <array>
#include <charconv>

#include "ruvia/http/HttpStatus.h"

namespace ruvia::edge {

namespace {

void appendDecimal(std::string& out, std::uint64_t value) {
    std::array<char, 20> digits;
    const auto [end, ec] =
        std::to_chars(digits.data(), digits.data() + digits.size(), value);
    (void)ec;
    out.append(digits.data(), static_cast<std::size_t>(end - digits.data()));
}

void appendStatusLine(
    std::string& out,
    HttpProtocolVersion protocolVersion,
    std::uint16_t status) {
    out.append(
        protocolVersion == HttpProtocolVersion::kHttp10
            ? "HTTP/1.0 "
            : "HTTP/1.1 ");
    appendDecimal(out, status);
    out.push_back(' ');
    if (const auto code = HttpStatusCode::tryFromValue(status)) {
        out.append(httpReasonPhrase(*code));
    }
    out.append("\r\n");
}

}  // namespace

std::string encodeResponse(
    HttpProtocolVersion protocolVersion,
    std::uint16_t status,
    const Headers& headers,
    std::string_view body,
    std::string_view xCache,
    std::optional<std::uint64_t> ageOverride,
    bool omitBody,
    bool keepAlive) {
    std::string out;
    out.reserve((omitBody ? 0 : body.size()) + 256);

    appendStatusLine(out, protocolVersion, status);

    bool keptContentLength = false;
    for (const auto& [name, value] : headers) {
        std::string lower(name);
        for (auto& c : lower) {
            c = toLowerAscii(c);
        }
        const bool nominated = connectionNominates(headers, name);
        if (isConnectionOrFramingField(lower) || nominated) {
            // For a HEAD response keep the origin's Content-Length; otherwise the
            // edge emits its own from the body length.
            if (!nominated && omitBody && lower == "content-length") {
                keptContentLength = true;
            } else {
                continue;
            }
        }
        if (ageOverride && lower == "age") {
            continue;
        }
        out.append(name);
        out.append(": ");
        out.append(value);
        out.append("\r\n");
    }

    if (!omitBody || !keptContentLength) {
        out.append("Content-Length: ");
        appendDecimal(out, body.size());
        out.append("\r\n");
    }
    out.append(keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
    out.append("X-Cache: ");
    out.append(xCache);
    out.append("\r\n");
    if (ageOverride) {
        out.append("Age: ");
        appendDecimal(out, *ageOverride);
        out.append("\r\n");
    }
    out.append("\r\n");
    if (!omitBody) {
        out.append(body);
    }
    return out;
}

std::string encodeStatusResponse(std::uint16_t status, HttpProtocolVersion protocolVersion) {
    return encodeResponse(
        protocolVersion,
        status,
        Headers{},
        {},
        "MISS",
        std::nullopt);
}

std::string encodeStreamingHead(
    HttpProtocolVersion protocolVersion,
    std::uint16_t status,
    const Headers& headers,
    std::string_view xCache,
    ClientFraming framing,
    std::size_t contentLength,
    bool keepAlive) {
    std::string out;
    appendStatusLine(out, protocolVersion, status);

    for (const auto& [name, value] : headers) {
        std::string lower(name);
        for (auto& c : lower) {
            c = toLowerAscii(c);
        }
        const bool nominated = connectionNominates(headers, name);
        if (isConnectionOrFramingField(lower) || nominated) {
            // For a bodyless response keep the origin's Content-Length (HEAD);
            // otherwise the edge emits its own framing below.
            if (!nominated && framing == ClientFraming::kNoBody &&
                lower == "content-length") {
                // keep it
            } else {
                continue;
            }
        }
        out.append(name);
        out.append(": ");
        out.append(value);
        out.append("\r\n");
    }

    if (framing == ClientFraming::kLength) {
        out.append("Content-Length: ");
        appendDecimal(out, contentLength);
        out.append("\r\n");
    } else if (framing == ClientFraming::kChunked) {
        out.append("Transfer-Encoding: chunked\r\n");
    }
    out.append(keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
    out.append("X-Cache: ");
    out.append(xCache);
    out.append("\r\n");
    out.append("\r\n");
    return out;
}

std::string encodeChunk(std::string_view chunk) {
    std::string out;
    std::array<char, 16> hex;
    const auto [end, ec] =
        std::to_chars(hex.data(), hex.data() + hex.size(), chunk.size(), 16);
    (void)ec;
    out.append(hex.data(), static_cast<std::size_t>(end - hex.data()));
    out.append("\r\n");
    out.append(chunk);
    out.append("\r\n");
    return out;
}

}  // namespace ruvia::edge
