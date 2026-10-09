#include <array>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_local_critical_streams.h"

#include "client/http_client_upload_state.h"
#include "http3/http3_client_request_driver.h"
#include "http3/http3_client_sans_io_session_engine.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
using driver_type = ruvia::detail::http3_client_request_driver;

std::optional<ruvia::detail::http3_client_request_write> make_request(
    std::pmr::memory_resource* pool, std::string_view body = "payload") {
    ruvia::detail::http_client_request_storage storage("POST", "/upload", pool);
    storage.set_body(body);
    auto created = ruvia::detail::http3_client_request_write::create(
        std::move(storage), "https", "example.com", pool);
    if ((created.index() != 0)) {
        return std::nullopt;
    }
    return std::move(std::get<0>(created));
}

struct received final {
    std::string method_;
    std::string path_;
    std::string body_;
    int finished_{};
    std::string trailer_;
};

void receive(void* context_value, const ruvia::http3_connection_event& event) {
    auto& received_value = *static_cast<received*>(context_value);
    switch (event.kind_) {
        case ruvia::http3_connection_event_kind::request_head:
            received_value.method_ = event.head_->method_;
            received_value.path_ = event.head_->path_;
            break;
        case ruvia::http3_connection_event_kind::body:
            received_value.body_.append(event.body_.data(), event.body_.size());
            break;
        case ruvia::http3_connection_event_kind::trailer_field:
            received_value.trailer_ = std::string(event.trailer_.name_) + ":" + std::string(event.trailer_.value_);
            break;
        case ruvia::http3_connection_event_kind::message_end:
            ++received_value.finished_;
            break;
        default:
            break;
    }
}
}  // namespace

RUVIA_TEST(http3_client_request_driver_rebuilds_rejected_early_request_on_fresh_stream) {
    std::pmr::unsynchronized_pool_resource pool;
    ruvia::detail::http_client_request_storage storage("GET", "/safe", &pool);
    auto created = ruvia::detail::http3_client_request_write::create(
        std::move(storage), "https", "example.com", &pool);
    RUVIA_CHECK((created.index() == 0));
    if ((created.index() != 0)) {
        return;
    }
    driver_type driver(std::move(std::get<0>(created)));
    std::array<std::string, 2> wires;
    std::size_t attempt_value{};
    std::size_t registrations{};
    auto open = [&] {
        return ruvia::quic_stream_open_result{
            .status_ = ruvia::quic_operation_status::accepted,
            .stream_id_ = attempt_value == 0 ? 0U : 4U};
    };
    auto register_response = [&](std::uint64_t id, ruvia::http_known_method method) {
        RUVIA_CHECK_EQ(id, attempt_value == 0 ? 0U : 4U);
        RUVIA_CHECK(method == ruvia::http_known_method::get);
        ++registrations;
        return true;
    };
    auto write = [&](std::uint64_t, std::span<const char> bytes_value) {
        wires[attempt_value].append(bytes_value.data(), bytes_value.size());
        return ruvia::quic_stream_write_result{
            .status_ = ruvia::quic_operation_status::accepted, .accepted_ = bytes_value.size()};
    };
    auto finish_value = [](std::uint64_t) { return ruvia::quic_operation_status::accepted; };
    for (int i = 0; i < 8 && !driver.finished(); ++i) {
        RUVIA_CHECK(driver.drive(open, register_response, write, finish_value) != driver_type::result_type::fatal);
    }
    RUVIA_CHECK(driver.finished());
    RUVIA_CHECK_EQ(registrations, 1U);
    RUVIA_CHECK(driver.replay_after_rejected_early_stream("https", "example.com", &pool));
    attempt_value = 1;
    for (int i = 0; i < 8 && !driver.finished(); ++i) {
        RUVIA_CHECK(driver.drive(open, register_response, write, finish_value) != driver_type::result_type::fatal);
    }
    RUVIA_CHECK(driver.finished());
    RUVIA_CHECK_EQ(driver.stream_id(), std::optional<driver_type::stream_id_type>{4});
    RUVIA_CHECK_EQ(registrations, 2U);
    RUVIA_CHECK(!wires[0].empty() && wires[0] == wires[1]);
    ruvia::http3_connection peer(ruvia::http3_peer_role::server, &pool);
    received received;
    RUVIA_CHECK(peer.feed(4, wires[1], true, false, receive, &received).status_ ==
                ruvia::http3_connection_status::message_end);
    RUVIA_CHECK(received.method_ == "GET" && received.path_ == "/safe");
    RUVIA_CHECK_EQ(received.finished_, 1);
}

RUVIA_TEST(http3_client_request_driver_retries_exact_want_bytes_and_finishes_after_payload) {
    std::pmr::unsynchronized_pool_resource pool;
    auto created = make_request(&pool);
    RUVIA_CHECK(created.has_value());
    if (!created) {
        return;
    }
    driver_type driver(std::move(*created));
    std::string wire;
    std::string pending_bytes;
    const char* pending_address = nullptr;
    std::size_t pending_size{};
    int open_attempts{};
    int registrations{};
    int finish_attempts{};
    bool accepted_initial_part{};
    bool want_issued{};

    auto open = [&]() -> ruvia::quic_stream_open_result {
        ++open_attempts;
        if (open_attempts == 1) {
            return {.status_ = ruvia::quic_operation_status::would_block};
        }
        return {.status_ = ruvia::quic_operation_status::accepted, .stream_id_ = 0};
    };
    auto register_response = [&](std::uint64_t id, ruvia::http_known_method method) {
        RUVIA_CHECK_EQ(id, 0U);
        RUVIA_CHECK(method == ruvia::http_known_method::post);
        ++registrations;
        RUVIA_CHECK(wire.empty());
        return true;
    };
    auto write = [&](std::uint64_t id, std::span<const char> offered) -> ruvia::quic_stream_write_result {
        RUVIA_CHECK_EQ(id, 0U);
        RUVIA_CHECK_EQ(registrations, 1);
        if (!accepted_initial_part) {
            wire.push_back(offered.front());
            accepted_initial_part = true;
            return {.status_ = ruvia::quic_operation_status::accepted, .accepted_ = 1};
        }
        if (!want_issued) {
            pending_address = offered.data();
            pending_size = offered.size();
            pending_bytes.assign(offered.data(), offered.size());
            want_issued = true;
            return {.status_ = ruvia::quic_operation_status::would_block};
        }
        if (pending_address) {
            RUVIA_CHECK(offered.data() == pending_address);
            RUVIA_CHECK_EQ(offered.size(), pending_size);
            RUVIA_CHECK(std::string_view(offered.data(), offered.size()) == pending_bytes);
            pending_address = nullptr;
        }
        wire.append(offered.data(), offered.size());
        return {.status_ = ruvia::quic_operation_status::accepted, .accepted_ = offered.size()};
    };
    auto finish_value = [&](std::uint64_t id) {
        RUVIA_CHECK_EQ(id, 0U);
        ++finish_attempts;
        RUVIA_CHECK(!wire.empty());
        return finish_attempts == 1 ? ruvia::quic_operation_status::would_block : ruvia::quic_operation_status::accepted;
    };
    RUVIA_CHECK(driver.drive(open, register_response, write, finish_value) == driver_type::result_type::blocked);
    RUVIA_CHECK(!driver.stream_id());
    for (int i = 0; i < 128 && !driver.finished(); ++i) {
        RUVIA_CHECK(driver.drive(open, register_response, write, finish_value) != driver_type::result_type::fatal);
    }
    RUVIA_CHECK(driver.finished());
    RUVIA_CHECK_EQ(open_attempts, 2);
    RUVIA_CHECK_EQ(registrations, 1);
    RUVIA_CHECK_EQ(finish_attempts, 2);
    RUVIA_CHECK(want_issued && pending_address == nullptr);

    ruvia::http3_connection peer(ruvia::http3_peer_role::server, &pool);
    received received;
    const auto result_value = peer.feed(0, wire, true, false, receive, &received);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_connection_status::message_end);
    RUVIA_CHECK(received.method_ == "POST" && received.path_ == "/upload");
    RUVIA_CHECK(received.body_ == "payload" && received.finished_ == 1);
}

RUVIA_TEST(http3_client_request_driver_consumes_every_frame_across_single_byte_acknowledgements) {
    std::pmr::unsynchronized_pool_resource pool;
    auto created = make_request(&pool, "repeated");
    RUVIA_CHECK(created.has_value());
    if (!created) {
        return;
    }
    driver_type driver(std::move(*created));
    std::string wire;
    std::string pending;
    const char* pending_address = nullptr;
    int registrations{};
    int wants{};
    int accepted{};
    auto open = [] { return ruvia::quic_stream_open_result{.status_ = ruvia::quic_operation_status::accepted, .stream_id_ = 0}; };
    auto register_response = [&](std::uint64_t, ruvia::http_known_method method) {
        RUVIA_CHECK(method == ruvia::http_known_method::post);
        ++registrations;
        return true;
    };
    auto write = [&](std::uint64_t, std::span<const char> offered) -> ruvia::quic_stream_write_result {
        if (pending_address == nullptr) {
            pending_address = offered.data();
            pending.assign(offered.data(), offered.size());
            ++wants;
            return {.status_ = ruvia::quic_operation_status::would_block};
        }
        RUVIA_CHECK(offered.data() == pending_address);
        RUVIA_CHECK(std::string_view(offered.data(), offered.size()) == pending);
        pending_address = nullptr;
        wire.push_back(offered.front());
        ++accepted;
        return {.status_ = ruvia::quic_operation_status::accepted, .accepted_ = 1};
    };
    auto finish_value = [](std::uint64_t) { return ruvia::quic_operation_status::accepted; };
    for (int i = 0; i < 512 && !driver.finished() && !driver.failed(); ++i) {
        (void)driver.drive(open, register_response, write, finish_value);
    }
    RUVIA_CHECK(driver.finished() && !driver.failed());
    RUVIA_CHECK_EQ(registrations, 1);
    RUVIA_CHECK_EQ(wants, accepted);
    RUVIA_CHECK(pending_address == nullptr);
    ruvia::http3_connection peer(ruvia::http3_peer_role::server, &pool);
    received received;
    const auto result_value = peer.feed(0, wire, true, false, receive, &received);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_connection_status::message_end);
    RUVIA_CHECK(received.body_ == "repeated" && received.finished_ == 1);
}

RUVIA_TEST(http3_client_request_driver_can_defer_unopened_request_to_fresh_connection) {
    std::pmr::unsynchronized_pool_resource pool;
    auto created = make_request(&pool, "fresh");
    RUVIA_CHECK(created.has_value());
    if (!created) {
        return;
    }
    driver_type driver(std::move(*created));
    int registrations{};
    std::string wire;
    auto register_response = [&](std::uint64_t, ruvia::http_known_method) {
        ++registrations;
        return true;
    };
    auto write = [&](std::uint64_t id, std::span<const char> bytes_value) {
        RUVIA_CHECK_EQ(id, 8U);
        wire.append(bytes_value.data(), bytes_value.size());
        return ruvia::quic_stream_write_result{.status_ = ruvia::quic_operation_status::accepted,
            .accepted_ = bytes_value.size()};
    };
    auto finish_value = [](std::uint64_t) { return ruvia::quic_operation_status::accepted; };
    RUVIA_CHECK(driver.drive([] { return ruvia::quic_stream_open_result{
                                      .status_ = ruvia::quic_operation_status::draining}; },
                    register_response, write, finish_value) == driver_type::result_type::connection_draining);
    RUVIA_CHECK(!driver.stream_id() && !driver.failed());
    RUVIA_CHECK_EQ(registrations, 0);
    RUVIA_CHECK(wire.empty());
    auto request = driver.take_request_after_retirement();
    RUVIA_CHECK(request && request->body() == "fresh");
    RUVIA_CHECK(driver.failed() && !driver.take_request_after_retirement());
    auto rebuilt = ruvia::detail::http3_client_request_write::create(
        std::move(*request), "https", "example.com", &pool);
    RUVIA_CHECK((rebuilt.index() == 0));
    driver_type replacement(std::move(std::get<0>(rebuilt)));
    for (int i = 0; i < 32 && !replacement.finished(); ++i) {
        RUVIA_CHECK(replacement.drive([] { return ruvia::quic_stream_open_result{.status_ = ruvia::quic_operation_status::accepted, .stream_id_ = 8}; },
                        register_response, write, finish_value) != driver_type::result_type::fatal);
    }
    RUVIA_CHECK(replacement.finished());
    RUVIA_CHECK_EQ(registrations, 1);
    ruvia::http3_connection peer(ruvia::http3_peer_role::server, &pool);
    received received;
    RUVIA_CHECK(peer.feed(8, wire, true, false, receive, &received).status_ ==
                ruvia::http3_connection_status::message_end);
    RUVIA_CHECK(received.body_ == "fresh" && received.finished_ == 1);
}

RUVIA_TEST(http3_client_request_driver_never_writes_before_response_registration) {
    std::pmr::unsynchronized_pool_resource pool;
    auto created = make_request(&pool);
    RUVIA_CHECK(created.has_value());
    if (!created) {
        return;
    }
    driver_type driver(std::move(*created));
    int writes{};
    auto result_value = driver.drive([] { return ruvia::quic_stream_open_result{.status_ = ruvia::quic_operation_status::accepted, .stream_id_ = 4}; },
        [](std::uint64_t, ruvia::http_known_method) { return false; },
        [&](std::uint64_t, std::span<const char>) -> ruvia::quic_stream_write_result {
            ++writes;
            return {.status_ = ruvia::quic_operation_status::accepted, .accepted_ = 1};
        },
        [](std::uint64_t) { return ruvia::quic_operation_status::accepted; });
    RUVIA_CHECK(result_value == driver_type::result_type::fatal);
    RUVIA_CHECK(driver.failed());
    RUVIA_CHECK(driver.stream_id() == 4);
    RUVIA_CHECK_EQ(writes, 0);
}

RUVIA_TEST(http3_client_request_driver_writes_explicit_head_content_but_forbids_response_payload) {
    std::pmr::unsynchronized_pool_resource pool;
    ruvia::detail::http_client_request_storage storage("HEAD", "/resource", &pool);
    storage.set_body("explicit-content");
    auto created = ruvia::detail::http3_client_request_write::create(
        std::move(storage), "https", "example.com", &pool);
    RUVIA_CHECK((created.index() == 0));
    if ((created.index() != 0)) {
        return;
    }
    driver_type driver(std::move(std::get<0>(created)));
    std::string wire;
    ruvia::http3_connection responses_value(ruvia::http3_peer_role::client, &pool);
    auto register_response = [&](std::uint64_t id, ruvia::http_known_method method) {
        RUVIA_CHECK(method == ruvia::http_known_method::head);
        return responses_value.register_client_request(id, method).scope_ == ruvia::http3_connection_error_scope::none;
    };
    auto write = [&](std::uint64_t, std::span<const char> offered) {
        wire.append(offered.data(), offered.size());
        return ruvia::quic_stream_write_result{.status_ = ruvia::quic_operation_status::accepted,
            .accepted_ = offered.size()};
    };
    for (int i = 0; i < 16 && !driver.finished(); ++i) {
        RUVIA_CHECK(driver.drive([] { return ruvia::quic_stream_open_result{.status_ = ruvia::quic_operation_status::accepted, .stream_id_ = 0}; },
                        register_response, write,
                        [](std::uint64_t) { return ruvia::quic_operation_status::accepted; }) !=
                    driver_type::result_type::fatal);
    }
    RUVIA_CHECK(driver.finished());
    ruvia::http3_connection peer(ruvia::http3_peer_role::server, &pool);
    received received;
    const auto result_value = peer.feed(0, wire, true, false, receive, &received);
    RUVIA_CHECK(result_value.status_ == ruvia::http3_connection_status::message_end);
    RUVIA_CHECK(received.method_ == "HEAD" && received.body_ == "explicit-content");
    // A HEAD request body must not weaken the independently registered
    // response rules: HEADERS(:status=200) followed by DATA("x") is invalid.
    constexpr std::array<char, 8> invalid_response{1, 3, 0, 0, static_cast<char>(0xd9), 0, 1, 'x'};
    const auto response = responses_value.feed(0, invalid_response, true, false, [](void*, const ruvia::http3_connection_event&) {}, nullptr);
    RUVIA_CHECK(response.scope_ == ruvia::http3_connection_error_scope::stream);
    RUVIA_CHECK(response.code_ == ruvia::http3_connection_error_code::message_error);
}

RUVIA_TEST(http3_client_request_driver_fin_failure_cannot_commit_finish) {
    std::pmr::unsynchronized_pool_resource pool;
    auto created = make_request(&pool, "");
    RUVIA_CHECK(created.has_value());
    if (!created) {
        return;
    }
    driver_type driver(std::move(*created));
    auto open = [] { return ruvia::quic_stream_open_result{.status_ = ruvia::quic_operation_status::accepted, .stream_id_ = 0}; };
    auto register_response = [](std::uint64_t, ruvia::http_known_method) { return true; };
    auto write = [](std::uint64_t, std::span<const char> offered) {
        return ruvia::quic_stream_write_result{.status_ = ruvia::quic_operation_status::accepted,
            .accepted_ = offered.size()};
    };
    auto finish_value = [](std::uint64_t) { return ruvia::quic_operation_status::closing; };
    for (int i = 0; i < 16 && !driver.failed(); ++i) {
        (void)driver.drive(open, register_response, write, finish_value);
    }
    RUVIA_CHECK(driver.failed() && !driver.finished());
}

RUVIA_TEST(http3_client_request_driver_streams_bounded_chunks_and_trailing_headers_after_continue_gate) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    auto run = [&]() -> ruvia::task<void> {
        std::pmr::unsynchronized_pool_resource pool;
        const auto worker_value = attachment.loop().handle();
        ruvia::detail::http_client_upload_state upload(worker_value, &pool,
            {.content_length_ = 6, .expectation_ = ruvia::http_client_request_expectation::continue_value});
        ruvia::detail::http_client_request_storage storage("POST", "/upload", &pool);
        storage.bind_upload(upload);
        auto created = ruvia::detail::http3_client_request_write::create(std::move(storage), "https", "example.test", &pool);
        RUVIA_CHECK((created.index() == 0));
        if ((created.index() != 0)) {
            attachment.stop();
            co_return;
        }
        driver_type driver(std::move(std::get<0>(created)));
        std::string wire;
        auto open = [] { return ruvia::quic_stream_open_result{.status_ = ruvia::quic_operation_status::accepted, .stream_id_ = 0}; };
        auto register_response = [](auto, auto) { return true; };
        auto write = [&](auto, std::span<const char> bytes_value) {
            wire.append(bytes_value.data(), bytes_value.size());
            return ruvia::quic_stream_write_result{.status_ = ruvia::quic_operation_status::accepted, .accepted_ = bytes_value.size()};
        };
        int fins{};
        auto finish_value = [&](auto) { ++fins; return ruvia::quic_operation_status::accepted; };
        RUVIA_CHECK(driver.drive(open, register_response, write, finish_value) == driver_type::result_type::progress);
        const auto head_size = wire.size();
        upload.output_.chunk_.assign("abc");
        upload.output_.chunk_ready_ = true;
        RUVIA_CHECK(driver.drive(open, register_response, write, finish_value) == driver_type::result_type::blocked);
        RUVIA_CHECK(driver.waiting_for_content());
        RUVIA_CHECK_EQ(wire.size(), head_size);
        upload.content_released_ = true;
        for (int tick = 0; tick < 4 && upload.output_.chunk_ready_; ++tick) {
            RUVIA_CHECK(driver.drive(open, register_response, write, finish_value) != driver_type::result_type::fatal);
        }
        RUVIA_CHECK(!upload.output_.chunk_ready_ && upload.output_.chunk_.empty());
        RUVIA_CHECK(driver.drive(open, register_response, write, finish_value) == driver_type::result_type::blocked);
        upload.output_.chunk_.assign("def");
        upload.output_.chunk_ready_ = true;
        for (int tick = 0; tick < 4 && upload.output_.chunk_ready_; ++tick) {
            RUVIA_CHECK(driver.drive(open, register_response, write, finish_value) != driver_type::result_type::fatal);
        }
        upload.trailers_.push_back(ruvia::http_header::copy_of("x-end", "retained", &pool));
        upload.output_.end_requested_ = true;
        for (int tick = 0; tick < 4 && !driver.finished(); ++tick) {
            RUVIA_CHECK(driver.drive(open, register_response, write, finish_value) != driver_type::result_type::fatal);
        }
        RUVIA_CHECK(driver.finished() && upload.output_.ended_);
        RUVIA_CHECK_EQ(fins, 1);
        ruvia::http3_connection server(ruvia::http3_peer_role::server, &pool);
        received received;
        const auto result_value = server.feed(0, wire, true, false, receive, &received);
        RUVIA_CHECK(result_value.status_ == ruvia::http3_connection_status::message_end);
        RUVIA_CHECK_EQ(received.body_, "abcdef");
        RUVIA_CHECK_EQ(received.trailer_, "x-end:retained");
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
}

RUVIA_TEST(http3_client_request_trailers_honor_peer_field_limit_after_cursor_move) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    auto run = [&]() -> ruvia::task<void> {
        std::pmr::unsynchronized_pool_resource pool;
        ruvia::detail::http3_client_sans_io_session_engine engine(&pool);
        const auto prefixes = ruvia::http3_local_critical_streams::create({.max_field_section_size_ = 512});
        RUVIA_CHECK((prefixes.index() == 0));
        RUVIA_CHECK(engine.feed(3, std::get<0>(prefixes).control_prefix()).scope_ == ruvia::http3_connection_error_scope::none);
        const auto worker_value = attachment.loop().handle();
        ruvia::detail::http_client_upload_state upload(worker_value, &pool, {});
        upload.content_released_ = true;
        ruvia::detail::http_client_request_storage storage("POST", "/upload", &pool);
        storage.bind_upload(upload);
        auto cursor_value = ruvia::detail::http3_client_request_write::create(std::move(storage), "https", "example.com", &pool);
        RUVIA_CHECK((cursor_value.index() == 0));
        RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::post).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(std::get<0>(cursor_value).prepare_connection_head(0, engine));
        ruvia::detail::http3_client_request_write moved(std::move(std::get<0>(cursor_value)));
        const auto head = moved.next();
        RUVIA_CHECK((head.index() == 0));
        RUVIA_CHECK((moved.acknowledge(std::get<0>(head).size()).index() == 0));
        upload.trailers_.push_back(ruvia::http_header::copy_of("x-end", std::string(513, 't'), &pool));
        upload.output_.end_requested_ = true;
        const auto rejected = moved.next();
        RUVIA_CHECK((rejected.index() != 0) && moved.failed());
        RUVIA_CHECK(!upload.output_.ended_);
        attachment.stop();
        co_return;
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
}

RUVIA_TEST(http3_client_priority_updates_use_control_frames_and_bound_pending_output) {
    ruvia::test::counting_memory_resource pool;
    ruvia::detail::http3_client_sans_io_session_engine engine(&pool);
    RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::get).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(!engine.queue_priority_update(4, {.urgency_ = 1}));
    RUVIA_CHECK(!engine.queue_priority_update(0, {.urgency_ = 8}));
    RUVIA_CHECK(engine.pending_control_output().empty());
    RUVIA_CHECK(engine.queue_priority_update(0, {.urgency_ = 1, .incremental_ = true}));
    RUVIA_CHECK(engine.queue_priority_update(0, {.urgency_ = 6}));
    auto prefixes = ruvia::http3_local_critical_streams::create({});
    std::string wire(std::get<0>(prefixes).control_prefix().data(), std::get<0>(prefixes).control_prefix().size());
    const auto pending = engine.pending_control_output();
    wire.append(pending.data(), pending.size());
    ruvia::http3_connection server(ruvia::http3_peer_role::server, &pool);
    std::vector<ruvia::http_priority> priorities;
    const auto on_event = [](void* raw, const ruvia::http3_connection_event& event) {
        if (event.priority_update_) {
            static_cast<std::vector<ruvia::http_priority>*>(raw)->push_back(event.priority_update_->fields_.request_priority());
        }
    };
    RUVIA_CHECK(server.feed(2, wire, false, false, on_event, &priorities).scope_ == ruvia::http3_connection_error_scope::none);
    RUVIA_CHECK(priorities.size() == 2);
    RUVIA_CHECK(priorities[0].urgency_ == 1 && priorities[0].incremental_);
    RUVIA_CHECK(priorities[1].urgency_ == 6 && !priorities[1].incremental_);
    const auto bytes_value = pending.size();
    RUVIA_CHECK(!engine.consume_control_output(bytes_value + 1));
    RUVIA_CHECK(engine.consume_control_output(1));
    RUVIA_CHECK(engine.consume_control_output(bytes_value - 1));
    RUVIA_CHECK(engine.pending_control_output().empty());
    const auto baseline = pool.live_allocations();
    for (unsigned operation = 0; operation != 128; ++operation) {
        RUVIA_CHECK(engine.queue_priority_update(0, {.urgency_ = 1}));
        RUVIA_CHECK(engine.consume_control_output(engine.pending_control_output().size()));
        RUVIA_CHECK_EQ(pool.live_allocations(), baseline);
    }
    std::size_t queued{};
    while (engine.queue_priority_update(0, {.urgency_ = 2})) {
        ++queued;
        if (queued > 65536) {
            RUVIA_CHECK(false);
            break;
        }
    }
    RUVIA_CHECK(queued > 1 && engine.pending_control_output().size() <= ruvia::max_http_header_bytes);
    RUVIA_CHECK(engine.stop().scope_ == ruvia::http3_connection_error_scope::connection);
    RUVIA_CHECK(engine.pending_control_output().empty());
    RUVIA_CHECK(!engine.queue_priority_update(0, {}));
}

RUVIA_TEST(http3_client_request_head_honors_peer_field_limit_with_static_and_dynamic_qpack) {
    for (const std::uint64_t capacity : {0U, 256U}) {
        std::pmr::unsynchronized_pool_resource pool;
        ruvia::detail::http3_client_sans_io_session_engine engine(&pool);
        const auto prefixes = ruvia::http3_local_critical_streams::create({.qpack_max_table_capacity_ = capacity,
            .max_field_section_size_ = 0,
            .qpack_blocked_streams_ = 2});
        RUVIA_CHECK((prefixes.index() == 0));
        if ((prefixes.index() != 0)) {
            continue;
        }
        RUVIA_CHECK(engine.feed(3, std::get<0>(prefixes).control_prefix(), false, false).scope_ == ruvia::http3_connection_error_scope::none);
        RUVIA_CHECK(engine.register_request(0, ruvia::http_known_method::post).scope_ == ruvia::http3_connection_error_scope::none);
        const auto pending = engine.pending_encoder_output();
        const std::string encoder_before(pending.data(), pending.size());
        auto request = make_request(&pool);
        RUVIA_CHECK(request.has_value());
        if (request) {
            RUVIA_CHECK(!request->prepare_connection_head(0, engine));
            const auto encoder_after = engine.pending_encoder_output();
            RUVIA_CHECK_EQ(std::string_view(encoder_after.data(), encoder_after.size()), std::string_view(encoder_before));
        }
    }
}

RUVIA_TEST(http3_client_request_driver_uses_peer_qpack_settings_and_emits_dynamic_encoder_instructions) {
    std::pmr::unsynchronized_pool_resource pool;
    ruvia::detail::http3_client_sans_io_session_engine engine(&pool);
    auto prefixes = ruvia::http3_local_critical_streams::create({.qpack_max_table_capacity_ = 256, .qpack_blocked_streams_ = 2});
    RUVIA_CHECK((prefixes.index() == 0));
    if ((prefixes.index() != 0)) {
        return;
    }
    RUVIA_CHECK(engine.feed(3, std::get<0>(prefixes).control_prefix()).scope_ == ruvia::http3_connection_error_scope::none);
    auto created = make_request(&pool);
    RUVIA_CHECK(created.has_value());
    if (!created) {
        return;
    }
    driver_type driver(std::move(*created));
    std::string wire;
    auto open = [] { return ruvia::quic_stream_open_result{.status_ = ruvia::quic_operation_status::accepted, .stream_id_ = 0}; };
    auto register_response = [&](auto id, auto method) {
        const auto registered = engine.register_request(id, method);
        return registered.scope_ == ruvia::http3_connection_error_scope::none && driver.prepare_connection_head(id, engine);
    };
    auto write = [&](auto, std::span<const char> bytes_value) {
        wire.append(bytes_value.data(), bytes_value.size());
        return ruvia::quic_stream_write_result{.status_ = ruvia::quic_operation_status::accepted, .accepted_ = bytes_value.size()};
    };
    auto finish_value = [](auto) { return ruvia::quic_operation_status::accepted; };
    for (int tick = 0; tick < 8 && !driver.finished(); ++tick) {
        RUVIA_CHECK(driver.drive(open, register_response, write, finish_value) != driver_type::result_type::fatal);
    }
    RUVIA_CHECK(driver.finished());
    RUVIA_CHECK(!engine.pending_encoder_output().empty());
    ruvia::http3_connection peer(ruvia::http3_peer_role::server, &pool,
        {.qpack_max_table_capacity_ = 256, .qpack_blocked_streams_ = 2});
    received received;
    const auto blocked = peer.feed(0, wire, true, false, receive, &received);
    RUVIA_CHECK(blocked.status_ == ruvia::http3_connection_status::qpack_blocked);
    std::string instructions(1, char{2});
    const auto pending = engine.pending_encoder_output();
    instructions.append(pending.data(), pending.size());
    RUVIA_CHECK(peer.feed(6, std::span<const char>(instructions.data(), instructions.size()), false, false, receive, &received).scope_ == ruvia::http3_connection_error_scope::none);
    const auto resumed = peer.feed(0, std::span<const char>(wire.data(), wire.size()).subspan(blocked.consumed_bytes_), true, false, receive, &received);
    RUVIA_CHECK(resumed.status_ == ruvia::http3_connection_status::message_end);
    RUVIA_CHECK_EQ(received.body_, "payload");
    RUVIA_CHECK(engine.consume_encoder_output(pending.size()));
}
