#include "test_harness.h"
#include "memory_resource_fixture.h"

#include <cstddef>
#include <utility>

#include "ruvia/web/ModelTypes.h"

namespace {

using ruvia::test::CountingMemoryResource;

class TrackedValue final {
public:
    explicit TrackedValue(int value) noexcept
        : value_(value) {
        ++alive_;
    }

    TrackedValue(const TrackedValue&) = delete;
    TrackedValue& operator=(const TrackedValue&) = delete;

    TrackedValue(TrackedValue&& other) noexcept
        : value_(other.value_) {
        other.value_ = -1;
        ++alive_;
    }

    TrackedValue& operator=(TrackedValue&&) = delete;

    ~TrackedValue() {
        --alive_;
    }

    [[nodiscard]] int value() const noexcept {
        return value_;
    }

    [[nodiscard]] static std::size_t alive() noexcept {
        return alive_;
    }

private:
    int value_;
    static inline std::size_t alive_{0};
};

}  // namespace

template <typename T>
concept ExposesAnyRvalueModelBoxedArrayBorrow =
    requires { std::declval<const T&&>()[std::size_t{}]; } ||
    requires { std::declval<const T&&>().front(); } ||
    requires { std::declval<const T&&>().begin(); } ||
    requires { std::declval<const T&&>().end(); } || requires { std::declval<T&&>().emplace(1); } ||
    requires { std::declval<T&&>().emplaceMove(typename T::value_type{}); };

static_assert(!ExposesAnyRvalueModelBoxedArrayBorrow<ruvia::BoxedArray<ruvia::Int32>>);

RUVIA_TEST(model_list_clear_and_destructor_release_owned_elements) {
    CountingMemoryResource resource;
    {
        ruvia::BoxedArray<TrackedValue> values({.resource = &resource});
        values.emplace(1);
        values.emplace(2);
        RUVIA_CHECK_EQ(TrackedValue::alive(), std::size_t{2});

        values.clear();
        RUVIA_CHECK(values.empty());
        RUVIA_CHECK_EQ(TrackedValue::alive(), std::size_t{0});

        values.emplace(3);
        RUVIA_CHECK_EQ(TrackedValue::alive(), std::size_t{1});
    }

    RUVIA_CHECK_EQ(TrackedValue::alive(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(model_list_move_assignment_transfers_element_resource) {
    CountingMemoryResource sourceResource;
    CountingMemoryResource targetResource;
    {
        ruvia::BoxedArray<TrackedValue> source({.resource = &sourceResource});
        source.emplace(4);
        source.emplace(5);

        ruvia::BoxedArray<TrackedValue> target({.resource = &targetResource});
        target.emplace(9);
        RUVIA_CHECK_EQ(TrackedValue::alive(), std::size_t{3});

        target = std::move(source);
        RUVIA_CHECK_EQ(target.resource(), &sourceResource);
        RUVIA_CHECK_EQ(source.resource(), &sourceResource);
        RUVIA_CHECK_EQ(target.size(), std::size_t{2});
        RUVIA_CHECK_EQ(target[0].value(), 4);
        RUVIA_CHECK_EQ(target[1].value(), 5);
        RUVIA_CHECK(source.empty());
        RUVIA_CHECK_EQ(TrackedValue::alive(), std::size_t{2});
        RUVIA_CHECK_EQ(targetResource.liveAllocations(), std::size_t{0});

        source.emplace(6);
        RUVIA_CHECK_EQ(source.front().value(), 6);
        RUVIA_CHECK_EQ(TrackedValue::alive(), std::size_t{3});
    }

    RUVIA_CHECK_EQ(TrackedValue::alive(), std::size_t{0});
    RUVIA_CHECK_EQ(sourceResource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(targetResource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(sourceResource.allocationCount(), sourceResource.deallocationCount());
    RUVIA_CHECK_EQ(targetResource.allocationCount(), targetResource.deallocationCount());
}
