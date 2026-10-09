#include "http2/http2_sans_io_session.h"

#include <array>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/any_io_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/write.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/pmr_string.h"

#include "http2/http2_sans_io_session_engine.h"
#include "server/http_response_writer.h"

namespace ruvia::detail {
namespace {

template <typename stream_type>
task<void> run_http2_sans_io_writer(stream_type& stream, http2_sans_io_session_engine& engine) {
    std::pmr::string write_scratch(engine.worker_resource());
    for (;;) {
        while (engine.wants_write()) {
            engine.take_output(write_scratch);
            if (engine.write_failed()) {
                continue;
            }

            std::error_code write_error;
            std::size_t written_bytes = 0;
            bool write_done = false;
            if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, asio::ip::tcp::socket>) {
                write_done = try_plain_tcp_sync_write(stream,
                    asio::buffer(write_scratch.data(), write_scratch.size()), write_scratch.size(),
                    write_error, written_bytes);
            }
            if (!write_done) {
                const auto write_completion = co_await ruvia::async_asio(
                    [&stream, &write_scratch, written_bytes](auto handler) mutable {
                        asio::async_write(stream,
                            asio::buffer(write_scratch.data() + written_bytes,
                                write_scratch.size() - written_bytes),
                            std::move(handler));
                    });
                write_error = write_completion.error_code();
            }
            if (write_error) {
                engine.writer_write_failed(write_error);
                continue;
            }
            engine.touch_activity();
            engine.output_write_completed();
            ::ruvia::clear_pmr_string_retaining_small(write_scratch, 16 * 1024);
        }
        if (engine.writer_should_exit()) {
            co_return;
        }
        co_await engine.wait_for_write();
    }
}

template <typename stream_type>
task<void> run_http2_sans_io_session_impl(stream_type& stream, asio::ip::tcp::socket& socket,
    const route_table& routes_value, worker_memory& worker_value, http2_sans_io_session_context session_value,
    std::string_view initial_bytes) {
    auto executor = asio::any_io_executor(stream.get_executor());
    http2_sans_io_session_engine engine(executor, socket, routes_value, worker_value, std::move(session_value));

    engine.writer_submitting();
    try {
        asio::co_spawn(executor, ruvia::as_awaitable(run_http2_sans_io_writer(stream, engine)),
            [&engine](
                std::exception_ptr exception) noexcept { engine.writer_completed(exception); });
    } catch (...) {
        engine.writer_launch_failed();
        engine.terminate(std::make_error_code(std::errc::operation_canceled));
        throw;
    }

    bool initial_input_retained = false;
    std::error_code reader_terminal_error;
    std::exception_ptr reader_failure;
    try {
        engine.drain_events();
        if (!engine.connection_failed() && !initial_bytes.empty()) {
            const auto result_value = engine.feed_and_drain(initial_bytes);
            initial_input_retained = result_value == http2_feed_result::connection_not_started;
        }
        engine.wake_writer();

        if (!engine.connection_failed() && !initial_input_retained && !engine.terminated()) {
            std::array<char, 4096> read_buffer;
            for (;;) {
                engine.set_inactivity_phase();
                auto read_completion =
                    co_await ruvia::async_asio<std::size_t>([&stream, &read_buffer](auto handler) mutable {
                        stream.async_read_some(
                            asio::buffer(read_buffer.data(), read_buffer.size()), std::move(handler));
                    });
                const auto error = read_completion.error_code();
                const auto bytes_read = read_completion.result();
                const bool worker_stopped = !engine.worker_running();
                if (error || bytes_read == 0 || worker_stopped) {
                    reader_terminal_error =
                        error ? error
                              : std::make_error_code(worker_stopped ? std::errc::operation_canceled
                                                                    : std::errc::connection_reset);
                    break;
                }
                engine.touch_activity();
                const auto result_value =
                    engine.feed_and_drain(std::string_view(read_buffer.data(), bytes_read));
                engine.wake_writer();
                if (result_value == http2_feed_result::connection_not_started ||
                    result_value == http2_feed_result::protocol_failure || engine.write_failed()) {
                    if (result_value == http2_feed_result::connection_not_started ||
                        result_value == http2_feed_result::protocol_failure) {
                        reader_terminal_error = std::make_error_code(std::errc::protocol_error);
                    }
                    break;
                }
            }
        }
    } catch (...) {
        reader_failure = std::current_exception();
        reader_terminal_error = std::make_error_code(std::errc::operation_canceled);
    }

    if (!engine.terminated()) {
        if (!reader_terminal_error) {
            reader_terminal_error = engine.connection_failed() || initial_input_retained
                                        ? std::make_error_code(std::errc::protocol_error)
                                        : std::make_error_code(std::errc::connection_aborted);
        }
        engine.terminate(reader_terminal_error);
    }

    std::exception_ptr finish_failure;
    try {
        co_await engine.finish();
    } catch (...) {
        finish_failure = std::current_exception();
    }
    if (reader_failure != nullptr) {
        std::rethrow_exception(reader_failure);
    }
    if (finish_failure != nullptr) {
        std::rethrow_exception(finish_failure);
    }
}

}  // namespace

task<void> run_http2_sans_io_session(asio::ip::tcp::socket& stream, const route_table& routes_value,
    worker_memory& worker_value, http2_sans_io_session_context session_value, std::string_view initial_bytes) {
    return run_http2_sans_io_session_impl(
        stream, stream, routes_value, worker_value, std::move(session_value), initial_bytes);
}

task<void> run_http2_sans_io_session(asio::ssl::stream<asio::ip::tcp::socket&>& stream,
    const route_table& routes_value, worker_memory& worker_value, http2_sans_io_session_context session_value,
    std::string_view initial_bytes) {
    return run_http2_sans_io_session_impl(
        stream, stream.next_layer(), routes_value, worker_value, std::move(session_value), initial_bytes);
}

}  // namespace ruvia::detail
