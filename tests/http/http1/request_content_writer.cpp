#include <array>
#include <string>

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
    RUVIA_CHECK(prepared.prepared()->head().contains("Transfer-Encoding: chunked\r\n"));
    ruvia::Http1RequestContentWriter writer(*plan);
    const std::string payload = "hello";
    auto chunk = writer.planChunk(payload);
    RUVIA_CHECK(chunk.has_value());
    RUVIA_CHECK_EQ(std::string_view(chunk->prefix.data(), chunk->prefixSize), std::string_view("5\r\n"));
    RUVIA_CHECK_EQ(chunk->suffix, std::string_view("\r\n"));
    RUVIA_CHECK(!writer.commitChunk(4).has_value());
    RUVIA_CHECK(writer.commitChunk(5).has_value());
    std::array<char, 128> finish{};
    const std::array trailers{ruvia::HttpHeaderView{"Digest", "value"}};
    auto final = writer.planFinish(finish, trailers);
    RUVIA_CHECK(final.has_value());
    RUVIA_CHECK_EQ(*final, std::string_view("0\r\nDigest: value\r\n\r\n"));
    RUVIA_CHECK(!writer.planChunk(payload).has_value());
    RUVIA_CHECK(writer.commitFinish().has_value());
    RUVIA_CHECK(writer.finished());
}
RUVIA_TEST(http1_streaming_request_known_length_and_continue_gate) {
    ruvia::Http1ClientRequestWriter headWriter;
    std::array<char, 4096> head{};
    auto prepared = headWriter.prepareStreaming(ruvia::HttpOriginView::http({.host = "example.com"}),
        {.method = "PUT", .contentLength = 3}, head, {.expectation = ruvia::HttpClientRequestExpectation::kContinue});
    RUVIA_CHECK(prepared.prepared() != nullptr);
    RUVIA_CHECK(prepared.prepared()->head().contains("Content-Length: 3\r\n"));
    ruvia::Http1RequestContentWriter writer(*prepared.prepared()->contentPlan().streaming());
    const std::string payload = "abc";
    RUVIA_CHECK(!writer.planChunk(payload).has_value());
    writer.releaseContent();
    RUVIA_CHECK(!writer.planFinish({}).has_value());
    auto chunk = writer.planChunk(payload);
    RUVIA_CHECK(chunk.has_value());
    RUVIA_CHECK_EQ(chunk->prefixSize, 0u);
    RUVIA_CHECK(chunk->suffix.empty());
    RUVIA_CHECK(writer.commitChunk(3).has_value());
    RUVIA_CHECK(!writer.planChunk(payload).has_value());
    auto finish = writer.planFinish({});
    RUVIA_CHECK(finish.has_value());
    RUVIA_CHECK(finish->empty());
    RUVIA_CHECK(writer.commitFinish().has_value());
}
RUVIA_TEST(http1_streaming_request_rejects_forbidden_trailer_without_mutating_buffer) {
    std::array<char, 1024> head{};
    auto prepared = ruvia::Http1ClientRequestWriter{}.prepareStreaming(ruvia::HttpOriginView::http({.host = "example.com"}), {.method = "POST"}, head);
    ruvia::Http1RequestContentWriter writer(*prepared.prepared()->contentPlan().streaming());
    std::array<char, 128> finish{};
    finish.fill('X');
    const std::array trailers{ruvia::HttpHeaderView{"Content-Length", "0"}};
    auto rejected = writer.planFinish(finish, trailers);
    RUVIA_CHECK(!rejected.has_value());
    RUVIA_CHECK_EQ(finish.front(), 'X');
    RUVIA_CHECK(writer.planFinish(finish).has_value());
}
