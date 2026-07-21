#pragma once

#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

namespace ruvia {

template <typename Signature>
class MoveOnlyFunction;

template <typename Result, typename... Args>
class MoveOnlyFunction<Result(Args...)> final {
public:
    MoveOnlyFunction() noexcept = default;
    MoveOnlyFunction(std::nullptr_t) noexcept {}

    template <typename Fn>
        requires (!std::same_as<std::remove_cvref_t<Fn>, MoveOnlyFunction>) &&
                 std::is_invocable_r_v<Result, std::decay_t<Fn>&, Args...>
    MoveOnlyFunction(Fn&& fn) {
        using Stored = std::decay_t<Fn>;
        if constexpr (fitsInline<Stored>) {
            object_ = storage_;
            ::new (object_) Stored(std::forward<Fn>(fn));
            operations_ = &inlineOperations<Stored>;
        } else {
            object_ = new Stored(std::forward<Fn>(fn));
            operations_ = &heapOperations<Stored>;
        }
    }

    ~MoveOnlyFunction() {
        reset();
    }

    MoveOnlyFunction(const MoveOnlyFunction&) = delete;
    MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;

    MoveOnlyFunction(MoveOnlyFunction&& other) noexcept {
        moveFrom(other);
    }

    MoveOnlyFunction& operator=(MoveOnlyFunction&& other) noexcept {
        if (this != &other) {
            reset();
            moveFrom(other);
        }
        return *this;
    }

    MoveOnlyFunction& operator=(std::nullptr_t) noexcept {
        reset();
        return *this;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return operations_ != nullptr;
    }

    Result operator()(Args... args) {
        if (operations_ == nullptr) {
            std::terminate();
        }
        return operations_->invoke(object_, std::forward<Args>(args)...);
    }

private:
    static constexpr std::size_t kInlineSize = 3 * sizeof(void*);

    struct Operations final {
        Result (*invoke)(void*, Args&&...);
        void (*destroy)(void*) noexcept;
        void (*move)(void*, void*) noexcept;
    };

    template <typename Stored>
    static constexpr bool fitsInline =
        sizeof(Stored) <= kInlineSize &&
        alignof(Stored) <= alignof(std::max_align_t) &&
        std::is_nothrow_move_constructible_v<Stored>;

    template <typename Stored>
    static Result invoke(void* object, Args&&... args) {
        return std::invoke(
            *static_cast<Stored*>(object), std::forward<Args>(args)...);
    }

    template <typename Stored>
    static void destroyInline(void* object) noexcept {
        static_cast<Stored*>(object)->~Stored();
    }

    template <typename Stored>
    static void moveInline(void* source, void* destination) noexcept {
        auto* stored = static_cast<Stored*>(source);
        ::new (destination) Stored(std::move(*stored));
        stored->~Stored();
    }

    template <typename Stored>
    static void destroyHeap(void* object) noexcept {
        delete static_cast<Stored*>(object);
    }

    static void moveHeap(void*, void*) noexcept {}

    template <typename Stored>
    static inline constexpr Operations inlineOperations{
        &invoke<Stored>, &destroyInline<Stored>, &moveInline<Stored>};

    template <typename Stored>
    static inline constexpr Operations heapOperations{
        &invoke<Stored>, &destroyHeap<Stored>, &moveHeap};

    [[nodiscard]] bool isInline() const noexcept {
        return object_ == static_cast<const void*>(storage_);
    }

    void reset() noexcept {
        if (operations_ != nullptr) {
            operations_->destroy(object_);
            operations_ = nullptr;
            object_ = nullptr;
        }
    }

    void moveFrom(MoveOnlyFunction& other) noexcept {
        if (other.operations_ == nullptr) {
            return;
        }
        operations_ = other.operations_;
        if (other.isInline()) {
            object_ = storage_;
            operations_->move(other.object_, object_);
        } else {
            object_ = other.object_;
        }
        other.operations_ = nullptr;
        other.object_ = nullptr;
    }

    alignas(std::max_align_t) std::byte storage_[kInlineSize];
    void* object_{nullptr};
    const Operations* operations_{nullptr};
};

}  // namespace ruvia
