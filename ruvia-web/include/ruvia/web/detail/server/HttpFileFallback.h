#pragma once

#include "ruvia/http/detail/HttpResponseFileBody.h"
#include "ruvia/web/detail/server/HttpFileOpen.h"
#include "ruvia/web/detail/server/HttpNativeFile.h"
#include "ruvia/core/detail/AsioAwait.h"

#include "ruvia/web/detail/server/HttpFileChunkBuffer.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/core/memory/MemoryPool.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ios>
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
    std::pmr::string& chunk,
    ResponseFileBody fileBody,
    std::error_code& ec) {
    ensureFileChunkBuffer(chunk);

#if defined(ASIO_HAS_FILE)
    asio::stream_file input(stream.get_executor());
#if defined(_WIN32)
    auto nativeInput = openNativeFileForRead(
        fileBody,
        ec,
        NativeFileOpenOptions{.overlapped = true, .sequentialScan = true});
    if (!ec) {
        input.assign(nativeInput.get(), ec);
    }
    if (!ec) {
        static_cast<void>(nativeInput.release());
    }
#else
    input.open(fileBody.nativePathCStr(), asio::stream_file::read_only, ec);
#endif
    if (ec) {
        ec = asio::error::operation_aborted;
        co_return;
    }
    input.seek(
        static_cast<std::int64_t>(fileBody.offset()),
        asio::stream_file::seek_set,
        ec);
    if (ec) {
        ec = asio::error::operation_aborted;
        co_return;
    }

    std::uint64_t remaining = fileBody.length();
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
    auto input = openResponseFileInput(fileBody);
    if (!input) {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        co_return;
    }
    input.seekg(
        static_cast<std::streamoff>(fileBody.offset()),
        std::ios::beg);
    if (!input) {
        ec = std::make_error_code(std::errc::invalid_seek);
        co_return;
    }

    std::uint64_t remaining = fileBody.length();
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

template <typename Stream>
Task<void> writeFileFallbackWithLocalChunk(
    Stream& stream,
    WorkerMemory& memory,
    ResponseFileBody fileBody,
    std::error_code& ec) {
    std::pmr::string localChunk(memory.allocator<char>());
    co_await writeFileFallback(stream, localChunk, fileBody, ec);
}

template <typename Stream>
Task<void> writeFileFallback(
    Stream& stream,
    WorkerMemory& memory,
    std::pmr::string* reusableChunk,
    ResponseFileBody fileBody,
    std::error_code& ec) {
    if (reusableChunk != nullptr) {
        return writeFileFallback(stream, *reusableChunk, fileBody, ec);
    }
    return writeFileFallbackWithLocalChunk(stream, memory, fileBody, ec);
}

}  // namespace ruvia::detail
