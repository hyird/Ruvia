#pragma once

#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

namespace ruvia {

namespace detail {
struct MultipartPartAccess;
}  // namespace detail

class MultipartPart final {
public:
    [[nodiscard]] std::string_view name() const noexcept {
        return std::string_view(name_.data(), name_.size());
    }

    [[nodiscard]] std::string_view filename() const noexcept {
        return std::string_view(filename_.data(), filename_.size());
    }

    [[nodiscard]] std::string_view contentType() const noexcept {
        return contentType_;
    }

    [[nodiscard]] std::string_view body() const noexcept {
        return body_;
    }

private:
    friend struct detail::MultipartPartAccess;

    // name/filename are owned (decoded from Content-Disposition, so they may differ
    // from the raw request bytes and cannot be views into them); contentType/body
    // remain borrowed views into the request body.
    MultipartPart(
        std::pmr::string name,
        std::pmr::string filename,
        std::string_view contentType,
        std::string_view body) noexcept
        : name_(std::move(name)),
          filename_(std::move(filename)),
          contentType_(contentType),
          body_(body) {}

    std::pmr::string name_;
    std::pmr::string filename_;
    std::string_view contentType_;
    std::string_view body_;
};

}  // namespace ruvia
