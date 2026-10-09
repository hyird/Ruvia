#pragma once

#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

namespace ruvia {

namespace detail {

template <typename result_type, typename fn_type, typename... args_type>
concept move_only_function_target =
    (std::is_void_v<result_type> &&
        std::same_as<std::invoke_result_t<std::decay_t<fn_type>&, args_type...>, void>) ||
    (!std::is_void_v<result_type> && std::is_invocable_r_v<result_type, std::decay_t<fn_type>&, args_type...>);

}  // namespace detail

template <typename signature_type>
class move_only_function;

namespace detail {

inline constexpr std::size_t move_only_function_inline_size = 3 * sizeof(void*);
inline constexpr std::size_t move_only_function_inline_alignment = alignof(std::max_align_t);

template <typename stored_type>
inline constexpr bool move_only_function_fits_inline =
    sizeof(stored_type) <= move_only_function_inline_size &&
    alignof(stored_type) <= move_only_function_inline_alignment &&
    std::is_nothrow_move_constructible_v<stored_type>;

template <typename signature_type, typename fn_type>
inline constexpr bool move_only_function_borrow_safe_input =
    !std::same_as<std::remove_cvref_t<fn_type>, move_only_function<signature_type>> &&
    move_only_function_fits_inline<std::decay_t<fn_type>> &&
    std::is_trivially_constructible_v<std::decay_t<fn_type>, fn_type&&> &&
    std::is_trivially_move_constructible_v<std::decay_t<fn_type>> &&
    std::is_trivially_destructible_v<std::decay_t<fn_type>>;

}  // namespace detail

template <typename result_type, typename... args_type>
class move_only_function<result_type(args_type...)> final {
public:
    move_only_function() noexcept = default;
    move_only_function(std::nullptr_t) noexcept {}

    template <typename fn_type>
        requires(!std::same_as<std::remove_cvref_t<fn_type>, move_only_function>) &&
                detail::move_only_function_target<result_type, fn_type, args_type...>
    move_only_function(fn_type&& fn) {
        using stored_type = std::decay_t<fn_type>;
        if constexpr (std::is_pointer_v<std::remove_reference_t<fn_type>> || std::is_member_pointer_v<std::remove_reference_t<fn_type>>) {
            if (fn == nullptr) {
                return;
            }
        }
        if constexpr (detail::move_only_function_fits_inline<stored_type>) {
            object_ = storage_;
            ::new (object_) stored_type(std::forward<fn_type>(fn));
            operations_ = &inline_operations<stored_type>;
        } else {
            object_ = new stored_type(std::forward<fn_type>(fn));
            operations_ = &heap_operations<stored_type>;
        }
    }

    ~move_only_function() {
        reset();
    }

    move_only_function(const move_only_function&) = delete;
    move_only_function& operator=(const move_only_function&) = delete;

    move_only_function(move_only_function&& other) noexcept {
        move_from(other);
    }

    move_only_function& operator=(move_only_function&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(other);
        }
        return *this;
    }

    move_only_function& operator=(std::nullptr_t) noexcept {
        reset();
        return *this;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return operations_ != nullptr;
    }

    // The value-parameter signature is part of the erased callable contract;
    // forwarding it preserves value and reference argument behavior.
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    result_type operator()(args_type... args) {
        if (operations_ == nullptr) {
            std::terminate();
        }
        return operations_->invoke_(object_, std::forward<args_type>(args)...);
    }

private:
    // The members are initialized explicitly rather than left to default
    // construction: the operation tables below are const static aggregates, and
    // MSVC reports C4268 when such an object is zero-filled by a compiler
    // generated default constructor. Aggregate initialization is unaffected.
    struct operations final {
        result_type (*invoke_)(void*, args_type&&...) = nullptr;
        void (*destroy_)(void*) noexcept = nullptr;
        void (*move_)(void*, void*) noexcept = nullptr;
    };

    template <typename stored_type>
    static result_type invoke(void* object, args_type&&... args) {
        return std::invoke(*static_cast<stored_type*>(object), std::forward<args_type>(args)...);
    }

    template <typename stored_type>
    static void destroy_inline(void* object) noexcept {
        static_cast<stored_type*>(object)->~stored_type();
    }

    template <typename stored_type>
    static void move_inline(void* source_value, void* destination) noexcept {
        auto* stored = static_cast<stored_type*>(source_value);
        ::new (destination) stored_type(std::move(*stored));
        stored->~stored_type();
    }

    template <typename stored_type>
    static void destroy_heap(void* object) noexcept {
        delete static_cast<stored_type*>(object);
    }

    static void move_heap(void*, void*) noexcept {}

    template <typename stored_type>
    static inline constexpr operations inline_operations{
        &invoke<stored_type>, &destroy_inline<stored_type>, &move_inline<stored_type>};

    template <typename stored_type>
    static inline constexpr operations heap_operations{
        &invoke<stored_type>, &destroy_heap<stored_type>, &move_heap};

    [[nodiscard]] bool is_inline() const noexcept {
        return object_ == static_cast<const void*>(storage_);
    }

    void reset() noexcept {
        if (operations_ != nullptr) {
            operations_->destroy_(object_);
            operations_ = nullptr;
            object_ = nullptr;
        }
    }

    void move_from(move_only_function& other) noexcept {
        if (other.operations_ == nullptr) {
            return;
        }
        operations_ = other.operations_;
        if (other.is_inline()) {
            object_ = storage_;
            operations_->move_(other.object_, object_);
        } else {
            object_ = other.object_;
        }
        other.operations_ = nullptr;
        other.object_ = nullptr;
    }

    alignas(detail::move_only_function_inline_alignment)
        std::byte storage_[detail::move_only_function_inline_size];
    void* object_{nullptr};
    const operations* operations_{nullptr};
};

}  // namespace ruvia
