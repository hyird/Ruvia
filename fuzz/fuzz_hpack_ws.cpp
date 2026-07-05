// Smoke-fuzz for HPACK decoding (integer overflow, Huffman padding, dynamic
// table) and WebSocket frame/UTF-8/close validation -- both untrusted-peer attack
// surfaces. Full-byte-range random blocks reach every HPACK prefix, Huffman code,
// UTF-8 sequence and frame flag.
//
// Iteration count: argv[1] (default 200000). Build with a UBSan CMAKE_CXX_FLAGS.
#include "net/http2/Http2Hpack.h"
#include "net/http2/Http2FrameCodec.h"
#include "net/ws/HttpWebSocketUtils.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory_resource>
#include <string>
#include <string_view>

namespace d = ruvia::detail;

namespace {
std::uint64_t g = 0xb5297a4d1e6f30a7ULL;
std::uint64_t next() { g ^= g << 13; g ^= g >> 7; g ^= g << 17; return g; }
bool sink(void*, std::string_view, std::string_view) { return true; }
}  // namespace

int main(int argc, char** argv) {
    const long iterations = argc > 1 ? std::strtol(argv[1], nullptr, 10) : 200000;
    // A shared decoder exercises dynamic-table state across inputs; a fresh one
    // per iteration exercises the size-update / eviction paths from a clean table.
    d::HpackDecoder shared(std::pmr::get_default_resource());
    std::string buf;
    for (long i = 0; i < iterations; ++i) {
        const auto len = static_cast<std::size_t>(next() % 80);
        buf.resize(len);
        for (std::size_t j = 0; j < len; ++j) buf[j] = static_cast<char>(next() & 0xFF);
        const std::string_view s(buf.data(), buf.size());

        d::HpackDecoder fresh(std::pmr::get_default_resource());
        fresh.setMaxDynamicTableSize(256);
        (void)fresh.decode(s, nullptr, &sink);
        (void)shared.decode(s, nullptr, &sink);

        if (s.size() >= 2) {
            d::WebSocketFrameStart frame;
            (void)d::decodeWebSocketFrameStart(static_cast<unsigned char>(s[0]),
                                               static_cast<unsigned char>(s[1]), frame, true);
            (void)d::decodeWebSocketFrameStart(static_cast<unsigned char>(s[0]),
                                               static_cast<unsigned char>(s[1]), frame, false);
            (void)d::readWebSocketUint16(s.data());
        }
        if (s.size() >= 8) {
            std::uint64_t v = 0;
            (void)d::readWebSocketUint64(s.data(), v);
        }
        (void)d::isValidUtf8(s);
        (void)d::isValidWebSocketCloseCode(static_cast<std::uint16_t>(next()));
    }
    std::printf("fuzz_hpack_ws ok: %ld iterations\n", iterations);
    return 0;
}
