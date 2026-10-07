#include <memory_resource>
#include <stdexcept>
#include <utility>

#include "ruvia/web/ModelTypes.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

struct owned_value final {
    enum class ownership { cloned,
        transferred };

    explicit owned_value(ruvia::ModelOptions options = {})
        : owned_value(0, options) {}
    explicit owned_value(int key, ruvia::ModelOptions options = {})
        : key_(key),
          resource_(options.resource) {
        if (key < 0) {
            throw std::runtime_error("invalid element");
        }
    }

    owned_value rebindForModel(std::pmr::memory_resource* resource) const& {
        return owned_value(key_, {.resource = resource});
    }
    owned_value rebindForModel(std::pmr::memory_resource* resource) & {
        // An lvalue must never select a mutable ownership transfer.
        return owned_value(std::exchange(key_, 0), {.resource = resource});
    }
    owned_value rebindForModel(std::pmr::memory_resource* resource) && {
        owned_value result(std::exchange(key_, 0), {.resource = resource});
        result.ownership_ = ownership::transferred;
        return result;
    }

    int key_{};
    std::pmr::memory_resource* resource_{};
    ownership ownership_{ownership::cloned};
};

}  // namespace

RUVIA_TEST(model_containers_clone_borrowed_values_and_transfer_owned_values) {
    ruvia::test::CountingMemoryResource memory;
    {
        ruvia::Array<owned_value> dense({.resource = &memory});
        ruvia::BoxedArray<owned_value> boxed({.resource = &memory});
        owned_value borrowed(7);
        const owned_value constant(9);
        dense.emplace_back(borrowed);
        boxed.push_back(borrowed);
        dense.push_back(std::move(constant));
        boxed.emplace(std::move(constant));
        RUVIA_CHECK(borrowed.key_ == 7 && constant.key_ == 9);
        RUVIA_CHECK(dense[0].key_ == 7 && boxed[0].key_ == 7);
        RUVIA_CHECK(dense[1].key_ == 9 && boxed[1].key_ == 9);
        RUVIA_CHECK(dense[0].ownership_ == owned_value::ownership::cloned);
        RUVIA_CHECK(boxed[0].ownership_ == owned_value::ownership::cloned);

        owned_value first(11);
        owned_value second(13);
        dense.emplace_back(std::move(first));
        boxed.push_back(std::move(second));
        RUVIA_CHECK(first.key_ == 0 && second.key_ == 0);
        RUVIA_CHECK(dense.back().ownership_ == owned_value::ownership::transferred);
        RUVIA_CHECK(boxed.back().ownership_ == owned_value::ownership::transferred);
        dense.emplace_back();
        boxed.emplace();
        dense.emplace_back(17);
        boxed.emplace(19);
        for (const auto& value : dense) {
            RUVIA_CHECK(value.resource_ == &memory);
        }
        for (const auto& value : boxed) {
            RUVIA_CHECK(value.resource_ == &memory);
        }
        bool dense_failed{};
        bool boxed_failed{};
        try {
            dense.emplace_back(-1);
        } catch (const std::runtime_error&) {
            dense_failed = true;
        }
        try {
            boxed.emplace(-1);
        } catch (const std::runtime_error&) {
            boxed_failed = true;
        }
        RUVIA_CHECK(dense_failed && boxed_failed);
        RUVIA_CHECK(dense.size() == 5 && boxed.size() == 5);
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
}
