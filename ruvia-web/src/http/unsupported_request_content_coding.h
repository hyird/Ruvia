#pragma once

#include <exception>

#include "ruvia/http/http_content_coding.h"

namespace ruvia::detail {

// Web-runtime signal for an otherwise valid request whose complete
// Content-Encoding stack cannot be decoded. router dispatch owns the 415 JSON
// mapping and the RFC 9110 Accept-Encoding response advertisement.
class unsupported_request_content_coding final : public std::exception {
public:
    explicit unsupported_request_content_coding(const http_unsupported_content_coding&) noexcept {}

    [[nodiscard]] const char* what() const noexcept override {
        return "request Content-Encoding is not supported";
    }
};

}  // namespace ruvia::detail
