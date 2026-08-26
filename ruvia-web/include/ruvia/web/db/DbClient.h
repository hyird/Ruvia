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
    DbClient(EventLoop loop, DbConfig config);
    ~DbClient();

    DbClient(const DbClient&) = delete;
    DbClient& operator=(const DbClient&) = delete;
    DbClient(DbClient&&) = delete;
    DbClient& operator=(DbClient&&) = delete;

    // Lazy and worker-affine: start/await it on the bound event loop. Creating
    // more than one connect task is harmless; the first task that starts owns
    // the one allowed connection attempt and later starters fail.
    [[nodiscard]] Task<void> connect();

    [[nodiscard]] DbHandle withOptions(OperationOptions options) const;

    [[nodiscard]] ScopedOperation<DbRows> query(
        std::string_view sql, std::span<const DbValue> params = {}) const;
    [[nodiscard]] ScopedOperation<DbRows> query(
        std::string_view, std::initializer_list<DbValue>) const = delete;

    [[nodiscard]] ScopedOperation<DbExecResult> execute(
        std::string_view sql, std::span<const DbValue> params = {}) const;
    [[nodiscard]] ScopedOperation<DbExecResult> execute(
        std::string_view, std::initializer_list<DbValue>) const = delete;

    [[nodiscard]] ScopedOperation<DbStreamResult> queryStream(
        std::string_view sql, std::span<const DbValue> params = {}) const;
    [[nodiscard]] ScopedOperation<DbStreamResult> queryStream(
        std::string_view, std::initializer_list<DbValue>) const = delete;

    template <typename... Params>
        requires detail::DbParameterPack<Params...>
    [[nodiscard]] ScopedOperation<DbRows> query(std::string_view sql, Params&&... params) const {
        return withOptions({}).query(sql, std::forward<Params>(params)...);
    }

    template <typename... Params>
        requires detail::DbParameterPack<Params...>
    [[nodiscard]] ScopedOperation<DbExecResult> execute(
        std::string_view sql, Params&&... params) const {
        return withOptions({}).execute(sql, std::forward<Params>(params)...);
    }

    template <typename... Params>
        requires detail::DbParameterPack<Params...>
    [[nodiscard]] ScopedOperation<DbStreamResult> queryStream(
        std::string_view sql, Params&&... params) const {
        return withOptions({}).queryStream(sql, std::forward<Params>(params)...);
    }

    [[nodiscard]] ScopedOperation<DbTransaction> beginTransaction() const;

    // Idempotent and callable from any thread. Pool teardown runs on the bound
    // event loop; finish all operations before destroying the client.
    void close() noexcept;

    [[nodiscard]] const WorkerHandle& worker() const& noexcept;
    const WorkerHandle& worker() const&& = delete;

private:
    std::shared_ptr<detail::DbClientState> state_;
};

}  // namespace ruvia
