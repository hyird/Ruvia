#include <algorithm>
#include <array>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3VarInt.h"

#include "http3/Http3ClientSansIoSessionEngine.h"
#include "test_harness.h"

namespace {
class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t allocations{};
    std::size_t deallocations{};
    std::size_t bodyAllocations{};
    std::size_t liveBytes{};
    bool reject{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (reject) {
            throw std::bad_alloc();
        }
        void* const data = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++allocations;
        if (bytes >= 1024) {
            ++bodyAllocations;
        }
        liveBytes += bytes;
        return data;
    }
    void do_deallocate(void* data, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        liveBytes -= bytes;
        std::pmr::new_delete_resource()->deallocate(data, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

std::vector<char> frame(std::uint64_t type, std::span<const char> payload) {
    std::vector<char> result(16);
    const auto typeBytes = ruvia::encodeHttp3VarInt(result, type);
    const auto lengthBytes = ruvia::encodeHttp3VarInt(std::span<char>(result).subspan(*typeBytes), payload.size());
    result.resize(*typeBytes + *lengthBytes);
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}
std::vector<char> responseHead(std::string_view status = "200",
    std::optional<std::string_view> contentLength = {}) {
    std::pmr::monotonic_buffer_resource resource;
    std::vector<ruvia::Http3FieldSectionFieldView> fields{{":status", status}, {"x-session", "owned"}};
    if (contentLength) {
        fields.push_back({"content-length", *contentLength});
    }
    const auto encoded = ruvia::encodeHttp3FieldSection(fields, &resource);
    return frame(1, *encoded);
}
std::vector<char> body(std::string_view text) {
    return frame(0, std::span<const char>(text.data(), text.size()));
}
std::vector<char> responseTrailers() {
    std::pmr::monotonic_buffer_resource resource;
    constexpr std::array<ruvia::Http3FieldSectionFieldView, 2> fields{{{"x-one", "first"},
        {"x-two", "second"}}};
    const auto encoded = ruvia::encodeHttp3FieldSection(fields, &resource);
    return frame(1, *encoded);
}

bool planMatches(const std::optional<ruvia::HttpResponseBodyPlan>& plan,
    ruvia::HttpKnownMethod method, std::uint16_t status,
    ruvia::HttpResponseContentSemantics semantics, bool bodySuppressed) {
    return plan && plan->requestMethod() == method &&
           plan->responseStatus() == ruvia::HttpStatusCode::fromValue(status) &&
           plan->contentSemantics() == semantics &&
           plan->bodySuppressed() == bodySuppressed;
}

struct StreamedEvents final {
    explicit StreamedEvents(std::pmr::memory_resource* resource)
        : body(resource),
          trailerNames(resource) {}
    std::pmr::string body;
    std::pmr::string trailerNames;
    std::uint16_t status{};
    std::size_t heads{};
    std::size_t chunks{};
    std::size_t trailers{};
    std::size_t ends{};
    ruvia::detail::Http3ClientSansIoSessionEngine* engine{};
    bool checkedReentry{};
    bool failBody{};
};

void collectStreamedResponse(void* raw, const ruvia::Http3ConnectionEvent& event) {
    auto& observed = *static_cast<StreamedEvents*>(raw);
    switch (event.kind) {
        case ruvia::Http3ConnectionEventKind::kFinalHead:
            ++observed.heads;
            observed.status = event.head->status;
            return;
        case ruvia::Http3ConnectionEventKind::kBody:
            if (observed.failBody) {
                throw std::runtime_error("streaming response sink rejected body");
            }
            if (observed.engine && !observed.checkedReentry) {
                observed.checkedReentry = true;
                if (observed.engine->registerRequest(8, ruvia::HttpKnownMethod::kGet).status !=
                        ruvia::detail::Http3ClientSansIoSessionStatus::kInvalidState ||
                    observed.engine->release(event.streamId) ||
                    observed.engine->cancelRequest(event.streamId) ||
                    observed.engine->feed(event.streamId, {}).status !=
                        ruvia::detail::Http3ClientSansIoSessionStatus::kInvalidState ||
                    observed.engine->stop().status !=
                        ruvia::detail::Http3ClientSansIoSessionStatus::kInvalidState) {
                    throw std::logic_error("streaming sink illegally modified active engine");
                }
            }
            observed.body.append(event.body.data(), event.body.size());
            ++observed.chunks;
            return;
        case ruvia::Http3ConnectionEventKind::kTrailerField:
            observed.trailerNames.append(event.trailer.name);
            ++observed.trailers;
            return;
        case ruvia::Http3ConnectionEventKind::kMessageEnd:
            ++observed.ends;  // A sink must never see terminal delivery.
            return;
        default:
            return;
    }
}
}  // namespace

RUVIA_TEST(http3ClientSansIoSessionDeeplyRetainsInterleavedResponsesUntilRelease) {
    std::pmr::monotonic_buffer_resource worker;
    ruvia::detail::Http3ClientSansIoSessionEngine session(&worker);
    RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet).status ==
                ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
    RUVIA_CHECK(session.registerRequest(4, ruvia::HttpKnownMethod::kGet).status ==
                ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
    const auto head = responseHead();
    const auto payload = body("one");
    RUVIA_CHECK(session.feed(0, head).status == ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
    RUVIA_CHECK(!session.response(0).has_value());
    RUVIA_CHECK(session.feed(4, head).status == ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
    RUVIA_CHECK(session.feed(0, payload, true).status == ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
    RUVIA_CHECK(session.feed(4, body("two"), true).status == ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
    const auto first = session.response(0);
    const auto second = session.response(4);
    RUVIA_CHECK(first && second);
    RUVIA_CHECK(first->status == 200 && first->complete);
    RUVIA_CHECK(first->headers.size() == 1);
    RUVIA_CHECK(first->headers[0].name == "x-session");
    RUVIA_CHECK(first->headers[0].value == "owned");
    RUVIA_CHECK(std::string_view(first->body.data(), first->body.size()) == "one");
    RUVIA_CHECK(std::string_view(second->body.data(), second->body.size()) == "two");
    RUVIA_CHECK(session.retainedBodyBytes() == 6);
    RUVIA_CHECK(session.release(0));
    RUVIA_CHECK(session.retainedBodyBytes() == 3);
    RUVIA_CHECK(session.response(4).has_value());
}

RUVIA_TEST(http3ClientSansIoSessionRetainsTheProtocolFinalResponseBodyPlan) {
    CountingResource memory;
    std::optional<ruvia::HttpResponseBodyPlan> retainedPlan;
    {
        ruvia::detail::Http3ClientSansIoSessionEngine session(&memory);
        RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(0, responseHead("103")).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
        RUVIA_CHECK(!session.response(0));
        RUVIA_CHECK(session.feed(0, responseHead("200", "3")).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
        RUVIA_CHECK(session.feed(0, body("abc"), true).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        const auto getWithBody = session.response(0);
        RUVIA_CHECK(getWithBody && planMatches(getWithBody->responseBodyPlan,
                                       ruvia::HttpKnownMethod::kGet, 200,
                                       ruvia::HttpResponseContentSemantics::kWithContent, false));
        retainedPlan = getWithBody->responseBodyPlan;
        RUVIA_CHECK(session.release(0));

        RUVIA_CHECK(session.registerRequest(4, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(4, responseHead()).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
        RUVIA_CHECK(session.feed(4, {}, true).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        const auto getWithoutBody = session.response(4);
        RUVIA_CHECK(getWithoutBody && planMatches(getWithoutBody->responseBodyPlan,
                                          ruvia::HttpKnownMethod::kGet, 200,
                                          ruvia::HttpResponseContentSemantics::kWithContent, false));
        RUVIA_CHECK(session.release(4));

        RUVIA_CHECK(session.registerRequest(8, ruvia::HttpKnownMethod::kHead).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(8, responseHead("200", "5")).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
        RUVIA_CHECK(session.feed(8, {}, true).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        const auto head = session.response(8);
        RUVIA_CHECK(head && planMatches(head->responseBodyPlan, ruvia::HttpKnownMethod::kHead, 200,
                                ruvia::HttpResponseContentSemantics::kWithoutContent, true));
        RUVIA_CHECK(session.release(8));

        RUVIA_CHECK(session.registerRequest(12, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(12, responseHead("204")).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
        RUVIA_CHECK(session.feed(12, {}, true).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        const auto noContent = session.response(12);
        RUVIA_CHECK(noContent && planMatches(noContent->responseBodyPlan, ruvia::HttpKnownMethod::kGet, 204,
                                     ruvia::HttpResponseContentSemantics::kWithoutContent, true));
        RUVIA_CHECK(session.release(12));

        RUVIA_CHECK(session.registerRequest(16, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(16, responseHead("304", "7")).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
        RUVIA_CHECK(session.feed(16, {}, true).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        const auto notModified = session.response(16);
        RUVIA_CHECK(notModified && planMatches(notModified->responseBodyPlan, ruvia::HttpKnownMethod::kGet, 304,
                                       ruvia::HttpResponseContentSemantics::kWithoutContent, true));
        RUVIA_CHECK(session.release(16));

        RUVIA_CHECK(session.registerRequest(20, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(20, responseHead()).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
        RUVIA_CHECK(session.feed(20, {}, false, true).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kReset);
        const auto reset = session.response(20);
        RUVIA_CHECK(reset && reset->reset && planMatches(reset->responseBodyPlan, ruvia::HttpKnownMethod::kGet, 200, ruvia::HttpResponseContentSemantics::kWithContent, false));
        RUVIA_CHECK(session.release(20));
    }
    RUVIA_CHECK(planMatches(retainedPlan, ruvia::HttpKnownMethod::kGet, 200,
        ruvia::HttpResponseContentSemantics::kWithContent, false));
    RUVIA_CHECK(memory.allocations == memory.deallocations && memory.liveBytes == 0);
}

RUVIA_TEST(http3ClientSansIoSessionStreamsOwnedEventsWithoutRetainingBody) {
    CountingResource memory;
    {
        ruvia::detail::Http3ClientSansIoSessionEngine::Limits limits{};
        limits.maxBodyBytesPerStream = 2;
        limits.maxTotalBodyBytes = 2;
        ruvia::detail::Http3ClientSansIoSessionEngine session(&memory, limits);
        StreamedEvents observed(&memory);
        observed.engine = &session;
        RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet,
                               {.callback = collectStreamedResponse, .context = &observed})
                        .scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.registerRequest(4, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        auto head = responseHead();
        RUVIA_CHECK(session.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
        const auto buffered = body("ab");
        RUVIA_CHECK(session.feed(4, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(4, buffered, true).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        RUVIA_CHECK(session.retainedBodyBytes() == 2);
        auto first = body("abc");
        RUVIA_CHECK(session.feed(0, first).scope == ruvia::Http3ConnectionErrorScope::kNone);
        std::fill(first.begin(), first.end(), '?');
        RUVIA_CHECK(session.feed(0, body("def")).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.retainedBodyBytes() == 2);
        RUVIA_CHECK(session.feed(0, responseTrailers()).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(0, {}, true).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        const auto response = session.response(0);
        RUVIA_CHECK(response && response->complete && response->status == 200 && response->body.empty());
        RUVIA_CHECK(observed.heads == 1 && observed.chunks == 2 && observed.trailers == 2 &&
                    observed.ends == 0 && observed.status == 200 && observed.checkedReentry);
        RUVIA_CHECK(observed.body == "abcdef" && observed.trailerNames == "x-onex-two");
        RUVIA_CHECK(session.release(4));
        RUVIA_CHECK(session.retainedBodyBytes() == 0);
        RUVIA_CHECK(session.release(0));
    }
    RUVIA_CHECK(memory.allocations == memory.deallocations && memory.liveBytes == 0);
}

RUVIA_TEST(http3ClientSansIoSessionStreamedOwnerCommitsFinAfterSuccessfulFeed) {
    CountingResource memory;
    {
        ruvia::detail::Http3ClientSansIoSessionEngine session(&memory);
        StreamedEvents observed(&memory);
        RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet,
                               {.callback = collectStreamedResponse, .context = &observed})
                        .scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(0, responseHead()).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
        RUVIA_CHECK(observed.heads == 1 && observed.ends == 0 && !session.response(0));
        RUVIA_CHECK(session.feed(0, body("streamed")).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
        RUVIA_CHECK(session.feed(0, responseTrailers()).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
        RUVIA_CHECK(observed.ends == 0 && !session.response(0));
        RUVIA_CHECK(session.feed(0, {}, true).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        const auto retained = session.response(0);
        RUVIA_CHECK(retained && retained->complete && retained->status == 200);
        RUVIA_CHECK(retained->headers.size() == 1 && retained->trailers.size() == 2);
        RUVIA_CHECK(retained->body.empty() && observed.body == "streamed");
        RUVIA_CHECK(observed.ends == 0);
        RUVIA_CHECK(session.release(0));
    }
    RUVIA_CHECK(memory.allocations > 0);
    RUVIA_CHECK(memory.allocations == memory.deallocations && memory.liveBytes == 0);
}

RUVIA_TEST(http3ClientSansIoSessionStreamedMalformedFinCannotPublishCompletion) {
    CountingResource memory;
    {
        ruvia::detail::Http3ClientSansIoSessionEngine session(&memory);
        StreamedEvents observed(&memory);
        RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet,
                               {.callback = collectStreamedResponse, .context = &observed})
                        .scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        std::pmr::monotonic_buffer_resource scratch;
        constexpr std::array fields{ruvia::Http3FieldSectionFieldView{":status", "200"},
            ruvia::Http3FieldSectionFieldView{"content-length", "3"}};
        const auto encoded = ruvia::encodeHttp3FieldSection(fields, &scratch);
        RUVIA_CHECK(session.feed(0, frame(1, *encoded)).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        const auto mismatch = session.feed(0, body("no"), true);
        RUVIA_CHECK(mismatch.scope == ruvia::Http3ConnectionErrorScope::kStream);
        RUVIA_CHECK(observed.body == "no" && observed.ends == 0);
        const auto failed = session.response(0);
        RUVIA_CHECK(failed && !failed->complete && failed->result.scope == ruvia::Http3ConnectionErrorScope::kStream);
        RUVIA_CHECK(planMatches(failed->responseBodyPlan, ruvia::HttpKnownMethod::kGet, 200,
            ruvia::HttpResponseContentSemantics::kWithContent, false));
        RUVIA_CHECK(session.release(0));
    }
    RUVIA_CHECK(memory.allocations == memory.deallocations && memory.liveBytes == 0);
}

RUVIA_TEST(http3ClientSansIoSessionStreamedCancellationKeepsEarlierOwnedResultAlive) {
    CountingResource memory;
    {
        ruvia::detail::Http3ClientSansIoSessionEngine session(&memory);
        StreamedEvents earlier(&memory);
        StreamedEvents cancelled(&memory);
        RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet,
                               {.callback = collectStreamedResponse, .context = &earlier})
                        .scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.registerRequest(4, ruvia::HttpKnownMethod::kGet,
                               {.callback = collectStreamedResponse, .context = &cancelled})
                        .scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(0, responseHead()).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(0, body("kept"), true).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        RUVIA_CHECK(session.feed(4, responseHead()).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(4, body("temporary")).scope == ruvia::Http3ConnectionErrorScope::kNone);
        // A transport owner must have terminated both QUIC stream directions
        // before this local parser retirement; no further bytes are fed to 4.
        RUVIA_CHECK(session.cancelRequest(4));
        RUVIA_CHECK(session.response(4)->result.status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kLocalCancelled);
        RUVIA_CHECK(session.release(4));
        RUVIA_CHECK(earlier.body == "kept" && cancelled.body == "temporary");
        RUVIA_CHECK(session.response(0)->complete && session.release(0));
        RUVIA_CHECK(session.retainedBodyBytes() == 0);
    }
    RUVIA_CHECK(memory.allocations > 0);
    RUVIA_CHECK(memory.allocations == memory.deallocations && memory.liveBytes == 0);
}

RUVIA_TEST(http3ClientSansIoSessionStreamedSinkExceptionFailsEntireConnection) {
    CountingResource memory;
    {
        ruvia::detail::Http3ClientSansIoSessionEngine session(&memory);
        StreamedEvents observed(&memory);
        observed.failBody = true;
        RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet,
                               {.callback = collectStreamedResponse, .context = &observed})
                        .scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.registerRequest(4, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(0, responseHead()).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        auto mixed = body("explode");
        const auto illegal = frame(4, {});  // SETTINGS on a response stream.
        mixed.insert(mixed.end(), illegal.begin(), illegal.end());
        {
            ruvia::detail::Http3ClientSansIoSessionEngine protocolOnly(&memory);
            RUVIA_CHECK(protocolOnly.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                        ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(protocolOnly.feed(0, responseHead()).scope ==
                        ruvia::Http3ConnectionErrorScope::kNone);
            const auto parsed = protocolOnly.feed(0, mixed);
            RUVIA_CHECK(parsed.scope == ruvia::Http3ConnectionErrorScope::kConnection);
            RUVIA_CHECK(parsed.code == ruvia::Http3ConnectionErrorCode::kFrameUnexpected);
            RUVIA_CHECK(protocolOnly.release(0));
        }
        bool threw{};
        try {
            (void)session.feed(0, mixed);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        const auto failed = session.response(0);
        RUVIA_CHECK(failed && failed->result.scope == ruvia::Http3ConnectionErrorScope::kConnection &&
                    failed->result.code == ruvia::Http3ConnectionErrorCode::kInternalError);
        RUVIA_CHECK(planMatches(failed->responseBodyPlan, ruvia::HttpKnownMethod::kGet, 200,
            ruvia::HttpResponseContentSemantics::kWithContent, false));
        RUVIA_CHECK(session.response(4)->result.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(session.release(0) && session.release(4));
    }
    RUVIA_CHECK(memory.allocations == memory.deallocations && memory.liveBytes == 0);
}

RUVIA_TEST(http3ClientSansIoSessionPropagatesConnectionFailureToLiveStreams) {
    std::pmr::monotonic_buffer_resource worker;
    ruvia::detail::Http3ClientSansIoSessionEngine session(&worker);
    RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet).status ==
                ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
    RUVIA_CHECK(session.registerRequest(4, ruvia::HttpKnownMethod::kGet).status ==
                ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
    const auto failure = session.feed(0, body("unexpected"));
    RUVIA_CHECK(failure.status == ruvia::detail::Http3ClientSansIoSessionStatus::kConnectionError);
    RUVIA_CHECK(!session.response(0)->responseBodyPlan);
    RUVIA_CHECK(!session.response(4)->responseBodyPlan);
    RUVIA_CHECK(session.response(4)->result.status ==
                ruvia::detail::Http3ClientSansIoSessionStatus::kConnectionError);
    RUVIA_CHECK(session.release(0));
    RUVIA_CHECK(session.release(4));
}

RUVIA_TEST(http3ClientSansIoSessionFeedsPeerCriticalStreamsAndPropagatesClosure) {
    std::pmr::unsynchronized_pool_resource worker;
    ruvia::detail::Http3ClientSansIoSessionEngine session(&worker);
    RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet).status ==
                ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
    std::vector<char> control{0};
    const auto settings = frame(4, {});
    control.insert(control.end(), settings.begin(), settings.end());
    RUVIA_CHECK(session.feed(3, control).status ==
                ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
    RUVIA_CHECK(!session.response(3).has_value());
    const auto closed = session.feed(3, {}, true);
    RUVIA_CHECK(closed.status == ruvia::detail::Http3ClientSansIoSessionStatus::kConnectionError);
    RUVIA_CHECK(closed.code == ruvia::Http3ConnectionErrorCode::kClosedCriticalStream);
    RUVIA_CHECK(session.response(0)->result.status ==
                ruvia::detail::Http3ClientSansIoSessionStatus::kConnectionError);
    RUVIA_CHECK(session.release(0));
}

RUVIA_TEST(http3ClientSansIoSessionBoundsStreamsAndBodyAndRetainsReset) {
    std::pmr::monotonic_buffer_resource worker;
    ruvia::detail::Http3ClientSansIoSessionEngine::Limits limits{};
    limits.maxLiveStreams = 1;
    limits.maxBodyBytesPerStream = 2;
    limits.maxTotalBodyBytes = 2;
    ruvia::detail::Http3ClientSansIoSessionEngine session(&worker, limits);
    RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet).status ==
                ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
    RUVIA_CHECK(session.registerRequest(4, ruvia::HttpKnownMethod::kGet).status ==
                ruvia::detail::Http3ClientSansIoSessionStatus::kStreamLimitExceeded);
    RUVIA_CHECK(session.feed(0, responseHead()).status == ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
    const auto exceeded = session.feed(0, body("abc"));
    RUVIA_CHECK(exceeded.status == ruvia::detail::Http3ClientSansIoSessionStatus::kBodyLimitExceeded);
    RUVIA_CHECK(exceeded.scope == ruvia::Http3ConnectionErrorScope::kConnection);
    RUVIA_CHECK_EQ(session.retainedBodyBytes(), 0U);
    RUVIA_CHECK(session.response(0)->result.status ==
                ruvia::detail::Http3ClientSansIoSessionStatus::kBodyLimitExceeded);
    RUVIA_CHECK(!session.response(0)->complete);
    RUVIA_CHECK(session.release(0));
    RUVIA_CHECK(session.registerRequest(4, ruvia::HttpKnownMethod::kGet).status ==
                ruvia::detail::Http3ClientSansIoSessionStatus::kConnectionError);

    ruvia::detail::Http3ClientSansIoSessionEngine resetSession(&worker, limits);
    RUVIA_CHECK(resetSession.registerRequest(4, ruvia::HttpKnownMethod::kGet).status ==
                ruvia::detail::Http3ClientSansIoSessionStatus::kNeedMoreData);
    RUVIA_CHECK(resetSession.feed(4, {}, false, true).status ==
                ruvia::detail::Http3ClientSansIoSessionStatus::kReset);
    RUVIA_CHECK(resetSession.response(4)->reset);
    RUVIA_CHECK(!resetSession.response(4)->responseBodyPlan);
    RUVIA_CHECK(resetSession.release(4));
}

RUVIA_TEST(http3ClientSansIoSessionBodyLimitCannotBeHiddenBySameBatchLengthMismatch) {
    CountingResource memory;
    {
        ruvia::detail::Http3ClientSansIoSessionEngine session(&memory,
            {.maxLiveStreams = 2, .maxBodyBytesPerStream = 2, .maxTotalBodyBytes = 2});
        std::pmr::monotonic_buffer_resource scratch;
        constexpr std::array fields{ruvia::Http3FieldSectionFieldView{":status", "200"},
            ruvia::Http3FieldSectionFieldView{"content-length", "4"}};
        const auto encoded = ruvia::encodeHttp3FieldSection(fields, &scratch);
        const auto head = frame(1, *encoded);
        // Establish that the identical DATA+FIN would independently fail the
        // Content-Length contract; the body-limit check must not lose to it.
        {
            ruvia::detail::Http3ClientSansIoSessionEngine protocolOnly(&memory);
            RUVIA_CHECK(protocolOnly.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                        ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(protocolOnly.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
            const auto mismatch = protocolOnly.feed(0, body("abc"), true);
            RUVIA_CHECK(mismatch.scope == ruvia::Http3ConnectionErrorScope::kStream);
            RUVIA_CHECK(mismatch.code == ruvia::Http3ConnectionErrorCode::kMessageError);
            RUVIA_CHECK(protocolOnly.release(0));
        }
        RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.registerRequest(4, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
        const auto failure = session.feed(0, body("abc"), true);
        RUVIA_CHECK(failure.status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kBodyLimitExceeded);
        RUVIA_CHECK(failure.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(session.response(4)->result.scope ==
                    ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(session.release(0) && session.release(4));
        RUVIA_CHECK_EQ(session.retainedBodyBytes(), 0U);

        ruvia::detail::Http3ClientSansIoSessionEngine protocolPriority(&memory,
            {.maxLiveStreams = 2, .maxBodyBytesPerStream = 2, .maxTotalBodyBytes = 2});
        RUVIA_CHECK(protocolPriority.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(protocolPriority.registerRequest(4, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(protocolPriority.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
        auto mixed = body("abc");
        const auto invalid = frame(4, {});  // Forbidden SETTINGS after a response head.
        mixed.insert(mixed.end(), invalid.begin(), invalid.end());
        const auto protocolFailure = protocolPriority.feed(0, mixed);
        RUVIA_CHECK(protocolFailure.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(protocolFailure.code == ruvia::Http3ConnectionErrorCode::kFrameUnexpected);
        RUVIA_CHECK(protocolPriority.release(0) && protocolPriority.release(4));
    }
    RUVIA_CHECK(memory.allocations == memory.deallocations && memory.liveBytes == 0);
}

RUVIA_TEST(http3ClientSansIoSessionAggregateOverflowClosesOnlyIncompleteResults) {
    CountingResource worker;
    {
        ruvia::detail::Http3ClientSansIoSessionEngine session(&worker,
            {.maxLiveStreams = 2, .maxBodyBytesPerStream = 4, .maxTotalBodyBytes = 3});
        const auto head = responseHead();
        RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.registerRequest(4, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(4, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(0, body("ok"), true).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        const auto excess = session.feed(4, body("no"));
        RUVIA_CHECK(excess.status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kBodyLimitExceeded);
        RUVIA_CHECK(excess.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(session.response(0)->complete);
        RUVIA_CHECK(session.response(0)->result.status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        RUVIA_CHECK(!session.response(4)->complete);
        RUVIA_CHECK(session.response(4)->result.status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kBodyLimitExceeded);
        RUVIA_CHECK_EQ(session.retainedBodyBytes(), 2U);
        RUVIA_CHECK(session.release(0));
        RUVIA_CHECK(session.release(4));
        RUVIA_CHECK_EQ(session.retainedBodyBytes(), 0U);
    }
    RUVIA_CHECK_EQ(worker.allocations, worker.deallocations);
    RUVIA_CHECK_EQ(worker.liveBytes, 0U);
}

RUVIA_TEST(http3ClientSansIoSessionSharesBodyBudgetWithRetainedResultsAcrossTransfers) {
    CountingResource memory;
    {
        ruvia::detail::Http3ClientBodyBudget budget(3072);
        std::pmr::string older(1024, 'z', &memory);
        RUVIA_CHECK(budget.tryRetain(older.size()));
        const auto baseline = memory.liveBytes;
        const std::string payload(2048, 'x');
        for (unsigned iteration = 0; iteration < 24; ++iteration) {
            std::optional<std::pmr::string> retained;
            {
                ruvia::detail::Http3ClientSansIoSessionEngine engine(&memory, budget);
                RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                            ruvia::Http3ConnectionErrorScope::kNone);
                RUVIA_CHECK(engine.feed(0, responseHead()).scope == ruvia::Http3ConnectionErrorScope::kNone);
                RUVIA_CHECK(engine.feed(0, body(payload), true).status ==
                            ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
                RUVIA_CHECK_EQ(budget.used(), std::size_t{3072});
                retained = engine.takeBody(0);
                RUVIA_CHECK_EQ(budget.used(), older.size());
                RUVIA_CHECK(retained && budget.tryRetain(retained->size()));
                RUVIA_CHECK(engine.release(0));
                RUVIA_CHECK(engine.registerRequest(4, ruvia::HttpKnownMethod::kGet).scope ==
                            ruvia::Http3ConnectionErrorScope::kNone);
                RUVIA_CHECK(engine.feed(4, responseHead()).scope == ruvia::Http3ConnectionErrorScope::kNone);
                // No receive-owned body remains, but retained results exhaust
                // the same connection budget. Even one more body byte fails.
                const auto overflow = engine.feed(4, body("!"), true);
                RUVIA_CHECK(overflow.status == ruvia::detail::Http3ClientSansIoSessionStatus::kBodyLimitExceeded);
                RUVIA_CHECK_EQ(budget.used(), std::size_t{3072});
                RUVIA_CHECK_EQ(engine.retainedBodyBytes(), std::size_t{0});
                RUVIA_CHECK(engine.release(4));
            }
            RUVIA_CHECK(retained && std::string_view(*retained) == payload);
            const auto resultBytes = retained->size();
            retained.reset();
            budget.release(resultBytes);
            RUVIA_CHECK_EQ(memory.liveBytes, baseline);
            RUVIA_CHECK_EQ(budget.used(), older.size());
            RUVIA_CHECK(older.front() == 'z');
        }
        budget.release(older.size());
    }
    RUVIA_CHECK_EQ(memory.liveBytes, 0U);
    RUVIA_CHECK_EQ(memory.allocations, memory.deallocations);
}

RUVIA_TEST(http3ClientSansIoSessionBodyBudgetRollsBackAllocationFailureAndReleasesCancellation) {
    CountingResource memory;
    ruvia::detail::Http3ClientBodyBudget budget(4096);
    const std::string payload(2048, 'x');
    for (const bool rejectAllocation : {false, true}) {
        {
            ruvia::detail::Http3ClientSansIoSessionEngine engine(&memory, budget);
            RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                        ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(engine.feed(0, responseHead()).scope == ruvia::Http3ConnectionErrorScope::kNone);
            memory.reject = rejectAllocation;
            if (rejectAllocation) {
                RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)engine.feed(0, body(payload)); }));
                RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
            } else {
                RUVIA_CHECK(engine.feed(0, body(payload)).scope == ruvia::Http3ConnectionErrorScope::kNone);
                RUVIA_CHECK_EQ(budget.used(), payload.size());
                RUVIA_CHECK(engine.cancelRequest(0));  // Transport is already retired by the test owner.
                RUVIA_CHECK_EQ(budget.used(), payload.size());
            }
            memory.reject = false;
            // Destruction, even without release(), must return all remaining
            // receive-owned reservations and PMR allocations.
        }
        RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
        RUVIA_CHECK_EQ(memory.liveBytes, 0U);
    }
    RUVIA_CHECK_EQ(memory.allocations, memory.deallocations);
}

RUVIA_TEST(http3ClientSansIoSessionTransfersCompletedBodyWithoutCopyAndReclaimsLaterBodies) {
    CountingResource memory;
    {
        ruvia::detail::Http3ClientSansIoSessionEngine session(&memory);
        const auto head = responseHead();
        const std::string payload(2048, 'x');
        RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(!session.takeBody(0));
        RUVIA_CHECK(session.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(0, body(payload), true).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        const auto* original = session.response(0)->body.data();
        const auto beforeTransfer = memory.bodyAllocations;
        auto retained = session.takeBody(0);
        RUVIA_CHECK(retained && retained->data() == original);
        RUVIA_CHECK_EQ(memory.bodyAllocations, beforeTransfer);
        RUVIA_CHECK_EQ(session.retainedBodyBytes(), 0U);
        RUVIA_CHECK(!session.takeBody(0));
        RUVIA_CHECK(session.response(0)->headers.front().value == "owned");
        RUVIA_CHECK(session.release(0));
        const auto baseline = memory.liveBytes;
        for (std::uint64_t index = 1; index <= 24; ++index) {
            const auto id = index * 4;
            RUVIA_CHECK(session.registerRequest(id, ruvia::HttpKnownMethod::kGet).scope ==
                        ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(session.feed(id, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(session.feed(id, body(payload), true).status ==
                        ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
            auto temporary = session.takeBody(id);
            RUVIA_CHECK(temporary && std::string_view(*temporary) == payload);
            RUVIA_CHECK(session.release(id));
            temporary.reset();
            RUVIA_CHECK_EQ(memory.liveBytes, baseline);
            RUVIA_CHECK(retained && std::string_view(*retained) == payload);
        }
        const auto withResult = memory.liveBytes;
        retained.reset();
        RUVIA_CHECK(memory.liveBytes < withResult);
    }
    RUVIA_CHECK_EQ(memory.liveBytes, 0U);
    RUVIA_CHECK_EQ(memory.allocations, memory.deallocations);
}

RUVIA_TEST(http3ClientSansIoSessionReturnsEachRetiredBodyWhileOtherResultSurvives) {
    CountingResource worker;
    {
        ruvia::detail::Http3ClientSansIoSessionEngine session(&worker);
        const auto head = responseHead();
        const auto held = body("held");
        const auto repeated = body("next");
        RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(0, held, true).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        const auto retained = session.response(0);
        RUVIA_CHECK(retained && std::string_view(retained->body.data(), retained->body.size()) == "held");
        std::size_t baseline{};
        for (std::uint64_t i = 1; i != 48; ++i) {
            const auto id = i * 4;
            RUVIA_CHECK(session.registerRequest(id, ruvia::HttpKnownMethod::kGet).scope ==
                        ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(session.feed(id, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(session.feed(id, repeated, true).status ==
                        ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
            RUVIA_CHECK_EQ(session.retainedBodyBytes(), 8U);
            RUVIA_CHECK(session.release(id));
            RUVIA_CHECK_EQ(session.retainedBodyBytes(), 4U);
            RUVIA_CHECK(std::string_view(retained->body.data(), retained->body.size()) == "held");
            if (i == 1) {
                baseline = worker.liveBytes;
            } else {
                RUVIA_CHECK_EQ(worker.liveBytes, baseline);
            }
        }
        RUVIA_CHECK(session.release(0));
        RUVIA_CHECK_EQ(session.retainedBodyBytes(), 0U);
    }
    RUVIA_CHECK(worker.allocations > 0);
    RUVIA_CHECK_EQ(worker.allocations, worker.deallocations);
    RUVIA_CHECK_EQ(worker.liveBytes, 0U);
}

RUVIA_TEST(http3ClientSansIoSessionOwnsValidatedResponseTrailersUntilExplicitRelease) {
    CountingResource worker;
    {
        ruvia::detail::Http3ClientSansIoSessionEngine session(&worker);
        RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        const auto head = responseHead();
        RUVIA_CHECK(session.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(0, body("ok")).scope == ruvia::Http3ConnectionErrorScope::kNone);
        auto trailers = responseTrailers();
        RUVIA_CHECK(session.feed(0, trailers, true).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        std::fill(trailers.begin(), trailers.end(), '\0');
        const auto response = session.response(0);
        RUVIA_CHECK(response && response->complete && response->trailers.size() == 2);
        RUVIA_CHECK(response && response->trailers[0].name == "x-one" &&
                    response->trailers[0].value == "first");
        RUVIA_CHECK(response && response->trailers[1].name == "x-two" &&
                    response->trailers[1].value == "second");
        RUVIA_CHECK(session.release(0));
        RUVIA_CHECK_EQ(session.retainedBodyBytes(), 0U);
    }
    RUVIA_CHECK_EQ(worker.allocations, worker.deallocations);
    RUVIA_CHECK_EQ(worker.liveBytes, 0U);
}

RUVIA_TEST(http3ClientSansIoSessionLocalCancellationIsolatesOtherResponsesAndReclaimsStorage) {
    CountingResource worker;
    {
        ruvia::detail::Http3ClientSansIoSessionEngine session(&worker);
        RUVIA_CHECK(!session.cancelRequest(0));  // cold request was never registered
        RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.registerRequest(4, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        const auto head = responseHead();
        RUVIA_CHECK(session.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(0, body("abandoned")).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(4, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(4, body("retained"), true).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        RUVIA_CHECK_EQ(session.retainedBodyBytes(), 17U);
        RUVIA_CHECK(session.cancelRequest(0));
        RUVIA_CHECK(!session.cancelRequest(0));
        RUVIA_CHECK(!session.cancelRequest(4));  // a completed response is not cancelled
        const auto cancelled = session.response(0);
        RUVIA_CHECK(cancelled && !cancelled->complete && !cancelled->reset);
        RUVIA_CHECK(cancelled && cancelled->result.status ==
                                     ruvia::detail::Http3ClientSansIoSessionStatus::kLocalCancelled);
        RUVIA_CHECK(cancelled && cancelled->result.scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(cancelled && std::string_view(cancelled->body.data(), cancelled->body.size()) == "abandoned");
        const auto allocatedBeforeRelease = worker.liveBytes;
        RUVIA_CHECK(session.release(0));
        RUVIA_CHECK(worker.liveBytes < allocatedBeforeRelease);
        RUVIA_CHECK_EQ(session.retainedBodyBytes(), 8U);
        RUVIA_CHECK(session.registerRequest(8, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(8, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(8, body("next"), true).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        RUVIA_CHECK(std::string_view(session.response(4)->body.data(), session.response(4)->body.size()) ==
                    "retained");
        RUVIA_CHECK(std::string_view(session.response(8)->body.data(), session.response(8)->body.size()) ==
                    "next");
        RUVIA_CHECK(session.release(4));
        RUVIA_CHECK(session.release(8));
        RUVIA_CHECK_EQ(session.retainedBodyBytes(), 0U);
    }
    RUVIA_CHECK_EQ(worker.allocations, worker.deallocations);
    RUVIA_CHECK_EQ(worker.liveBytes, 0U);
}

RUVIA_TEST(http3ClientSansIoSessionRepeatedCancelledStreamsReturnToBaselineWithEarlierResultAlive) {
    CountingResource worker;
    {
        ruvia::detail::Http3ClientSansIoSessionEngine session(&worker);
        const auto head = responseHead();
        RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(0, body("keep"), true).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        const auto baseline = worker.liveBytes;
        for (std::uint64_t streamId = 4; streamId <= 128; streamId += 4) {
            RUVIA_CHECK(session.registerRequest(streamId, ruvia::HttpKnownMethod::kGet).scope ==
                        ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(session.feed(streamId, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(session.feed(streamId, body("drop")).scope == ruvia::Http3ConnectionErrorScope::kNone);
            RUVIA_CHECK(session.cancelRequest(streamId));
            RUVIA_CHECK(session.release(streamId));
            RUVIA_CHECK_EQ(worker.liveBytes, baseline);
            RUVIA_CHECK_EQ(session.retainedBodyBytes(), 4U);
            const auto retained = session.response(0);
            RUVIA_CHECK(retained && retained->complete &&
                        std::string_view(retained->body.data(), retained->body.size()) == "keep");
        }
        RUVIA_CHECK(session.release(0));
        RUVIA_CHECK_EQ(session.retainedBodyBytes(), 0U);
    }
    RUVIA_CHECK_EQ(worker.allocations, worker.deallocations);
    RUVIA_CHECK_EQ(worker.liveBytes, 0U);
}

RUVIA_TEST(http3ClientSansIoSessionExposesPeerAdmissionAndGoawayCutoff) {
    CountingResource worker;
    {
        ruvia::detail::Http3ClientSansIoSessionEngine session(&worker);
        RUVIA_CHECK(!session.peerSettings());
        RUVIA_CHECK(!session.peerGoawayId());
        RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.registerRequest(4, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        constexpr std::array<char, 6> control{0x00, 0x04, 0x00, 0x07, 0x01, 0x04};
        RUVIA_CHECK(session.feed(3, control).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.peerSettings().has_value());
        RUVIA_CHECK(session.peerGoawayId() == 4);
        // Already-open request 4 remains visible: a connection driver can
        // distinguish the GOAWAY cutoff from an arbitrary transport failure.
        RUVIA_CHECK_EQ(session.liveStreamCount(), 2U);
        RUVIA_CHECK(!session.response(4));
        const auto rejected = session.registerRequest(8, ruvia::HttpKnownMethod::kGet);
        RUVIA_CHECK(rejected.scope == ruvia::Http3ConnectionErrorScope::kStream);
        RUVIA_CHECK(rejected.code == ruvia::Http3ConnectionErrorCode::kRequestRejected);
        RUVIA_CHECK_EQ(session.liveStreamCount(), 2U);
        RUVIA_CHECK(session.stop().scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(!session.peerSettings() && !session.peerGoawayId());
        RUVIA_CHECK(session.release(0));
        RUVIA_CHECK(session.release(4));
    }
    RUVIA_CHECK_EQ(worker.allocations, worker.deallocations);
    RUVIA_CHECK_EQ(worker.liveBytes, 0U);
}

RUVIA_TEST(http3ClientSansIoSessionLocalStopPreservesOnlyAlreadyCompleteResponses) {
    CountingResource worker;
    {
        ruvia::detail::Http3ClientSansIoSessionEngine session(&worker);
        const auto head = responseHead();
        RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.registerRequest(4, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(0, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(4, head).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.feed(0, body("ok"), true).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        const auto retained = session.response(0);
        const auto returnsBeforeStop = worker.deallocations;
        const auto stopped = session.stop();
        RUVIA_CHECK(worker.deallocations > returnsBeforeStop);
        RUVIA_CHECK(retained && std::string_view(retained->body.data(), retained->body.size()) == "ok");
        const auto live_after_stop = worker.liveBytes;
        RUVIA_CHECK(session.stop().status == stopped.status);
        // Repeated stop can create and release empty debug iterator proxies;
        // it must preserve the retained response and its live storage.
        RUVIA_CHECK_EQ(worker.liveBytes, live_after_stop);
        RUVIA_CHECK(retained && std::string_view(retained->body.data(), retained->body.size()) == "ok");
        RUVIA_CHECK(stopped.status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kTransportError);
        RUVIA_CHECK(stopped.scope == ruvia::Http3ConnectionErrorScope::kConnection);
        RUVIA_CHECK(stopped.code == ruvia::Http3ConnectionErrorCode::kNoError);
        RUVIA_CHECK(session.response(0)->complete);
        RUVIA_CHECK(session.response(0)->result.status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        RUVIA_CHECK(!session.response(4)->complete);
        RUVIA_CHECK(session.response(4)->result.status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kTransportError);
        RUVIA_CHECK(session.feed(4, {}, true).status ==
                    ruvia::detail::Http3ClientSansIoSessionStatus::kTransportError);
        RUVIA_CHECK(session.release(0));
        RUVIA_CHECK(session.release(4));
        RUVIA_CHECK_EQ(session.retainedBodyBytes(), 0U);
    }
    RUVIA_CHECK_EQ(worker.allocations, worker.deallocations);
    RUVIA_CHECK_EQ(worker.liveBytes, 0U);
}

RUVIA_TEST(http3ClientSansIoSessionAllocationFailureTerminatesEveryLiveStream) {
    CountingResource worker;
    {
        ruvia::detail::Http3ClientSansIoSessionEngine session(&worker);
        RUVIA_CHECK(session.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(session.registerRequest(4, ruvia::HttpKnownMethod::kGet).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        const auto head = responseHead();
        worker.reject = true;
        bool caught{};
        try {
            (void)session.feed(0, head);
        } catch (const std::bad_alloc&) {
            caught = true;
        }
        worker.reject = false;
        RUVIA_CHECK(caught);
        RUVIA_CHECK(session.response(0)->result.code == ruvia::Http3ConnectionErrorCode::kInternalError);
        RUVIA_CHECK(session.response(4)->result.code == ruvia::Http3ConnectionErrorCode::kInternalError);
        RUVIA_CHECK(session.release(0));
        RUVIA_CHECK(session.release(4));
    }
    RUVIA_CHECK_EQ(worker.allocations, worker.deallocations);
    RUVIA_CHECK_EQ(worker.liveBytes, 0U);
}

RUVIA_TEST(http3_client_push_events_associate_out_of_order_streams_and_control_frames_commit_once) {
    CountingResource memory;
    using Engine = ruvia::detail::Http3ClientSansIoSessionEngine;
    struct Observed final {
        std::vector<ruvia::Http3ConnectionEventKind> events;
        std::string path;
        std::string body;
        std::uint64_t stream{};
        std::uint64_t push{};
        bool reentryRejected{};
        Engine* engine{};
    } observed;
    {
        Engine client(&memory);
        ruvia::Http3Connection server(ruvia::Http3PeerRole::kServer, &memory);
        observed.engine = &client;
        client.observePushes([](void* raw, const ruvia::Http3ConnectionEvent& event) {
            auto& state = *static_cast<Observed*>(raw);
            state.events.push_back(event.kind);
            state.push = *event.pushId;
            state.stream = event.streamId;
            if (event.kind == ruvia::Http3ConnectionEventKind::kPushPromise) {
                state.path = event.head->path;
                state.reentryRejected = !state.engine->queueCancelPush(*event.pushId) &&
                                        !state.engine->queueMaxPushId(100) && !state.engine->retirePushStream(3);
            }
            if (event.kind == ruvia::Http3ConnectionEventKind::kBody) {
                state.body.append(event.body.data(), event.body.size());
            }
        },
            &observed);
        RUVIA_CHECK(client.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope == ruvia::Http3ConnectionErrorScope::kNone);
        memory.reject = true;
        bool failed = false;
        try {
            (void)client.queueMaxPushId(0);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        memory.reject = false;
        RUVIA_CHECK(failed);
        RUVIA_CHECK(client.pendingControlOutput().empty());
        RUVIA_CHECK(client.queueMaxPushId(0));
        const char prefix[]{0, 4, 0};
        RUVIA_CHECK(server.feed(2, prefix, false, false, [](void*, const ruvia::Http3ConnectionEvent&) {}, nullptr).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(server.feed(2, client.pendingControlOutput(), false, false, [](void*, const ruvia::Http3ConnectionEvent&) {}, nullptr).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(server.peerMaxPushId() == 0);
        RUVIA_CHECK(client.consumeControlOutput(client.pendingControlOutput().size()));
        auto promise = server.preparePushPromise(0, 0, {.authority = "example.test", .path = "/asset"});
        RUVIA_CHECK(promise.has_value());
        const char pushPrefix[]{1, 0};
        auto pending = client.feed(3, pushPrefix);
        RUVIA_CHECK(pending.status == ruvia::detail::Http3ClientSansIoSessionStatus::kPushPromisePending);
        RUVIA_CHECK(observed.events.size() == 1 && observed.events[0] == ruvia::Http3ConnectionEventKind::kPushStream);
        RUVIA_CHECK(observed.stream == 3 && observed.push == 0);
        RUVIA_CHECK(client.feed(0, *promise).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(observed.path == "/asset" && observed.reentryRejected);
        auto wire = responseHead("200", "5");
        auto payload = body("asset");
        wire.insert(wire.end(), payload.begin(), payload.end());
        RUVIA_CHECK(client.feed(3, wire, true).status == ruvia::detail::Http3ClientSansIoSessionStatus::kMessageEnd);
        RUVIA_CHECK(observed.body == "asset");
        RUVIA_CHECK(client.retirePushStream(3));
        RUVIA_CHECK(client.queuePushPriorityUpdate(0, {.urgency = 1, .incremental = true}));
        RUVIA_CHECK(client.queueMaxPushId(1));
        RUVIA_CHECK(client.consumeControlOutput(client.pendingControlOutput().size()));
        RUVIA_CHECK(client.queueCancelPush(0));
        RUVIA_CHECK(client.pendingControlOutput().size() == 3);
        RUVIA_CHECK(client.consumeControlOutput(3));
        RUVIA_CHECK(client.pendingControlOutput().empty());
        const auto liveBeforeRepeatedOutput = memory.liveBytes;
        for (std::uint64_t maximum = 2; maximum < 102; ++maximum) {
            RUVIA_CHECK(client.queueMaxPushId(maximum));
            RUVIA_CHECK(client.consumeControlOutput(client.pendingControlOutput().size()));
            RUVIA_CHECK(memory.liveBytes == liveBeforeRepeatedOutput);
        }
        RUVIA_CHECK(client.cancelRequest(0));
        RUVIA_CHECK(client.release(0));
    }
    RUVIA_CHECK(memory.liveBytes == 0 && memory.allocations == memory.deallocations);
}
