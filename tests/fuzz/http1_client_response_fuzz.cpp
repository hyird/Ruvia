#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/HttpClient.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>

namespace {

// The request method is the only client-side input that changes how a response
// is framed (HEAD suppresses the body, CONNECT tunnels, OPTIONS keeps content),
// so pick it from a control byte and feed the remaining bytes as the raw wire.
constexpr std::string_view kMethods[] = {
    "GET", "HEAD", "POST", "PUT", "DELETE", "OPTIONS", "CONNECT",
};

void exercisePlan(const ruvia::Http1ClientResponsePlan& plan) noexcept {
    (void)plan.informational();
    (void)plan.withoutContent();
    (void)plan.knownLength();
    (void)plan.chunked();
    (void)plan.closeDelimited();
    (void)plan.connectTunnel();
    (void)plan.protocolUpgrade();
    if (const auto* zero = plan.zeroContent()) {
        (void)zero->knownLength();
        (void)zero->chunked();
        (void)zero->closeDelimited();
    }
}

void exerciseResult(const ruvia::Http1ClientResponseParseResult& result) noexcept {
    (void)result.needMore();
    (void)result.failure();
    const auto* parsed = result.parsed();
    if (parsed == nullptr) {
        return;
    }
    const auto& head = parsed->head();
    (void)head.status();
    (void)head.protocolVersion();
    // Every header name/value is a borrowed view into the fuzzed wire; touching
    // all of them turns any out-of-bounds slice into an ASan/UBSan trip.
    for (const auto& header : head.headers()) {
        (void)header.name();
        (void)header.value();
    }
    exercisePlan(parsed->plan());
}

}  // namespace

// Fuzzes the outbound HTTP client's HTTP/1 response head parser, which consumes
// untrusted bytes from a remote server. Covers status-line and field-line
// grammar, Content-Length / Transfer-Encoding framing decisions, interim (1xx)
// handling, and the method-dependent body-presence rules -- a surface that had
// no fuzzer while the request parser did.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const auto method = kMethods[data[0] % (sizeof(kMethods) / sizeof(kMethods[0]))];
    const auto wire = std::string_view(
        reinterpret_cast<const char*>(data + 1),
        size - 1);

    std::array<char, 2048> headBuffer;
    const auto origin = ruvia::HttpOrigin::https("example.test");
    const ruvia::Http1ClientRequestWriter writer;

    const auto preparedResult = method == "CONNECT"
        ? writer.prepareConnect(
              origin,
              std::span<const ruvia::HttpHeaderView>{},
              headBuffer)
        : [&] {
              ruvia::HttpClientRequest request;
              request.method = method;
              return writer.prepare(origin, request, headBuffer);
          }();

    const auto* prepared = preparedResult.prepared();
    if (prepared == nullptr) {
        return 0;
    }

    std::pmr::monotonic_buffer_resource resource;
    ruvia::Http1ClientResponseParser parser(*prepared, &resource);
    exerciseResult(parser.parse(wire));
    return 0;
}
