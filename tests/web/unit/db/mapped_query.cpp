#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <utility>

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

RUVIA_TEST(db_mapped_result_mappers_cover_empty_rows_and_shape_errors) {
    ruvia::detail::DbMapOneEntity<Entity> one;
    auto empty = ruvia::detail::DbResultAccess::makeResult(std::pmr::get_default_resource());
    const auto noEntity = one(std::move(empty), std::pmr::get_default_resource());
    RUVIA_CHECK(!noEntity.has_value());

    auto rows = ruvia::detail::DbResultAccess::makeResult(std::pmr::get_default_resource());
    auto& names = ruvia::detail::DbResultAccess::columnNames(rows);
    names.emplace_back("id");
    names.emplace_back("name");
    auto& fields = ruvia::detail::DbResultAccess::fields(rows);
    fields.push_back(ruvia::detail::DbResultAccess::ownedField("3", std::pmr::get_default_resource()));
    fields.push_back(ruvia::detail::DbResultAccess::ownedField("first", std::pmr::get_default_resource()));
    auto& resultRows = ruvia::detail::DbResultAccess::rows(rows);
    resultRows.push_back(ruvia::detail::DbResultAccess::borrowedRow(
        fields.data(), fields.size(), names.data(), names.size(), std::pmr::get_default_resource()));
    const auto entity = one(std::move(rows), std::pmr::get_default_resource());
    RUVIA_CHECK(entity.has_value());
    RUVIA_CHECK_EQ(entity->get<"id">(), 3);
    RUVIA_CHECK_EQ(entity->get<"name">(), std::string_view("first"));

    auto counts = ruvia::detail::DbResultAccess::makeResult(std::pmr::get_default_resource());
    auto& countNames = ruvia::detail::DbResultAccess::columnNames(counts);
    countNames.emplace_back("count");
    auto& countFields = ruvia::detail::DbResultAccess::fields(counts);
    countFields.push_back(ruvia::detail::DbResultAccess::ownedField("7", std::pmr::get_default_resource()));
    ruvia::detail::DbResultAccess::rows(counts).push_back(ruvia::detail::DbResultAccess::borrowedRow(
        countFields.data(), 1, countNames.data(), countNames.size(), std::pmr::get_default_resource()));
    RUVIA_CHECK_EQ(ruvia::detail::dbCountValue(counts), std::uint64_t{7});
    auto emptyCount = ruvia::detail::DbResultAccess::makeResult(std::pmr::get_default_resource());
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)ruvia::detail::dbCountValue(emptyCount); }));
    auto missingCount = ruvia::detail::DbResultAccess::makeResult(std::pmr::get_default_resource());
    auto& missingCountNames = ruvia::detail::DbResultAccess::columnNames(missingCount);
    missingCountNames.emplace_back("other");
    auto& missingCountFields = ruvia::detail::DbResultAccess::fields(missingCount);
    missingCountFields.push_back(ruvia::detail::DbResultAccess::ownedField("7", std::pmr::get_default_resource()));
    ruvia::detail::DbResultAccess::rows(missingCount).push_back(ruvia::detail::DbResultAccess::borrowedRow(missingCountFields.data(), 1, missingCountNames.data(), missingCountNames.size(), std::pmr::get_default_resource()));
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)ruvia::detail::dbCountValue(missingCount); }));
    auto nullCount = ruvia::detail::DbResultAccess::makeResult(std::pmr::get_default_resource());
    auto& nullCountNames = ruvia::detail::DbResultAccess::columnNames(nullCount);
    nullCountNames.emplace_back("count");
    auto& nullCountFields = ruvia::detail::DbResultAccess::fields(nullCount);
    nullCountFields.push_back(ruvia::detail::DbResultAccess::nullField(std::pmr::get_default_resource()));
    ruvia::detail::DbResultAccess::rows(nullCount).push_back(ruvia::detail::DbResultAccess::borrowedRow(
        nullCountFields.data(), 1, nullCountNames.data(), nullCountNames.size(), std::pmr::get_default_resource()));
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)ruvia::detail::dbCountValue(nullCount); }));

    auto existsRows = ruvia::detail::DbResultAccess::makeResult(std::pmr::get_default_resource());
    auto& existsNames = ruvia::detail::DbResultAccess::columnNames(existsRows);
    existsNames.emplace_back("exists");
    auto& existsFields = ruvia::detail::DbResultAccess::fields(existsRows);
    existsFields.push_back(ruvia::detail::DbResultAccess::ownedField("true", std::pmr::get_default_resource()));
    ruvia::detail::DbResultAccess::rows(existsRows).push_back(ruvia::detail::DbResultAccess::borrowedRow(existsFields.data(), 1, existsNames.data(), existsNames.size(), std::pmr::get_default_resource()));
    ruvia::detail::DbMapExists exists;
    RUVIA_CHECK(exists(std::move(existsRows), std::pmr::get_default_resource()));

    auto badExists = ruvia::detail::DbResultAccess::makeResult(std::pmr::get_default_resource());
    auto& badNames = ruvia::detail::DbResultAccess::columnNames(badExists);
    badNames.emplace_back("exists");
    auto& badFields = ruvia::detail::DbResultAccess::fields(badExists);
    badFields.push_back(ruvia::detail::DbResultAccess::ownedField("not-bool", std::pmr::get_default_resource()));
    ruvia::detail::DbResultAccess::rows(badExists).push_back(ruvia::detail::DbResultAccess::borrowedRow(
        badFields.data(), 1, badNames.data(), badNames.size(), std::pmr::get_default_resource()));
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)exists(std::move(badExists), std::pmr::get_default_resource()); }));

    auto wrongExists = ruvia::detail::DbResultAccess::makeResult(std::pmr::get_default_resource());
    auto& wrongNames = ruvia::detail::DbResultAccess::columnNames(wrongExists);
    wrongNames.emplace_back("exists");
    auto& wrongFields = ruvia::detail::DbResultAccess::fields(wrongExists);
    wrongFields.push_back(ruvia::detail::DbResultAccess::ownedField("true", std::pmr::get_default_resource()));
    wrongFields.push_back(ruvia::detail::DbResultAccess::ownedField("false", std::pmr::get_default_resource()));
    auto& wrongRows = ruvia::detail::DbResultAccess::rows(wrongExists);
    wrongRows.push_back(ruvia::detail::DbResultAccess::borrowedRow(
        wrongFields.data(), 1, wrongNames.data(), wrongNames.size(), std::pmr::get_default_resource()));
    wrongRows.push_back(ruvia::detail::DbResultAccess::borrowedRow(
        wrongFields.data() + 1, 1, wrongNames.data(), wrongNames.size(), std::pmr::get_default_resource()));
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)exists(std::move(wrongExists), std::pmr::get_default_resource()); }));

    auto missingExists = ruvia::detail::DbResultAccess::makeResult(std::pmr::get_default_resource());
    auto& missingFields = ruvia::detail::DbResultAccess::fields(missingExists);
    missingFields.push_back(ruvia::detail::DbResultAccess::ownedField("true", std::pmr::get_default_resource()));
    auto& missingNames = ruvia::detail::DbResultAccess::columnNames(missingExists);
    missingNames.emplace_back("other");
    ruvia::detail::DbResultAccess::rows(missingExists).push_back(ruvia::detail::DbResultAccess::borrowedRow(missingFields.data(), 1, missingNames.data(), missingNames.size(), std::pmr::get_default_resource()));
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { (void)exists(std::move(missingExists), std::pmr::get_default_resource()); }));
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

ruvia::Task<ruvia::DbRows> countRowsTask(std::pmr::memory_resource* resource,
    QueryGate* gate = nullptr, bool invalid = false, bool* started = nullptr) {
    if (started != nullptr) {
        *started = true;
    }
    if (gate != nullptr) {
        co_await *gate;
    }
    auto rows = ruvia::detail::DbResultAccess::makeResult(resource);
    auto& names = ruvia::detail::DbResultAccess::columnNames(rows);
    names.emplace_back("count");
    auto& fields = ruvia::detail::DbResultAccess::fields(rows);
    fields.push_back(ruvia::detail::DbResultAccess::ownedField(invalid ? "invalid" : "17", resource));
    ruvia::detail::DbResultAccess::rows(rows).push_back(ruvia::detail::DbResultAccess::borrowedRow(fields.data(), fields.size(), names.data(), names.size(), resource));
    co_return rows;
}

RUVIA_TEST(db_mapped_page_repeated_operations_release_temporaries_and_retain_results) {
    using Page = std::pair<ruvia::DbEntityRows<Entity>, std::uint64_t>;
    asio::io_context context;
    ruvia::test::CountingMemoryResource resource;
    const auto perform = [&] {
        context.restart();
        std::optional<Page> result;
        std::exception_ptr failure;
        auto query = ruvia::detail::queryDbPair(ownedRowsTask(std::pmr::string(500, 'p', &resource), &resource), countRowsTask(&resource));
        ruvia::detail::asyncStartTask(ruvia::detail::mapDbQueryAndCount<ruvia::DbEntityRows<Entity>>(
                                          std::move(query), &resource, ruvia::detail::DbMapEntityRows<Entity>{}),
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
    for (int i = 0; i < 12; ++i) {
        {
            auto next = perform();
            RUVIA_CHECK_EQ(next->second, std::uint64_t{17});
            RUVIA_CHECK_EQ(next->first.size(), std::size_t{1});
        }
        RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
        const std::string expected(500, 'p');
        RUVIA_CHECK_EQ(std::string_view(retained->first[0].get<"name">()), std::string_view(expected));
    }
    retained.reset();
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

RUVIA_TEST(db_mapped_page_cold_drop_failures_and_cancellation_release_both_queries) {
    asio::io_context context;
    ruvia::test::CountingMemoryResource resource;
    const auto operation = [&](QueryGate* first, QueryGate* second, bool invalidRow, bool invalidCount, bool* countStarted) {
        return ruvia::detail::mapDbQueryAndCount<ruvia::DbEntityRows<Entity>>(
            ruvia::detail::queryDbPair(ownedRowsTask(std::pmr::string(500, 'p', &resource), &resource, invalidRow, first),
                countRowsTask(&resource, second, invalidCount, countStarted)),
            &resource, ruvia::detail::DbMapEntityRows<Entity>{});
    };
    bool countStarted = false;
    {
        auto cold = operation(nullptr, nullptr, false, false, &countStarted);
        RUVIA_CHECK(resource.liveAllocations() > 0);
    }
    RUVIA_CHECK(!countStarted);
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    for (int scenario = 0; scenario < 4; ++scenario) {
        context.restart();
        QueryGate gate;
        countStarted = false;
        bool failed = false;
        bool cancelled = false;
        ruvia::detail::asyncStartTask(operation(scenario == 0 ? &gate : nullptr,
                                          scenario == 1 ? &gate : nullptr, scenario == 2, scenario == 3, &countStarted),
            asio::bind_executor(context, [&](auto completion) {
                failed = completion.failure() != nullptr;
                if (completion.failure()) {
                    try {
                        std::rethrow_exception(completion.failure()->exception());
                    } catch (const ruvia::DbError& error) {
                        cancelled = error.code() == ruvia::DbError::Code::kCancelled;
                    } catch (const ruvia::DbConversionError&) {
                    }
                }
            }));
        if (scenario < 2) {
            RUVIA_CHECK(gate.continuation != nullptr);
            RUVIA_CHECK_EQ(countStarted, scenario == 1);
            gate.resume(true);
        }
        context.run();
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(cancelled, scenario < 2);
        RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}

}  // namespace
