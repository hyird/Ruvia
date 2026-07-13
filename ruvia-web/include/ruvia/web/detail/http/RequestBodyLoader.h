#pragma once

#include "ruvia/core/Task.h"

#include <string_view>

namespace ruvia::detail {

class RequestBodyLoader final {
public:
    using ReadAll = Task<std::string_view> (*)(void*);
    using Discard = Task<void> (*)(void*);

    constexpr RequestBodyLoader(void* target, ReadAll readAll, Discard discard) noexcept
        : target_(target), readAll_(readAll), discard_(discard) {}

    RequestBodyLoader(const RequestBodyLoader&) = delete;
    RequestBodyLoader& operator=(const RequestBodyLoader&) = delete;

    [[nodiscard]] Task<std::string_view> readAll() {
        return readAll_(target_);
    }

    Task<void> discard() {
        return discard_(target_);
    }

private:
    void* target_;
    ReadAll readAll_;
    Discard discard_;
};

}  // namespace ruvia::detail
