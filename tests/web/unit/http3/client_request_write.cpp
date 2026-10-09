#include <cstddef>
#include <memory_resource>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "http3/http3_client_request_write.h"
#include "test_harness.h"

namespace {
class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_{};
    std::size_t returns_{};
    std::size_t attempts_{};
    std::size_t fail_on_attempt_{};
    std::size_t large_allocations_{};
    std::size_t large_returns_{};
    bool reject_{};

private:
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        if (size >= 32) {
            ++attempts_;
            if (reject_ || attempts_ == fail_on_attempt_) {
                throw std::bad_alloc();
            }
        }
        ++allocations_;
        if (size >= 1024) {
            ++large_allocations_;
        }
        return std::pmr::new_delete_resource()->allocate(size, alignment);
    }
    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        ++returns_;
        if (size >= 1024) {
            ++large_returns_;
        }
        std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
using cursor = ruvia::detail::http3_client_request_write;

cursor create(std::string_view body, std::pmr::memory_resource* resource, std::string_view method = "POST") {
    ruvia::detail::http_client_request_storage request(method, "/upload?q=1", resource);
    request.set_body(body);
    auto result_value = cursor::create(std::move(request), "https", "example.com", resource);
    if ((result_value.index() != 0)) {
        throw std::runtime_error("HTTP/3 request cursor creation failed");
    }
    return std::move(std::get<0>(result_value));
}

bool send_headers(cursor& cursor_value) {
    auto output = cursor_value.next();
    return output.index() == 0 && !std::get<0>(output).empty() && cursor_value.acknowledge(std::get<0>(output).size()).index() == 0;
}

void check_encoded_body(cursor& cursor_value, ruvia::testing::test_context& ruvia_ctx,
    std::string_view expected_head, std::string_view body, const char* expected_body_address,
    bool same_address) {
    auto head = cursor_value.next();
    RUVIA_CHECK((head.index() == 0));
    if ((head.index() != 0)) {
        return;
    }
    RUVIA_CHECK_EQ(std::string_view(std::get<0>(head).data(), std::get<0>(head).size()), expected_head);
    const auto head_ack = cursor_value.acknowledge(std::get<0>(head).size());
    RUVIA_CHECK((head_ack.index() == 0));
    if ((head_ack.index() != 0)) {
        return;
    }

    auto frame = cursor_value.next();
    RUVIA_CHECK((frame.index() == 0));
    if ((frame.index() != 0)) {
        return;
    }
    const auto frame_ack = cursor_value.acknowledge(std::get<0>(frame).size());
    RUVIA_CHECK((frame_ack.index() == 0));
    if ((frame_ack.index() != 0)) {
        return;
    }

    auto payload_value = cursor_value.next();
    RUVIA_CHECK((payload_value.index() == 0));
    if ((payload_value.index() != 0)) {
        return;
    }
    RUVIA_CHECK_EQ(std::get<0>(payload_value).size(), body.size());
    RUVIA_CHECK_EQ(std::string_view(std::get<0>(payload_value).data(), std::get<0>(payload_value).size()), body);
    RUVIA_CHECK((std::get<0>(payload_value).data() == expected_body_address) == same_address);
}
}  // namespace

RUVIA_TEST(http3_client_request_write_retired_handoff_preserves_whole_request_and_returns_storage) {
    counting_resource memory;
    const std::string body(2048, 'p');
    std::optional<ruvia::detail::http_client_request_storage> retained;
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        const auto phase = scenario % 3;
        const std::string_view method = scenario < 3 ? "POST" : "HEAD";
        {
            auto cursor_value = create(body, &memory, method);
            const auto head = cursor_value.next();
            RUVIA_CHECK((head.index() == 0) && !std::get<0>(head).empty());
            const std::string encoded_head(std::get<0>(head).data(), std::get<0>(head).size());
            if (phase != 0) {
                RUVIA_CHECK((cursor_value.acknowledge(std::get<0>(head).size()).index() == 0));
                const auto framing = cursor_value.next();
                RUVIA_CHECK((framing.index() == 0) && (cursor_value.acknowledge(std::get<0>(framing).size()).index() == 0));
                const auto payload_value = cursor_value.next();
                RUVIA_CHECK((payload_value.index() == 0) && std::get<0>(payload_value).size() == body.size());
                RUVIA_CHECK((cursor_value.acknowledge(phase == 1 ? 7 : std::get<0>(payload_value).size()).index() == 0));
                if (phase == 1) {
                    RUVIA_CHECK(std::get<0>(cursor_value.next()).size() == body.size() - 7);
                } else {
                    RUVIA_CHECK((cursor_value.acknowledge_fin(true).index() == 0));
                }
            }
            // The caller has now retired transport; a previous WANT span can
            // be invalidated, but all original bytes must remain replayable.
            const auto before_transfer = memory.large_allocations_;
            auto request = cursor_value.take_request_after_retirement();
            RUVIA_CHECK(request && request->method() == method && request->target() == "/upload?q=1");
            RUVIA_CHECK(request && request->body() == body);
            RUVIA_CHECK_EQ(memory.large_allocations_, before_transfer);
            RUVIA_CHECK((cursor_value.next().index() != 0) && !cursor_value.take_request_after_retirement());
            auto replacement = cursor::create(std::move(*request), "https", "example.com", &memory);
            RUVIA_CHECK((replacement.index() == 0));
            const auto replay_head = std::get<0>(replacement).next();
            RUVIA_CHECK((replay_head.index() == 0) && std::string_view(std::get<0>(replay_head).data(), std::get<0>(replay_head).size()) == encoded_head);
            retained.emplace(std::move(*std::get<0>(replacement).take_request_after_retirement()));
        }
        RUVIA_CHECK(retained->body() == body);
        RUVIA_CHECK(memory.allocations_ > memory.returns_);
        retained.reset();
        RUVIA_CHECK_EQ(memory.allocations_, memory.returns_);
    }
}

RUVIA_TEST(http3_client_request_write_handoff_normalizes_into_new_owner_before_old_resource_retires) {
    counting_resource destination;
    const std::string body(2048, 'p');
    const std::string header_value(256, 'h');
    for (const bool fail_allocation : {false, true}) {
        std::optional<cursor> replacement;
        std::string expected_head;
        {
            counting_resource source;
            {
                ruvia::detail::http_client_request_storage request("POST", "/handoff", &source);
                request.append_header("x-retained", header_value).set_body(body);
                auto original = cursor::create(std::move(request), "https", "example.com", &source);
                RUVIA_CHECK((original.index() == 0));
                const auto head = std::get<0>(original).next();
                expected_head.assign(std::get<0>(head).data(), std::get<0>(head).size());
                auto transferred = std::get<0>(original).take_request_after_retirement();
                destination.reject_ = fail_allocation;
                auto rebuilt = cursor::create(std::move(*transferred), "https", "example.com", &destination);
                destination.reject_ = false;
                if (fail_allocation) {
                    RUVIA_CHECK((rebuilt.index() != 0) && std::get<1>(rebuilt) == cursor::error_type::out_of_memory);
                } else {
                    RUVIA_CHECK((rebuilt.index() == 0));
                    replacement.emplace(std::move(std::get<0>(rebuilt)));
                }
            }
            RUVIA_CHECK_EQ(source.allocations_, source.returns_);
        }
        // The source allocator no longer exists: HEADERS and DATA must now
        // refer exclusively to the replacement owner's storage.
        if (replacement) {
            const auto head = replacement->next();
            RUVIA_CHECK((head.index() == 0) && std::string_view(std::get<0>(head).data(), std::get<0>(head).size()) == expected_head);
            RUVIA_CHECK((replacement->acknowledge(std::get<0>(head).size()).index() == 0));
            const auto frame = replacement->next();
            RUVIA_CHECK((frame.index() == 0) && (replacement->acknowledge(std::get<0>(frame).size()).index() == 0));
            const auto payload_value = replacement->next();
            RUVIA_CHECK((payload_value.index() == 0) && std::string_view(std::get<0>(payload_value).data(), std::get<0>(payload_value).size()) == body);
            RUVIA_CHECK((replacement->acknowledge(std::get<0>(payload_value).size()).index() == 0));
            RUVIA_CHECK((replacement->acknowledge_fin(true).index() == 0));
        }
        replacement.reset();
        RUVIA_CHECK_EQ(destination.allocations_, destination.returns_);
    }
}

RUVIA_TEST(http3_client_request_write_owns_body_and_segments_the_request) {
    auto cursor_value = create("payload", nullptr);
    RUVIA_CHECK(send_headers(cursor_value));
    auto frame = cursor_value.next();
    RUVIA_CHECK((frame.index() == 0) && !std::get<0>(frame).empty() && static_cast<unsigned char>((std::get<0>(frame))[0]) == 0);
    if ((frame.index() == 0)) {
        RUVIA_CHECK((cursor_value.acknowledge(std::get<0>(frame).size()).index() == 0));
    }
    auto payload_value = cursor_value.next();
    RUVIA_CHECK((payload_value.index() == 0) && std::string_view(std::get<0>(payload_value).data(), std::get<0>(payload_value).size()) == "payload");
    if ((payload_value.index() == 0)) {
        RUVIA_CHECK((cursor_value.acknowledge(std::get<0>(payload_value).size()).index() == 0));
    }
    RUVIA_CHECK(cursor_value.fin_ready());
    RUVIA_CHECK(!cursor_value.finished());
    RUVIA_CHECK((cursor_value.acknowledge_fin(true).index() == 0));
    RUVIA_CHECK(cursor_value.finished());
}

RUVIA_TEST(http3_client_request_write_partial_acknowledgement_and_want_keep_same_bytes) {
    auto cursor_value = create("body", nullptr);
    auto offered = cursor_value.next();
    RUVIA_CHECK((offered.index() == 0) && std::get<0>(offered).size() > 1);
    if ((offered.index() != 0) || std::get<0>(offered).empty()) {
        return;
    }
    const auto* address = std::get<0>(offered).data();
    const auto length = std::get<0>(offered).size();
    const auto first = std::get<0>(offered).front();
    RUVIA_CHECK((cursor_value.acknowledge(0).index() == 0));
    auto retry = cursor_value.next();
    RUVIA_CHECK((retry.index() == 0) && std::get<0>(retry).data() == address && std::get<0>(retry).size() == length && std::get<0>(retry).front() == first);
    RUVIA_CHECK((cursor_value.acknowledge(1).index() == 0));
    auto rest = cursor_value.next();
    RUVIA_CHECK((rest.index() == 0) && std::get<0>(rest).data() == address + 1 && std::get<0>(rest).size() == length - 1);
}

RUVIA_TEST(http3_client_request_write_parallel_cursors_and_no_zero_length_data) {
    auto first = create("one", nullptr);
    auto second = create("two", nullptr);
    auto a = first.next();
    auto b = second.next();
    RUVIA_CHECK((a.index() == 0) && (b.index() == 0) && std::get<0>(a).data() != std::get<0>(b).data());
    auto empty = create("", nullptr);
    RUVIA_CHECK(send_headers(empty));
    auto no_data = empty.next();
    RUVIA_CHECK((no_data.index() == 0) && std::get<0>(no_data).empty() && empty.fin_ready());
    RUVIA_CHECK((empty.acknowledge_fin(true).index() == 0));
    auto head = create("", nullptr, "HEAD");
    RUVIA_CHECK(send_headers(head));
    auto head_done = head.next();
    RUVIA_CHECK((head_done.index() == 0) && std::get<0>(head_done).empty() && head.fin_ready());
    RUVIA_CHECK((head.acknowledge_fin(true).index() == 0));
}

RUVIA_TEST(http3_client_request_write_uses_trace_zero_content_plan_without_dropping_supplied_bytes) {
    counting_resource resource;
    {
        ruvia::detail::http_client_request_storage request("TRACE", "/", &resource);
        request.set_body("must-not-be-silently-removed");
        const auto rejected = cursor::create(std::move(request), "https", "example.com", &resource);
        RUVIA_CHECK((rejected.index() != 0) && std::get<1>(rejected) == cursor::error_type::request_encoding);
        auto empty = create("", &resource, "TRACE");
        RUVIA_CHECK(send_headers(empty));
        const auto no_data = empty.next();
        RUVIA_CHECK((no_data.index() == 0) && std::get<0>(no_data).empty() && empty.fin_ready());
        RUVIA_CHECK((empty.acknowledge_fin(true).index() == 0));
        ruvia::detail::http_client_request_storage sensitive("TRACE", "/", &resource);
        sensitive.append_header("Cookie", "session=private");
        const auto unsafe = cursor::create(std::move(sensitive), "https", "example.com", &resource);
        RUVIA_CHECK((unsafe.index() != 0) && std::get<1>(unsafe) == cursor::error_type::request_encoding);
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.returns_);
}

RUVIA_TEST(http3_client_request_write_rejects_length_and_invalid_fields) {
    ruvia::detail::http_client_request_storage mismatch("POST", "/", nullptr);
    mismatch.append_header("content-length", "99").set_body("short");
    auto rejected = cursor::create(std::move(mismatch), "https", "example.com", nullptr);
    RUVIA_CHECK((rejected.index() != 0));
    ruvia::detail::http_client_request_storage invalid("GET", "/", nullptr);
    invalid.append_header("bad name", "value");
    auto bad_field = cursor::create(std::move(invalid), "https", "example.com", nullptr);
    RUVIA_CHECK((bad_field.index() != 0));

    ruvia::detail::http_client_request_storage bodyless("GET", "/", nullptr);
    bodyless.append_header("Content-Length", "3");
    auto bad_length = cursor::create(std::move(bodyless), "https", "example.com", nullptr);
    RUVIA_CHECK((bad_length.index() != 0) && std::get<1>(bad_length) == cursor::error_type::request_encoding);

    ruvia::detail::http_client_request_storage head("HEAD", "/", nullptr);
    head.append_header("content-length", "4").set_body("body");
    auto head_length = cursor::create(std::move(head), "https", "example.com", nullptr);
    RUVIA_CHECK((head_length.index() == 0) && send_headers(std::get<0>(head_length)));
    if ((head_length.index() == 0)) {
        const auto framing = std::get<0>(head_length).next();
        RUVIA_CHECK((framing.index() == 0) && (std::get<0>(head_length).acknowledge(std::get<0>(framing).size()).index() == 0));
        const auto payload_value = std::get<0>(head_length).next();
        RUVIA_CHECK((payload_value.index() == 0) && std::string_view(std::get<0>(payload_value).data(), std::get<0>(payload_value).size()) == "body");
        RUVIA_CHECK((std::get<0>(head_length).acknowledge(std::get<0>(payload_value).size()).index() == 0));
        RUVIA_CHECK((std::get<0>(head_length).acknowledge_fin(true).index() == 0));
    }
    ruvia::detail::http_client_request_storage wrong_head("HEAD", "/", nullptr);
    wrong_head.append_header("content-length", "5").set_body("body");
    auto wrong_head_length = cursor::create(std::move(wrong_head), "https", "example.com", nullptr);
    RUVIA_CHECK((wrong_head_length.index() != 0) && std::get<1>(wrong_head_length) == cursor::error_type::request_encoding);

    ruvia::detail::http_client_request_storage zero("GET", "/", nullptr);
    zero.append_header("content-length", "0");
    auto allowed = cursor::create(std::move(zero), "https", "example.com", nullptr);
    RUVIA_CHECK((allowed.index() == 0) && send_headers(std::get<0>(allowed)));
    if ((allowed.index() == 0)) {
        auto no_data = std::get<0>(allowed).next();
        RUVIA_CHECK((no_data.index() == 0) && std::get<0>(no_data).empty() && std::get<0>(allowed).fin_ready());
    }

    ruvia::detail::http_client_request_storage invalid_target("GET", "relative", nullptr);
    const auto rejected_target = cursor::create(std::move(invalid_target), "https", "example.com", nullptr);
    RUVIA_CHECK((rejected_target.index() != 0) && std::get<1>(rejected_target) == cursor::error_type::request_encoding);

    ruvia::detail::http_client_request_storage invalid_connect("CONNECT", "example.com", nullptr);
    const auto rejected_connect = cursor::create(std::move(invalid_connect), "https", "example.com", nullptr);
    RUVIA_CHECK((rejected_connect.index() != 0) && std::get<1>(rejected_connect) == cursor::error_type::request_encoding);

    ruvia::detail::http_client_request_storage tunnel("CONNECT", "example.com:443", nullptr);
    auto unsupported = cursor::create(std::move(tunnel), "https", "example.com:443", nullptr);
    RUVIA_CHECK((unsupported.index() != 0) && std::get<1>(unsupported) == cursor::error_type::unsupported_tunnel);
}

RUVIA_TEST(http3_client_request_write_failure_does_not_commit_fin_and_releases_pool) {
    counting_resource pool;
    {
        auto cursor_value = create("body", &pool);
        RUVIA_CHECK(send_headers(cursor_value));
        auto frame = cursor_value.next();
        RUVIA_CHECK((frame.index() == 0) && (cursor_value.acknowledge(std::get<0>(frame).size()).index() == 0));
        auto body = cursor_value.next();
        RUVIA_CHECK((body.index() == 0) && (cursor_value.acknowledge(std::get<0>(body).size()).index() == 0));
        RUVIA_CHECK(cursor_value.fin_ready());
        RUVIA_CHECK((cursor_value.acknowledge_fin(false).index() == 0));
        RUVIA_CHECK(cursor_value.failed() && !cursor_value.finished());
        for (int i = 0; i < 6; ++i) {
            auto repeated = create("reused", &pool);
            RUVIA_CHECK(pool.allocations_ > pool.returns_);
            RUVIA_CHECK((repeated.next().index() == 0));
        }
    }
    RUVIA_CHECK(pool.allocations_ > 0);
    RUVIA_CHECK_EQ(pool.allocations_, pool.returns_);
}

RUVIA_TEST(http3_client_request_write_allocation_failure_is_reported) {
    counting_resource pool;
    pool.reject_ = true;
    ruvia::detail::http_client_request_storage request("POST", "/failure", nullptr);
    request.set_body("body");
    auto failed = cursor::create(std::move(request), "https", "example.com", &pool);
    RUVIA_CHECK((failed.index() != 0) && std::get<1>(failed) == cursor::error_type::out_of_memory);
}

RUVIA_TEST(http3_client_request_write_create_failure_leaves_same_resource_request_retryable) {
    const std::string body(2048, 'r');
    std::string expected_head;
    {
        ruvia::detail::http_client_request_storage reference("POST", "/transaction", nullptr);
        reference.append_header("x-transaction", "kept").set_body(body);
        auto encoded = cursor::create(std::move(reference), "https", "example.com", nullptr);
        RUVIA_CHECK((encoded.index() == 0));
        if ((encoded.index() == 0)) {
            const auto head = std::get<0>(encoded).next();
            RUVIA_CHECK((head.index() == 0));
            expected_head.assign(std::get<0>(head).data(), std::get<0>(head).size());
        }
    }

    bool reached_successful_offset = false;
    for (std::size_t fail_offset = 1; fail_offset < 128 && !reached_successful_offset; ++fail_offset) {
        counting_resource resource;
        {
            ruvia::detail::http_client_request_storage request("POST", "/transaction", &resource);
            request.append_header("x-transaction", "kept").set_body(body);
            const auto body_address = request.body().data();
            const auto live_before = resource.allocations_ - resource.returns_;
            resource.fail_on_attempt_ = resource.attempts_ + fail_offset;
            auto result_value = cursor::create(std::move(request), "https", "example.com", &resource);
            resource.fail_on_attempt_ = 0;
            if ((result_value.index() != 0)) {
                RUVIA_CHECK_EQ(std::get<1>(result_value), cursor::error_type::out_of_memory);
                RUVIA_CHECK_EQ(request.method(), "POST");
                RUVIA_CHECK_EQ(request.target(), "/transaction");
                RUVIA_CHECK_EQ(request.body(), body);
                RUVIA_CHECK_EQ(resource.allocations_ - resource.returns_, live_before);

                auto retry = cursor::create(std::move(request), "https", "example.com", &resource);
                RUVIA_CHECK((retry.index() == 0));
                if ((retry.index() == 0)) {
                    check_encoded_body(std::get<0>(retry), ruvia_ctx, expected_head, body, body_address, true);
                }
            } else {
                reached_successful_offset = true;
                check_encoded_body(std::get<0>(result_value), ruvia_ctx, expected_head, body, body_address, true);
            }
        }
        RUVIA_CHECK(resource.allocations_ > 0);
        RUVIA_CHECK_EQ(resource.allocations_, resource.returns_);
    }
    RUVIA_CHECK(reached_successful_offset);
}

RUVIA_TEST(http3_client_request_write_create_failure_leaves_cross_resource_request_retryable) {
    const std::string body(2048, 'n');
    std::string expected_head;
    {
        ruvia::detail::http_client_request_storage reference("PUT", "/normalize", nullptr);
        reference.append_header("x-normalize", "kept").set_body(body);
        auto encoded = cursor::create(std::move(reference), "https", "example.com", nullptr);
        RUVIA_CHECK((encoded.index() == 0));
        if ((encoded.index() == 0)) {
            const auto head = std::get<0>(encoded).next();
            RUVIA_CHECK((head.index() == 0));
            expected_head.assign(std::get<0>(head).data(), std::get<0>(head).size());
        }
    }

    bool reached_successful_offset = false;
    for (std::size_t fail_offset = 1; fail_offset < 128 && !reached_successful_offset; ++fail_offset) {
        counting_resource source;
        counting_resource destination;
        {
            ruvia::detail::http_client_request_storage request("PUT", "/normalize", &source);
            request.append_header("x-normalize", "kept").set_body(body);
            const auto body_address = request.body().data();
            const auto source_live_before = source.allocations_ - source.returns_;
            destination.fail_on_attempt_ = destination.attempts_ + fail_offset;
            auto result_value = cursor::create(std::move(request), "https", "example.com", &destination);
            destination.fail_on_attempt_ = 0;
            if ((result_value.index() != 0)) {
                RUVIA_CHECK_EQ(std::get<1>(result_value), cursor::error_type::out_of_memory);
                RUVIA_CHECK_EQ(request.method(), "PUT");
                RUVIA_CHECK_EQ(request.target(), "/normalize");
                RUVIA_CHECK_EQ(request.body(), body);
                RUVIA_CHECK_EQ(source.allocations_ - source.returns_, source_live_before);
                RUVIA_CHECK_EQ(destination.allocations_, destination.returns_);

                auto retry = cursor::create(std::move(request), "https", "example.com", &destination);
                RUVIA_CHECK((retry.index() == 0));
                RUVIA_CHECK_EQ(source.large_allocations_, source.large_returns_);
                if ((retry.index() == 0)) {
                    check_encoded_body(std::get<0>(retry), ruvia_ctx, expected_head, body, body_address, false);
                }
            } else {
                reached_successful_offset = true;
                RUVIA_CHECK_EQ(source.large_allocations_, source.large_returns_);
                check_encoded_body(std::get<0>(result_value), ruvia_ctx, expected_head, body, body_address, false);
            }
        }
        RUVIA_CHECK(source.allocations_ > 0);
        RUVIA_CHECK_EQ(source.allocations_, source.returns_);
        RUVIA_CHECK(destination.allocations_ > 0);
        RUVIA_CHECK_EQ(destination.allocations_, destination.returns_);
    }
    RUVIA_CHECK(reached_successful_offset);
}

RUVIA_TEST(http3_client_request_write_invalid_encoding_preserves_request_for_retry) {
    counting_resource resource;
    const std::string body(256, 'i');
    {
        ruvia::detail::http_client_request_storage request("POST", "/retry-invalid", &resource);
        request.append_header("x-preserved", "yes").set_body(body);
        const auto body_address = request.body().data();
        auto rejected = cursor::create(std::move(request), "bad^scheme", "example.com", &resource);
        RUVIA_CHECK((rejected.index() != 0) && std::get<1>(rejected) == cursor::error_type::request_encoding);
        RUVIA_CHECK_EQ(request.method(), "POST");
        RUVIA_CHECK_EQ(request.target(), "/retry-invalid");
        RUVIA_CHECK_EQ(request.body(), body);

        auto retry = cursor::create(std::move(request), "https", "example.com", &resource);
        RUVIA_CHECK((retry.index() == 0));
        if ((retry.index() == 0) && send_headers(std::get<0>(retry))) {
            auto frame = std::get<0>(retry).next();
            RUVIA_CHECK((frame.index() == 0));
            if ((frame.index() == 0) && (std::get<0>(retry).acknowledge(std::get<0>(frame).size()).index() == 0)) {
                const auto payload_value = std::get<0>(retry).next();
                RUVIA_CHECK((payload_value.index() == 0));
                if ((payload_value.index() == 0)) {
                    RUVIA_CHECK(std::get<0>(payload_value).data() == body_address);
                }
            }
        }
    }
    RUVIA_CHECK(resource.allocations_ > 0);
    RUVIA_CHECK_EQ(resource.allocations_, resource.returns_);
}

RUVIA_TEST(http3_client_request_write_repeated_transactional_builds_return_storage) {
    counting_resource resource;
    const std::string body(512, 'b');
    for (int i = 0; i < 64; ++i) {
        {
            ruvia::detail::http_client_request_storage request("POST", "/repeat", &resource);
            request.append_header("x-repeat", "value").set_body(body);
            auto cursor_value = cursor::create(std::move(request), "https", "example.com", &resource);
            RUVIA_CHECK((cursor_value.index() == 0));
            if ((cursor_value.index() == 0)) {
                RUVIA_CHECK((std::get<0>(cursor_value).next().index() == 0));
            }
        }
        RUVIA_CHECK_EQ(resource.allocations_, resource.returns_);
    }
    RUVIA_CHECK(resource.allocations_ > 0);
}

RUVIA_TEST(http3_client_request_write_cold_cursor_returns_storage) {
    counting_resource pool;
    {
        ruvia::detail::http_client_request_storage request("POST", "/cold", &pool);
        request.set_body("body");
        auto cold = cursor::create(std::move(request), "https", "example.com", &pool);
        RUVIA_CHECK((cold.index() == 0));
    }
    RUVIA_CHECK_EQ(pool.allocations_, pool.returns_);
}
