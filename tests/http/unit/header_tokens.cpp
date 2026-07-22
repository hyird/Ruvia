#include "test_harness.h"

#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/HttpMediaType.h"
#include "ruvia/http/detail/HttpQualityValue.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/HttpConnectionFields.h"
#include "ruvia/http/detail/HttpEntityTag.h"
#include "ruvia/http/detail/HttpOws.h"
#include "ruvia/http/detail/HttpTransferEncoding.h"
#include "ruvia/http/detail/parser/MimeFieldGrammar.h"
#include "ruvia/http/detail/http2/Http2FramePayload.h"
#include "ruvia/http/detail/websocket/HttpWebSocketHandshakeFields.h"

namespace {

using ruvia::detail::httpHasExactToken;
using ruvia::detail::httpHasToken;
using ruvia::detail::httpTrimQuotes;

struct MatchAnyHeaderToken final {
    [[nodiscard]] constexpr bool operator()(std::string_view) const noexcept {
        return true;
    }
};

template <typename Input>
concept AcceptsAnyBorrowedHttpSubviewInput =
    requires(Input&& input) {
        ruvia::detail::httpTrimOws(std::forward<Input>(input));
    } ||
    requires(Input&& input) {
        ruvia::detail::httpTrimQuotes(std::forward<Input>(input));
    } ||
    requires(Input&& input) {
        ruvia::detail::httpFindHeaderToken(
            std::forward<Input>(input),
            MatchAnyHeaderToken{});
    } ||
    requires(Input&& input) {
        ruvia::detail::httpHeaderTokenBeforeParameters(
            std::forward<Input>(input));
    } ||
    requires(Input&& input) {
        ruvia::detail::httpMediaTypeOnly(std::forward<Input>(input));
    } ||
    requires(Input&& input) {
        ruvia::detail::httpTrimWeakEtagPrefix(
            std::forward<Input>(input));
    } ||
    requires(const ruvia::HttpRequest& request, Input&& input) {
        ruvia::detail::chooseWebSocketSubprotocol(
            request,
            std::forward<Input>(input));
    };

template <typename Input>
concept AcceptsAllBorrowedHttpSubviewInputs = requires(
    const ruvia::HttpRequest& request,
    Input&& input) {
    ruvia::detail::httpTrimOws(std::forward<Input>(input));
    ruvia::detail::httpTrimQuotes(std::forward<Input>(input));
    ruvia::detail::httpFindHeaderToken(
        std::forward<Input>(input),
        MatchAnyHeaderToken{});
    ruvia::detail::httpHeaderTokenBeforeParameters(
        std::forward<Input>(input));
    ruvia::detail::httpMediaTypeOnly(std::forward<Input>(input));
    ruvia::detail::httpTrimWeakEtagPrefix(std::forward<Input>(input));
    ruvia::detail::chooseWebSocketSubprotocol(
        request,
        std::forward<Input>(input));
};

static_assert(!AcceptsAnyBorrowedHttpSubviewInput<std::string>);
static_assert(!AcceptsAnyBorrowedHttpSubviewInput<const std::string>);
static_assert(!AcceptsAnyBorrowedHttpSubviewInput<std::pmr::string>);
static_assert(AcceptsAllBorrowedHttpSubviewInputs<std::string&>);
static_assert(AcceptsAllBorrowedHttpSubviewInputs<std::pmr::string&>);
static_assert(AcceptsAllBorrowedHttpSubviewInputs<std::string_view>);

template <typename Input>
concept AcceptsAnyBorrowedHttpParserOutputInput =
    requires(
        Input&& input,
        ruvia::detail::HttpMediaTypeParts& mediaType,
        std::string_view& first,
        std::string_view& second,
        bool& flag,
        ruvia::detail::HttpUpgradeProtocol& protocol,
        const ruvia::detail::Http2FrameHeader& frame) {
        ruvia::detail::httpParseMediaTypeParts(
            std::forward<Input>(input), false, mediaType);
    } ||
    requires(
        Input&& input,
        ruvia::detail::HttpMediaTypeParts& mediaType) {
        ruvia::detail::httpParseMediaType(
            std::forward<Input>(input), false, mediaType);
    } ||
    requires(Input&& input, std::string_view& first, std::string_view& second) {
        ruvia::detail::httpParseMimeParameter(
            std::forward<Input>(input), first, second);
    } ||
    requires(Input&& input, std::string_view& first, bool& flag) {
        ruvia::detail::httpParseTransferCodingSyntax(
            std::forward<Input>(input), first, flag);
    } ||
    requires(Input&& input, ruvia::detail::HttpUpgradeProtocol& protocol) {
        ruvia::detail::httpParseUpgradeProtocol(
            std::forward<Input>(input), protocol);
    } ||
    requires(
        Input&& input,
        const ruvia::detail::Http2FrameHeader& frame,
        std::string_view& first) {
        ruvia::detail::http2StripPadAndPriority(
            frame, std::forward<Input>(input), false, first);
    } ||
    requires(
        Input&& input,
        const ruvia::detail::Http2FrameHeader& frame,
        std::string_view& first) {
        ruvia::detail::http2DecodeHeadersPayload(
            frame, std::forward<Input>(input), first);
    } ||
    requires(
        Input&& input,
        const ruvia::detail::Http2FrameHeader& frame,
        std::string_view& first) {
        ruvia::detail::http2DecodeDataPayload(
            frame, std::forward<Input>(input), first);
    };

template <typename Input>
concept AcceptsAllBorrowedHttpParserOutputInputs = requires(
    Input&& input,
    ruvia::detail::HttpMediaTypeParts& mediaType,
    std::string_view& first,
    std::string_view& second,
    bool& flag,
    ruvia::detail::HttpUpgradeProtocol& protocol,
    const ruvia::detail::Http2FrameHeader& frame) {
    ruvia::detail::httpParseMediaTypeParts(
        std::forward<Input>(input), false, mediaType);
    ruvia::detail::httpParseMediaType(
        std::forward<Input>(input), false, mediaType);
    ruvia::detail::httpParseMimeParameter(
        std::forward<Input>(input), first, second);
    ruvia::detail::httpParseTransferCodingSyntax(
        std::forward<Input>(input), first, flag);
    ruvia::detail::httpParseUpgradeProtocol(
        std::forward<Input>(input), protocol);
    ruvia::detail::http2StripPadAndPriority(
        frame, std::forward<Input>(input), false, first);
    ruvia::detail::http2DecodeHeadersPayload(
        frame, std::forward<Input>(input), first);
    ruvia::detail::http2DecodeDataPayload(
        frame, std::forward<Input>(input), first);
};

static_assert(!AcceptsAnyBorrowedHttpParserOutputInput<std::string>);
static_assert(!AcceptsAnyBorrowedHttpParserOutputInput<const std::string>);
static_assert(!AcceptsAnyBorrowedHttpParserOutputInput<std::pmr::string>);
static_assert(AcceptsAllBorrowedHttpParserOutputInputs<std::string&>);
static_assert(AcceptsAllBorrowedHttpParserOutputInputs<std::pmr::string&>);
static_assert(AcceptsAllBorrowedHttpParserOutputInputs<std::string_view>);

}  // namespace

RUVIA_TEST(header_has_token_case_insensitive) {
    RUVIA_CHECK(httpHasToken("gzip, deflate", "deflate"));
    RUVIA_CHECK(httpHasToken("gzip, deflate", "GZIP"));            // case-insensitive
    RUVIA_CHECK(httpHasToken("keep-alive, Upgrade", "upgrade"));
    RUVIA_CHECK(httpHasToken("  gzip  ,  deflate ", "deflate"));   // surrounding OWS tolerated
    RUVIA_CHECK(httpHasToken("gzip", "gzip"));                     // a single token
    RUVIA_CHECK(!httpHasToken("gzip, deflate", "br"));
    RUVIA_CHECK(!httpHasToken("gzipx", "gzip"));                   // a substring is not a token
    RUVIA_CHECK(!httpHasToken("gzip", ""));                        // empty expected
    RUVIA_CHECK(!httpHasToken("", "gzip"));                        // empty value
}

RUVIA_TEST(header_has_token_skips_empty_list_items) {
    // Doubled, leading, and trailing commas (which real proxies emit) produce
    // empty list items that must be skipped -- not matched, and not stopping the
    // scan from reaching the real tokens.
    RUVIA_CHECK(httpHasToken("gzip,,deflate", "deflate"));
    RUVIA_CHECK(httpHasToken(",gzip", "gzip"));
    RUVIA_CHECK(httpHasToken("gzip,", "gzip"));
    RUVIA_CHECK(httpHasToken(" , gzip , ", "gzip"));
    // A list of only empty items never matches anything.
    RUVIA_CHECK(!httpHasToken(",,", "gzip"));
}

RUVIA_TEST(header_has_exact_token_case_sensitive) {
    RUVIA_CHECK(httpHasExactToken("gzip, deflate", "deflate"));
    RUVIA_CHECK(!httpHasExactToken("gzip, DEFLATE", "deflate"));   // case-sensitive
    RUVIA_CHECK(httpHasExactToken("a, b, c", "b"));
    RUVIA_CHECK(!httpHasExactToken("a, b, c", "d"));
}

RUVIA_TEST(header_trim_quotes) {
    RUVIA_CHECK_EQ(httpTrimQuotes("\"abc\""), std::string_view("abc"));
    RUVIA_CHECK_EQ(httpTrimQuotes("abc"), std::string_view("abc"));      // no quotes
    RUVIA_CHECK_EQ(httpTrimQuotes("\"\""), std::string_view(""));         // empty quoted
    RUVIA_CHECK_EQ(httpTrimQuotes("\""), std::string_view("\""));         // one quote is too short
    RUVIA_CHECK_EQ(httpTrimQuotes("\"abc"), std::string_view("\"abc"));   // only a leading quote
}

RUVIA_TEST(header_decode_quoted_pairs) {
    // RFC 7230 §3.2.6: inside a quoted-string, "\X" represents the octet X. The
    // input is already quote-trimmed; a valid unquoted token has no backslash, so
    // every '\' is an escape (a trailing lone '\' from malformed input is kept).
    auto* resource = std::pmr::get_default_resource();
    const auto decode = [resource](std::string_view value) {
        std::pmr::string out(resource);
        ruvia::detail::httpAppendDecodedQuotedPairs(out, value);
        return std::string(out.data(), out.size());
    };
    RUVIA_CHECK_EQ(decode("plain"), std::string("plain"));      // no escapes -> unchanged
    RUVIA_CHECK_EQ(decode("a\\\"b"), std::string("a\"b"));      // \" -> "
    RUVIA_CHECK_EQ(decode("x\\\\y"), std::string("x\\y"));      // two backslashes -> one
    RUVIA_CHECK_EQ(decode("\\a\\b\\c"), std::string("abc"));    // each pair unescaped
    RUVIA_CHECK_EQ(decode("end\\"), std::string("end\\"));      // trailing lone '\' kept verbatim
}
