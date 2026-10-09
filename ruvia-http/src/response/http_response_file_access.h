#pragma once

#include <cstdint>
#include <filesystem>
#include <utility>

#include "ruvia/http/detail/util/native_path.h"
#include "ruvia/http/http_response.h"

namespace ruvia::detail {

struct http_response_file_access final {
    static void set_file(http_response& response, std::filesystem::path file, std::uint64_t size) {
        response.set_file_body(std::move(file), size);
    }

    static void set_file(http_response& response, std::filesystem::path file, std::uint64_t size,
        std::uint64_t offset, std::uint64_t length) {
        response.set_file_body(std::move(file), size, offset, length);
    }

    static void set_file(http_response& response, std::filesystem::path file, std::uint64_t size,
        std::uint64_t offset, std::uint64_t length, http_response_file_identity identity) {
        response.set_file_body(std::move(file), size, offset, length, identity);
    }

    static void set_borrowed_file(
        http_response& response, const std::filesystem::path& file, std::uint64_t size) {
        response.set_borrowed_file_body(file, size);
    }

    static void set_borrowed_native_file(
        http_response& response, const http_native_path_char_type* file, std::uint64_t size) {
        response.set_borrowed_native_file_body(file, size);
    }

    static void set_borrowed_file(http_response& response, const std::filesystem::path& file,
        std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
        response.set_borrowed_file_body(file, size, offset, length);
    }

    static void set_borrowed_native_file(http_response& response, const http_native_path_char_type* file,
        std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
        response.set_borrowed_native_file_body(file, size, offset, length);
    }
};

inline void set_response_file_body(
    http_response& response, std::filesystem::path file, std::uint64_t size) {
    http_response_file_access::set_file(response, std::move(file), size);
}

inline void set_response_file_body(http_response& response, std::filesystem::path file,
    std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
    http_response_file_access::set_file(response, std::move(file), size, offset, length);
}

inline void set_response_file_body(http_response& response, std::filesystem::path file,
    std::uint64_t size, std::uint64_t offset, std::uint64_t length, http_response_file_identity identity) {
    http_response_file_access::set_file(response, std::move(file), size, offset, length, identity);
}

inline void set_response_borrowed_file_body(
    http_response& response, const std::filesystem::path& file, std::uint64_t size) {
    http_response_file_access::set_borrowed_file(response, file, size);
}

inline void set_response_borrowed_file_body(http_response& response, const std::filesystem::path& file,
    std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
    http_response_file_access::set_borrowed_file(response, file, size, offset, length);
}

inline void set_response_borrowed_native_file_body(
    http_response& response, const http_native_path_char_type* file, std::uint64_t size) {
    http_response_file_access::set_borrowed_native_file(response, file, size);
}

inline void set_response_borrowed_native_file_body(http_response& response,
    const http_native_path_char_type* file, std::uint64_t size, std::uint64_t offset,
    std::uint64_t length) {
    http_response_file_access::set_borrowed_native_file(response, file, size, offset, length);
}

}  // namespace ruvia::detail
