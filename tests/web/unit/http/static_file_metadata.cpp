#include "test_harness.h"

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/detail/field/HttpDate.h"
#include "ruvia/http/detail/field/HttpEntityTag.h"
#include "ruvia/web/detail/http/static/StaticFileMetadata.h"

namespace {

using ruvia::detail::guessStaticFileContentType;

std::string_view guess(const char* name) {
    return guessStaticFileContentType(std::filesystem::path(name));
}

std::string fileEtag(
    std::uint64_t size, std::uint64_t modifiedToken, ruvia::detail::ResponseFileIdentity identity) {
    const auto out = ruvia::detail::makeStaticFileSnapshotEtag(
        std::pmr::get_default_resource(), size, modifiedToken, identity);
    return std::string(out.data(), out.size());
}

}  // namespace

// Deriving a static file's content type from its extension.

RUVIA_TEST(content_type_guessing) {
    RUVIA_CHECK_EQ(guess("index.html"), std::string_view("text/html; charset=utf-8"));
    RUVIA_CHECK_EQ(guess("page.htm"), std::string_view("text/html; charset=utf-8"));
    RUVIA_CHECK_EQ(guess("style.css"), std::string_view("text/css; charset=utf-8"));
    RUVIA_CHECK_EQ(guess("app.js"), std::string_view("text/javascript; charset=utf-8"));
    RUVIA_CHECK_EQ(guess("mod.mjs"), std::string_view("text/javascript; charset=utf-8"));
    RUVIA_CHECK_EQ(guess("data.json"), std::string_view("application/json; charset=utf-8"));
    RUVIA_CHECK_EQ(guess("photo.png"), std::string_view("image/png"));
    RUVIA_CHECK_EQ(guess("photo.jpeg"), std::string_view("image/jpeg"));
    // ".jpg" is a distinct token in the same branch as ".jpeg" and is the far more
    // common spelling -- pin it so a regression can't silently serve it as a
    // download (octet-stream) instead of an image.
    RUVIA_CHECK_EQ(guess("photo.jpg"), std::string_view("image/jpeg"));
    RUVIA_CHECK_EQ(guess("anim.gif"), std::string_view("image/gif"));
    RUVIA_CHECK_EQ(guess("notes.txt"), std::string_view("text/plain; charset=utf-8"));
    RUVIA_CHECK_EQ(guess("server.log"), std::string_view("text/plain; charset=utf-8"));
    RUVIA_CHECK_EQ(guess("icon.svg"), std::string_view("image/svg+xml"));
    RUVIA_CHECK_EQ(guess("mod.wasm"), std::string_view("application/wasm"));
    // The extension match is case-insensitive end to end, so an uppercase extension
    // maps just like its lowercase form (not to the octet-stream fallback).
    RUVIA_CHECK_EQ(guess("PHOTO.PNG"), std::string_view("image/png"));
    RUVIA_CHECK_EQ(guess("PAGE.HTML"), std::string_view("text/html; charset=utf-8"));
    // Unknown extension and no extension fall back to a non-executable octet-stream.
    RUVIA_CHECK_EQ(guess("archive.xyz"), std::string_view("application/octet-stream"));
    RUVIA_CHECK_EQ(guess("noext"), std::string_view("application/octet-stream"));
}

RUVIA_TEST(http_extension_equals_is_case_insensitive) {
    using ruvia::detail::staticFileExtensionEquals;
    RUVIA_CHECK(staticFileExtensionEquals(std::string_view("html"), "html"));
    RUVIA_CHECK(staticFileExtensionEquals(std::string_view("HTML"), "html"));
    RUVIA_CHECK(staticFileExtensionEquals(std::string_view("Json"), "json"));
    RUVIA_CHECK(staticFileExtensionEquals(std::string_view(""), ""));
    RUVIA_CHECK(!staticFileExtensionEquals(std::string_view("htm"), "html"));
    RUVIA_CHECK(!staticFileExtensionEquals(std::string_view("jpeg"), "json"));
}

RUVIA_TEST(static_file_append_unsigned_decimal) {
    using ruvia::detail::appendStaticFileUnsigned;
    std::pmr::string output(std::pmr::get_default_resource());
    appendStaticFileUnsigned(output, 0);
    RUVIA_CHECK_EQ(std::string_view(output), std::string_view("0"));
    output.clear();
    appendStaticFileUnsigned(output, 12345);
    RUVIA_CHECK_EQ(std::string_view(output), std::string_view("12345"));
    // Appends onto existing content rather than replacing it.
    appendStaticFileUnsigned(output, 67);
    RUVIA_CHECK_EQ(std::string_view(output), std::string_view("1234567"));
    // The 64-bit maximum.
    output.clear();
    appendStaticFileUnsigned(output, (std::numeric_limits<std::uint64_t>::max)());
    RUVIA_CHECK_EQ(std::string_view(output), std::string_view("18446744073709551615"));
}

RUVIA_TEST(static_file_etag_deterministic_and_sensitive) {
    const auto identity = ruvia::detail::ResponseFileIdentity::checked({1, 2, 3, 4});
    const auto replacement = ruvia::detail::ResponseFileIdentity::checked({1, 2, 3, 5});
    const auto base = fileEtag(100, 123456, identity);
    // The strong validator binds framing metadata and the exact indexed file.
    RUVIA_CHECK_EQ(base, std::string("\"100-123456-1-2-3-4\""));
    RUVIA_CHECK_EQ(base, fileEtag(100, 123456, identity));
    RUVIA_CHECK(base != fileEtag(101, 123456, identity));
    RUVIA_CHECK(base != fileEtag(100, 123457, identity));
    RUVIA_CHECK(base != fileEtag(100, 123456, replacement));
    RUVIA_CHECK(base.size() >= 2 && base.front() == '"' && base.back() == '"');  // quoted-string
}
