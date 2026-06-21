#pragma once

#include "../../runtime/AsioAwait.h"

#include "HttpFileChunkBuffer.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/memory/MemoryPool.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory_resource>
#include <string>
#include <system_error>
#include <utility>

#include <asio.hpp>

namespace ruvia::detail {

template <typename Stream>
Task<void> writeFileChunk(
    Stream& stream,
    std::pmr::string& chunk,
    std::size_t size,
    std::error_code& ec) {
    ec = co_await asyncError([&stream, &chunk, size](auto handler) mutable {
        asio::async_write(stream, asio::buffer(chunk.data(), size), std::move(handler));
    });
}

template <typename Stream>
Task<void> writeFileFallback(
    Stream& stream,
    WorkerMemory& memory,
    std::pmr::string* reusableChunk,
    const FileBody& fileBody,
    std::error_code& ec) {
    std::pmr::string localChunk(memory.allocator<char>());
    auto& chunk = reusableChunk == nullptr ? localChunk : *reusableChunk;
    ensureFileChunkBuffer(chunk);

#if defined(ASIO_HAS_FILE)
    asio::stream_file input(stream.get_executor());
    input.open(fileTokenPath(fileBody.file).string(), asio::stream_file::read_only, ec);
    if (ec) {
        ec = asio::error::operation_aborted;
        co_return;
    }
    input.seek(static_cast<std::int64_t>(fileBody.offset), asio::stream_file::seek_set, ec);
    if (ec) {
        ec = asio::error::operation_aborted;
        co_return;
    }

    std::uint64_t remaining = fileBody.length;
    while (remaining > 0) {
        const auto nextRead = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), remaining));
        auto [readEc, read] = co_await asyncResult<std::size_t>([&input, &chunk, nextRead](auto handler) mutable {
            input.async_read_some(asio::buffer(chunk.data(), nextRead), std::move(handler));
        });
        ec = readEc;
        if (ec || read == 0) {
            co_return;
        }
        remaining -= read;
        co_await writeFileChunk(stream, chunk, read, ec);
        if (ec) {
            co_return;
        }
    }
#else
    std::ifstream input(fileTokenPath(fileBody.file), std::ios::binary);
    if (!input) {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        co_return;
    }
    input.seekg(static_cast<std::streamoff>(fileBody.offset), std::ios::beg);
    if (!input) {
        ec = std::make_error_code(std::errc::invalid_seek);
        co_return;
    }

    std::uint64_t remaining = fileBody.length;
    while (remaining > 0) {
        const auto nextRead = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), remaining));
        input.read(chunk.data(), static_cast<std::streamsize>(nextRead));
        const auto read = input.gcount();
        if (read <= 0) {
            ec = std::make_error_code(std::errc::io_error);
            co_return;
        }
        remaining -= static_cast<std::uint64_t>(read);
        co_await writeFileChunk(stream, chunk, static_cast<std::size_t>(read), ec);
        if (ec) {
            co_return;
        }
    }
    ec = {};
#endif
}

}  // namespace ruvia::detail
