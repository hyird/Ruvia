#include "ruvia/http/detail/http2/Http2Hpack.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// A spread of static-table names so the fuzzer reliably drives the encoder's
// static exact-index and name-index branches (hpackFindStaticHeaderMatch)
// instead of waiting for coverage to rediscover them from raw bytes. Values
// like "GET"/"200" also let (name,value) pairs hit the fully-indexed path.
constexpr std::string_view kStaticNames[] = {
    ":method", ":path", ":scheme", ":status", ":authority",
    "content-type", "content-length", "accept", "accept-encoding",
    "user-agent", "cookie", "set-cookie", "authorization", "date",
    "etag", "location", "range", "accept-ranges",
};

struct Header final {
    std::string name;
    std::string value;
};

// Carve arbitrary bytes into (name, value) pairs. A control byte per pair picks
// either a static-table name or a length-prefixed literal, then a length-
// prefixed value follows. Whatever is produced is exactly what the round trip
// must reproduce.
std::vector<Header> carve(std::string_view input) {
    std::vector<Header> headers;
    std::size_t i = 0;
    const auto readChunk = [&](std::size_t& cursor) -> std::string {
        if (cursor >= input.size()) {
            return std::string();
        }
        const auto length = static_cast<unsigned char>(input[cursor++]);
        const auto take = std::min<std::size_t>(length, input.size() - cursor);
        std::string chunk(input.substr(cursor, take));
        cursor += take;
        return chunk;
    };
    while (i < input.size() && headers.size() < 64) {
        const auto control = static_cast<unsigned char>(input[i++]);
        Header header;
        if ((control & 0x01U) != 0) {
            header.name = std::string(
                kStaticNames[(control >> 1U) % (sizeof(kStaticNames) / sizeof(kStaticNames[0]))]);
        } else {
            header.name = readChunk(i);
        }
        header.value = readChunk(i);
        headers.push_back(std::move(header));
    }
    return headers;
}

struct Recorder final {
    std::vector<Header> headers;
    bool overflow{false};
};

bool recordHeader(void* target, std::string_view name, std::string_view value) {
    auto& recorder = *static_cast<Recorder*>(target);
    if (recorder.headers.size() >= 4096) {
        recorder.overflow = true;
        return false;
    }
    recorder.headers.push_back(Header{std::string(name), std::string(value)});
    return true;
}

}  // namespace

// Round-trips the HPACK encoder against the decoder: every (name, value) pair
// the encoder accepts -- including the static exact-index, static name-index,
// never-indexed, and integer/string-length boundary paths -- must decode back
// byte-for-byte. A mismatch is a genuine header-confusion or framing bug, not
// just a crash. The encoder never inserts into the dynamic table (literal
// without indexing), so each header is independent and order is preserved.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = std::string_view(
        reinterpret_cast<const char*>(data), size);
    const auto headers = carve(input);
    if (headers.empty()) {
        return 0;
    }

    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string block(&resource);
    for (const auto& header : headers) {
        ruvia::detail::HpackEncoder::encodeHeader(block, header.name, header.value);
    }

    ruvia::detail::HpackDecoder decoder(&resource);
    Recorder recorder;
    const auto result = decoder.decode(
        std::string_view(block.data(), block.size()), &recorder, &recordHeader);

    // The encoder only emits well-formed HPACK, so its own output must decode
    // cleanly and reproduce every field exactly.
    if (result.failure() != nullptr || recorder.overflow) {
        __builtin_trap();
    }
    if (recorder.headers.size() != headers.size()) {
        __builtin_trap();
    }
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (recorder.headers[i].name != headers[i].name ||
            recorder.headers[i].value != headers[i].value) {
            __builtin_trap();
        }
    }
    return 0;
}
