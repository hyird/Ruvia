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
#include "ruvia/web/detail/http/StaticFileMetadata.h"

namespace {

using ruvia::detail::guessStaticFileContentType;

std::string_view guess(const char* name) {
    return guessStaticFileContentType(std::filesystem::path(name));
}

}  // namespace

// Small field-value primitives: the weak-etag prefix and unsigned decimal output.

RUVIA_TEST(http_trim_weak_etag_prefix) {
    using ruvia::detail::httpTrimWeakEtagPrefix;
    RUVIA_CHECK_EQ(httpTrimWeakEtagPrefix("W/\"abc\""), std::string_view("\"abc\""));
    RUVIA_CHECK_EQ(httpTrimWeakEtagPrefix("\"abc\""), std::string_view("\"abc\""));  // strong etag unchanged
    RUVIA_CHECK_EQ(httpTrimWeakEtagPrefix("W/"), std::string_view(""));
    RUVIA_CHECK_EQ(httpTrimWeakEtagPrefix("W"), std::string_view("W"));  // needs both prefix chars
}

RUVIA_TEST(http_append_unsigned_decimal) {
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
