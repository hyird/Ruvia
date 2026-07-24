#pragma once

#include "ruvia/http/detail/coding/HttpContentCoding.h"

#include <exception>

namespace ruvia::detail {

// Web-runtime signal for an otherwise valid request whose complete
// Content-Encoding stack cannot be decoded. Router dispatch owns the 415 JSON
// mapping and the RFC 9110 Accept-Encoding response advertisement.
class UnsupportedRequestContentCoding final : public std::exception {
public:
    explicit UnsupportedRequestContentCoding(const HttpUnsupportedContentCoding&) noexcept {}

    [[nodiscard]] const char* what() const noexcept override {
        return "request Content-Encoding is not supported";
    }
};

}  // namespace ruvia::detail
