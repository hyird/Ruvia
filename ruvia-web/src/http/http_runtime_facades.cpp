#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/bytes.h"
#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/task.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/multipart_reader.h"
#include "ruvia/web/streaming.h"
#include "ruvia/web/websocket.h"

#include "context/context_access.h"
#include "context/context_services.h"
#include "http/inline_http_header_views.h"
#include "integration/operation_interrupt.h"
#include "integration/operation_lane_lease.h"

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
auto run_response_stream_output(response_stream_output output, ruvia::operation_options operation,
    const char* operation_name, operation_type start) -> decltype(start()) {
    auto lease = ruvia::detail::claim_operation_lane(output.active_, response_stream_lane_in_progress);
    ruvia::detail::operation_interrupt interrupt(
        output.interrupts_, output.worker_, operation, output.context_, &ruvia::detail::context_access::reset_interrupted_request, operation_name);
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

sse_writer::sse_writer(const sse_writer& other) noexcept
    : writer_(other.writer_),
      registration_(other.registration_, this) {}

sse_writer::sse_writer(sse_writer&& other) noexcept
    : writer_(std::exchange(other.writer_, nullptr)),
      registration_(std::move(other.registration_), this) {}

sse_writer::sse_writer(response_stream_writer& writer) noexcept
    : writer_(writer.operation_scope_.active() ? &writer : nullptr),
      registration_(writer.operation_scope_, this, &sse_writer::expire_capability) {}

response_stream_writer& sse_writer::writer() const {
    registration_.require_active();
    return *writer_;
}

void sse_writer::expire_capability(void* target) noexcept {
    static_cast<sse_writer*>(target)->writer_ = nullptr;
}

http_tunnel& context::tunnel() const {
    const auto* output = response_output().tunnel();
    if (output == nullptr) {
        throw std::logic_error("HTTP tunnel is available only in an established CONNECT route");
    }
    return output->tunnel();
}

websocket& context::websocket() const {
    const auto* output = response_output().get_websocket();
    if (output == nullptr) {
        throw std::logic_error("websocket is not available");
    }
    return output->get_websocket();
}

response_stream_writer& context::stream() {
    const auto* output = response_output().response_stream();
    if (output == nullptr) {
        throw std::logic_error("response body is not streamable");
    }
    return output->writer();
}

sse_writer context::stream_sse() {
    auto& writer = stream();
    if (!writer.committed()) {
        set_stable_response_header("Content-Type", "text/event-stream");
        set_stable_response_header("Cache-Control", "no-cache");
    }
    return sse_writer(writer);
}

namespace {

template <typename view_type>
task<std::optional<view_type>> read_body(
    detail::callback_ref<task<std::optional<std::span<const std::byte>>>(operation_options)> read, bool& read_active,
    operation_options operation) {
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

task<void> write_transferred_chunk(response_stream_output output, task<void> (*write)(void*, std::string_view),
    std::string chunk, operation_options operation) {
    co_await run_response_stream_output(output, std::move(operation), "response stream write",
        [&] { return write(output.target_, chunk); });
}

task<void> end_owned(response_stream_output output, task<void> (*end)(void*, std::span<const http_header_view>),
    std::vector<http_header> fields, operation_options operation) {
    const detail::inline_http_header_views views(fields);
    co_await run_response_stream_output(output, std::move(operation), "response stream end",
        [&] { return end(output.target_, views.view()); });
}

task<timer_sleep_result> sleep_stopped_by_either(void* target,
    task<timer_sleep_result> (*sleep)(void*, std::chrono::milliseconds, const stop_token&),
    std::chrono::milliseconds duration, stop_token context_stop, stop_token stop) {
    const stop_group either(context_stop, stop);
    co_return co_await sleep(target, duration, either.token());
}

}  // namespace

task<std::optional<std::span<const std::byte>>> body_reader::read(operation_options operation) & {
    return ::ruvia::make_scoped_operation(operation_scope_, read_body<std::span<const std::byte>>(read_, read_active_, std::move(operation)));
}

task<std::optional<std::string_view>> body_reader::text(operation_options operation) & {
    return ::ruvia::make_scoped_operation(operation_scope_, read_body<std::string_view>(read_, read_active_, std::move(operation)));
}

void response_stream_writer::require_output() const {
    require_active();
    if (worker_ == nullptr || context_ == nullptr) {
        throw std::logic_error("response stream is not bound to a request");
    }
}

task<stream_commit_result> response_stream_writer::commit(operation_options operation) & {
    require_output();
    return ::ruvia::make_scoped_operation(operation_scope_,
        run_response_stream_output({target_, output_active_, *interrupts_, *worker_, context_}, std::move(operation), "response stream commit",
            [target = target_, commit = commit_] { return commit(target); }));
}

task<stream_commit_result> sse_writer::commit(operation_options operation) & {
    return writer().commit(std::move(operation));
}

task<void> response_stream_writer::write(std::span<const std::byte> chunk, operation_options operation) & {
    return write(as_chars(chunk), std::move(operation));
}

task<void> response_stream_writer::write(std::string_view chunk, operation_options operation) & {
    require_active();
    return write_owned(std::string(chunk), std::move(operation));
}

task<void> response_stream_writer::write_owned(std::string&& chunk, operation_options operation) {
    require_output();
    return ::ruvia::make_scoped_operation(operation_scope_,
        write_transferred_chunk({target_, output_active_, *interrupts_, *worker_, context_}, write_, std::move(chunk), std::move(operation)));
}

task<void> response_stream_writer::writeln(std::string_view chunk, operation_options operation) & {
    require_active();
    std::string owned;
    owned.reserve(chunk.size() + 1U);
    owned.append(chunk);
    owned.push_back('\n');
    return write_owned(std::move(owned), std::move(operation));
}

task<timer_sleep_result> response_stream_writer::sleep(timeout_duration duration, stop_token stop) & {
    require_active();
    if (!stop.stoppable()) {
        return ::ruvia::make_scoped_operation(operation_scope_, sleep_(target_, duration.value(), stop_token_));
    }
    return ::ruvia::make_scoped_operation(
        operation_scope_, sleep_stopped_by_either(target_, sleep_, duration.value(), stop_token_, std::move(stop)));
}

task<void> response_stream_writer::end(std::span<const http_header_view> trailers, operation_options operation) & {
    require_output();
    std::vector<http_header> owned_trailers_value;
    owned_trailers_value.reserve(trailers.size());
    for (const auto& header : trailers) {
        owned_trailers_value.emplace_back(header.name(), header.value());
    }
    return ::ruvia::make_scoped_operation(
        operation_scope_, end_owned({target_, output_active_, *interrupts_, *worker_, context_}, end_, std::move(owned_trailers_value), std::move(operation)));
}

task<timer_sleep_result> sse_writer::sleep(timeout_duration duration, stop_token stop) & {
    return writer().sleep(duration, std::move(stop));
}

task<void> sse_writer::end(std::span<const http_header_view> trailers, operation_options operation) & {
    return writer().end(trailers, std::move(operation));
}

task<void> sse_writer::write(const sse_message& message, operation_options operation) & {
    auto& stream_writer = writer();
    auto frame = format_sse_message(message);
    return stream_writer.write(std::move(frame), std::move(operation));
}

task<std::optional<multipart_stream_part>> multipart_reader::read(operation_options operation) & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(operation_scope_, read_task(std::move(operation)));
}

body_reader& multipart_reader::get_body_reader() const {
    registration_.require_active();
    return *body_reader_;
}

void multipart_reader::expire_capability(void* target) noexcept {
    auto& reader_value = *static_cast<multipart_reader*>(target);
    reader_value.operation_scope_.close();
    reader_value.body_reader_ = nullptr;
    reader_value.state_.emplace<expired_state_type>();
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
            if (const auto* part = result_value.part()) {
                read_guard_value.commit();
                co_return *part;
            }
            if (result_value.done() != nullptr) {
                while (co_await get_body_reader().read(steps.next("multipart body read"))) {
                }
                state_.emplace<finished_state_type>();
                read_guard_value.commit();
                co_return std::nullopt;
            }
            if (result_value.need_input() != nullptr) {
                auto chunk = co_await get_body_reader().read(steps.next("multipart body read"));
                if (!chunk) {
                    std::get<receiving_state_type>(state_).parser_.finish_input();
                } else {
                    std::get<receiving_state_type>(state_).parser_.feed(as_chars(*chunk));
                }
                continue;
            }
            if (const auto* failure = result_value.failure()) {
                throw failure->protocol_error();
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
