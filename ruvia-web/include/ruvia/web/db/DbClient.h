#pragma once

#include <concepts>
#include <initializer_list>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/EventLoop.h"
#include "ruvia/core/Task.h"
#include "ruvia/web/db/DbHandle.h"
#include "ruvia/web/db/DbTypes.h"

namespace ruvia {

namespace detail {
class DbClientState;
}

// One database pool bound to one Ruvia event loop. It is the standalone
// counterpart of Context::db(): application-created and attached workers use
// the same worker-local backend without an HTTP App or a special context.
class DbClient final {
public:
    DbClient(EventLoop loop, const DbConfig& config);
    ~DbClient();

    DbClient(const DbClient&) = delete;
    DbClient& operator=(const DbClient&) = delete;
    DbClient(DbClient&&) = delete;
    DbClient& operator=(DbClient&&) = delete;

    // Lazy and worker-affine: start/await it on the bound event loop. Creating
    // more than one connect task is harmless; the first task that starts owns
    // the one allowed connection attempt and later starters fail.
    [[nodiscard]] Task<void> connect() &;
    Task<void> connect() && = delete;

    // Handles and operations borrow this client's open lifecycle. A temporary
    // client would begin pool shutdown at the end of the full expression.
    [[nodiscard]] DbHandle withOptions(OperationOptions options) const&;
    DbHandle withOptions(OperationOptions) const&& = delete;

    [[nodiscard]] ScopedOperation<DbRows> query(std::string_view sql, std::span<const DbValue> params = {}) const&;
    ScopedOperation<DbRows> query(std::string_view, std::span<const DbValue> = {}) const&& = delete;
    [[nodiscard]] ScopedOperation<DbRows> query(std::string_view, std::initializer_list<DbValue>) const& = delete;
    ScopedOperation<DbRows> query(std::string_view, std::initializer_list<DbValue>) const&& = delete;

    [[nodiscard]] ScopedOperation<DbExecResult> execute(std::string_view sql, std::span<const DbValue> params = {}) const&;
    ScopedOperation<DbExecResult> execute(std::string_view, std::span<const DbValue> = {}) const&& = delete;
    [[nodiscard]] ScopedOperation<DbExecResult> execute(std::string_view, std::initializer_list<DbValue>) const& = delete;
    ScopedOperation<DbExecResult> execute(std::string_view, std::initializer_list<DbValue>) const&& = delete;

    [[nodiscard]] ScopedOperation<DbStreamResult> queryStream(std::string_view sql, std::span<const DbValue> params = {}) const&;
    ScopedOperation<DbStreamResult> queryStream(std::string_view, std::span<const DbValue> = {}) const&& = delete;
    [[nodiscard]] ScopedOperation<DbStreamResult> queryStream(std::string_view, std::initializer_list<DbValue>) const& = delete;
    ScopedOperation<DbStreamResult> queryStream(std::string_view, std::initializer_list<DbValue>) const&& = delete;

    template <typename... Params>
        requires detail::DbParameterPack<Params...>
    [[nodiscard]] ScopedOperation<DbRows> query(std::string_view sql, Params&&... params) const& {
        return withOptions({}).query(sql, std::forward<Params>(params)...);
    }
    template <typename... Params>
        requires detail::DbParameterPack<Params...>
    ScopedOperation<DbRows> query(std::string_view, Params&&...) const&& = delete;

    template <typename... Params>
        requires detail::DbParameterPack<Params...>
    [[nodiscard]] ScopedOperation<DbExecResult> execute(std::string_view sql, Params&&... params) const& {
        return withOptions({}).execute(sql, std::forward<Params>(params)...);
    }
    template <typename... Params>
        requires detail::DbParameterPack<Params...>
    ScopedOperation<DbExecResult> execute(std::string_view, Params&&...) const&& = delete;

    template <typename... Params>
        requires detail::DbParameterPack<Params...>
    [[nodiscard]] ScopedOperation<DbStreamResult> queryStream(std::string_view sql, Params&&... params) const& {
        return withOptions({}).queryStream(sql, std::forward<Params>(params)...);
    }
    template <typename... Params>
        requires detail::DbParameterPack<Params...>
    ScopedOperation<DbStreamResult> queryStream(std::string_view, Params&&...) const&& = delete;

    [[nodiscard]] ScopedOperation<DbTransaction> beginTransaction() const&;
    ScopedOperation<DbTransaction> beginTransaction() const&& = delete;

    // Idempotent and callable from any thread. Pool teardown runs on the bound
    // event loop; use shutdown() when its completion must be awaited.
    void close() noexcept;
    // Requests cancellation, joins worker-owned operations, and completes on
    // the bound event loop after the client teardown is finished.
    [[nodiscard]] Task<void> shutdown() &;
    Task<void> shutdown() && = delete;

    [[nodiscard]] const WorkerHandle& worker() const& noexcept;
    const WorkerHandle& worker() const&& = delete;

private:
    std::shared_ptr<detail::DbClientState> state_;
};

}  // namespace ruvia
