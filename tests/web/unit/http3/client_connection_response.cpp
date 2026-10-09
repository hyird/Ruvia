#include "http3_client_connection_fixture.h"

namespace {

ruvia::task<void> exercise_real_response(asio::io_context& io, const ruvia::worker_handle& worker_value,
    ruvia::event_loop_attachment& attachment, local_quic_response_peer& peer,
    ruvia::testing::test_context& ruvia_ctx) {
    try {
        counting_resource memory;
        {
            ruvia::detail::client_transport_config_view config{
                .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification};
            ruvia::detail::http3_quic_client_tls_context tls(config);
            ruvia::task_scope tasks(worker_value, {.resource_ = &memory});
            connection_type connection(io, worker_value, tasks, tls,
                ruvia::http_origin_view::https({.host_ = "127.0.0.1", .port_ = peer.port()}), 10s,
                &memory, 4, 16 * 1024 * 1024);
            connection_watchdog watchdog(io, connection);
            ruvia::detail::http_client_response_state response(worker_value, &memory);
            response.buffered_limit_ = 6;
            ruvia::detail::http_client_request_storage request("GET", "/incremental", &memory);
            const auto deadline_value = std::chrono::steady_clock::now() + 12s;
            const auto submitted = connection.submit(std::move(request), response, deadline_value);
            RUVIA_CHECK(submitted.outcome_ == connection_type::outcome_type::pending);
            RUVIA_CHECK(response.references_ == 2);
            connection.start();

            std::exception_ptr failure;
            try {
                const bool got_head = co_await wait_for_peer(worker_value, peer, [&] { return peer.first_part_ready() && response.head_ready_; }, 8s);
                RUVIA_CHECK(got_head);
                if (got_head) {
                    RUVIA_CHECK(response.response_body_plan_.has_value());
                    RUVIA_CHECK_EQ(response.status_.value(), 200);
                    RUVIA_CHECK_EQ(response.headers_.size(), std::size_t{1});
                    if (!response.headers_.empty()) {
                        RUVIA_CHECK_EQ(response.headers_.front().name(), std::string_view("content-length"));
                        RUVIA_CHECK_EQ(response.headers_.front().value(), std::string_view("6"));
                    }

                    auto first = co_await response.consume_body<std::string_view>();
                    RUVIA_CHECK(first.has_value());
                    if (first) {
                        RUVIA_CHECK_EQ(*first, std::string_view("abc"));
                        peer.allow_final_part();
                        const bool got_final = co_await wait_for_peer(worker_value, peer, [&] { return peer.final_part_sent() && response.pending_.size() == 3; }, 3s);
                        RUVIA_CHECK(got_final);
                        RUVIA_CHECK_EQ(*first, std::string_view("abc"));
                        if (got_final) {
                            auto second = co_await response.consume_body<std::string_view>();
                            RUVIA_CHECK(second.has_value());
                            if (second) {
                                RUVIA_CHECK_EQ(*second, std::string_view("def"));
                            }
                            co_await connection.wait(submitted.id_);
                            RUVIA_CHECK(response.complete_);
                            const auto completed = connection.result(submitted.id_);
                            RUVIA_CHECK(completed != nullptr);
                            if (completed != nullptr) {
                                RUVIA_CHECK(completed->outcome_ == connection_type::outcome_type::complete);
                                RUVIA_CHECK(completed->response_body_plan_.has_value());
                                RUVIA_CHECK_EQ(completed->status_, std::uint16_t{200});
                            }
                        }
                    }
                }
            } catch (...) {
                failure = std::current_exception();
            }

            connection.request_stop();
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            connection.consumer_released(submitted.id_);
            RUVIA_CHECK_EQ(connection.retained_requests(), std::size_t{0});
            RUVIA_CHECK(response.references_ == 1);
            RUVIA_CHECK(response.http3_connection_ == nullptr && response.http3_request_id_ == 0);
            if (failure != nullptr) {
                std::rethrow_exception(failure);
            }
        }
        RUVIA_CHECK_EQ(memory.allocations_, memory.returns_);
        RUVIA_CHECK_EQ(memory.live_bytes_, std::size_t{0});
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::task<void> exercise_response_release_across_generations(asio::io_context& io,
    const ruvia::worker_handle& worker_value, ruvia::event_loop_attachment& attachment,
    local_quic_response_peer& first_peer, local_quic_response_peer& second_peer,
    ruvia::testing::test_context& ruvia_ctx) {
    try {
        counting_resource first_memory;
        counting_resource second_memory;
        {
            ruvia::detail::http3_client_body_budget body_budget(7);
            {
                ruvia::detail::http_client_response_state first_state(worker_value, &first_memory);
                ruvia::detail::http_client_response_state second_state(worker_value, &second_memory);
                std::string_view body_view;
                std::string_view header_view;
                std::string_view trailer_view;
                {
                    ruvia::detail::client_transport_config_view config{
                        .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification};
                    ruvia::detail::http3_quic_client_tls_context tls(config);
                    ruvia::task_scope tasks(worker_value, {.resource_ = &first_memory});
                    {
                        connection_type connection(io, worker_value, tasks, tls,
                            ruvia::http_origin_view::https({.host_ = "127.0.0.1", .port_ = first_peer.port()}),
                            10s, &first_memory, 4, 16 * 1024 * 1024, 30s, &body_budget);
                        connection_watchdog watchdog(io, connection);
                        first_state.buffered_limit_ = 7;
                        const auto submitted = connection.submit(
                            ruvia::detail::http_client_request_storage("GET", "/retain-response", &first_memory),
                            first_state);
                        RUVIA_CHECK(submitted.outcome_ == connection_type::outcome_type::pending);
                        connection.start();

                        std::exception_ptr failure;
                        try {
                            const bool got_head = co_await wait_for_peer(worker_value, first_peer, [&] { return first_peer.first_part_ready() && first_state.head_ready_; }, 8s);
                            RUVIA_CHECK(got_head);
                            first_peer.allow_final_part();
                            const bool completed = co_await wait_for_peer(worker_value, first_peer, [&] { return first_peer.final_part_sent() && first_state.complete_; }, 5s);
                            RUVIA_CHECK(completed);
                        } catch (...) {
                            failure = std::current_exception();
                        }

                        connection.request_stop();
                        try {
                            co_await connection.wait(submitted.id_);
                        } catch (...) {
                            if (failure == nullptr) {
                                failure = std::current_exception();
                            }
                        }
                        try {
                            co_await tasks.join();
                        } catch (...) {
                            if (failure == nullptr) {
                                failure = std::current_exception();
                            }
                        }
                        watchdog.disarm();
                        RUVIA_CHECK(!watchdog.expired());
                        RUVIA_CHECK(connection.result(submitted.id_) != nullptr);
                        RUVIA_CHECK(first_state.complete_);
                        RUVIA_CHECK_EQ(first_state.status_.value(), 200);
                        RUVIA_CHECK_EQ(first_state.protocol_version_, ruvia::http_protocol_version::http3);
                        RUVIA_CHECK_EQ(first_state.headers_.size(), std::size_t{1});
                        RUVIA_CHECK_EQ(first_state.pending_, std::string_view("abcdef"));
                        first_state.trailers_.push_back(
                            ruvia::http_header::copy_of("x-retained", "trailer", &first_memory));
                        header_view = first_state.headers_.front().value();
                        trailer_view = first_state.trailers_.front().value();
                        RUVIA_CHECK_EQ(body_budget.used(), std::size_t{6});
                        RUVIA_CHECK(connection.release_response_request(submitted.id_));
                        RUVIA_CHECK_EQ(first_state.http3_body_budget_.retained_bytes(), std::size_t{6});
                        RUVIA_CHECK(!first_state.error_code_ && first_state.failure_ == nullptr);
                        RUVIA_CHECK_EQ(connection.retained_requests(), std::size_t{0});
                        RUVIA_CHECK_EQ(first_state.references_, std::size_t{1});
                        RUVIA_CHECK(first_state.http3_connection_ == nullptr);
                        if (failure != nullptr) {
                            std::rethrow_exception(failure);
                        }
                    }
                }

                RUVIA_CHECK(first_state.complete_);
                RUVIA_CHECK_EQ(first_state.status_.value(), 200);
                RUVIA_CHECK_EQ(first_state.protocol_version_, ruvia::http_protocol_version::http3);
                RUVIA_CHECK_EQ(first_state.headers_.front().value(), header_view);
                RUVIA_CHECK_EQ(first_state.trailers_.front().value(), trailer_view);
                {
                    auto first_body_read = co_await first_state.consume_body<std::string_view>();
                    RUVIA_CHECK(first_body_read.has_value());
                    if (first_body_read) {
                        body_view = *first_body_read;
                        RUVIA_CHECK_EQ(body_view, std::string_view("abcdef"));
                    }
                }
                RUVIA_CHECK_EQ(first_state.offset_, std::size_t{6});
                RUVIA_CHECK_EQ(first_state.http3_body_budget_.retained_bytes(), std::size_t{6});
                RUVIA_CHECK_EQ(body_budget.used(), std::size_t{6});

                {
                    ruvia::detail::client_transport_config_view config{
                        .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification};
                    ruvia::detail::http3_quic_client_tls_context tls(config);
                    ruvia::task_scope tasks(worker_value, {.resource_ = &second_memory});
                    {
                        connection_type connection(io, worker_value, tasks, tls,
                            ruvia::http_origin_view::https({.host_ = "127.0.0.1", .port_ = second_peer.port()}),
                            10s, &second_memory, 4, 16 * 1024 * 1024, 30s, &body_budget);
                        connection_watchdog watchdog(io, connection);
                        const auto submitted = connection.submit(
                            ruvia::detail::http_client_request_storage("GET", "/next-generation", &second_memory),
                            second_state);
                        RUVIA_CHECK(submitted.outcome_ == connection_type::outcome_type::pending);
                        connection.start();

                        std::exception_ptr failure;
                        try {
                            const bool blocked_at_budget = co_await wait_for_peer(worker_value, second_peer, [&] { return second_peer.first_part_ready() && second_state.head_ready_ &&
                                                                                                                          second_state.producer_body_bytes() == 1; }, 8s);
                            RUVIA_CHECK(blocked_at_budget);
                            RUVIA_CHECK_EQ(body_budget.used(), std::size_t{7});
                            RUVIA_CHECK_EQ(body_view, std::string_view("abcdef"));
                            RUVIA_CHECK_EQ(first_state.http3_body_budget_.retained_bytes(), std::size_t{6});
                            body_view = {};
                            first_state.release_consumed_body_prefix();
                            RUVIA_CHECK(first_state.buffered_.empty());
                            RUVIA_CHECK_EQ(first_state.offset_, std::size_t{0});
                            RUVIA_CHECK_EQ(first_state.http3_body_budget_.retained_bytes(), std::size_t{0});
                            RUVIA_CHECK_EQ(body_budget.used(), std::size_t{1});
                            second_peer.allow_final_part();
                            const bool completed = co_await wait_for_peer(worker_value, second_peer, [&] { return second_peer.final_part_sent() && second_state.complete_; }, 5s);
                            RUVIA_CHECK(completed);
                        } catch (...) {
                            failure = std::current_exception();
                        }

                        connection.request_stop();
                        try {
                            co_await connection.wait(submitted.id_);
                        } catch (...) {
                            if (failure == nullptr) {
                                failure = std::current_exception();
                            }
                        }
                        try {
                            co_await tasks.join();
                        } catch (...) {
                            if (failure == nullptr) {
                                failure = std::current_exception();
                            }
                        }
                        watchdog.disarm();
                        RUVIA_CHECK(!watchdog.expired());
                        RUVIA_CHECK(connection.result(submitted.id_) != nullptr);
                        RUVIA_CHECK(connection.release_response_request(submitted.id_));
                        RUVIA_CHECK_EQ(second_state.http3_body_budget_.retained_bytes(), std::size_t{6});
                        RUVIA_CHECK_EQ(connection.retained_requests(), std::size_t{0});
                        RUVIA_CHECK_EQ(second_state.references_, std::size_t{1});
                        if (failure != nullptr) {
                            std::rethrow_exception(failure);
                        }
                    }
                }
                RUVIA_CHECK(second_state.complete_);
                RUVIA_CHECK_EQ(second_state.pending_, std::string_view("abcdef"));
                RUVIA_CHECK_EQ(body_budget.used(), std::size_t{6});
                second_state.discard_response_body();
                RUVIA_CHECK_EQ(body_budget.used(), std::size_t{0});
            }
        }
        RUVIA_CHECK_EQ(first_memory.allocations_, first_memory.returns_);
        RUVIA_CHECK_EQ(first_memory.live_bytes_, std::size_t{0});
        RUVIA_CHECK_EQ(second_memory.allocations_, second_memory.returns_);
        RUVIA_CHECK_EQ(second_memory.live_bytes_, std::size_t{0});
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::task<void> exercise_connection_error_priority(asio::io_context& io,
    const ruvia::worker_handle& worker_value, ruvia::event_loop_attachment& attachment,
    local_quic_response_peer& peer, ruvia::testing::test_context& ruvia_ctx) {
    try {
        counting_resource memory;
        counting_resource response_memory;
        {
            ruvia::detail::client_transport_config_view config{
                .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification};
            ruvia::detail::http3_quic_client_tls_context tls(config);
            ruvia::task_scope tasks(worker_value, {.resource_ = &memory});
            connection_type connection(io, worker_value, tasks, tls,
                ruvia::http_origin_view::https({.host_ = "127.0.0.1", .port_ = peer.port()}), 10s,
                &memory, 4, 16 * 1024 * 1024);
            connection_watchdog watchdog(io, connection);
            ruvia::detail::http_client_response_state response(worker_value, &response_memory);
            ruvia::detail::http_client_request_storage request("GET", "/protocol-priority", &memory);
            const auto deadline_value = std::chrono::steady_clock::now() + 12s;
            const auto submitted = connection.submit(std::move(request), response, deadline_value);
            RUVIA_CHECK(submitted.outcome_ == connection_type::outcome_type::pending);
            connection.start();

            std::exception_ptr failure;
            try {
                const bool got_head = co_await wait_for_peer(worker_value, peer, [&] { return peer.first_part_ready() && response.head_ready_; }, 8s);
                RUVIA_CHECK(got_head);
                if (got_head) {
                    RUVIA_CHECK_EQ(response.pending_, std::string_view("abc"));
                    response_memory.reject_allocations();
                    peer.allow_final_part();
                    const bool terminal = co_await wait_for_peer(
                        worker_value, peer, [&] { return response.complete_; }, 4s);
                    RUVIA_CHECK(terminal);
                    if (terminal) {
                        RUVIA_CHECK(response_memory.rejected_allocations_ != 0);
                        RUVIA_CHECK(response.error_code_.has_value());
                        RUVIA_CHECK_EQ(*response.error_code_,
                            static_cast<std::uint8_t>(ruvia::http_client_error::code_type::protocol_error));
                        RUVIA_CHECK(response.failure_ == nullptr);
                        const auto result_value = connection.result(submitted.id_);
                        RUVIA_CHECK(result_value != nullptr);
                        if (result_value != nullptr) {
                            RUVIA_CHECK(result_value->outcome_ == connection_type::outcome_type::protocol_error);
                        }
                    }
                }
            } catch (...) {
                failure = std::current_exception();
            }

            response_memory.reject_allocations(false);
            connection.request_stop();
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            response.discard_response_body();
            RUVIA_CHECK(response.error_code_.has_value());
            RUVIA_CHECK_EQ(*response.error_code_,
                static_cast<std::uint8_t>(ruvia::http_client_error::code_type::protocol_error));
            RUVIA_CHECK(connection.release_response_request(submitted.id_));
            RUVIA_CHECK_EQ(connection.retained_requests(), std::size_t{0});
            RUVIA_CHECK(response.references_ == 1);
            RUVIA_CHECK(response.failure_ == nullptr);
            RUVIA_CHECK_EQ(response.protocol_version_, ruvia::http_protocol_version::http3);
            RUVIA_CHECK_EQ(response.status_.value(), 200);
            RUVIA_CHECK_EQ(response.headers_.size(), std::size_t{1});
            if (failure != nullptr) {
                std::rethrow_exception(failure);
            }
        }
        RUVIA_CHECK_EQ(memory.allocations_, memory.returns_);
        RUVIA_CHECK_EQ(memory.live_bytes_, std::size_t{0});
        RUVIA_CHECK_EQ(response_memory.allocations_, response_memory.returns_);
        RUVIA_CHECK_EQ(response_memory.live_bytes_, std::size_t{0});
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::task<void> exercise_parser_registration_allocation_failure(asio::io_context& io,
    const ruvia::worker_handle& worker_value, ruvia::event_loop_attachment& attachment,
    local_quic_response_peer& peer, ruvia::testing::test_context& ruvia_ctx) {
    try {
        counting_resource registration_memory;
        counting_resource::allocation_type registration_allocation{};
        {
            ruvia::detail::http3_client_sans_io_session_engine parser(&registration_memory);
            registration_memory.begin_trace();
            const auto registered = parser.register_request(0, ruvia::http_known_method::get);
            RUVIA_CHECK(registered.scope_ == ruvia::http3_connection_error_scope::none);
            const auto allocation = registration_memory.first_allocation();
            RUVIA_CHECK(allocation.has_value());
            if (!allocation) {
                throw std::runtime_error("HTTP/3 parser registration made no PMR allocation");
            }
            registration_allocation = *allocation;
            (void)parser.stop();
        }
        RUVIA_CHECK_EQ(registration_memory.allocations_, registration_memory.returns_);
        RUVIA_CHECK_EQ(registration_memory.live_bytes_, std::size_t{0});

        counting_resource memory;
        {
            ruvia::detail::client_transport_config_view config{
                .tls_peer_verification_ = ruvia::tls_peer_verification_policy::skip_verification};
            ruvia::detail::http3_quic_client_tls_context tls(config);
            ruvia::task_scope tasks(worker_value, {.resource_ = &memory});
            connection_type connection(io, worker_value, tasks, tls,
                ruvia::http_origin_view::https({.host_ = "127.0.0.1", .port_ = peer.port()}), 10s,
                &memory, 4, 16 * 1024 * 1024);
            connection_watchdog watchdog(io, connection);
            const auto first = connection.submit(
                ruvia::detail::http_client_request_storage("GET", "/parser-allocation", &memory));
            const auto sibling = connection.submit(
                ruvia::detail::http_client_request_storage("GET", "/sibling-must-not-write", &memory));
            RUVIA_CHECK(first.outcome_ == connection_type::outcome_type::pending);
            RUVIA_CHECK(sibling.outcome_ == connection_type::outcome_type::pending);
            // register_request()'s first allocation is inside responses_.try_emplace().
            // The request driver opens its QUIC stream before invoking this callback,
            // and emits HEADERS only after it returns.
            memory.fail_first_allocation(registration_allocation);
            connection.start();

            std::exception_ptr failure;
            try {
                co_await connection.wait(first.id_);
                co_await connection.wait(sibling.id_);
            } catch (...) {
                failure = std::current_exception();
            }
            if (failure != nullptr) {
                connection.request_stop();
            }
            try {
                co_await tasks.join();
            } catch (...) {
                if (failure == nullptr) {
                    failure = std::current_exception();
                }
            }
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            RUVIA_CHECK(memory.rejected_allocations_ == 1);
            RUVIA_CHECK(memory.last_rejected_allocation() == registration_allocation);
            RUVIA_CHECK(memory.matching_allocation_attempts() == 1);
            RUVIA_CHECK(!connection.running());
            RUVIA_CHECK(peer.synchronize());
            // Registration failure can close the client before the peer has
            // confirmed the TLS handshake. Only the no-request-payload contract
            // is synchronized here; peer handshake readiness is not guaranteed.
            RUVIA_CHECK_EQ(peer.request_payload_bytes(), std::size_t{0});

            const auto first_result = connection.result(first.id_);
            const auto sibling_result = connection.result(sibling.id_);
            RUVIA_CHECK(first_result != nullptr &&
                        first_result->outcome_ == connection_type::outcome_type::transport_error);
            RUVIA_CHECK(sibling_result != nullptr &&
                        sibling_result->outcome_ == connection_type::outcome_type::transport_error);
            RUVIA_CHECK(first_result != nullptr && !first_result->response_body_plan_);
            RUVIA_CHECK(sibling_result != nullptr && !sibling_result->response_body_plan_);
            RUVIA_CHECK(connection.submit(ruvia::detail::http_client_request_storage(
                                              "GET", "/must-not-reuse", &memory))
                            .outcome_ == connection_type::outcome_type::connection_draining);
            RUVIA_CHECK(connection.release(first.id_));
            RUVIA_CHECK(connection.release(sibling.id_));
            RUVIA_CHECK_EQ(connection.retained_requests(), std::size_t{0});
            if (failure != nullptr) {
                std::rethrow_exception(failure);
            }
        }
        RUVIA_CHECK_EQ(memory.allocations_, memory.returns_);
        RUVIA_CHECK_EQ(memory.live_bytes_, std::size_t{0});
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

}  // namespace

RUVIA_TEST(http3_client_connection_response_plan_moves_across_resource_handoff_as_an_owned_value) {
    counting_resource source;
    counting_resource target;
    {
        connection_type::response_type original(&source);
        original.outcome_ = connection_type::outcome_type::complete;
        original.status_ = 200;
        original.response_body_plan_ = ruvia::plan_http_response_body(
            ruvia::http_known_method::head, ruvia::http_status::ok);
        original.headers_.push_back(ruvia::http_header::copy_of("x-result", "owned", &source));
        original.trailers_.push_back(ruvia::http_header::copy_of("x-trailer", "owned", &source));
        original.body_.assign("representation");

        connection_type::response_type moved(std::move(original));
        connection_type::response_type handed_off(&target);
        handed_off = std::move(moved);
        RUVIA_CHECK(handed_off.outcome_ == connection_type::outcome_type::complete);
        RUVIA_CHECK(handed_off.response_body_plan_ &&
                    handed_off.response_body_plan_->request_method() == ruvia::http_known_method::head &&
                    handed_off.response_body_plan_->response_status() == ruvia::http_status::ok &&
                    handed_off.response_body_plan_->body_suppressed());
        RUVIA_CHECK(handed_off.headers_.size() == 1 && handed_off.headers_.front().value() == "owned");
        RUVIA_CHECK(handed_off.trailers_.size() == 1 && handed_off.trailers_.front().value() == "owned");
        RUVIA_CHECK(handed_off.body_ == "representation");
    }
    RUVIA_CHECK(source.allocations_ == source.returns_ && source.live_bytes_ == 0);
    RUVIA_CHECK(target.allocations_ == target.returns_ && target.live_bytes_ == 0);
}

RUVIA_TEST(http3_client_connection_publishes_real_quic_response_incrementally_and_reclaims_request_state) {
    test_identity_files identity;
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    auto root = attachment.loop().start(exercise_real_response(io, worker_value, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
}

RUVIA_TEST(http3_client_pool_publishes_incremental_response_through_public_response_api) {
    test_identity_files identity;
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    auto root = attachment.loop().start(
        exercise_public_http3_client_pool(io, worker_value, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
}

RUVIA_TEST(http3_client_connection_response_release_preserves_data_and_wakes_the_next_budget_generation) {
    test_identity_files identity;
    local_quic_response_peer first_peer(identity);
    local_quic_response_peer second_peer(identity);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    auto root = attachment.loop().start(exercise_response_release_across_generations(
        io, worker_value, attachment, first_peer, second_peer, ruvia_ctx));
    attachment.run();
    root.get();
}

RUVIA_TEST(http3_client_connection_parser_registration_allocation_failure_closes_and_joins_the_connection) {
    test_identity_files identity;
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    auto root = attachment.loop().start(
        exercise_parser_registration_allocation_failure(io, worker_value, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
}

RUVIA_TEST(http3_client_connection_protocol_error_wins_over_callback_allocation_failure_in_one_feed) {
    test_identity_files identity;
    local_quic_response_peer peer(identity, true);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    auto root = attachment.loop().start(exercise_connection_error_priority(io, worker_value, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
}

RUVIA_TEST(http3_public_client_pool_preserves_owned_response_and_priority) {
    test_identity_files identity;
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    auto root = attachment.loop().start(exercise_public_http3_client_pool(
        io, worker_value, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
    peer.rethrow_if_failed();
}
