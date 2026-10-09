#include <limits>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "client/http_client_request_storage.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {
using ruvia::detail::http_client_request_storage;
using ruvia::detail::http_client_request_storage_access;

class failing_resource final : public std::pmr::memory_resource {
public:
    explicit failing_resource(std::size_t fail_at)
        : fail_at_(fail_at) {}
    std::size_t attempts_{0};
    std::size_t live_{0};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        // Debug iterator bookkeeping may allocate inside noexcept container
        // constructors; inject failures into owned request data only.
        if (bytes_value >= 32 && attempts_++ == fail_at_) {
            throw std::bad_alloc();
        }
        auto* result_value = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++live_;
        return result_value;
    }
    void do_deallocate(void* value, std::size_t bytes_value, std::size_t alignment) override {
        --live_;
        std::pmr::new_delete_resource()->deallocate(value, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::size_t fail_at_;
};
}  // namespace

RUVIA_TEST(client_request_storage_same_resource_preserves_owned_storage) {
    ruvia::test::counting_memory_resource resource;
    const std::string target(200, 't');
    const std::string body(2048, 'b');
    const std::string value(300, 'v');
    http_client_request_storage request("POST", target, &resource);
    request.append_header("X-Test", value).set_body(body);
    request.set_replay_safe(true);
    const auto* target_data = request.target().data();
    const auto* body_data = request.body().data();
    std::pmr::vector<ruvia::http_header_view> original_headers;
    const auto original = http_client_request_storage_access::view(request, original_headers);
    const auto* name_data = original.headers_.front().name().data();
    const auto* value_data = original.headers_.front().value().data();
    auto transferred = std::move(request).into_resource(&resource);
    // Moving STL containers may allocate debug iterator metadata. The request's
    // target, body and header storage must still transfer without being copied.
    RUVIA_CHECK(transferred.target().data() == target_data);
    RUVIA_CHECK(transferred.body().data() == body_data);
    std::pmr::vector<ruvia::http_header_view> headers;
    const auto view = http_client_request_storage_access::view(transferred, headers);
    RUVIA_CHECK(view.method_.view() == "POST");
    RUVIA_CHECK(view.replay_safe_);
    RUVIA_CHECK(view.headers_.size() == 1);
    RUVIA_CHECK(view.headers_[0].name() == "x-test");
    RUVIA_CHECK(view.headers_[0].value() == value);
    RUVIA_CHECK(view.headers_[0].name().data() == name_data);
    RUVIA_CHECK(view.headers_[0].value().data() == value_data);
    RUVIA_CHECK(view.content_.borrowed_bytes()->value() == body);
}

RUVIA_TEST(client_request_storage_transfer_outlives_source_resource) {
    ruvia::test::counting_memory_resource destination;
    const std::string method(100, 'M');
    const std::string target(200, 't');
    const std::string name(80, 'H');
    const std::string value(300, 'v');
    const std::string body(2048, 'b');
    {
        std::optional<http_client_request_storage> transferred;
        {
            ruvia::test::counting_memory_resource source;
            {
                http_client_request_storage request(method, target, &source);
                request.append_header(name, value).set_body(body);
                request.set_replay_safe(true);
                transferred.emplace(std::move(request).into_resource(&destination));
            }
            RUVIA_CHECK(source.live_allocations() == 0);
        }
        std::pmr::vector<ruvia::http_header_view> headers;
        const auto view = http_client_request_storage_access::view(*transferred, headers);
        RUVIA_CHECK(view.method_.view() == method);
        RUVIA_CHECK(view.replay_safe_);
        RUVIA_CHECK(view.target_.view() == target);
        RUVIA_CHECK(view.headers_.size() == 1);
        RUVIA_CHECK(view.headers_[0].name() == std::string(80, 'h'));
        RUVIA_CHECK(view.headers_[0].value() == value);
        RUVIA_CHECK(view.content_.borrowed_bytes()->value() == body);
        RUVIA_CHECK(destination.live_allocations() > 0);
    }
    RUVIA_CHECK(destination.live_allocations() == 0);
}

RUVIA_TEST(client_request_storage_failed_transfer_releases_partial_copy) {
    ruvia::test::counting_memory_resource source;
    {
        const std::string text(256, 'x');
        http_client_request_storage request(text, text, &source);
        request.append_header("X-Test", text).set_body(text);
        failing_resource baseline((std::numeric_limits<std::size_t>::max)());
        {
            auto copied = std::move(request).into_resource(&baseline);
            RUVIA_CHECK(copied.body() == text);
        }
        RUVIA_CHECK(baseline.live_ == 0);
        for (std::size_t fail_at = 0; fail_at < baseline.attempts_; ++fail_at) {
            failing_resource destination(fail_at);
            bool threw = false;
            try {
                auto copied = std::move(request).into_resource(&destination);
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            RUVIA_CHECK(threw);
            RUVIA_CHECK(destination.live_ == 0);
            std::pmr::vector<ruvia::http_header_view> headers;
            const auto view = http_client_request_storage_access::view(request, headers);
            RUVIA_CHECK(view.method_.view() == text);
            RUVIA_CHECK(view.target_.view() == text);
            RUVIA_CHECK(view.headers_[0].value() == text);
            RUVIA_CHECK(view.content_.borrowed_bytes()->value() == text);
        }
    }
    RUVIA_CHECK(source.live_allocations() == 0);
}

RUVIA_TEST(client_request_storage_transfer_preserves_empty_content_kind) {
    ruvia::test::counting_memory_resource source;
    ruvia::test::counting_memory_resource destination;
    for (bool same_resource : {false, true}) {
        for (bool explicit_body : {false, true}) {
            http_client_request_storage request("POST", "/", &source);
            if (explicit_body) {
                request.set_body("");
            }
            auto transferred = std::move(request).into_resource(
                same_resource ? &source : &destination);
            std::pmr::vector<ruvia::http_header_view> headers;
            const auto view = http_client_request_storage_access::view(transferred, headers);
            RUVIA_CHECK((view.content_.borrowed_bytes() != nullptr) == explicit_body);
            RUVIA_CHECK((view.content_.without_content() != nullptr) != explicit_body);
        }
    }
}
