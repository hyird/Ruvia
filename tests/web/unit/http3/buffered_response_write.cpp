#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/Http3ClientResponse.h"
#include "ruvia/web/detail/http3/Http3BufferedResponseWrite.h"

#include "test_harness.h"

namespace {

class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t allocations{};
    std::size_t returns{};
    bool reject{};

private:
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        if (reject) {
            throw std::bad_alloc();
        }
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(size, alignment);
    }
    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        ++returns;
        std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

using Cursor = ruvia::detail::Http3BufferedResponseWrite;

Cursor makeCursor(const ruvia::HttpResponse& response,
    const ruvia::HttpBufferedResponseWritePlan& plan, std::pmr::memory_resource* resource) {
    auto result = Cursor::create(response, plan, resource);
    if (!result) {
        throw std::runtime_error("failed to create HTTP/3 response cursor");
    }
    return std::move(*result);
}

void acknowledgeAll(Cursor& cursor) {
    for (;;) {
        auto next = cursor.next();
        if (!next) {
            throw std::runtime_error("failed to retrieve HTTP/3 output segment");
        }
        if (next->empty()) {
            break;
        }
        if (!cursor.acknowledge(next->size())) {
            throw std::runtime_error("failed to acknowledge HTTP/3 output segment");
        }
    }
}

}  // namespace

RUVIA_TEST(http3BufferedResponseWriteOrdersHeadersDataAndExplicitFin) {
    ruvia::HttpResponse response;
    response.body("payload");
    const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
    Cursor cursor = makeCursor(response, plan, nullptr);

    RUVIA_CHECK(cursor.nextStep() == Cursor::NextStep::kBytes);
    auto headers = cursor.next();
    RUVIA_CHECK(headers && !headers->empty());
    RUVIA_CHECK(static_cast<unsigned char>((*headers)[0]) == 0x01);
    RUVIA_CHECK(cursor.acknowledge(headers->size()));
    RUVIA_CHECK(cursor.nextStep() == Cursor::NextStep::kBytes);
    RUVIA_CHECK(!cursor.finReady());

    auto frame = cursor.next();
    RUVIA_CHECK(frame && !frame->empty());
    RUVIA_CHECK(static_cast<unsigned char>((*frame)[0]) == 0x00);
    RUVIA_CHECK(cursor.acknowledge(frame->size()));
    RUVIA_CHECK(cursor.nextStep() == Cursor::NextStep::kBytes);
    auto body = cursor.next();
    RUVIA_CHECK(body && std::string_view(body->data(), body->size()) == "payload");
    if (!body) {
        return;
    }
    const auto partialBodySize = body->size() / 2;
    RUVIA_CHECK(cursor.acknowledge(partialBodySize));
    RUVIA_CHECK(cursor.nextStep() == Cursor::NextStep::kBytes);
    const auto bodyRemainder = cursor.next();
    RUVIA_CHECK(bodyRemainder &&
                bodyRemainder->data() == body->data() + partialBodySize);
    if (!bodyRemainder) {
        return;
    }
    RUVIA_CHECK(cursor.acknowledge(bodyRemainder->size()));
    RUVIA_CHECK(cursor.nextStep() == Cursor::NextStep::kFin);
    RUVIA_CHECK(cursor.finReady());
    RUVIA_CHECK(!cursor.finished());
    RUVIA_CHECK(cursor.acknowledgeFin(true));
    RUVIA_CHECK(cursor.finished());
    RUVIA_CHECK(cursor.nextStep() == Cursor::NextStep::kComplete);
}

RUVIA_TEST(http3BufferedResponseWritePreservesEncodedDecodedFieldSectionSizeAcrossMoveAndFin) {
    ruvia::HttpResponse response;
    response.body("payload");
    response.header("X-Projection", "retained");
    const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
    const auto encoded = ruvia::encodeHttp3ResponseHead(response, plan);
    RUVIA_CHECK(encoded.has_value());
    if (!encoded) {
        return;
    }
    const auto decodedSize = encoded->decodedFieldSectionSize();
    RUVIA_CHECK(decodedSize > 42U);

    Cursor cursor = makeCursor(response, plan, nullptr);
    RUVIA_CHECK_EQ(cursor.decodedFieldSectionSize(), decodedSize);
    const auto headers = cursor.next();
    RUVIA_CHECK(headers && headers->size() > 1);
    if (!headers || headers->size() <= 1) {
        return;
    }
    const auto partialSize = headers->size() / 2;
    RUVIA_CHECK(cursor.acknowledge(partialSize));
    RUVIA_CHECK_EQ(cursor.decodedFieldSectionSize(), decodedSize);
    const auto remainder = cursor.next();
    RUVIA_CHECK(remainder && remainder->size() == headers->size() - partialSize);
    if (!remainder) {
        return;
    }
    RUVIA_CHECK(cursor.acknowledge(remainder->size()));
    RUVIA_CHECK_EQ(cursor.decodedFieldSectionSize(), decodedSize);

    Cursor moved(std::move(cursor));
    RUVIA_CHECK_EQ(moved.decodedFieldSectionSize(), decodedSize);
    RUVIA_CHECK_EQ(cursor.decodedFieldSectionSize(), 0U);
    RUVIA_CHECK(cursor.failed());
    const auto frame = moved.next();
    RUVIA_CHECK(frame && !frame->empty());
    if (!frame || frame->empty()) {
        return;
    }
    RUVIA_CHECK(moved.acknowledge(frame->size()));
    RUVIA_CHECK_EQ(moved.decodedFieldSectionSize(), decodedSize);
    const auto body = moved.next();
    RUVIA_CHECK(body && std::string_view(body->data(), body->size()) == "payload");
    if (!body) {
        return;
    }
    RUVIA_CHECK(moved.acknowledge(body->size()));
    RUVIA_CHECK(moved.finReady());
    RUVIA_CHECK_EQ(moved.decodedFieldSectionSize(), decodedSize);
    RUVIA_CHECK(moved.acknowledgeFin(true));
    RUVIA_CHECK(moved.finished());
    RUVIA_CHECK_EQ(moved.decodedFieldSectionSize(), decodedSize);
}

RUVIA_TEST(http3BufferedResponseWritePartialAndWantKeepTheOfferedAddressStable) {
    ruvia::HttpResponse response;
    response.body("abcd");
    const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
    auto cursor = makeCursor(response, plan, nullptr);
    RUVIA_CHECK(!cursor.acknowledge(1));
    auto offered = cursor.next();
    RUVIA_CHECK(offered && offered->size() > 1);
    const auto original = std::string(offered->data(), offered->size());
    const auto* address = offered->data();
    const char first = offered->front();
    const auto demand = cursor.nextStep();
    RUVIA_CHECK(demand == Cursor::NextStep::kBytes);
    RUVIA_CHECK(cursor.acknowledge(0));
    auto retry = cursor.next();
    RUVIA_CHECK(retry && retry->data() == address && retry->front() == first &&
                retry->size() == offered->size());
    RUVIA_CHECK(std::string_view(retry->data(), retry->size()) == original);
    RUVIA_CHECK(!cursor.acknowledge(retry->size() + 1));
    RUVIA_CHECK(cursor.acknowledge(1));
    auto remainder = cursor.next();
    RUVIA_CHECK(remainder && remainder->data() == address + 1);
}

RUVIA_TEST(http3BufferedResponseWriteCannotMoveAnOutstandingWriteBuffer) {
    ruvia::HttpResponse response;
    response.body("payload");
    const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
    auto cursor = makeCursor(response, plan, nullptr);
    const auto segment = cursor.next();
    RUVIA_CHECK(segment && !segment->empty());
    if (!segment || segment->empty()) {
        return;
    }
    bool rejected = false;
    try {
        Cursor moved(std::move(cursor));
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    const auto retry = cursor.next();
    RUVIA_CHECK(retry && retry->data() == segment->data() && retry->size() == segment->size());
    RUVIA_CHECK(cursor.acknowledge(segment->size()));
    // Moving after all previously offered bytes were confirmed is safe.
    Cursor moved(std::move(cursor));
    acknowledgeAll(moved);
    RUVIA_CHECK(moved.acknowledgeFin(true));
}

RUVIA_TEST(http3BufferedResponseWriteHeadRetainsRepresentationLengthWithoutData) {
    ruvia::HttpResponse response;
    response.body("head representation");
    const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kHead, response);
    Cursor cursor = makeCursor(response, plan, nullptr);
    acknowledgeAll(cursor);
    RUVIA_CHECK(cursor.finReady());
    RUVIA_CHECK(cursor.nextStep() == Cursor::NextStep::kFin);
    RUVIA_CHECK(cursor.acknowledgeFin(true));
    RUVIA_CHECK(cursor.finished());
    RUVIA_CHECK(cursor.nextStep() == Cursor::NextStep::kComplete);
    RUVIA_CHECK_EQ(plan.contentLength(), std::uint64_t{19});
}

RUVIA_TEST(http3BufferedResponseWriteDemandIsPureAndFindsHeadersOnlyFin) {
    CountingResource memory;
    for (const auto method : {ruvia::HttpKnownMethod::kHead, ruvia::HttpKnownMethod::kGet}) {
        ruvia::HttpResponse response;
        if (method == ruvia::HttpKnownMethod::kHead) {
            response.body("representation");
        }
        const auto plan = ruvia::planBufferedHttpResponseWrite(method, response);
        auto cursor = makeCursor(response, plan, &memory);
        RUVIA_CHECK(cursor.nextStep() == Cursor::NextStep::kBytes);

        auto headers = cursor.next();
        RUVIA_CHECK(headers && !headers->empty());
        if (!headers || headers->empty()) {
            return;
        }
        const auto original = std::string(headers->data(), headers->size());
        const auto* address = headers->data();
        const auto allocations = memory.allocations;
        const auto returns = memory.returns;
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            RUVIA_CHECK(cursor.nextStep() == Cursor::NextStep::kBytes);
            RUVIA_CHECK_EQ(memory.allocations, allocations);
            RUVIA_CHECK_EQ(memory.returns, returns);
        }
        const auto retry = cursor.next();
        RUVIA_CHECK(retry && retry->data() == address &&
                    std::string_view(retry->data(), retry->size()) == original);
        RUVIA_CHECK(cursor.acknowledge(headers->size()));
        RUVIA_CHECK(cursor.nextStep() == Cursor::NextStep::kFin);
        RUVIA_CHECK_EQ(memory.allocations, allocations);
        RUVIA_CHECK_EQ(memory.returns, returns);

        const auto fin = cursor.next();
        RUVIA_CHECK(fin && fin->empty() && cursor.finReady());
        RUVIA_CHECK(cursor.acknowledgeFin(true));
        RUVIA_CHECK(cursor.nextStep() == Cursor::NextStep::kComplete);
    }
    RUVIA_CHECK_EQ(memory.allocations, memory.returns);
}

RUVIA_TEST(http3BufferedResponseWriteFileWithoutPayloadSendsOnlyMetadata) {
    CountingResource memory;
    for (unsigned scenario = 0; scenario < 2; ++scenario) {
        const auto method = scenario == 0 ? ruvia::HttpKnownMethod::kHead : ruvia::HttpKnownMethod::kGet;
        const std::uint64_t length = scenario == 0 ? 5 : 0;
        ruvia::HttpResponse response;
        response.fileBody("unopened-response.bin", length, 0, length, {}, true);
        response.header("X-Projection", "retained");
        const auto plan = ruvia::planBufferedHttpResponseWrite(method, response);
        RUVIA_CHECK(!plan.sendBody() || plan.contentLength() == 0);
        const auto encoded = ruvia::encodeHttp3ResponseHead(response, plan, {}, &memory);
        RUVIA_CHECK(encoded.has_value());
        if (!encoded) {
            return;
        }
        const auto decodedSize = encoded->decodedFieldSectionSize();
        RUVIA_CHECK(decodedSize > 42U);
        auto cursor = makeCursor(response, plan, &memory);
        RUVIA_CHECK_EQ(cursor.decodedFieldSectionSize(), decodedSize);
        ruvia::Http3ClientResponse decoder(0, method, &memory);
        struct Received {
            std::optional<std::uint64_t> length;
            unsigned heads{};
            unsigned bodies{};
            unsigned ends{};
        } received;
        const auto callback = [](void* opaque, const ruvia::Http3ClientResponseEvent& event) {
            auto& state = *static_cast<Received*>(opaque);
            if (event.kind == ruvia::Http3ClientResponseEventKind::kFinalHead) {
                ++state.heads;
                state.length = event.head->contentLength;
            } else if (event.kind == ruvia::Http3ClientResponseEventKind::kBody) {
                ++state.bodies;
            } else if (event.kind == ruvia::Http3ClientResponseEventKind::kMessageEnd) {
                ++state.ends;
            }
        };
        const auto headers = cursor.next();
        RUVIA_CHECK(headers && !headers->empty());
        if (!headers || headers->empty()) {
            return;
        }
        RUVIA_CHECK(decoder.feed(*headers, false, false, callback, &received).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(cursor.acknowledge(headers->size()));
        RUVIA_CHECK_EQ(cursor.decodedFieldSectionSize(), decodedSize);
        const auto following = cursor.next();
        RUVIA_CHECK(following && following->empty() && cursor.finReady());
        RUVIA_CHECK_EQ(cursor.decodedFieldSectionSize(), decodedSize);
        RUVIA_CHECK(decoder.feed({}, true, false, callback, &received).status == ruvia::Http3ClientResponseStatus::kMessageEnd);
        RUVIA_CHECK(cursor.acknowledgeFin(true));
        RUVIA_CHECK_EQ(cursor.decodedFieldSectionSize(), decodedSize);
        RUVIA_CHECK(received.heads == 1 && received.bodies == 0 && received.ends == 1);
        RUVIA_CHECK(received.length == length);
    }
    RUVIA_CHECK_EQ(memory.allocations, memory.returns);
}

RUVIA_TEST(http3BufferedResponseWriteEmptyAndNoContentResponsesOmitData) {
    for (const auto status : {ruvia::http_status::kOk, ruvia::http_status::kNoContent,
             ruvia::http_status::kResetContent, ruvia::http_status::kNotModified}) {
        ruvia::HttpResponse response;
        response.status(status);
        const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
        auto cursor = makeCursor(response, plan, nullptr);
        acknowledgeAll(cursor);
        RUVIA_CHECK(cursor.finReady());
        RUVIA_CHECK(cursor.acknowledgeFin(true));
    }
}

RUVIA_TEST(http3BufferedResponseWriteRejectsFilesAndEncodingFailures) {
    ruvia::HttpResponse fileResponse;
    fileResponse.fileBody("response.bin", 5, 0, 5, {}, true);
    const auto filePlan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, fileResponse);
    auto file = Cursor::create(fileResponse, filePlan, nullptr);
    RUVIA_CHECK(!file && file.error() == Cursor::Error::kFileBodyUnsupported);

    ruvia::HttpResponse invalid;
    invalid.header("connection", "close");
    const auto invalidPlan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, invalid);
    auto encoded = Cursor::create(invalid, invalidPlan, nullptr);
    RUVIA_CHECK(!encoded && encoded.error() == Cursor::Error::kResponseEncoding);
}

RUVIA_TEST(http3BufferedResponseWriteReturnsPoolStorageOnlyAfterCursorRetirement) {
    CountingResource memory;
    ruvia::HttpResponse response;
    response.body("retained body");
    const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
    {
        auto held = makeCursor(response, plan, &memory);
        for (int i = 0; i < 8; ++i) {
            auto cursor = makeCursor(response, plan, &memory);
            RUVIA_CHECK(memory.allocations > memory.returns);
            RUVIA_CHECK(response.bodyBytes() == "retained body");
            acknowledgeAll(cursor);
            RUVIA_CHECK(cursor.acknowledgeFin(true));
        }
        RUVIA_CHECK(memory.allocations > memory.returns);
        RUVIA_CHECK(response.bodyBytes() == "retained body");
        acknowledgeAll(held);
        RUVIA_CHECK(held.acknowledgeFin(true));
    }
    RUVIA_CHECK_EQ(memory.allocations, memory.returns);
}

RUVIA_TEST(http3BufferedResponseWriteTransportFailureDoesNotCommitFin) {
    ruvia::HttpResponse response;
    response.body("failure");
    const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
    auto cursor = makeCursor(response, plan, nullptr);
    acknowledgeAll(cursor);
    RUVIA_CHECK(cursor.finReady());
    RUVIA_CHECK(cursor.nextStep() == Cursor::NextStep::kFin);
    RUVIA_CHECK(cursor.acknowledgeFin(false));
    RUVIA_CHECK(cursor.failed());
    RUVIA_CHECK(cursor.nextStep() == Cursor::NextStep::kFailed);
    RUVIA_CHECK(!cursor.finished());
    RUVIA_CHECK(!cursor.next());
}

RUVIA_TEST(http3BufferedResponseWriteHandlesOutOfMemoryAndNotStartedCleanup) {
    ruvia::HttpResponse response;
    response.body("body");
    const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
    CountingResource memory;
    memory.reject = true;
    auto failed = Cursor::create(response, plan, &memory);
    RUVIA_CHECK(!failed && failed.error() == Cursor::Error::kOutOfMemory);
    memory.reject = false;
    {
        auto cold = Cursor::create(response, plan, &memory);
        RUVIA_CHECK(cold.has_value());
    }
    RUVIA_CHECK_EQ(memory.allocations, memory.returns);
}
