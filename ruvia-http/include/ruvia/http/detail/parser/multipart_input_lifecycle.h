#pragma once

#include <cstddef>
#include <memory_resource>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/http_io_buffer.h"

namespace ruvia::detail {

struct multipart_borrowed_input final {
    std::string_view bytes_;
};

struct multipart_streaming_input_open final {
    explicit multipart_streaming_input_open(std::pmr::memory_resource* resource) noexcept
        : bytes_(http_io_buffer::allocator_type(resource)) {}
    http_io_buffer bytes_;
};

struct multipart_streaming_input_eof final {
    explicit multipart_streaming_input_eof(http_io_buffer&& input) noexcept
        : bytes_(std::move(input)) {}
    http_io_buffer bytes_;
};

// Owns the multipart byte source and its EOF lifecycle. Parser grammar progress
// remains orthogonal in multipart_parser::state_.
class multipart_input_lifecycle final {
public:
    explicit multipart_input_lifecycle(std::pmr::memory_resource* resource);
    explicit multipart_input_lifecycle(multipart_borrowed_input input) noexcept;

    [[nodiscard]] const multipart_borrowed_input* borrowed() const& noexcept;
    [[nodiscard]] const multipart_borrowed_input* borrowed() const&& = delete;
    [[nodiscard]] const multipart_streaming_input_open* streaming_open() const& noexcept;
    [[nodiscard]] const multipart_streaming_input_open* streaming_open() const&& = delete;
    [[nodiscard]] const multipart_streaming_input_eof* streaming_eof() const& noexcept;
    [[nodiscard]] const multipart_streaming_input_eof* streaming_eof() const&& = delete;
    [[nodiscard]] bool eof() const noexcept;
    [[nodiscard]] std::string_view view() const& noexcept;
    [[nodiscard]] std::string_view view() const&& = delete;

    void feed(std::string_view chunk);
    void finish_input() noexcept;
    void consume(std::size_t bytes) noexcept;

private:
    using value_type = std::variant<multipart_borrowed_input, multipart_streaming_input_open,
        multipart_streaming_input_eof>;

    [[nodiscard]] http_io_buffer* owned_buffer() noexcept;
    [[nodiscard]] const http_io_buffer* owned_buffer() const noexcept;

    value_type value_;
    std::size_t borrowed_offset_{0};
};

}  // namespace ruvia::detail
