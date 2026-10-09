#include <array>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/Http1RequestContentWriter.h"

#include "test_harness.h"

RUVIA_TEST(http1_streaming_request_chunked_upload_and_trailers) {
    ruvia::Http1ClientRequestWriter headWriter;
    std::array<char, 4096> head{};
    auto prepared = headWriter.prepareStreaming(ruvia::HttpOriginView::https({.host = "example.com"}), {.method = "POST"}, head);
    RUVIA_CHECK(prepared.prepared() != nullptr);
    auto* plan = prepared.prepared()->contentPlan().streaming();
    RUVIA_CHECK(plan != nullptr);
    RUVIA_CHECK((prepared.prepared()->head().find("Transfer-Encoding: chunked\r\n") != std::string_view::npos));
    ruvia::Http1RequestContentWriter writer(*plan);
    const std::string payload = "hello";
    auto chunk = writer.planChunk(payload);
    RUVIA_CHECK((chunk.index() == 0));
    RUVIA_CHECK_EQ(std::string_view(std::get<0>(chunk).prefix.data(), std::get<0>(chunk).prefixSize), std::string_view("5\r\n"));
    RUVIA_CHECK_EQ(std::get<0>(chunk).suffix, std::string_view("\r\n"));
    RUVIA_CHECK(!(writer.commitChunk(4).index() == 0));
    RUVIA_CHECK((writer.commitChunk(5).index() == 0));
    std::array<char, 128> finish{};
    const std::array trailers{ruvia::HttpHeaderView{"Digest", "value"}};
    auto final = writer.planFinish(finish, trailers);
    RUVIA_CHECK((final.index() == 0));
    RUVIA_CHECK_EQ(std::get<0>(final), std::string_view("0\r\nDigest: value\r\n\r\n"));
    RUVIA_CHECK(!(writer.planChunk(payload).index() == 0));
    RUVIA_CHECK((writer.commitFinish().index() == 0));
    RUVIA_CHECK(writer.finished());
}
RUVIA_TEST(http1_streaming_request_known_length_and_continue_gate) {
    ruvia::Http1ClientRequestWriter headWriter;
    std::array<char, 4096> head{};
    auto prepared = headWriter.prepareStreaming(ruvia::HttpOriginView::http({.host = "example.com"}),
        {.method = "PUT", .contentLength = 3}, head, {.expectation = ruvia::HttpClientRequestExpectation::kContinue});
    RUVIA_CHECK(prepared.prepared() != nullptr);
    RUVIA_CHECK((prepared.prepared()->head().find("Content-Length: 3\r\n") != std::string_view::npos));
    ruvia::Http1RequestContentWriter writer(*prepared.prepared()->contentPlan().streaming());
    const std::string payload = "abc";
    RUVIA_CHECK(!(writer.planChunk(payload).index() == 0));
    writer.releaseContent();
    RUVIA_CHECK(!(writer.planFinish({}).index() == 0));
    auto chunk = writer.planChunk(payload);
    RUVIA_CHECK((chunk.index() == 0));
    RUVIA_CHECK_EQ(std::get<0>(chunk).prefixSize, 0u);
    RUVIA_CHECK(std::get<0>(chunk).suffix.empty());
    RUVIA_CHECK((writer.commitChunk(3).index() == 0));
    RUVIA_CHECK(!(writer.planChunk(payload).index() == 0));
    auto finish = writer.planFinish({});
    RUVIA_CHECK((finish.index() == 0));
    RUVIA_CHECK(std::get<0>(finish).empty());
    RUVIA_CHECK((writer.commitFinish().index() == 0));
}
RUVIA_TEST(http1_streaming_request_rejects_forbidden_trailer_without_mutating_buffer) {
    std::array<char, 1024> head{};
    auto prepared = ruvia::Http1ClientRequestWriter{}.prepareStreaming(ruvia::HttpOriginView::http({.host = "example.com"}), {.method = "POST"}, head);
    ruvia::Http1RequestContentWriter writer(*prepared.prepared()->contentPlan().streaming());
    std::array<char, 128> finish{};
    finish.fill('X');
    const std::array trailers{ruvia::HttpHeaderView{"Content-Length", "0"}};
    auto rejected = writer.planFinish(finish, trailers);
    RUVIA_CHECK(!(rejected.index() == 0));
    RUVIA_CHECK_EQ(finish.front(), 'X');
    RUVIA_CHECK((writer.planFinish(finish).index() == 0));
}
