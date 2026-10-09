#pragma once

#include <optional>
#include <span>
#include <utility>

#include "ruvia/core/task.h"
#include "ruvia/web/streaming.h"

#include "http/request_body_loader.h"
#include "http/streaming_access.h"

namespace ruvia::detail {

template <typename reader_type>
[[nodiscard]] task<std::optional<std::span<const std::byte>>> body_reader_read_thunk(void* target) {
    return static_cast<reader_type*>(target)->read();
}

template <typename reader_type>
void emplace_body_reader_facade(std::optional<body_reader>& storage, reader_type& reader_value) {
    streaming_access::emplace_body_reader(storage, &reader_value, &body_reader_read_thunk<reader_type>);
}

template <typename reader_type>
[[nodiscard]] body_reader make_body_reader_facade(reader_type& reader_value) noexcept {
    return streaming_access::make_body_reader(&reader_value, &body_reader_read_thunk<reader_type>);
}

template <typename reader_type>
class body_reader_binding final {
public:
    template <typename... args_type>
    explicit body_reader_binding(args_type&&... args)
        : reader_(std::forward<args_type>(args)...),
          facade_(make_body_reader_facade(reader_)) {}

    body_reader_binding(const body_reader_binding&) = delete;
    body_reader_binding& operator=(const body_reader_binding&) = delete;
    body_reader_binding(body_reader_binding&&) = delete;
    body_reader_binding& operator=(body_reader_binding&&) = delete;

    [[nodiscard]] reader_type& reader() noexcept {
        return reader_;
    }
    [[nodiscard]] const reader_type& reader() const noexcept {
        return reader_;
    }
    [[nodiscard]] body_reader& facade() noexcept {
        return facade_;
    }

private:
    reader_type reader_;
    body_reader facade_;
};

template <typename loader_type>
[[nodiscard]] task<std::string_view> request_body_loader_read_all_thunk(void* target) {
    return static_cast<loader_type*>(target)->read_all();
}

template <typename loader_type>
task<void> request_body_loader_discard_thunk(void* target) {
    return static_cast<loader_type*>(target)->discard();
}

template <typename loader_type>
[[nodiscard]] request_body_loader make_request_body_loader_facade(loader_type& loader) noexcept {
    return request_body_loader(
        &loader, &request_body_loader_read_all_thunk<loader_type>, &request_body_loader_discard_thunk<loader_type>);
}

template <typename loader_type>
class request_body_loader_binding final {
public:
    template <typename... args_type>
    explicit request_body_loader_binding(args_type&&... args)
        : loader_(std::forward<args_type>(args)...),
          facade_(make_request_body_loader_facade(loader_)) {}

    request_body_loader_binding(const request_body_loader_binding&) = delete;
    request_body_loader_binding& operator=(const request_body_loader_binding&) = delete;
    request_body_loader_binding(request_body_loader_binding&&) = delete;
    request_body_loader_binding& operator=(request_body_loader_binding&&) = delete;

    [[nodiscard]] loader_type& loader() noexcept {
        return loader_;
    }
    [[nodiscard]] const loader_type& loader() const noexcept {
        return loader_;
    }
    [[nodiscard]] request_body_loader& facade() noexcept {
        return facade_;
    }

private:
    loader_type loader_;
    request_body_loader facade_;
};

}  // namespace ruvia::detail
