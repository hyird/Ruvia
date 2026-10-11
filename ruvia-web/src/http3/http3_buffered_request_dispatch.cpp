#include "http3/http3_buffered_request_dispatch.h"

#include <array>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <variant>

#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/web/error.h"

#include "http3/http3_response_stream_sink.h"
#include "http3/http3_server_connection.h"
#include "router/route_endpoint.h"
#include "router/route_resolution.h"
#include "router/route_table.h"
#include "server/http_buffered_response.h"
#include "server/http_response_stream_dispatch.h"

namespace ruvia::detail {

http3_buffered_request_dispatch::http3_buffered_request_dispatch(http3_server_connection& owner,
    std::uint64_t stream_id, std::optional<std::uint64_t> push_id, bool received_early_data,
    connection_scanner::entry_type& scanner_entry)
    : owner_(owner),
      session_(owner.session_),
      routes_(owner.routes_),
      worker_(owner.worker_),
      services_(owner.services_.with_inbound_buffer_pool(*owner.session_.inbound_buffer_pool())),
      options_(owner.options_),
      stream_id_(stream_id),
      push_id_(push_id),
      received_early_data_(received_early_data),
      start_(std::chrono::steady_clock::now()),
      scanner_entry_(scanner_entry),
      active_request_body_(worker_.resource()),
      tunnel_input_available_(services_.worker()),
      tunnel_output_available_(services_.worker()),
      interim_output_(worker_.resource(), this, [](void* raw, const http_interim_response_head& head) -> task<void> {
          return static_cast<http3_buffered_request_dispatch*>(raw)->write_interim_response(head);
      }),
      connection_advertisements_(worker_.resource(), this, [](void* raw, std::span<const std::string_view> origins) -> task<void> {
          auto& dispatch = *static_cast<http3_buffered_request_dispatch*>(raw);
          if (!dispatch.session_.queue_origin_advertisement(origins)) {
              throw std::invalid_argument("HTTP/3 ORIGIN advertisement rejected");
          }
          co_return; }, nullptr),
      push_output_(worker_.resource(), this, [](void* raw, http_push_request_view request) -> task<bool> {
          auto& dispatch = *static_cast<http3_buffered_request_dispatch*>(raw);
          if (dispatch.push_id_ || dispatch.response_aborted() || dispatch.stream_output_ended_) {
              co_return false;
          }
          co_return co_await dispatch.owner_.push_request(dispatch.stream_id_, request); }) {}

http3_buffered_request_dispatch::~http3_buffered_request_dispatch() {
    if (handler_active_) {
        std::terminate();
    }
    disarm_peer_transport_fin_timeout();
    release_dispatch_storage();
}

task<http3_buffered_request_dispatch::prepare_status_type> http3_buffered_request_dispatch::prepare() & {
    if (!on_worker()) {
        co_return prepare_status_type::wrong_worker;
    }
    if (state_ == state_type::cancelled) {
        co_return prepare_status_type::cancelled;
    }
    if (state_ != state_type::cold) {
        co_return prepare_status_type::already_prepared;
    }
    if (cancellation_requested()) {
        cancel();
        co_return prepare_status_type::cancelled;
    }

    state_ = state_type::preparing;
    auto acquired = session_.acquire_request(stream_id_);
    if (!acquired) {
        state_ = state_type::failed;
        co_return prepare_status_type::request_unavailable;
    }
    lease_.emplace(std::move(*acquired));

    try {
        request_memory_.emplace(worker_);
        combined_worker_and_request_stop_ =
            combine_stop_tokens(services_.get_stop_token(), request_stop_source_.token());
        // Admission arms this deadline in place when a handler deadline applies.
        request_deadline_.emplace(combined_worker_and_request_stop_);
        // Tunnel I/O waits on worker signals, so stop must wake both directions.
        request_deadline_->token().register_callback(tunnel_stop_registration_, [this]() noexcept {
            tunnel_input_available_.notify();
            tunnel_output_available_.notify();
        });
        const auto& request = lease_->request().request();
        const bool upstream_declared_early_data = request.header("early-data").has_value();
        request_services_.emplace(services_
                .with_request_channels({
                    .interim_output_ = &interim_output_,
                    .connection_advertisements_ = &connection_advertisements_,
                    .push_output_ = push_id_ ? nullptr : &push_output_,
                    .priority_update_ = session_.request_priority_update(stream_id_),
                })
                .with_early_data_info({received_early_data_, upstream_declared_early_data}));
        if (const auto* trailers = session_.request_trailers(stream_id_)) {
            *request_services_ = request_services_->with_request_trailers(*trailers);
        }
        if (session_.streaming_request(stream_id_)) {
            streaming_access::emplace_body_reader(request_body_reader_, this,
                [](void* raw) -> task<std::optional<std::span<const std::byte>>> {
                    return static_cast<http3_buffered_request_dispatch*>(raw)->read_request_body();
                });
        }
        if (cancellation_requested()) {
            cancel();
            release_dispatch_storage();
            co_return prepare_status_type::cancelled;
        }
        state_ = state_type::prepared;
        co_return prepare_status_type::prepared;
    } catch (...) {
        fail(std::current_exception());
        co_return prepare_status_type::failed;
    }
}

task<http3_buffered_request_dispatch::run_status_type> http3_buffered_request_dispatch::run_handler() & {
    if (!on_worker()) {
        co_return run_status_type::wrong_worker;
    }
    if (state_ == state_type::cold) {
        const auto prepared = co_await prepare();
        if (prepared == prepare_status_type::cancelled) {
            co_return run_status_type::cancelled;
        }
        if (prepared != prepare_status_type::prepared) {
            co_return run_status_type::failed;
        }
    } else if (state_ == state_type::cancelled) {
        co_return run_status_type::cancelled;
    } else if (state_ != state_type::prepared) {
        co_return state_ == state_type::failed ? run_status_type::failed : run_status_type::already_run;
    }

    if (cancellation_requested()) {
        cancel();
        release_dispatch_storage();
        co_return run_status_type::cancelled;
    }

    state_ = state_type::running;
    handler_active_ = true;
    run_status_type result_value = run_status_type::failed;
    try {
        result_value = co_await run_handler_inner();
    } catch (...) {
        failure_ = std::current_exception();
        if (stream_output_active_ && !peer_field_section_rejected_) {
            abort_tunnel();
        }
    }
    // The child frame (including router responses and preparation temporaries)
    // is gone before releasing any resource those objects may reference.
    handler_active_ = false;
    if (cancellation_requested() || (result_value == run_status_type::cancelled && !peer_field_section_rejected_)) {
        cancel();
        release_dispatch_storage();
        co_return run_status_type::cancelled;
    }
    if (peer_field_section_rejected_) {
        result_value = run_status_type::peer_field_section_limit;
    }
    if (result_value == run_status_type::tunnel_complete || result_value == run_status_type::output_complete) {
        state_ = state_type::complete;
        release_dispatch_storage();
        co_return result_value;
    }
    if (result_value == run_status_type::peer_field_section_limit) {
        stream_output_active_ = false;
        state_ = state_type::peer_limit_rejected;
        release_dispatch_storage();
        co_return result_value;
    }
    if (result_value != run_status_type::response_ready) {
        fail(failure_);
        co_return result_value;
    }
    state_ = state_type::output_ready;
    co_return run_status_type::response_ready;
}

task<http3_buffered_request_dispatch::run_status_type> http3_buffered_request_dispatch::run_handler_inner() {
    const auto& request = lease_->request().request();
    const auto& resolution = lease_->resolution();
    admission_.emplace(admit_http_request(
        {
            .request_ = request,
            .resolution_ = resolution,
            .coding_ = http_response_coding_selection_for(request),
            .resource_ = request_memory_->resource(),
            .received_early_data_ = received_early_data_,
            .start_ = start_,
        },
        owner_.admission_policy_, *request_services_, request_deadline_));
    auto& admission = *admission_;
    deadline_armed_ = admission.deadline_armed();
    const auto& services = admission.services();
    auto dispatch_services = services;
    if (request_body_reader_) {
        dispatch_services = dispatch_services.with_streaming_request_body(*request_body_reader_);
    }
    const auto* resolved = admission.resolved();

    bool websocket_response = false;
    std::optional<http_response> selected_response;
    switch (admission.endpoint()) {
        case http_request_endpoint::too_early:
        case http_request_endpoint::rejection:
        case http_request_endpoint::not_acceptable:
            selected_response.emplace(co_await respond_http_request_rejection(
                routes_, request, *request_memory_, *admission.rejection(), services));
            break;
        case http_request_endpoint::websocket: {
            websocket_response = true;
            auto result_value = co_await run_websocket_handler(dispatch_services);
            if (const auto* terminal = std::get_if<run_status_type>(&result_value)) {
                co_return *terminal;
            }
            selected_response.emplace(std::get<http_response>(std::move(result_value)));
            break;
        }
        case http_request_endpoint::tunnel: {
            auto result_value = co_await run_tunnel_handler(dispatch_services);
            if (const auto* terminal = std::get_if<run_status_type>(&result_value)) {
                co_return *terminal;
            }
            selected_response.emplace(std::get<http_response>(std::move(result_value)));
            break;
        }
        case http_request_endpoint::response_stream: {
            http3_response_stream_sink sink_value(*this, services_.worker(), request.known_method(),
                resolved->route().endpoint().response_stream()->kind(), worker_.resource(),
                admission.response_stream_coding(), admission.coding_availability());
            auto result_value = co_await dispatch_response_stream_with(sink_value, routes_, request, *resolved,
                *request_memory_, dispatch_services, [this]() noexcept { return response_aborted(); });
            if (peer_field_section_rejected_) {
                co_return run_status_type::peer_field_section_limit;
            }
            if (result_value.peer_aborted_before_commit() != nullptr) {
                co_return run_status_type::cancelled;
            }
            if (const auto status = result_value.committed_status()) {
                record_access(*status);
                if (const auto* failed = result_value.failed_after_commit()) {
                    options_.connection_failure_.invoke(admission.client_address(), failed->exception());
                    abort_tunnel();
                    co_return run_status_type::cancelled;
                }
                co_return run_status_type::output_complete;
            }
            if (auto* route = result_value.route_response()) {
                selected_response.emplace(std::move(*route).take_response());
            } else if (auto* recovered = result_value.recovered_failure()) {
                selected_response.emplace(std::move(*recovered).take_response());
            } else {
                throw std::logic_error("HTTP/3 response stream dispatch has no terminal outcome");
            }
            break;
        }
        case http_request_endpoint::stream_body:
        case http_request_endpoint::buffered:
            selected_response.emplace(co_await routes_.dispatch_buffered_response(request, resolution,
                *request_memory_, options_.document_root_.binding(), dispatch_services, admission.static_file_mode()));
            break;
    }
    if (peer_field_section_rejected_) {
        co_return run_status_type::peer_field_section_limit;
    }
    // A websocket rejection does not consume the tunnel body or enter the file
    // response path; only its uncommitted buffered preparation joins here.
    if (!websocket_response && session_.streaming_request(stream_id_)) {
        co_await drain_request_body();
    }
    if (!websocket_response && cancellation_requested()) {
        co_return run_status_type::cancelled;
    }
    response_.emplace(std::move(*selected_response));

    auto preparation = co_await admission.tail().prepare(*response_, options_, routes_, *request_memory_, services,
        {.terminal_stop_ = &services.get_stop_token(),
            .recovery_mode_ = websocket_response ? buffered_response_recovery_mode::immediately_disabled
                                                 : buffered_response_recovery_mode::negotiated_then_disabled});
    if (!preparation || cancellation_requested()) {
        co_return run_status_type::cancelled;
    }

    if (!websocket_response && (response_->file_body() || response_->has_multipart_file_body()) &&
        preparation->write_plan_.send_body() && preparation->write_plan_.content_length() != 0) {
        std::exception_ptr file_failure;
        {
            response_file_source source(*response_, response_file_io{.pool_ = options_.blocking_pool_,
                                                        .worker_ = services_.worker(),
                                                        .stop_ = request_deadline_->token()});
            auto first = co_await source.next();
            if (first.kind() == response_body_chunk::kind_type::failed) {
                file_failure = std::make_exception_ptr(std::runtime_error("HTTP/3 response file could not be read"));
            } else {
                try {
                    co_return co_await write_file_response(source, first, preparation->write_plan_);
                } catch (...) {
                    file_failure = std::current_exception();
                }
            }
        }
        if (peer_field_section_rejected_) {
            co_return run_status_type::peer_field_section_limit;
        }
        if (cancellation_requested()) {
            co_return run_status_type::cancelled;
        }
        if (interim_output_.final_committed()) {
            options_.connection_failure_.invoke(admission.client_address(), file_failure);
            abort_tunnel();
            co_return run_status_type::cancelled;
        }
        // Nothing of the file response was committed: answer the failure.
        response_.reset();
        response_.emplace(co_await routes_.handle_exception(request, *request_memory_, file_failure, services));
        admission.tail().disable_response_coding();
        preparation = co_await admission.tail().prepare(*response_, options_, routes_, *request_memory_, services,
            {.terminal_stop_ = &services.get_stop_token()});
        if (!preparation || cancellation_requested()) {
            co_return run_status_type::cancelled;
        }
    }
    // Interim and push prelude output already started this stream's ordered
    // publication; the final response follows on the same path.
    if (stream_output_active_) {
        co_return co_await write_buffered_after_interim(preparation->write_plan_);
    }
    auto encoded_head = session_.encode_response_head(stream_id_, *response_, preparation->write_plan_);
    if (encoded_head.index() != 0) {
        co_return std::get<1>(encoded_head).kind_ == http3_response_head_error::peer_field_section_limit
            ? run_status_type::peer_field_section_limit
            : run_status_type::failed;
    }
    auto output = http3_buffered_response_output::create(
        *response_, preparation->write_plan_, std::move(std::get<0>(encoded_head)), owner_.output(), stream_id_);
    if (output.index() != 0) {
        co_return std::get<1>(output) == http3_buffered_response_error::file_body_unsupported
            ? run_status_type::file_payload_unsupported
            : run_status_type::failed;
    }
    output_.emplace(std::move(std::get<0>(output)));
    co_return run_status_type::response_ready;
}

task<std::optional<std::span<const std::byte>>> http3_buffered_request_dispatch::read_request_body() {
    for (;;) {
        if (response_aborted()) {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
        const auto failure = session_.streaming_body_failure(stream_id_);
        if (failure != http3_sans_io_session_engine::rejection_type::none) {
            throw http_error({.status_ = failure == http3_sans_io_session_engine::rejection_type::body_too_large ? http_status::content_too_large : http_status::service_unavailable,
                .code_ = "request_body_unavailable",
                .message_ = "HTTP/3 request body limit exceeded"});
        }
        // clear() keeps capacity: the next read reuses this buffer.
        active_request_body_.clear();
        active_request_body_.resize(16 * 1024);
        const auto read = session_.read_tunnel_data(stream_id_, std::span<char>(active_request_body_.data(), active_request_body_.size()));
        active_request_body_.resize(read.bytes_);
        if (read.reset_) {
            throw std::system_error(std::make_error_code(std::errc::connection_reset));
        }
        if (read.bytes_ != 0) {
            owner_.notify_activation();
            co_return std::as_bytes(std::span<const char>(active_request_body_.data(), active_request_body_.size()));
        }
        if (read.ended_) {
            co_return std::nullopt;
        }
        co_await tunnel_input_available_.wait();
    }
}

task<void> http3_buffered_request_dispatch::drain_request_body() {
    for (;;) {
        if (response_aborted()) {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
        std::array<char, 4096> bytes_value{};
        const auto read = session_.read_tunnel_data(stream_id_, bytes_value);
        if (read.reset_) {
            throw std::system_error(std::make_error_code(std::errc::connection_reset));
        }
        if (read.bytes_ != 0) {
            owner_.notify_activation();
            continue;
        }
        if (read.ended_) {
            co_return;
        }
        co_await tunnel_input_available_.wait();
    }
}

task<http3_buffered_request_dispatch::run_status_type> http3_buffered_request_dispatch::write_file_response(
    response_file_source& source, response_body_chunk chunk, http_buffered_response_write_plan plan) {
    auto encoded = session_.encode_response_head(stream_id_, *response_, plan);
    if (encoded.index() != 0) {
        if (std::get<1>(encoded).kind_ == http3_response_head_error::peer_field_section_limit) {
            reject_peer_field_section();
        }
        throw std::invalid_argument("invalid HTTP/3 file response head");
    }
    auto& head = std::get<0>(encoded);
    // SETTINGS can arrive while the file is opened off the worker. Recheck the
    // final decoded size before committing any response bytes.
    if (!response_field_section_allowed(head.headers_.decoded_field_section_size())) {
        reject_peer_field_section();
    }
    http3_data_write_plan data(head.body_plan_, plan.content_length());
    commit_final_response();
    co_await publish_headers(head.headers_);
    for (;;) {
        if (chunk.kind() == response_body_chunk::kind_type::failed) {
            throw std::runtime_error("HTTP/3 response file failed after its head was committed");
        }
        if (chunk.kind() == response_body_chunk::kind_type::end) {
            break;
        }
        const auto bytes_value = chunk.bytes();
        if (!bytes_value.empty()) {
            const auto planned = data.plan_chunk(std::span<const char>(bytes_value.data(), bytes_value.size()), false);
            if (planned.index() != 0) {
                throw std::length_error("invalid HTTP/3 file content length");
            }
            co_await publish_data(std::get<0>(planned));
            if (data.commit_payload(bytes_value.size(), false).index() != 0) {
                std::terminate();
            }
        }
        chunk = co_await source.next();
    }
    if (data.plan_chunk({}, true).index() != 0) {
        throw std::length_error("HTTP/3 file content length incomplete");
    }
    co_await finish_response();
    if (data.commit_payload(0, true).index() != 0) {
        std::terminate();
    }
    record_access(response_->status());
    co_return run_status_type::output_complete;
}

task<void> http3_buffered_request_dispatch::write_interim_response(const http_interim_response_head& head) {
    auto encoded = session_.encode_interim_response_head(stream_id_, head);
    if (encoded.index() != 0) {
        if (std::get<1>(encoded).kind_ == http3_response_head_error::peer_field_section_limit) {
            reject_peer_field_section();
        }
        throw std::invalid_argument("invalid HTTP/3 interim response head");
    }
    if (!response_field_section_allowed(std::get<0>(encoded).headers_.decoded_field_section_size())) {
        reject_peer_field_section();
    }
    co_await publish_headers(std::get<0>(encoded).headers_);
}

task<http3_buffered_request_dispatch::run_status_type> http3_buffered_request_dispatch::write_buffered_after_interim(
    http_buffered_response_write_plan plan) {
    auto encoded = session_.encode_response_head(stream_id_, *response_, plan);
    if (encoded.index() != 0) {
        if (std::get<1>(encoded).kind_ == http3_response_head_error::peer_field_section_limit) {
            reject_peer_field_section();
        }
        throw std::invalid_argument("invalid HTTP/3 buffered response head");
    }
    auto& head = std::get<0>(encoded);
    if (!response_field_section_allowed(head.headers_.decoded_field_section_size())) {
        reject_peer_field_section();
    }
    commit_final_response();
    co_await publish_headers(head.headers_);
    if (plan.send_body()) {
        http3_data_write_plan data(head.body_plan_, plan.content_length());
        const auto body = response_->body_bytes();
        if (!body.empty()) {
            const auto planned = data.plan_chunk(std::span<const char>(body.data(), body.size()), false);
            if (planned.index() != 0) {
                throw std::length_error("invalid HTTP/3 buffered content length");
            }
            co_await publish_data(std::get<0>(planned));
            if (data.commit_payload(body.size(), false).index() != 0) {
                std::terminate();
            }
        }
        if (data.plan_chunk({}, true).index() != 0) {
            throw std::length_error("HTTP/3 buffered content length incomplete");
        }
        co_await finish_response();
        if (data.commit_payload(0, true).index() != 0) {
            std::terminate();
        }
    } else {
        co_await finish_response();
    }
    record_access(response_->status());
    co_return run_status_type::output_complete;
}

http3_buffered_request_dispatch::publication_demand_type
http3_buffered_request_dispatch::publication_demand() const noexcept {
    if (!on_worker()) {
        return publication_demand_type::wrong_worker;
    }
    if (stream_output_active_) {
        if (state_ == state_type::cancelled || cancellation_requested()) {
            return publication_demand_type::local_cancelled;
        }
        if (stream_output_ended_) {
            return publication_demand_type::local_complete;
        }
        if (output_pending()) {
            return publication_demand_type::data;
        }
        if (tunnel_establish_pending_ || fin_pending_) {
            return publication_demand_type::control;
        }
        return publication_demand_type::not_ready;
    }
    switch (state_) {
        case state_type::complete:
            return publication_demand_type::local_complete;
        case state_type::cancelled:
            return publication_demand_type::local_cancelled;
        case state_type::peer_limit_rejected:
            return publication_demand_type::local_peer_limit_rejected;
        case state_type::failed:
            return publication_demand_type::local_failed;
        case state_type::cold:
        case state_type::preparing:
        case state_type::prepared:
        case state_type::running:
            return publication_demand_type::not_ready;
        case state_type::output_ready:
        case state_type::publishing:
            break;
    }
    if (cancellation_requested()) {
        return publication_demand_type::local_cancelled;
    }
    if (!output_) {
        return publication_demand_type::local_failed;
    }
    // The peer limit is advisory: gate only before the first accepted byte.
    // Once a HEADERS prefix is published it cannot be withdrawn or rewritten.
    if (!output_published_ && exceeds_peer_field_section_limit()) {
        return publication_demand_type::local_peer_limit_rejected;
    }
    switch (output_->next_step()) {
        case http3_buffered_response_output::next_step_type::bytes:
            return publication_demand_type::data;
        case http3_buffered_response_output::next_step_type::fin:
            return publication_demand_type::control;
        case http3_buffered_response_output::next_step_type::complete:
        case http3_buffered_response_output::next_step_type::failed:
            return publication_demand_type::local_failed;
    }
    return publication_demand_type::local_failed;
}

http3_buffered_request_dispatch::publish_result_type http3_buffered_request_dispatch::publish_step() & noexcept {
    const auto demand = publication_demand();
    if (stream_output_active_ && (demand == publication_demand_type::data ||
                                     demand == publication_demand_type::control)) {
        return publish_stream_step(demand);
    }
    switch (demand) {
        case publication_demand_type::wrong_worker:
            return {publish_status_type::wrong_worker};
        case publication_demand_type::not_ready:
            return {publish_status_type::not_ready};
        case publication_demand_type::local_complete:
            return {publish_status_type::complete};
        case publication_demand_type::local_cancelled:
            cancel();
            return {publish_status_type::cancelled};
        case publication_demand_type::local_peer_limit_rejected:
            if (state_ != state_type::peer_limit_rejected) {
                state_ = state_type::peer_limit_rejected;
                release_dispatch_storage();
            }
            return {publish_status_type::peer_limit_rejected};
        case publication_demand_type::local_failed:
            if (state_ != state_type::failed) {
                fail();
            }
            return {publish_status_type::failed};
        case publication_demand_type::data:
        case publication_demand_type::control:
            break;
    }

    state_ = state_type::publishing;
    const auto publication = output_->publish_step();
    switch (publication.status_) {
        case http3_buffered_response_output::status_type::bytes:
            output_published_ = true;
            return {publish_status_type::bytes_published};
        case http3_buffered_response_output::status_type::fin:
            record_access(response_->status());
            state_ = state_type::complete;
            release_dispatch_storage();
            return {publish_status_type::fin_published};
        case http3_buffered_response_output::status_type::backpressured:
            return {publish_status_type::backpressured,
                publication.block_reason_ == http3_buffered_response_output::block_reason_type::control
                    ? publish_block_reason_type::control
                    : publish_block_reason_type::data};
        case http3_buffered_response_output::status_type::complete:
            state_ = state_type::complete;
            release_dispatch_storage();
            return {publish_status_type::complete};
        case http3_buffered_response_output::status_type::failed:
            fail();
            return {publish_status_type::failed};
    }
    fail();
    return {publish_status_type::failed};
}

http3_buffered_request_dispatch::publish_result_type
http3_buffered_request_dispatch::publish_stream_step(publication_demand_type demand) noexcept {
    auto& output = owner_.output();
    if (demand == publication_demand_type::data) {
        std::size_t accepted = 0;
        while (output_pending()) {
            auto& segment = pending_head_.empty() ? pending_body_ : pending_head_;
            const auto written = output.write(stream_id_, std::as_bytes(segment));
            segment = segment.subspan(written.accepted_);
            accepted += written.accepted_;
            if (!segment.empty()) {
                break;
            }
        }
        if (accepted == 0) {
            return {publish_status_type::backpressured, publish_block_reason_type::data};
        }
        if (!output_pending()) {
            // A CONNECT 2xx head is complete: permit tunnel input and HTTP
            // Datagrams before QUIC can send it, so a peer reacting to the 2xx
            // never races tunnel establishment.
            if (tunnel_establish_pending_ &&
                output.establish_tunnel(stream_id_) == http3_server_stream_output::status_type::accepted) {
                tunnel_establish_pending_ = false;
                tunnel_established_ = true;
            }
            tunnel_output_available_.notify();
        }
        return {publish_status_type::bytes_published};
    }

    if (tunnel_establish_pending_) {
        if (output.establish_tunnel(stream_id_) != http3_server_stream_output::status_type::accepted) {
            return {publish_status_type::backpressured, publish_block_reason_type::control};
        }
        tunnel_establish_pending_ = false;
        tunnel_established_ = true;
        tunnel_output_available_.notify();
        return {publish_status_type::control_published};
    }
    if (output.finish(stream_id_) != http3_server_stream_output::status_type::accepted) {
        return {publish_status_type::backpressured, publish_block_reason_type::control};
    }
    fin_pending_ = false;
    stream_output_ended_ = true;
    if (tunnel_established_) {
        arm_peer_transport_fin_timeout();
    }
    tunnel_output_available_.notify();
    return {publish_status_type::fin_published};
}

bool http3_buffered_request_dispatch::register_publication_deadline_callback(
    move_only_function<void()> callback_value) & {
    if (!on_worker()) {
        throw std::logic_error("HTTP/3 publication deadline registration must run on its worker");
    }
    if (state_ != state_type::output_ready || !callback_value) {
        throw std::logic_error("HTTP/3 publication deadline registration requires ready output");
    }
    if (!deadline_armed_) {
        return false;
    }
    if (publication_deadline_callback_registered_ || !request_deadline_) {
        throw std::logic_error("HTTP/3 publication deadline callback is already registered");
    }
    publication_deadline_callback_registered_ = true;
    request_deadline_->token().register_callback(
        publication_deadline_registration_, std::move(callback_value));
    return true;
}

http3_buffered_request_dispatch::cancellation_reason_type
http3_buffered_request_dispatch::cancellation_reason() const noexcept {
    latch_cancellation_reason();
    return cancellation_reason_;
}

void http3_buffered_request_dispatch::cancel() & noexcept {
    if (!on_worker()) {
        std::terminate();
    }
    if (state_ == state_type::complete || state_ == state_type::failed ||
        state_ == state_type::cancelled || state_ == state_type::peer_limit_rejected) {
        return;
    }
    // Set the terminal state first: stop callbacks may resume a suspended handler.
    disarm_peer_transport_fin_timeout();
    cancellation_requested_ = true;
    latch_cancellation_reason();
    state_ = state_type::cancelled;
    tunnel_aborted_ = true;
    request_stop_source_.request_stop();
    tunnel_input_available_.notify();
    tunnel_output_available_.notify();
    if (!handler_active_) {
        release_dispatch_storage();
    }
}

task<void> http3_buffered_request_dispatch::publish_headers(const http3_headers_frame& frame) {
    pending_head_ = frame.frame();
    co_await publish_output();
}

task<void> http3_buffered_request_dispatch::publish_data(const http3_data_write_plan::chunk_type& chunk) {
    if (!chunk.emits_data_) {
        co_return;
    }
    pending_head_ = std::span<const char>(chunk.frame_header_.data(), chunk.frame_header_size_);
    pending_body_ = chunk.payload_;
    co_await publish_output();
}

task<void> http3_buffered_request_dispatch::publish_response_bytes(std::span<const char> bytes_value) {
    pending_head_ = bytes_value;
    co_await publish_output();
}

task<void> http3_buffered_request_dispatch::publish_output() {
    if (!co_await await_output()) {
        throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    }
}

task<bool> http3_buffered_request_dispatch::await_output() {
    // The staged spans borrow the suspended writer's storage: never keep them
    // past this frame, whatever the outcome.
    struct pending_guard final {
        http3_buffered_request_dispatch& owner_;
        ~pending_guard() {
            owner_.pending_head_ = {};
            owner_.pending_body_ = {};
        }
    } guard{*this};
    if (response_aborted() || !on_worker() || stream_output_ended_ || fin_pending_) {
        co_return false;
    }
    stream_output_active_ = true;
    notify_output();
    while (output_pending()) {
        if (response_aborted()) {
            co_return false;
        }
        co_await tunnel_output_available_.wait();
    }
    co_return !response_aborted();
}

void http3_buffered_request_dispatch::reject_peer_field_section() {
    // The router can recover an uncommitted writer exception into an application
    // response. Preserve this transport refusal independently of that recovery;
    // the connection owner resets only this stream after joining its handler.
    peer_field_section_rejected_ = true;
    throw std::length_error("HTTP/3 response exceeds peer field section limit");
}

bool http3_buffered_request_dispatch::response_field_section_allowed(std::size_t decoded_size) const noexcept {
    const auto limit = session_.peer_max_field_section_size();
    return !limit || decoded_size <= *limit;
}

task<void> http3_buffered_request_dispatch::finish_response() {
    if (stream_output_ended_) {
        co_return;
    }
    if (!stream_output_active_ || fin_pending_ || response_aborted()) {
        throw std::logic_error("HTTP/3 response cannot be concluded in this state");
    }
    fin_pending_ = true;
    notify_output();
    while (!stream_output_ended_) {
        if (response_aborted()) {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
        co_await tunnel_output_available_.wait();
    }
}

void http3_buffered_request_dispatch::record_access(http_status_code status) const noexcept {
    if (admission_) {
        admission_->tail().record_access(options_.access_log_, status);
    }
}

bool http3_buffered_request_dispatch::on_worker() const noexcept {
    return services_.worker().is_current();
}

void http3_buffered_request_dispatch::notify_output() noexcept {
    owner_.tunnel_output_ready(stream_id_);
}

bool http3_buffered_request_dispatch::cancellation_requested() const noexcept {
    latch_cancellation_reason();
    return cancellation_reason_ != cancellation_reason_type::none ||
           (request_deadline_ && request_deadline_->token().stop_requested());
}

void http3_buffered_request_dispatch::latch_cancellation_reason() const noexcept {
    if (cancellation_reason_ != cancellation_reason_type::none) {
        return;
    }
    if (services_.get_stop_token().stop_requested()) {
        cancellation_reason_ = cancellation_reason_type::worker_stop;
    } else if (cancellation_requested_ || request_stop_source_.stop_requested()) {
        cancellation_reason_ = cancellation_reason_type::explicit_value;
    } else if (request_deadline_ && request_deadline_->token().stop_requested()) {
        cancellation_reason_ = request_deadline_->exceeded()
                                   ? cancellation_reason_type::deadline
                                   : cancellation_reason_type::worker_stop;
    }
}

bool http3_buffered_request_dispatch::exceeds_peer_field_section_limit() const noexcept {
    const auto limit = session_.peer_max_field_section_size();
    return limit.has_value() && output_ &&
           std::cmp_greater(output_->decoded_field_section_size(), *limit);
}

void http3_buffered_request_dispatch::fail(std::exception_ptr failure) noexcept {
    disarm_peer_transport_fin_timeout();
    if (handler_active_) {
        std::terminate();
    }
    if (failure != nullptr) {
        failure_ = std::move(failure);
    }
    state_ = state_type::failed;
    release_dispatch_storage();
}

void http3_buffered_request_dispatch::release_dispatch_storage() noexcept {
    if (handler_active_) {
        std::terminate();
    }
    publication_deadline_registration_.reset();
    tunnel_stop_registration_.reset();
    latch_cancellation_reason();
    admission_.reset();
    request_services_.reset();
    request_body_reader_.reset();
    std::pmr::string(active_request_body_.get_allocator()).swap(active_request_body_);
    request_deadline_.reset();
    combined_worker_and_request_stop_ = stop_token{};
    output_.reset();
    response_.reset();
    request_memory_.reset();

    const bool had_lease = lease_.has_value();
    if (had_lease && (tunnel_aborted_ || peer_field_section_rejected_)) {
        // A local abort can finish before peer FIN reaches the input owner.
        // Retire the protocol request while its lease still pins the head; the
        // lease then frees this stream without waiting for more peer input.
        (void)session_.cancel_request(stream_id_);
    }
    lease_.reset();
    if (had_lease) {
        // Response FIN completes only the send direction. The input owner keeps
        // an open receive direction alive until validated FIN or cancellation.
        (void)session_.release(stream_id_);
    }
}

}  // namespace ruvia::detail
