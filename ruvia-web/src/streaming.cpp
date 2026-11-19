#include "streaming.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "borrowed_text.h"
#include "byte_view.h"
#include "callback_ref.h"
#include "connection_lifecycle.h"
#include "context.h"
#include "context_access.h"
#include "http1_server_response.h"
#include "http_field_section_budget.h"
#include "http_header.h"
#include "http_multipart.h"
#include "http_server_sent_events.h"
#include "inline_storage.h"
#include "io_operation.h"
#include "middleware.h"
#include "route_table.h"
#include "stop_token.h"
#include "task.h"
#include "task_scope.h"
#include "worker_ref.h"
namespace {

constexpr const char* response_stream_lane_in_progress = "response stream output operation is already in progress";

// What an output operation of a response stream borrows from its writer.
struct response_stream_output final {
    void* target_;
    bool& active_;
    ruvia::detail::operation_interrupt_source& interrupts_;
    const ruvia::worker_ref& worker_;
    ruvia::context* context_;
};

template <typename operation_type>
auto run_response_stream_output(response_stream_output output, ruvia::operation_options operation, const char* operation_name, operation_type start) -> decltype(start()) {
    auto lease = ruvia::detail::claim_operation_lane(output.active_, response_stream_lane_in_progress);
    ruvia::detail::operation_interrupt interrupt(output.interrupts_, output.worker_, operation, output.context_, &ruvia::detail::context_access::reset_interrupted_request, operation_name);
    try {
        if constexpr (std::is_void_v<typename decltype(start())::value_type>) {
            co_await start();
            interrupt.throw_if_interrupted();
        } else {
            auto result = co_await start();
            interrupt.throw_if_interrupted();
            co_return result;
        }
    } catch (...) {
        interrupt.throw_if_interrupted();
        throw;
    }
}

}  // namespace

namespace ruvia {

sse_writer::sse_writer(const sse_writer& other) noexcept :
    writer_(other.writer_),
    registration_(other.registration_, *this) {}

sse_writer::sse_writer(sse_writer&& other) noexcept :
    writer_(std::exchange(other.writer_, nullptr)),
    registration_(std::move(other.registration_), *this) {}

sse_writer::sse_writer(response_stream_writer& writer) noexcept :
    writer_(writer.operation_scope_.active() ? &writer : nullptr),
    registration_(writer.operation_scope_, *this) {}

response_stream_writer& sse_writer::writer() const {
    registration_.require_active();
    return *writer_;
}

void sse_writer::expire_capability() noexcept {
    writer_ = nullptr;
}

http_tunnel& context::tunnel() & {
    const auto* output = response_output().tunnel();
    if (output == nullptr) {
        throw std::logic_error("HTTP tunnel is available only in an established CONNECT route");
    }
    return output->tunnel();
}

websocket& context::websocket() & {
    const auto* output = response_output().get_websocket();
    if (output == nullptr) {
        throw std::logic_error("websocket is not available");
    }
    return output->get_websocket();
}

namespace {

template <typename view_type>
task<std::optional<view_type>> read_body(detail::callback_ref<task<std::optional<std::span<const std::byte>>>(operation_options)> read, bool& read_active, operation_options operation) {
    auto lease = detail::claim_operation_lane(read_active, "request body read is already in progress");
    const auto chunk = co_await read(std::move(operation));
    if (!chunk) {
        co_return std::nullopt;
    }
    if constexpr (std::same_as<view_type, std::string_view>) {
        co_return as_chars(*chunk);
    } else {
        co_return *chunk;
    }
}

task<void> end_owned(response_stream_output output, task<void> (*end)(void*, std::span<const http_header_view>), std::vector<http_header> fields, operation_options operation) {
    const detail::inline_http_header_views views(fields);
    co_await run_response_stream_output(output, std::move(operation), "response stream end", [&] {
        return end(output.target_, views.view());
    });
}

task<sleep_status> sleep_stopped_by_either(void* target, task<sleep_status> (*sleep)(void*, timeout_duration, const stop_token&), timeout_duration duration, stop_token context_stop, stop_token stop) {
    const stop_group either{context_stop, stop};
    co_return co_await sleep(target, duration, either.token());
}

}  // namespace

task<std::optional<std::span<const std::byte>>> body_reader::read(operation_options operation) & {
    return operation_scope_.attach(read_body<std::span<const std::byte>>(read_, read_active_, std::move(operation)));
}

task<std::optional<std::string_view>> body_reader::read_text(operation_options operation) & {
    return operation_scope_.attach(read_body<std::string_view>(read_, read_active_, std::move(operation)));
}

void response_stream_writer::require_output() const {
    require_active();
    if (worker_ == nullptr || context_ == nullptr) {
        throw std::logic_error("response stream is not bound to a request");
    }
}

task<stream_commit_result> response_stream_writer::commit(operation_options operation) & {
    require_output();
    return operation_scope_.attach(run_response_stream_output({target_, output_active_, *interrupts_, *worker_, context_}, std::move(operation), "response stream commit", [target = target_, commit = commit_] {
        return commit(target);
    }));
}

task<stream_commit_result> detail::streaming_access::commit_prepared(response_stream_writer& writer, callback_ref<void()> prepare, operation_options operation) {
    writer.require_output();
    return writer.operation_scope_.attach(run_response_stream_output({writer.target_, writer.output_active_, *writer.interrupts_, *writer.worker_, writer.context_}, std::move(operation), "response stream commit", [target = writer.target_, commit = writer.commit_, prepare] {
        prepare();
        return commit(target);
    }));
}

task<stream_commit_result> sse_writer::commit(operation_options operation) & {
    return writer().commit(std::move(operation));
}

task<void> response_stream_writer::write(outbound_bytes chunk, operation_options operation) & {
    require_active();
    require_output();
    return operation_scope_.attach(run_response_stream_output({target_, output_active_, *interrupts_, *worker_, context_}, std::move(operation), "response stream write", [target = target_, write = write_, chunk = std::move(chunk).take_string()] {
        return write(target, chunk);
    }));
}

task<void> detail::streaming_access::write_borrowed(response_stream_writer& writer, borrowed_text chunk, operation_options operation) {
    writer.require_output();
    return writer.operation_scope_.attach(run_response_stream_output({writer.target_, writer.output_active_, *writer.interrupts_, *writer.worker_, writer.context_}, std::move(operation), "response stream write", [target = writer.target_, write = writer.write_, chunk] {
        return write(target, chunk.view());
    }));
}

task<void> response_stream_writer::writeln(outbound_bytes chunk, operation_options operation) & {
    require_active();
    auto owned = std::move(chunk).take_string();
    owned.push_back('\n');
    return write(std::move(owned), std::move(operation));
}

task<sleep_status> response_stream_writer::sleep(timeout_duration duration, stop_token stop) & {
    require_active();
    if (!stop.stoppable() || stop == stop_token_) {
        return operation_scope_.attach(sleep_(target_, duration, stop_token_));
    }
    return operation_scope_.attach(sleep_stopped_by_either(target_, sleep_, duration, stop_token_, std::move(stop)));
}

task<void> response_stream_writer::finish(argument_list<http_header_view> trailers, operation_options operation) & {
    require_output();
    const auto trailer_span = trailers.span();
    auto budget = http_trailer_section_budget();
    for (const auto& trailer : trailer_span) {
        if (!budget.add_field(trailer.name(), trailer.value())) {
            throw std::invalid_argument("HTTP response trailer section exceeds its budget");
        }
    }
    std::vector<http_header> owned_trailers_value;
    owned_trailers_value.reserve(trailer_span.size());
    for (const auto& header : trailer_span) {
        owned_trailers_value.emplace_back(header.name(), header.value());
    }
    return operation_scope_.attach(end_owned({target_, output_active_, *interrupts_, *worker_, context_}, end_, std::move(owned_trailers_value), std::move(operation)));
}

task<sleep_status> sse_writer::sleep(timeout_duration duration, stop_token stop) & {
    return writer().sleep(duration, std::move(stop));
}

task<void> sse_writer::finish(argument_list<http_header_view> trailers, operation_options operation) & {
    return writer().finish(trailers.span(), std::move(operation));
}

task<void> sse_writer::write(const sse_message& message, operation_options operation) & {
    auto& stream_writer = writer();
    auto frame = format_sse_message(message);
    return stream_writer.write(std::move(frame), std::move(operation));
}

task<std::optional<multipart_stream_part>> multipart_reader::read(operation_options operation) & {
    registration_.require_active();
    return operation_scope_.attach(read_task(std::move(operation)));
}

body_reader& multipart_reader::get_body_reader() const {
    registration_.require_active();
    return *body_reader_;
}

void multipart_reader::expire_capability() noexcept {
    operation_scope_.close();
    body_reader_ = nullptr;
    state_.emplace<expired_state_type>();
}

task<std::optional<multipart_stream_part>> multipart_reader::read_task(operation_options operation) {
    auto lease = detail::claim_operation_lane(read_active_, "multipart body read is already in progress");
    if (std::holds_alternative<finished_state_type>(state_)) {
        co_return std::nullopt;
    }
    if (std::holds_alternative<failed_state_type>(state_)) {
        throw std::logic_error("multipart body consumption previously failed");
    }
    if (!std::holds_alternative<receiving_state_type>(state_)) {
        throw std::logic_error("multipart body lifetime has expired");
    }

    if (operation.stop_token_.stop_requested()) {
        detail::operation_interrupt::throw_cause(detail::operation_interrupt_cause::cancelled, "multipart body read");
    }
    const detail::operation_steps steps(std::move(operation));
    read_guard_type read_guard_value(*this);
    try {
        for (;;) {
            auto result_value = std::get<receiving_state_type>(state_).parser_.poll();
            if (const auto* part = result_value.get_if<multipart_stream_part>()) {
                read_guard_value.commit();
                co_return *part;
            }
            if (result_value.holds<decode_complete>()) {
                while (co_await get_body_reader().read(steps.next("multipart body read"))) {
                }
                state_.emplace<finished_state_type>();
                read_guard_value.commit();
                co_return std::nullopt;
            }
            if (result_value.holds<decode_need_input>()) {
                auto chunk = co_await get_body_reader().read(steps.next("multipart body read"));
                if (!chunk) {
                    std::get<receiving_state_type>(state_).parser_.finish_input();
                } else {
                    std::get<receiving_state_type>(state_).parser_.feed(as_chars(*chunk));
                }
                continue;
            }
            if (const auto* failure = result_value.get_if<multipart_parse_failure>()) {
                throw make_http_protocol_error(failure->error());
            }
            throw std::logic_error("unexpected multipart poll result");
        }
    } catch (const std::system_error& error) {
        // An interrupted body read fed the parser nothing; parsing resumes on
        // the next read.
        if (steps.interrupted(error)) {
            read_guard_value.commit();
        }
        throw;
    }
}

}  // namespace ruvia
