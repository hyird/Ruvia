#pragma once

#include <initializer_list>
#include <stdexcept>
#include <string_view>

namespace ruvia::detail {

// OpenSSL consumes C strings; validate the complete path before any file access.
// Passwords are binary data and do not belong to this validation.
inline void validate_tls_file_paths(std::initializer_list<std::string_view> paths) {
    for (const auto path : paths) {
        if (path.contains('\0')) {
            throw std::invalid_argument("TLS file paths must not contain NUL bytes");
        }
    }
}

}  // namespace ruvia::detail
