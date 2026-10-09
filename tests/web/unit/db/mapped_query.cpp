#include <array>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/web/db/db_entity.h"
#include "ruvia/web/db/db_repository.h"
#include "ruvia/web/detail/db/db_mapped_query.h"
#include "ruvia/web/detail/db/db_result_access.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {
template <typename t_type, typename callback>
asio::awaitable<void> await_task(ruvia::task<t_type> task_value, callback callback_value) {
    std::optional<t_type> result;
    std::exception_ptr failure;
    try {
        result.emplace(co_await ruvia::as_awaitable(std::move(task_value)));
    } catch (...) {
        failure = std::current_exception();
    }
    callback_value(std::move(failure), std::move(result));
    co_return;
}

template <typename t_type, typename callback>
void start_task(asio::io_context& context_value, ruvia::task<t_type> task_value, callback callback_value) {
    asio::co_spawn(context_value, await_task(std::move(task_value), std::move(callback_value)), asio::detached);
    context_value.poll();
    context_value.restart();
}

using entity_type = ruvia::db_entity<ruvia::fixed_string{"items"},
    ruvia::db_column<ruvia::fixed_string{"id"}, int>,
    ruvia::db_column<ruvia::fixed_string{"name"}, std::pmr::string>>;

RUVIA_DB_PROJECTION(numeric_projection, ruvia::db_column<"id", int>)
RUVIA_DB_PROJECTION(named_projection, ruvia::db_column<"id", int>, ruvia::db_column<"name", std::pmr::string>)

ruvia::task<ruvia::db_rows> rows_task() {
    auto rows = ruvia::detail::db_result_access::make_result(std::pmr::get_default_resource());
    auto& names = ruvia::detail::db_result_access::column_names(rows);
    names.emplace_back(std::pmr::string("id", std::pmr::get_default_resource()));
    names.emplace_back(std::pmr::string("name", std::pmr::get_default_resource()));
    auto& fields_value = ruvia::detail::db_result_access::fields(rows);
    fields_value.push_back(ruvia::detail::db_result_access::owned_field("3", std::pmr::get_default_resource()));
    fields_value.push_back(ruvia::detail::db_result_access::owned_field("retained", std::pmr::get_default_resource()));
    auto& output = ruvia::detail::db_result_access::rows(rows);
    output.push_back(ruvia::detail::db_result_access::borrowed_row(fields_value.data(), fields_value.size(), names.data(), names.size(), std::pmr::get_default_resource()));
    co_return std::move(rows);
}

RUVIA_TEST(db_mapped_query_success_owns_result_after_source_task) {
    asio::io_context context;
    bool called = false;
    std::optional<ruvia::entity_rows<entity_type>> result;
    start_task(context,
        ruvia::detail::map_db_query<ruvia::entity_rows<entity_type>>(rows_task(), std::pmr::get_default_resource(), ruvia::detail::db_map_entity_rows<entity_type>{}),
        [&](std::exception_ptr failure, auto value) {
            called = true;
            if (failure) {
                std::rethrow_exception(failure);
            }
            result.emplace(std::move(*value));
        });
    context.run();
    RUVIA_CHECK(called);
    if (!result) {
        return;
    }
    RUVIA_CHECK_EQ((*result)[0].get<"id">(), 3);
}

RUVIA_TEST(db_mapped_result_mappers_cover_empty_rows_and_shape_errors) {
    ruvia::detail::db_map_one_entity<entity_type> one;
    auto empty = ruvia::detail::db_result_access::make_result(std::pmr::get_default_resource());
    const auto no_entity = one(std::move(empty), std::pmr::get_default_resource());
    RUVIA_CHECK(!no_entity.has_value());

    auto rows = ruvia::detail::db_result_access::make_result(std::pmr::get_default_resource());
    auto& names = ruvia::detail::db_result_access::column_names(rows);
    names.emplace_back("id");
    names.emplace_back("name");
    auto& fields_value = ruvia::detail::db_result_access::fields(rows);
    fields_value.push_back(ruvia::detail::db_result_access::owned_field("3", std::pmr::get_default_resource()));
    fields_value.push_back(ruvia::detail::db_result_access::owned_field("first", std::pmr::get_default_resource()));
    auto& result_rows = ruvia::detail::db_result_access::rows(rows);
    result_rows.push_back(ruvia::detail::db_result_access::borrowed_row(
        fields_value.data(), fields_value.size(), names.data(), names.size(), std::pmr::get_default_resource()));
    const auto entity = one(std::move(rows), std::pmr::get_default_resource());
    RUVIA_CHECK(entity.has_value());
    RUVIA_CHECK_EQ(entity->get<"id">(), 3);
    RUVIA_CHECK_EQ(entity->get<"name">(), std::string_view("first"));

    auto counts = ruvia::detail::db_result_access::make_result(std::pmr::get_default_resource());
    auto& count_names = ruvia::detail::db_result_access::column_names(counts);
    count_names.emplace_back("count");
    auto& count_fields = ruvia::detail::db_result_access::fields(counts);
    count_fields.push_back(ruvia::detail::db_result_access::owned_field("7", std::pmr::get_default_resource()));
    ruvia::detail::db_result_access::rows(counts).push_back(ruvia::detail::db_result_access::borrowed_row(
        count_fields.data(), 1, count_names.data(), count_names.size(), std::pmr::get_default_resource()));
    RUVIA_CHECK_EQ(ruvia::detail::db_count_value(counts), std::uint64_t{7});
    auto empty_count = ruvia::detail::db_result_access::make_result(std::pmr::get_default_resource());
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::detail::db_count_value(empty_count); }));
    auto missing_count = ruvia::detail::db_result_access::make_result(std::pmr::get_default_resource());
    auto& missing_count_names = ruvia::detail::db_result_access::column_names(missing_count);
    missing_count_names.emplace_back("other");
    auto& missing_count_fields = ruvia::detail::db_result_access::fields(missing_count);
    missing_count_fields.push_back(ruvia::detail::db_result_access::owned_field("7", std::pmr::get_default_resource()));
    ruvia::detail::db_result_access::rows(missing_count).push_back(ruvia::detail::db_result_access::borrowed_row(missing_count_fields.data(), 1, missing_count_names.data(), missing_count_names.size(), std::pmr::get_default_resource()));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::detail::db_count_value(missing_count); }));
    auto null_count = ruvia::detail::db_result_access::make_result(std::pmr::get_default_resource());
    auto& null_count_names = ruvia::detail::db_result_access::column_names(null_count);
    null_count_names.emplace_back("count");
    auto& null_count_fields = ruvia::detail::db_result_access::fields(null_count);
    null_count_fields.push_back(ruvia::detail::db_result_access::null_field(std::pmr::get_default_resource()));
    ruvia::detail::db_result_access::rows(null_count).push_back(ruvia::detail::db_result_access::borrowed_row(null_count_fields.data(), 1, null_count_names.data(), null_count_names.size(), std::pmr::get_default_resource()));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::detail::db_count_value(null_count); }));

    auto exists_rows = ruvia::detail::db_result_access::make_result(std::pmr::get_default_resource());
    auto& exists_names = ruvia::detail::db_result_access::column_names(exists_rows);
    exists_names.emplace_back("exists");
    auto& exists_fields = ruvia::detail::db_result_access::fields(exists_rows);
    exists_fields.push_back(ruvia::detail::db_result_access::owned_field("true", std::pmr::get_default_resource()));
    ruvia::detail::db_result_access::rows(exists_rows).push_back(ruvia::detail::db_result_access::borrowed_row(exists_fields.data(), 1, exists_names.data(), exists_names.size(), std::pmr::get_default_resource()));
    ruvia::detail::db_map_exists exists;
    RUVIA_CHECK(exists(std::move(exists_rows), std::pmr::get_default_resource()));

    auto bad_exists = ruvia::detail::db_result_access::make_result(std::pmr::get_default_resource());
    auto& bad_names = ruvia::detail::db_result_access::column_names(bad_exists);
    bad_names.emplace_back("exists");
    auto& bad_fields = ruvia::detail::db_result_access::fields(bad_exists);
    bad_fields.push_back(ruvia::detail::db_result_access::owned_field("not-bool", std::pmr::get_default_resource()));
    ruvia::detail::db_result_access::rows(bad_exists).push_back(ruvia::detail::db_result_access::borrowed_row(bad_fields.data(), 1, bad_names.data(), bad_names.size(), std::pmr::get_default_resource()));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)exists(std::move(bad_exists), std::pmr::get_default_resource()); }));

    auto wrong_exists = ruvia::detail::db_result_access::make_result(std::pmr::get_default_resource());
    auto& wrong_names = ruvia::detail::db_result_access::column_names(wrong_exists);
    wrong_names.emplace_back("exists");
    auto& wrong_fields = ruvia::detail::db_result_access::fields(wrong_exists);
    wrong_fields.push_back(ruvia::detail::db_result_access::owned_field("true", std::pmr::get_default_resource()));
    wrong_fields.push_back(ruvia::detail::db_result_access::owned_field("false", std::pmr::get_default_resource()));
    auto& wrong_rows = ruvia::detail::db_result_access::rows(wrong_exists);
    wrong_rows.push_back(ruvia::detail::db_result_access::borrowed_row(
        wrong_fields.data(), 1, wrong_names.data(), wrong_names.size(), std::pmr::get_default_resource()));
    wrong_rows.push_back(ruvia::detail::db_result_access::borrowed_row(
        wrong_fields.data() + 1, 1, wrong_names.data(), wrong_names.size(), std::pmr::get_default_resource()));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)exists(std::move(wrong_exists), std::pmr::get_default_resource()); }));

    auto missing_exists = ruvia::detail::db_result_access::make_result(std::pmr::get_default_resource());
    auto& missing_fields = ruvia::detail::db_result_access::fields(missing_exists);
    missing_fields.push_back(ruvia::detail::db_result_access::owned_field("true", std::pmr::get_default_resource()));
    auto& missing_names = ruvia::detail::db_result_access::column_names(missing_exists);
    missing_names.emplace_back("other");
    ruvia::detail::db_result_access::rows(missing_exists).push_back(ruvia::detail::db_result_access::borrowed_row(missing_fields.data(), 1, missing_names.data(), missing_names.size(), std::pmr::get_default_resource()));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)exists(std::move(missing_exists), std::pmr::get_default_resource()); }));
}

struct query_gate final {
    std::coroutine_handle<> continuation_{};
    bool cancelled_{false};
    bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> value) noexcept {
        continuation_ = value;
    }
    void await_resume() {
        if (cancelled_) {
            throw ruvia::db_error(ruvia::db_error::code_type::cancelled, ruvia::db_driver::postgresql, "cancelled");
        }
    }
    void resume(bool cancel = false) {
        cancelled_ = cancel;
        auto saved = std::exchange(continuation_, {});
        saved.resume();
    }
};

ruvia::task<ruvia::db_rows> owned_rows_task(std::pmr::string value, std::pmr::memory_resource* resource,
    bool invalid = false, query_gate* gate = nullptr) {
    if (gate != nullptr) {
        co_await *gate;
    }
    auto rows = ruvia::detail::db_result_access::make_result(resource);
    auto& names = ruvia::detail::db_result_access::column_names(rows);
    names.emplace_back("id");
    names.emplace_back("name");
    auto& fields_value = ruvia::detail::db_result_access::fields(rows);
    fields_value.push_back(ruvia::detail::db_result_access::owned_field(invalid ? "invalid" : "3", resource));
    fields_value.push_back(ruvia::detail::db_result_access::owned_field(value, resource));
    ruvia::detail::db_result_access::rows(rows).push_back(ruvia::detail::db_result_access::borrowed_row(fields_value.data(), fields_value.size(), names.data(), names.size(), resource));
    co_return rows;
}

RUVIA_TEST(db_mapped_query_repeated_results_release_temporaries_and_retain_fields) {
    asio::io_context context;
    ruvia::test::counting_memory_resource resource;
    const auto perform = [&] {
        context.restart();
        std::optional<ruvia::entity_rows<entity_type>> result;
        std::exception_ptr failure;
        start_task(context,
            ruvia::detail::map_db_query<ruvia::entity_rows<entity_type>>(
                owned_rows_task(std::pmr::string(500, 'x', &resource), &resource), &resource, ruvia::detail::db_map_entity_rows<entity_type>{}),
            [&](std::exception_ptr error, auto value) {
                if (error) {
                    failure = std::move(error);
                } else {
                    result.emplace(std::move(*value));
                }
            });
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
    const auto baseline = resource.live_allocations();
    const std::string expected(500, 'x');
    for (int i = 0; i < 20; ++i) {
        {
            auto next_value = perform();
            RUVIA_CHECK_EQ((*next_value)[0].get<"name">().size(), std::size_t{500});
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
        RUVIA_CHECK_EQ((*retained)[0].get<"name">(), std::string_view(expected));
    }
    retained.reset();
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(db_mapping_rebinds_column_positions_when_row_schema_changes) {
    auto* resource = std::pmr::get_default_resource();
    const std::array first_names{std::pmr::string("id"), std::pmr::string("name")};
    const std::array reversed_names{std::pmr::string("name"), std::pmr::string("id")};
    const std::array first_fields{ruvia::detail::db_result_access::owned_field("3", resource), ruvia::detail::db_result_access::owned_field("first", resource)};
    const std::array second_fields{ruvia::detail::db_result_access::owned_field("second", resource), ruvia::detail::db_result_access::owned_field("4", resource)};
    const std::array third_fields{ruvia::detail::db_result_access::owned_field("third", resource), ruvia::detail::db_result_access::owned_field("5", resource)};
    auto rows = ruvia::detail::db_result_access::make_result(resource);
    auto& result_rows = ruvia::detail::db_result_access::rows(rows);
    result_rows.push_back(ruvia::detail::db_result_access::borrowed_row(first_fields.data(), first_fields.size(), first_names.data(), first_names.size(), resource));
    result_rows.push_back(ruvia::detail::db_result_access::borrowed_row(second_fields.data(), second_fields.size(), reversed_names.data(), reversed_names.size(), resource));
    result_rows.push_back(ruvia::detail::db_result_access::borrowed_row(third_fields.data(), third_fields.size(), reversed_names.data(), reversed_names.size(), resource));
    const auto mapped = ruvia::detail::map_entity_rows<entity_type>(std::move(rows), resource);
    RUVIA_CHECK_EQ(mapped.size(), std::size_t{3});
    RUVIA_CHECK_EQ(mapped[0].get<"id">(), 3);
    RUVIA_CHECK_EQ(mapped[0].get<"name">(), std::string_view("first"));
    RUVIA_CHECK_EQ(mapped[1].get<"id">(), 4);
    RUVIA_CHECK_EQ(mapped[1].get<"name">(), std::string_view("second"));
    RUVIA_CHECK_EQ(mapped[2].get<"id">(), 5);
    RUVIA_CHECK_EQ(mapped[2].get<"name">(), std::string_view("third"));
}

RUVIA_TEST(db_mapping_preserves_field_error_order_with_missing_columns) {
    auto* resource = std::pmr::get_default_resource();
    const std::array names{std::pmr::string("id")};
    ruvia::detail::db_entity_row_decoder<entity_type> decoder;
    for (const bool invalid : {true, false}) {
        const std::array fields_value{ruvia::detail::db_result_access::owned_field(invalid ? "invalid" : "7", resource)};
        const auto row = ruvia::detail::db_result_access::borrowed_row(fields_value.data(), fields_value.size(), names.data(), names.size(), resource);
        bool rejected = false;
        try {
            (void)decoder.decode(row, resource);
        } catch (const ruvia::db_conversion_error& error) {
            rejected = true;
            RUVIA_CHECK(invalid);
            RUVIA_CHECK(error.code() == ruvia::db_conversion_error::code_type::invalid_format);
        } catch (const std::out_of_range& error) {
            rejected = true;
            RUVIA_CHECK(!invalid);
            RUVIA_CHECK_EQ(std::string_view(error.what()), std::string_view("database result has no such column"));
        }
        RUVIA_CHECK(rejected);
    }
}

RUVIA_TEST(db_mapping_reserves_known_row_count_once) {
    using numeric_type = ruvia::db_entity<"numbers", ruvia::db_column<"id", int>>;
    using projection_type = numeric_projection;
    for (const std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{128}}) {
        const auto make_rows = [count] {
            auto* source_value = std::pmr::get_default_resource();
            auto rows = ruvia::detail::db_result_access::make_result(source_value);
            auto& names = ruvia::detail::db_result_access::column_names(rows);
            names.emplace_back("id");
            auto& fields_value = ruvia::detail::db_result_access::fields(rows);
            fields_value.push_back(ruvia::detail::db_result_access::owned_field("7", source_value));
            for (std::size_t i = 0; i < count; ++i) {
                ruvia::detail::db_result_access::rows(rows).push_back(
                    ruvia::detail::db_result_access::borrowed_row(fields_value.data(), fields_value.size(), names.data(), names.size(), source_value));
            }
            return rows;
        };
        ruvia::test::counting_memory_resource resource;
        // Debug standard libraries may allocate iterator-tracking storage even
        // for an empty vector. Compare against one reserve on the same type.
        const auto reserved_allocations = [count]<typename t_type>() {
            ruvia::test::counting_memory_resource baseline;
            {
                std::pmr::vector<t_type> rows(&baseline);
                rows.reserve(count);
            }
            return baseline.allocation_count();
        };
        const auto entity_allocations = reserved_allocations.template operator()<numeric_type>();
        const auto projection_allocations = reserved_allocations.template operator()<projection_type>();
        {
            const auto mapped = ruvia::detail::map_entity_rows<numeric_type>(make_rows(), &resource);
            RUVIA_CHECK_EQ(mapped.size(), count);
            RUVIA_CHECK_EQ(resource.allocation_count(), entity_allocations);
            for (const auto& entity : mapped) {
                RUVIA_CHECK_EQ(entity.get<"id">(), 7);
            }
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        {
            const ruvia::detail::db_map_projection<projection_type> mapper({});
            const auto mapped = mapper(make_rows(), &resource);
            RUVIA_CHECK_EQ(mapped.size(), count);
            RUVIA_CHECK_EQ(resource.allocation_count(), entity_allocations + projection_allocations);
            for (const auto& entity : mapped) {
                RUVIA_CHECK_EQ(entity.get<"id">(), 7);
            }
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
}

RUVIA_TEST(db_projection_selection_does_not_allocate_or_borrow_field_names) {
    using output_type = named_projection;
    ruvia::test::counting_memory_resource source;
    std::optional<ruvia::detail::db_map_projection<output_type>> mapper;
    {
        std::pmr::vector<std::pmr::string> selection(&source);
        selection.emplace_back("name");
        const auto allocations = source.allocation_count();
        mapper.emplace(selection);
        RUVIA_CHECK_EQ(source.allocation_count(), allocations);
        selection.front() = "id";
    }
    RUVIA_CHECK_EQ(source.live_allocations(), std::size_t{0});

    auto* resource = std::pmr::get_default_resource();
    const std::array names{std::pmr::string("id", resource), std::pmr::string("name", resource)};
    const std::array fields_value{
        ruvia::detail::db_result_access::owned_field("not-an-integer", resource),
        ruvia::detail::db_result_access::owned_field("selected", resource)};
    const auto row = ruvia::detail::db_result_access::borrowed_row(fields_value.data(), fields_value.size(), names.data(), names.size(), resource);
    const auto result_value = mapper->decode(row, resource);
    RUVIA_CHECK(!result_value.is_set<"id">());
    RUVIA_CHECK_EQ(result_value.get<"name">(), std::string_view("selected"));
}

RUVIA_TEST(db_projection_mapping_reclaims_operations_and_preserves_partial_results) {
    using output_type = named_projection;
    asio::io_context context;
    ruvia::test::counting_memory_resource resource;
    const auto operation = [&](bool invalid, query_gate* gate_value, bool partial) {
        std::pmr::vector<std::pmr::string> names(&resource);
        if (!partial) {
            names.emplace_back("id");
        }
        names.emplace_back("name");
        return ruvia::detail::map_db_query<ruvia::entity_rows<output_type>>(
            owned_rows_task(std::pmr::string(500, 'p', &resource), &resource, invalid, gate_value), &resource,
            ruvia::detail::db_map_projection<output_type>(names));
    };
    {
        auto cold = operation(false, nullptr, false);
        RUVIA_CHECK(resource.live_allocations() > 0);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    std::optional<ruvia::entity_rows<output_type>> retained;
    start_task(context, operation(false, nullptr, true), [&](std::exception_ptr failure, auto value) {
        if (failure) {
            std::rethrow_exception(failure);
        }
        retained.emplace(std::move(*value));
    });
    context.run();
    RUVIA_CHECK(retained.has_value());
    RUVIA_CHECK(!(*retained)[0].is_set<"id">());
    const auto baseline = resource.live_allocations();
    for (int i = 0; i < 12; ++i) {
        for (int mode = 0; mode < 3; ++mode) {
            context.restart();
            query_gate gate;
            bool observed_value = false;
            start_task(context, operation(mode == 1, mode == 2 ? &gate : nullptr, false), [&](std::exception_ptr failure, auto value) {
                if (mode == 0) {
                    if (failure) {
                        std::rethrow_exception(failure);
                    }
                    auto result_value = std::move(*value);
                    observed_value = result_value[0].template get<"id">() == 3;
                } else if (failure) {
                    try {
                        std::rethrow_exception(failure);
                    } catch (const ruvia::db_conversion_error&) {
                        observed_value = mode == 1;
                    } catch (const ruvia::db_error& error) {
                        observed_value = mode == 2 && error.code() == ruvia::db_error::code_type::cancelled;
                    }
                }
            });
            if (mode == 2) {
                gate.resume(true);
            }
            context.run();
            RUVIA_CHECK(observed_value);
            RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
            RUVIA_CHECK_EQ((*retained)[0].get<"name">(), std::string_view(std::string(500, 'p')));
        }
    }
    retained.reset();
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(db_mapped_query_cold_drop_conversion_failure_and_cancellation_release_storage) {
    asio::io_context context;
    ruvia::test::counting_memory_resource resource;
    {
        auto cold = ruvia::detail::map_db_query<ruvia::entity_rows<entity_type>>(
            owned_rows_task(std::pmr::string(500, 'x', &resource), &resource), &resource, ruvia::detail::db_map_entity_rows<entity_type>{});
        RUVIA_CHECK(resource.live_allocations() > 0);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    bool conversion_failure = false;
    start_task(context,
        ruvia::detail::map_db_query<ruvia::entity_rows<entity_type>>(
            owned_rows_task(std::pmr::string(500, 'x', &resource), &resource, true), &resource, ruvia::detail::db_map_entity_rows<entity_type>{}),
        [&](std::exception_ptr failure, auto) {
            if (failure) {
                try {
                    std::rethrow_exception(failure);
                } catch (const ruvia::db_conversion_error&) {
                    conversion_failure = true;
                }
            }
        });
    context.run();
    RUVIA_CHECK(conversion_failure);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    query_gate gate;
    bool cancelled = false;
    context.restart();
    start_task(context,
        ruvia::detail::map_db_query<ruvia::entity_rows<entity_type>>(
            owned_rows_task(std::pmr::string(500, 'x', &resource), &resource, false, &gate), &resource, ruvia::detail::db_map_entity_rows<entity_type>{}),
        [&](std::exception_ptr failure, auto) {
            if (failure) {
                try {
                    std::rethrow_exception(failure);
                } catch (const ruvia::db_error& error) {
                    cancelled = error.code() == ruvia::db_error::code_type::cancelled;
                }
            }
        });
    RUVIA_CHECK(gate.continuation_ != nullptr);
    RUVIA_CHECK(resource.live_allocations() > 0);
    gate.resume(true);
    context.run();
    RUVIA_CHECK(cancelled);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(db_relation_mapped_query_cold_drop_and_cancellation_release_owned_plan) {
    using target_type = ruvia::db_entity<"targets", ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>>;
    using source = ruvia::db_entity<"sources", ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>,
        ruvia::db_many_to_one<"target", target_type, ruvia::db_join_column<"id", "id">>>;
    ruvia::test::counting_memory_resource resource;
    const auto operation = [&](query_gate* gate_value) {
        ruvia::db_query query(&resource);
        query.select(query.column("id", "s")).from("sources", "s");
        ruvia::detail::db_relation_plan plan(&resource);
        plan.add<source>(query, "s", "target");
        return ruvia::detail::map_db_query<ruvia::entity_rows<source>>(
            owned_rows_task(std::pmr::string(500, 'x', &resource), &resource, false, gate_value),
            &resource, ruvia::detail::db_map_related_entities<source>{std::move(plan)});
    };
    {
        auto cold = operation(nullptr);
        RUVIA_CHECK(resource.live_allocations() > 0);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    asio::io_context context;
    for (int i = 0; i < 12; ++i) {
        query_gate gate;
        bool cancelled = false;
        context.restart();
        start_task(context, operation(&gate), [&](std::exception_ptr failure, auto) {
            if (failure) {
                try {
                    std::rethrow_exception(failure);
                } catch (const ruvia::db_error& error) {
                    cancelled = error.code() == ruvia::db_error::code_type::cancelled;
                }
            }
        });
        RUVIA_CHECK(gate.continuation_ != nullptr);
        RUVIA_CHECK(resource.live_allocations() > 0);
        gate.resume(true);
        context.run();
        RUVIA_CHECK(cancelled);
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

ruvia::task<ruvia::db_rows> count_rows_task(std::pmr::memory_resource* resource,
    query_gate* gate = nullptr, bool invalid = false, bool* started = nullptr) {
    if (started != nullptr) {
        *started = true;
    }
    if (gate != nullptr) {
        co_await *gate;
    }
    auto rows = ruvia::detail::db_result_access::make_result(resource);
    auto& names = ruvia::detail::db_result_access::column_names(rows);
    names.emplace_back("count");
    auto& fields_value = ruvia::detail::db_result_access::fields(rows);
    fields_value.push_back(ruvia::detail::db_result_access::owned_field(invalid ? "invalid" : "17", resource));
    ruvia::detail::db_result_access::rows(rows).push_back(ruvia::detail::db_result_access::borrowed_row(fields_value.data(), fields_value.size(), names.data(), names.size(), resource));
    co_return rows;
}

RUVIA_TEST(db_mapped_page_repeated_operations_release_temporaries_and_retain_results) {
    using page_type = std::pair<ruvia::entity_rows<entity_type>, std::uint64_t>;
    asio::io_context context;
    ruvia::test::counting_memory_resource resource;
    const auto perform = [&] {
        context.restart();
        std::optional<page_type> result;
        std::exception_ptr failure;
        auto query = ruvia::detail::query_db_pair(owned_rows_task(std::pmr::string(500, 'p', &resource), &resource), count_rows_task(&resource));
        start_task(context, ruvia::detail::map_db_query_and_count<ruvia::entity_rows<entity_type>>(std::move(query), &resource, ruvia::detail::db_map_entity_rows<entity_type>{}),
            [&](std::exception_ptr error, auto value) {
                if (error) {
                    failure = std::move(error);
                } else {
                    result.emplace(std::move(*value));
                }
            });
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
    const auto baseline = resource.live_allocations();
    for (int i = 0; i < 12; ++i) {
        {
            auto next_value = perform();
            RUVIA_CHECK_EQ(next_value->second, std::uint64_t{17});
            RUVIA_CHECK_EQ(next_value->first.size(), std::size_t{1});
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
        const std::string expected(500, 'p');
        RUVIA_CHECK_EQ(std::string_view(retained->first[0].get<"name">()), std::string_view(expected));
    }
    retained.reset();
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

RUVIA_TEST(db_mapped_page_cold_drop_failures_and_cancellation_release_both_queries) {
    asio::io_context context;
    ruvia::test::counting_memory_resource resource;
    const auto operation = [&](query_gate* first, query_gate* second, bool invalid_row, bool invalid_count, bool* count_started) {
        return ruvia::detail::map_db_query_and_count<ruvia::entity_rows<entity_type>>(
            ruvia::detail::query_db_pair(owned_rows_task(std::pmr::string(500, 'p', &resource), &resource, invalid_row, first),
                count_rows_task(&resource, second, invalid_count, count_started)),
            &resource, ruvia::detail::db_map_entity_rows<entity_type>{});
    };
    bool count_started = false;
    {
        auto cold = operation(nullptr, nullptr, false, false, &count_started);
        RUVIA_CHECK(resource.live_allocations() > 0);
    }
    RUVIA_CHECK(!count_started);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    for (int scenario = 0; scenario < 4; ++scenario) {
        context.restart();
        query_gate gate;
        count_started = false;
        bool failed = false;
        bool cancelled = false;
        start_task(context, operation(scenario == 0 ? &gate : nullptr, scenario == 1 ? &gate : nullptr, scenario == 2, scenario == 3, &count_started),
            [&](std::exception_ptr failure, auto) {
                failed = failure != nullptr;
                if (failure) {
                    try {
                        std::rethrow_exception(failure);
                    } catch (const ruvia::db_error& error) {
                        cancelled = error.code() == ruvia::db_error::code_type::cancelled;
                    } catch (const ruvia::db_conversion_error&) {
                    }
                }
            });
        if (scenario < 2) {
            RUVIA_CHECK(gate.continuation_ != nullptr);
            RUVIA_CHECK_EQ(count_started, scenario == 1);
            gate.resume(true);
        }
        context.run();
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(cancelled, scenario < 2);
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}

}  // namespace
