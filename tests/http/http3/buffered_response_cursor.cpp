#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/http3_buffered_response_cursor.h"
#include "ruvia/http/http3_client_response.h"

#include "test_harness.h"

namespace {

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_{};
    std::size_t returns_{};
    bool reject_{};

private:
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        if (reject_ && size >= 32) {
            throw std::bad_alloc();
        }
        ++allocations_;
        return std::pmr::new_delete_resource()->allocate(size, alignment);
    }
    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        ++returns_;
        std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

using cursor = ruvia::http3_buffered_response_cursor;

cursor make_cursor(const ruvia::http_response& response,
    const ruvia::http_buffered_response_write_plan& plan, std::pmr::memory_resource* resource) {
    auto result_value = cursor::create(response, plan, resource);
    if ((result_value.index() != 0)) {
        throw std::runtime_error("failed to create HTTP/3 response cursor");
    }
    return std::move(std::get<0>(result_value));
}

void acknowledge_all(cursor& cursor_value) {
    for (;;) {
        auto next_value = cursor_value.next();
        if ((next_value.index() != 0)) {
            throw std::runtime_error("failed to retrieve HTTP/3 output segment");
        }
        if (std::get<0>(next_value).empty()) {
            break;
        }
        if (cursor_value.acknowledge(std::get<0>(next_value).size()).index() != 0) {
            throw std::runtime_error("failed to acknowledge HTTP/3 output segment");
        }
    }
}

}  // namespace

RUVIA_TEST(http3_buffered_response_write_orders_headers_data_and_explicit_fin) {
    ruvia::http_response response;
    response.body("payload");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    cursor cursor_value = make_cursor(response, plan, nullptr);

    RUVIA_CHECK(cursor_value.next_step() == cursor::step::bytes);
    auto headers = cursor_value.next();
    RUVIA_CHECK((headers.index() == 0) && !std::get<0>(headers).empty());
    RUVIA_CHECK(static_cast<unsigned char>((std::get<0>(headers))[0]) == 0x01);
    RUVIA_CHECK((cursor_value.acknowledge(std::get<0>(headers).size())).index() == 0);
    RUVIA_CHECK(cursor_value.next_step() == cursor::step::bytes);
    RUVIA_CHECK(!cursor_value.fin_ready());

    auto frame = cursor_value.next();
    RUVIA_CHECK((frame.index() == 0) && !std::get<0>(frame).empty());
    RUVIA_CHECK(static_cast<unsigned char>((std::get<0>(frame))[0]) == 0x00);
    RUVIA_CHECK((cursor_value.acknowledge(std::get<0>(frame).size())).index() == 0);
    RUVIA_CHECK(cursor_value.next_step() == cursor::step::bytes);
    auto body = cursor_value.next();
    RUVIA_CHECK((body.index() == 0) && std::string_view(std::get<0>(body).data(), std::get<0>(body).size()) == "payload");
    if ((body.index() != 0)) {
        return;
    }
    const auto partial_body_size = std::get<0>(body).size() / 2;
    RUVIA_CHECK((cursor_value.acknowledge(partial_body_size)).index() == 0);
    RUVIA_CHECK(cursor_value.next_step() == cursor::step::bytes);
    const auto body_remainder = cursor_value.next();
    RUVIA_CHECK((body_remainder.index() == 0) &&
                std::get<0>(body_remainder).data() == std::get<0>(body).data() + partial_body_size);
    if ((body_remainder.index() != 0)) {
        return;
    }
    RUVIA_CHECK((cursor_value.acknowledge(std::get<0>(body_remainder).size())).index() == 0);
    RUVIA_CHECK(cursor_value.next_step() == cursor::step::fin);
    RUVIA_CHECK(cursor_value.fin_ready());
    RUVIA_CHECK(!cursor_value.finished());
    RUVIA_CHECK((cursor_value.acknowledge_fin(true)).index() == 0);
    RUVIA_CHECK(cursor_value.finished());
    RUVIA_CHECK(cursor_value.next_step() == cursor::step::complete);
}

RUVIA_TEST(http3_buffered_response_write_preserves_encoded_decoded_field_section_size_across_move_and_fin) {
    ruvia::http_response response;
    response.body("payload");
    response.header("X-Projection", "retained");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    const auto encoded = ruvia::encode_http3_response_head(response, plan);
    RUVIA_CHECK((encoded.index() == 0));
    if ((encoded.index() != 0)) {
        return;
    }
    const auto decoded_size = std::get<0>(encoded).field_section_.decoded_field_section_size();
    RUVIA_CHECK(decoded_size > 42U);

    cursor cursor_value = make_cursor(response, plan, nullptr);
    RUVIA_CHECK_EQ(cursor_value.decoded_field_section_size(), decoded_size);
    const auto headers = cursor_value.next();
    RUVIA_CHECK((headers.index() == 0) && std::get<0>(headers).size() > 1);
    if ((headers.index() != 0) || std::get<0>(headers).size() <= 1) {
        return;
    }
    const auto partial_size = std::get<0>(headers).size() / 2;
    RUVIA_CHECK((cursor_value.acknowledge(partial_size)).index() == 0);
    RUVIA_CHECK_EQ(cursor_value.decoded_field_section_size(), decoded_size);
    const auto remainder = cursor_value.next();
    RUVIA_CHECK((remainder.index() == 0) && std::get<0>(remainder).size() == std::get<0>(headers).size() - partial_size);
    if ((remainder.index() != 0)) {
        return;
    }
    RUVIA_CHECK((cursor_value.acknowledge(std::get<0>(remainder).size())).index() == 0);
    RUVIA_CHECK_EQ(cursor_value.decoded_field_section_size(), decoded_size);

    cursor moved(std::move(cursor_value));
    RUVIA_CHECK_EQ(moved.decoded_field_section_size(), decoded_size);
    RUVIA_CHECK_EQ(cursor_value.decoded_field_section_size(), 0U);
    RUVIA_CHECK(cursor_value.failed());
    const auto frame = moved.next();
    RUVIA_CHECK((frame.index() == 0) && !std::get<0>(frame).empty());
    if ((frame.index() != 0) || std::get<0>(frame).empty()) {
        return;
    }
    RUVIA_CHECK((moved.acknowledge(std::get<0>(frame).size())).index() == 0);
    RUVIA_CHECK_EQ(moved.decoded_field_section_size(), decoded_size);
    const auto body = moved.next();
    RUVIA_CHECK((body.index() == 0) && std::string_view(std::get<0>(body).data(), std::get<0>(body).size()) == "payload");
    if ((body.index() != 0)) {
        return;
    }
    RUVIA_CHECK((moved.acknowledge(std::get<0>(body).size())).index() == 0);
    RUVIA_CHECK(moved.fin_ready());
    RUVIA_CHECK_EQ(moved.decoded_field_section_size(), decoded_size);
    RUVIA_CHECK((moved.acknowledge_fin(true)).index() == 0);
    RUVIA_CHECK(moved.finished());
    RUVIA_CHECK_EQ(moved.decoded_field_section_size(), decoded_size);
}

RUVIA_TEST(http3_buffered_response_write_partial_and_want_keep_the_offered_address_stable) {
    ruvia::http_response response;
    response.body("abcd");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    auto cursor_value = make_cursor(response, plan, nullptr);
    RUVIA_CHECK((cursor_value.acknowledge(1)).index() != 0);
    auto offered = cursor_value.next();
    RUVIA_CHECK((offered.index() == 0) && std::get<0>(offered).size() > 1);
    const auto original = std::string(std::get<0>(offered).data(), std::get<0>(offered).size());
    const auto* address = std::get<0>(offered).data();
    const char first = std::get<0>(offered).front();
    const auto demand = cursor_value.next_step();
    RUVIA_CHECK(demand == cursor::step::bytes);
    RUVIA_CHECK((cursor_value.acknowledge(0)).index() == 0);
    auto retry = cursor_value.next();
    RUVIA_CHECK((retry.index() == 0) && std::get<0>(retry).data() == address && std::get<0>(retry).front() == first &&
                std::get<0>(retry).size() == std::get<0>(offered).size());
    RUVIA_CHECK(std::string_view(std::get<0>(retry).data(), std::get<0>(retry).size()) == original);
    RUVIA_CHECK((cursor_value.acknowledge(std::get<0>(retry).size() + 1).index() != 0));
    RUVIA_CHECK((cursor_value.acknowledge(1)).index() == 0);
    auto remainder = cursor_value.next();
    RUVIA_CHECK((remainder.index() == 0) && std::get<0>(remainder).data() == address + 1);
}

RUVIA_TEST(http3_buffered_response_write_cannot_move_an_outstanding_write_buffer) {
    ruvia::http_response response;
    response.body("payload");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    auto cursor_value = make_cursor(response, plan, nullptr);
    const auto segment = cursor_value.next();
    RUVIA_CHECK((segment.index() == 0) && !std::get<0>(segment).empty());
    if ((segment.index() != 0) || std::get<0>(segment).empty()) {
        return;
    }
    bool rejected = false;
    try {
        cursor moved(std::move(cursor_value));
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    const auto retry = cursor_value.next();
    RUVIA_CHECK((retry.index() == 0) && std::get<0>(retry).data() == std::get<0>(segment).data() && std::get<0>(retry).size() == std::get<0>(segment).size());
    RUVIA_CHECK((cursor_value.acknowledge(std::get<0>(segment).size())).index() == 0);
    // Moving after all previously offered bytes were confirmed is safe.
    cursor moved(std::move(cursor_value));
    acknowledge_all(moved);
    RUVIA_CHECK((moved.acknowledge_fin(true)).index() == 0);
}

RUVIA_TEST(http3_buffered_response_write_head_retains_representation_length_without_data) {
    ruvia::http_response response;
    response.body("head representation");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::head, response);
    cursor cursor_value = make_cursor(response, plan, nullptr);
    acknowledge_all(cursor_value);
    RUVIA_CHECK(cursor_value.fin_ready());
    RUVIA_CHECK(cursor_value.next_step() == cursor::step::fin);
    RUVIA_CHECK((cursor_value.acknowledge_fin(true)).index() == 0);
    RUVIA_CHECK(cursor_value.finished());
    RUVIA_CHECK(cursor_value.next_step() == cursor::step::complete);
    RUVIA_CHECK_EQ(plan.content_length(), std::uint64_t{19});
}

RUVIA_TEST(http3_buffered_response_write_demand_is_pure_and_finds_headers_only_fin) {
    counting_resource memory;
    for (const auto method : {ruvia::http_known_method::head, ruvia::http_known_method::get}) {
        ruvia::http_response response;
        if (method == ruvia::http_known_method::head) {
            response.body("representation");
        }
        const auto plan = ruvia::plan_buffered_http_response_write(method, response);
        auto cursor_value = make_cursor(response, plan, &memory);
        RUVIA_CHECK(cursor_value.next_step() == cursor::step::bytes);

        auto headers = cursor_value.next();
        RUVIA_CHECK((headers.index() == 0) && !std::get<0>(headers).empty());
        if ((headers.index() != 0) || std::get<0>(headers).empty()) {
            return;
        }
        const auto original = std::string(std::get<0>(headers).data(), std::get<0>(headers).size());
        const auto* address = std::get<0>(headers).data();
        const auto allocations = memory.allocations_;
        const auto returns = memory.returns_;
        for (unsigned attempt_value = 0; attempt_value < 4; ++attempt_value) {
            RUVIA_CHECK(cursor_value.next_step() == cursor::step::bytes);
            RUVIA_CHECK_EQ(memory.allocations_, allocations);
            RUVIA_CHECK_EQ(memory.returns_, returns);
        }
        const auto retry = cursor_value.next();
        RUVIA_CHECK((retry.index() == 0) && std::get<0>(retry).data() == address &&
                    std::string_view(std::get<0>(retry).data(), std::get<0>(retry).size()) == original);
        RUVIA_CHECK((cursor_value.acknowledge(std::get<0>(headers).size())).index() == 0);
        RUVIA_CHECK(cursor_value.next_step() == cursor::step::fin);
        RUVIA_CHECK_EQ(memory.allocations_, allocations);
        RUVIA_CHECK_EQ(memory.returns_, returns);

        const auto fin = cursor_value.next();
        RUVIA_CHECK((fin.index() == 0) && std::get<0>(fin).empty() && cursor_value.fin_ready());
        RUVIA_CHECK((cursor_value.acknowledge_fin(true)).index() == 0);
        RUVIA_CHECK(cursor_value.next_step() == cursor::step::complete);
    }
    RUVIA_CHECK_EQ(memory.allocations_, memory.returns_);
}

RUVIA_TEST(http3_buffered_response_write_file_without_payload_sends_only_metadata) {
    counting_resource memory;
    for (unsigned scenario = 0; scenario < 2; ++scenario) {
        const auto method = scenario == 0 ? ruvia::http_known_method::head : ruvia::http_known_method::get;
        const std::uint64_t length = scenario == 0 ? 5 : 0;
        ruvia::http_response response;
        response.file_body("unopened-response.bin", length, 0, length, ruvia::http_response_file_identity::checked({}));
        response.header("X-Projection", "retained");
        const auto plan = ruvia::plan_buffered_http_response_write(method, response);
        RUVIA_CHECK(!plan.send_body() || plan.content_length() == 0);
        const auto encoded = ruvia::encode_http3_response_head(response, plan, {}, &memory);
        RUVIA_CHECK((encoded.index() == 0));
        if ((encoded.index() != 0)) {
            return;
        }
        const auto decoded_size = std::get<0>(encoded).field_section_.decoded_field_section_size();
        RUVIA_CHECK(decoded_size > 42U);
        auto cursor_value = make_cursor(response, plan, &memory);
        RUVIA_CHECK_EQ(cursor_value.decoded_field_section_size(), decoded_size);
        ruvia::http3_client_response decoder(0, method, &memory);
        struct received_events final {
            std::optional<std::uint64_t> length_;
            unsigned heads_{};
            unsigned bodies_{};
            unsigned ends_{};
        } received;
        const auto callback_value = [](void* opaque, const ruvia::http3_client_response_event& event) {
            auto& state_value = *static_cast<received_events*>(opaque);
            if (event.kind_ == ruvia::http3_client_response_event_kind::final_head) {
                ++state_value.heads_;
                state_value.length_ = event.head_->content_length_;
            } else if (event.kind_ == ruvia::http3_client_response_event_kind::body) {
                ++state_value.bodies_;
            } else if (event.kind_ == ruvia::http3_client_response_event_kind::message_end) {
                ++state_value.ends_;
            }
        };
        const auto headers = cursor_value.next();
        RUVIA_CHECK((headers.index() == 0) && !std::get<0>(headers).empty());
        if ((headers.index() != 0) || std::get<0>(headers).empty()) {
            return;
        }
        RUVIA_CHECK(decoder.feed(std::get<0>(headers), false, false, callback_value, &received).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK((cursor_value.acknowledge(std::get<0>(headers).size())).index() == 0);
        RUVIA_CHECK_EQ(cursor_value.decoded_field_section_size(), decoded_size);
        const auto following = cursor_value.next();
        RUVIA_CHECK((following.index() == 0) && std::get<0>(following).empty() && cursor_value.fin_ready());
        RUVIA_CHECK_EQ(cursor_value.decoded_field_section_size(), decoded_size);
        RUVIA_CHECK(decoder.feed({}, true, false, callback_value, &received).status_ == ruvia::http3_client_response_status::message_end);
        RUVIA_CHECK((cursor_value.acknowledge_fin(true)).index() == 0);
        RUVIA_CHECK_EQ(cursor_value.decoded_field_section_size(), decoded_size);
        RUVIA_CHECK(received.heads_ == 1 && received.bodies_ == 0 && received.ends_ == 1);
        RUVIA_CHECK(received.length_ == length);
    }
    RUVIA_CHECK_EQ(memory.allocations_, memory.returns_);
}

RUVIA_TEST(http3_buffered_response_write_empty_and_no_content_responses_omit_data) {
    for (const auto status : {ruvia::http_status::ok, ruvia::http_status::no_content,
             ruvia::http_status::reset_content, ruvia::http_status::not_modified}) {
        ruvia::http_response response;
        response.status(status);
        const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
        auto cursor_value = make_cursor(response, plan, nullptr);
        acknowledge_all(cursor_value);
        RUVIA_CHECK(cursor_value.fin_ready());
        RUVIA_CHECK((cursor_value.acknowledge_fin(true)).index() == 0);
    }
}

RUVIA_TEST(http3_buffered_response_write_rejects_files_and_encoding_failures) {
    ruvia::http_response file_response;
    file_response.file_body("response.bin", 5, 0, 5, ruvia::http_response_file_identity::checked({}));
    const auto file_plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, file_response);
    auto file = cursor::create(file_response, file_plan, nullptr);
    RUVIA_CHECK((file.index() != 0) && std::get<1>(file) == cursor::error::file_body_unsupported);

    ruvia::http_response invalid;
    invalid.header("connection", "close");
    const auto invalid_plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, invalid);
    auto encoded = cursor::create(invalid, invalid_plan, nullptr);
    RUVIA_CHECK((encoded.index() != 0) && std::get<1>(encoded) == cursor::error::response_encoding);
}

RUVIA_TEST(http3_buffered_response_write_returns_pool_storage_only_after_cursor_retirement) {
    counting_resource memory;
    ruvia::http_response response;
    response.body("retained body");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    {
        auto held = make_cursor(response, plan, &memory);
        for (int i = 0; i < 8; ++i) {
            auto cursor_value = make_cursor(response, plan, &memory);
            RUVIA_CHECK(memory.allocations_ > memory.returns_);
            RUVIA_CHECK(response.body_bytes() == "retained body");
            acknowledge_all(cursor_value);
            RUVIA_CHECK((cursor_value.acknowledge_fin(true)).index() == 0);
        }
        RUVIA_CHECK(memory.allocations_ > memory.returns_);
        RUVIA_CHECK(response.body_bytes() == "retained body");
        acknowledge_all(held);
        RUVIA_CHECK((held.acknowledge_fin(true)).index() == 0);
    }
    RUVIA_CHECK_EQ(memory.allocations_, memory.returns_);
}

RUVIA_TEST(http3_buffered_response_write_transport_failure_does_not_commit_fin) {
    ruvia::http_response response;
    response.body("failure");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    auto cursor_value = make_cursor(response, plan, nullptr);
    acknowledge_all(cursor_value);
    RUVIA_CHECK(cursor_value.fin_ready());
    RUVIA_CHECK(cursor_value.next_step() == cursor::step::fin);
    RUVIA_CHECK((cursor_value.acknowledge_fin(false)).index() == 0);
    RUVIA_CHECK(cursor_value.failed());
    RUVIA_CHECK(cursor_value.next_step() == cursor::step::failed);
    RUVIA_CHECK(!cursor_value.finished());
    RUVIA_CHECK((cursor_value.next().index() != 0));
}

RUVIA_TEST(http3_buffered_response_write_handles_out_of_memory_and_not_started_cleanup) {
    ruvia::http_response response;
    response.body("body");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    counting_resource memory;
    memory.reject_ = true;
    auto failed = cursor::create(response, plan, &memory);
    RUVIA_CHECK((failed.index() != 0) && std::get<1>(failed) == cursor::error::out_of_memory);
    memory.reject_ = false;
    {
        auto cold = cursor::create(response, plan, &memory);
        RUVIA_CHECK((cold.index() == 0));
    }
    RUVIA_CHECK_EQ(memory.allocations_, memory.returns_);
}

RUVIA_TEST(http3_buffered_response_cursor_consumes_encoded_head_storage) {
    counting_resource head_memory;
    counting_resource cursor_memory;
    ruvia::http_response response;
    response.body("retained body");
    response.header("X-Projection", "retained");
    const auto plan = ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    std::optional<cursor> cursor;
    std::size_t decoded_size{};
    {
        auto head = ruvia::encode_http3_response_head(response, plan, {}, &head_memory);
        RUVIA_CHECK((head.index() == 0));
        if ((head.index() != 0)) {
            return;
        }
        decoded_size = std::get<0>(head).field_section_.decoded_field_section_size();
        auto created = cursor::create(response, plan, std::move(std::get<0>(head)), &cursor_memory);
        RUVIA_CHECK((created.index() == 0));
        if ((created.index() != 0)) {
            return;
        }
        cursor.emplace(std::move(std::get<0>(created)));
    }
    RUVIA_CHECK(head_memory.allocations_ > 0);
    RUVIA_CHECK_EQ(head_memory.allocations_, head_memory.returns_);
    RUVIA_CHECK(cursor_memory.allocations_ > cursor_memory.returns_);
    RUVIA_CHECK_EQ(cursor->decoded_field_section_size(), decoded_size);
    acknowledge_all(*cursor);
    RUVIA_CHECK((cursor->acknowledge_fin(true)).index() == 0);
    RUVIA_CHECK(cursor->finished());
    RUVIA_CHECK(response.body_bytes() == "retained body");
    cursor.reset();
    RUVIA_CHECK_EQ(cursor_memory.allocations_, cursor_memory.returns_);
}
