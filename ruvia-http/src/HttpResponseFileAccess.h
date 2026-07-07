#pragma once

#include "HttpResponseFileBody.h"
#include "ruvia/http/HttpResponse.h"

#include <cstdint>
#include <filesystem>
#include <utility>

namespace ruvia::detail {

struct HttpResponseFileAccess final {
    static void setFile(HttpResponse& response, std::filesystem::path file, std::uint64_t size) {
        response.setFileBody(std::move(file), size);
    }

    static void setFile(
        HttpResponse& response,
        std::filesystem::path file,
        std::uint64_t size,
        std::uint64_t offset,
        std::uint64_t length) {
        response.setFileBody(std::move(file), size, offset, length);
    }

    static void setBorrowedFile(HttpResponse& response, const std::filesystem::path& file, std::uint64_t size) {
        response.setBorrowedFileBody(file, size);
    }

    static void setBorrowedNativeFile(
        HttpResponse& response,
        const NativePathChar* file,
        std::uint64_t size) {
        response.setBorrowedNativeFileBody(file, size);
    }

    static void setBorrowedFile(
        HttpResponse& response,
        const std::filesystem::path& file,
        std::uint64_t size,
        std::uint64_t offset,
        std::uint64_t length) {
        response.setBorrowedFileBody(file, size, offset, length);
    }

    static void setBorrowedNativeFile(
        HttpResponse& response,
        const NativePathChar* file,
        std::uint64_t size,
        std::uint64_t offset,
        std::uint64_t length) {
        response.setBorrowedNativeFileBody(file, size, offset, length);
    }

    [[nodiscard]] static bool hasFile(const HttpResponse& response) noexcept {
        return response.hasFileBody();
    }

    [[nodiscard]] static ResponseFileBody file(const HttpResponse& response) {
        const auto& body = response.fileBody();
        return ResponseFileBody{
            .nativePath = body.nativePathCStr(),
            .size = body.size_,
            .offset = body.offset_,
            .length = body.length_};
    }
};

inline void setResponseFileBody(HttpResponse& response, std::filesystem::path file, std::uint64_t size) {
    HttpResponseFileAccess::setFile(response, std::move(file), size);
}

inline void setResponseFileBody(
    HttpResponse& response,
    std::filesystem::path file,
    std::uint64_t size,
    std::uint64_t offset,
    std::uint64_t length) {
    HttpResponseFileAccess::setFile(response, std::move(file), size, offset, length);
}

inline void setResponseBorrowedFileBody(
    HttpResponse& response,
    const std::filesystem::path& file,
    std::uint64_t size) {
    HttpResponseFileAccess::setBorrowedFile(response, file, size);
}

inline void setResponseBorrowedFileBody(
    HttpResponse& response,
    const std::filesystem::path& file,
    std::uint64_t size,
    std::uint64_t offset,
    std::uint64_t length) {
    HttpResponseFileAccess::setBorrowedFile(response, file, size, offset, length);
}

inline void setResponseBorrowedNativeFileBody(
    HttpResponse& response,
    const NativePathChar* file,
    std::uint64_t size) {
    HttpResponseFileAccess::setBorrowedNativeFile(response, file, size);
}

inline void setResponseBorrowedNativeFileBody(
    HttpResponse& response,
    const NativePathChar* file,
    std::uint64_t size,
    std::uint64_t offset,
    std::uint64_t length) {
    HttpResponseFileAccess::setBorrowedNativeFile(response, file, size, offset, length);
}

[[nodiscard]] inline bool responseHasFileBody(const HttpResponse& response) noexcept {
    return HttpResponseFileAccess::hasFile(response);
}

[[nodiscard]] inline ResponseFileBody responseFileBody(const HttpResponse& response) {
    return HttpResponseFileAccess::file(response);
}

}  // namespace ruvia::detail
