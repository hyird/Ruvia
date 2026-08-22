#include <array>
#include <concepts>
#include <cstddef>
#include <thread>
#include <type_traits>

#include <ruvia/web/App.h>

static_assert(std::same_as<decltype(ruvia::app()), ruvia::App&>);
static_assert(!std::default_initializable<ruvia::App>);
static_assert(!std::destructible<ruvia::App>);
static_assert(!std::copy_constructible<ruvia::App>);
static_assert(!std::move_constructible<ruvia::App>);
static_assert(!std::is_copy_assignable_v<ruvia::App>);
static_assert(!std::is_move_assignable_v<ruvia::App>);

int main() {
    constexpr std::size_t threadCount = 8;
    std::array<ruvia::App*, threadCount> instances{};
    std::array<std::thread, threadCount> threads;

    for (std::size_t index = 0; index < threadCount; ++index) {
        threads[index] = std::thread([&, index] { instances[index] = &ruvia::app(); });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (const auto* instance : instances) {
        if (instance != instances.front()) {
            return 1;
        }
    }
    return 0;
}
