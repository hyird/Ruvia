#pragma once

#include <exception>
#include <string_view>

namespace ruvia::detail {

// Web-runtime signal for an otherwise valid request whose complete
// Content-Encoding stack cannot be decoded. router dispatch owns the 415 JSON
// mapping and the RFC 9110 Accept-Encoding response advertisement, which names
// what the failed read accepts: a streaming read decodes nothing.
class unsupported_request_content_coding final : public std::exception {
public:
    // `accepted_codings` is an Accept-Encoding field value with static storage.
    explicit unsupported_request_content_coding(std::string_view accepted_codings) noexcept
        : accepted_codings_(accepted_codings) {}

    [[nodiscard]] const char* what() const noexcept override {
        return "request Content-Encoding is not supported";
    }

    [[nodiscard]] std::string_view accepted_codings() const noexcept {
        return accepted_codings_;
    }

private:
    std::string_view accepted_codings_;
};

}  // namespace ruvia::detail
