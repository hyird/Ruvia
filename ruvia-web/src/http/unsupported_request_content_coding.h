#pragma once

#include <exception>
#include <string_view>

#include "ruvia/http/http_content_coding.h"

namespace ruvia::detail {

// Web-runtime signal for an otherwise valid request whose complete
// Content-Encoding stack cannot be decoded. router dispatch owns the 415 JSON
// mapping and the RFC 9110 Accept-Encoding response advertisement, which names
// what the failed read accepts: a streaming read decodes nothing.
class unsupported_request_content_coding final : public std::exception {
public:
    enum class read_type { decoded,
        streaming };

    explicit unsupported_request_content_coding(read_type read) noexcept
        : read_(read) {}

    [[nodiscard]] const char* what() const noexcept override {
        return "request Content-Encoding is not supported";
    }

    // An Accept-Encoding field value with static storage.
    [[nodiscard]] std::string_view accepted_codings() const noexcept {
        return read_ == read_type::decoded ? http_supported_request_content_codings() : "identity";
    }

private:
    read_type read_;
};

}  // namespace ruvia::detail
