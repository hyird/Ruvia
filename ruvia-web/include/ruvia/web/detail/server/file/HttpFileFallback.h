#pragma once

#include "ruvia/http/detail/response/HttpResponseFileBody.h"
#include "ruvia/web/detail/server/file/HttpFileOpen.h"
#include "ruvia/web/detail/server/file/HttpNativeFile.h"
#include "ruvia/core/detail/io/AsioAwait.h"

#include "ruvia/web/detail/server/file/HttpFileChunkBuffer.h"
#include "ruvia/core/Task.h"
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
Task<std::error_code> writeFileChunk(Stream& stream, std::pmr::string& chunk, std::size_t size) {
    const auto writeCompletion = co_await asyncAsio([&stream, &chunk, size](auto handler) mutable { asio::async_write(stream, asio::buffer(chunk.data(), size), std::move(handler)); });
    co_return writeCompletion.errorCode();
}

template <typename Stream>
Task<std::error_code> writeFileFallback(Stream& stream, std::pmr::string& chunk, ResponseFileBody fileBody) {
    ensureFileChunkBuffer(chunk);
    std::error_code error;

#if defined(ASIO_HAS_FILE)
    asio::stream_file input(stream.get_executor());
#if defined(__unix__) || defined(__APPLE__) || defined(_WIN32)
    auto nativeInput = openNativeFileForRead(fileBody, error,
        NativeFileOpenOptions{
#if defined(_WIN32)
            .overlapped = true,
#else
            .overlapped = false,
#endif
            .sequentialScan = true});
    if (!error) {
        input.assign(nativeInput.get(), error);
    }
    if (!error) {
        static_cast<void>(nativeInput.release());
    }
#else
    input.open(fileBody.nativePathCStr(), asio::stream_file::read_only, error);
#endif
    if (error) {
        co_return error;
    }
    input.seek(static_cast<std::int64_t>(fileBody.offset()), asio::stream_file::seek_set, error);
    if (error) {
        co_return error;
    }

    std::uint64_t remaining = fileBody.length();
    while (remaining > 0) {
        const auto nextRead = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), remaining));
        auto readCompletion = co_await asyncAsio<std::size_t>([&input, &chunk, nextRead](auto handler) mutable { input.async_read_some(asio::buffer(chunk.data(), nextRead), std::move(handler)); });
        const auto readEc = readCompletion.errorCode();
        const auto read = readCompletion.result();
        if (readEc) {
            co_return readEc;
        }
        if (read == 0) {
            co_return std::make_error_code(std::errc::io_error);
        }
        remaining -= read;
        error = co_await writeFileChunk(stream, chunk, read);
        if (error) {
            co_return error;
        }
    }
#else
    auto input = openResponseFileInput(fileBody);
    if (!input) {
        co_return std::make_error_code(std::errc::no_such_file_or_directory);
    }
    input.seekg(static_cast<std::streamoff>(fileBody.offset()), std::ios::beg);
    if (!input) {
        co_return std::make_error_code(std::errc::invalid_seek);
    }

    std::uint64_t remaining = fileBody.length();
    while (remaining > 0) {
        const auto nextRead = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), remaining));
        input.read(chunk.data(), static_cast<std::streamsize>(nextRead));
        const auto read = input.gcount();
        if (read <= 0) {
            co_return std::make_error_code(std::errc::io_error);
        }
        remaining -= static_cast<std::uint64_t>(read);
        error = co_await writeFileChunk(stream, chunk, static_cast<std::size_t>(read));
        if (error) {
            co_return error;
        }
    }
#endif
    co_return std::error_code{};
}

template <typename Stream>
Task<std::error_code> writeFileFallbackWithLocalChunk(Stream& stream, WorkerMemory& memory, ResponseFileBody fileBody) {
    std::pmr::string localChunk(memory.allocator<char>());
    co_return co_await writeFileFallback(stream, localChunk, fileBody);
}

template <typename Stream>
Task<std::error_code> writeFileFallback(Stream& stream, WorkerMemory& memory, std::pmr::string* reusableChunk, ResponseFileBody fileBody) {
    if (reusableChunk != nullptr) {
        return writeFileFallback(stream, *reusableChunk, fileBody);
    }
    return writeFileFallbackWithLocalChunk(stream, memory, fileBody);
}

}  // namespace ruvia::detail
