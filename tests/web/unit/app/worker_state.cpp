#include "ruvia/web/detail/integration/worker_state.h"

#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::test::counting_memory_resource;

struct state_init final {
    std::vector<int>* destroyed_;
    int value_;
};

template <int tag>
struct tracked_state final {
    explicit tracked_state(state_init init)
        : destroyed_(init.destroyed_),
          value_(init.value_) {}

    ~tracked_state() {
        destroyed_->push_back(value_);
    }

    std::vector<int>* destroyed_;
    int value_;
};

struct missing_state final {};

}  // namespace

RUVIA_TEST(worker_state_registration_accepts_one_valid_definition_per_type) {
    std::vector<int> destroyed;
    std::vector<ruvia::detail::worker_state_definition> definitions;
    ruvia::detail::append_worker_state_definition(
        definitions, ruvia::detail::worker_state_definition::make<tracked_state<1>>(
                         [&] { return state_init{&destroyed, 1}; }));

    auto duplicate = ruvia::detail::worker_state_definition::make<tracked_state<1>>(
        [&] { return state_init{&destroyed, 2}; });
    bool rejected_duplicate = false;
    try {
        ruvia::detail::append_worker_state_definition(definitions, std::move(duplicate));
    } catch (const std::invalid_argument& error) {
        rejected_duplicate =
            std::string_view(error.what()) == "worker state type is already registered";
    }
    RUVIA_CHECK(rejected_duplicate);
    RUVIA_CHECK(duplicate.valid());

    RUVIA_CHECK_EQ(definitions.size(), std::size_t{1});
}

RUVIA_TEST(worker_state_registry_rejects_duplicate_types_before_factory_or_owner_allocation) {
    int factory_calls = 0;
    std::vector<int> destroyed;
    ruvia::detail::worker_state_definition definitions[] = {
        ruvia::detail::worker_state_definition::make<tracked_state<1>>([&] {
            ++factory_calls;
            return state_init{&destroyed, 1};
        }),
        ruvia::detail::worker_state_definition::make<tracked_state<1>>([&] {
            ++factory_calls;
            return state_init{&destroyed, 2};
        }),
    };
    counting_memory_resource resource;

    bool rejected = false;
    try {
        ruvia::detail::worker_state_registry registry(&resource, definitions);
        registry.initialize();
    } catch (const std::invalid_argument& error) {
        rejected = std::string_view(error.what()) == "worker state type is already registered";
    }

    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(factory_calls, 0);
    RUVIA_CHECK_EQ(resource.allocation_count(), std::size_t{0});
    RUVIA_CHECK(destroyed.empty());
}

RUVIA_TEST(worker_state_registry_indexes_types_and_destroys_in_reverse_registration_order) {
    int factory_calls = 0;
    std::vector<int> destroyed;
    destroyed.reserve(3);
    ruvia::detail::worker_state_definition definitions[] = {
        ruvia::detail::worker_state_definition::make<tracked_state<1>>([&] {
            ++factory_calls;
            return state_init{&destroyed, 1};
        }),
        ruvia::detail::worker_state_definition::make<tracked_state<2>>([&] {
            ++factory_calls;
            return state_init{&destroyed, 2};
        }),
        ruvia::detail::worker_state_definition::make<tracked_state<3>>([&] {
            ++factory_calls;
            return state_init{&destroyed, 3};
        }),
    };
    ruvia::detail::worker_state_registry registry(nullptr, definitions);

    registry.initialize();
    RUVIA_CHECK_EQ(factory_calls, 3);
    auto* first = static_cast<tracked_state<1>*>(
        registry.instance(ruvia::detail::worker_state_type_key<tracked_state<1>>()));
    auto* second = static_cast<tracked_state<2>*>(
        registry.instance(ruvia::detail::worker_state_type_key<tracked_state<2>>()));
    auto* third = static_cast<tracked_state<3>*>(
        registry.instance(ruvia::detail::worker_state_type_key<tracked_state<3>>()));
    RUVIA_CHECK(first != nullptr);
    RUVIA_CHECK(second != nullptr);
    RUVIA_CHECK(third != nullptr);
    RUVIA_CHECK_EQ(first->value_, 1);
    RUVIA_CHECK_EQ(second->value_, 2);
    RUVIA_CHECK_EQ(third->value_, 3);
    RUVIA_CHECK(registry.instance(ruvia::detail::worker_state_type_key<missing_state>()) == nullptr);

    registry.shutdown();
    RUVIA_CHECK(registry.instance(ruvia::detail::worker_state_type_key<tracked_state<1>>()) == nullptr);
    RUVIA_CHECK_EQ(destroyed.size(), std::size_t{3});
    RUVIA_CHECK_EQ(destroyed[0], 3);
    RUVIA_CHECK_EQ(destroyed[1], 2);
    RUVIA_CHECK_EQ(destroyed[2], 1);
}
