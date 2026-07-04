#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/HttpResponse.h"
#include "net/server/HttpResponseHead.h"
#include "net/server/HttpResponseHeadBuffer.h"
#include "net/server/HttpResponseHeadPolicy.h"

namespace {

using ruvia::HttpResponse;
using ruvia::detail::ResponseHeadBuffer;
using ruvia::detail::ResponseWritePolicy;
using ruvia::detail::appendResponseHead;
using ruvia::detail::responseWritePolicy;

std::string emitHead(
    HttpResponse& response, ResponseWritePolicy policy, bool suppressAutoContentLength = false) {
    ResponseHeadBuffer buffer(std::pmr::new_delete_resource());
    appendResponseHead(response, buffer, policy, suppressAutoContentLength);
    const auto view = buffer.view();
    return std::string(view.data(), view.size());
}

std::size_t countOccurrences(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    for (auto pos = haystack.find(needle); pos != std::string_view::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

}  // namespace

RUVIA_TEST(response_head_emits_well_formed_normal) {
    HttpResponse response(std::pmr::new_delete_resource());
    response.status(200);
    response.header("X-Foo", "bar");
    response.setBodyCopy("hello");
    const auto head = emitHead(response, ResponseWritePolicy::normal());

    RUVIA_CHECK(head.starts_with("HTTP/1.1 200 OK\r\n"));
    RUVIA_CHECK(head.find("X-Foo: bar\r\n") != std::string::npos);
    RUVIA_CHECK(head.find("Server: ruvia\r\n") != std::string::npos);        // auto-injected
    RUVIA_CHECK(head.find("Date: ") != std::string::npos);                   // auto-injected
    RUVIA_CHECK(head.find("Content-Length: 5\r\n") != std::string::npos);    // auto, body size
    RUVIA_CHECK(head.ends_with("\r\n\r\n"));                                  // blank-line terminator
}

RUVIA_TEST(response_head_does_not_duplicate_present_server_date) {
    HttpResponse response(std::pmr::new_delete_resource());
    response.header("Server", "custom");
    response.header("Date", "Wed, 21 Oct 2015 07:28:00 GMT");
    response.setBodyCopy("x");
    const auto head = emitHead(response, ResponseWritePolicy::normal());

    RUVIA_CHECK(head.find("Server: custom\r\n") != std::string::npos);
    RUVIA_CHECK(head.find("Server: ruvia\r\n") == std::string::npos);  // no auto override
    RUVIA_CHECK_EQ(countOccurrences(head, "Server: "), std::size_t{1});
    RUVIA_CHECK_EQ(countOccurrences(head, "Date: "), std::size_t{1});   // exactly one Date
}

RUVIA_TEST(response_head_suppresses_auto_content_length) {
    // A streaming/chunked writer owns framing itself, so the auto Content-Length
    // must be withheld when suppression is requested.
    HttpResponse response(std::pmr::new_delete_resource());
    response.setBodyCopy("hello");
    const auto head = emitHead(response, ResponseWritePolicy::normal(), /*suppress=*/true);
    RUVIA_CHECK(head.find("Content-Length:") == std::string::npos);
}

RUVIA_TEST(response_head_bodyless_status_omits_auto_content_length) {
    HttpResponse response(std::pmr::new_delete_resource());
    response.status(204);
    const auto head = emitHead(response, responseWritePolicy(204));
    RUVIA_CHECK(head.starts_with("HTTP/1.1 204 No Content\r\n"));
    RUVIA_CHECK(head.find("Content-Length:") == std::string::npos);
    RUVIA_CHECK(head.ends_with("\r\n\r\n"));
}

RUVIA_TEST(response_head_heap_spill_preserves_full_output) {
    // Force the emitted head well past the 512-byte stack buffer so the heap
    // (reserveAdditional) emit path runs. Every header must survive intact and
    // the precomputed size bound must not undercount -- an undercount would let
    // the unchecked raw stack sink overflow or the output truncate.
    HttpResponse response(std::pmr::new_delete_resource());
    response.status(200);
    const std::string big(200, 'v');
    for (int i = 0; i < 10; ++i) {
        response.header("X-Pad-" + std::to_string(i), big);
    }
    response.setBodyCopy("body");
    const auto head = emitHead(response, ResponseWritePolicy::normal());

    RUVIA_CHECK(head.starts_with("HTTP/1.1 200 OK\r\n"));
    for (int i = 0; i < 10; ++i) {
        RUVIA_CHECK(head.find("X-Pad-" + std::to_string(i) + ": " + big + "\r\n") != std::string::npos);
    }
    RUVIA_CHECK(head.find("Content-Length: 4\r\n") != std::string::npos);
    RUVIA_CHECK(head.ends_with("\r\n\r\n"));
}
