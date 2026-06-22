#pragma once

#include "ruvia/app/Task.h"

#include <stdexcept>
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

    [[nodiscard]] explicit operator bool() const noexcept {
        return readAll_ != nullptr && discard_ != nullptr;
    }

    [[nodiscard]] Task<std::string_view> readAll() {
        if (readAll_ == nullptr) {
            throw std::logic_error("request body is not buffered");
        }
        return readAll_(target_);
    }

    Task<void> discard() {
        if (discard_ == nullptr) {
            throw std::logic_error("request body is not buffered");
        }
        return discard_(target_);
    }

private:
    void* target_{nullptr};
    ReadAll readAll_{nullptr};
    Discard discard_{nullptr};
};

}  // namespace ruvia::detail
