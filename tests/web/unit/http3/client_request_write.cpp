#include <cstddef>
#include <memory_resource>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "http3/Http3ClientRequestWrite.h"
#include "test_harness.h"

namespace {
class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t allocations{};
    std::size_t returns{};
    std::size_t attempts{};
    std::size_t failOnAttempt{};
    std::size_t largeAllocations{};
    std::size_t largeReturns{};
    bool reject{};

private:
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        if (size >= 32) {
            ++attempts;
            if (reject || attempts == failOnAttempt) {
                throw std::bad_alloc();
            }
        }
        ++allocations;
        if (size >= 1024) {
            ++largeAllocations;
        }
        return std::pmr::new_delete_resource()->allocate(size, alignment);
    }
    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        ++returns;
        if (size >= 1024) {
            ++largeReturns;
        }
        std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
using Cursor = ruvia::detail::Http3ClientRequestWrite;

Cursor create(std::string_view body, std::pmr::memory_resource* resource, std::string_view method = "POST") {
    ruvia::detail::HttpClientRequestStorage request(method, "/upload?q=1", resource);
    request.setBody(body);
    auto result = Cursor::create(std::move(request), "https", "example.com", resource);
    if (!result) {
        throw std::runtime_error("HTTP/3 request cursor creation failed");
    }
    return std::move(*result);
}

bool sendHeaders(Cursor& cursor) {
    auto output = cursor.next();
    return output && !output->empty() && static_cast<bool>(cursor.acknowledge(output->size()));
}

void checkEncodedBody(Cursor& cursor, ruvia::testing::TestContext& ruvia_ctx,
    std::string_view expectedHead, std::string_view body, const char* expectedBodyAddress,
    bool sameAddress) {
    auto head = cursor.next();
    RUVIA_CHECK(head.has_value());
    if (!head) {
        return;
    }
    RUVIA_CHECK_EQ(std::string_view(head->data(), head->size()), expectedHead);
    const auto headAck = cursor.acknowledge(head->size());
    RUVIA_CHECK(headAck.has_value());
    if (!headAck) {
        return;
    }

    auto frame = cursor.next();
    RUVIA_CHECK(frame.has_value());
    if (!frame) {
        return;
    }
    const auto frameAck = cursor.acknowledge(frame->size());
    RUVIA_CHECK(frameAck.has_value());
    if (!frameAck) {
        return;
    }

    auto payload = cursor.next();
    RUVIA_CHECK(payload.has_value());
    if (!payload) {
        return;
    }
    RUVIA_CHECK_EQ(payload->size(), body.size());
    RUVIA_CHECK_EQ(std::string_view(payload->data(), payload->size()), body);
    RUVIA_CHECK((payload->data() == expectedBodyAddress) == sameAddress);
}
}  // namespace

RUVIA_TEST(http3ClientRequestWriteRetiredHandoffPreservesWholeRequestAndReturnsStorage) {
    CountingResource memory;
    const std::string body(2048, 'p');
    std::optional<ruvia::detail::HttpClientRequestStorage> retained;
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        const auto phase = scenario % 3;
        const std::string_view method = scenario < 3 ? "POST" : "HEAD";
        {
            auto cursor = create(body, &memory, method);
            const auto head = cursor.next();
            RUVIA_CHECK(head && !head->empty());
            const std::string encodedHead(head->data(), head->size());
            if (phase != 0) {
                RUVIA_CHECK(cursor.acknowledge(head->size()));
                const auto framing = cursor.next();
                RUVIA_CHECK(framing && cursor.acknowledge(framing->size()));
                const auto payload = cursor.next();
                RUVIA_CHECK(payload && payload->size() == body.size());
                RUVIA_CHECK(cursor.acknowledge(phase == 1 ? 7 : payload->size()));
                if (phase == 1) {
                    RUVIA_CHECK(cursor.next()->size() == body.size() - 7);
                } else {
                    RUVIA_CHECK(cursor.acknowledgeFin(true));
                }
            }
            // The caller has now retired transport; a previous WANT span can
            // be invalidated, but all original bytes must remain replayable.
            const auto beforeTransfer = memory.largeAllocations;
            auto request = cursor.takeRequestAfterRetirement();
            RUVIA_CHECK(request && request->method() == method && request->target() == "/upload?q=1");
            RUVIA_CHECK(request && request->body() == body);
            RUVIA_CHECK_EQ(memory.largeAllocations, beforeTransfer);
            RUVIA_CHECK(!cursor.next() && !cursor.takeRequestAfterRetirement());
            auto replacement = Cursor::create(std::move(*request), "https", "example.com", &memory);
            RUVIA_CHECK(replacement.has_value());
            const auto replayHead = replacement->next();
            RUVIA_CHECK(replayHead && std::string_view(replayHead->data(), replayHead->size()) == encodedHead);
            retained.emplace(std::move(*replacement->takeRequestAfterRetirement()));
        }
        RUVIA_CHECK(retained->body() == body);
        RUVIA_CHECK(memory.allocations > memory.returns);
        retained.reset();
        RUVIA_CHECK_EQ(memory.allocations, memory.returns);
    }
}

RUVIA_TEST(http3ClientRequestWriteHandoffNormalizesIntoNewOwnerBeforeOldResourceRetires) {
    CountingResource destination;
    const std::string body(2048, 'p');
    const std::string header(256, 'h');
    for (const bool failAllocation : {false, true}) {
        std::optional<Cursor> replacement;
        std::string expectedHead;
        {
            CountingResource source;
            {
                ruvia::detail::HttpClientRequestStorage request("POST", "/handoff", &source);
                request.appendHeader("x-retained", header).setBody(body);
                auto original = Cursor::create(std::move(request), "https", "example.com", &source);
                RUVIA_CHECK(original.has_value());
                const auto head = original->next();
                expectedHead.assign(head->data(), head->size());
                auto transferred = original->takeRequestAfterRetirement();
                destination.reject = failAllocation;
                auto rebuilt = Cursor::create(std::move(*transferred), "https", "example.com", &destination);
                destination.reject = false;
                if (failAllocation) {
                    RUVIA_CHECK(!rebuilt && rebuilt.error() == Cursor::Error::kOutOfMemory);
                } else {
                    RUVIA_CHECK(rebuilt.has_value());
                    replacement.emplace(std::move(*rebuilt));
                }
            }
            RUVIA_CHECK_EQ(source.allocations, source.returns);
        }
        // The source allocator no longer exists: HEADERS and DATA must now
        // refer exclusively to the replacement owner's storage.
        if (replacement) {
            const auto head = replacement->next();
            RUVIA_CHECK(head && std::string_view(head->data(), head->size()) == expectedHead);
            RUVIA_CHECK(replacement->acknowledge(head->size()));
            const auto frame = replacement->next();
            RUVIA_CHECK(frame && replacement->acknowledge(frame->size()));
            const auto payload = replacement->next();
            RUVIA_CHECK(payload && std::string_view(payload->data(), payload->size()) == body);
            RUVIA_CHECK(replacement->acknowledge(payload->size()));
            RUVIA_CHECK(replacement->acknowledgeFin(true));
        }
        replacement.reset();
        RUVIA_CHECK_EQ(destination.allocations, destination.returns);
    }
}

RUVIA_TEST(http3ClientRequestWriteOwnsBodyAndSegmentsTheRequest) {
    auto cursor = create("payload", nullptr);
    RUVIA_CHECK(sendHeaders(cursor));
    auto frame = cursor.next();
    RUVIA_CHECK(frame && !frame->empty() && static_cast<unsigned char>((*frame)[0]) == 0);
    if (frame) {
        RUVIA_CHECK(cursor.acknowledge(frame->size()));
    }
    auto payload = cursor.next();
    RUVIA_CHECK(payload && std::string_view(payload->data(), payload->size()) == "payload");
    if (payload) {
        RUVIA_CHECK(cursor.acknowledge(payload->size()));
    }
    RUVIA_CHECK(cursor.finReady());
    RUVIA_CHECK(!cursor.finished());
    RUVIA_CHECK(cursor.acknowledgeFin(true));
    RUVIA_CHECK(cursor.finished());
}

RUVIA_TEST(http3ClientRequestWritePartialAcknowledgementAndWantKeepSameBytes) {
    auto cursor = create("body", nullptr);
    auto offered = cursor.next();
    RUVIA_CHECK(offered && offered->size() > 1);
    if (!offered || offered->empty()) {
        return;
    }
    const auto* address = offered->data();
    const auto length = offered->size();
    const auto first = offered->front();
    RUVIA_CHECK(cursor.acknowledge(0));
    auto retry = cursor.next();
    RUVIA_CHECK(retry && retry->data() == address && retry->size() == length && retry->front() == first);
    RUVIA_CHECK(cursor.acknowledge(1));
    auto rest = cursor.next();
    RUVIA_CHECK(rest && rest->data() == address + 1 && rest->size() == length - 1);
}

RUVIA_TEST(http3ClientRequestWriteParallelCursorsAndNoZeroLengthData) {
    auto first = create("one", nullptr);
    auto second = create("two", nullptr);
    auto a = first.next();
    auto b = second.next();
    RUVIA_CHECK(a && b && a->data() != b->data());
    auto empty = create("", nullptr);
    RUVIA_CHECK(sendHeaders(empty));
    auto noData = empty.next();
    RUVIA_CHECK(noData && noData->empty() && empty.finReady());
    RUVIA_CHECK(empty.acknowledgeFin(true));
    auto head = create("", nullptr, "HEAD");
    RUVIA_CHECK(sendHeaders(head));
    auto headDone = head.next();
    RUVIA_CHECK(headDone && headDone->empty() && head.finReady());
    RUVIA_CHECK(head.acknowledgeFin(true));
}

RUVIA_TEST(http3ClientRequestWriteUsesTraceZeroContentPlanWithoutDroppingSuppliedBytes) {
    CountingResource resource;
    {
        ruvia::detail::HttpClientRequestStorage request("TRACE", "/", &resource);
        request.setBody("must-not-be-silently-removed");
        const auto rejected = Cursor::create(std::move(request), "https", "example.com", &resource);
        RUVIA_CHECK(!rejected && rejected.error() == Cursor::Error::kRequestEncoding);
        auto empty = create("", &resource, "TRACE");
        RUVIA_CHECK(sendHeaders(empty));
        const auto noData = empty.next();
        RUVIA_CHECK(noData && noData->empty() && empty.finReady());
        RUVIA_CHECK(empty.acknowledgeFin(true));
        ruvia::detail::HttpClientRequestStorage sensitive("TRACE", "/", &resource);
        sensitive.appendHeader("Cookie", "session=private");
        const auto unsafe = Cursor::create(std::move(sensitive), "https", "example.com", &resource);
        RUVIA_CHECK(!unsafe && unsafe.error() == Cursor::Error::kRequestEncoding);
    }
    RUVIA_CHECK_EQ(resource.allocations, resource.returns);
}

RUVIA_TEST(http3ClientRequestWriteRejectsLengthAndInvalidFields) {
    ruvia::detail::HttpClientRequestStorage mismatch("POST", "/", nullptr);
    mismatch.appendHeader("content-length", "99").setBody("short");
    auto rejected = Cursor::create(std::move(mismatch), "https", "example.com", nullptr);
    RUVIA_CHECK(!rejected);
    ruvia::detail::HttpClientRequestStorage invalid("GET", "/", nullptr);
    invalid.appendHeader("bad name", "value");
    auto badField = Cursor::create(std::move(invalid), "https", "example.com", nullptr);
    RUVIA_CHECK(!badField);

    ruvia::detail::HttpClientRequestStorage bodyless("GET", "/", nullptr);
    bodyless.appendHeader("Content-Length", "3");
    auto badLength = Cursor::create(std::move(bodyless), "https", "example.com", nullptr);
    RUVIA_CHECK(!badLength && badLength.error() == Cursor::Error::kRequestEncoding);

    ruvia::detail::HttpClientRequestStorage head("HEAD", "/", nullptr);
    head.appendHeader("content-length", "4").setBody("body");
    auto headLength = Cursor::create(std::move(head), "https", "example.com", nullptr);
    RUVIA_CHECK(headLength && sendHeaders(*headLength));
    if (headLength) {
        const auto framing = headLength->next();
        RUVIA_CHECK(framing && headLength->acknowledge(framing->size()));
        const auto payload = headLength->next();
        RUVIA_CHECK(payload && std::string_view(payload->data(), payload->size()) == "body");
        RUVIA_CHECK(headLength->acknowledge(payload->size()));
        RUVIA_CHECK(headLength->acknowledgeFin(true));
    }
    ruvia::detail::HttpClientRequestStorage wrongHead("HEAD", "/", nullptr);
    wrongHead.appendHeader("content-length", "5").setBody("body");
    auto wrongHeadLength = Cursor::create(std::move(wrongHead), "https", "example.com", nullptr);
    RUVIA_CHECK(!wrongHeadLength && wrongHeadLength.error() == Cursor::Error::kRequestEncoding);

    ruvia::detail::HttpClientRequestStorage zero("GET", "/", nullptr);
    zero.appendHeader("content-length", "0");
    auto allowed = Cursor::create(std::move(zero), "https", "example.com", nullptr);
    RUVIA_CHECK(allowed && sendHeaders(*allowed));
    if (allowed) {
        auto noData = allowed->next();
        RUVIA_CHECK(noData && noData->empty() && allowed->finReady());
    }

    ruvia::detail::HttpClientRequestStorage invalidTarget("GET", "relative", nullptr);
    const auto rejectedTarget = Cursor::create(std::move(invalidTarget), "https", "example.com", nullptr);
    RUVIA_CHECK(!rejectedTarget && rejectedTarget.error() == Cursor::Error::kRequestEncoding);

    ruvia::detail::HttpClientRequestStorage invalidConnect("CONNECT", "example.com", nullptr);
    const auto rejectedConnect = Cursor::create(std::move(invalidConnect), "https", "example.com", nullptr);
    RUVIA_CHECK(!rejectedConnect && rejectedConnect.error() == Cursor::Error::kRequestEncoding);

    ruvia::detail::HttpClientRequestStorage tunnel("CONNECT", "example.com:443", nullptr);
    auto unsupported = Cursor::create(std::move(tunnel), "https", "example.com:443", nullptr);
    RUVIA_CHECK(!unsupported && unsupported.error() == Cursor::Error::kUnsupportedTunnel);
}

RUVIA_TEST(http3ClientRequestWriteFailureDoesNotCommitFinAndReleasesPool) {
    CountingResource pool;
    {
        auto cursor = create("body", &pool);
        RUVIA_CHECK(sendHeaders(cursor));
        auto frame = cursor.next();
        RUVIA_CHECK(frame && cursor.acknowledge(frame->size()));
        auto body = cursor.next();
        RUVIA_CHECK(body && cursor.acknowledge(body->size()));
        RUVIA_CHECK(cursor.finReady());
        RUVIA_CHECK(cursor.acknowledgeFin(false));
        RUVIA_CHECK(cursor.failed() && !cursor.finished());
        for (int i = 0; i < 6; ++i) {
            auto repeated = create("reused", &pool);
            RUVIA_CHECK(pool.allocations > pool.returns);
            RUVIA_CHECK(repeated.next());
        }
    }
    RUVIA_CHECK(pool.allocations > 0);
    RUVIA_CHECK_EQ(pool.allocations, pool.returns);
}

RUVIA_TEST(http3ClientRequestWriteAllocationFailureIsReported) {
    CountingResource pool;
    pool.reject = true;
    ruvia::detail::HttpClientRequestStorage request("POST", "/failure", nullptr);
    request.setBody("body");
    auto failed = Cursor::create(std::move(request), "https", "example.com", &pool);
    RUVIA_CHECK(!failed && failed.error() == Cursor::Error::kOutOfMemory);
}

RUVIA_TEST(http3ClientRequestWriteCreateFailureLeavesSameResourceRequestRetryable) {
    const std::string body(2048, 'r');
    std::string expectedHead;
    {
        ruvia::detail::HttpClientRequestStorage reference("POST", "/transaction", nullptr);
        reference.appendHeader("x-transaction", "kept").setBody(body);
        auto encoded = Cursor::create(std::move(reference), "https", "example.com", nullptr);
        RUVIA_CHECK(encoded.has_value());
        if (encoded) {
            const auto head = encoded->next();
            RUVIA_CHECK(head.has_value());
            expectedHead.assign(head->data(), head->size());
        }
    }

    bool reachedSuccessfulOffset = false;
    for (std::size_t failOffset = 1; failOffset < 128 && !reachedSuccessfulOffset; ++failOffset) {
        CountingResource resource;
        {
            ruvia::detail::HttpClientRequestStorage request("POST", "/transaction", &resource);
            request.appendHeader("x-transaction", "kept").setBody(body);
            const auto bodyAddress = request.body().data();
            const auto liveBefore = resource.allocations - resource.returns;
            resource.failOnAttempt = resource.attempts + failOffset;
            auto result = Cursor::create(std::move(request), "https", "example.com", &resource);
            resource.failOnAttempt = 0;
            if (!result) {
                RUVIA_CHECK_EQ(result.error(), Cursor::Error::kOutOfMemory);
                RUVIA_CHECK_EQ(request.method(), "POST");
                RUVIA_CHECK_EQ(request.target(), "/transaction");
                RUVIA_CHECK_EQ(request.body(), body);
                RUVIA_CHECK_EQ(resource.allocations - resource.returns, liveBefore);

                auto retry = Cursor::create(std::move(request), "https", "example.com", &resource);
                RUVIA_CHECK(retry.has_value());
                if (retry) {
                    checkEncodedBody(*retry, ruvia_ctx, expectedHead, body, bodyAddress, true);
                }
            } else {
                reachedSuccessfulOffset = true;
                checkEncodedBody(*result, ruvia_ctx, expectedHead, body, bodyAddress, true);
            }
        }
        RUVIA_CHECK(resource.allocations > 0);
        RUVIA_CHECK_EQ(resource.allocations, resource.returns);
    }
    RUVIA_CHECK(reachedSuccessfulOffset);
}

RUVIA_TEST(http3ClientRequestWriteCreateFailureLeavesCrossResourceRequestRetryable) {
    const std::string body(2048, 'n');
    std::string expectedHead;
    {
        ruvia::detail::HttpClientRequestStorage reference("PUT", "/normalize", nullptr);
        reference.appendHeader("x-normalize", "kept").setBody(body);
        auto encoded = Cursor::create(std::move(reference), "https", "example.com", nullptr);
        RUVIA_CHECK(encoded.has_value());
        if (encoded) {
            const auto head = encoded->next();
            RUVIA_CHECK(head.has_value());
            expectedHead.assign(head->data(), head->size());
        }
    }

    bool reachedSuccessfulOffset = false;
    for (std::size_t failOffset = 1; failOffset < 128 && !reachedSuccessfulOffset; ++failOffset) {
        CountingResource source;
        CountingResource destination;
        {
            ruvia::detail::HttpClientRequestStorage request("PUT", "/normalize", &source);
            request.appendHeader("x-normalize", "kept").setBody(body);
            const auto bodyAddress = request.body().data();
            const auto sourceLiveBefore = source.allocations - source.returns;
            destination.failOnAttempt = destination.attempts + failOffset;
            auto result = Cursor::create(std::move(request), "https", "example.com", &destination);
            destination.failOnAttempt = 0;
            if (!result) {
                RUVIA_CHECK_EQ(result.error(), Cursor::Error::kOutOfMemory);
                RUVIA_CHECK_EQ(request.method(), "PUT");
                RUVIA_CHECK_EQ(request.target(), "/normalize");
                RUVIA_CHECK_EQ(request.body(), body);
                RUVIA_CHECK_EQ(source.allocations - source.returns, sourceLiveBefore);
                RUVIA_CHECK_EQ(destination.allocations, destination.returns);

                auto retry = Cursor::create(std::move(request), "https", "example.com", &destination);
                RUVIA_CHECK(retry.has_value());
                RUVIA_CHECK_EQ(source.largeAllocations, source.largeReturns);
                if (retry) {
                    checkEncodedBody(*retry, ruvia_ctx, expectedHead, body, bodyAddress, false);
                }
            } else {
                reachedSuccessfulOffset = true;
                RUVIA_CHECK_EQ(source.largeAllocations, source.largeReturns);
                checkEncodedBody(*result, ruvia_ctx, expectedHead, body, bodyAddress, false);
            }
        }
        RUVIA_CHECK(source.allocations > 0);
        RUVIA_CHECK_EQ(source.allocations, source.returns);
        RUVIA_CHECK(destination.allocations > 0);
        RUVIA_CHECK_EQ(destination.allocations, destination.returns);
    }
    RUVIA_CHECK(reachedSuccessfulOffset);
}

RUVIA_TEST(http3ClientRequestWriteInvalidEncodingPreservesRequestForRetry) {
    CountingResource resource;
    const std::string body(256, 'i');
    {
        ruvia::detail::HttpClientRequestStorage request("POST", "/retry-invalid", &resource);
        request.appendHeader("x-preserved", "yes").setBody(body);
        const auto bodyAddress = request.body().data();
        auto rejected = Cursor::create(std::move(request), "bad^scheme", "example.com", &resource);
        RUVIA_CHECK(!rejected && rejected.error() == Cursor::Error::kRequestEncoding);
        RUVIA_CHECK_EQ(request.method(), "POST");
        RUVIA_CHECK_EQ(request.target(), "/retry-invalid");
        RUVIA_CHECK_EQ(request.body(), body);

        auto retry = Cursor::create(std::move(request), "https", "example.com", &resource);
        RUVIA_CHECK(retry.has_value());
        if (retry && sendHeaders(*retry)) {
            auto frame = retry->next();
            RUVIA_CHECK(frame.has_value());
            if (frame && retry->acknowledge(frame->size())) {
                const auto payload = retry->next();
                RUVIA_CHECK(payload.has_value());
                if (payload) {
                    RUVIA_CHECK(payload->data() == bodyAddress);
                }
            }
        }
    }
    RUVIA_CHECK(resource.allocations > 0);
    RUVIA_CHECK_EQ(resource.allocations, resource.returns);
}

RUVIA_TEST(http3ClientRequestWriteRepeatedTransactionalBuildsReturnStorage) {
    CountingResource resource;
    const std::string body(512, 'b');
    for (int i = 0; i < 64; ++i) {
        {
            ruvia::detail::HttpClientRequestStorage request("POST", "/repeat", &resource);
            request.appendHeader("x-repeat", "value").setBody(body);
            auto cursor = Cursor::create(std::move(request), "https", "example.com", &resource);
            RUVIA_CHECK(cursor.has_value());
            if (cursor) {
                RUVIA_CHECK(cursor->next());
            }
        }
        RUVIA_CHECK_EQ(resource.allocations, resource.returns);
    }
    RUVIA_CHECK(resource.allocations > 0);
}

RUVIA_TEST(http3ClientRequestWriteColdCursorReturnsStorage) {
    CountingResource pool;
    {
        ruvia::detail::HttpClientRequestStorage request("POST", "/cold", &pool);
        request.setBody("body");
        auto cold = Cursor::create(std::move(request), "https", "example.com", &pool);
        RUVIA_CHECK(cold.has_value());
    }
    RUVIA_CHECK_EQ(pool.allocations, pool.returns);
}
