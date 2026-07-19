#include "test_harness.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/Http1InterimResponseWriter.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpInterimResponse.h"
#include "ruvia/http/HttpLimits.h"

namespace {

using ruvia::Http1InterimResponsePrepareError;
using ruvia::Http1InterimResponseWriter;
using ruvia::Http1InterimConnectionDisposition;
using ruvia::HttpHeaderView;
using ruvia::HttpInterimResponseHead;

template <typename T>
concept HasAnyRvalueHttp1InterimResponsePrepareAccessor =
    requires(T&& result) { std::move(result).bufferTooSmall(); } ||
    requires(T&& result) { std::move(result).prepared(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasResultKindDiscriminator = requires(const T& result) {
    result.kind();
};

template <typename T>
concept HasBooleanFinalConnectionClose = requires(const T& prepared) {
    prepared.requiresFinalConnectionClose();
};

static_assert(!HasAnyRvalueHttp1InterimResponsePrepareAccessor<
    ruvia::Http1InterimResponsePrepareResult>);
static_assert(!HasResultKindDiscriminator<
    ruvia::Http1InterimResponsePrepareResult>);
static_assert(!HasBooleanFinalConnectionClose<
    ruvia::PreparedHttp1InterimResponse>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::PreparedHttp1InterimResponse&>()
                 .connectionDisposition()),
    Http1InterimConnectionDisposition>);

[[nodiscard]] bool unchanged(
    const std::array<char, 64>& buffer,
    char sentinel) {
    return std::ranges::all_of(buffer, [sentinel](char value) {
        return value == sentinel;
    });
}

}  // namespace

RUVIA_TEST(http1_interim_response_writer_emits_exact_typed_head) {
    std::array<char, 64> buffer{};
    const auto result = Http1InterimResponseWriter().prepare(
        HttpInterimResponseHead(ruvia::http_status::kContinue), buffer);
    const auto* const prepared = result.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared != nullptr) {
        RUVIA_CHECK_EQ(
            prepared->head(),
            std::string_view("HTTP/1.1 100 Continue\r\n\r\n"));
        RUVIA_CHECK_EQ(
            prepared->connectionDisposition(),
            Http1InterimConnectionDisposition::kUnchanged);
    }

    const HttpHeaderView hints[] = {
        {"Link", "</style.css>; rel=preload"},
        {"Content-Type", "text/html; charset=utf-8"},
        {"X-Hint", "warm"},
    };
    std::array<char, 256> hintsBuffer{};
    const auto hintsResult = Http1InterimResponseWriter().prepare(
        HttpInterimResponseHead(ruvia::http_status::kEarlyHints, hints), hintsBuffer);
    const auto* const preparedHints = hintsResult.prepared();
    RUVIA_CHECK(preparedHints != nullptr);
    if (preparedHints != nullptr) {
        RUVIA_CHECK_EQ(
            preparedHints->head(),
            std::string_view(
                "HTTP/1.1 103 Early Hints\r\n"
                "Link: </style.css>; rel=preload\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "X-Hint: warm\r\n\r\n"));
        RUVIA_CHECK(!preparedHints->head().contains("Server:"));
        RUVIA_CHECK(!preparedHints->head().contains("Date:"));
    }
}

RUVIA_TEST(http1_interim_response_writer_preserves_required_status_line_space) {
    std::array<char, 32> buffer{};
    const auto result = Http1InterimResponseWriter().prepare(
        HttpInterimResponseHead(ruvia::HttpStatusCode::fromValue(199)), buffer);
    const auto* const prepared = result.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared != nullptr) {
        // RFC 9112 section 4 requires the SP after status-code even when the
        // optional reason phrase is absent.
        RUVIA_CHECK_EQ(
            prepared->head(),
            std::string_view("HTTP/1.1 199 \r\n\r\n"));
    }
}

RUVIA_TEST(http1_interim_response_writer_closes_after_containing_response) {
    const HttpHeaderView fields[] = {
        {"Connection", "close, Upgrade"},
        {"Upgrade", "example/1"},
    };
    std::array<char, 128> buffer{};
    const auto result = Http1InterimResponseWriter().prepare(
        HttpInterimResponseHead(ruvia::http_status::kEarlyHints, fields), buffer);
    const auto* const prepared = result.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared != nullptr) {
        RUVIA_CHECK_EQ(
            prepared->connectionDisposition(),
            Http1InterimConnectionDisposition::kCloseAfterInterimResponse);
        RUVIA_CHECK(prepared->head().contains("Upgrade: example/1\r\n"));
    }
}

RUVIA_TEST(http1_interim_response_writer_buffer_too_small_is_transactional) {
    std::array<char, 8> buffer;
    buffer.fill('#');
    const auto result = Http1InterimResponseWriter().prepare(
        HttpInterimResponseHead(ruvia::http_status::kContinue), buffer);
    const auto* const tooSmall = result.bufferTooSmall();
    RUVIA_CHECK(tooSmall != nullptr);
    if (tooSmall != nullptr) {
        RUVIA_CHECK_EQ(tooSmall->requiredHeadBytes(), std::size_t{25});
    }
    RUVIA_CHECK(std::ranges::all_of(buffer, [](char value) {
        return value == '#';
    }));
}

RUVIA_TEST(http1_interim_response_writer_rejects_invalid_fields_transactionally) {
    const auto rejects = [&](
        std::span<const HttpHeaderView> fields,
        Http1InterimResponsePrepareError expected) {
        std::array<char, 64> buffer;
        buffer.fill('@');
        const auto result = Http1InterimResponseWriter().prepare(
            HttpInterimResponseHead(ruvia::http_status::kEarlyHints, fields), buffer);
        const auto* const failure = result.failure();
        return failure != nullptr &&
            failure->error() == expected &&
            unchanged(buffer, '@');
    };

    const HttpHeaderView malformedName[] = {{"Bad Name", "value"}};
    const HttpHeaderView malformedValue[] = {{"X-Test", "a\r\nb"}};
    const HttpHeaderView malformedContentEncoding[] = {
        {"Content-Encoding", "gzip;level=9"}};
    const HttpHeaderView emptyContentEncoding[] = {
        {"Content-Encoding", ""}};
    const HttpHeaderView malformedContentType[] = {
        {"Content-Type", "not a media type"}};
    const HttpHeaderView emptyContentType[] = {
        {"Content-Type", ""}};
    const HttpHeaderView contentLength[] = {{"Content-Length", "0"}};
    const HttpHeaderView transferEncoding[] = {{"Transfer-Encoding", "chunked"}};
    const HttpHeaderView trailer[] = {{"Trailer", "X-Checksum"}};
    const HttpHeaderView te[] = {{"TE", "trailers"}};
    const HttpHeaderView duplicateServer[] = {
        {"Server", "one"},
        {"server", "two"},
    };
    const HttpHeaderView invalidConnection[] = {{"Connection", ","}};
    const HttpHeaderView managedConnection[] = {
        {"Connection", "close, date"},
    };
    const HttpHeaderView invalidUpgrade[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "bad protocol"},
    };
    const HttpHeaderView unlistedUpgrade[] = {{"Upgrade", "example/1"}};

    RUVIA_CHECK(rejects(
        malformedName, Http1InterimResponsePrepareError::kInvalidHeader));
    RUVIA_CHECK(rejects(
        malformedValue, Http1InterimResponsePrepareError::kInvalidHeader));
    RUVIA_CHECK(rejects(
        malformedContentEncoding,
        Http1InterimResponsePrepareError::kInvalidHeader));
    RUVIA_CHECK(rejects(
        emptyContentEncoding,
        Http1InterimResponsePrepareError::kInvalidHeader));
    RUVIA_CHECK(rejects(
        malformedContentType,
        Http1InterimResponsePrepareError::kInvalidHeader));
    RUVIA_CHECK(rejects(
        emptyContentType,
        Http1InterimResponsePrepareError::kInvalidHeader));
    RUVIA_CHECK(rejects(
        contentLength,
        Http1InterimResponsePrepareError::kContentLengthForbidden));
    RUVIA_CHECK(rejects(
        transferEncoding,
        Http1InterimResponsePrepareError::kTransferEncodingForbidden));
    RUVIA_CHECK(rejects(
        trailer, Http1InterimResponsePrepareError::kTrailerForbidden));
    RUVIA_CHECK(rejects(
        te, Http1InterimResponsePrepareError::kTeFieldForbidden));
    RUVIA_CHECK(rejects(
        duplicateServer,
        Http1InterimResponsePrepareError::kRepeatedSingleton));
    RUVIA_CHECK(rejects(
        invalidConnection,
        Http1InterimResponsePrepareError::kInvalidConnection));
    RUVIA_CHECK(rejects(
        managedConnection,
        Http1InterimResponsePrepareError::kInvalidConnection));
    RUVIA_CHECK(rejects(
        invalidUpgrade, Http1InterimResponsePrepareError::kInvalidUpgrade));
    RUVIA_CHECK(rejects(
        unlistedUpgrade,
        Http1InterimResponsePrepareError::kUpgradeConnectionOptionRequired));
}

RUVIA_TEST(http1_interim_response_writer_enforces_field_and_size_limits) {
    std::vector<HttpHeaderView> tooMany(
        ruvia::kMaxHttpHeaderFields + 1,
        HttpHeaderView("X-Hint", "value"));
    std::array<char, 64> buffer;
    buffer.fill('!');
    const auto tooManyResult = Http1InterimResponseWriter().prepare(
        HttpInterimResponseHead(
            ruvia::http_status::kEarlyHints,
            std::span<const HttpHeaderView>(tooMany)),
        buffer);
    RUVIA_CHECK(tooManyResult.failure() != nullptr);
    if (tooManyResult.failure() != nullptr) {
        RUVIA_CHECK_EQ(
            tooManyResult.failure()->error(),
            Http1InterimResponsePrepareError::kTooManyHeaders);
    }
    RUVIA_CHECK(unchanged(buffer, '!'));

    const std::string oversizedValue(ruvia::kMaxHttpHeaderBytes, 'x');
    const HttpHeaderView oversized[] = {{"X-Hint", oversizedValue}};
    const auto oversizedResult = Http1InterimResponseWriter().prepare(
        HttpInterimResponseHead(ruvia::http_status::kEarlyHints, oversized), buffer);
    RUVIA_CHECK(oversizedResult.failure() != nullptr);
    if (oversizedResult.failure() != nullptr) {
        RUVIA_CHECK_EQ(
            oversizedResult.failure()->error(),
            Http1InterimResponsePrepareError::kHeaderTooLarge);
    }
    RUVIA_CHECK(unchanged(buffer, '!'));
}
