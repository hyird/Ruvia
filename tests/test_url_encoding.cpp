#include <doctest/doctest.h>
#include "ruvia/http/UrlEncoding.h"

using namespace ruvia::detail;

// ─── hasUrlEncoding ────────────────────────────────────────────────────────

TEST_SUITE("UrlEncoding / hasUrlEncoding") {

TEST_CASE("plain string → false") {
    CHECK_FALSE(hasUrlEncoding("hello", UrlDecodeMode::kPercent));
}

TEST_CASE("percent-encoded → true") {
    CHECK(hasUrlEncoding("hello%20world", UrlDecodeMode::kPercent));
}

TEST_CASE("plus in form mode → true") {
    CHECK(hasUrlEncoding("hello+world", UrlDecodeMode::kForm));
}

TEST_CASE("plus in percent mode → false") {
    CHECK_FALSE(hasUrlEncoding("hello+world", UrlDecodeMode::kPercent));
}

TEST_CASE("empty string → false") {
    CHECK_FALSE(hasUrlEncoding("", UrlDecodeMode::kPercent));
}

}  // TEST_SUITE

// ─── validateUrlEncoding ───────────────────────────────────────────────────

TEST_SUITE("UrlEncoding / validateUrlEncoding") {

TEST_CASE("valid percent-encoded") {
    CHECK(validateUrlEncoding("hello%20world"));
    CHECK(validateUrlEncoding("%3D"));
    CHECK(validateUrlEncoding("a%2Bb"));
}

TEST_CASE("incomplete percent sequence rejected") {
    CHECK_FALSE(validateUrlEncoding("hello%2"));
    CHECK_FALSE(validateUrlEncoding("hello%"));
    CHECK_FALSE(validateUrlEncoding("%2"));
}

TEST_CASE("invalid hex digits rejected") {
    CHECK_FALSE(validateUrlEncoding("%GG"));
    CHECK_FALSE(validateUrlEncoding("%2G"));
    CHECK_FALSE(validateUrlEncoding("%G2"));
}

TEST_CASE("no encoding is valid") {
    CHECK(validateUrlEncoding("hello"));
    CHECK(validateUrlEncoding(""));
}

}  // TEST_SUITE

// ─── decodeUrlComponent ────────────────────────────────────────────────────

TEST_SUITE("UrlEncoding / decodeUrlComponent") {

TEST_CASE("percent decoding") {
    std::string out;
    REQUIRE(decodeUrlComponent("hello%20world", out, UrlDecodeMode::kPercent));
    CHECK(out == "hello world");
}

TEST_CASE("form plus decoding") {
    std::string out;
    REQUIRE(decodeUrlComponent("hello+world", out, UrlDecodeMode::kForm));
    CHECK(out == "hello world");
}

TEST_CASE("plus in percent mode not decoded") {
    std::string out;
    REQUIRE(decodeUrlComponent("hello+world", out, UrlDecodeMode::kPercent));
    CHECK(out == "hello+world");
}

TEST_CASE("multiple encoded sequences") {
    std::string out;
    REQUIRE(decodeUrlComponent("a%3Db%26c%3D1", out, UrlDecodeMode::kPercent));
    CHECK(out == "a=b&c=1");
}

TEST_CASE("null byte decoding") {
    std::string out;
    REQUIRE(decodeUrlComponent("%00", out, UrlDecodeMode::kPercent));
    REQUIRE(out.size() == 1);
    CHECK(out[0] == '\0');
}

TEST_CASE("incomplete percent sequence fails") {
    std::string out;
    CHECK_FALSE(decodeUrlComponent("hello%2", out, UrlDecodeMode::kPercent));
}

TEST_CASE("invalid hex fails") {
    std::string out;
    CHECK_FALSE(decodeUrlComponent("hello%GG", out, UrlDecodeMode::kPercent));
}

TEST_CASE("empty string decodes to empty") {
    std::string out;
    REQUIRE(decodeUrlComponent("", out, UrlDecodeMode::kPercent));
    CHECK(out.empty());
}

TEST_CASE("uppercase hex accepted") {
    std::string out;
    REQUIRE(decodeUrlComponent("%2F", out, UrlDecodeMode::kPercent));
    CHECK(out == "/");
}

TEST_CASE("lowercase hex accepted") {
    std::string out;
    REQUIRE(decodeUrlComponent("%2f", out, UrlDecodeMode::kPercent));
    CHECK(out == "/");
}

}  // TEST_SUITE

// ─── urlComponentEquals ────────────────────────────────────────────────────

TEST_SUITE("UrlEncoding / urlComponentEquals") {

TEST_CASE("no encoding, equal") {
    CHECK(urlComponentEquals("hello", "hello", UrlDecodeMode::kPercent));
}

TEST_CASE("encoded equals decoded") {
    CHECK(urlComponentEquals("hello%20world", "hello world", UrlDecodeMode::kPercent));
}

TEST_CASE("form plus equals space") {
    CHECK(urlComponentEquals("hello+world", "hello world", UrlDecodeMode::kForm));
}

TEST_CASE("different values") {
    CHECK_FALSE(urlComponentEquals("hello", "world", UrlDecodeMode::kPercent));
}

TEST_CASE("encoded different length") {
    CHECK_FALSE(urlComponentEquals("hello%20", "hello", UrlDecodeMode::kPercent));
}

TEST_CASE("invalid encoding") {
    CHECK_FALSE(urlComponentEquals("hello%GG", "helloGG", UrlDecodeMode::kPercent));
}

}  // TEST_SUITE
