#pragma once

#include <system_error>
#include <utility>
#include <variant>

namespace ruvia::detail {

enum class HttpStreamEnd : unsigned char { kKeepOpen,
    kEnd };

class HttpStreamReadData final {
private:
    constexpr HttpStreamReadData() noexcept = default;
    friend class HttpStreamReadResult;
};

class HttpStreamReadEnd final {
private:
    constexpr HttpStreamReadEnd() noexcept = default;
    friend class HttpStreamReadResult;
};

class HttpStreamReadFailure final {
public:
    [[nodiscard]] std::error_code errorCode() const noexcept {
        return errorCode_;
    }

private:
    explicit HttpStreamReadFailure(std::error_code errorCode) noexcept
        : errorCode_(errorCode) {}
    friend class HttpStreamReadResult;

    std::error_code errorCode_;
};

class HttpStreamReadResult final {
public:
    [[nodiscard]] static constexpr HttpStreamReadResult makeData() noexcept {
        return HttpStreamReadResult(HttpStreamReadData{});
    }

    [[nodiscard]] static constexpr HttpStreamReadResult makeEnd() noexcept {
        return HttpStreamReadResult(HttpStreamReadEnd{});
    }

    [[nodiscard]] static HttpStreamReadResult makeFailure(std::error_code errorCode) noexcept {
        return HttpStreamReadResult(HttpStreamReadFailure(errorCode));
    }

    [[nodiscard]] const HttpStreamReadData* data() const& noexcept {
        return std::get_if<HttpStreamReadData>(&value_);
    }
    const HttpStreamReadData* data() const&& = delete;

    [[nodiscard]] const HttpStreamReadEnd* end() const& noexcept {
        return std::get_if<HttpStreamReadEnd>(&value_);
    }
    const HttpStreamReadEnd* end() const&& = delete;

    [[nodiscard]] const HttpStreamReadFailure* failure() const& noexcept {
        return std::get_if<HttpStreamReadFailure>(&value_);
    }
    const HttpStreamReadFailure* failure() const&& = delete;

private:
    explicit constexpr HttpStreamReadResult(HttpStreamReadData data) noexcept
        : value_(data) {}

    explicit constexpr HttpStreamReadResult(HttpStreamReadEnd end) noexcept
        : value_(end) {}

    explicit HttpStreamReadResult(HttpStreamReadFailure failure) noexcept
        : value_(std::move(failure)) {}

    std::variant<HttpStreamReadData, HttpStreamReadEnd, HttpStreamReadFailure> value_;
};

}  // namespace ruvia::detail
