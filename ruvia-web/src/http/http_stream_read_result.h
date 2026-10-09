#pragma once

#include <system_error>
#include <utility>
#include <variant>

namespace ruvia::detail {

enum class http_stream_end : unsigned char { keep_open,
    end };

class http_stream_read_data final {
private:
    constexpr http_stream_read_data() noexcept = default;
    friend class http_stream_read_result;
};

class http_stream_read_end final {
private:
    constexpr http_stream_read_end() noexcept = default;
    friend class http_stream_read_result;
};

class http_stream_read_failure final {
public:
    [[nodiscard]] std::error_code error_code() const noexcept {
        return error_code_;
    }

private:
    explicit http_stream_read_failure(std::error_code error_code) noexcept
        : error_code_(error_code) {}
    friend class http_stream_read_result;

    std::error_code error_code_;
};

class http_stream_read_result final {
public:
    [[nodiscard]] static constexpr http_stream_read_result make_data() noexcept {
        return http_stream_read_result(http_stream_read_data{});
    }

    [[nodiscard]] static constexpr http_stream_read_result make_end() noexcept {
        return http_stream_read_result(http_stream_read_end{});
    }

    [[nodiscard]] static http_stream_read_result make_failure(std::error_code error_code) noexcept {
        return http_stream_read_result(http_stream_read_failure(error_code));
    }

    [[nodiscard]] const http_stream_read_data* data() const& noexcept {
        return std::get_if<http_stream_read_data>(&value_);
    }
    const http_stream_read_data* data() const&& = delete;

    [[nodiscard]] const http_stream_read_end* end() const& noexcept {
        return std::get_if<http_stream_read_end>(&value_);
    }
    const http_stream_read_end* end() const&& = delete;

    [[nodiscard]] const http_stream_read_failure* failure() const& noexcept {
        return std::get_if<http_stream_read_failure>(&value_);
    }
    const http_stream_read_failure* failure() const&& = delete;

private:
    explicit constexpr http_stream_read_result(http_stream_read_data data) noexcept
        : value_(data) {}

    explicit constexpr http_stream_read_result(http_stream_read_end end) noexcept
        : value_(end) {}

    explicit http_stream_read_result(http_stream_read_failure failure) noexcept
        : value_(std::move(failure)) {}

    std::variant<http_stream_read_data, http_stream_read_end, http_stream_read_failure> value_;
};

}  // namespace ruvia::detail
