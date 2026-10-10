#include <algorithm>
#include <array>

#include <asio/write.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_request_target.h"

#include "client/client_transport.h"
#include "client/http_client_config_validation.h"
#include "client/http_client_pool.h"
#include "client/http_client_response_decoding.h"
#include "client/http_client_response_state.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] std::pmr::string http2_authority(
    const http_client_config_storage& config, std::pmr::memory_resource* resource) {
    auto authority = client_uri_host(config.host_, resource);
    const auto port = http_client_port(config);
    const auto default_port = config.scheme_ == http_scheme::https ? 443 : 80;
    if (port != default_port) {
        authority.push_back(':');
        client_port_text_buffer_type port_buffer{};
        authority.append(format_client_port(port, port_buffer));
    }
    return authority;
}

}  // namespace

task<void> http_client_pool::initialize_http2(
    connection_type& connection, const ruvia::operation_timeout& timeout) {
    connection.http2_ = make_pmr_object<::ruvia::http2_connection>(
        resource_, ::ruvia::http2_connection::client({.resource_ = resource_, .enable_push_ = config_.push_.enabled_, .receive_origin_advertisements_ = config_.advertisements_.receive_origins_}));
    while (connection.http2_->wants_write()) {
        const auto output = connection.http2_->pending_output();
        co_await connection.transport_.write(output, timeout);
        (void)connection.http2_->consume_output(output.size());
    }
    std::array<char, 16384> input{};
    while (!connection.http2_->received_peer_settings()) {
        const auto bytes_value = co_await connection.transport_.read_some(input, timeout);
        if (bytes_value == 0) {
            throw http_client_error(
                http_client_error::code_type::io_error, "upstream closed during HTTP/2 preface");
        }
        const auto status = connection.http2_->feed(std::string_view(input.data(), bytes_value));
        while (auto event = connection.http2_->next_event()) {
            retain_http2_advertisement(connection, *event);
        }
        if (status == http2_feed_result::protocol_failure || connection.http2_->connection_error()) {
            throw http_client_error(
                http_client_error::code_type::protocol_error, "invalid HTTP/2 connection preface");
        }
        while (connection.http2_->wants_write()) {
            const auto output = connection.http2_->pending_output();
            co_await connection.transport_.write(output, timeout);
            (void)connection.http2_->consume_output(output.size());
        }
    }
    auto& runtime = *connection.http2_runtime_;
    auto generation = ++runtime.generation_;
    if (generation == 0) {
        generation = ++runtime.generation_;
    }
    runtime.running_ = true;
    runtime.draining_ = false;
    runtime.failed_ = false;
    runtime.session_tasks_ = 0;
    try {
        ++runtime.session_tasks_;
        try {
            background_tasks_.spawn(run_http2_reader(connection, generation));
        } catch (...) {
            --runtime.session_tasks_;
            throw;
        }
        ++runtime.session_tasks_;
        try {
            background_tasks_.spawn(run_http2_writer(connection, generation));
        } catch (...) {
            --runtime.session_tasks_;
            throw;
        }
    } catch (...) {
        const auto failure = std::current_exception();
        fail_http2_session(connection, generation, {}, failure);
        std::rethrow_exception(failure);
    }
}

void http_client_pool::retain_http2_advertisement(connection_type& connection, const http2_event& event) {
    const auto slot = static_cast<std::size_t>(&connection - connections_.data());
    if (const auto* origins = event.origin_advertisement()) {
        (void)advertisements_.retain(slot, http_protocol_version::http2, *origins);
    } else if (const auto* service = event.alternative_service_advertisement()) {
        (void)advertisements_.retain(slot, *service);
    }
}

http_client_pool::http2_push_driver_type::http2_push_driver_type(http_client_pool& pool, connection_type& conn, http_client_response value)
    : owner_(pool),
      connection_(conn),
      response_(std::move(value)),
      timeout_(pool.config_.push_.timeout_),
      pending_(pool.worker_, response_) {}
http_client_pool::http2_push_driver_type::~http2_push_driver_type() {
    if (registered_) {
        owner_.remove_http2_pending(connection_, pending_);
        --owner_.active_pushes_;
    }
}

void http_client_pool::accept_http2_push(connection_type& connection, const http2_push_promise_event& promise) {
    const auto authority = http2_authority(config_, resource_);
    if (!config_.push_.enabled_ || pushes_.size() >= config_.push_.max_queued_pushes_ ||
        active_pushes_ >= config_.push_.max_concurrent_pushes_ ||
        !http_ascii_equals_ignore_case(promise.request_.scheme_, config_.scheme_ == http_scheme::https ? "https" : "http") ||
        !http_authorities_equal(borrowed_text(promise.request_.authority_), borrowed_text(std::string_view(authority)),
            config_.scheme_ == http_scheme::https ? 443 : 80)) {
        ++rejected_pushes_;
        submit_http2_reset(connection, promise.promised_stream_id_);
        return;
    }
    try {
        http_client_response response(*this);
        auto& state_value = *response.state_;
        state_value.buffered_limit_ = config_.max_response_bytes_;
        state_value.transport_ = http_client_response_transport::http2;
        state_value.request_method_ = classify_http_method(promise.request_.method_);
        state_value.connection_index_ = static_cast<std::size_t>(&connection - connections_.data());
        state_value.stream_id_ = promise.promised_stream_id_;
        state_value.request_id_ = ++connection.http2_runtime_->next_request_id_;
        if (state_value.request_id_ == 0) {
            state_value.request_id_ = ++connection.http2_runtime_->next_request_id_;
        }
        state_value.promised_request_.emplace(state_value.resource_);
        auto& request = *state_value.promised_request_;
        request.method_ = promise.request_.method_;
        request.scheme_ = promise.request_.scheme_;
        request.authority_ = promise.request_.authority_;
        request.path_ = promise.request_.path_;
        request.headers_.reserve(promise.request_.headers_.size());
        for (const auto& field : promise.request_.headers_) {
            request.headers_.push_back(http_header::copy_of(field.name(), field.value(), state_value.resource_));
        }
        auto driver = make_pmr_object<http2_push_driver_type>(resource_, *this, connection, http_client_response(response.state_, true));
        driver->pending_.timeout_ = &driver->timeout_;
        driver->pending_.request_id_ = state_value.request_id_;
        driver->pending_.stream_id_ = promise.promised_stream_id_;
        connection.http2_runtime_->pending_.push_back(&driver->pending_);
        driver->registered_ = true;
        ++active_pushes_;
        // Register before the reader drains another event. The driver is address
        // stable and its destructor unregisters on cold spawn failure as well.
        background_tasks_.spawn(run_http2_push(std::move(driver)));
        pushes_.push_back(http_client_push(std::move(response)));
        ++received_pushes_;
    } catch (...) {
        ++rejected_pushes_;
        submit_http2_reset(connection, promise.promised_stream_id_);
        // A resource admission failure is local to this push, not its parent.
    }
}
task<void> http_client_pool::run_http2_push(std::unique_ptr<http2_push_driver_type, pmr_object_deleter<http2_push_driver_type>> driver) {
    auto& pending = driver->pending_;
    auto& state_value = *driver->response_.state_;
    worker_timer_registration timer;
    try {
        if (const auto remaining = driver->timeout_.remaining()) {
            (worker_).schedule_timer(timer, worker_timer_deadline_after(*remaining), [this, raw = driver.get()](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    cancel_http2_stream(raw->connection_, raw->pending_.request_id_, client_abort_reason::timeout);
                }
            });
        }
        while (!pending.complete_ && !pending.failed()) {
            co_await pending.signal_.wait();
        }
        timer.cancel();
        state_value.failure_ = pending.failure_;
        if (pending.error_) {
            state_value.error_code_ = static_cast<std::uint8_t>(*pending.error_);
        }
    } catch (...) {
        state_value.failure_ = std::current_exception();
        submit_http2_reset(driver->connection_, pending.stream_id_);
    }
    state_value.complete_ = true;
    state_value.head_signal_.notify();
    state_value.data_signal_.notify();
    state_value.space_signal_.notify();
}

void http_client_pool::drain_http2_events(connection_type& connection) {
    if (!connection.http2_) {
        return;
    }
    auto& runtime = *connection.http2_runtime_;
    const auto find_pending = [&runtime](std::uint32_t stream_id) -> http2_pending_stream_type* {
        const auto match =
            std::ranges::find_if(runtime.pending_, [stream_id](const http2_pending_stream_type* pending) {
                return pending->stream_id_ == stream_id;
            });
        return match == runtime.pending_.end() ? nullptr : *match;
    };
    const auto fail_pending = [this, &connection](http2_pending_stream_type& pending,
                                  std::uint32_t stream_id, std::exception_ptr failure,
                                  bool reset_stream) noexcept {
        if (pending.complete_ || pending.failed()) {
            return;
        }
        pending.failure_ = std::move(failure);
        if (reset_stream) {
            submit_http2_reset(connection, stream_id);
        }
        pending.signal_.notify();
    };
    bool released_data = false;
    while (auto event = connection.http2_->next_event()) {
        retain_http2_advertisement(connection, *event);
        if (const auto* push = event->push_promise()) {
            accept_http2_push(connection, *push);
        } else if (const auto* interim = event->informational_head()) {
            auto* pending = find_pending(interim->stream_id());
            if (pending == nullptr || pending->complete_ || pending->failed()) {
                continue;
            }
            try {
                auto& state_value = *pending->response_->state_;
                std::pmr::vector<http_header_view> fields_value(state_value.resource_);
                for (const auto& field : interim->head().headers()) {
                    fields_value.emplace_back(field.name(), field.value());
                }
                state_value.retain_informational(interim->head().status(), fields_value);
                if (state_value.upload_ && interim->request_content_signal() == http_client_request_content_signal::continue_value) {
                    state_value.upload_->content_released_ = true;
                    pending->signal_.notify();
                }
            } catch (...) {
                fail_pending(*pending, interim->stream_id(), std::current_exception(), true);
            }
        } else if (auto* head = event->response_head()) {
            auto* pending = find_pending(head->stream_id());
            if (pending == nullptr || pending->complete_ || pending->failed()) {
                continue;
            }
            try {
                auto response_head = std::move(*head).take_head();
                auto& state_value = *pending->response_->state_;
                if (state_value.upload_ && !state_value.upload_->output_.ended_) {
                    state_value.upload_->output_.stop();
                }
                state_value.status_ = response_head.status();
                state_value.protocol_version_ = response_head.protocol_version();
                state_value.response_body_plan_ = plan_http_response_body(state_value.request_method_, state_value.status_);
                auto response_headers_value = std::move(response_head).take_headers();
                state_value.headers_.clear();
                state_value.headers_.reserve(response_headers_value.size());
                for (const auto& header : response_headers_value) {
                    state_value.headers_.push_back(http_header::copy_of(
                        header.name(), header.value(), state_value.resource_));
                }
                if (state_value.tunnel_) {
                    state_value.tunnel_->accepted_ = state_value.response_body_plan_->content_semantics() == http_response_content_semantics_type::connect_tunnel;
                    if (state_value.tunnel_->accepted_ && state_value.tunnel_->udp_) {
                        std::pmr::vector<http_header_view> fields_value(state_value.resource_);
                        for (const auto& field : state_value.headers_) {
                            fields_value.emplace_back(field.name(), field.value());
                        }
                        if ((validate_http_connect_udp_response(state_value.protocol_version_, state_value.status_.value(), fields_value).index() != 0)) {
                            throw http_client_error(http_client_error::code_type::protocol_error, "invalid CONNECT-UDP response head");
                        }
                    }
                    if (!state_value.tunnel_->accepted_) {
                        state_value.tunnel_->output_.stop();
                    }
                    pending->signal_.notify();
                }
                if (!state_value.tunnel_ || !state_value.tunnel_->accepted_) {
                    configure_http_client_response_decoding(state_value);
                }
                state_value.head_ready_ = true;
                pending->response_->state_->head_signal_.notify();
            } catch (...) {
                fail_pending(*pending, head->stream_id(), std::current_exception(), true);
            }
        } else if (event->message_body_chunk() != nullptr || event->tunnel_data() != nullptr) {
            const auto consume = [&](auto* chunk) {
                auto* pending = find_pending(chunk->stream_id());
                if (pending != nullptr && !pending->complete_ && !pending->failed()) {
                    auto& state_value = *pending->response_->state_;
                    const auto retained =
                        state_value.buffered_.size() - state_value.offset_ + state_value.pending_.size();
                    if (state_value.collect_all_ && chunk->bytes().size() >
                                                        config_.max_response_bytes_ - std::min(retained, config_.max_response_bytes_)) {
                        pending->error_ = http_client_error::code_type::response_too_large;
                        submit_http2_reset(connection, chunk->stream_id());
                        pending->signal_.notify();
                    } else {
                        try {
                            state_value.pending_.append(chunk->bytes());
                            state_value.data_signal_.notify();
                        } catch (...) {
                            fail_pending(*pending, chunk->stream_id(), std::current_exception(), true);
                        }
                    }
                }
                if (pending != nullptr && !pending->complete_ && !pending->failed() &&
                    !pending->response_->state_->collect_all_) {
                    auto credit = chunk->take_credit();
                    if (credit.valid()) {
                        auto& retained = pending->response_->state_->http2_data_credit_;
                        if (retained) {
                            if (retained->merge(std::move(credit)) !=
                                http2_received_data_credit_merge_status::merged) {
                                std::terminate();
                            }
                        } else {
                            retained.emplace(std::move(credit));
                        }
                    }
                }
                // Unretained event credits return on destruction, including failed
                // or cancelled streams. Wake the writer to flush WINDOW_UPDATE.
                released_data = true;
            };
            if (auto* chunk = event->message_body_chunk()) {
                consume(chunk);
            } else {
                consume(event->tunnel_data());
            }
        } else if (const auto* tunnel_end = event->tunnel_end()) {
            if (auto* pending = find_pending(tunnel_end->stream_id()); pending != nullptr && !pending->failed()) {
                auto& state_value = *pending->response_->state_;
                if (!state_value.tunnel_ || !state_value.tunnel_->accepted_) {
                    pending->error_ = http_client_error::code_type::protocol_error;
                } else {
                    state_value.tunnel_->receive_ended_ = true;
                    state_value.data_signal_.notify();
                }
                pending->signal_.notify();
            }
            runtime.state_signal_.notify();
        } else if (auto* end = event->message_end()) {
            if (auto* pending = find_pending(end->stream_id());
                pending != nullptr && !pending->failed()) {
                try {
                    const bool content_semantics_present =
                        end->content_semantics() == http2_message_content_semantics::content;
                    auto response_trailers = std::move(*end).take_trailers();
                    auto& state_value = *pending->response_->state_;
                    state_value.trailers_.clear();
                    state_value.trailers_.reserve(response_trailers.size());
                    for (const auto& trailer : response_trailers) {
                        state_value.trailers_.push_back(http_header::copy_of(
                            trailer.name(), trailer.value(), state_value.resource_));
                    }
                    if (pending->timeout_ == nullptr) {
                        std::terminate();
                    }
                    if (pending->timeout_->expired()) {
                        throw http_client_error(http_client_error::code_type::timeout,
                            "HTTP/2 response body decoding timed out");
                    }
                    decode_http_client_response_content_encoding(
                        *pending->response_->state_, content_semantics_present,
                        config_.max_response_bytes_);
                    if (pending->timeout_->expired()) {
                        throw http_client_error(http_client_error::code_type::timeout,
                            "HTTP/2 response body decoding timed out");
                    }
                    pending->complete_ = true;
                } catch (...) {
                    fail_pending(*pending, end->stream_id(), std::current_exception(), false);
                }
                if (pending->complete_) {
                    pending->signal_.notify();
                }
            }
            runtime.state_signal_.notify();
        } else if (const auto* closed = event->stream_closed()) {
            if (auto* pending = find_pending(closed->stream_id());
                pending != nullptr && !pending->complete_ && !pending->failed() &&
                !pending->retryable_) {
                const auto& state_value = *pending->response_->state_;
                if (!state_value.tunnel_ || !state_value.tunnel_->accepted_ || !state_value.tunnel_->receive_ended_ || !state_value.tunnel_->output_.end_requested_) {
                    pending->error_ = http_client_error::code_type::protocol_error;
                    pending->signal_.notify();
                }
            }
            runtime.state_signal_.notify();
        } else if (const auto* unprocessed = event->request_unprocessed()) {
            if (auto* pending = find_pending(unprocessed->stream_id());
                pending != nullptr && !pending->complete_ && !pending->failed()) {
                pending->retryable_ = true;
                pending->signal_.notify();
            }
            runtime.draining_ = true;
            runtime.state_signal_.notify();
        } else if (event->goaway() != nullptr) {
            runtime.draining_ = true;
            runtime.state_signal_.notify();
        }
    }
    for (const auto stream_id : connection.http2_->take_drained_data_streams()) {
        if (auto* pending = find_pending(stream_id); pending != nullptr) {
            pending->signal_.notify();
        }
    }
    if (released_data || connection.http2_->wants_write()) {
        runtime.write_signal_.notify();
    }
}

void http_client_pool::fail_http2_session(connection_type& connection, std::uint64_t generation,
    std::error_code transport_error, const std::exception_ptr& failure) noexcept {
    auto& runtime = *connection.http2_runtime_;
    if (runtime.generation_ != generation || runtime.failed_) {
        return;
    }
    runtime.failed_ = true;
    runtime.draining_ = true;
    connection.connected_ = false;
    (void)connection.transport_.clear_deadline();
    std::error_code ignored;
    connection.transport_.resolver().cancel();
    (void)connection.transport_.stream().lowest_layer().cancel(ignored);
    (void)connection.transport_.stream().lowest_layer().close(ignored);
    auto pending_error = http_client_error::code_type::io_error;
    switch (connection.transport_.abort_reason()) {
        case client_abort_reason::timeout:
            pending_error = http_client_error::code_type::timeout;
            break;
        case client_abort_reason::cancelled:
            pending_error = http_client_error::code_type::cancelled;
            break;
        case client_abort_reason::closing:
            pending_error = http_client_error::code_type::closing;
            break;
        case client_abort_reason::none:
            if (transport_error == std::errc::timed_out) {
                pending_error = http_client_error::code_type::timeout;
            } else if (transport_error == std::errc::protocol_error) {
                pending_error = http_client_error::code_type::protocol_error;
            } else {
                pending_error = connection.transport_.error_code(transport_error);
            }
            break;
    }
    for (auto* pending : runtime.pending_) {
        if (!pending->complete_ && !pending->failed() && !pending->retryable_) {
            if (failure != nullptr) {
                pending->failure_ = failure;
            } else {
                pending->error_ = pending_error;
            }
            pending->signal_.notify();
        }
    }
    runtime.write_signal_.notify();
    runtime.state_signal_.notify();
}

void http_client_pool::finish_http2_session_task(
    connection_type& connection, std::uint64_t generation) noexcept {
    auto& runtime = *connection.http2_runtime_;
    if (runtime.generation_ != generation || runtime.session_tasks_ == 0) {
        std::terminate();
    }
    --runtime.session_tasks_;
    if (runtime.session_tasks_ == 0) {
        runtime.running_ = false;
    }
    runtime.state_signal_.notify();
}

task<void> http_client_pool::run_http2_reader(connection_type& connection, std::uint64_t generation) {
    struct finish final {
        http_client_pool& pool_;
        connection_type& connection_;
        std::uint64_t generation_;
        ~finish() {
            pool_.finish_http2_session_task(connection_, generation_);
        }
    } finish_value{*this, connection, generation};
    std::array<char, 16384> input{};
    try {
        auto& runtime = *connection.http2_runtime_;
        while (runtime.generation_ == generation && !runtime.failed_) {
            asio_completion<std::size_t> completion =
                config_.scheme_ == http_scheme::https
                    ? co_await ruvia::async_asio<std::size_t>([&connection, &input](auto handler) mutable {
                          connection.transport_.stream().async_read_some(
                              asio::buffer(input), std::move(handler));
                      })
                    : co_await ruvia::async_asio<std::size_t>([&connection, &input](auto handler) mutable {
                          connection.transport_.stream().next_layer().async_read_some(
                              asio::buffer(input), std::move(handler));
                      });
            if (completion.error_code() || completion.result() == 0) {
                fail_http2_session(connection, generation,
                    completion.error_code() ? completion.error_code()
                                            : std::make_error_code(std::errc::connection_reset));
                co_return;
            }
            wire_counters_.received_ += completion.result();
            const auto bytes_value = std::string_view(input.data(), completion.result());
            for (;;) {
                const auto status = connection.http2_->feed(bytes_value);
                drain_http2_events(connection);
                if (status == http2_feed_result::protocol_failure ||
                    connection.http2_->connection_error()) {
                    fail_http2_session(
                        connection, generation, std::make_error_code(std::errc::protocol_error));
                    co_return;
                }
                if (status != http2_feed_result::events_pending) {
                    break;
                }
            }
        }
    } catch (...) {
        fail_http2_session(connection, generation, {}, std::current_exception());
    }
}

task<void> http_client_pool::run_http2_writer(connection_type& connection, std::uint64_t generation) {
    struct finish final {
        http_client_pool& pool_;
        connection_type& connection_;
        std::uint64_t generation_;
        ~finish() {
            pool_.finish_http2_session_task(connection_, generation_);
        }
    } finish_value{*this, connection, generation};
    std::pmr::string output(resource_);
    try {
        auto& runtime = *connection.http2_runtime_;
        for (;;) {
            while (runtime.generation_ == generation && !runtime.failed_ && connection.http2_ &&
                   connection.http2_->wants_write()) {
                const auto pending = connection.http2_->pending_output();
                output.assign(pending);
                (void)connection.http2_->consume_output(pending.size());
                const ruvia::operation_timeout write_timeout(config_.write_timeout_);
                if (!connection.transport_.arm_deadline(write_timeout, client_deadline_kind::socket)) {
                    fail_http2_session(
                        connection, generation, std::make_error_code(std::errc::timed_out));
                    co_return;
                }
                asio_completion<std::size_t> completion =
                    config_.scheme_ == http_scheme::https
                        ? co_await ruvia::async_asio<std::size_t>(
                              [&connection, &output](auto handler) mutable {
                                  asio::async_write(
                                      connection.transport_.stream(), asio::buffer(output), std::move(handler));
                              })
                        : co_await ruvia::async_asio<std::size_t>(
                              [&connection, &output](auto handler) mutable {
                                  asio::async_write(connection.transport_.stream().next_layer(),
                                      asio::buffer(output), std::move(handler));
                              });
                const bool timed_out = connection.transport_.clear_deadline() || write_timeout.expired();
                if (timed_out) {
                    fail_http2_session(
                        connection, generation, std::make_error_code(std::errc::timed_out));
                    co_return;
                }
                if (completion.error_code()) {
                    fail_http2_session(connection, generation, completion.error_code());
                    co_return;
                }
                wire_counters_.sent_ += completion.result();
            }
            if (runtime.generation_ != generation || runtime.failed_) {
                co_return;
            }
            co_await runtime.write_signal_.wait();
        }
    } catch (...) {
        fail_http2_session(connection, generation, {}, std::current_exception());
    }
}

void http_client_pool::submit_http2_reset(connection_type& connection, std::uint32_t stream_id) noexcept {
    if (stream_id == 0 || !connection.http2_) {
        return;
    }
    auto& runtime = *connection.http2_runtime_;
    try {
        (void)connection.http2_->submit_reset(stream_id, http2_error_code::cancel);
        runtime.write_signal_.notify();
    } catch (...) {
        fail_http2_session(connection, runtime.generation_, {}, std::current_exception());
    }
}

void http_client_pool::cancel_http2_stream(
    connection_type& connection, std::uint64_t request_id, client_abort_reason reason) noexcept {
    auto& runtime = *connection.http2_runtime_;
    const auto match = std::ranges::find_if(runtime.pending_,
        [request_id](const http2_pending_stream_type* pending) { return pending->request_id_ == request_id; });
    if (match == runtime.pending_.end()) {
        return;
    }
    auto& pending = **match;
    if (pending.complete_ || pending.failed() || pending.retryable_) {
        return;
    }
    pending.error_ = reason == client_abort_reason::timeout     ? http_client_error::code_type::timeout
                     : reason == client_abort_reason::cancelled ? http_client_error::code_type::cancelled
                                                                : http_client_error::code_type::closing;
    submit_http2_reset(connection, pending.stream_id_);
    pending.signal_.notify();
    runtime.state_signal_.notify();
}

void http_client_pool::http2_pending_registration_type::reset() noexcept {
    if (!active_) {
        return;
    }
    active_ = false;
    pool_.remove_http2_pending(connection_, pending_);
}

void http_client_pool::remove_http2_pending(
    connection_type& connection, http2_pending_stream_type& pending) noexcept {
    auto& runtime = *connection.http2_runtime_;
    if (!pending.complete_ && !pending.retryable_) {
        submit_http2_reset(connection, pending.stream_id_);
    }
    release_response_data(*pending.response_->state_);
    const auto match = std::ranges::find(runtime.pending_, &pending);
    if (match == runtime.pending_.end()) {
        std::terminate();
    }
    runtime.pending_.erase(match);
    runtime.state_signal_.notify();
    if (runtime.draining_ && runtime.pending_.empty()) {
        std::error_code ignored;
        (void)connection.transport_.stream().lowest_layer().cancel(ignored);
        (void)connection.transport_.stream().lowest_layer().close(ignored);
        connection.connected_ = false;
        runtime.write_signal_.notify();
    }
}

task<void> http_client_pool::wait_for_http2_session_stop(
    connection_type& connection, const ruvia::operation_timeout& timeout, stop_token stop_token_value) {
    auto& runtime = *connection.http2_runtime_;
    worker_timer_registration deadline_timer;
    if (const auto remaining = timeout.remaining()) {
        if (remaining->count() == 0) {
            throw http_client_error(
                http_client_error::code_type::timeout, "HTTP/2 session shutdown wait timed out");
        }
        (worker_).schedule_timer(deadline_timer, worker_timer_deadline_after(*remaining), [&runtime](worker_timer_outcome outcome) noexcept {
            if (outcome == worker_timer_outcome::expired) {
                runtime.state_signal_.notify();
            }
        });
    }
    std::uint64_t cancellation_id = 0;
    if (stop_token_value.stoppable()) {
        if (runtime.state_cancellation_waiters_++ == 0) {
            runtime.state_cancellation_id_ = cancellation_target_->next_operation_id();
        }
        cancellation_id = runtime.state_cancellation_id_;
    }
    struct shared_cancellation_wait final {
        http2_runtime_type& runtime_;
        std::uint64_t id_;
        ~shared_cancellation_wait() {
            if (id_ != 0) {
                if (runtime_.state_cancellation_waiters_ == 0 || runtime_.state_cancellation_id_ != id_) {
                    std::terminate();
                }
                if (--runtime_.state_cancellation_waiters_ == 0) {
                    runtime_.state_cancellation_id_ = 0;
                }
            }
        }
    } shared_wait{runtime, cancellation_id};
    auto cancellation = worker_cancellation_registration<http_client_cancellation_target>::observe(cancellation_target_, cancellation_id);
    cancellation.arm(stop_token_value);
    while (runtime.session_tasks_ != 0 || !runtime.pending_.empty()) {
        if (stop_token_value.stop_requested()) {
            throw http_client_error(
                http_client_error::code_type::cancelled, "HTTP/2 session shutdown wait cancelled");
        }
        if (timeout.expired()) {
            throw http_client_error(
                http_client_error::code_type::timeout, "HTTP/2 session shutdown wait timed out");
        }
        co_await runtime.state_signal_.wait();
    }
}

task<void> http_client_pool::execute_http2(connection_type& connection,
    const http_client_request_storage& request, const ruvia::operation_timeout& timeout, stop_token stop_token_value,
    http_client_response& response) {
    std::pmr::vector<http_header_view> headers(resource_);
    auto source_value = http_client_request_storage_access::view(request, headers);
    std::pmr::string cookie_header(resource_);
    policy_.append_headers(request, headers, cookie_header);
    source_value.headers_ = std::span<const http_header_view>(headers);
    auto authority = http2_authority(config_, resource_);
    const auto* body = source_value.content_.borrowed_bytes();
    const auto content = request.upload() != nullptr ? ::ruvia::http2_request_content::streaming(request.upload()->config_.content_length_) : body ? ::ruvia::http2_request_content::known_length(body->value().size())
                                                                                                                                                   : ::ruvia::http2_request_content::none();

    for (int attempt_value = 0; attempt_value < 2; ++attempt_value) {
        auto& runtime = *connection.http2_runtime_;
        if (!connection.http2_ || runtime.failed_ || runtime.draining_) {
            if (runtime.draining_ && runtime.pending_.empty()) {
                std::error_code ignored;
                (void)connection.transport_.stream().lowest_layer().cancel(ignored);
                (void)connection.transport_.stream().lowest_layer().close(ignored);
                connection.connected_ = false;
                runtime.write_signal_.notify();
            }
            co_await wait_for_http2_session_stop(connection, timeout, stop_token_value);
            co_await ensure_connected(connection, timeout, timeout, stop_token_value);
            if (connection.protocol_ != wire_protocol_type::http2) {
                throw http_client_error(http_client_error::code_type::protocol_unavailable,
                    "upstream no longer negotiated HTTP/2");
            }
        }

        http2_pending_stream_type pending(worker_, response);
        pending.timeout_ = &timeout;
        pending.request_id_ = ++runtime.next_request_id_;
        if (pending.request_id_ == 0) {
            pending.request_id_ = ++runtime.next_request_id_;
        }
        response.state_->transport_ = http_client_response_transport::http2;
        response.state_->request_method_ = classify_http_method(request.method());
        response.state_->connection_index_ =
            static_cast<std::size_t>(&connection - connections_.data());
        response.state_->request_id_ = pending.request_id_;
        runtime.pending_.push_back(&pending);
        http2_pending_registration_type pending_registration(*this, connection, pending);
        worker_timer_registration deadline_timer;
        if (const auto remaining = timeout.remaining()) {
            (worker_).schedule_timer(deadline_timer, worker_timer_deadline_after(*remaining), [this, &connection, request_id = pending.request_id_](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    cancel_http2_stream(connection, request_id, client_abort_reason::timeout);
                }
            });
        }
        worker_cancellation_registration cancellation(cancellation_target_, pending.cancellation_id_);
        response.state_->cancellation_id_ = cancellation.id();
        cancellation.arm(stop_token_value);

        for (;;) {
            if (pending.failed()) {
                break;
            }
            if (timeout.expired()) {
                pending.error_ = http_client_error::code_type::timeout;
                break;
            }
            const auto submitted = request.is_tunnel()
                                       ? (request.tunnel_protocol().empty()
                                                 ? connection.http2_->submit_request_head(http2_connect_request_head_view{.authority_ = borrowed_text(request.tunnel_authority()), .headers_ = headers})
                                                 : connection.http2_->submit_request_head(http2_extended_connect_request_head_view{.protocol_ = borrowed_text(request.tunnel_protocol()), .scheme_ = config_.scheme_ == http_scheme::https ? "https" : "http", .authority_ = borrowed_text(request.tunnel_authority()), .target_ = source_value.target_, .headers_ = headers}))
                                       : connection.http2_->submit_request_head(::ruvia::http2_regular_request_head_view{
                                             .method_ = source_value.method_,
                                             .scheme_ = config_.scheme_ == http_scheme::https ? "https" : "http",
                                             .authority_ = borrowed_text(std::string_view(authority)),
                                             .target_ = source_value.target_,
                                             .headers_ = headers,
                                             .content_ = content,
                                             .expectation_ = request.upload() != nullptr ? request.upload()->config_.expectation_ : http_client_request_expectation::none});
            if (const auto* accepted = submitted.submitted()) {
                pending.stream_id_ = accepted->stream_id();
                response.state_->stream_id_ = pending.stream_id_;
                if (body && !body->value().empty()) {
                    const auto status = connection.http2_->submit_data(
                        pending.stream_id_, body->value(), http2_end_stream::end_stream);
                    if (status != http2_data_submit_status::accepted &&
                        status != http2_data_submit_status::queued) {
                        pending.error_ = http_client_error::code_type::protocol_error;
                    }
                }
                runtime.write_signal_.notify();
                break;
            }
            const auto error = submitted.failure()->error();
            if (error == http2_request_head_submit_error::peer_stream_limit_reached ||
                error == http2_request_head_submit_error::local_stream_capacity_reached) {
                co_await runtime.state_signal_.wait();
                if (pending.failed()) {
                    break;
                }
                continue;
            }
            if (error == http2_request_head_submit_error::connection_unavailable) {
                // GOAWAY may have been consumed during the preface, or local
                // stream IDs may be exhausted (no GOAWAY at all). Either way this
                // session admits no new streams: drain it so the retry opens a
                // new connection (RFC 9113 §5.1.1, §6.8).
                pending.retryable_ = true;
                runtime.draining_ = true;
                runtime.state_signal_.notify();
            } else {
                pending.error_ = http_client_error::code_type::invalid_request;
            }
            break;
        }

        if (auto* upload = request.output(); upload != nullptr && pending.stream_id_ != 0 && !pending.failed() && !pending.retryable_) {
            upload->wake_target_ = &pending.signal_;
            upload->wake_ = [](void* target) noexcept { static_cast<worker_signal*>(target)->notify(); };
            struct upload_wake_guard {
                http_client_output_queue& upload_;
                ~upload_wake_guard() {
                    upload_.wake_ = nullptr;
                    upload_.wake_target_ = nullptr;
                }
            } wake_guard{*upload};
            worker_timer_registration continue_timer;
            if (request.upload() != nullptr && !request.upload()->content_released_) {
                (worker_).schedule_timer(continue_timer, worker_timer_deadline_after(request.upload()->config_.continue_timeout_), [upload, policy = request.upload()](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired && !upload->stopped_) {
                        policy->content_released_ = true;
                        upload->notify_data();
                    }
                });
            }
            while (!upload->ended_ && !upload->stopped_ && !pending.complete_ && !pending.failed() && !pending.retryable_) {
                if (request.is_tunnel() ? !request.tunnel()->accepted_ : !request.upload()->content_released_) {
                    co_await pending.signal_.wait();
                    continue;
                }
                continue_timer.cancel();
                if (!request.is_tunnel()) {
                    const auto released = connection.http2_->release_request_content(pending.stream_id_);
                    if (released == http2_request_content_release_status::closed) {
                        break;
                    }
                }
                if (upload->chunk_ready_) {
                    const auto submitted = connection.http2_->submit_data(pending.stream_id_, upload->chunk_, http2_end_stream::keep_open);
                    if (submitted == http2_data_submit_status::backpressured || submitted == http2_data_submit_status::expectation_pending) {
                        co_await pending.signal_.wait();
                        continue;
                    }
                    if (submitted != http2_data_submit_status::accepted && submitted != http2_data_submit_status::queued) {
                        pending.error_ = http_client_error::code_type::invalid_request;
                        submit_http2_reset(connection, pending.stream_id_);
                        break;
                    }
                    runtime.write_signal_.notify();
                    while (connection.http2_->has_queued_data(pending.stream_id_) && !pending.failed() && !pending.retryable_ && !upload->stopped_) {
                        co_await pending.signal_.wait();
                    }
                    if (!pending.failed() && !pending.retryable_ && !upload->stopped_) {
                        upload->acknowledge_chunk();
                    }
                } else if (upload->end_requested_) {
                    std::pmr::vector<http_header_view> trailers(response.state_->resource_);
                    bool finish_accepted{};
                    if (request.is_tunnel()) {
                        const auto finished = connection.http2_->submit_data(pending.stream_id_, {}, http2_end_stream::end_stream);
                        finish_accepted = finished == http2_data_submit_status::accepted || finished == http2_data_submit_status::queued;
                    } else {
                        for (const auto& field : request.upload()->trailers_) {
                            trailers.emplace_back(field.name(), field.value());
                        }
                        const auto finished = connection.http2_->finish_request(pending.stream_id_, trailers);
                        finish_accepted = finished == http2_finish_request_status::accepted || finished == http2_finish_request_status::queued;
                    }
                    if (!finish_accepted) {
                        pending.error_ = http_client_error::code_type::invalid_request;
                        submit_http2_reset(connection, pending.stream_id_);
                        break;
                    }
                    runtime.write_signal_.notify();
                    while (connection.http2_->has_queued_data(pending.stream_id_) && !pending.failed() && !pending.retryable_ && !upload->stopped_) {
                        co_await pending.signal_.wait();
                    }
                    if (!pending.failed() && !pending.retryable_ && !upload->stopped_) {
                        upload->finish();
                    }
                } else {
                    co_await pending.signal_.wait();
                }
            }
        }
        while (!pending.complete_ && !pending.failed() && !pending.retryable_) {
            if (request.tunnel() != nullptr && request.tunnel()->accepted_ && request.tunnel()->receive_ended_ && request.tunnel()->output_.ended_) {
                pending.complete_ = true;
                break;
            }
            co_await pending.signal_.wait();
        }
        deadline_timer.cancel();
        const bool retryable = pending.retryable_;
        const auto error = pending.error_;
        const auto failure = pending.failure_;
        pending_registration.reset();
        if (retryable && request.output() == nullptr && attempt_value == 0 && !timeout.expired()) {
            continue;
        }
        if (retryable) {
            throw http_client_error(http_client_error::code_type::protocol_error,
                "HTTP/2 request was not processed after GOAWAY");
        }
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        if (error) {
            switch (*error) {
                case http_client_error::code_type::timeout:
                    throw http_client_error(*error, "HTTP/2 request timed out");
                case http_client_error::code_type::cancelled:
                    throw http_client_error(*error, "HTTP/2 request cancelled");
                case http_client_error::code_type::response_too_large:
                    throw http_client_error(*error, "HTTP/2 response exceeds configured byte limit");
                case http_client_error::code_type::closing:
                    throw http_client_error(*error, "HTTP client pool is closing");
                case http_client_error::code_type::tls_failed:
                    throw http_client_error(*error, "HTTP/2 TLS connection failed");
                case http_client_error::code_type::io_error:
                    throw http_client_error(*error, "HTTP/2 connection failed");
                case http_client_error::code_type::protocol_error:
                    throw http_client_error(*error, "HTTP/2 protocol failed");
                default:
                    throw http_client_error(*error, "HTTP/2 stream failed");
            }
        }
        co_return;
    }
    throw http_client_error(http_client_error::code_type::protocol_error, "HTTP/2 request retry exhausted");
}

}  // namespace ruvia::detail
