#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http1_request_content_writer.h"
#include "ruvia/http/http_limits.h"

#include "test_harness.h"

namespace {

ruvia::http1_request_content_writer chunked_request_writer() {
    std::array<char, 1024> head{};
    auto prepared = ruvia::http1_client_request_writer{}.prepare_streaming(
        ruvia::http_origin_view::http({.host_ = "example.com"}), {.method_ = "POST"}, head);
    if (prepared.prepared() == nullptr || prepared.prepared()->content_plan().streaming() == nullptr) {
        throw std::logic_error("cannot prepare test request content");
    }
    return ruvia::http1_request_content_writer(*prepared.prepared()->content_plan().streaming());
}

}  // namespace

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

RUVIA_TEST(http1_streaming_request_finish_accepts_trailers_borrowed_from_output) {
    enum class alias_kind { name,
        value,
        disjoint,
        overlapping,
        cross_field };
    for (const auto kind : {alias_kind::name, alias_kind::value, alias_kind::disjoint,
             alias_kind::overlapping, alias_kind::cross_field}) {
        auto writer = chunked_request_writer();
        std::array<char, 128> buffer{};
        buffer.fill('#');
        std::array<ruvia::http_header_view, 2> fields{};
        std::size_t count = 1;
        std::string expected = "0\r\nDigest: value\r\n\r\n";
        const auto copy = [&](std::size_t offset, std::string_view text) {
            std::copy(text.begin(), text.end(), buffer.begin() + static_cast<std::ptrdiff_t>(offset));
            return std::string_view(buffer.data() + offset, text.size());
        };
        switch (kind) {
            case alias_kind::name:
                fields[0] = {copy(0, "Digest"), "value"};
                break;
            case alias_kind::value:
                fields[0] = {"Digest", copy(0, "value")};
                break;
            case alias_kind::disjoint:
                fields[0] = {copy(0, "Digest"), copy(16, "value")};
                break;
            case alias_kind::overlapping: {
                const auto text = copy(0, "DigestValue");
                fields[0] = {text.substr(0, 6), text.substr(3)};
                expected = "0\r\nDigest: estValue\r\n\r\n";
                break;
            }
            case alias_kind::cross_field:
                fields[0] = {"X-First", copy(32, "first")};
                fields[1] = {copy(0, "Digest"), "second"};
                count = 2;
                expected = "0\r\nX-First: first\r\nDigest: second\r\n\r\n";
                break;
        }
        const auto trailers = std::span(fields).first(count);
        const auto original = buffer;
        const auto too_small = writer.plan_finish(std::span(buffer).first(expected.size() - 1), trailers);
        RUVIA_CHECK(too_small.index() == 1);
        if (too_small.index() == 1) {
            RUVIA_CHECK(std::get<1>(too_small) == ruvia::http1_request_content_write_error::output_too_small);
        }
        RUVIA_CHECK(buffer == original);
        RUVIA_CHECK(!writer.finished());

        const auto planned = writer.plan_finish(buffer, trailers);
        RUVIA_CHECK(planned.index() == 0);
        if (planned.index() == 0) {
            RUVIA_CHECK_EQ(std::get<0>(planned), std::string_view(expected));
        }
        RUVIA_CHECK_EQ(std::string_view(buffer.data() + expected.size(), buffer.size() - expected.size()),
            std::string_view(original.data() + expected.size(), original.size() - expected.size()));
        const auto repeated = writer.plan_finish(buffer, trailers);
        RUVIA_CHECK(repeated.index() == 1);
        if (repeated.index() == 1) {
            RUVIA_CHECK(std::get<1>(repeated) == ruvia::http1_request_content_write_error::write_pending);
        }
        RUVIA_CHECK(writer.commit_finish().index() == 0);
        RUVIA_CHECK(writer.finished());
        const auto finished = writer.plan_finish(buffer);
        RUVIA_CHECK(finished.index() == 1);
        if (finished.index() == 1) {
            RUVIA_CHECK(std::get<1>(finished) == ruvia::http1_request_content_write_error::stopped);
        }
    }
}

RUVIA_TEST(http1_streaming_request_finish_accepts_output_alias_at_trailer_limit) {
    auto writer = chunked_request_writer();
    constexpr std::size_t framing_bytes = std::string_view("0\r\nDigest: \r\n\r\n").size();
    const std::string value(ruvia::max_http_header_bytes - framing_bytes, 'v');
    std::string buffer(ruvia::max_http_header_bytes, 'v');
    const std::array trailers{ruvia::http_header_view{"Digest", std::string_view(buffer).substr(0, value.size())}};
    const auto planned = writer.plan_finish(std::span(buffer.data(), buffer.size()), trailers);
    RUVIA_CHECK(planned.index() == 0);
    if (planned.index() == 0) {
        const std::string expected = "0\r\nDigest: " + value + "\r\n\r\n";
        RUVIA_CHECK_EQ(std::get<0>(planned), std::string_view(expected));
    }
    RUVIA_CHECK(writer.commit_finish().index() == 0);
    RUVIA_CHECK(writer.finished());
}
