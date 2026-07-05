// Smoke-fuzz for the full HTTP/1 request parser -- the primary untrusted attack
// surface (request line, header block, framing, smuggling). Request-shaped
// prefixes seed deep header/chunk states; the rest is random adversarial bytes.
//
// Iteration count: argv[1] (default 200000). Build with a UBSan CMAKE_CXX_FLAGS to
// detect undefined behaviour.
#include "http/HttpParserInternal.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace d = ruvia::detail;

namespace {
std::uint64_t g = 0xd1b54a32d192ed03ULL;
std::uint64_t next() { g ^= g << 13; g ^= g >> 7; g ^= g << 17; return g; }
}  // namespace

int main(int argc, char** argv) {
    const long iterations = argc > 1 ? std::strtol(argv[1], nullptr, 10) : 200000;
    d::HttpServerParser parser;

    static constexpr std::string_view prefixes[] = {
        "", "GET ", "POST / HTTP/1.1\r\n", "GET /a?b HTTP/1.1\r\nHost: x\r\n",
        "CONNECT a:1 HTTP/1.1\r\n", "OPTIONS * HTTP/1.1\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 5\r\n",
    };
    std::string buf;
    static volatile std::size_t sink = 0;
    for (long i = 0; i < iterations; ++i) {
        buf.clear();
        if ((next() & 1) != 0) {
            buf.append(prefixes[next() % (sizeof(prefixes) / sizeof(prefixes[0]))]);
        }
        const auto extra = static_cast<std::size_t>(next() % 96);
        for (std::size_t j = 0; j < extra; ++j) {
            static constexpr char pool[] =
                "GET POST HTTP/1.:;=,\r\n \tHost:Content-Length Transfer-Encoding "
                "chunked0123456789abcdef%\\?#/@[]-_.\x00\x7f\x80\xff";
            buf.push_back(pool[next() % (sizeof(pool) - 1)]);
        }
        const auto result = parser.parse(std::string_view(buf.data(), buf.size()));
        sink += result.consumedBytes + result.request.headers().size();
    }
    std::printf("fuzz_request ok: %ld iterations\n", iterations);
    return 0;
}
