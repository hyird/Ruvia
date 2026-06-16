#include <doctest/doctest.h>
#include "ruvia/http/Error.h"
#include "ruvia/http/HttpStatus.h"

using namespace ruvia;

// ─── defaultStatusText ─────────────────────────────────────────────────────

TEST_SUITE("Error / defaultStatusText") {

TEST_CASE("common status codes have correct text") {
    CHECK(defaultStatusText(200) == "OK");
    CHECK(defaultStatusText(201) == "Created");
    CHECK(defaultStatusText(204) == "No Content");
    CHECK(defaultStatusText(206) == "Partial Content");
    CHECK(defaultStatusText(301) == "Moved Permanently");
    CHECK(defaultStatusText(302) == "Found");
    CHECK(defaultStatusText(304) == "Not Modified");
    CHECK(defaultStatusText(400) == "Bad Request");
    CHECK(defaultStatusText(401) == "Unauthorized");
    CHECK(defaultStatusText(403) == "Forbidden");
    CHECK(defaultStatusText(404) == "Not Found");
    CHECK(defaultStatusText(405) == "Method Not Allowed");
    CHECK(defaultStatusText(413) == "Payload Too Large");
    CHECK(defaultStatusText(429) == "Too Many Requests");
    CHECK(defaultStatusText(431) == "Request Header Fields Too Large");
    CHECK(defaultStatusText(500) == "Internal Server Error");
    CHECK(defaultStatusText(501) == "Not Implemented");
    CHECK(defaultStatusText(505) == "HTTP Version Not Supported");
}

TEST_CASE("unknown 4xx code falls back to Bad Request") {
    CHECK(defaultStatusText(499) == "Bad Request");
}

TEST_CASE("unknown 5xx code falls back to Internal Server Error") {
    CHECK(defaultStatusText(599) == "Internal Server Error");
}

}  // TEST_SUITE

// ─── defaultErrorCode ──────────────────────────────────────────────────────

TEST_SUITE("Error / defaultErrorCode") {

TEST_CASE("common error codes") {
    CHECK(defaultErrorCode(400) == "bad_request");
    CHECK(defaultErrorCode(401) == "unauthorized");
    CHECK(defaultErrorCode(403) == "forbidden");
    CHECK(defaultErrorCode(404) == "not_found");
    CHECK(defaultErrorCode(405) == "method_not_allowed");
    CHECK(defaultErrorCode(409) == "conflict");
    CHECK(defaultErrorCode(412) == "precondition_failed");
    CHECK(defaultErrorCode(413) == "payload_too_large");
    CHECK(defaultErrorCode(416) == "range_not_satisfiable");
    CHECK(defaultErrorCode(417) == "expectation_failed");
    CHECK(defaultErrorCode(422) == "unprocessable_entity");
    CHECK(defaultErrorCode(429) == "too_many_requests");
    CHECK(defaultErrorCode(431) == "request_header_fields_too_large");
    CHECK(defaultErrorCode(500) == "internal_error");
    CHECK(defaultErrorCode(501) == "not_implemented");
    CHECK(defaultErrorCode(505) == "http_version_not_supported");
}

TEST_CASE("503 returns service_unavailable — our fix") {
    // 429 was wrongly used for connection-limit; we changed it to 503
    CHECK(defaultErrorCode(503) == "service_unavailable");
}

TEST_CASE("unknown 4xx code falls back to bad_request") {
    CHECK(defaultErrorCode(499) == "bad_request");
}

TEST_CASE("unknown 5xx code falls back to internal_error") {
    CHECK(defaultErrorCode(599) == "internal_error");
}

}  // TEST_SUITE

// ─── makeErrorResponse ─────────────────────────────────────────────────────

TEST_SUITE("Error / makeErrorResponse") {

TEST_CASE("error response has correct status code") {
    auto resp = makeErrorResponse(
        std::pmr::get_default_resource(),
        HttpErrorInfo{.statusCode = 404, .message = "resource not found"});
    CHECK(resp.statusCode() == 404);
    CHECK(resp.statusText() == "Not Found");
}

TEST_CASE("error response body is JSON") {
    auto resp = makeErrorResponse(
        std::pmr::get_default_resource(),
        HttpErrorInfo{.statusCode = 400, .code = "bad_input", .message = "field is required"});
    const auto body = resp.bodyBytes();
    CHECK(body.find('"') != std::string_view::npos);
    CHECK(body.find("bad_input") != std::string_view::npos);
    CHECK(body.find("field is required") != std::string_view::npos);
}

TEST_CASE("error response Content-Type is application/json") {
    auto resp = makeErrorResponse(
        std::pmr::get_default_resource(),
        HttpErrorInfo{.statusCode = 500});
    const auto ct = resp.header("Content-Type");
    CHECK(ct.find("application/json") != std::string_view::npos);
}

TEST_CASE("closeConnection adds Connection: close header") {
    auto resp = makeErrorResponse(
        std::pmr::get_default_resource(),
        HttpErrorInfo{.statusCode = 400},
        true);
    CHECK(resp.header("Connection") == "close");
}

TEST_CASE("no closeConnection has no Connection header") {
    auto resp = makeErrorResponse(
        std::pmr::get_default_resource(),
        HttpErrorInfo{.statusCode = 400},
        false);
    CHECK(resp.header("Connection").empty());
}

TEST_CASE("auto-filled code when empty") {
    auto resp = makeErrorResponse(
        std::pmr::get_default_resource(),
        HttpErrorInfo{.statusCode = 404, .message = "oops"});
    CHECK(resp.bodyBytes().find("not_found") != std::string_view::npos);
}

TEST_CASE("auto-filled statusText when empty") {
    auto resp = makeErrorResponse(
        std::pmr::get_default_resource(),
        HttpErrorInfo{.statusCode = 404});
    CHECK(resp.statusText() == "Not Found");
}

TEST_CASE("auto-filled message from statusText when empty") {
    auto resp = makeErrorResponse(
        std::pmr::get_default_resource(),
        HttpErrorInfo{.statusCode = 500});
    CHECK_FALSE(resp.bodyBytes().empty());
}

}  // TEST_SUITE

// ─── HttpStatusEntry ───────────────────────────────────────────────────────

TEST_SUITE("Error / HttpStatus cached status lines") {

TEST_CASE("200 OK has a cached status line") {
    const auto* e = detail::httpStatusEntry(200);
    REQUIRE(e != nullptr);
    CHECK(e->code == 200);
    CHECK(e->text == "OK");
    CHECK(e->line == "HTTP/1.1 200 OK\r\n");
}

TEST_CASE("unknown status has no cached entry") {
    CHECK(detail::httpStatusEntry(418) != nullptr);  // teapot is known
    CHECK(detail::httpStatusEntry(999) == nullptr);  // 999 not cached
}

TEST_CASE("httpCachedStatusLine returns pre-built line for known codes") {
    const auto line = detail::httpCachedStatusLine(200, "OK");
    CHECK(line == "HTTP/1.1 200 OK\r\n");
}

TEST_CASE("httpCachedStatusLine returns empty when text mismatches") {
    // If user sets custom status text, we cannot use the cached line
    const auto line = detail::httpCachedStatusLine(200, "Success");
    CHECK(line.empty());
}

}  // TEST_SUITE

// ─── HttpError exception ───────────────────────────────────────────────────

TEST_SUITE("Error / HttpError exception") {

TEST_CASE("what() returns message") {
    HttpError e(404, "not_found", "the resource was not found");
    CHECK(std::string_view(e.what()) == "the resource was not found");
}

TEST_CASE("info() returns all fields") {
    HttpError e(422, "validation_error", "invalid input", "Unprocessable Entity");
    const auto info = e.info();
    CHECK(info.statusCode == 422);
    CHECK(info.code == "validation_error");
    CHECK(info.message == "invalid input");
    CHECK(info.statusText == "Unprocessable Entity");
}

TEST_CASE("HttpError is throwable and catchable") {
    bool caught = false;
    try {
        throw HttpError(500, "internal_error", "oops");
    } catch (const HttpError& ex) {
        caught = true;
        CHECK(ex.info().statusCode == 500);
    }
    CHECK(caught);
}

}  // TEST_SUITE
