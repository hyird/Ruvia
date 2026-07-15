#include <ruvia/core/Task.h>
#include <ruvia/core/detail/AsioAwait.h>

#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>

#include <concepts>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

template <typename Result>
concept HasLooseCompletionFields = requires(Result& result) {
    result.exception;
    result.value;
};

template <typename Result>
concept HasRvalueCompletionBorrow =
    requires(Result&& result) { std::move(result).success(); } ||
    requires(Result&& result) { std::move(result).failure(); };

static_assert(!std::default_initializable<
              ruvia::detail::TaskCompletionResult<int>>);
static_assert(!std::default_initializable<
              ruvia::detail::TaskCompletionResult<void>>);
static_assert(!HasLooseCompletionFields<
              ruvia::detail::TaskCompletionResult<int>>);
static_assert(!HasRvalueCompletionBorrow<
              ruvia::detail::TaskCompletionResult<int>>);
static_assert(!HasRvalueCompletionBorrow<
              ruvia::detail::TaskCompletionResult<void>>);

ruvia::Task<std::unique_ptr<int>> makeValue() {
    co_return std::make_unique<int>(42);
}

ruvia::Task<int> failValue() {
    throw std::runtime_error("value failure");
    co_return 0;
}

ruvia::Task<void> completeVoid() {
    co_return;
}

ruvia::Task<void> failVoid() {
    throw std::runtime_error("void failure");
    co_return;
}

bool hasFailureMessage(
    const ruvia::detail::TaskCompletionFailure& failure,
    std::string_view expected) {
    try {
        std::rethrow_exception(failure.exception());
    } catch (const std::runtime_error& error) {
        return error.what() == expected;
    }
    return false;
}

}  // namespace

int main() {
    asio::io_context ioContext(1);
    int completed = 0;
    bool valid = true;

    ruvia::detail::asyncStartTask(
        makeValue(),
        asio::bind_executor(
            ioContext.get_executor(),
            [&completed, &valid](auto result) {
                auto* success = result.success();
                valid = valid && success != nullptr &&
                        result.failure() == nullptr;
                if (success != nullptr) {
                    auto value = std::move(*success).takeValue();
                    valid = valid && value != nullptr && *value == 42;
                }
                ++completed;
            }));
    ruvia::detail::asyncStartTask(
        failValue(),
        asio::bind_executor(
            ioContext.get_executor(),
            [&completed, &valid](auto result) {
                const auto* failure = result.failure();
                valid = valid && result.success() == nullptr &&
                        failure != nullptr;
                if (failure != nullptr) {
                    valid = valid &&
                            hasFailureMessage(*failure, "value failure");
                }
                ++completed;
            }));
    ruvia::detail::asyncStartTask(
        completeVoid(),
        asio::bind_executor(
            ioContext.get_executor(),
            [&completed, &valid](auto result) {
                valid = valid && result.success() != nullptr &&
                        result.failure() == nullptr;
                ++completed;
            }));
    ruvia::detail::asyncStartTask(
        failVoid(),
        asio::bind_executor(
            ioContext.get_executor(),
            [&completed, &valid](auto result) {
                const auto* failure = result.failure();
                valid = valid && result.success() == nullptr &&
                        failure != nullptr;
                if (failure != nullptr) {
                    valid = valid &&
                            hasFailureMessage(*failure, "void failure");
                }
                ++completed;
            }));

    ioContext.run();
    return valid && completed == 4 ? 0 : 1;
}
