#include "ruvia/web/detail/callback.h"

#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "test_harness.h"

namespace {

struct callable_lifetime final {
    int live_{};
    bool reject_copy_{};
};

template <bool is_noexcept>
class tracked_callable final {
public:
    tracked_callable(callable_lifetime& lifetime, int value)
        : lifetime_(&lifetime),
          value_(value) {
        ++lifetime_->live_;
    }

    tracked_callable(const tracked_callable& other)
        : lifetime_(other.lifetime_),
          value_(other.value_) {
        if (lifetime_->reject_copy_) {
            throw std::runtime_error("callable copy rejected");
        }
        ++lifetime_->live_;
    }

    tracked_callable(tracked_callable&& other) noexcept
        : lifetime_(std::exchange(other.lifetime_, nullptr)),
          value_(other.value_) {}

    ~tracked_callable() {
        if (lifetime_ != nullptr) {
            --lifetime_->live_;
        }
    }

    int operator()(int offset) noexcept(is_noexcept) {
        if constexpr (!is_noexcept) {
            if (offset < 0) {
                throw std::runtime_error("callback operation failed");
            }
        }
        return value_++ + offset;
    }

private:
    callable_lifetime* lifetime_;
    int value_;
};

template <bool is_noexcept>
void exercise_callback_ownership(ruvia::testing::test_context& ruvia_ctx) {
    using signature_type = std::conditional_t<is_noexcept, int(int) noexcept, int(int)>;
    using owner_type = ruvia::detail::callback<signature_type>;
    callable_lifetime lifetime;
    ruvia::detail::callback_ref_type<signature_type> borrowed;
    {
        owner_type source_value(tracked_callable<is_noexcept>(lifetime, 10));
        owner_type copy(source_value);
        RUVIA_CHECK_EQ(lifetime.live_, 2);
        RUVIA_CHECK_EQ(source_value(2), 12);
        RUVIA_CHECK_EQ(copy(2), 12);
        RUVIA_CHECK(!(source_value == copy));

        borrowed = ruvia::detail::callback_access::ref(source_value);
        RUVIA_CHECK_EQ(borrowed(2), 13);
        RUVIA_CHECK_EQ(copy(2), 13);
        owner_type moved(std::move(source_value));
        RUVIA_CHECK(!source_value);
        RUVIA_CHECK(moved);
        RUVIA_CHECK_EQ(lifetime.live_, 2);
        RUVIA_CHECK_EQ(borrowed(2), 14);

        owner_type assigned(tracked_callable<is_noexcept>(lifetime, 99));
        RUVIA_CHECK_EQ(lifetime.live_, 3);
        lifetime.reject_copy_ = true;
        RUVIA_CHECK(ruvia::testing::throws_on([&] { assigned = moved; }));
        RUVIA_CHECK(ruvia::testing::throws_on([&] { owner_type rejected(moved); }));
        RUVIA_CHECK_EQ(lifetime.live_, 3);
        RUVIA_CHECK_EQ(assigned(0), 99);
        lifetime.reject_copy_ = false;
        assigned = moved;
        RUVIA_CHECK_EQ(lifetime.live_, 3);
        RUVIA_CHECK_EQ(assigned(0), 13);
        assigned = std::move(copy);
        RUVIA_CHECK(!copy);
        RUVIA_CHECK_EQ(lifetime.live_, 2);
        assigned = nullptr;
        RUVIA_CHECK(!assigned);
        RUVIA_CHECK_EQ(lifetime.live_, 1);
    }
    // An escaped invocation view must not retain its owner's callable storage.
    RUVIA_CHECK_EQ(lifetime.live_, 0);
    borrowed = nullptr;
}

RUVIA_TEST(callback_owners_copy_move_and_release_both_signature_policies) {
    exercise_callback_ownership<false>(ruvia_ctx);
    exercise_callback_ownership<true>(ruvia_ctx);
}

struct moved_argument final {
    moved_argument(int& moves, int value)
        : moves_(&moves),
          value_(std::make_unique<int>(value)) {}
    moved_argument(moved_argument&& other) noexcept
        : moves_(other.moves_),
          value_(std::move(other.value_)) {
        ++*moves_;
    }
    int* moves_;
    std::unique_ptr<int> value_;
};

RUVIA_TEST(callback_owner_and_borrowed_view_transfer_arguments_equivalently) {
    ruvia::detail::callback<int(moved_argument)> owner_value([](moved_argument argument) { return *argument.value_; });
    const auto view = ruvia::detail::callback_access::ref(owner_value);
    int owner_moves{};
    int view_moves{};
    RUVIA_CHECK_EQ(owner_value(moved_argument(owner_moves, 7)), 7);
    RUVIA_CHECK_EQ(view(moved_argument(view_moves, 9)), 9);
    RUVIA_CHECK_EQ(owner_moves, view_moves);
}

RUVIA_TEST(callback_invocation_preserves_operation_failure_and_empty_owner_errors) {
    ruvia::detail::callback<int(int)> empty;
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)empty(1); }));
    callable_lifetime lifetime;
    ruvia::detail::callback<int(int)> callback_value(tracked_callable<false>(lifetime, 7));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)callback_value(-1); }));
    RUVIA_CHECK_EQ(callback_value(1), 8);
}

}  // namespace
