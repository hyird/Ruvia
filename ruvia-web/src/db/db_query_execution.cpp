#include "db/db_query_execution.h"

#include <array>
#include <stdexcept>

#include "ruvia/web/db/db_query.h"

#include "db/db_pool.h"
#include "db/db_pool_utils.h"
#ifdef RUVIA_ENABLE_REDIS
#include "db/db_query_cache.h"
#include "db/db_query_cache_state.h"
#endif

namespace ruvia::detail {

db_query_plan db_query_plan::prepare(const db_query& query, const db_query* count, const db_query_backend& backend) {
    const auto driver = db_pool_driver(backend.pool_);
    const auto prepare_step = [&](const db_query& input) {
        auto statement = input.compile(driver, backend.resource_);
        if (!statement.returns_rows()) {
            throw std::invalid_argument(count ? "query and count require statements that return rows"
                                              : "query requires a statement that returns rows");
        }
#ifdef RUVIA_ENABLE_REDIS
        std::optional<std::pmr::string> key;
        if (backend.cache_ != nullptr) {
            key = backend.cache_->key(input, statement, driver);
        }
        return db_query_step{std::move(statement.sql_), std::move(statement.params_), std::move(key),
            input.cache_duration()};
#else
        return db_query_step{std::move(statement.sql_), std::move(statement.params_)};
#endif
    };
    db_query_plan plan{prepare_step(query), std::nullopt};
    if (count != nullptr) {
        plan.second_.emplace(prepare_step(*count));
    }
    return plan;
}

task<db_rows> query_db_backend(const db_query_backend& backend, std::pmr::string sql,
    std::pmr::vector<db_value> params, operation_options& options) {
    return visit_db_pool(backend.pool_, [&](auto& pool) {
        if (backend.slot_.has_value()) {
            return pool.query_on_transaction_slot(
                *backend.slot_, std::move(sql), std::move(params), backend.resource_, options);
        }
        return pool.query(std::move(sql), std::move(params), backend.resource_, std::move(options), backend.scope_);
    });
}

// One deadline and one execution chain for direct, cached, and mixed sequences.
// A statement without a cache key goes straight to the pool.
template <bool with_count>
task<db_query_result<with_count>> execute_db_query_plan(
    db_query_plan plan, db_query_backend backend, operation_options options) {
    const operation_timeout deadline_value(options.timeout_);
    std::array<std::optional<db_rows>, with_count ? 2 : 1> results;
    for (std::size_t index = 0; index < results.size(); ++index) {
        auto& step = index == 0 ? plan.first_ : plan.second_.value();
        auto step_options = db_required_options(options, deadline_value);
#ifdef RUVIA_ENABLE_REDIS
        if (backend.cache_ != nullptr && step.cache_key_.has_value()) {
            // The cache runs only on pooled backends; a transaction never
            // carries one.
            results[index].emplace(co_await backend.cache_->wrap(step.cache_duration_,
                std::move(*step.cache_key_),
                db_cache_query(backend, std::move(step.sql_), std::move(step.params_)), std::move(step_options),
                deadline_value));
        } else
#endif
        {
            try {
                results[index].emplace(co_await query_db_backend(
                    backend, std::move(step.sql_), std::move(step.params_), step_options));
            } catch (...) {
                if (backend.backend_failed_ != nullptr) {
                    *backend.backend_failed_ = true;
                }
                throw;
            }
        }
        if (deadline_value.expired()) {
            throw_db_query_timeout();
        }
    }
    if constexpr (with_count) {
        co_return std::pair{std::move(*results[0]), std::move(*results[1])};
    } else {
        co_return std::move(*results[0]);
    }
}

template task<db_query_result<false>> execute_db_query_plan<false>(
    db_query_plan plan, db_query_backend backend, operation_options options);
template task<db_query_result<true>> execute_db_query_plan<true>(
    db_query_plan plan, db_query_backend backend, operation_options options);

}  // namespace ruvia::detail
