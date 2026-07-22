#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/edge/detail/proxy/HeaderRules.h"
#include "ruvia/http/HttpProtocolVersion.h"

namespace ruvia::edge {

// How the edge frames a streamed response body to the client.
enum class ClientFraming : std::uint8_t {
    kNoBody,   // no message body (HEAD keeps the origin Content-Length; 204/304 none)
    kLength,   // exact Content-Length, streamed straight through
    kChunked,  // unknown length re-encoded as Transfer-Encoding: chunked
    kCloseDelimited,  // HTTP/1.0 unknown length; socket close ends the body
};

// Serialize a response for the client: status line, curated headers, a fresh
// Content-Length, a Connection header reflecting keep-alive, an X-Cache marker,
// and -- when the edge computes its own age for a cache hit -- an Age header
// (dropping any inherited one).
//
// omitBody serves a HEAD response: no message body is appended, and the resource
// length is reported by keeping the origin's Content-Length when present, else
// computing it from `body` (which for HEAD is the full representation used only
// for its size). Transfer-Encoding is still dropped in both modes.
[[nodiscard]] std::string encodeResponse(
    HttpProtocolVersion protocolVersion,
    std::uint16_t status,
    const Headers& headers,
    std::string_view body,
    std::string_view xCache,
    std::optional<std::uint64_t> ageOverride,
    bool omitBody = false,
    bool keepAlive = false);

// A minimal status-only response for edge-generated errors.
[[nodiscard]] std::string encodeStatusResponse(
    std::uint16_t status,
    HttpProtocolVersion protocolVersion = HttpProtocolVersion::kHttp11);

// Serialize just the response head for a streamed response: status line, curated
// headers, the chosen framing header, Connection and X-Cache. No body follows;
// the caller streams it (raw for kLength, chunk-framed for kChunked).
[[nodiscard]] std::string encodeStreamingHead(
    HttpProtocolVersion protocolVersion,
    std::uint16_t status,
    const Headers& headers,
    std::string_view xCache,
    ClientFraming framing,
    std::size_t contentLength,
    bool keepAlive);

// Wrap one body chunk in HTTP/1 chunked framing.
[[nodiscard]] std::string encodeChunk(std::string_view chunk);

}  // namespace ruvia::edge
