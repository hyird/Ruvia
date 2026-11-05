#include "http2/http2_sans_io_session.h"

#include <array>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/any_io_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/error.hpp>

#include "ruvia/core/async.h"
#include "ruvia/http/http_pmr_string.h"

#include "http2/http2_data_output_budget.h"
#include "http2/http2_sans_io_session_engine.h"
#include "server/connection_scanner.h"
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
            {
                // The reader keeps the entry in its own phase (idle, a body
                // read, or long_lived for an RFC 8441 tunnel); this clock
                // lets write_timeout close a peer that stopped reading
                // regardless. Every transfer renews it.
                const connection_scanner::entry::write_scope write_stall_clock(engine.scanner_entry());
                write_error = (co_await connection_gather_write(stream, write_scratch, {}, engine.scanner_entry())).error_;
            }
            if (write_error) {
                engine.writer_write_failed(write_error);
                continue;
            }
            engine.output_write_completed();
            // Leave room for the frame header and string capacity growth, but keep retention bounded.
            http_clear_pmr_string_retaining_small(
                write_scratch, http2_data_output_credit_slots * http2_data_output_credit_bytes + 4096);
        }
        if (engine.writer_should_exit()) {
            co_return;
        }
        co_await engine.wait_for_write();
    }
}

template <typename stream_type>
task<http_connection_close> run_http2_sans_io_session_impl(stream_type& stream, asio::ip::tcp::socket& socket,
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

    std::error_code reader_terminal_error;
    std::exception_ptr reader_failure;
    try {
        engine.drain_events();
        if (!engine.connection_failed() && !initial_bytes.empty()) {
            (void)engine.feed_and_drain(initial_bytes);
        }
        engine.wake_writer();

        if (!engine.connection_failed() && !engine.terminated()) {
            std::array<char, 4096> read_buffer;
            for (;;) {
                engine.set_inactivity_phase();
                auto read_completion =
                    co_await ruvia::async_asio<std::size_t>([&stream, &read_buffer](auto handler) mutable {
                        stream.async_read_some(asio::buffer(read_buffer), std::move(handler));
                    });
                const auto error = read_completion.error_code();
                const auto bytes_read = read_completion.result();
                if (error || bytes_read == 0) {
                    reader_terminal_error =
                        error ? error
                              : std::make_error_code(std::errc::connection_reset);
                    if (reader_terminal_error == asio::error::eof) {
                        engine.peer_input_ended();
                    }
                    break;
                }
                engine.touch_activity();
                const auto result_value =
                    engine.feed_and_drain(std::string_view(read_buffer.data(), bytes_read));
                engine.wake_writer();
                if (result_value == http2_feed_result::protocol_failure || engine.write_failed()) {
                    if (result_value == http2_feed_result::protocol_failure) {
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
            reader_terminal_error = engine.connection_failed() ? std::make_error_code(std::errc::protocol_error)
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
    // A failed write leaves nothing to end gracefully. A clean peer end (FIN,
    // or TLS close_notify) earns this side's final GOAWAY and close alert. A
    // completed drain (worker drain, request limit, or a timeout's drain at its
    // bound) or a connection error ended the session with GOAWAY while peer
    // frames may still arrive: close lingering (RFC 9113 §9.1, RFC 9112 §9.6).
    // Transport errors close at once, and so does a stopping worker, whatever
    // this mode.
    if (engine.write_failed()) {
        co_return http_connection_close::abort;
    }
    if (reader_terminal_error == asio::error::eof) {
        co_return http_connection_close::notify;
    }
    co_return engine.drain_finished() || engine.connection_failed() ? http_connection_close::linger
                                                                    : http_connection_close::abort;
}

}  // namespace

task<http_connection_close> run_http2_sans_io_session(asio::ip::tcp::socket& stream, const route_table& routes_value,
    worker_memory& worker_value, http2_sans_io_session_context session_value, std::string_view initial_bytes) {
    return run_http2_sans_io_session_impl(
        stream, stream, routes_value, worker_value, std::move(session_value), initial_bytes);
}

task<http_connection_close> run_http2_sans_io_session(server_tls_stream& stream,
    const route_table& routes_value, worker_memory& worker_value, http2_sans_io_session_context session_value,
    std::string_view initial_bytes) {
    return run_http2_sans_io_session_impl(
        stream, stream.lowest_layer(), routes_value, worker_value, std::move(session_value), initial_bytes);
}

}  // namespace ruvia::detail
