#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/detail/body/HttpStreamBodyReader.h"

namespace {

struct UnusedBodyStream final {};

ruvia::detail::Http1RequestBodyPlan parseBodyPlan(std::string_view wire) {
    return ruvia::detail::Http1ServerRequestParser().parseMessage(wire).bodyPlan;
}

}  // namespace

RUVIA_TEST(http1_without_body_plan_preserves_the_entire_pipeline) {
    const auto plan = parseBodyPlan(
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(plan.withoutBody() != nullptr);

    UnusedBodyStream stream;
    ruvia::detail::ConnectionScanner::Entry scannerEntry;
    std::pmr::monotonic_buffer_resource resource;
    ruvia::detail::StreamBodyReader<UnusedBodyStream> reader(
        stream,
        std::pmr::polymorphic_allocator<char>(&resource),
        "GET /next HTTP/1.1\r\nHost: x\r\n\r\n",
        plan,
        1024,
        scannerEntry);
    RUVIA_CHECK(
        reader.consumption() ==
        ruvia::detail::Http1RequestBodyConsumption::kComplete);

    std::pmr::string restored(&resource);
    std::size_t usedBytes = 0;
    reader.restorePipeline(restored, usedBytes);
    RUVIA_CHECK_EQ(usedBytes, restored.size());
    RUVIA_CHECK_EQ(
        std::string_view(restored.data(), restored.size()),
        std::string_view("GET /next HTTP/1.1\r\nHost: x\r\n\r\n"));
}
