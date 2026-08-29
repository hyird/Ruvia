#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/web/detail/integration/WorkerState.h"

namespace {

struct StateInit final {
    std::vector<int>* destroyed;
    int value;
};

template <int Tag>
struct TrackedState final {
    explicit TrackedState(StateInit init)
        : destroyed(init.destroyed),
          value(init.value) {}

    ~TrackedState() {
        destroyed->push_back(value);
    }

    std::vector<int>* destroyed;
    int value;
};

struct MissingState final {};

class CountingMemoryResource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t allocationCount() const noexcept {
        return allocationCount_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocationCount_;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t allocationCount_{0};
};

}  // namespace

RUVIA_TEST(worker_state_registration_accepts_one_valid_definition_per_type) {
    std::vector<int> destroyed;
    std::vector<ruvia::detail::WorkerStateDefinition> definitions;
    ruvia::detail::appendWorkerStateDefinition(
        definitions, ruvia::detail::WorkerStateDefinition::make<TrackedState<1>>(
                         [&] { return StateInit{&destroyed, 1}; }));

    auto duplicate = ruvia::detail::WorkerStateDefinition::make<TrackedState<1>>(
        [&] { return StateInit{&destroyed, 2}; });
    bool rejectedDuplicate = false;
    try {
        ruvia::detail::appendWorkerStateDefinition(definitions, std::move(duplicate));
    } catch (const std::invalid_argument& error) {
        rejectedDuplicate =
            std::string_view(error.what()) == "worker state type is already registered";
    }
    RUVIA_CHECK(rejectedDuplicate);
    RUVIA_CHECK(duplicate.valid());

    RUVIA_CHECK_EQ(definitions.size(), std::size_t{1});
}

RUVIA_TEST(worker_state_registry_rejects_duplicate_types_before_factory_or_owner_allocation) {
    int factoryCalls = 0;
    std::vector<int> destroyed;
    ruvia::detail::WorkerStateDefinition definitions[] = {
        ruvia::detail::WorkerStateDefinition::make<TrackedState<1>>([&] {
            ++factoryCalls;
            return StateInit{&destroyed, 1};
        }),
        ruvia::detail::WorkerStateDefinition::make<TrackedState<1>>([&] {
            ++factoryCalls;
            return StateInit{&destroyed, 2};
        }),
    };
    CountingMemoryResource resource;

    bool rejected = false;
    try {
        ruvia::detail::WorkerStateRegistry registry(&resource, definitions);
        registry.initialize();
    } catch (const std::invalid_argument& error) {
        rejected = std::string_view(error.what()) == "worker state type is already registered";
    }

    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(factoryCalls, 0);
    RUVIA_CHECK_EQ(resource.allocationCount(), std::size_t{0});
    RUVIA_CHECK(destroyed.empty());
}

RUVIA_TEST(worker_state_registry_indexes_types_and_destroys_in_reverse_registration_order) {
    int factoryCalls = 0;
    std::vector<int> destroyed;
    destroyed.reserve(3);
    ruvia::detail::WorkerStateDefinition definitions[] = {
        ruvia::detail::WorkerStateDefinition::make<TrackedState<1>>([&] {
            ++factoryCalls;
            return StateInit{&destroyed, 1};
        }),
        ruvia::detail::WorkerStateDefinition::make<TrackedState<2>>([&] {
            ++factoryCalls;
            return StateInit{&destroyed, 2};
        }),
        ruvia::detail::WorkerStateDefinition::make<TrackedState<3>>([&] {
            ++factoryCalls;
            return StateInit{&destroyed, 3};
        }),
    };
    ruvia::detail::WorkerStateRegistry registry(nullptr, definitions);

    registry.initialize();
    RUVIA_CHECK_EQ(factoryCalls, 3);
    auto* first = static_cast<TrackedState<1>*>(
        registry.instance(ruvia::detail::workerStateTypeKey<TrackedState<1>>()));
    auto* second = static_cast<TrackedState<2>*>(
        registry.instance(ruvia::detail::workerStateTypeKey<TrackedState<2>>()));
    auto* third = static_cast<TrackedState<3>*>(
        registry.instance(ruvia::detail::workerStateTypeKey<TrackedState<3>>()));
    RUVIA_CHECK(first != nullptr);
    RUVIA_CHECK(second != nullptr);
    RUVIA_CHECK(third != nullptr);
    RUVIA_CHECK_EQ(first->value, 1);
    RUVIA_CHECK_EQ(second->value, 2);
    RUVIA_CHECK_EQ(third->value, 3);
    RUVIA_CHECK(registry.instance(ruvia::detail::workerStateTypeKey<MissingState>()) == nullptr);

    registry.shutdown();
    RUVIA_CHECK(registry.instance(ruvia::detail::workerStateTypeKey<TrackedState<1>>()) == nullptr);
    RUVIA_CHECK_EQ(destroyed.size(), std::size_t{3});
    RUVIA_CHECK_EQ(destroyed[0], 3);
    RUVIA_CHECK_EQ(destroyed[1], 2);
    RUVIA_CHECK_EQ(destroyed[2], 1);
}
