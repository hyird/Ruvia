#include "test_harness.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/parser/MultipartPartAccess.h"
#include "ruvia/http/detail/parser/MultipartDelimiter.h"
#include "ruvia/http/detail/parser/MultipartPartHeaders.h"
#include "ruvia/http/detail/parser/MultipartStreamPartAccess.h"
#include "ruvia/http/MultipartParser.h"

namespace {

template <typename Input>
concept AcceptsMultipartPartName = requires(Input&& input) {
    ruvia::detail::MultipartPartAccess::make(
        std::forward<Input>(input), {}, {}, {}, std::pmr::get_default_resource());
    ruvia::detail::MultipartPartAccess::makeDecoded(
        std::forward<Input>(input), {}, {}, {}, std::pmr::get_default_resource());
};

template <typename Input>
concept AcceptsMultipartPartFilename = requires(Input&& input) {
    ruvia::detail::MultipartPartAccess::make(
        {}, std::forward<Input>(input), {}, {}, std::pmr::get_default_resource());
    ruvia::detail::MultipartPartAccess::makeDecoded(
        {}, std::forward<Input>(input), {}, {}, std::pmr::get_default_resource());
};

template <typename Input>
concept AcceptsMultipartPartContentType = requires(Input&& input) {
    ruvia::detail::MultipartPartAccess::make(
        {}, {}, std::forward<Input>(input), {}, std::pmr::get_default_resource());
};

template <typename Input>
concept AcceptsMultipartPartBody = requires(Input&& input) {
    ruvia::detail::MultipartPartAccess::make(
        {}, {}, {}, std::forward<Input>(input), std::pmr::get_default_resource());
};

template <typename Input>
concept AcceptsDecodedMultipartPartContentType = requires(Input&& input) {
    ruvia::detail::MultipartPartAccess::makeDecoded(
        {}, {}, std::forward<Input>(input), {}, std::pmr::get_default_resource());
};

template <typename Input>
concept AcceptsDecodedMultipartPartBody = requires(Input&& input) {
    ruvia::detail::MultipartPartAccess::makeDecoded(
        {}, {}, {}, std::forward<Input>(input), std::pmr::get_default_resource());
};

template <typename Input>
concept AcceptsMultipartStreamName = requires(Input&& input) {
    ruvia::detail::MultipartStreamPartAccess::make(
        std::forward<Input>(input), {}, {}, {}, ruvia::MultipartChunkPhase::kComplete);
};

template <typename Input>
concept AcceptsMultipartStreamFilename = requires(Input&& input) {
    ruvia::detail::MultipartStreamPartAccess::make(
        {}, std::forward<Input>(input), {}, {}, ruvia::MultipartChunkPhase::kComplete);
};

template <typename Input>
concept AcceptsMultipartStreamContentType = requires(Input&& input) {
    ruvia::detail::MultipartStreamPartAccess::make(
        {}, {}, std::forward<Input>(input), {}, ruvia::MultipartChunkPhase::kComplete);
};

template <typename Input>
concept AcceptsMultipartStreamBody = requires(Input&& input) {
    ruvia::detail::MultipartStreamPartAccess::make(
        {}, {}, {}, std::forward<Input>(input), ruvia::MultipartChunkPhase::kComplete);
};

static_assert(AcceptsMultipartPartName<std::string>);
static_assert(AcceptsMultipartPartName<std::pmr::string>);
static_assert(AcceptsMultipartPartFilename<std::string>);
static_assert(AcceptsMultipartPartFilename<std::pmr::string>);

#define RUVIA_ASSERT_REJECTS_TEMPORARY_MULTIPART_FIELD(Concept) \
    static_assert(!Concept<std::string>);                       \
    static_assert(!Concept<const std::string>);                 \
    static_assert(!Concept<std::pmr::string>)

RUVIA_ASSERT_REJECTS_TEMPORARY_MULTIPART_FIELD(AcceptsMultipartPartContentType);
RUVIA_ASSERT_REJECTS_TEMPORARY_MULTIPART_FIELD(AcceptsMultipartPartBody);
RUVIA_ASSERT_REJECTS_TEMPORARY_MULTIPART_FIELD(AcceptsDecodedMultipartPartContentType);
RUVIA_ASSERT_REJECTS_TEMPORARY_MULTIPART_FIELD(AcceptsDecodedMultipartPartBody);
RUVIA_ASSERT_REJECTS_TEMPORARY_MULTIPART_FIELD(AcceptsMultipartStreamName);
RUVIA_ASSERT_REJECTS_TEMPORARY_MULTIPART_FIELD(AcceptsMultipartStreamFilename);
RUVIA_ASSERT_REJECTS_TEMPORARY_MULTIPART_FIELD(AcceptsMultipartStreamContentType);
RUVIA_ASSERT_REJECTS_TEMPORARY_MULTIPART_FIELD(AcceptsMultipartStreamBody);

#undef RUVIA_ASSERT_REJECTS_TEMPORARY_MULTIPART_FIELD

#define RUVIA_ASSERT_ACCEPTS_STABLE_MULTIPART_FIELD(Concept) \
    static_assert(Concept<std::string&>);                    \
    static_assert(Concept<std::pmr::string&>);               \
    static_assert(Concept<std::string_view>)

RUVIA_ASSERT_ACCEPTS_STABLE_MULTIPART_FIELD(AcceptsMultipartPartContentType);
RUVIA_ASSERT_ACCEPTS_STABLE_MULTIPART_FIELD(AcceptsMultipartPartBody);
RUVIA_ASSERT_ACCEPTS_STABLE_MULTIPART_FIELD(AcceptsDecodedMultipartPartContentType);
RUVIA_ASSERT_ACCEPTS_STABLE_MULTIPART_FIELD(AcceptsDecodedMultipartPartBody);
RUVIA_ASSERT_ACCEPTS_STABLE_MULTIPART_FIELD(AcceptsMultipartStreamName);
RUVIA_ASSERT_ACCEPTS_STABLE_MULTIPART_FIELD(AcceptsMultipartStreamFilename);
RUVIA_ASSERT_ACCEPTS_STABLE_MULTIPART_FIELD(AcceptsMultipartStreamContentType);
RUVIA_ASSERT_ACCEPTS_STABLE_MULTIPART_FIELD(AcceptsMultipartStreamBody);

#undef RUVIA_ASSERT_ACCEPTS_STABLE_MULTIPART_FIELD

template <typename T>
concept HasMultipartStatus = requires(const T& result) { result.status(); };

template <typename T>
concept HasMultipartOffset = requires(const T& result) {
    { result.offset() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasMultipartLineBytes = requires(const T& result) {
    { result.lineBytes() } -> std::same_as<std::size_t>;
};

template <typename T>
concept HasMultipartError = requires(const T& result) { result.error(); };

template <typename T>
concept HasMultipartProtocolError = requires(const T& result) {
    { result.protocolError() } -> std::same_as<ruvia::HttpProtocolError>;
};

template <typename T>
concept ExposesRvalueMultipartInputStorage =
    requires(T&& input) { std::move(input).borrowed(); } ||
    requires(T&& input) { std::move(input).streamingOpen(); } ||
    requires(T&& input) { std::move(input).streamingEof(); } ||
    requires(T&& input) { std::move(input).view(); };

static_assert(!ExposesRvalueMultipartInputStorage<ruvia::detail::MultipartInputLifecycle>);

template <typename T>
concept HasMultipartParseError = requires(const T& result) {
    { result.parseError() } -> std::same_as<ruvia::MultipartParseError>;
};

template <typename T>
concept HasAnyRvalueMultipartPollAccessor =
    requires(T&& result) { std::move(result).needInput(); } ||
    requires(T&& result) { std::move(result).part(); } ||
    requires(T&& result) { std::move(result).done(); } ||
    requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueMultipartDelimiterAccessor =
    requires(T&& result) { std::move(result).noMatch(); } ||
    requires(T&& result) { std::move(result).needInput(); } ||
    requires(T&& result) { std::move(result).part(); } ||
    requires(T&& result) { std::move(result).close(); };

template <typename T>
concept HasAnyRvalueMultipartPartHeaderAccessor = requires(T&& result) {
    std::move(result).headers();
} || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept HasAnyRvalueMultipartBoundaryAccessor =
    requires(T&& result) { std::move(result).boundary(); } || requires(T&& result) {
        std::move(result).notApplicable();
    } || requires(T&& result) { std::move(result).failure(); };

template <typename T>
concept ExposesAnyRvalueMultipartOwnedView = requires(T&& value) { std::move(value).value(); } ||
                                             requires(T&& value) { std::move(value).name(); } ||
                                             requires(T&& value) { std::move(value).filename(); };

static_assert(std::same_as<decltype(std::declval<ruvia::MultipartParser&>().poll()),
    ruvia::MultipartPollResult>);
static_assert(!std::default_initializable<ruvia::MultipartPollResult>);
static_assert(!HasMultipartStatus<ruvia::MultipartPollResult>);
static_assert(!HasAnyRvalueMultipartPollAccessor<ruvia::MultipartPollResult>);
static_assert(std::same_as<decltype(std::declval<const ruvia::MultipartPollResult&>().part()),
    const ruvia::MultipartStreamPart*>);
static_assert(std::same_as<decltype(std::declval<const ruvia::MultipartPollResult&>().failure()),
    const ruvia::MultipartPollFailure*>);
static_assert(!HasMultipartError<ruvia::MultipartPollNeedInput>);
static_assert(!HasMultipartError<ruvia::MultipartStreamPart>);
static_assert(!HasMultipartError<ruvia::MultipartPollDone>);
static_assert(!HasMultipartError<ruvia::MultipartPollFailure>);
static_assert(HasMultipartProtocolError<ruvia::MultipartPollFailure>);
static_assert(
    std::same_as<decltype(ruvia::parseMultipartBody(std::string_view{},
                     ruvia::MultipartParseOptions{.boundary = ruvia::MultipartBoundary("x"),
                         .resource = std::pmr::get_default_resource()})),
        ruvia::MultipartBodyParseResult>);
static_assert(!std::default_initializable<ruvia::MultipartBodyParseResult>);
static_assert(std::same_as<decltype(std::declval<const ruvia::MultipartBodyParseResult&>().body()),
    const ruvia::MultipartBody*>);
static_assert(
    std::same_as<decltype(std::declval<const ruvia::MultipartBodyParseResult&>().failure()),
        const ruvia::MultipartBodyParseFailure*>);
static_assert(!HasMultipartError<ruvia::MultipartBodyParseFailure>);
static_assert(HasMultipartProtocolError<ruvia::MultipartBodyParseFailure>);

static_assert(!std::default_initializable<ruvia::detail::HttpMultipartDelimiterResult>);
static_assert(!HasMultipartStatus<ruvia::detail::HttpMultipartDelimiterResult>);
static_assert(!HasAnyRvalueMultipartDelimiterAccessor<ruvia::detail::HttpMultipartDelimiterResult>);
static_assert(!HasMultipartOffset<ruvia::detail::HttpMultipartDelimiterNoMatch>);
static_assert(HasMultipartOffset<ruvia::detail::HttpMultipartDelimiterNeedInput>);
static_assert(!HasMultipartLineBytes<ruvia::detail::HttpMultipartDelimiterNeedInput>);
static_assert(HasMultipartLineBytes<ruvia::detail::HttpMultipartPartDelimiter>);
static_assert(HasMultipartLineBytes<ruvia::detail::HttpMultipartCloseDelimiter>);

static_assert(!std::default_initializable<ruvia::MultipartBoundaryParseResult>);
static_assert(!HasMultipartStatus<ruvia::MultipartBoundaryParseResult>);
static_assert(!HasAnyRvalueMultipartBoundaryAccessor<ruvia::MultipartBoundaryParseResult>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::MultipartBoundaryParseResult&>().notApplicable()),
    const ruvia::MultipartBoundaryNotApplicable*>);
static_assert(!HasMultipartError<ruvia::MultipartBoundary>);
static_assert(!ExposesAnyRvalueMultipartOwnedView<ruvia::MultipartBoundary>);
static_assert(!ExposesAnyRvalueMultipartOwnedView<ruvia::MultipartPart>);
static_assert(!HasMultipartError<ruvia::MultipartBoundaryParseFailure>);
static_assert(HasMultipartProtocolError<ruvia::MultipartBoundaryParseFailure>);
static_assert(!std::default_initializable<ruvia::detail::HttpMultipartPartHeaderParseResult>);
static_assert(
    !HasAnyRvalueMultipartPartHeaderAccessor<ruvia::detail::HttpMultipartPartHeaderParseResult>);
static_assert(!HasMultipartError<ruvia::detail::HttpMultipartPartHeaders>);
static_assert(!HasMultipartError<ruvia::detail::HttpMultipartPartHeaderParseFailure>);
static_assert(HasMultipartParseError<ruvia::detail::HttpMultipartPartHeaderParseFailure>);

}  // namespace

RUVIA_TEST(multipart_parser_handles_deterministic_arbitrary_bytes) {
    std::uint64_t state = 0x4D55'4C54'4950'4152ULL;
    const auto next = [&state]() {
        state ^= state << 7U;
        state ^= state >> 9U;
        return state;
    };

    for (std::size_t sample = 0; sample < 2048; ++sample) {
        std::string input(static_cast<std::size_t>(next() % 1025U), '\0');
        for (auto& byte : input) {
            byte = static_cast<char>(next());
        }

        const auto boundary = ruvia::parseMultipartBoundary(input);
        const auto boundaryAlternatives =
            static_cast<unsigned int>(boundary.boundary() != nullptr) +
            static_cast<unsigned int>(boundary.notApplicable() != nullptr) +
            static_cast<unsigned int>(boundary.failure() != nullptr);
        RUVIA_CHECK_EQ(boundaryAlternatives, 1U);

        std::pmr::monotonic_buffer_resource resource;
        const auto complete = ruvia::parseMultipartBody(input,
            {.boundary = ruvia::MultipartBoundary("FUZZ"), .resource = &resource});
        RUVIA_CHECK_EQ(static_cast<unsigned int>(complete.body() != nullptr) +
                           static_cast<unsigned int>(complete.failure() != nullptr),
            1U);

        ruvia::MultipartParser parser(
            {.boundary = ruvia::MultipartBoundary("FUZZ"), .resource = &resource});
        parser.feed(input);
        parser.finishInput();

        bool terminal = false;
        for (std::size_t step = 0; step <= input.size() + 1; ++step) {
            const auto result = parser.poll();
            const auto alternatives = static_cast<unsigned int>(result.needInput() != nullptr) +
                                      static_cast<unsigned int>(result.part() != nullptr) +
                                      static_cast<unsigned int>(result.done() != nullptr) +
                                      static_cast<unsigned int>(result.failure() != nullptr);
            RUVIA_CHECK_EQ(alternatives, 1U);
            if (result.done() != nullptr || result.failure() != nullptr) {
                terminal = true;
                break;
            }
            RUVIA_CHECK(result.part() != nullptr);
        }
        RUVIA_CHECK(terminal);
    }
}

// A lone '-' after the boundary token is not the closing "--" delimiter.
RUVIA_TEST(multipart_boundary_lone_dash_is_not_a_delimiter) {
    using ruvia::detail::httpFindMultipartBodyDelimiter;
    const std::string_view body = "\r\n--abc-x\r\n--abc\r\n";
    const auto match = httpFindMultipartBodyDelimiter(
        body, ruvia::MultipartBoundary("abc"), /*inputFinished=*/true);
    const auto* part = match.part();
    RUVIA_CHECK(part != nullptr);
    if (part != nullptr) {
        RUVIA_CHECK_EQ(part->offset(), body.find("\r\n--abc\r\n"));
    }
}

RUVIA_TEST(multipart_boundary_prefix_of_longer_token_is_not_a_delimiter) {
    using ruvia::detail::httpFindInitialMultipartDelimiter;
    using ruvia::detail::httpFindMultipartBodyDelimiter;
    const std::string_view body = "\r\n--abcXYZ\r\n--abc\r\n";
    const auto bodyMatch = httpFindMultipartBodyDelimiter(
        body, ruvia::MultipartBoundary("abc"), /*inputFinished=*/true);
    const auto* bodyPart = bodyMatch.part();
    RUVIA_CHECK(bodyPart != nullptr);
    if (bodyPart != nullptr) {
        RUVIA_CHECK_EQ(bodyPart->offset(), body.find("\r\n--abc\r\n"));
    }

    // The initial delimiter must begin the entity or a new line; a matching
    // token embedded in preamble text is not a delimiter.
    const std::string_view preamble = "prefix--abc\r\ntext\r\n--abc\r\n";
    const auto initial = httpFindInitialMultipartDelimiter(
        preamble, ruvia::MultipartBoundary("abc"), /*inputFinished=*/true);
    const auto* initialPart = initial.part();
    RUVIA_CHECK(initialPart != nullptr);
    if (initialPart != nullptr) {
        RUVIA_CHECK_EQ(initialPart->offset(), preamble.rfind("--abc\r\n"));
    }
}

RUVIA_TEST(multipart_boundary_close_delimiter_still_matches) {
    using ruvia::detail::httpMatchMultipartDelimiterLine;
    const auto boundary = ruvia::MultipartBoundary("abc");
    const auto close = httpMatchMultipartDelimiterLine("--abc--\r\n", boundary, false);
    RUVIA_CHECK(close.close() != nullptr);
    const auto part = httpMatchMultipartDelimiterLine("--abc\r\nrest", boundary, false);
    RUVIA_CHECK(part.part() != nullptr);

    // RFC 2046 transport-padding is accepted on both delimiter forms.
    const auto paddedPart = httpMatchMultipartDelimiterLine("--abc \t\r\n", boundary, false);
    RUVIA_CHECK(paddedPart.part() != nullptr);
    const auto paddedClose = httpMatchMultipartDelimiterLine("--abc-- \t\r\n", boundary, false);
    RUVIA_CHECK(paddedClose.close() != nullptr);

    // A close delimiter at the current chunk edge is ambiguous until EOF.
    const auto ambiguousClose = httpMatchMultipartDelimiterLine("--abc--", boundary, false);
    RUVIA_CHECK(ambiguousClose.needInput() != nullptr);
    const auto eofClose = httpMatchMultipartDelimiterLine("--abc--", boundary, true);
    RUVIA_CHECK(eofClose.close() != nullptr);
}

RUVIA_TEST(multipart_boundary_value_enforces_rfc2046_grammar) {
    const auto throwsOn = [](std::string_view value) {
        try {
            (void)ruvia::MultipartBoundary(value);
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };

    const auto spaced = ruvia::MultipartBoundary("a b");
    RUVIA_CHECK_EQ(spaced.value(), std::string_view("a b"));
    const auto maximum = ruvia::MultipartBoundary(std::string(70, 'x'));
    RUVIA_CHECK_EQ(maximum.value().size(), std::size_t{70});
    RUVIA_CHECK(throwsOn(""));
    RUVIA_CHECK(throwsOn(std::string(71, 'x')));
    RUVIA_CHECK(throwsOn("trailing "));
    RUVIA_CHECK(throwsOn("semi;colon"));
    RUVIA_CHECK(throwsOn("bad\r\nvalue"));
}

// Boundary extraction owns MIME parameter quoting and returns the same typed
// value consumed by buffered and streaming parsers.
RUVIA_TEST(multipart_boundary_from_content_type) {
    const auto plain = ruvia::parseMultipartBoundary("multipart/form-data; boundary=abc123");
    RUVIA_CHECK(plain.boundary() != nullptr);
    RUVIA_CHECK(plain.notApplicable() == nullptr);
    RUVIA_CHECK(plain.failure() == nullptr);
    RUVIA_CHECK_EQ(plain.boundary()->value(), std::string_view("abc123"));

    const auto quoted = ruvia::parseMultipartBoundary(R"(multipart/form-data; boundary="a b")");
    RUVIA_CHECK(quoted.boundary() != nullptr);
    RUVIA_CHECK_EQ(quoted.boundary()->value(), std::string_view("a b"));

    const auto quotedSpecial =
        ruvia::parseMultipartBoundary(R"(multipart/form-data; boundary="a:b")");
    RUVIA_CHECK(quotedSpecial.boundary() != nullptr);
    RUVIA_CHECK_EQ(quotedSpecial.boundary()->value(), std::string_view("a:b"));

    const auto quotedPair =
        ruvia::parseMultipartBoundary(R"(multipart/form-data; boundary="a\?b")");
    RUVIA_CHECK(quotedPair.boundary() != nullptr);
    RUVIA_CHECK_EQ(quotedPair.boundary()->value(), std::string_view("a?b"));

    // A different media type is not applicable to the multipart parser.
    const auto wrongType = ruvia::parseMultipartBoundary("text/plain; boundary=abc");
    RUVIA_CHECK(wrongType.boundary() == nullptr);
    RUVIA_CHECK(wrongType.notApplicable() != nullptr);
    RUVIA_CHECK(wrongType.failure() == nullptr);

    // Once multipart/form-data is declared, an invalid boundary is an HTTP
    // request failure rather than a Web-layer parsing policy decision.
    for (const std::string_view invalid : {"multipart/form-data",
             "multipart/form-data; charset=utf-8", "multipart/form-data; boundary=",
             "multipart/form-data; boundary=a:b", R"(multipart/form-data; boundary="a;b")",
             "multipart/form-data; boundary=one; boundary=two",
             "multipart/form-data; boundary=abc; charset=utf-8; CHARSET=latin1",
             "multipart/form-data; boundary=abc; broken",
             "multipart/form-data; broken; boundary=abc", "multipart/form-data; boundary =abc",
             "multipart/form-data; boundary= abc",
             "multipart/form-data; boundary=abc; charset=unquoted value",
             R"(multipart/form-data; boundary=abc; charset="unterminated)",
             "multipart/form-data; boundary=abc; =value"}) {
        const auto result = ruvia::parseMultipartBoundary(invalid);
        RUVIA_CHECK(result.boundary() == nullptr);
        RUVIA_CHECK(result.notApplicable() == nullptr);
        RUVIA_CHECK(result.failure() != nullptr);
        if (result.failure() != nullptr) {
            const auto error = result.failure()->protocolError();
            RUVIA_CHECK_EQ(error.status(), ruvia::http_status::kBadRequest);
            RUVIA_CHECK_EQ(
                std::string_view(error.what()), std::string_view("invalid multipart boundary"));
        }
    }

    const auto extension =
        ruvia::parseMultipartBoundary(R"(multipart/form-data; charset="utf-8"; boundary=abc)");
    RUVIA_CHECK(extension.boundary() != nullptr);
    if (extension.boundary() != nullptr) {
        RUVIA_CHECK_EQ(extension.boundary()->value(), std::string_view("abc"));
    }
}

RUVIA_TEST(multipart_parser_commits_an_eof_close_only_after_finish_input) {
    ruvia::MultipartParser parser({.boundary = ruvia::MultipartBoundary("BOUNDARY"),
        .resource = std::pmr::get_default_resource()});
    parser.feed(
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "value\r\n"
        "--BOUNDARY--");

    const auto first = parser.poll();
    const auto* firstPart = first.part();
    RUVIA_CHECK(firstPart != nullptr);
    if (firstPart != nullptr) {
        RUVIA_CHECK(firstPart->phase() == ruvia::MultipartChunkPhase::kFirst);
        RUVIA_CHECK_EQ(firstPart->body(), std::string_view("value"));
    }

    const auto waiting = parser.poll();
    RUVIA_CHECK(waiting.needInput() != nullptr);
    RUVIA_CHECK(waiting.part() == nullptr);
    RUVIA_CHECK(waiting.done() == nullptr);

    parser.finishInput();
    const auto last = parser.poll();
    const auto* lastPart = last.part();
    RUVIA_CHECK(lastPart != nullptr);
    if (lastPart != nullptr) {
        RUVIA_CHECK(lastPart->phase() == ruvia::MultipartChunkPhase::kLast);
        RUVIA_CHECK(lastPart->body().empty());
    }
    const auto done = parser.poll();
    RUVIA_CHECK(done.done() != nullptr);
    RUVIA_CHECK(done.needInput() == nullptr);
    RUVIA_CHECK(done.part() == nullptr);

    bool feedAfterFinishThrew = false;
    try {
        parser.feed("ignored");
    } catch (const std::logic_error&) {
        feedAfterFinishThrew = true;
    }
    RUVIA_CHECK(feedAfterFinishThrew);
}

RUVIA_TEST(multipart_input_lifecycle_has_three_exclusive_states) {
    ruvia::detail::MultipartInputLifecycle streaming(std::pmr::get_default_resource());
    RUVIA_CHECK(streaming.streamingOpen() != nullptr);
    RUVIA_CHECK(streaming.streamingEof() == nullptr);
    RUVIA_CHECK(streaming.borrowed() == nullptr);
    RUVIA_CHECK(!streaming.eof());

    streaming.feed("abcdef");
    streaming.consume(2);
    RUVIA_CHECK_EQ(streaming.view(), std::string_view("cdef"));
    streaming.finishInput();
    RUVIA_CHECK(streaming.streamingOpen() == nullptr);
    RUVIA_CHECK(streaming.streamingEof() != nullptr);
    RUVIA_CHECK(streaming.eof());
    RUVIA_CHECK_EQ(streaming.view(), std::string_view("cdef"));

    // EOF is an idempotent transition and cannot discard pending bytes.
    streaming.finishInput();
    RUVIA_CHECK(streaming.streamingEof() != nullptr);
    RUVIA_CHECK_EQ(streaming.view(), std::string_view("cdef"));
}

RUVIA_TEST(multipart_borrowed_input_is_complete_and_rejects_feed) {
    ruvia::detail::MultipartInputLifecycle borrowed(
        ruvia::detail::MultipartBorrowedInput{"--BOUNDARY--"});
    RUVIA_CHECK(borrowed.borrowed() != nullptr);
    RUVIA_CHECK(borrowed.eof());
    RUVIA_CHECK_EQ(borrowed.view(), std::string_view("--BOUNDARY--"));

    borrowed.finishInput();
    RUVIA_CHECK(borrowed.borrowed() != nullptr);
    bool feedThrew = false;
    try {
        borrowed.feed("ignored");
    } catch (const std::logic_error&) {
        feedThrew = true;
    }
    RUVIA_CHECK(feedThrew);
}

RUVIA_TEST(multipart_parser_reports_typed_incomplete_body) {
    ruvia::MultipartParser parser({.boundary = ruvia::MultipartBoundary("BOUNDARY"),
        .resource = std::pmr::get_default_resource()});
    parser.feed(
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "truncated");
    parser.finishInput();

    const auto result = parser.poll();
    RUVIA_CHECK(result.failure() != nullptr);
    RUVIA_CHECK(result.needInput() == nullptr);
    RUVIA_CHECK(result.part() == nullptr);
    RUVIA_CHECK(result.done() == nullptr);
    if (result.failure() != nullptr) {
        const auto error = result.failure()->protocolError();
        RUVIA_CHECK_EQ(error.status(), ruvia::http_status::kBadRequest);
        RUVIA_CHECK_EQ(
            std::string_view(error.what()), std::string_view("incomplete multipart body"));
    }
}

// Part header parsing owns either the parsed views or a typed failure.
RUVIA_TEST(multipart_part_header_result_is_discriminated) {
    using ruvia::detail::httpParseMultipartPartHeaders;

    const auto parsed = httpParseMultipartPartHeaders(
        "Content-Disposition: form-data; name=\"field\"; filename=\"f.txt\"\r\n"
        "Content-Type: text/plain");
    const auto* headers = parsed.headers();
    RUVIA_CHECK(headers != nullptr);
    RUVIA_CHECK(parsed.failure() == nullptr);
    if (headers != nullptr) {
        RUVIA_CHECK_EQ(headers->name(), std::string_view("field"));
        RUVIA_CHECK_EQ(headers->filename(), std::string_view("f.txt"));
        RUVIA_CHECK(headers->hasFilename());
        RUVIA_CHECK_EQ(headers->contentType(), std::string_view("text/plain"));
    }

    // form-data with no name parameter.
    const auto missingName = httpParseMultipartPartHeaders("Content-Disposition: form-data");
    RUVIA_CHECK(missingName.failure() != nullptr);
    if (missingName.failure() != nullptr) {
        RUVIA_CHECK(
            missingName.failure()->parseError() == ruvia::MultipartParseError::kMissingFieldName);
    }

    // A non-form-data disposition, and no disposition at all, are invalid.
    for (const std::string_view invalid :
        {"Content-Disposition: attachment; name=\"x\"", "Content-Type: text/plain"}) {
        const auto result = httpParseMultipartPartHeaders(invalid);
        RUVIA_CHECK(result.failure() != nullptr);
        if (result.failure() != nullptr) {
            RUVIA_CHECK(result.failure()->parseError() ==
                        ruvia::MultipartParseError::kInvalidContentDisposition);
        }
    }
}

RUVIA_TEST(multipart_part_preserves_empty_filename_parameter_presence) {
    using ruvia::detail::httpParseMultipartPartHeaders;

    const auto headersOnly = httpParseMultipartPartHeaders(
        "Content-Disposition: form-data; name=\"upload\"; filename=\"\"");
    const auto* headers = headersOnly.headers();
    RUVIA_CHECK(headers != nullptr);
    if (headers != nullptr) {
        RUVIA_CHECK(headers->hasFilename());
        RUVIA_CHECK_EQ(headers->filename(), std::string_view());
    }

    const auto noFilename =
        httpParseMultipartPartHeaders("Content-Disposition: form-data; name=\"upload\"");
    const auto* noFilenameHeaders = noFilename.headers();
    RUVIA_CHECK(noFilenameHeaders != nullptr);
    if (noFilenameHeaders != nullptr) {
        RUVIA_CHECK(!noFilenameHeaders->hasFilename());
        RUVIA_CHECK_EQ(noFilenameHeaders->filename(), std::string_view());
    }

    const std::string body =
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"upload\"; filename=\"\"\r\n"
        "\r\n"
        "data\r\n"
        "--BOUNDARY--\r\n";
    const auto complete =
        ruvia::parseMultipartBody(body, {.boundary = ruvia::MultipartBoundary("BOUNDARY"),
                                            .resource = std::pmr::get_default_resource()});
    RUVIA_CHECK(complete.failure() == nullptr);
    if (complete.body() != nullptr) {
        const auto& parts = complete.body()->parts();
        RUVIA_CHECK_EQ(parts.size(), std::size_t{1});
        if (!parts.empty()) {
            RUVIA_CHECK(parts[0].hasFilename());
            RUVIA_CHECK_EQ(parts[0].filename(), std::string_view());
        }
    }

    ruvia::MultipartParser parser({.boundary = ruvia::MultipartBoundary("BOUNDARY"),
        .resource = std::pmr::get_default_resource()});
    parser.feed(body);
    parser.finishInput();
    const auto streamed = parser.poll();
    const auto* part = streamed.part();
    RUVIA_CHECK(part != nullptr);
    if (part != nullptr) {
        RUVIA_CHECK(part->hasFilename());
        RUVIA_CHECK_EQ(part->filename(), std::string_view());
    }
}

RUVIA_TEST(multipart_part_header_rejects_ambiguous_disposition_parameters) {
    using ruvia::detail::httpParseMultipartPartHeaders;

    for (const std::string_view invalid : {"Content-Disposition: form-data; name=\"unterminated",
             "Content-Disposition: form-data; name=unquoted value",
             "Content-Disposition: form-data; name=field; name=shadow",
             "Content-Disposition: form-data; name=field; filename=a; filename=b",
             "Content-Disposition: form-data; name=field; FileName*=UTF-8''evil.txt",
             "Content-Disposition: form-data; name=field; x=one; X=two",
             "Content-Disposition: form-data; name=field; broken",
             "Content-Disposition: form-data; name=field\r\n"
             "Content-Disposition: form-data; name=shadow"}) {
        const auto parsed = httpParseMultipartPartHeaders(invalid);
        RUVIA_CHECK(parsed.failure() != nullptr);
        if (parsed.failure() != nullptr) {
            RUVIA_CHECK(parsed.failure()->parseError() ==
                        ruvia::MultipartParseError::kInvalidContentDisposition);
        }

        std::string body = "--BOUNDARY\r\n";
        body.append(invalid);
        body.append("\r\n\r\nvalue\r\n--BOUNDARY--\r\n");
        const auto complete =
            ruvia::parseMultipartBody(body, {.boundary = ruvia::MultipartBoundary("BOUNDARY"),
                                                .resource = std::pmr::get_default_resource()});
        RUVIA_CHECK(complete.failure() != nullptr);
        if (complete.failure() != nullptr) {
            RUVIA_CHECK_EQ(
                complete.failure()->protocolError().status(), ruvia::http_status::kBadRequest);
            RUVIA_CHECK_EQ(std::string_view(complete.failure()->protocolError().what()),
                std::string_view("invalid multipart content disposition"));
        }
    }

    const auto escaped = httpParseMultipartPartHeaders(
        "Content-Disposition: form-data; name=\"a\\\"b\"; filename=\"x\\\\y\"");
    RUVIA_CHECK(escaped.headers() != nullptr);
    if (escaped.headers() != nullptr) {
        RUVIA_CHECK_EQ(escaped.headers()->name(), std::string_view("a\\\"b"));
        RUVIA_CHECK_EQ(escaped.headers()->filename(), std::string_view("x\\\\y"));
    }

    // MIME structured fields allow linear whitespace around separator
    // characters; this differs from top-level HTTP media-type parameters.
    const auto spaced = httpParseMultipartPartHeaders(
        "Content-Disposition: form-data; name = field; filename = \"a.txt\"");
    RUVIA_CHECK(spaced.headers() != nullptr);
    if (spaced.headers() != nullptr) {
        RUVIA_CHECK_EQ(spaced.headers()->name(), std::string_view("field"));
        RUVIA_CHECK_EQ(spaced.headers()->filename(), std::string_view("a.txt"));
    }
}

RUVIA_TEST(multipart_part_header_rejects_ambiguous_header_blocks) {
    using ruvia::detail::httpParseMultipartPartHeaders;

    for (const std::string_view invalid : {"Broken-Line\r\n"
                                           "Content-Disposition: form-data; name=field",
             " Content-Disposition: form-data; name=field",
             "Content-Disposition : form-data; name=field",
             "Content-Disposition: form-data; name=field\r\n"
             " filename=shadow.txt",
             "Content-Disposition: form-data; name=field\r\n"
             "Content-Type: text/plain\r\n"
             "Content-Type: application/json"}) {
        const auto parsed = httpParseMultipartPartHeaders(invalid);
        RUVIA_CHECK(parsed.failure() != nullptr);
        if (parsed.failure() != nullptr) {
            RUVIA_CHECK(
                parsed.failure()->parseError() == ruvia::MultipartParseError::kInvalidPartHeaders);
        }

        std::string body = "--BOUNDARY\r\n";
        body.append(invalid);
        body.append("\r\n\r\nvalue\r\n--BOUNDARY--\r\n");
        const auto complete =
            ruvia::parseMultipartBody(body, {.boundary = ruvia::MultipartBoundary("BOUNDARY"),
                                                .resource = std::pmr::get_default_resource()});
        RUVIA_CHECK(complete.failure() != nullptr);
        if (complete.failure() != nullptr) {
            RUVIA_CHECK_EQ(
                complete.failure()->protocolError().status(), ruvia::http_status::kBadRequest);
            RUVIA_CHECK_EQ(std::string_view(complete.failure()->protocolError().what()),
                std::string_view("invalid multipart part headers"));
        }
    }
}

RUVIA_TEST(multipart_part_header_rejects_invalid_content_types) {
    using ruvia::detail::httpParseMultipartPartHeaders;

    for (const std::string_view contentType :
        {"", "text", "text/", "/plain", "*/plain", "text/*", "text/plain; charset",
            "text/plain; charset=", "text/plain; charset=utf-8; CHARSET=latin1"}) {
        std::string headers =
            "Content-Disposition: form-data; name=field\r\n"
            "Content-Type: ";
        headers.append(contentType);
        const auto parsed = httpParseMultipartPartHeaders(headers);
        RUVIA_CHECK(parsed.failure() != nullptr);
        if (parsed.failure() != nullptr) {
            RUVIA_CHECK(
                parsed.failure()->parseError() == ruvia::MultipartParseError::kInvalidPartHeaders);
        }
    }

    const auto valid = httpParseMultipartPartHeaders(
        "Content-Disposition: form-data; name=field\r\n"
        "Content-Type: text/plain; charset = \"UTF-8\"");
    RUVIA_CHECK(valid.headers() != nullptr);
    if (valid.headers() != nullptr) {
        RUVIA_CHECK_EQ(
            valid.headers()->contentType(), std::string_view("text/plain; charset = \"UTF-8\""));
    }
}

RUVIA_TEST(multipart_part_header_names_are_case_insensitive) {
    using ruvia::detail::httpParseMultipartPartHeaders;

    // HTTP field names are case-insensitive; a part that lowercases them (some
    // clients do) must still be recognized, with name and content type extracted.
    const auto parsed = httpParseMultipartPartHeaders(
        "content-disposition: form-data; name=\"field\"\r\n"
        "content-type: image/png");
    const auto* headers = parsed.headers();
    RUVIA_CHECK(headers != nullptr);
    if (headers != nullptr) {
        RUVIA_CHECK_EQ(headers->name(), std::string_view("field"));
        RUVIA_CHECK_EQ(headers->contentType(), std::string_view("image/png"));
    }
}

RUVIA_TEST(multipart_header_value_in_block_lookup) {
    using ruvia::detail::httpHeaderValueInBlock;
    const std::string_view block =
        "Content-Disposition: form-data; name=\"a\"\r\n"
        "Content-Type: text/plain";
    // Case-insensitive name match with OWS-trimmed value; the last line has no
    // trailing CRLF and must still be found.
    RUVIA_CHECK(httpHeaderValueInBlock(block, "content-type") == std::string_view("text/plain"));
    RUVIA_CHECK(httpHeaderValueInBlock(block, "CONTENT-TYPE") == std::string_view("text/plain"));
    RUVIA_CHECK(httpHeaderValueInBlock(block, "Content-Disposition") ==
                std::string_view("form-data; name=\"a\""));
    // Missing header -> nullopt.
    RUVIA_CHECK(!httpHeaderValueInBlock(block, "X-Absent").has_value());
    // A line without a colon is skipped, not matched by name.
    RUVIA_CHECK(!httpHeaderValueInBlock("garbageline\r\nX: v", "garbageline").has_value());
    // Surrounding OWS on the value is trimmed.
    RUVIA_CHECK(httpHeaderValueInBlock("X:   spaced   ", "X") == std::string_view("spaced"));
}

RUVIA_TEST(multipart_header_value_in_block_uses_last_match) {
    using ruvia::detail::httpHeaderValueInBlock;
    const std::string_view block =
        "Content-Type: text/plain\r\n"
        "X-Other: value\r\n"
        "content-type: image/png";

    RUVIA_CHECK(httpHeaderValueInBlock(block, "Content-Type") == std::string_view("image/png"));
}

RUVIA_TEST(multipart_disposition_parameter_extraction) {
    using ruvia::detail::httpDispositionParameter;
    const std::string_view disposition = "form-data; name=\"field\"; filename=\"a.txt\"";
    RUVIA_CHECK(httpDispositionParameter(disposition, "name") == std::string_view("field"));
    RUVIA_CHECK(httpDispositionParameter(disposition, "filename") == std::string_view("a.txt"));
    // An unquoted parameter value is returned as-is.
    RUVIA_CHECK(
        httpDispositionParameter("form-data; name=plain", "name") == std::string_view("plain"));
    // An absent parameter is nullopt.
    RUVIA_CHECK(!httpDispositionParameter(disposition, "charset").has_value());
    // Parameter names are case-insensitive (RFC 6266 §4.1), like the Content-Type
    // boundary parameter -- `Name`/`FileName` must resolve, not be rejected.
    const std::string_view mixedCase = "form-data; Name=\"field\"; FileName=\"a.txt\"";
    RUVIA_CHECK(httpDispositionParameter(mixedCase, "name") == std::string_view("field"));
    RUVIA_CHECK(httpDispositionParameter(mixedCase, "filename") == std::string_view("a.txt"));
}

RUVIA_TEST(multipart_is_form_data_disposition) {
    using ruvia::detail::httpIsFormDataDisposition;
    RUVIA_CHECK(httpIsFormDataDisposition("form-data; name=\"x\""));
    RUVIA_CHECK(httpIsFormDataDisposition("FORM-DATA"));                      // case-insensitive
    RUVIA_CHECK(httpIsFormDataDisposition("  form-data  ; filename=\"y\""));  // OWS-trimmed type
    RUVIA_CHECK(!httpIsFormDataDisposition("attachment; name=\"x\""));
    RUVIA_CHECK(!httpIsFormDataDisposition("form-data-extra"));  // whole type compared
    RUVIA_CHECK(!httpIsFormDataDisposition(""));
}

RUVIA_TEST(multipart_part_access_decodes_quoted_pairs) {
    // The buffered parser builds parts via MultipartPartAccess::make, which must
    // decode RFC 7230 §3.2.6 quoted-pairs in name/filename (they are part-owned so
    // they may differ from the raw request bytes); contentType/body stay verbatim.
    auto* resource = std::pmr::get_default_resource();
    const auto part = ruvia::detail::MultipartPartAccess::make(
        "a\\\"b", "x\\\\y.txt", "text/plain", "the body", resource);
    RUVIA_CHECK_EQ(std::string(part.name()), std::string("a\"b"));
    RUVIA_CHECK_EQ(std::string(part.filename()), std::string("x\\y.txt"));
    RUVIA_CHECK_EQ(std::string(part.contentType()), std::string("text/plain"));
    RUVIA_CHECK_EQ(std::string(part.body()), std::string("the body"));
}

RUVIA_TEST(multipart_complete_body_parser_returns_borrowed_part_bodies) {
    const std::string body =
        "preamble\r\n"
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name = \"field\"\r\n\r\n"
        "value\r\n"
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"upload\"; filename=\"a.txt\"\r\n"
        "Content-Type: text/plain\r\n\r\n"
        "file-data\r\n"
        "--BOUNDARY--\r\n";
    const auto parsed =
        ruvia::parseMultipartBody(body, {.boundary = ruvia::MultipartBoundary("BOUNDARY"),
                                            .resource = std::pmr::get_default_resource()});
    RUVIA_CHECK(parsed.failure() == nullptr);
    const auto& parts = parsed.body()->parts();
    RUVIA_CHECK_EQ(parts.size(), std::size_t{2});
    RUVIA_CHECK_EQ(parts[0].name(), std::string_view("field"));
    RUVIA_CHECK_EQ(parts[0].body(), std::string_view("value"));
    RUVIA_CHECK_EQ(parts[1].name(), std::string_view("upload"));
    RUVIA_CHECK_EQ(parts[1].filename(), std::string_view("a.txt"));
    RUVIA_CHECK_EQ(parts[1].contentType(), std::string_view("text/plain"));
    RUVIA_CHECK_EQ(parts[1].body(), std::string_view("file-data"));
    RUVIA_CHECK(parts[0].body().data() >= body.data());
    RUVIA_CHECK(parts[0].body().data() < body.data() + body.size());
}

RUVIA_TEST(multipart_complete_body_parser_rejects_malformed_body) {
    const auto parsed = ruvia::parseMultipartBody(
        "--BOUNDARY\r\nContent-Disposition: form-data; name=\"x\"\r\n\r\nmissing close",
        {.boundary = ruvia::MultipartBoundary("BOUNDARY"),
            .resource = std::pmr::get_default_resource()});
    RUVIA_CHECK(parsed.body() == nullptr);
    RUVIA_CHECK(parsed.failure() != nullptr);
    RUVIA_CHECK_EQ(parsed.failure()->protocolError().status(), ruvia::http_status::kBadRequest);
    RUVIA_CHECK_EQ(std::string_view(parsed.failure()->protocolError().what()),
        std::string_view("incomplete multipart body"));
}

RUVIA_TEST(multipart_complete_body_parser_shares_incremental_limits) {
    std::string oversizedPreamble(64 * 1024 + 1, 'x');
    const auto complete = ruvia::parseMultipartBody(
        oversizedPreamble, {.boundary = ruvia::MultipartBoundary("BOUNDARY"),
                               .resource = std::pmr::get_default_resource()});
    RUVIA_CHECK(complete.failure() != nullptr);
    RUVIA_CHECK_EQ(
        complete.failure()->protocolError().status(), ruvia::http_status::kContentTooLarge);
    RUVIA_CHECK_EQ(std::string_view(complete.failure()->protocolError().what()),
        std::string_view("multipart preamble exceeds limit"));

    ruvia::MultipartParser incremental({.boundary = ruvia::MultipartBoundary("BOUNDARY"),
        .resource = std::pmr::get_default_resource()});
    incremental.feed(oversizedPreamble);
    const auto streamed = incremental.poll();
    RUVIA_CHECK(streamed.failure() != nullptr);
    RUVIA_CHECK_EQ(
        streamed.failure()->protocolError().status(), ruvia::http_status::kContentTooLarge);
    RUVIA_CHECK_EQ(std::string_view(streamed.failure()->protocolError().what()),
        std::string_view(complete.failure()->protocolError().what()));
    const auto repeated = incremental.poll();
    RUVIA_CHECK(repeated.failure() != nullptr);
    RUVIA_CHECK_EQ(std::string_view(repeated.failure()->protocolError().what()),
        std::string_view(complete.failure()->protocolError().what()));
    bool feedAfterFailureThrew = false;
    try {
        incremental.feed("--BOUNDARY--");
    } catch (const std::logic_error&) {
        feedAfterFailureThrew = true;
    }
    RUVIA_CHECK(feedAfterFailureThrew);
}

RUVIA_TEST(multipart_complete_limits_cannot_be_bypassed_by_terminators) {
    auto* const resource = std::pmr::get_default_resource();
    const auto parse = [resource](std::string_view body) {
        return ruvia::parseMultipartBody(
            body, {.boundary = ruvia::MultipartBoundary("BOUNDARY"), .resource = resource});
    };
    const auto checkFailure = [&ruvia_ctx, &parse, resource](
                                  const std::string& body, std::string_view message) {
        const auto result = parse(body);
        RUVIA_CHECK(result.failure() != nullptr);
        if (const auto* failure = result.failure()) {
            RUVIA_CHECK_EQ(failure->protocolError().status(), ruvia::http_status::kContentTooLarge);
            RUVIA_CHECK_EQ(std::string_view(failure->protocolError().what()), message);
        }

        ruvia::MultipartParser incremental(
            {.boundary = ruvia::MultipartBoundary("BOUNDARY"), .resource = resource});
        incremental.feed(body);
        incremental.finishInput();
        const auto streamed = incremental.poll();
        RUVIA_CHECK(streamed.failure() != nullptr);
        if (const auto* failure = streamed.failure()) {
            RUVIA_CHECK_EQ(failure->protocolError().status(), ruvia::http_status::kContentTooLarge);
            RUVIA_CHECK_EQ(std::string_view(failure->protocolError().what()), message);
        }
    };

    // A complete boundary used to skip the preamble cap because the limit was
    // checked only while the parser was still searching for that boundary.
    std::string preamble(64 * 1024 + 1, 'p');
    preamble.append("\r\n--BOUNDARY--\r\n");
    checkFailure(preamble, "multipart preamble exceeds limit");

    // Likewise, finding CRLF CRLF in the same input bypassed the part-header
    // check, even when the complete block was already larger than 64 KiB.
    std::string headers =
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "X-Large: ";
    headers.append(64 * 1024, 'h');
    headers.append(
        "\r\n\r\nvalue\r\n"
        "--BOUNDARY--\r\n");
    checkFailure(headers, "multipart part headers exceed limit");

    // A completed delimiter line with excessive transport-padding must be
    // bounded too; the previous check ran only while the line was incomplete.
    std::string delimiter = "--BOUNDARY";
    delimiter.append(64 * 1024, ' ');
    delimiter.append("\r\n");
    checkFailure(delimiter, "multipart delimiter line exceeds limit");

    // Exercise the same completed-line check after a part body, where the
    // delimiter is discovered by the streaming body scanner rather than the
    // initial-boundary path.
    std::string bodyDelimiter =
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"field\"\r\n"
        "\r\n"
        "value\r\n"
        "--BOUNDARY--";
    bodyDelimiter.append(64 * 1024, ' ');
    bodyDelimiter.append("\r\n");
    checkFailure(bodyDelimiter, "multipart delimiter line exceeds limit");
}
