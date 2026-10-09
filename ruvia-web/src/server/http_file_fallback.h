#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <memory_resource>
#include <string>
#include <system_error>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_response_file.h"

#include "server/http_file_chunk_buffer.h"
#include "server/http_file_open.h"
#include "server/http_native_file.h"

namespace ruvia::detail {

template <typename stream_type>
task<std::error_code> write_file_chunk(stream_type& stream, std::pmr::string& chunk, std::size_t size) {
    const auto write_completion = co_await ruvia::async_asio([&stream, &chunk, size](auto handler) mutable {
        asio::async_write(stream, asio::buffer(chunk.data(), size), std::move(handler));
    });
    co_return write_completion.error_code();
}

template <typename stream_type>
task<std::error_code> write_file_fallback(
    stream_type& stream, std::pmr::string& chunk, http_response_file_view file_body) {
    ensure_file_chunk_buffer(chunk);
    std::error_code error;

#if defined(ASIO_HAS_FILE)
    asio::stream_file input(stream.get_executor());
#if defined(__unix__) || defined(_WIN32)
    auto native_input = open_native_file_for_read(file_body, error,
        native_file_open_options{
#if defined(_WIN32)
            .overlapped_ = true,
#else
            .overlapped_ = false,
#endif
            .sequential_scan_ = true});
    if (!error) {
        input.assign(native_input.get(), error);
    }
    if (!error) {
        static_cast<void>(native_input.release());
    }
#else
    input.open(file_body.native_path_c_str(), asio::stream_file::read_only, error);
#endif
    if (error) {
        co_return error;
    }
    input.seek(static_cast<std::int64_t>(file_body.offset()), asio::stream_file::seek_set, error);
    if (error) {
        co_return error;
    }

    std::uint64_t remaining = file_body.length();
    while (remaining > 0) {
        const auto next_read =
            static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), remaining));
        auto read_completion =
            co_await ruvia::async_asio<std::size_t>([&input, &chunk, next_read](auto handler) mutable {
                input.async_read_some(asio::buffer(chunk.data(), next_read), std::move(handler));
            });
        const auto read_ec = read_completion.error_code();
        const auto read = read_completion.result();
        if (read_ec) {
            co_return read_ec;
        }
        if (read == 0) {
            co_return std::make_error_code(std::errc::io_error);
        }
        remaining -= read;
        error = co_await write_file_chunk(stream, chunk, read);
        if (error) {
            co_return error;
        }
    }
#else
    auto input = open_response_file_input(file_body);
    if (!input) {
        co_return std::make_error_code(std::errc::no_such_file_or_directory);
    }
    input.seekg(static_cast<std::streamoff>(file_body.offset()), std::ios::beg);
    if (!input) {
        co_return std::make_error_code(std::errc::invalid_seek);
    }

    std::uint64_t remaining = file_body.length();
    while (remaining > 0) {
        const auto next_read =
            static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), remaining));
        input.read(chunk.data(), static_cast<std::streamsize>(next_read));
        const auto read = input.gcount();
        if (read <= 0) {
            co_return std::make_error_code(std::errc::io_error);
        }
        remaining -= static_cast<std::uint64_t>(read);
        error = co_await write_file_chunk(stream, chunk, static_cast<std::size_t>(read));
        if (error) {
            co_return error;
        }
    }
#endif
    co_return std::error_code{};
}

template <typename stream_type>
task<std::error_code> write_file_fallback_with_local_chunk(
    stream_type& stream, worker_memory& memory, http_response_file_view file_body) {
    std::pmr::string local_chunk(memory.allocator<char>());
    co_return co_await write_file_fallback(stream, local_chunk, file_body);
}

template <typename stream_type>
task<std::error_code> write_file_fallback(stream_type& stream, worker_memory& memory,
    std::pmr::string* reusable_chunk, http_response_file_view file_body) {
    if (reusable_chunk != nullptr) {
        return write_file_fallback(stream, *reusable_chunk, file_body);
    }
    return write_file_fallback_with_local_chunk(stream, memory, file_body);
}

}  // namespace ruvia::detail
