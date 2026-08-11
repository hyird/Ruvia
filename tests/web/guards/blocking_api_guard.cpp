#include <chrono>
#include <concepts>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/WebWorker.h"

// Blocking offload: the pool is referred to, never copied; a result is consumed
// once, from an rvalue; and the offload spellings keep their return shapes.
namespace {

template <typename T>
concept HasLvalueBlockingValue = requires(ruvia::BlockingResult<T>& result) { result.value(); };

template <typename T>
concept HasRvalueBlockingError = requires(T&& result) { std::move(result).error(); };

using BlockingIntTask = ruvia::Task<ruvia::BlockingResult<int>>;
using BlockingVoidTask = ruvia::Task<ruvia::BlockingResult<void>>;

}  // namespace

static_assert(!std::is_copy_constructible_v<ruvia::BlockingPool>);
static_assert(!std::is_copy_assignable_v<ruvia::BlockingPool>);
static_assert(!std::is_move_constructible_v<ruvia::BlockingPool>);
static_assert(!std::is_move_assignable_v<ruvia::BlockingPool>);
static_assert(!std::is_copy_constructible_v<ruvia::BlockingResult<int>>);
static_assert(!std::is_copy_assignable_v<ruvia::BlockingResult<int>>);
static_assert(std::is_move_constructible_v<ruvia::BlockingResult<int>>);
static_assert(!std::is_constructible_v<ruvia::BlockingResult<int>, ruvia::BlockingStatus>);
static_assert(!std::is_constructible_v<ruvia::BlockingOperationRejected, ruvia::BlockingStatus>);
static_assert(!HasLvalueBlockingValue<int>);
static_assert(!HasLvalueBlockingValue<void>);
static_assert(!HasRvalueBlockingError<ruvia::BlockingResult<int>>);
static_assert(std::is_base_of_v<std::runtime_error, ruvia::BlockingOperationRejected>);
static_assert(std::same_as<decltype(ruvia::runBlocking(std::declval<ruvia::BlockingPool&>(), std::declval<ruvia::WorkerHandle>(), std::declval<ruvia::StopToken>(), std::declval<int (*)()>())), BlockingIntTask>);
static_assert(std::same_as<decltype(ruvia::runBlocking(std::declval<ruvia::BlockingPool&>(), std::declval<ruvia::WorkerHandle>(), std::chrono::seconds(1), std::declval<ruvia::StopToken>(), std::declval<void (*)()>())), BlockingVoidTask>);
static_assert(std::same_as<decltype(std::declval<const ruvia::Context&>().runBlocking(std::declval<int (*)()>())), ruvia::Task<int>>);
static_assert(std::same_as<decltype(std::declval<const ruvia::Context&>().runBlocking(std::declval<void (*)()>())), ruvia::Task<void>>);
static_assert(std::same_as<decltype(std::declval<const ruvia::Context&>().tryRunBlocking(std::declval<int (*)()>())), BlockingIntTask>);
static_assert(std::same_as<decltype(std::declval<const ruvia::Context&>().tryRunBlocking(std::chrono::seconds(1), std::declval<void (*)()>())), BlockingVoidTask>);
static_assert(std::same_as<decltype(std::declval<const ruvia::WebWorkerContext&>().tryRunBlocking(std::declval<int (*)()>())), BlockingIntTask>);

int main() {
    return 0;
}
