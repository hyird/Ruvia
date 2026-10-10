#include "http3/http3_buffered_request_dispatch.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <variant>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_var_int.h"
#include "ruvia/http/http3_websocket_handshake.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/web/error.h"

#include "context/context_access.h"
#include "http/http_tunnel_session.h"
#include "http3/http3_response_stream_sink.h"
#include "router/route_early_data.h"
#include "router/route_endpoint.h"
#include "router/route_resolution.h"
#include "router/route_table.h"
#include "server/http_buffered_response.h"
#include "server/http_file_open.h"
#include "server/http_response_stream_dispatch.h"
#include "websocket/http_websocket_connection.h"
#include "websocket/http_websocket_session.h"
#include "websocket/websocket_response_headers.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] std::int64_t steady_now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] bool has_unsupported_websocket_version(const http_request& request) noexcept {
    std::size_t version_count = 0;
    std::string_view version;
    for (const auto& header : request.headers()) {
        if (!http_ascii_equals_ignore_case(header.name(), "sec-websocket-version")) {
            continue;
        }
        if (++version_count != 1) {
            return false;
        }
        version = header.value();
    }
    return version_count == 1 && version != "13";
}

class http3_tunnel_transport final {
public:
    [[nodiscard]] task<std::optional<http_datagram_input>> read_datagram_input() {
        return dispatch_.read_datagram_input();
    }
    [[nodiscard]] http_datagram_session_config datagram_config() const {
        return dispatch_.datagram_config();
    }
    void send_datagram(std::span<const std::byte> bytes_value) {
        dispatch_.send_datagram(bytes_value);
    }
    explicit http3_tunnel_transport(http3_buffered_request_dispatch& dispatch) noexcept
        : dispatch_(dispatch) {}
    [[nodiscard]] task<http_stream_read_result> read_more(std::pmr::string& buffer) {
        return dispatch_.read_tunnel(buffer);
    }
    [[nodiscard]] task<std::error_code> write_bytes(std::string_view bytes_value, http_stream_end end) {
        return dispatch_.write_tunnel(bytes_value, end);
    }
    void abort() noexcept {
        dispatch_.abort_tunnel();
    }

private:
    http3_buffered_request_dispatch& dispatch_;
};

class http3_websocket_transport final {
public:
    explicit http3_websocket_transport(http3_buffered_request_dispatch& dispatch) noexcept
        : dispatch_(dispatch) {}

    [[nodiscard]] asio::any_io_executor executor() const noexcept {
        return dispatch_.executor();
    }

    [[nodiscard]] task<http_stream_read_result> read_more(std::pmr::string& buffer) {
        return dispatch_.read_tunnel(buffer);
    }

    [[nodiscard]] task<std::error_code> write_bytes(std::string_view bytes_value,
        websocket_transport_disposition disposition) {
        return dispatch_.write_tunnel(bytes_value, disposition == websocket_transport_disposition::end_transport ? http_stream_end::end : http_stream_end::keep_open);
    }

    void abort() noexcept {
        dispatch_.abort_tunnel();
    }

private:
    http3_buffered_request_dispatch& dispatch_;
};

}  // namespace

http3_buffered_request_dispatch::http3_buffered_request_dispatch(
    http3_sans_io_session_engine& session_value, const route_table& routes_value, worker_memory& worker_value,
    context_services services, const http_server_options& options,
    http3_stream_buffer& outbound, http3_stream_id message_id,
    connection_scanner::entry_type& scanner_entry, asio::any_io_executor executor,
    http3_tunnel_callbacks tunnel_callbacks, std::uint64_t response_prelude_bytes)
    : session_(session_value),
      routes_(routes_value),
      worker_(worker_value),
      services_(services.with_inbound_buffer_pool(*session_value.inbound_buffer_pool())),
      options_(options),
      outbound_(outbound),
      message_id_(message_id),
      scanner_entry_(scanner_entry),
      executor_(std::move(executor)),
      tunnel_callbacks_(tunnel_callbacks),
      active_request_body_(worker_.resource()),
      tunnel_input_available_(services_.worker()),
      tunnel_output_available_(services_.worker()),
      stream_frame_(worker_.resource()),
      tunnel_data_frame_(worker_.resource()),
      interim_output_(worker_.resource(), this, [](void* raw, const http_interim_response_head& head) -> task<void> {
          co_await static_cast<http3_buffered_request_dispatch*>(raw)->write_interim_response(head);
      }),
      connection_advertisements_(worker_.resource(), this, [](void* raw, std::span<const std::string_view> origins) -> task<void> {
          auto& dispatch = *static_cast<http3_buffered_request_dispatch*>(raw);
          if (!dispatch.session_.queue_origin_advertisement(origins)) {
              throw std::invalid_argument("HTTP/3 ORIGIN advertisement rejected");
          }
          co_return; }, nullptr),
      push_output_(worker_.resource(), this, [](void* raw, http_push_request_view request) -> task<bool> {
          auto& dispatch = *static_cast<http3_buffered_request_dispatch*>(raw);
          if (dispatch.tunnel_callbacks_.push_ == nullptr || dispatch.message_id_.push_id_ || dispatch.response_aborted() || dispatch.stream_output_ended_) {
              co_return false;
          }
          co_return co_await dispatch.tunnel_callbacks_.push_(dispatch.tunnel_callbacks_.context_, dispatch.message_id_.stream_id_, request); }) {
    stream_output_active_ = response_prelude_bytes != 0;
    published_wire_bytes_ = response_prelude_bytes;
    stream_published_wire_bytes_ = response_prelude_bytes;
}

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
    auto acquired = session_.acquire_request(message_id_.stream_id_);
    if (!acquired) {
        state_ = state_type::failed;
        co_return prepare_status_type::request_unavailable;
    }
    lease_.emplace(std::move(*acquired));

    try {
        request_memory_.emplace(worker_);
        combined_worker_and_request_stop_ =
            combine_stop_tokens(services_.get_stop_token(), request_stop_source_.token());
        request_deadline_.emplace(combined_worker_and_request_stop_);
        // Tunnel I/O waits on worker signals, so stop must wake both directions.
        request_deadline_->token().register_callback(tunnel_stop_registration_, [this]() noexcept {
            tunnel_input_available_.notify();
            tunnel_output_available_.notify();
        });
        const auto* resolved = lease_->resolution().resolved();
        const auto route_deadline = resolved != nullptr ? resolved->route().deadline_ms() : 0;
        const auto handler_deadline = effective_handler_deadline(
            options_.deadline_ ? std::optional{options_.deadline_->handler_} : std::nullopt,
            route_deadline);
        if (handler_deadline > std::chrono::milliseconds::zero()) {
            request_deadline_->arm(services_.worker(), handler_deadline);
            deadline_armed_ = true;
        }
        const auto& request = lease_->request().request();
        const bool upstream_declared_early_data = request.header("early-data").has_value();
        request_services_.emplace(services_.with_request_deadline(*request_deadline_)
                .with_interim_output(interim_output_)
                .with_connection_advertisements(connection_advertisements_)
                .with_early_data_info({message_id_.received_early_data_,
                    upstream_declared_early_data}));
        if (tunnel_callbacks_.push_ != nullptr && !message_id_.push_id_) {
            *request_services_ = request_services_->with_push_output(push_output_);
        }
        if (const auto* trailers = session_.request_trailers(message_id_.stream_id_)) {
            *request_services_ = request_services_->with_request_trailers(*trailers);
        }
        if (const auto* priority = session_.request_priority_update(message_id_.stream_id_)) {
            *request_services_ = request_services_->with_request_priority_update(*priority);
        }
        if (session_.streaming_request(message_id_.stream_id_)) {
            streaming_access::emplace_body_reader(request_body_reader_, this,
                [](void* raw) -> task<std::optional<std::span<const std::byte>>> { co_return co_await static_cast<http3_buffered_request_dispatch*>(raw)->read_request_body(); });
            *request_services_ = request_services_->with_streaming_request_body(*request_body_reader_);
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
    const auto* resolved = resolution.resolved();
    const bool early_request_safe = !message_id_.received_early_data_ ||
                                    early_data_request_allowed(request.known_method(), !request.body_bytes().empty(), resolution);
    const bool websocket_response = early_request_safe && resolved != nullptr &&
                                    resolved->route().endpoint().get_websocket() != nullptr;
    std::optional<http_response> selected_response;
    if (!early_request_safe) {
        selected_response.emplace(http_response::options_type{.resource_ = worker_.resource()});
        selected_response->status(http_status::too_early);
        selected_response->header("content-length", "0");
    } else if (websocket_response) {
        auto result_value = co_await run_websocket_handler();
        if (const auto* terminal = std::get_if<run_status_type>(&result_value)) {
            co_return *terminal;
        }
        selected_response.emplace(std::get<http_response>(std::move(result_value)));
    }
    const auto coding_negotiation = http_response_coding_for(request);
    auto coding_policy = http_response_coding_policy::disabled();
    if (const auto* selection = coding_negotiation.selected()) {
        coding_policy = http_response_coding_policy::selected(*selection);
    } else {
        coding_policy = http_response_coding_policy::no_acceptable_coding();
    }

    if (selected_response.has_value()) {
        // Early rejection and uncommitted websocket responses bypass ordinary routing.
    } else if (resolved != nullptr && resolved->route().endpoint().tunnel() != nullptr) {
        selected_response = co_await run_tunnel_handler();
        if (!selected_response.has_value()) {
            co_return cancellation_requested() || tunnel_aborted_ ? run_status_type::cancelled : run_status_type::tunnel_complete;
        }
    } else if (resolved != nullptr && resolved->route().endpoint().response_stream() != nullptr) {
        if (coding_policy.selection() == nullptr) {
            selected_response.emplace(co_await routes_.handle_error(request, *request_memory_,
                http_error_info({.status_ = http_status::not_acceptable, .code_ = "not_acceptable", .message_ = "no acceptable response content coding"}), *request_services_));
        } else {
            http3_response_stream_sink sink_value(*this, services_.worker(), request.known_method(), resolved->route().endpoint().response_stream()->kind(),
                worker_.resource(), *coding_policy.selection(), options_.compression_.has_value() ? http_response_coding_availability::identity_and_compression : http_response_coding_availability::identity_only);
            auto result_value = co_await dispatch_response_stream_with(sink_value, routes_, request, *resolved, *request_memory_, *request_services_, [this]() noexcept { return response_aborted(); });
            if (peer_field_section_rejected_) {
                co_return run_status_type::peer_field_section_limit;
            }
            if (result_value.peer_aborted_before_commit() != nullptr) {
                co_return run_status_type::cancelled;
            }
            if (const auto status = result_value.committed_status()) {
                if (const auto* failed = result_value.failed_after_commit()) {
                    options_.connection_failure_.invoke(services_.resolve_conn_info(request).client().address(), failed->exception());
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
        }
    } else {
        selected_response.emplace(co_await routes_.dispatch_buffered_response(request, resolution,
            *request_memory_, options_.document_root_.binding(), *request_services_,
            services_.precompressed_static_files() ? static_file_selection_mode::precompressed
                                                   : static_file_selection_mode::identity_only));
    }
    if (peer_field_section_rejected_) {
        co_return run_status_type::peer_field_section_limit;
    }
    auto response = std::move(*selected_response);
    // websocket rejection does not consume the tunnel body or enter the file
    // response driver; only its uncommitted buffered preparation joins here.
    if (!websocket_response && session_.streaming_request(message_id_.stream_id_)) {
        co_await drain_request_body();
    }
    if (!websocket_response && cancellation_requested()) {
        co_return run_status_type::cancelled;
    }
    response_.emplace(std::move(response));

    auto preparation = co_await prepare_application_response(
        request, coding_policy, *response_, options_, routes_, *request_memory_, *request_services_,
        {.terminal_stop_ = &request_services_->get_stop_token(),
            .recovery_mode_ = websocket_response ? buffered_response_recovery_mode::immediately_disabled : buffered_response_recovery_mode::negotiated_then_disabled});
    if (!preparation || cancellation_requested()) {
        co_return run_status_type::cancelled;
    }

    if (!websocket_response && response_->file_body().has_value() && preparation->write_plan_.send_body() && preparation->write_plan_.content_length() != 0) {
        std::exception_ptr file_failure;
        try {
            co_return co_await write_file_response(preparation->write_plan_);
        } catch (...) {
            file_failure = std::current_exception();
        }
        if (peer_field_section_rejected_) {
            co_return run_status_type::peer_field_section_limit;
        }
        if (interim_output_.final_committed()) {
            options_.connection_failure_.invoke(services_.resolve_conn_info(request).client().address(), file_failure);
            abort_tunnel();
            co_return run_status_type::cancelled;
        }
        response_.reset();
        response_.emplace(co_await routes_.handle_exception(request, *request_memory_, file_failure, *request_services_));
        preparation = co_await prepare_application_response(request, http_response_coding_policy::disabled(),
            *response_, options_, routes_, *request_memory_, *request_services_,
            {.terminal_stop_ = &request_services_->get_stop_token()});
        if (!preparation || cancellation_requested()) {
            co_return run_status_type::cancelled;
        }
    }
    // Interim/push prelude bytes already own the stream's send offset; only the
    // stream path accounts for them in its FIN (websocket rejection included).
    if (stream_output_active_) {
        co_return co_await write_buffered_after_interim(preparation->write_plan_);
    }
    auto encoded_head = session_.encode_response_head(message_id_.stream_id_, *response_, preparation->write_plan_);
    if ((encoded_head.index() != 0)) {
        co_return std::get<1>(encoded_head).kind_ == http3_response_head_error::peer_field_section_limit
            ? run_status_type::peer_field_section_limit
            : run_status_type::failed;
    }
    auto output = http3_buffered_response_output::create(
        *response_, preparation->write_plan_, std::move(std::get<0>(encoded_head)), worker_, outbound_, message_id_);
    if ((output.index() != 0)) {
        co_return std::get<1>(output) == http3_buffered_response_output_error::file_body_unsupported
            ? run_status_type::file_payload_unsupported
            : run_status_type::failed;
    }
    output_.emplace(std::move(std::get<0>(output)));
    co_return run_status_type::response_ready;
}

task<std::optional<std::span<const std::byte>>> http3_buffered_request_dispatch::read_request_body() {
    std::pmr::string(active_request_body_.get_allocator()).swap(active_request_body_);
    for (;;) {
        if (response_aborted()) {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
        const auto failure = session_.streaming_body_failure(message_id_.stream_id_);
        if (failure != http3_sans_io_session_engine::rejection_type::none) {
            throw http_error({.status_ = failure == http3_sans_io_session_engine::rejection_type::body_too_large ? http_status::content_too_large : http_status::service_unavailable,
                .code_ = "request_body_unavailable",
                .message_ = "HTTP/3 request body limit exceeded"});
        }
        active_request_body_.resize(16 * 1024);
        const auto read = session_.read_tunnel_data(message_id_.stream_id_, std::span<char>(active_request_body_.data(), active_request_body_.size()));
        active_request_body_.resize(read.bytes_);
        if (read.reset_) {
            throw std::system_error(std::make_error_code(std::errc::connection_reset));
        }
        if (read.bytes_ != 0) {
            if (tunnel_callbacks_.input_consumed_ != nullptr) {
                tunnel_callbacks_.input_consumed_(tunnel_callbacks_.context_);
            }
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
        const auto read = session_.read_tunnel_data(message_id_.stream_id_, bytes_value);
        if (read.reset_) {
            throw std::system_error(std::make_error_code(std::errc::connection_reset));
        }
        if (read.bytes_ != 0) {
            if (tunnel_callbacks_.input_consumed_ != nullptr) {
                tunnel_callbacks_.input_consumed_(tunnel_callbacks_.context_);
            }
            continue;
        }
        if (read.ended_) {
            co_return;
        }
        co_await tunnel_input_available_.wait();
    }
}

task<http3_buffered_request_dispatch::run_status_type> http3_buffered_request_dispatch::write_file_response(http_buffered_response_write_plan plan) {
    auto* pool = services_.get_blocking_pool();
    if (pool == nullptr) {
        pool = options_.blocking_pool_;
    }
    if (pool == nullptr) {
        throw http_error({.status_ = http_status::service_unavailable, .code_ = "file_io_unavailable", .message_ = "file output requires the server blocking pool"});
    }
    auto encoded = session_.encode_response_head(message_id_.stream_id_, *response_, plan);
    if ((encoded.index() != 0)) {
        if (std::get<1>(encoded).kind_ == http3_response_head_error::peer_field_section_limit) {
            reject_peer_field_section();
        }
        throw std::invalid_argument("invalid HTTP/3 file response head");
    }
    const auto file = *response_->file_body();
    auto input = co_await run_blocking(*pool, services_.worker(), request_deadline_->token(),
        [path = file.to_path(), size = file.size(), identity = file.identity()]() mutable {
            auto opened = open_response_file_input(http_response_file_view(path.c_str(), size, 0, size, identity));
            if (!opened) {
                throw std::runtime_error("HTTP/3 response file could not be opened or changed identity");
            }
            return opened;
        });
    // SETTINGS can arrive while the file open is offloaded. Recheck the final
    // decoded size before committing any response bytes.
    const auto peer_limit = session_.peer_max_field_section_size();
    if (peer_limit && std::get<0>(encoded).field_section_.decoded_field_section_size() > *peer_limit) {
        reject_peer_field_section();
    }
    http3_data_write_plan data(std::get<0>(encoded).body_plan_, plan.content_length());
    commit_final_response();
    co_await publish_response_frame(static_cast<std::uint64_t>(http3_frame_type::headers), std::get<0>(encoded).field_section_.field_section_);
    struct read_result final {
        response_file_input input_;
        std::array<char, 16 * 1024> bytes_{};
        std::size_t count_{};
    };
    for (std::size_t index = 0; index < response_->body_segment_count(); ++index) {
        const auto segment = response_->body_segment(index);
        if (!segment.file_) {
            std::size_t offset = 0;
            while (offset < segment.bytes_.size()) {
                const auto count = std::min<std::size_t>(16 * 1024, segment.bytes_.size() - offset);
                const auto bytes_value = segment.bytes_.substr(offset, count);
                const auto chunk = data.plan_chunk(std::span<const char>(bytes_value.data(), bytes_value.size()), false);
                if ((chunk.index() != 0)) {
                    throw std::length_error("invalid HTTP/3 multipart content length");
                }
                co_await publish_response_frame(static_cast<std::uint64_t>(http3_frame_type::data), std::get<0>(chunk).payload_);
                if ((data.commit_payload(bytes_value.size(), false).index() != 0)) {
                    std::terminate();
                }
                offset += count;
            }
            continue;
        }
        input = co_await run_blocking(*pool, services_.worker(), request_deadline_->token(),
            [source = std::move(input), offset = segment.file_->offset()]() mutable {
                source.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
                if (!source) {
                    throw std::runtime_error("HTTP/3 response file range seek failed");
                }
                return std::move(source);
            });
        std::uint64_t remaining = segment.file_->length();
        while (remaining != 0) {
            auto read = co_await run_blocking(*pool, services_.worker(), request_deadline_->token(),
                [source = std::move(input), remaining]() mutable {
                    read_result result_value{std::move(source)};
                    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(result_value.bytes_.size(), remaining));
                    result_value.input_.read(result_value.bytes_.data(), static_cast<std::streamsize>(count));
                    if (!result_value.input_ || result_value.input_.gcount() <= 0) {
                        throw std::runtime_error("HTTP/3 response file ended before its declared length");
                    }
                    result_value.count_ = static_cast<std::size_t>(result_value.input_.gcount());
                    return result_value;
                });
            input = std::move(read.input_);
            const auto chunk = data.plan_chunk(std::span<const char>(read.bytes_.data(), read.count_), false);
            if ((chunk.index() != 0)) {
                throw std::length_error("invalid HTTP/3 file content length");
            }
            co_await publish_response_frame(static_cast<std::uint64_t>(http3_frame_type::data), std::get<0>(chunk).payload_);
            if ((data.commit_payload(read.count_, false).index() != 0)) {
                std::terminate();
            }
            remaining -= read.count_;
        }
    }
    const auto matches = co_await run_blocking(*pool, services_.worker(), request_deadline_->token(),
        [source = std::move(input), identity = file.identity(), size = file.size()]() mutable { return source.matches_snapshot(identity, size); });
    if (!matches) {
        throw std::runtime_error("HTTP/3 response file changed while it was being sent");
    }
    if ((data.plan_chunk({}, true).index() != 0)) {
        std::terminate();
    }
    co_await finish_response();
    if ((data.commit_payload(0, true).index() != 0)) {
        std::terminate();
    }
    co_return run_status_type::output_complete;
}

task<std::optional<http_response>> http3_buffered_request_dispatch::run_tunnel_handler() {
    const auto& request = lease_->request().request();
    const auto& resolved = *lease_->resolution().resolved();
    const auto& endpoint = *resolved.route().endpoint().tunnel();
    const bool udp = endpoint.protocol() == "connect-udp";
    if (udp && (validate_http_connect_udp_request(request).index() != 0)) {
        co_return co_await routes_.handle_error(request, *request_memory_,
            http_error_info({.status_ = http_status::bad_request, .message_ = "invalid CONNECT-UDP request head"}), *request_services_);
    }
    std::optional<http_tunnel_session<http3_tunnel_transport>> tunnel_session;
    auto establish_and_run = [&](context& context_value) -> task<void> {
        auto response = context_access::streaming_head(context_value);
        if (udp) {
            auto negotiated = prepare_http_connect_udp_response(std::move(response), http_protocol_version::http3);
            if ((negotiated.index() != 0)) {
                throw std::invalid_argument("invalid CONNECT-UDP response metadata");
            }
            response = std::move(std::get<0>(negotiated));
        }
        auto head = session_.encode_connect_response_head(message_id_.stream_id_, response);
        if ((head.index() != 0)) {
            if (std::get<1>(head).kind_ == http3_response_head_error::peer_field_section_limit) {
                reject_peer_field_section();
            }
            throw std::invalid_argument("HTTP/3 CONNECT response head rejected");
        }
        if (!response_field_section_allowed(std::get<0>(head).field_section_.decoded_field_section_size())) {
            reject_peer_field_section();
        }
        std::pmr::vector<char> framed(std::get<0>(head).field_section_.field_section_.size() + http3_frame_header_max_bytes, worker_.resource());
        const auto frame_size = encode_http3_frame(framed, static_cast<std::uint64_t>(http3_frame_type::headers), std::get<0>(head).field_section_.field_section_);
        if ((frame_size.index() != 0)) {
            throw std::length_error("HTTP/3 CONNECT response framing failed");
        }
        framed.resize(std::get<0>(frame_size));
        if (tunnel_callbacks_.attach_scanner_ == nullptr ||
            !tunnel_callbacks_.attach_scanner_(tunnel_callbacks_.context_, message_id_.stream_id_, scanner_entry_)) {
            throw std::runtime_error("HTTP/3 tunnel scanner attachment failed");
        }
        context_access::mark_tunnel_handshake_started(context_value);
        if (const auto error = co_await publish_tunnel_handshake(framed); error) {
            throw std::system_error(error, "HTTP/3 CONNECT response publication");
        }
        tunnel_session.emplace(http3_tunnel_transport(*this), services_.worker(), *context_value.pool());
        co_await invoke_tunnel_handler(*tunnel_session, scanner_entry_, endpoint.handler(), context_value);
    };
    const auto terminal = make_callable_ref<void, context&>(establish_and_run);
    std::optional<http_response> response;
    std::exception_ptr failure;
    try {
        response = co_await routes_.dispatch_tunnel(request, resolved, *request_memory_, terminal, *request_services_);
    } catch (...) {
        failure = std::current_exception();
    }
    if (tunnel_session.has_value()) {
        co_await finish_tunnel_session(*tunnel_session, failure, options_.connection_failure_, services_.get_conn_info().remote().address(), scanner_entry_, endpoint.config().peer_transport_fin_timeout_);
        if (!cancellation_requested() && !tunnel_aborted_) {
            (void)co_await wait_tunnel_receive_end();
        }
        co_return std::nullopt;
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    co_return std::move(response);
}

task<http3_buffered_request_dispatch::websocket_dispatch_result_type>
http3_buffered_request_dispatch::run_websocket_handler() {
    const auto& request = lease_->request().request();
    const auto& resolution = lease_->resolution();
    const auto* resolved = resolution.resolved();
    if (resolved == nullptr || resolved->route().endpoint().get_websocket() == nullptr) {
        co_return run_status_type::failed;
    }
    const auto& endpoint = *resolved->route().endpoint().get_websocket();
    peer_transport_fin_timeout_ = endpoint.lifecycle().peer_transport_fin_timeout_;
    const auto protocol = lease_->request().extended_connect_protocol();
    const auto validation = validate_http3_websocket_handshake(
        request, protocol, !session_.tunnel_receive_ended(message_id_.stream_id_));

    if ((validation.index() != 0)) {
        const auto& failure = std::get<1>(validation);
        const auto unsupported = failure.kind() ==
                                     http3_websocket_handshake_failure::kind_type::unsupported_version ||
                                 (request.known_method() == http_known_method::connect &&
                                     http_ascii_equals_ignore_case(protocol, "websocket") &&
                                     has_unsupported_websocket_version(request));
        const auto protocol_error = failure.protocol_error();
        auto response = co_await routes_.handle_error(request, *request_memory_,
            http_error_info({.status_ = protocol_error.status(),
                .code_ = unsupported ? "websocket_version_unsupported"
                                     : "invalid_websocket_handshake",
                .message_ = protocol_error.what()}),
            *request_services_);
        if (unsupported) {
            response.remove_header("Sec-WebSocket-Version");
            response.header("Sec-WebSocket-Version", "13");
        } else {
            failure.apply_required_response_headers(response);
        }
        co_return std::move(response);
    }

    using connection_type = websocket_connection<http3_websocket_transport>;
    std::optional<connection_type> websocket_connection;
    auto upgrade_and_run = [&](context& context_value) -> task<void> {
        const auto response_headers_value = websocket_response_headers(context_value);
        auto handshake = make_http3_websocket_handshake(request, protocol, true,
            {.supported_subprotocols_ = endpoint.subprotocols(),
                .response_headers_ = response_headers_value,
                .resource_ = request_memory_->resource(),
                .deflate_ = endpoint.deflate()});
        if ((handshake.index() != 0)) {
            throw std::runtime_error("HTTP/3 WebSocket handshake construction failed");
        }
        if (tunnel_callbacks_.attach_scanner_ == nullptr ||
            !tunnel_callbacks_.attach_scanner_(
                tunnel_callbacks_.context_, message_id_.stream_id_, scanner_entry_)) {
            throw std::runtime_error("HTTP/3 WebSocket scanner attachment failed");
        }
        context_access::mark_websocket_handshake_started(context_value);
        if (const auto error = co_await publish_tunnel_handshake(std::get<0>(handshake).headers_frame()); error) {
            throw std::system_error(error, "failed to publish HTTP/3 WebSocket handshake");
        }
        websocket_connection.emplace(http3_websocket_transport{*this}, services_.worker(),
            scanner_entry_, endpoint.lifecycle(),
            protocol_byte_limit::limited(options_.max_websocket_message_bytes_),
            session_.inbound_buffer_pool(), std::string_view{}, std::get<0>(handshake).compression(),
            endpoint.deflate().compression_level_);
        co_await invoke_websocket_handler(*websocket_connection, scanner_entry_,
            endpoint.handler(), context_value);
    };
    const auto terminal = make_callable_ref<void, context&>(upgrade_and_run);
    std::optional<http_response> buffered;
    std::exception_ptr exception;
    try {
        buffered = co_await routes_.dispatch_websocket(
            request, *resolved, *request_memory_, terminal, *request_services_);
    } catch (...) {
        exception = std::current_exception();
    }

    if (websocket_connection.has_value()) {
        co_await finish_websocket_session(*websocket_connection, exception,
            options_.connection_failure_, services_.get_conn_info().remote().address());
        if (cancellation_requested()) {
            co_return run_status_type::cancelled;
        }
        const auto receive_ended = co_await wait_tunnel_receive_end();
        co_return receive_ended ? run_status_type::tunnel_complete : run_status_type::cancelled;
    }
    if (cancellation_requested()) {
        co_return run_status_type::cancelled;
    }
    if (exception != nullptr) {
        std::rethrow_exception(exception);
    }
    if (buffered.has_value()) {
        co_return std::move(*buffered);
    }
    co_return run_status_type::failed;
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
        if (outbound_.stopped()) {
            return publication_demand_type::local_buffer_stopped;
        }
        if (tunnel_established_control_pending_) {
            return publication_demand_type::control;
        }
        if (stream_frame_offset_ < stream_frame_.size() || tunnel_data_pending_) {
            return publication_demand_type::data;
        }
        if (tunnel_fin_pending_) {
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
    if (outbound_.stopped()) {
        return publication_demand_type::local_buffer_stopped;
    }
    if (!output_) {
        return publication_demand_type::local_failed;
    }
    // The peer limit is advisory: gate only before the first handoff. Once a
    // HEADERS prefix is published it cannot be withdrawn or rewritten.
    if (published_wire_bytes_ == 0 && exceeds_peer_field_section_limit()) {
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
        case publication_demand_type::local_buffer_stopped:
            fail();
            return {publish_status_type::failed};
        case publication_demand_type::local_peer_limit_rejected:
            if (state_ == state_type::peer_limit_rejected) {
                return {publish_status_type::peer_limit_rejected};
            }
            state_ = state_type::publishing;
            state_ = state_type::peer_limit_rejected;
            release_dispatch_storage();
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
    published_wire_bytes_ += publication.bytes_accepted_;
    switch (publication.status_) {
        case http3_buffered_response_output::status_type::bytes:
            return {publish_status_type::bytes_published, publication.bytes_accepted_};
        case http3_buffered_response_output::status_type::fin:
            state_ = state_type::complete;
            release_dispatch_storage();
            return {publish_status_type::fin_published};
        case http3_buffered_response_output::status_type::backpressured:
            return {publish_status_type::backpressured, 0,
                publication.block_reason_ == http3_buffered_response_output::block_reason_type::data
                    ? publish_block_reason_type::data
                : publication.block_reason_ == http3_buffered_response_output::block_reason_type::control
                    ? publish_block_reason_type::control
                    : publish_block_reason_type::none};
        case http3_buffered_response_output::status_type::complete:
            state_ = state_type::complete;
            release_dispatch_storage();
            return {publish_status_type::complete};
        case http3_buffered_response_output::status_type::failed:
            fail();
            return {publish_status_type::failed, publication.bytes_accepted_};
    }
    fail();
    return {publish_status_type::failed, publication.bytes_accepted_};
}

http3_buffered_request_dispatch::publish_result_type
http3_buffered_request_dispatch::publish_stream_step(publication_demand_type demand) noexcept {
    if (!on_worker()) {
        return {publish_status_type::wrong_worker};
    }
    if (demand == publication_demand_type::local_cancelled) {
        cancel();
        return {publish_status_type::cancelled};
    }
    if (demand == publication_demand_type::local_buffer_stopped) {
        cancel();
        return {publish_status_type::cancelled};
    }
    if (demand == publication_demand_type::not_ready) {
        return {publish_status_type::not_ready};
    }

    if (demand == publication_demand_type::data) {
        std::span<const std::byte> bytes;
        if (stream_frame_offset_ < stream_frame_.size()) {
            const auto count = (std::min)(http3_stream_buffer::max_block_bytes,
                stream_frame_.size() - stream_frame_offset_);
            bytes = std::as_bytes(std::span<const char>(
                stream_frame_.data() + stream_frame_offset_, count));
        } else if (tunnel_data_pending_) {
            bytes = std::as_bytes(std::span<const char>(
                tunnel_data_frame_.data(), tunnel_data_frame_.size()));
        } else {
            return {publish_status_type::not_ready};
        }
        if (bytes.size() > http3_var_int_max - stream_published_wire_bytes_) {
            return {publish_status_type::failed};
        }
        const auto result_value = outbound_.try_send(message_id_, bytes);
        if (result_value == http3_stream_buffer::send_result::full || result_value == http3_stream_buffer::send_result::no_block) {
            return {publish_status_type::backpressured, 0, publish_block_reason_type::data};
        }
        if (result_value != http3_stream_buffer::send_result::sent) {
            return {publish_status_type::failed};
        }
        stream_published_wire_bytes_ += bytes.size();
        published_wire_bytes_ += bytes.size();
        stream_frame_offset_ += bytes.size();
        if (stream_frame_offset_ == stream_frame_.size()) {
            tunnel_output_available_.notify();
        } else {
            tunnel_data_pending_ = false;
            tunnel_data_frame_.clear();
            tunnel_output_available_.notify();
        }
        return {publish_status_type::bytes_published, bytes.size()};
    }

    const bool establishing_tunnel = tunnel_established_control_pending_;
    const auto remaining_head_bytes = establishing_tunnel
                                          ? stream_frame_.size() - stream_frame_offset_
                                          : std::size_t{0};
    if (remaining_head_bytes > http3_var_int_max - stream_published_wire_bytes_) {
        return {publish_status_type::failed};
    }
    http3_stream_control event{
        .kind_ = establishing_tunnel ? http3_stream_control::kind::tunnel_established
                                     : http3_stream_control::kind::stream_fin,
        .id_ = message_id_,
        .value_ = stream_published_wire_bytes_ + remaining_head_bytes};
    const auto result_value = outbound_.try_send_control(event);
    if (result_value == http3_stream_buffer::control_result::full) {
        return {publish_status_type::backpressured, 0, publish_block_reason_type::control};
    }
    if (result_value != http3_stream_buffer::control_result::sent) {
        return {publish_status_type::failed};
    }
    if (establishing_tunnel) {
        tunnel_established_control_pending_ = false;
        tunnel_established_control_published_ = true;
        tunnel_output_available_.notify();
        notify_tunnel_output();
        return {publish_status_type::control_published};
    }
    tunnel_fin_pending_ = false;
    stream_output_ended_ = true;
    if (tunnel_established_control_published_) {
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

task<std::error_code> http3_buffered_request_dispatch::publish_tunnel_handshake(
    std::span<const char> headers_frame) {
    if (!on_worker() || headers_frame.empty() || interim_output_.final_committed() ||
        stream_frame_offset_ < stream_frame_.size() || tunnel_callbacks_.output_ready_ == nullptr) {
        co_return std::make_error_code(std::errc::invalid_argument);
    }
    if (headers_frame.size() > http3_var_int_max - stream_published_wire_bytes_) {
        co_return std::make_error_code(std::errc::value_too_large);
    }
    try {
        stream_frame_.assign(headers_frame.data(), headers_frame.size());
    } catch (...) {
        co_return std::make_error_code(std::errc::not_enough_memory);
    }
    commit_final_response();
    stream_output_active_ = true;
    stream_frame_offset_ = 0;
    // Admit the future HEAD byte barrier before any successful CONNECT bytes.
    // A peer can send its first native datagram immediately upon seeing 2xx.
    tunnel_established_control_pending_ = true;
    notify_tunnel_output();
    while (!tunnel_established_control_published_ ||
           stream_frame_offset_ < stream_frame_.size()) {
        if (cancellation_requested() || tunnel_aborted_) {
            co_return std::make_error_code(std::errc::operation_canceled);
        }
        co_await tunnel_output_available_.wait();
    }
    if (cancellation_requested() || tunnel_aborted_) {
        co_return std::make_error_code(std::errc::operation_canceled);
    }
    co_return std::error_code{};
}

task<void> http3_buffered_request_dispatch::write_interim_response(const http_interim_response_head& head) {
    auto encoded = session_.encode_interim_response_head(message_id_.stream_id_, head);
    if ((encoded.index() != 0)) {
        if (std::get<1>(encoded).kind_ == http3_response_head_error::peer_field_section_limit) {
            reject_peer_field_section();
        }
        throw std::invalid_argument("invalid HTTP/3 interim response head");
    }
    if (!response_field_section_allowed(std::get<0>(encoded).field_section_.decoded_field_section_size())) {
        reject_peer_field_section();
    }
    co_await publish_response_frame(static_cast<std::uint64_t>(http3_frame_type::headers), std::get<0>(encoded).field_section_.field_section_);
}

task<http3_buffered_request_dispatch::run_status_type> http3_buffered_request_dispatch::write_buffered_after_interim(http_buffered_response_write_plan plan) {
    auto encoded = session_.encode_response_head(message_id_.stream_id_, *response_, plan);
    if ((encoded.index() != 0)) {
        if (std::get<1>(encoded).kind_ == http3_response_head_error::peer_field_section_limit) {
            reject_peer_field_section();
        }
        throw std::invalid_argument("invalid HTTP/3 buffered response head");
    }
    if (!response_field_section_allowed(std::get<0>(encoded).field_section_.decoded_field_section_size())) {
        reject_peer_field_section();
    }
    commit_final_response();
    co_await publish_response_frame(static_cast<std::uint64_t>(http3_frame_type::headers), std::get<0>(encoded).field_section_.field_section_);
    if (plan.send_body()) {
        auto body = response_->body_bytes();
        while (!body.empty()) {
            const auto count = std::min(body.size(), std::size_t{16 * 1024});
            co_await publish_response_frame(static_cast<std::uint64_t>(http3_frame_type::data), std::span<const char>(body.data(), count));
            body.remove_prefix(count);
        }
    }
    co_await finish_response();
    co_return run_status_type::output_complete;
}

task<void> http3_buffered_request_dispatch::publish_response_frame(std::uint64_t type, std::span<const char> payload_value) {
    if (response_aborted()) {
        throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    }
    if (!on_worker() || stream_output_ended_ || tunnel_callbacks_.output_ready_ == nullptr || stream_frame_offset_ < stream_frame_.size()) {
        throw std::logic_error("HTTP/3 response publication is unavailable or already active");
    }
    std::array<char, http3_frame_header_max_bytes> header_value{};
    const auto encoded = encode_http3_frame_header(header_value, type, payload_value.size());
    if ((encoded.index() != 0)) {
        throw std::length_error("HTTP/3 response frame exceeds its wire limit");
    }
    stream_frame_.assign(header_value.data(), std::get<0>(encoded));
    stream_frame_.append(payload_value.data(), payload_value.size());
    co_await await_response_publication();
}

task<void> http3_buffered_request_dispatch::publish_response_bytes(std::span<const char> bytes_value) {
    if (response_aborted()) {
        throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    }
    if (!on_worker() || stream_output_ended_ || tunnel_callbacks_.output_ready_ == nullptr || stream_frame_offset_ < stream_frame_.size()) {
        throw std::logic_error("HTTP/3 response publication is unavailable or already active");
    }
    stream_frame_.assign(bytes_value.data(), bytes_value.size());
    co_await await_response_publication();
}

task<void> http3_buffered_request_dispatch::await_response_publication() {
    stream_frame_offset_ = 0;
    stream_output_active_ = true;
    notify_tunnel_output();
    while (stream_frame_offset_ < stream_frame_.size()) {
        if (response_aborted()) {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
        co_await tunnel_output_available_.wait();
    }
    if (response_aborted()) {
        throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    }
    std::pmr::string(stream_frame_.get_allocator()).swap(stream_frame_);
    stream_frame_offset_ = 0;
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
    if (!stream_output_active_ || tunnel_fin_pending_ || response_aborted()) {
        throw std::logic_error("HTTP/3 response cannot be concluded in this state");
    }
    tunnel_fin_pending_ = true;
    notify_tunnel_output();
    while (!stream_output_ended_) {
        if (response_aborted()) {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
        co_await tunnel_output_available_.wait();
    }
}

void http3_buffered_request_dispatch::notify_tunnel_input() noexcept {
    if (!on_worker()) {
        std::terminate();
    }
    tunnel_input_available_.notify();
}

task<std::optional<http_datagram_input>> http3_buffered_request_dispatch::read_datagram_input() {
    if (!on_worker()) {
        throw std::logic_error("HTTP Datagram read requires its worker");
    }
    for (;;) {
        if (cancellation_requested() || tunnel_aborted_) {
            throw std::system_error(std::make_error_code(std::errc::operation_canceled));
        }
        if (auto native = session_.take_datagram(message_id_.stream_id_)) {
            co_return http_datagram_input{std::move(*native), true};
        }
        std::array<char, 4096> bytes_value{};
        const auto result_value = session_.read_tunnel_data(message_id_.stream_id_, bytes_value);
        if (result_value.reset_ || result_value.ended_) {
            disarm_peer_transport_fin_timeout();
        }
        if (result_value.overflow_) {
            abort_tunnel();
            throw std::system_error(std::make_error_code(std::errc::no_buffer_space));
        }
        if (result_value.reset_) {
            throw std::system_error(std::make_error_code(std::errc::connection_reset));
        }
        if (result_value.bytes_ != 0) {
            if (tunnel_callbacks_.input_consumed_) {
                tunnel_callbacks_.input_consumed_(tunnel_callbacks_.context_);
            }
            co_return http_datagram_input{std::pmr::string(bytes_value.data(), result_value.bytes_, worker_.resource()), false};
        }
        if (result_value.ended_) {
            co_return std::nullopt;
        }
        co_await tunnel_input_available_.wait();
    }
}
void http3_buffered_request_dispatch::send_datagram(std::span<const std::byte> bytes_value) {
    if (!on_worker() || !stream_output_active_ || stream_output_ended_ || cancellation_requested() || tunnel_aborted_ || !tunnel_callbacks_.send_datagram_) {
        throw std::runtime_error("HTTP Datagram sending direction is closed");
    }
    tunnel_callbacks_.send_datagram_(tunnel_callbacks_.context_, message_id_.stream_id_, bytes_value);
}

task<http_stream_read_result> http3_buffered_request_dispatch::read_tunnel(
    std::pmr::string& buffer) {
    if (!on_worker()) {
        co_return http_stream_read_result::make_failure(
            std::make_error_code(std::errc::operation_not_permitted));
    }
    for (;;) {
        if (cancellation_requested() || tunnel_aborted_) {
            co_return http_stream_read_result::make_failure(
                std::make_error_code(std::errc::operation_canceled));
        }
        std::array<char, 4096> bytes_value{};
        const auto result_value = session_.read_tunnel_data(message_id_.stream_id_, bytes_value);
        if (result_value.reset_ || result_value.ended_) {
            disarm_peer_transport_fin_timeout();
        }
        if (result_value.overflow_) {
            abort_tunnel();
            co_return http_stream_read_result::make_failure(
                std::make_error_code(std::errc::no_buffer_space));
        }
        if (result_value.reset_) {
            co_return http_stream_read_result::make_failure(
                std::make_error_code(std::errc::connection_reset));
        }
        if (result_value.bytes_ != 0) {
            buffer.append(bytes_value.data(), result_value.bytes_);
            if (tunnel_callbacks_.input_consumed_ != nullptr) {
                tunnel_callbacks_.input_consumed_(tunnel_callbacks_.context_);
            }
            co_return http_stream_read_result::make_data();
        }
        if (result_value.ended_) {
            co_return http_stream_read_result::make_end();
        }
        co_await tunnel_input_available_.wait();
    }
}

task<std::error_code> http3_buffered_request_dispatch::write_tunnel(
    std::string_view bytes_value, http_stream_end disposition) {
    if (!on_worker() || !stream_output_active_ || stream_output_ended_) {
        co_return std::make_error_code(std::errc::operation_not_permitted);
    }
    constexpr auto max_frame_header = http3_frame_header_max_bytes;
    constexpr auto max_payload = http3_stream_buffer::max_block_bytes - max_frame_header;
    std::size_t offset = 0;
    while (offset < bytes_value.size()) {
        if (cancellation_requested() || tunnel_aborted_) {
            co_return std::make_error_code(std::errc::operation_canceled);
        }
        if (tunnel_data_pending_ || tunnel_fin_pending_) {
            co_return std::make_error_code(std::errc::operation_not_permitted);
        }
        const auto count = (std::min)(max_payload, bytes_value.size() - offset);
        std::array<char, max_frame_header> header_value{};
        const auto encoded = encode_http3_frame_header(header_value,
            static_cast<std::uint64_t>(http3_frame_type::data), count);
        if ((encoded.index() != 0)) {
            abort_tunnel();
            co_return std::make_error_code(std::errc::message_size);
        }
        try {
            tunnel_data_frame_.assign(header_value.data(), std::get<0>(encoded));
            tunnel_data_frame_.append(bytes_value.data() + offset, count);
        } catch (...) {
            abort_tunnel();
            co_return std::make_error_code(std::errc::not_enough_memory);
        }
        tunnel_data_pending_ = true;
        notify_tunnel_output();
        while (tunnel_data_pending_) {
            if (cancellation_requested() || tunnel_aborted_) {
                co_return std::make_error_code(std::errc::operation_canceled);
            }
            co_await tunnel_output_available_.wait();
        }
        offset += count;
    }
    if (disposition == http_stream_end::end) {
        tunnel_fin_pending_ = true;
        notify_tunnel_output();
        while (!stream_output_ended_) {
            if (cancellation_requested() || tunnel_aborted_) {
                co_return std::make_error_code(std::errc::operation_canceled);
            }
            co_await tunnel_output_available_.wait();
        }
    }
    co_return std::error_code{};
}

task<bool> http3_buffered_request_dispatch::wait_tunnel_receive_end() {
    if (!on_worker()) {
        co_return false;
    }
    for (;;) {
        if (cancellation_requested() || tunnel_aborted_) {
            co_return false;
        }
        const auto result_value = session_.read_tunnel_data(message_id_.stream_id_, {});
        if (result_value.reset_ || result_value.overflow_) {
            disarm_peer_transport_fin_timeout();
            co_return false;
        }
        if (result_value.ended_) {
            disarm_peer_transport_fin_timeout();
            co_return true;
        }
        co_await tunnel_input_available_.wait();
    }
}

void http3_buffered_request_dispatch::peer_transport_fin_timeout_tick(
    void* target, std::int64_t now_ms) noexcept {
    auto& dispatch = *static_cast<http3_buffered_request_dispatch*>(target);
    if (!dispatch.on_worker()) {
        std::terminate();
    }
    const auto peer_state = dispatch.session_.read_tunnel_data(dispatch.message_id_.stream_id_, {});
    if (peer_state.ended_ || peer_state.reset_ || peer_state.overflow_) {
        dispatch.disarm_peer_transport_fin_timeout();
        return;
    }
    if (now_ms >= dispatch.peer_transport_fin_deadline_ms_) {
        dispatch.abort_tunnel();
    }
}

void http3_buffered_request_dispatch::arm_peer_transport_fin_timeout() noexcept {
    if (!on_worker() || peer_fin_timeout_armed_ ||
        peer_transport_fin_timeout_ <= std::chrono::milliseconds::zero()) {
        return;
    }
    const auto peer_state = session_.read_tunnel_data(message_id_.stream_id_, {});
    if (peer_state.ended_ || peer_state.reset_ || peer_state.overflow_) {
        return;
    }
    const auto now = steady_now_ms();
    const auto timeout = peer_transport_fin_timeout_.count();
    peer_transport_fin_deadline_ms_ = timeout > (std::numeric_limits<std::int64_t>::max)() - now
                                          ? (std::numeric_limits<std::int64_t>::max)()
                                          : now + timeout;
    scanner_entry_.register_periodic_check(
        peer_transport_fin_check_, this, &http3_buffered_request_dispatch::peer_transport_fin_timeout_tick);
    peer_fin_timeout_armed_ = true;
}

void http3_buffered_request_dispatch::disarm_peer_transport_fin_timeout() noexcept {
    peer_transport_fin_check_.reset();
    peer_fin_timeout_armed_ = false;
    peer_transport_fin_deadline_ms_ = 0;
}

void http3_buffered_request_dispatch::abort_tunnel() noexcept {
    if (!on_worker()) {
        std::terminate();
    }
    if (tunnel_aborted_) {
        return;
    }
    disarm_peer_transport_fin_timeout();
    tunnel_aborted_ = true;
    tunnel_input_available_.notify();
    tunnel_output_available_.notify();
    if (tunnel_callbacks_.abort_ != nullptr) {
        tunnel_callbacks_.abort_(tunnel_callbacks_.context_, message_id_.stream_id_);
    }
}

void http3_buffered_request_dispatch::notify_tunnel_output() noexcept {
    if (!on_worker()) {
        std::terminate();
    }
    if (tunnel_callbacks_.output_ready_ != nullptr) {
        tunnel_callbacks_.output_ready_(tunnel_callbacks_.context_, message_id_.stream_id_);
    }
}

bool http3_buffered_request_dispatch::handler_active() const noexcept {
    return handler_active_;
}

bool http3_buffered_request_dispatch::response_ready() const noexcept {
    return state_ == state_type::output_ready || state_ == state_type::publishing;
}

bool http3_buffered_request_dispatch::complete() const noexcept {
    return state_ == state_type::complete;
}

std::uint64_t http3_buffered_request_dispatch::published_wire_bytes() const noexcept {
    return published_wire_bytes_;
}

std::exception_ptr http3_buffered_request_dispatch::failure() const noexcept {
    return failure_;
}

bool http3_buffered_request_dispatch::on_worker() const noexcept {
    return services_.worker().is_current();
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
        // A local abort can finish before peer FIN reaches the input buffer.
        // Retire the protocol request while its lease still pins the head; the
        // lease then frees this stream without waiting for more peer input.
        (void)session_.cancel_request(message_id_.stream_id_);
    }
    lease_.reset();
    if (had_lease) {
        // Response FIN completes only the send direction. The input owner keeps
        // an open receive direction alive until validated FIN or cancellation.
        (void)session_.release(message_id_.stream_id_);
    }
}

}  // namespace ruvia::detail
