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
#include <variant>
#include <vector>

#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_var_int.h"

#include "http3/http3_client_sans_io_session_engine.h"
#include "test_harness.h"

namespace {
class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_{};
    std::size_t deallocations_{};
    std::size_t body_allocations_{};
    std::size_t live_bytes_{};
    bool reject_{};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_) {
            throw std::bad_alloc();
        }
        void* const data = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++allocations_;
        if (bytes_value >= 1024) {
            ++body_allocations_;
        }
        live_bytes_ += bytes_value;
        return data;
    }
    void do_deallocate(void* data, std::size_t bytes_value, std::size_t alignment) override {
        ++deallocations_;
        live_bytes_ -= bytes_value;
        std::pmr::new_delete_resource()->deallocate(data, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

std::vector<char> frame(std::uint64_t type, std::span<const char> payload_value) {
    std::vector<char> result_value(16);
    const auto type_bytes = ruvia::encode_http3_var_int(result_value, type);
    const auto length_bytes = ruvia::encode_http3_var_int(std::span<char>(result_value).subspan(std::get<0>(type_bytes)), payload_value.size());
    result_value.resize(std::get<0>(type_bytes) + std::get<0>(length_bytes));
    result_value.insert(result_value.end(), payload_value.begin(), payload_value.end());
    return result_value;
}
std::vector<char> response_head(std::string_view status = "200",
    std::optional<std::string_view> content_length = {}) {
    std::pmr::monotonic_buffer_resource resource;
    std::vector<ruvia::http3_field_section_field_view> fields_value{{":status", status}, {"x-session", "owned"}};
    if (content_length) {
        fields_value.push_back({"content-length", *content_length});
    }
    const auto encoded = ruvia::encode_http3_field_section(fields_value, &resource);
    return frame(1, std::get<0>(encoded));
}
std::vector<char> body(std::string_view text) {
    return frame(0, std::span<const char>(text.data(), text.size()));
}
std::vector<char> response_trailers() {
    std::pmr::monotonic_buffer_resource resource;
    constexpr std::array<ruvia::http3_field_section_field_view, 2> fields_value{{{"x-one", "first"},
        {"x-two", "second"}}};
    const auto encoded = ruvia::encode_http3_field_section(fields_value, &resource);
    return frame(1, std::get<0>(encoded));
}

bool plan_matches(const std::optional<ruvia::http_response_body_plan>& plan,
    ruvia::http_known_method method, std::uint16_t status,
    ruvia::http_response_content_semantics semantics, bool body_suppressed) {
    return plan && plan->request_method() == method &&
           plan->response_status() == ruvia::http_status_code::from_value(status) &&
           plan->content_semantics() == semantics &&
           plan->body_suppressed() == body_suppressed;
}

struct streamed_events final {
    explicit streamed_events(std::pmr::memory_resource* resource)
        : body_(resource),
          trailer_names_(resource) {}
    std::pmr::string body_;
    std::pmr::string trailer_names_;
    std::uint16_t status_{};
    std::size_t heads_{};
    std::size_t chunks_{};
    std::size_t trailers_{};
    std::size_t ends_{};
    ruvia::detail::http3_client_sans_io_session_engine* engine_{};
    bool checked_reentry_{};
    bool fail_body_{};
};

void collect_streamed_response(void* raw, const ruvia::http3_connection_event& event) {
    auto& observed_value = *static_cast<streamed_events*>(raw);
    switch (event.kind_) {
        case ruvia::http3_connection_event_kind::final_head:
            ++observed_value.heads_;
            observed_value.status_ = event.head_->status_;
            return;
        case ruvia::http3_connection_event_kind::body:
            if (observed_value.fail_body_) {
                throw std::runtime_error("streaming response sink rejected body");
            }
            if (observed_value.engine_ && !observed_value.checked_reentry_) {
                observed_value.checked_reentry_ = true;
                if (observed_value.engine_->register_request(8, ruvia::http_known_method::get).status_ !=
                        ruvia::detail::http3_client_sans_io_session_status::invalid_state ||
                    observed_value.engine_->release(event.stream_id_) ||
                    observed_value.engine_->cancel_request(event.stream_id_) ||
                    observed_value.engine_->feed(event.stream_id_, {}).status_ !=
                        ruvia::detail::http3_client_sans_io_session_status::invalid_state ||
                    observed_value.engine_->stop().status_ !=
                        ruvia::detail::http3_client_sans_io_session_status::invalid_state) {
                    throw std::logic_error("streaming sink illegally modified active engine");
                }
            }
            observed_value.body_.append(event.body_.data(), event.body_.size());
            ++observed_value.chunks_;
            return;
        case ruvia::http3_connection_event_kind::trailer_field:
            observed_value.trailer_names_.append(event.trailer_.name_);
            ++observed_value.trailers_;
            return;
        case ruvia::http3_connection_event_kind::message_end:
            ++observed_value.ends_;  // A sink must never see terminal delivery.
            return;
        default:
            return;
    }
}
}  // namespace

RUVIA_TEST(http3_client_sans_io_session_deeply_retains_interleaved_responses_until_release) {
    std::pmr::monotonic_buffer_resource worker;
    ruvia::detail::http3_client_sans_io_session_engine session_value(&worker);
    RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get).status_ ==
                ruvia::detail::http3_client_sans_io_session_status::need_more_data);
    RUVIA_CHECK(session_value.register_request(4, ruvia::http_known_method::get).status_ ==
                ruvia::detail::http3_client_sans_io_session_status::need_more_data);
    const auto head = response_head();
    const auto payload_value = body("one");
    RUVIA_CHECK(session_value.feed(0, head).status_ == ruvia::detail::http3_client_sans_io_session_status::need_more_data);
    RUVIA_CHECK(!session_value.response(0).has_value());
    RUVIA_CHECK(session_value.feed(4, head).status_ == ruvia::detail::http3_client_sans_io_session_status::need_more_data);
    RUVIA_CHECK(session_value.feed(0, payload_value, true).status_ == ruvia::detail::http3_client_sans_io_session_status::message_end);
    RUVIA_CHECK(session_value.feed(4, body("two"), true).status_ == ruvia::detail::http3_client_sans_io_session_status::message_end);
    const auto first = session_value.response(0);
    const auto second = session_value.response(4);
    RUVIA_CHECK(first && second);
    RUVIA_CHECK(first->status_ == 200 && first->complete_);
    RUVIA_CHECK(first->headers_.size() == 1);
    RUVIA_CHECK(first->headers_[0].name_ == "x-session");
    RUVIA_CHECK(first->headers_[0].value_ == "owned");
    RUVIA_CHECK(std::string_view(first->body_.data(), first->body_.size()) == "one");
    RUVIA_CHECK(std::string_view(second->body_.data(), second->body_.size()) == "two");
    RUVIA_CHECK(session_value.retained_body_bytes() == 6);
    RUVIA_CHECK(session_value.release(0));
    RUVIA_CHECK(session_value.retained_body_bytes() == 3);
    RUVIA_CHECK(session_value.response(4).has_value());
}

RUVIA_TEST(http3_client_sans_io_session_retains_the_protocol_final_response_body_plan) {
    counting_resource memory;
    std::optional<ruvia::http_response_body_plan> retained_plan;
    {
        ruvia::detail::http3_client_sans_io_session_engine session_value(&memory);
        RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(0, response_head("103")).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::need_more_data);
        RUVIA_CHECK(!session_value.response(0));
        RUVIA_CHECK(session_value.feed(0, response_head("200", "3")).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::need_more_data);
        RUVIA_CHECK(session_value.feed(0, body("abc"), true).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::message_end);
        const auto get_with_body = session_value.response(0);
        RUVIA_CHECK(get_with_body && plan_matches(get_with_body->response_body_plan_,
                                         ruvia::http_known_method::get, 200,
                                         ruvia::http_response_content_semantics::with_content, false));
        retained_plan = get_with_body->response_body_plan_;
        RUVIA_CHECK(session_value.release(0));

        RUVIA_CHECK(session_value.register_request(4, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(4, response_head()).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::need_more_data);
        RUVIA_CHECK(session_value.feed(4, {}, true).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::message_end);
        const auto get_without_body = session_value.response(4);
        RUVIA_CHECK(get_without_body && plan_matches(get_without_body->response_body_plan_,
                                            ruvia::http_known_method::get, 200,
                                            ruvia::http_response_content_semantics::with_content, false));
        RUVIA_CHECK(session_value.release(4));

        RUVIA_CHECK(session_value.register_request(8, ruvia::http_known_method::head).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(8, response_head("200", "5")).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::need_more_data);
        RUVIA_CHECK(session_value.feed(8, {}, true).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::message_end);
        const auto head = session_value.response(8);
        RUVIA_CHECK(head && plan_matches(head->response_body_plan_, ruvia::http_known_method::head, 200,
                                ruvia::http_response_content_semantics::without_content, true));
        RUVIA_CHECK(session_value.release(8));

        RUVIA_CHECK(session_value.register_request(12, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(12, response_head("204")).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::need_more_data);
        RUVIA_CHECK(session_value.feed(12, {}, true).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::message_end);
        const auto no_content = session_value.response(12);
        RUVIA_CHECK(no_content && plan_matches(no_content->response_body_plan_, ruvia::http_known_method::get, 204,
                                      ruvia::http_response_content_semantics::without_content, true));
        RUVIA_CHECK(session_value.release(12));

        RUVIA_CHECK(session_value.register_request(16, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(16, response_head("304", "7")).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::need_more_data);
        RUVIA_CHECK(session_value.feed(16, {}, true).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::message_end);
        const auto not_modified = session_value.response(16);
        RUVIA_CHECK(not_modified && plan_matches(not_modified->response_body_plan_, ruvia::http_known_method::get, 304,
                                        ruvia::http_response_content_semantics::without_content, true));
        RUVIA_CHECK(session_value.release(16));

        RUVIA_CHECK(session_value.register_request(20, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(20, response_head()).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::need_more_data);
        RUVIA_CHECK(session_value.feed(20, {}, false, true).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::reset);
        const auto reset = session_value.response(20);
        RUVIA_CHECK(reset && reset->reset_ && plan_matches(reset->response_body_plan_, ruvia::http_known_method::get, 200, ruvia::http_response_content_semantics::with_content, false));
        RUVIA_CHECK(session_value.release(20));
    }
    RUVIA_CHECK(plan_matches(retained_plan, ruvia::http_known_method::get, 200,
        ruvia::http_response_content_semantics::with_content, false));
    RUVIA_CHECK(memory.allocations_ == memory.deallocations_ && memory.live_bytes_ == 0);
}

RUVIA_TEST(http3_client_sans_io_session_streams_owned_events_without_retaining_body) {
    counting_resource memory;
    {
        ruvia::detail::http3_client_sans_io_session_engine::limits_type limits{};
        limits.max_body_bytes_per_stream_ = 2;
        limits.max_total_body_bytes_ = 2;
        ruvia::detail::http3_client_sans_io_session_engine session_value(&memory, limits);
        streamed_events observed_value(&memory);
        observed_value.engine_ = &session_value;
        RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get,
                                     {.callback_ = collect_streamed_response, .context_ = &observed_value})
                        .scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.register_request(4, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        auto head = response_head();
        RUVIA_CHECK(session_value.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
        const auto buffered = body("ab");
        RUVIA_CHECK(session_value.feed(4, head).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(4, buffered, true).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::message_end);
        RUVIA_CHECK(session_value.retained_body_bytes() == 2);
        auto first = body("abc");
        RUVIA_CHECK(session_value.feed(0, first).scope_ == ruvia::http3_connection_error_scope::none);
        std::fill(first.begin(), first.end(), '?');
        RUVIA_CHECK(session_value.feed(0, body("def")).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.retained_body_bytes() == 2);
        RUVIA_CHECK(session_value.feed(0, response_trailers()).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(0, {}, true).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::message_end);
        const auto response = session_value.response(0);
        RUVIA_CHECK(response && response->complete_ && response->status_ == 200 && response->body_.empty());
        RUVIA_CHECK(observed_value.heads_ == 1 && observed_value.chunks_ == 2 && observed_value.trailers_ == 2 &&
                    observed_value.ends_ == 0 && observed_value.status_ == 200 && observed_value.checked_reentry_);
        RUVIA_CHECK(observed_value.body_ == "abcdef" && observed_value.trailer_names_ == "x-onex-two");
        RUVIA_CHECK(session_value.release(4));
        RUVIA_CHECK(session_value.retained_body_bytes() == 0);
        RUVIA_CHECK(session_value.release(0));
    }
    RUVIA_CHECK(memory.allocations_ == memory.deallocations_ && memory.live_bytes_ == 0);
}

RUVIA_TEST(http3_client_sans_io_session_streamed_owner_commits_fin_after_successful_feed) {
    counting_resource memory;
    {
        ruvia::detail::http3_client_sans_io_session_engine session_value(&memory);
        streamed_events observed_value(&memory);
        RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get,
                                     {.callback_ = collect_streamed_response, .context_ = &observed_value})
                        .scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(0, response_head()).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::need_more_data);
        RUVIA_CHECK(observed_value.heads_ == 1 && observed_value.ends_ == 0 && !session_value.response(0));
        RUVIA_CHECK(session_value.feed(0, body("streamed")).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::need_more_data);
        RUVIA_CHECK(session_value.feed(0, response_trailers()).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::need_more_data);
        RUVIA_CHECK(observed_value.ends_ == 0 && !session_value.response(0));
        RUVIA_CHECK(session_value.feed(0, {}, true).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::message_end);
        const auto retained = session_value.response(0);
        RUVIA_CHECK(retained && retained->complete_ && retained->status_ == 200);
        RUVIA_CHECK(retained->headers_.size() == 1 && retained->trailers_.size() == 2);
        RUVIA_CHECK(retained->body_.empty() && observed_value.body_ == "streamed");
        RUVIA_CHECK(observed_value.ends_ == 0);
        RUVIA_CHECK(session_value.release(0));
    }
    RUVIA_CHECK(memory.allocations_ > 0);
    RUVIA_CHECK(memory.allocations_ == memory.deallocations_ && memory.live_bytes_ == 0);
}

RUVIA_TEST(http3_client_sans_io_session_streamed_malformed_fin_cannot_publish_completion) {
    counting_resource memory;
    {
        ruvia::detail::http3_client_sans_io_session_engine session_value(&memory);
        streamed_events observed_value(&memory);
        RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get,
                                     {.callback_ = collect_streamed_response, .context_ = &observed_value})
                        .scope_ ==
                    ruvia::http3_connection_error_scope::none);
        std::pmr::monotonic_buffer_resource scratch;
        constexpr std::array fields_value{ruvia::http3_field_section_field_view{":status", "200"},
            ruvia::http3_field_section_field_view{"content-length", "3"}};
        const auto encoded = ruvia::encode_http3_field_section(fields_value, &scratch);
        RUVIA_CHECK(session_value.feed(0, frame(1, std::get<0>(encoded))).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        const auto mismatch = session_value.feed(0, body("no"), true);
        RUVIA_CHECK(mismatch.scope_ == ruvia::http3_connection_error_scope::stream);
        RUVIA_CHECK(observed_value.body_ == "no" && observed_value.ends_ == 0);
        const auto failed = session_value.response(0);
        RUVIA_CHECK(failed && !failed->complete_ && failed->result_.scope_ == ruvia::http3_connection_error_scope::stream);
        RUVIA_CHECK(plan_matches(failed->response_body_plan_, ruvia::http_known_method::get, 200,
            ruvia::http_response_content_semantics::with_content, false));
        RUVIA_CHECK(session_value.release(0));
    }
    RUVIA_CHECK(memory.allocations_ == memory.deallocations_ && memory.live_bytes_ == 0);
}

RUVIA_TEST(http3_client_sans_io_session_streamed_cancellation_keeps_earlier_owned_result_alive) {
    counting_resource memory;
    {
        ruvia::detail::http3_client_sans_io_session_engine session_value(&memory);
        streamed_events earlier(&memory);
        streamed_events cancelled(&memory);
        RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get,
                                     {.callback_ = collect_streamed_response, .context_ = &earlier})
                        .scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.register_request(4, ruvia::http_known_method::get,
                                     {.callback_ = collect_streamed_response, .context_ = &cancelled})
                        .scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(0, response_head()).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(0, body("kept"), true).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::message_end);
        RUVIA_CHECK(session_value.feed(4, response_head()).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(4, body("temporary")).scope_ == ruvia::http3_connection_error_scope::none);
        // A transport owner must have terminated both QUIC stream directions
        // before this local parser retirement; no further bytes are fed to 4.
        RUVIA_CHECK(session_value.cancel_request(4));
        RUVIA_CHECK(session_value.response(4)->result_.status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::local_cancelled);
        RUVIA_CHECK(session_value.release(4));
        RUVIA_CHECK(earlier.body_ == "kept" && cancelled.body_ == "temporary");
        RUVIA_CHECK(session_value.response(0)->complete_ && session_value.release(0));
        RUVIA_CHECK(session_value.retained_body_bytes() == 0);
    }
    RUVIA_CHECK(memory.allocations_ > 0);
    RUVIA_CHECK(memory.allocations_ == memory.deallocations_ && memory.live_bytes_ == 0);
}

RUVIA_TEST(http3_client_sans_io_session_streamed_sink_exception_fails_entire_connection) {
    counting_resource memory;
    {
        ruvia::detail::http3_client_sans_io_session_engine session_value(&memory);
        streamed_events observed_value(&memory);
        observed_value.fail_body_ = true;
        RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get,
                                     {.callback_ = collect_streamed_response, .context_ = &observed_value})
                        .scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.register_request(4, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(0, response_head()).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        auto mixed = body("explode");
        const auto illegal = frame(4, {});  // SETTINGS on a response stream.
        mixed.insert(mixed.end(), illegal.begin(), illegal.end());
        {
            ruvia::detail::http3_client_sans_io_session_engine protocol_only(&memory);
            RUVIA_CHECK(protocol_only.register_request(0, ruvia::http_known_method::get).scope_ ==
                        ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(protocol_only.feed(0, response_head()).scope_ ==
                        ruvia::http3_connection_error_scope::none);
            const auto parsed_value = protocol_only.feed(0, mixed);
            RUVIA_CHECK(parsed_value.scope_ == ruvia::http3_connection_error_scope::connection);
            RUVIA_CHECK(parsed_value.code_ == ruvia::http3_connection_error_code::frame_unexpected);
            RUVIA_CHECK(protocol_only.release(0));
        }
        bool threw{};
        try {
            (void)session_value.feed(0, mixed);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        const auto failed = session_value.response(0);
        RUVIA_CHECK(failed && failed->result_.scope_ == ruvia::http3_connection_error_scope::connection &&
                    failed->result_.code_ == ruvia::http3_connection_error_code::internal_error);
        RUVIA_CHECK(plan_matches(failed->response_body_plan_, ruvia::http_known_method::get, 200,
            ruvia::http_response_content_semantics::with_content, false));
        RUVIA_CHECK(session_value.response(4)->result_.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(session_value.release(0) && session_value.release(4));
    }
    RUVIA_CHECK(memory.allocations_ == memory.deallocations_ && memory.live_bytes_ == 0);
}

RUVIA_TEST(http3_client_sans_io_session_propagates_connection_failure_to_live_streams) {
    std::pmr::monotonic_buffer_resource worker;
    ruvia::detail::http3_client_sans_io_session_engine session_value(&worker);
    RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get).status_ ==
                ruvia::detail::http3_client_sans_io_session_status::need_more_data);
    RUVIA_CHECK(session_value.register_request(4, ruvia::http_known_method::get).status_ ==
                ruvia::detail::http3_client_sans_io_session_status::need_more_data);
    const auto failure = session_value.feed(0, body("unexpected"));
    RUVIA_CHECK(failure.status_ == ruvia::detail::http3_client_sans_io_session_status::connection_error);
    RUVIA_CHECK(!session_value.response(0)->response_body_plan_);
    RUVIA_CHECK(!session_value.response(4)->response_body_plan_);
    RUVIA_CHECK(session_value.response(4)->result_.status_ ==
                ruvia::detail::http3_client_sans_io_session_status::connection_error);
    RUVIA_CHECK(session_value.release(0));
    RUVIA_CHECK(session_value.release(4));
}

RUVIA_TEST(http3_client_sans_io_session_feeds_peer_critical_streams_and_propagates_closure) {
    std::pmr::unsynchronized_pool_resource worker;
    ruvia::detail::http3_client_sans_io_session_engine session_value(&worker);
    RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get).status_ ==
                ruvia::detail::http3_client_sans_io_session_status::need_more_data);
    std::vector<char> control{0};
    const auto settings = frame(4, {});
    control.insert(control.end(), settings.begin(), settings.end());
    RUVIA_CHECK(session_value.feed(3, control).status_ ==
                ruvia::detail::http3_client_sans_io_session_status::need_more_data);
    RUVIA_CHECK(!session_value.response(3).has_value());
    const auto closed = session_value.feed(3, {}, true);
    RUVIA_CHECK(closed.status_ == ruvia::detail::http3_client_sans_io_session_status::connection_error);
    RUVIA_CHECK(closed.code_ == ruvia::http3_connection_error_code::closed_critical_stream);
    RUVIA_CHECK(session_value.response(0)->result_.status_ ==
                ruvia::detail::http3_client_sans_io_session_status::connection_error);
    RUVIA_CHECK(session_value.release(0));
}

RUVIA_TEST(http3_client_sans_io_session_bounds_streams_and_body_and_retains_reset) {
    std::pmr::monotonic_buffer_resource worker;
    ruvia::detail::http3_client_sans_io_session_engine::limits_type limits{};
    limits.max_live_streams_ = 1;
    limits.max_body_bytes_per_stream_ = 2;
    limits.max_total_body_bytes_ = 2;
    ruvia::detail::http3_client_sans_io_session_engine session_value(&worker, limits);
    RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get).status_ ==
                ruvia::detail::http3_client_sans_io_session_status::need_more_data);
    RUVIA_CHECK(session_value.register_request(4, ruvia::http_known_method::get).status_ ==
                ruvia::detail::http3_client_sans_io_session_status::stream_limit_exceeded);
    RUVIA_CHECK(session_value.feed(0, response_head()).status_ == ruvia::detail::http3_client_sans_io_session_status::need_more_data);
    const auto exceeded = session_value.feed(0, body("abc"));
    RUVIA_CHECK(exceeded.status_ == ruvia::detail::http3_client_sans_io_session_status::body_limit_exceeded);
    RUVIA_CHECK(exceeded.scope_ == ruvia::http3_connection_error_scope::connection);
    RUVIA_CHECK_EQ(session_value.retained_body_bytes(), 0U);
    RUVIA_CHECK(session_value.response(0)->result_.status_ ==
                ruvia::detail::http3_client_sans_io_session_status::body_limit_exceeded);
    RUVIA_CHECK(!session_value.response(0)->complete_);
    RUVIA_CHECK(session_value.release(0));
    RUVIA_CHECK(session_value.register_request(4, ruvia::http_known_method::get).status_ ==
                ruvia::detail::http3_client_sans_io_session_status::connection_error);

    ruvia::detail::http3_client_sans_io_session_engine reset_session(&worker, limits);
    RUVIA_CHECK(reset_session.register_request(4, ruvia::http_known_method::get).status_ ==
                ruvia::detail::http3_client_sans_io_session_status::need_more_data);
    RUVIA_CHECK(reset_session.feed(4, {}, false, true).status_ ==
                ruvia::detail::http3_client_sans_io_session_status::reset);
    RUVIA_CHECK(reset_session.response(4)->reset_);
    RUVIA_CHECK(!reset_session.response(4)->response_body_plan_);
    RUVIA_CHECK(reset_session.release(4));
}

RUVIA_TEST(http3_client_sans_io_session_body_limit_cannot_be_hidden_by_same_batch_length_mismatch) {
    counting_resource memory;
    {
        ruvia::detail::http3_client_sans_io_session_engine session_value(&memory,
            {.max_live_streams_ = 2, .max_body_bytes_per_stream_ = 2, .max_total_body_bytes_ = 2});
        std::pmr::monotonic_buffer_resource scratch;
        constexpr std::array fields_value{ruvia::http3_field_section_field_view{":status", "200"},
            ruvia::http3_field_section_field_view{"content-length", "4"}};
        const auto encoded = ruvia::encode_http3_field_section(fields_value, &scratch);
        const auto head = frame(1, std::get<0>(encoded));
        // Establish that the identical DATA+FIN would independently fail the
        // Content-Length contract; the body-limit check must not lose to it.
        {
            ruvia::detail::http3_client_sans_io_session_engine protocol_only(&memory);
            RUVIA_CHECK(protocol_only.register_request(0, ruvia::http_known_method::get).scope_ ==
                        ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(protocol_only.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
            const auto mismatch = protocol_only.feed(0, body("abc"), true);
            RUVIA_CHECK(mismatch.scope_ == ruvia::http3_connection_error_scope::stream);
            RUVIA_CHECK(mismatch.code_ == ruvia::http3_connection_error_code::message_error);
            RUVIA_CHECK(protocol_only.release(0));
        }
        RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.register_request(4, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
        const auto failure = session_value.feed(0, body("abc"), true);
        RUVIA_CHECK(failure.status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::body_limit_exceeded);
        RUVIA_CHECK(failure.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(session_value.response(4)->result_.scope_ ==
                    ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(session_value.release(0) && session_value.release(4));
        RUVIA_CHECK_EQ(session_value.retained_body_bytes(), 0U);

        ruvia::detail::http3_client_sans_io_session_engine protocol_priority(&memory,
            {.max_live_streams_ = 2, .max_body_bytes_per_stream_ = 2, .max_total_body_bytes_ = 2});
        RUVIA_CHECK(protocol_priority.register_request(0, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(protocol_priority.register_request(4, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(protocol_priority.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
        auto mixed = body("abc");
        const auto invalid = frame(4, {});  // Forbidden SETTINGS after a response head.
        mixed.insert(mixed.end(), invalid.begin(), invalid.end());
        const auto protocol_failure = protocol_priority.feed(0, mixed);
        RUVIA_CHECK(protocol_failure.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(protocol_failure.code_ == ruvia::http3_connection_error_code::frame_unexpected);
        RUVIA_CHECK(protocol_priority.release(0) && protocol_priority.release(4));
    }
    RUVIA_CHECK(memory.allocations_ == memory.deallocations_ && memory.live_bytes_ == 0);
}

RUVIA_TEST(http3_client_sans_io_session_aggregate_overflow_closes_only_incomplete_results) {
    counting_resource worker;
    {
        ruvia::detail::http3_client_sans_io_session_engine session_value(&worker,
            {.max_live_streams_ = 2, .max_body_bytes_per_stream_ = 4, .max_total_body_bytes_ = 3});
        const auto head = response_head();
        RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.register_request(4, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(4, head).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(0, body("ok"), true).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::message_end);
        const auto excess = session_value.feed(4, body("no"));
        RUVIA_CHECK(excess.status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::body_limit_exceeded);
        RUVIA_CHECK(excess.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(session_value.response(0)->complete_);
        RUVIA_CHECK(session_value.response(0)->result_.status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::message_end);
        RUVIA_CHECK(!session_value.response(4)->complete_);
        RUVIA_CHECK(session_value.response(4)->result_.status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::body_limit_exceeded);
        RUVIA_CHECK_EQ(session_value.retained_body_bytes(), 2U);
        RUVIA_CHECK(session_value.release(0));
        RUVIA_CHECK(session_value.release(4));
        RUVIA_CHECK_EQ(session_value.retained_body_bytes(), 0U);
    }
    RUVIA_CHECK_EQ(worker.allocations_, worker.deallocations_);
    RUVIA_CHECK_EQ(worker.live_bytes_, 0U);
}

RUVIA_TEST(http3_client_sans_io_session_shares_body_budget_with_retained_results_across_transfers) {
    counting_resource memory;
    {
        ruvia::detail::http3_client_body_budget budget(3072);
        std::pmr::string older(1024, 'z', &memory);
        RUVIA_CHECK(budget.try_retain(older.size()));
        const auto baseline = memory.live_bytes_;
        const std::string payload_value(2048, 'x');
        for (unsigned iteration = 0; iteration < 24; ++iteration) {
            std::optional<std::pmr::string> retained;
            {
                ruvia::detail::http3_client_sans_io_session_engine engine(&memory, budget);
                RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get).scope_ ==
                            ruvia::http3_connection_error_scope::none);
                RUVIA_CHECK(engine.feed(0, response_head()).scope_ == ruvia::http3_connection_error_scope::none);
                RUVIA_CHECK(engine.feed(0, body(payload_value), true).status_ ==
                            ruvia::detail::http3_client_sans_io_session_status::message_end);
                RUVIA_CHECK_EQ(budget.used(), std::size_t{3072});
                retained = engine.take_body(0);
                RUVIA_CHECK_EQ(budget.used(), older.size());
                RUVIA_CHECK(retained && budget.try_retain(retained->size()));
                RUVIA_CHECK(engine.release(0));
                RUVIA_CHECK(engine.register_request(4, ruvia::http_known_method::get).scope_ ==
                            ruvia::http3_connection_error_scope::none);
                RUVIA_CHECK(engine.feed(4, response_head()).scope_ == ruvia::http3_connection_error_scope::none);
                // No receive-owned body remains, but retained results exhaust
                // the same connection budget. Even one more body byte fails.
                const auto overflow = engine.feed(4, body("!"), true);
                RUVIA_CHECK(overflow.status_ == ruvia::detail::http3_client_sans_io_session_status::body_limit_exceeded);
                RUVIA_CHECK_EQ(budget.used(), std::size_t{3072});
                RUVIA_CHECK_EQ(engine.retained_body_bytes(), std::size_t{0});
                RUVIA_CHECK(engine.release(4));
            }
            RUVIA_CHECK(retained && std::string_view(*retained) == payload_value);
            const auto result_bytes = retained->size();
            retained.reset();
            budget.release(result_bytes);
            RUVIA_CHECK_EQ(memory.live_bytes_, baseline);
            RUVIA_CHECK_EQ(budget.used(), older.size());
            RUVIA_CHECK(older.front() == 'z');
        }
        budget.release(older.size());
    }
    RUVIA_CHECK_EQ(memory.live_bytes_, 0U);
    RUVIA_CHECK_EQ(memory.allocations_, memory.deallocations_);
}

RUVIA_TEST(http3_client_sans_io_session_body_budget_rolls_back_allocation_failure_and_releases_cancellation) {
    counting_resource memory;
    ruvia::detail::http3_client_body_budget budget(4096);
    const std::string payload_value(2048, 'x');
    for (const bool reject_allocation : {false, true}) {
        {
            ruvia::detail::http3_client_sans_io_session_engine engine(&memory, budget);
            RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get).scope_ ==
                        ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(engine.feed(0, response_head()).scope_ == ruvia::http3_connection_error_scope::none);
            memory.reject_ = reject_allocation;
            if (reject_allocation) {
                RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)engine.feed(0, body(payload_value)); }));
                RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
            } else {
                RUVIA_CHECK(engine.feed(0, body(payload_value)).scope_ == ruvia::http3_connection_error_scope::none);
                RUVIA_CHECK_EQ(budget.used(), payload_value.size());
                RUVIA_CHECK(engine.cancel_request(0));  // Transport is already retired by the test owner.
                RUVIA_CHECK_EQ(budget.used(), payload_value.size());
            }
            memory.reject_ = false;
            // Destruction, even without release(), must return all remaining
            // receive-owned reservations and PMR allocations.
        }
        RUVIA_CHECK_EQ(budget.used(), std::size_t{0});
        RUVIA_CHECK_EQ(memory.live_bytes_, 0U);
    }
    RUVIA_CHECK_EQ(memory.allocations_, memory.deallocations_);
}

RUVIA_TEST(http3_client_sans_io_session_transfers_completed_body_without_copy_and_reclaims_later_bodies) {
    counting_resource memory;
    {
        ruvia::detail::http3_client_sans_io_session_engine session_value(&memory);
        const auto head = response_head();
        const std::string payload_value(2048, 'x');
        RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(!session_value.take_body(0));
        RUVIA_CHECK(session_value.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(0, body(payload_value), true).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::message_end);
        const auto* original = session_value.response(0)->body_.data();
        const auto before_transfer = memory.body_allocations_;
        auto retained = session_value.take_body(0);
        RUVIA_CHECK(retained && retained->data() == original);
        RUVIA_CHECK_EQ(memory.body_allocations_, before_transfer);
        RUVIA_CHECK_EQ(session_value.retained_body_bytes(), 0U);
        RUVIA_CHECK(!session_value.take_body(0));
        RUVIA_CHECK(session_value.response(0)->headers_.front().value_ == "owned");
        RUVIA_CHECK(session_value.release(0));
        const auto baseline = memory.live_bytes_;
        for (std::uint64_t index = 1; index <= 24; ++index) {
            const auto id = index * 4;
            RUVIA_CHECK(session_value.register_request(id, ruvia::http_known_method::get).scope_ ==
                        ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(session_value.feed(id, head).scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(session_value.feed(id, body(payload_value), true).status_ ==
                        ruvia::detail::http3_client_sans_io_session_status::message_end);
            auto temporary = session_value.take_body(id);
            RUVIA_CHECK(temporary && std::string_view(*temporary) == payload_value);
            RUVIA_CHECK(session_value.release(id));
            temporary.reset();
            RUVIA_CHECK_EQ(memory.live_bytes_, baseline);
            RUVIA_CHECK(retained && std::string_view(*retained) == payload_value);
        }
        const auto with_result = memory.live_bytes_;
        retained.reset();
        RUVIA_CHECK(memory.live_bytes_ < with_result);
    }
    RUVIA_CHECK_EQ(memory.live_bytes_, 0U);
    RUVIA_CHECK_EQ(memory.allocations_, memory.deallocations_);
}

RUVIA_TEST(http3_client_sans_io_session_returns_each_retired_body_while_other_result_survives) {
    counting_resource worker;
    {
        ruvia::detail::http3_client_sans_io_session_engine session_value(&worker);
        const auto head = response_head();
        const auto held = body("held");
        const auto repeated = body("next");
        RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(0, held, true).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::message_end);
        const auto retained = session_value.response(0);
        RUVIA_CHECK(retained && std::string_view(retained->body_.data(), retained->body_.size()) == "held");
        std::size_t baseline{};
        for (std::uint64_t i = 1; i != 48; ++i) {
            const auto id = i * 4;
            RUVIA_CHECK(session_value.register_request(id, ruvia::http_known_method::get).scope_ ==
                        ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(session_value.feed(id, head).scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(session_value.feed(id, repeated, true).status_ ==
                        ruvia::detail::http3_client_sans_io_session_status::message_end);
            RUVIA_CHECK_EQ(session_value.retained_body_bytes(), 8U);
            RUVIA_CHECK(session_value.release(id));
            RUVIA_CHECK_EQ(session_value.retained_body_bytes(), 4U);
            RUVIA_CHECK(std::string_view(retained->body_.data(), retained->body_.size()) == "held");
            if (i == 1) {
                baseline = worker.live_bytes_;
            } else {
                RUVIA_CHECK_EQ(worker.live_bytes_, baseline);
            }
        }
        RUVIA_CHECK(session_value.release(0));
        RUVIA_CHECK_EQ(session_value.retained_body_bytes(), 0U);
    }
    RUVIA_CHECK(worker.allocations_ > 0);
    RUVIA_CHECK_EQ(worker.allocations_, worker.deallocations_);
    RUVIA_CHECK_EQ(worker.live_bytes_, 0U);
}

RUVIA_TEST(http3_client_sans_io_session_owns_validated_response_trailers_until_explicit_release) {
    counting_resource worker;
    {
        ruvia::detail::http3_client_sans_io_session_engine session_value(&worker);
        RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        const auto head = response_head();
        RUVIA_CHECK(session_value.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(0, body("ok")).scope_ == ruvia::http3_connection_error_scope::none);
        auto trailers = response_trailers();
        RUVIA_CHECK(session_value.feed(0, trailers, true).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::message_end);
        std::fill(trailers.begin(), trailers.end(), '\0');
        const auto response = session_value.response(0);
        RUVIA_CHECK(response && response->complete_ && response->trailers_.size() == 2);
        RUVIA_CHECK(response && response->trailers_[0].name_ == "x-one" &&
                    response->trailers_[0].value_ == "first");
        RUVIA_CHECK(response && response->trailers_[1].name_ == "x-two" &&
                    response->trailers_[1].value_ == "second");
        RUVIA_CHECK(session_value.release(0));
        RUVIA_CHECK_EQ(session_value.retained_body_bytes(), 0U);
    }
    RUVIA_CHECK_EQ(worker.allocations_, worker.deallocations_);
    RUVIA_CHECK_EQ(worker.live_bytes_, 0U);
}

RUVIA_TEST(http3_client_sans_io_session_local_cancellation_isolates_other_responses_and_reclaims_storage) {
    counting_resource worker;
    {
        ruvia::detail::http3_client_sans_io_session_engine session_value(&worker);
        RUVIA_CHECK(!session_value.cancel_request(0));  // cold request was never registered
        RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.register_request(4, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        const auto head = response_head();
        RUVIA_CHECK(session_value.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(0, body("abandoned")).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(4, head).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(4, body("retained"), true).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::message_end);
        RUVIA_CHECK_EQ(session_value.retained_body_bytes(), 17U);
        RUVIA_CHECK(session_value.cancel_request(0));
        RUVIA_CHECK(!session_value.cancel_request(0));
        RUVIA_CHECK(!session_value.cancel_request(4));  // a completed response is not cancelled
        const auto cancelled = session_value.response(0);
        RUVIA_CHECK(cancelled && !cancelled->complete_ && !cancelled->reset_);
        RUVIA_CHECK(cancelled && cancelled->result_.status_ ==
                                     ruvia::detail::http3_client_sans_io_session_status::local_cancelled);
        RUVIA_CHECK(cancelled && cancelled->result_.scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(cancelled && std::string_view(cancelled->body_.data(), cancelled->body_.size()) == "abandoned");
        const auto allocated_before_release = worker.live_bytes_;
        RUVIA_CHECK(session_value.release(0));
        RUVIA_CHECK(worker.live_bytes_ < allocated_before_release);
        RUVIA_CHECK_EQ(session_value.retained_body_bytes(), 8U);
        RUVIA_CHECK(session_value.register_request(8, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(8, head).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(8, body("next"), true).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::message_end);
        RUVIA_CHECK(std::string_view(session_value.response(4)->body_.data(), session_value.response(4)->body_.size()) ==
                    "retained");
        RUVIA_CHECK(std::string_view(session_value.response(8)->body_.data(), session_value.response(8)->body_.size()) ==
                    "next");
        RUVIA_CHECK(session_value.release(4));
        RUVIA_CHECK(session_value.release(8));
        RUVIA_CHECK_EQ(session_value.retained_body_bytes(), 0U);
    }
    RUVIA_CHECK_EQ(worker.allocations_, worker.deallocations_);
    RUVIA_CHECK_EQ(worker.live_bytes_, 0U);
}

RUVIA_TEST(http3_client_sans_io_session_repeated_cancelled_streams_return_to_baseline_with_earlier_result_alive) {
    counting_resource worker;
    {
        ruvia::detail::http3_client_sans_io_session_engine session_value(&worker);
        const auto head = response_head();
        RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(0, body("keep"), true).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::message_end);
        const auto baseline = worker.live_bytes_;
        for (std::uint64_t stream_id = 4; stream_id <= 128; stream_id += 4) {
            RUVIA_CHECK(session_value.register_request(stream_id, ruvia::http_known_method::get).scope_ ==
                        ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(session_value.feed(stream_id, head).scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(session_value.feed(stream_id, body("drop")).scope_ == ruvia::http3_connection_error_scope::none);
            RUVIA_CHECK(session_value.cancel_request(stream_id));
            RUVIA_CHECK(session_value.release(stream_id));
            RUVIA_CHECK_EQ(worker.live_bytes_, baseline);
            RUVIA_CHECK_EQ(session_value.retained_body_bytes(), 4U);
            const auto retained = session_value.response(0);
            RUVIA_CHECK(retained && retained->complete_ &&
                        std::string_view(retained->body_.data(), retained->body_.size()) == "keep");
        }
        RUVIA_CHECK(session_value.release(0));
        RUVIA_CHECK_EQ(session_value.retained_body_bytes(), 0U);
    }
    RUVIA_CHECK_EQ(worker.allocations_, worker.deallocations_);
    RUVIA_CHECK_EQ(worker.live_bytes_, 0U);
}

RUVIA_TEST(http3_client_sans_io_session_exposes_peer_admission_and_goaway_cutoff) {
    counting_resource worker;
    {
        ruvia::detail::http3_client_sans_io_session_engine session_value(&worker);
        RUVIA_CHECK(!session_value.peer_settings());
        RUVIA_CHECK(!session_value.peer_goaway_id());
        RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.register_request(4, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        constexpr std::array<char, 6> control{0x00, 0x04, 0x00, 0x07, 0x01, 0x04};
        RUVIA_CHECK(session_value.feed(3, control).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.peer_settings().has_value());
        RUVIA_CHECK(session_value.peer_goaway_id() == 4);
        // Already-open request 4 remains visible: a connection driver can
        // distinguish the GOAWAY cutoff from an arbitrary transport failure.
        RUVIA_CHECK_EQ(session_value.live_stream_count(), 2U);
        RUVIA_CHECK(!session_value.response(4));
        const auto rejected = session_value.register_request(8, ruvia::http_known_method::get);
        RUVIA_CHECK(rejected.scope_ == ruvia::http3_connection_error_scope::stream);
        RUVIA_CHECK(rejected.code_ == ruvia::http3_connection_error_code::request_rejected);
        RUVIA_CHECK_EQ(session_value.live_stream_count(), 2U);
        RUVIA_CHECK(session_value.stop().scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(!session_value.peer_settings() && !session_value.peer_goaway_id());
        RUVIA_CHECK(session_value.release(0));
        RUVIA_CHECK(session_value.release(4));
    }
    RUVIA_CHECK_EQ(worker.allocations_, worker.deallocations_);
    RUVIA_CHECK_EQ(worker.live_bytes_, 0U);
}

RUVIA_TEST(http3_client_sans_io_session_local_stop_preserves_only_already_complete_responses) {
    counting_resource worker;
    {
        ruvia::detail::http3_client_sans_io_session_engine session_value(&worker);
        const auto head = response_head();
        RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.register_request(4, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(0, head).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(4, head).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.feed(0, body("ok"), true).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::message_end);
        const auto retained = session_value.response(0);
        const auto returns_before_stop = worker.deallocations_;
        const auto stopped = session_value.stop();
        RUVIA_CHECK(worker.deallocations_ > returns_before_stop);
        RUVIA_CHECK(retained && std::string_view(retained->body_.data(), retained->body_.size()) == "ok");
        const auto live_after_stop = worker.live_bytes_;
        RUVIA_CHECK(session_value.stop().status_ == stopped.status_);
        // Repeated stop can create and release empty debug iterator proxies;
        // it must preserve the retained response and its live storage.
        RUVIA_CHECK_EQ(worker.live_bytes_, live_after_stop);
        RUVIA_CHECK(retained && std::string_view(retained->body_.data(), retained->body_.size()) == "ok");
        RUVIA_CHECK(stopped.status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::transport_error);
        RUVIA_CHECK(stopped.scope_ == ruvia::http3_connection_error_scope::connection);
        RUVIA_CHECK(stopped.code_ == ruvia::http3_connection_error_code::no_error);
        RUVIA_CHECK(session_value.response(0)->complete_);
        RUVIA_CHECK(session_value.response(0)->result_.status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::message_end);
        RUVIA_CHECK(!session_value.response(4)->complete_);
        RUVIA_CHECK(session_value.response(4)->result_.status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::transport_error);
        RUVIA_CHECK(session_value.feed(4, {}, true).status_ ==
                    ruvia::detail::http3_client_sans_io_session_status::transport_error);
        RUVIA_CHECK(session_value.release(0));
        RUVIA_CHECK(session_value.release(4));
        RUVIA_CHECK_EQ(session_value.retained_body_bytes(), 0U);
    }
    RUVIA_CHECK_EQ(worker.allocations_, worker.deallocations_);
    RUVIA_CHECK_EQ(worker.live_bytes_, 0U);
}

RUVIA_TEST(http3_client_sans_io_session_allocation_failure_terminates_every_live_stream) {
    counting_resource worker;
    {
        ruvia::detail::http3_client_sans_io_session_engine session_value(&worker);
        RUVIA_CHECK(session_value.register_request(0, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(session_value.register_request(4, ruvia::http_known_method::get).scope_ ==
                    ruvia::http3_connection_error_scope::none);
        const auto head = response_head();
        worker.reject_ = true;
        bool caught{};
        try {
            (void)session_value.feed(0, head);
        } catch (const std::bad_alloc&) {
            caught = true;
        }
        worker.reject_ = false;
        RUVIA_CHECK(caught);
        RUVIA_CHECK(session_value.response(0)->result_.code_ == ruvia::http3_connection_error_code::internal_error);
        RUVIA_CHECK(session_value.response(4)->result_.code_ == ruvia::http3_connection_error_code::internal_error);
        RUVIA_CHECK(session_value.release(0));
        RUVIA_CHECK(session_value.release(4));
    }
    RUVIA_CHECK_EQ(worker.allocations_, worker.deallocations_);
    RUVIA_CHECK_EQ(worker.live_bytes_, 0U);
}

RUVIA_TEST(http3_client_push_events_associate_out_of_order_streams_and_control_frames_commit_once) {
    counting_resource memory;
    using engine_type = ruvia::detail::http3_client_sans_io_session_engine;
    struct observed_type final {
        std::vector<ruvia::http3_connection_event_kind> events_;
        std::string path_;
        std::string body_;
        std::uint64_t stream_{};
        std::uint64_t push_{};
        bool reentry_rejected_{};
        engine_type* engine_{};
    } observed;
    {
        engine_type client(&memory);
        ruvia::http3_connection server(ruvia::http3_peer_role::server, &memory);
        observed.engine_ = &client;
        client.observe_pushes([](void* raw, const ruvia::http3_connection_event& event) {
            auto& state_value = *static_cast<observed_type*>(raw);
            state_value.events_.push_back(event.kind_);
            state_value.push_ = *event.push_id_;
            state_value.stream_ = event.stream_id_;
            if (event.kind_ == ruvia::http3_connection_event_kind::push_promise) {
                state_value.path_ = event.head_->path_;
                state_value.reentry_rejected_ = !state_value.engine_->queue_cancel_push(*event.push_id_) &&
                                                !state_value.engine_->queue_max_push_id(100) && !state_value.engine_->retire_push_stream(3);
            }
            if (event.kind_ == ruvia::http3_connection_event_kind::body) {
                state_value.body_.append(event.body_.data(), event.body_.size());
            }
        },
            &observed);
        RUVIA_CHECK(client.register_request(0, ruvia::http_known_method::get).scope_ == ruvia::http3_connection_error_scope::none);
        memory.reject_ = true;
        bool failed = false;
        try {
            (void)client.queue_max_push_id(0);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        memory.reject_ = false;
        RUVIA_CHECK(failed);
        RUVIA_CHECK(client.pending_control_output().empty());
        RUVIA_CHECK(client.queue_max_push_id(0));
        const char prefix[]{0, 4, 0};
        RUVIA_CHECK(server.feed(2, prefix, false, false, [](void*, const ruvia::http3_connection_event&) {}, nullptr).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(server.feed(2, client.pending_control_output(), false, false, [](void*, const ruvia::http3_connection_event&) {}, nullptr).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(server.peer_max_push_id() == 0);
        RUVIA_CHECK(client.consume_control_output(client.pending_control_output().size()));
        auto promise = server.prepare_push_promise(0, 0, {.authority_ = "example.test", .path_ = "/asset"});
        RUVIA_CHECK((promise.index() == 0));
        const char push_prefix[]{1, 0};
        auto pending = client.feed(3, push_prefix);
        RUVIA_CHECK(pending.status_ == ruvia::detail::http3_client_sans_io_session_status::push_promise_pending);
        RUVIA_CHECK(observed.events_.size() == 1 && observed.events_[0] == ruvia::http3_connection_event_kind::push_stream);
        RUVIA_CHECK(observed.stream_ == 3 && observed.push_ == 0);
        RUVIA_CHECK(client.feed(0, std::get<0>(promise)).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(observed.path_ == "/asset" && observed.reentry_rejected_);
        auto wire = response_head("200", "5");
        auto payload_value = body("asset");
        wire.insert(wire.end(), payload_value.begin(), payload_value.end());
        RUVIA_CHECK(client.feed(3, wire, true).status_ == ruvia::detail::http3_client_sans_io_session_status::message_end);
        RUVIA_CHECK(observed.body_ == "asset");
        RUVIA_CHECK(client.retire_push_stream(3));
        RUVIA_CHECK(client.queue_push_priority_update(0, {.urgency_ = 1, .incremental_ = true}));
        RUVIA_CHECK(client.queue_max_push_id(1));
        RUVIA_CHECK(client.consume_control_output(client.pending_control_output().size()));
        RUVIA_CHECK(client.queue_cancel_push(0));
        RUVIA_CHECK(client.pending_control_output().size() == 3);
        RUVIA_CHECK(client.consume_control_output(3));
        RUVIA_CHECK(client.pending_control_output().empty());
        const auto live_before_repeated_output = memory.live_bytes_;
        for (std::uint64_t maximum = 2; maximum < 102; ++maximum) {
            RUVIA_CHECK(client.queue_max_push_id(maximum));
            RUVIA_CHECK(client.consume_control_output(client.pending_control_output().size()));
            RUVIA_CHECK(memory.live_bytes_ == live_before_repeated_output);
        }
        RUVIA_CHECK(client.cancel_request(0));
        RUVIA_CHECK(client.release(0));
    }
    RUVIA_CHECK(memory.live_bytes_ == 0 && memory.allocations_ == memory.deallocations_);
}
