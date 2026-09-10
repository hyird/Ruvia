#include <coroutine>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>

#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/db/DbEntity.h"
#include "ruvia/web/db/DbRepository.h"
#include "ruvia/web/detail/db/DbMappedQuery.h"
#include "ruvia/web/detail/db/DbResultAccess.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {
using Entity = ruvia::DbEntity<ruvia::FixedString{"items"},
    ruvia::DbColumn<ruvia::FixedString{"id"}, int>,
    ruvia::DbColumn<ruvia::FixedString{"name"}, std::pmr::string>>;

ruvia::Task<ruvia::DbRows> rowsTask() {
    auto rows = ruvia::detail::DbResultAccess::makeResult(std::pmr::get_default_resource());
    auto& names = ruvia::detail::DbResultAccess::columnNames(rows);
    names.emplace_back(std::pmr::string("id", std::pmr::get_default_resource()));
    names.emplace_back(std::pmr::string("name", std::pmr::get_default_resource()));
    auto& fields = ruvia::detail::DbResultAccess::fields(rows);
    fields.push_back(ruvia::detail::DbResultAccess::ownedField("3", std::pmr::get_default_resource()));
    fields.push_back(ruvia::detail::DbResultAccess::ownedField("retained", std::pmr::get_default_resource()));
    auto& output = ruvia::detail::DbResultAccess::rows(rows);
    output.push_back(ruvia::detail::DbResultAccess::borrowedRow(fields.data(), fields.size(), names.data(), names.size(), std::pmr::get_default_resource()));
    co_return std::move(rows);
}

RUVIA_TEST(db_mapped_query_success_owns_result_after_source_task) {
    asio::io_context context;
    bool called = false;
    std::optional<ruvia::DbEntityRows<Entity>> result;
    ruvia::detail::asyncStartTask(
        ruvia::detail::mapDbQuery<ruvia::DbEntityRows<Entity>>(rowsTask(), std::pmr::get_default_resource(), ruvia::detail::DbMapEntityRows<Entity>{}),
        asio::bind_executor(context, [&](auto completion) {
            called = true;
            if (completion.failure()) {
                std::rethrow_exception(completion.failure()->exception());
            }
            result.emplace(std::move(*completion.success()).takeValue());
        }));
    context.run();
    RUVIA_CHECK(called);
    if (!result) {
        return;
    }
    RUVIA_CHECK_EQ((*result)[0].get<"id">(), 3);
}

struct QueryGate final {
    std::coroutine_handle<> continuation{};
    bool cancelled{false};
    bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> value) noexcept {
        continuation = value;
    }
    void await_resume() {
        if (cancelled) {
            throw ruvia::DbError(ruvia::DbError::Code::kCancelled, ruvia::DbDriver::kPostgreSql, "cancelled");
        }
    }
    void resume(bool cancel = false) {
        cancelled = cancel;
        auto saved = std::exchange(continuation, {});
        saved.resume();
    }
};

ruvia::Task<ruvia::DbRows> ownedRowsTask(std::pmr::string value, std::pmr::memory_resource* resource,
    bool invalid = false, QueryGate* gate = nullptr) {
    if (gate != nullptr) {
        co_await *gate;
    }
    auto rows = ruvia::detail::DbResultAccess::makeResult(resource);
    auto& names = ruvia::detail::DbResultAccess::columnNames(rows);
    names.emplace_back("id");
    names.emplace_back("name");
    auto& fields = ruvia::detail::DbResultAccess::fields(rows);
    fields.push_back(ruvia::detail::DbResultAccess::ownedField(invalid ? "invalid" : "3", resource));
    fields.push_back(ruvia::detail::DbResultAccess::ownedField(value, resource));
    ruvia::detail::DbResultAccess::rows(rows).push_back(ruvia::detail::DbResultAccess::borrowedRow(fields.data(), fields.size(), names.data(), names.size(), resource));
    co_return rows;
}

RUVIA_TEST(db_mapped_query_repeated_results_release_temporaries_and_retain_fields) {
    asio::io_context context;
    ruvia::test::CountingMemoryResource resource;
    const auto perform = [&] {
        context.restart();
        std::optional<ruvia::DbEntityRows<Entity>> result;
        std::exception_ptr failure;
        ruvia::detail::asyncStartTask(
            ruvia::detail::mapDbQuery<ruvia::DbEntityRows<Entity>>(
                ownedRowsTask(std::pmr::string(500, 'x', &resource), &resource), &resource, ruvia::detail::DbMapEntityRows<Entity>{}),
            asio::bind_executor(context, [&](auto completion) {
                if (completion.failure()) {
                    failure = completion.failure()->exception();
                } else {
                    result.emplace(std::move(*completion.success()).takeValue());
                }
            }));
        context.run();
        if (failure) {
            std::rethrow_exception(failure);
        }
        return result;
    };
    auto retained = perform();
    RUVIA_CHECK(retained.has_value());
    if (!retained) {
        return;
    }
    const auto baseline = resource.liveAllocations();
    const std::string expected(500, 'x');
    for (int i = 0; i < 20; ++i) {
        {
            auto next = perform();
            RUVIA_CHECK_EQ((*next)[0].get<"name">().size(), std::size_t{500});
        }
        RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
        RUVIA_CHECK_EQ((*retained)[0].get<"name">(), std::string_view(expected));
    }
    retained.reset();
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(db_mapped_query_cold_drop_conversion_failure_and_cancellation_release_storage) {
    asio::io_context context;
    ruvia::test::CountingMemoryResource resource;
    {
        auto cold = ruvia::detail::mapDbQuery<ruvia::DbEntityRows<Entity>>(
            ownedRowsTask(std::pmr::string(500, 'x', &resource), &resource), &resource, ruvia::detail::DbMapEntityRows<Entity>{});
        RUVIA_CHECK(resource.liveAllocations() > 0);
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    bool conversionFailure = false;
    ruvia::detail::asyncStartTask(
        ruvia::detail::mapDbQuery<ruvia::DbEntityRows<Entity>>(
            ownedRowsTask(std::pmr::string(500, 'x', &resource), &resource, true), &resource, ruvia::detail::DbMapEntityRows<Entity>{}),
        asio::bind_executor(context, [&](auto completion) {
            if (completion.failure()) {
                try {
                    std::rethrow_exception(completion.failure()->exception());
                } catch (const ruvia::DbConversionError&) {
                    conversionFailure = true;
                }
            }
        }));
    context.run();
    RUVIA_CHECK(conversionFailure);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    QueryGate gate;
    bool cancelled = false;
    context.restart();
    ruvia::detail::asyncStartTask(
        ruvia::detail::mapDbQuery<ruvia::DbEntityRows<Entity>>(
            ownedRowsTask(std::pmr::string(500, 'x', &resource), &resource, false, &gate), &resource, ruvia::detail::DbMapEntityRows<Entity>{}),
        asio::bind_executor(context, [&](auto completion) {
            if (completion.failure()) {
                try {
                    std::rethrow_exception(completion.failure()->exception());
                } catch (const ruvia::DbError& error) {
                    cancelled = error.code() == ruvia::DbError::Code::kCancelled;
                }
            }
        }));
    RUVIA_CHECK(gate.continuation != nullptr);
    RUVIA_CHECK(resource.liveAllocations() > 0);
    gate.resume(true);
    context.run();
    RUVIA_CHECK(cancelled);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(db_relation_mapped_query_cold_drop_and_cancellation_release_owned_plan) {
    using Target = ruvia::DbEntity<"targets", ruvia::DbColumn<"id", std::int64_t, ruvia::DbColumnOptions{.primaryKey = true}>>;
    using Source = ruvia::DbEntity<"sources", ruvia::DbColumn<"id", std::int64_t, ruvia::DbColumnOptions{.primaryKey = true}>,
        ruvia::DbManyToOne<"target", Target, ruvia::DbJoinColumn<"id", "id">>>;
    ruvia::test::CountingMemoryResource resource;
    const auto operation = [&](QueryGate* gate) {
        ruvia::DbQuery query(&resource);
        query.select(query.column("id", "s")).from("sources", "s");
        ruvia::detail::DbRelationPlan plan(&resource);
        plan.add<Source>(query, "s", "target");
        return ruvia::detail::mapDbQuery<ruvia::DbEntityRows<Source>>(
            ownedRowsTask(std::pmr::string(500, 'x', &resource), &resource, false, gate),
            &resource, ruvia::detail::DbMapRelatedEntities<Source>{std::move(plan)});
    };
    {
        auto cold = operation(nullptr);
        RUVIA_CHECK(resource.liveAllocations() > 0);
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    asio::io_context context;
    for (int i = 0; i < 12; ++i) {
        QueryGate gate;
        bool cancelled = false;
        context.restart();
        ruvia::detail::asyncStartTask(operation(&gate), asio::bind_executor(context, [&](auto completion) {
            if (completion.failure()) {
                try {
                    std::rethrow_exception(completion.failure()->exception());
                } catch (const ruvia::DbError& error) {
                    cancelled = error.code() == ruvia::DbError::Code::kCancelled;
                }
            }
        }));
        RUVIA_CHECK(gate.continuation != nullptr);
        RUVIA_CHECK(resource.liveAllocations() > 0);
        gate.resume(true);
        context.run();
        RUVIA_CHECK(cancelled);
        RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

}  // namespace
