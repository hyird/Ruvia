#pragma once

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/memory/process_resource.h"
#include "ruvia/web/detail/callback_ref.h"

namespace ruvia::detail {

// Value-semantic PMR owner composed with the allocation-free invocation view.
// Copy/move/clone/destruction have one implementation for both signature
// policies. Captured references must still outlive the owning callback.
template <typename result_type, bool is_noexcept, typename... args_types>
class callback_owner final {
public:
    constexpr callback_owner() noexcept = default;
    constexpr callback_owner(std::nullptr_t) noexcept {}

    template <typename callable_type, typename stored_type = std::decay_t<callable_type>>
        requires(!std::is_same_v<stored_type, callback_owner> &&
                    std::is_invocable_r_v<result_type, stored_type&, args_types...> &&
                    std::is_copy_constructible_v<stored_type> &&
                    (!is_noexcept || (!std::is_lvalue_reference_v<callable_type> &&
                                         std::is_nothrow_invocable_r_v<result_type, stored_type&, args_types...>)))
    callback_owner(callable_type&& callable)
        : view_(construct_pmr_object<stored_type>(process_resource(), std::forward<callable_type>(callable)),
              [](void* target, args_types... args) noexcept(is_noexcept) -> result_type {
                  return (*static_cast<stored_type*>(target))(std::forward<args_types>(args)...);
              }),
          destroy_([](void* target, std::pmr::memory_resource* resource) noexcept {
              destroy_pmr_object(static_cast<stored_type*>(target), resource);
          }),
          clone_([](const void* target, std::pmr::memory_resource* resource) -> void* {
              return construct_pmr_object<stored_type>(resource, *static_cast<const stored_type*>(target));
          }),
          resource_(process_resource()) {}

    callback_owner(const callback_owner& other) {
        copy_from(other);
    }

    callback_owner& operator=(const callback_owner& other) {
        if (this != &other) {
            callback_owner copy(other);
            swap(copy);
        }
        return *this;
    }

    callback_owner(callback_owner&& other) noexcept {
        move_from(other);
    }

    callback_owner& operator=(callback_owner&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(other);
        }
        return *this;
    }

    ~callback_owner() {
        reset();
    }

    [[nodiscard]] result_type operator()(args_types... args) const noexcept(is_noexcept) {
        if (!view_) {
            if constexpr (is_noexcept) {
                std::terminate();
            } else {
                throw std::logic_error("callback is empty");
            }
        }
        return view_.invoke_target(std::forward<args_types>(args)...);
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return static_cast<bool>(view_);
    }

    [[nodiscard]] friend constexpr bool operator==(
        const callback_owner& left, const callback_owner& right) noexcept {
        return left.view_ == right.view_;
    }

private:
    friend struct callback_access;
    using view_type = callback_ref<result_type, is_noexcept, args_types...>;
    using destroy_type = void (*)(void*, std::pmr::memory_resource*) noexcept;
    using clone_type = void* (*)(const void*, std::pmr::memory_resource*);

    [[nodiscard]] constexpr view_type callback_ref_view() const noexcept {
        return view_;
    }

    void copy_from(const callback_owner& other) {
        auto* const target = other.clone_ == nullptr
                                 ? other.view_.target_
                                 : other.clone_(other.view_.target_, other.resource_);
        view_ = view_type(target, other.view_.invoke_);
        destroy_ = other.destroy_;
        clone_ = other.clone_;
        resource_ = other.resource_;
    }

    void move_from(callback_owner& other) noexcept {
        view_ = std::exchange(other.view_, {});
        destroy_ = std::exchange(other.destroy_, nullptr);
        clone_ = std::exchange(other.clone_, nullptr);
        resource_ = std::exchange(other.resource_, nullptr);
    }

    void swap(callback_owner& other) noexcept {
        std::swap(view_, other.view_);
        std::swap(destroy_, other.destroy_);
        std::swap(clone_, other.clone_);
        std::swap(resource_, other.resource_);
    }

    void reset() noexcept {
        if (destroy_ != nullptr) {
            destroy_(view_.target_, resource_);
        }
        view_ = {};
        destroy_ = nullptr;
        clone_ = nullptr;
        resource_ = nullptr;
    }

    view_type view_;
    destroy_type destroy_{nullptr};
    clone_type clone_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
};

template <typename signature_type>
using callback = typename callback_signature<signature_type>::template apply<callback_owner>;

}  // namespace ruvia::detail
