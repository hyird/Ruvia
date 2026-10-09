#include <array>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http1_request_content_writer.h"

#include "test_harness.h"

RUVIA_TEST(http1_streaming_request_chunked_upload_and_trailers) {
    ruvia::http1_client_request_writer head_writer;
    std::array<char, 4096> head{};
    auto prepared = head_writer.prepare_streaming(ruvia::http_origin_view::https({.host_ = "example.com"}), {.method_ = "POST"}, head);
    RUVIA_CHECK(prepared.prepared() != nullptr);
    auto* plan = prepared.prepared()->content_plan().streaming();
    RUVIA_CHECK(plan != nullptr);
    RUVIA_CHECK((prepared.prepared()->head().find("Transfer-Encoding: chunked\r\n") != std::string_view::npos));
    ruvia::http1_request_content_writer writer(*plan);
    const std::string payload_value = "hello";
    auto chunk = writer.plan_chunk(payload_value);
    RUVIA_CHECK((chunk.index() == 0));
    RUVIA_CHECK_EQ(std::string_view(std::get<0>(chunk).prefix_.data(), std::get<0>(chunk).prefix_size_), std::string_view("5\r\n"));
    RUVIA_CHECK_EQ(std::get<0>(chunk).suffix_, std::string_view("\r\n"));
    RUVIA_CHECK(!(writer.commit_chunk(4).index() == 0));
    RUVIA_CHECK((writer.commit_chunk(5).index() == 0));
    std::array<char, 128> finish_value{};
    const std::array trailers{ruvia::http_header_view{"Digest", "value"}};
    auto final = writer.plan_finish(finish_value, trailers);
    RUVIA_CHECK((final.index() == 0));
    RUVIA_CHECK_EQ(std::get<0>(final), std::string_view("0\r\nDigest: value\r\n\r\n"));
    RUVIA_CHECK(!(writer.plan_chunk(payload_value).index() == 0));
    RUVIA_CHECK((writer.commit_finish().index() == 0));
    RUVIA_CHECK(writer.finished());
}
RUVIA_TEST(http1_streaming_request_known_length_and_continue_gate) {
    ruvia::http1_client_request_writer head_writer;
    std::array<char, 4096> head{};
    auto prepared = head_writer.prepare_streaming(ruvia::http_origin_view::http({.host_ = "example.com"}),
        {.method_ = "PUT", .content_length_ = 3}, head, {.expectation_ = ruvia::http_client_request_expectation::continue_value});
    RUVIA_CHECK(prepared.prepared() != nullptr);
    RUVIA_CHECK((prepared.prepared()->head().find("Content-Length: 3\r\n") != std::string_view::npos));
    ruvia::http1_request_content_writer writer(*prepared.prepared()->content_plan().streaming());
    const std::string payload_value = "abc";
    RUVIA_CHECK(!(writer.plan_chunk(payload_value).index() == 0));
    writer.release_content();
    RUVIA_CHECK(!(writer.plan_finish({}).index() == 0));
    auto chunk = writer.plan_chunk(payload_value);
    RUVIA_CHECK((chunk.index() == 0));
    RUVIA_CHECK_EQ(std::get<0>(chunk).prefix_size_, 0u);
    RUVIA_CHECK(std::get<0>(chunk).suffix_.empty());
    RUVIA_CHECK((writer.commit_chunk(3).index() == 0));
    RUVIA_CHECK(!(writer.plan_chunk(payload_value).index() == 0));
    auto finish_value = writer.plan_finish({});
    RUVIA_CHECK((finish_value.index() == 0));
    RUVIA_CHECK(std::get<0>(finish_value).empty());
    RUVIA_CHECK((writer.commit_finish().index() == 0));
}
RUVIA_TEST(http1_streaming_request_rejects_forbidden_trailer_without_mutating_buffer) {
    std::array<char, 1024> head{};
    auto prepared = ruvia::http1_client_request_writer{}.prepare_streaming(ruvia::http_origin_view::http({.host_ = "example.com"}), {.method_ = "POST"}, head);
    ruvia::http1_request_content_writer writer(*prepared.prepared()->content_plan().streaming());
    std::array<char, 128> finish_value{};
    finish_value.fill('X');
    const std::array trailers{ruvia::http_header_view{"Content-Length", "0"}};
    auto rejected = writer.plan_finish(finish_value, trailers);
    RUVIA_CHECK(!(rejected.index() == 0));
    RUVIA_CHECK_EQ(finish_value.front(), 'X');
    RUVIA_CHECK((writer.plan_finish(finish_value).index() == 0));
}
