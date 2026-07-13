#pragma once

#include "ruvia/http/detail/HttpContentCoding.h"

#include <cstdint>
#include <exception>

namespace ruvia::detail {

// Web-runtime signal for an otherwise valid request whose complete
// Content-Encoding stack cannot be decoded. Router dispatch owns the 415 JSON
// mapping and the RFC 9110 Accept-Encoding response advertisement.
class UnsupportedRequestContentCoding final : public std::exception {
public:
    explicit UnsupportedRequestContentCoding(
        const HttpUnsupportedContentCoding&) noexcept
        : status_(HttpUnsupportedContentCoding::status()) {}

    [[nodiscard]] std::uint16_t status() const noexcept {
        return status_;
    }

    [[nodiscard]] const char* what() const noexcept override {
        return "request Content-Encoding is not supported";
    }

private:
    std::uint16_t status_;
};

}  // namespace ruvia::detail
