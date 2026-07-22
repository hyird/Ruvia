#include "test_harness.h"

#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/web/detail/StaticPathNormalization.h"
#include "ruvia/web/Error.h"

namespace {

using ruvia::HttpError;
using ruvia::detail::isDriveQualifiedPath;
using ruvia::detail::normalizeStaticRelativePath;

std::string normalize(std::string_view input) {
    const auto out = normalizeStaticRelativePath(input, std::pmr::new_delete_resource());
    return std::string(out.data(), out.size());
}

bool rejects(std::string_view input) {
    try {
        (void)normalizeStaticRelativePath(input, std::pmr::new_delete_resource());
        return false;
    } catch (const HttpError&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(static_path_normalizes_plain_and_dot_segments) {
    RUVIA_CHECK_EQ(normalize("a/b/c.txt"), std::string("a/b/c.txt"));
    RUVIA_CHECK_EQ(normalize("./a/./b"), std::string("a/b"));  // "." segments dropped
    RUVIA_CHECK_EQ(normalize("a//b"), std::string("a/b"));     // empty segment dropped
    RUVIA_CHECK_EQ(normalize(""), std::string(""));            // empty input -> the root itself
    RUVIA_CHECK_EQ(normalize("a/"), std::string("a"));         // trailing slash dropped
}

RUVIA_TEST(static_path_applies_dotdot_within_root) {
    RUVIA_CHECK_EQ(normalize("a/b/../c"), std::string("a/c"));        // pop one segment
    RUVIA_CHECK_EQ(normalize("a/../b"), std::string("b"));            // pop to root, then descend
    RUVIA_CHECK_EQ(normalize("a/.."), std::string(""));              // pop back to the root
    RUVIA_CHECK_EQ(normalize("a/b/../../c/d"), std::string("c/d"));   // pop two segments
}

RUVIA_TEST(static_path_rejects_escape_above_root) {
    // Any ".." that would ascend above the document root is a traversal attempt.
    RUVIA_CHECK(rejects(".."));
    RUVIA_CHECK(rejects("../etc/passwd"));
    RUVIA_CHECK(rejects("a/../.."));       // escapes after returning to root
    RUVIA_CHECK(rejects("a/b/../../.."));  // net one level above root
}

RUVIA_TEST(static_path_rejects_absolute_and_backslash_forms) {
    RUVIA_CHECK(rejects("/etc/passwd"));          // leading '/'
    RUVIA_CHECK(rejects("\\absolute\\path"));  // leading '\\'
    RUVIA_CHECK(rejects("C:\\secret"));        // drive-qualified path
    RUVIA_CHECK(rejects("c:/secret"));         // lowercase drive prefix
    // '\\' is a separator too, so backslash traversal cannot bypass the ".." guard.
    RUVIA_CHECK(rejects("a\\..\\.."));
    // A benign backslash path is normalized with '/' separators.
    RUVIA_CHECK_EQ(normalize("a\\b\\c"), std::string("a/b/c"));
}

RUVIA_TEST(static_path_only_exact_dot_segments_are_special) {
    // Only a segment equal to exactly "." or ".." is treated specially. A segment
    // that merely contains dots -- or is three-or-more dots -- is an ordinary
    // filename and must be preserved verbatim. This guards against a regression to
    // substring-based ".." detection, which a "...."-style payload could exploit to
    // either smuggle traversal or wrongly 403 a legitimate file.
    RUVIA_CHECK_EQ(normalize("..."), std::string("..."));         // three dots: a literal name
    RUVIA_CHECK_EQ(normalize("...."), std::string("...."));       // four dots
    RUVIA_CHECK_EQ(normalize("..a"), std::string("..a"));         // leading dots, not ".."
    RUVIA_CHECK_EQ(normalize("a.."), std::string("a.."));         // trailing dots, not ".."
    RUVIA_CHECK_EQ(normalize("a/.../b"), std::string("a/.../b")); // "..." between real segments
    // A real ".." still traverses even when separators are mixed around it.
    RUVIA_CHECK_EQ(normalize("a\\b/../c"), std::string("a/c"));
}

RUVIA_TEST(static_path_drive_qualified_predicate) {
    RUVIA_CHECK(isDriveQualifiedPath("C:"));
    RUVIA_CHECK(isDriveQualifiedPath("z:/x"));
    RUVIA_CHECK(!isDriveQualifiedPath("C"));      // too short to hold a drive spec
    RUVIA_CHECK(!isDriveQualifiedPath("1:/x"));   // drive letter must be alphabetic
    RUVIA_CHECK(!isDriveQualifiedPath("ab:/x"));  // ':' must be the second character
}
