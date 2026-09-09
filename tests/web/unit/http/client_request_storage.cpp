#include <limits>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ruvia/web/detail/client/HttpClientRegistry.h"
#include "ruvia/web/detail/client/HttpClientRequestStorage.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {
using ruvia::detail::HttpClientRequestStorage;
using ruvia::detail::HttpClientRequestStorageAccess;

class FailingResource final : public std::pmr::memory_resource {
public:
    explicit FailingResource(std::size_t failAt)
        : failAt_(failAt) {}
    std::size_t attempts{0};
    std::size_t live{0};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (attempts++ == failAt_) {
            throw std::bad_alloc();
        }
        auto* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++live;
        return result;
    }
    void do_deallocate(void* value, std::size_t bytes, std::size_t alignment) override {
        --live;
        std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::size_t failAt_;
};
}  // namespace

RUVIA_TEST(client_request_storage_same_resource_transfers_without_allocation) {
    ruvia::test::RejectingMemoryResource resource;
    const std::string target(200, 't');
    const std::string body(2048, 'b');
    const std::string value(300, 'v');
    HttpClientRequestStorage request("POST", target, &resource);
    request.appendHeader("X-Test", value).setBody(body);
    const auto* targetData = request.target().data();
    const auto* bodyData = request.body().data();
    const auto allocations = resource.allocationCount();
    resource.rejectAllocations();
    auto transferred = std::move(request).intoResource(&resource);
    RUVIA_CHECK(resource.allocationCount() == allocations);
    RUVIA_CHECK(transferred.target().data() == targetData);
    RUVIA_CHECK(transferred.body().data() == bodyData);
    std::pmr::vector<ruvia::HttpHeaderView> headers;
    const auto view = HttpClientRequestStorageAccess::view(transferred, headers);
    RUVIA_CHECK(view.method.view() == "POST");
    RUVIA_CHECK(view.headers.size() == 1);
    RUVIA_CHECK(view.headers[0].name() == "x-test");
    RUVIA_CHECK(view.headers[0].value() == value);
    RUVIA_CHECK(view.content.borrowedBytes()->value() == body);
}

RUVIA_TEST(client_request_storage_transfer_outlives_source_resource) {
    ruvia::test::CountingMemoryResource destination;
    const std::string method(100, 'M');
    const std::string target(200, 't');
    const std::string name(80, 'H');
    const std::string value(300, 'v');
    const std::string body(2048, 'b');
    {
        std::optional<HttpClientRequestStorage> transferred;
        {
            ruvia::test::CountingMemoryResource source;
            {
                HttpClientRequestStorage request(method, target, &source);
                request.appendHeader(name, value).setBody(body);
                transferred.emplace(std::move(request).intoResource(&destination));
            }
            RUVIA_CHECK(source.liveAllocations() == 0);
        }
        std::pmr::vector<ruvia::HttpHeaderView> headers;
        const auto view = HttpClientRequestStorageAccess::view(*transferred, headers);
        RUVIA_CHECK(view.method.view() == method);
        RUVIA_CHECK(view.target.view() == target);
        RUVIA_CHECK(view.headers.size() == 1);
        RUVIA_CHECK(view.headers[0].name() == std::string(80, 'h'));
        RUVIA_CHECK(view.headers[0].value() == value);
        RUVIA_CHECK(view.content.borrowedBytes()->value() == body);
        RUVIA_CHECK(destination.liveAllocations() > 0);
    }
    RUVIA_CHECK(destination.liveAllocations() == 0);
}

RUVIA_TEST(client_request_storage_failed_transfer_releases_partial_copy) {
    ruvia::test::CountingMemoryResource source;
    {
        const std::string text(256, 'x');
        HttpClientRequestStorage request(text, text, &source);
        request.appendHeader("X-Test", text).setBody(text);
        FailingResource baseline((std::numeric_limits<std::size_t>::max)());
        {
            auto copied = std::move(request).intoResource(&baseline);
            RUVIA_CHECK(copied.body() == text);
        }
        RUVIA_CHECK(baseline.live == 0);
        for (std::size_t failAt = 0; failAt < baseline.attempts; ++failAt) {
            FailingResource destination(failAt);
            bool threw = false;
            try {
                auto copied = std::move(request).intoResource(&destination);
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            RUVIA_CHECK(threw);
            RUVIA_CHECK(destination.live == 0);
            std::pmr::vector<ruvia::HttpHeaderView> headers;
            const auto view = HttpClientRequestStorageAccess::view(request, headers);
            RUVIA_CHECK(view.method.view() == text);
            RUVIA_CHECK(view.target.view() == text);
            RUVIA_CHECK(view.headers[0].value() == text);
            RUVIA_CHECK(view.content.borrowedBytes()->value() == text);
        }
    }
    RUVIA_CHECK(source.liveAllocations() == 0);
}

RUVIA_TEST(client_request_storage_transfer_preserves_empty_content_kind) {
    ruvia::test::CountingMemoryResource source;
    ruvia::test::CountingMemoryResource destination;
    for (bool sameResource : {false, true}) {
        for (bool explicitBody : {false, true}) {
            HttpClientRequestStorage request("POST", "/", &source);
            if (explicitBody) {
                request.setBody("");
            }
            auto transferred = std::move(request).intoResource(
                sameResource ? &source : &destination);
            std::pmr::vector<ruvia::HttpHeaderView> headers;
            const auto view = HttpClientRequestStorageAccess::view(transferred, headers);
            RUVIA_CHECK((view.content.borrowedBytes() != nullptr) == explicitBody);
            RUVIA_CHECK((view.content.withoutContent() != nullptr) != explicitBody);
        }
    }
}
