#include <cstdint>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/web/db/db_entity.h"
#include "ruvia/web/db/db_handle.h"
#include "ruvia/web/db/db_repository.h"

#include "backend_client_fixture.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

RUVIA_DB_ENTITY(entity, "users",
    ruvia::db_column<ruvia::fixed_string{"id"}, int>,
    ruvia::db_column<ruvia::fixed_string{"name"}, std::pmr::string,
        ruvia::db_column_options{.nullable_ = true}>)

RUVIA_DB_ENTITY(owned_type, "owned", ruvia::db_column<"text", ruvia::string>,
    ruvia::db_column<"values", std::pmr::vector<std::optional<std::pmr::string>>>)

RUVIA_DB_ENTITY(string_array_entity, "string_arrays",
    ruvia::db_column<"values", std::pmr::vector<std::pmr::string>>)

RUVIA_DB_ENTITY(owned_container_entity, "owned_containers",
    ruvia::db_column<"text", std::pmr::string>,
    ruvia::db_column<"values", std::pmr::vector<std::pmr::string>>)

class equivalent_memory_resource final : public std::pmr::memory_resource {
public:
    explicit equivalent_memory_resource(std::pmr::memory_resource* upstream)
        : upstream_(upstream) {}

    void reject_allocations(bool reject = true) noexcept {
        reject_allocations_ = reject;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_allocations_) {
            throw std::bad_alloc();
        }
        return upstream_->allocate(bytes_value, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        upstream_->deallocate(pointer, bytes_value, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        const auto* equivalent = dynamic_cast<const equivalent_memory_resource*>(&other);
        return equivalent != nullptr && upstream_ == equivalent->upstream_;
    }

    std::pmr::memory_resource* upstream_;
    bool reject_allocations_{false};
};

RUVIA_TEST(db_entity_tracks_unset_null_and_value_states) {
    entity entity;
    RUVIA_CHECK(!entity.is_set<"id">());
    entity.set<"id">(7);
    RUVIA_CHECK(entity.is_set<"id">());
    RUVIA_CHECK_EQ(entity.get<"id">(), 7);
    const auto& const_entity = entity;
    RUVIA_CHECK_EQ(const_entity.get<"id">(), 7);
    entity.set_null<"name">();
    RUVIA_CHECK(entity.is_null<"name">());
    entity.reset<"id">();
    RUVIA_CHECK(!entity.is_set<"id">());
    RUVIA_CHECK_EQ(entity::table_name(), std::string_view("users"));
    RUVIA_CHECK_EQ(entity::column_index<"name">(), std::size_t{1});
}

RUVIA_TEST(db_entity_set_normalizes_nested_owned_values_to_entity_resource) {
    ruvia::test::counting_memory_resource source, target;
    {
        owned_type entity(&target);
        const auto baseline = target.live_allocations();
        {
            ruvia::string text(std::string(200, 'x'), {.resource_ = &source});
            entity.set<"text">(std::move(text));
            std::pmr::vector<std::optional<std::pmr::string>> values(&source);
            values.emplace_back(std::pmr::string(200, 'y', &source));
            values.emplace_back(std::nullopt);
            entity.set<"values">(std::move(values));
        }
        RUVIA_CHECK_EQ(source.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(std::string_view(entity.get<"text">()), std::string(200, 'x'));
        RUVIA_CHECK_EQ(std::string_view(*entity.get<"values">()[0]), std::string(200, 'y'));
        RUVIA_CHECK(entity.get<"values">()[0]->get_allocator().resource() == &target);
        entity.reset<"text">();
        entity.reset<"values">();
        RUVIA_CHECK_EQ(target.live_allocations(), baseline);
    }
    RUVIA_CHECK_EQ(target.allocation_count(), target.deallocation_count());
}

RUVIA_TEST(db_entity_string_vector_assignment_normalizes_equivalent_resource_allocators) {
    ruvia::test::counting_memory_resource upstream;
    equivalent_memory_resource target(&upstream);
    {
        string_array_entity entity(&target);
        {
            equivalent_memory_resource source_value(&upstream);
            RUVIA_CHECK(source_value.is_equal(target));
            std::pmr::vector<std::pmr::string> values(&source_value);
            values.emplace_back(200, 'a');
            values.emplace_back(240, 'b');
            entity.set<"values">(values);
            RUVIA_CHECK_EQ(values.size(), std::size_t{2});
            RUVIA_CHECK_EQ(values[0], std::string_view(std::string(200, 'a')));
            RUVIA_CHECK_EQ(values[1], std::string_view(std::string(240, 'b')));
            RUVIA_CHECK(values.get_allocator().resource() == &source_value);
            RUVIA_CHECK(values[0].get_allocator().resource() == &source_value);
        }
        const auto& lvalue_stored = entity.get<"values">();
        RUVIA_CHECK(lvalue_stored.get_allocator().resource() == &target);
        RUVIA_CHECK(lvalue_stored[0].get_allocator().resource() == &target);
        RUVIA_CHECK(lvalue_stored[1].get_allocator().resource() == &target);
        RUVIA_CHECK_EQ(lvalue_stored[0], std::string_view(std::string(200, 'a')));
        RUVIA_CHECK_EQ(lvalue_stored[1], std::string_view(std::string(240, 'b')));

        {
            equivalent_memory_resource source_value(&upstream);
            std::pmr::vector<std::pmr::string> values(&source_value);
            values.emplace_back(260, 'b');
            values.emplace_back(280, 'c');
            entity.set<"values">(std::move(values));
        }
        const auto& rvalue_stored = entity.get<"values">();
        RUVIA_CHECK(rvalue_stored.get_allocator().resource() == &target);
        RUVIA_CHECK(rvalue_stored[0].get_allocator().resource() == &target);
        RUVIA_CHECK(rvalue_stored[1].get_allocator().resource() == &target);
        RUVIA_CHECK_EQ(rvalue_stored[0], std::string_view(std::string(260, 'b')));
        RUVIA_CHECK_EQ(rvalue_stored[1], std::string_view(std::string(280, 'c')));

        target.reject_allocations();
        RUVIA_CHECK(ruvia::testing::throws_on([&] {
            equivalent_memory_resource source_value(&upstream);
            std::pmr::vector<std::pmr::string> values(&source_value);
            values.emplace_back(220, 'd');
            entity.set<"values">(std::move(values));
        }));
        target.reject_allocations(false);
        RUVIA_CHECK(entity.is_set<"values">());
        RUVIA_CHECK_EQ(entity.get<"values">().size(), std::size_t{2});
        RUVIA_CHECK_EQ(entity.get<"values">()[0], std::string_view(std::string(260, 'b')));
        RUVIA_CHECK(entity.get<"values">().get_allocator().resource() == &target);
        RUVIA_CHECK(entity.get<"values">()[0].get_allocator().resource() == &target);
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

RUVIA_TEST(db_entity_reset_owned_containers_preserves_value_on_allocation_failure) {
    for (const bool text : {false, true}) {
        ruvia::test::counting_memory_resource upstream;
        equivalent_memory_resource target(&upstream);
        {
            owned_container_entity entity(&target);
            entity.set<"text">(std::string(200, 'a'));
            std::pmr::vector<std::pmr::string> values(&upstream);
            values.emplace_back(240, 'b');
            entity.set<"values">(values);
            bool failed = false;
            target.reject_allocations();
            try {
                if (text) {
                    entity.reset<"text">();
                } else {
                    entity.reset<"values">();
                }
            } catch (const std::bad_alloc&) {
                failed = true;
            }
            target.reject_allocations(false);
            if (failed) {
                RUVIA_CHECK(entity.is_set<"text">());
                RUVIA_CHECK(entity.is_set<"values">());
                RUVIA_CHECK_EQ(entity.get<"text">(), std::string_view(std::string(200, 'a')));
                RUVIA_CHECK_EQ(entity.get<"values">().size(), std::size_t{1});
                RUVIA_CHECK_EQ(entity.get<"values">()[0], std::string_view(std::string(240, 'b')));
                RUVIA_CHECK(entity.get<"text">().get_allocator().resource() == &target);
                RUVIA_CHECK(entity.get<"values">().get_allocator().resource() == &target);
                RUVIA_CHECK(entity.get<"values">()[0].get_allocator().resource() == &target);
            } else {
                // Implementations without debug proxies can clear without allocating.
                RUVIA_CHECK(text ? !entity.is_set<"text">() : !entity.is_set<"values">());
            }
        }
        RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
    }
}

RUVIA_TEST(db_entity_rows_reject_elements_from_a_different_memory_resource) {
    ruvia::test::counting_memory_resource result_resource;
    ruvia::test::counting_memory_resource entity_resource;
    {
        ruvia::entity_rows<entity> rows(&result_resource);
        entity entity(&entity_resource);
        entity.set<"id">(7);
        entity.set<"name">(std::string(200, 'x'));
        RUVIA_CHECK(ruvia::testing::throws_on([&] { rows.push_back(std::move(entity)); }));
        RUVIA_CHECK(rows.empty());
    }
    RUVIA_CHECK_EQ(result_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(entity_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(entity_resource.allocation_count(), entity_resource.deallocation_count());
}

RUVIA_DB_PROJECTION(entity_projection,
    RUVIA_DB_COLUMN(id, int),
    RUVIA_DB_COLUMN(name, std::pmr::string))

RUVIA_TEST(db_entities_projections_and_rows_construct_from_resource_owners) {
    ruvia::test::counting_memory_resource resource;
    entity owner_entity(&resource);
    entity from_owner(owner_entity);
    RUVIA_CHECK_EQ(from_owner.resource(), &resource);

    entity_projection proj_from_owner(owner_entity);
    RUVIA_CHECK_EQ(proj_from_owner.resource(), &resource);

    ruvia::entity_rows<entity> rows(owner_entity);
    RUVIA_CHECK_EQ(rows.resource(), &resource);
}

}  // namespace

RUVIA_DB_ENTITY(equivalent_rebind_entity, "equivalent_rebind",
    ruvia::db_column<"text", std::pmr::string>,
    ruvia::db_column<"opt_text", ruvia::string>,
    ruvia::db_column<"numbers", std::pmr::vector<int>>,
    ruvia::db_column<"strings", std::pmr::vector<std::pmr::string>>)

RUVIA_TEST(db_entity_set_rebinds_rvalues_across_equivalent_resources_without_stealing_from_retired_source) {
    ruvia::test::counting_memory_resource upstream;
    {
        ruvia::test::equivalent_tracking_resource source(upstream);
        ruvia::test::equivalent_tracking_resource target(upstream);
        RUVIA_CHECK(source.is_equal(target));
        std::optional<equivalent_rebind_entity> entity(std::in_place, &target);
        {
            std::pmr::string text(200, 't', &source);
            ruvia::string opt_text(std::string(200, 'o'), ruvia::model_options{.resource_ = &source});
            std::pmr::vector<int> numbers(64, 42, &source);
            std::pmr::vector<std::pmr::string> strings(&source);
            strings.emplace_back(200, 's');

            entity->set<"text">(std::move(text));
            entity->set<"opt_text">(std::move(opt_text));
            entity->set<"numbers">(std::move(numbers));
            entity->set<"strings">(std::move(strings));
        }
        source.retire();
        RUVIA_CHECK_EQ(entity->get<"text">(), std::string_view(std::string(200, 't')));
        RUVIA_CHECK_EQ(entity->get<"opt_text">().view(), std::string_view(std::string(200, 'o')));
        RUVIA_CHECK_EQ(entity->get<"numbers">().size(), std::size_t{64});
        RUVIA_CHECK_EQ(entity->get<"strings">().size(), std::size_t{1});
        RUVIA_CHECK_EQ(entity->get<"strings">()[0], std::string_view(std::string(200, 's')));
        entity.reset();
        RUVIA_CHECK_EQ(source.callbacks_after_retirement(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

#ifdef RUVIA_ENABLE_POSTGRESQL
RUVIA_DB_ENTITY(numeric_decode_entity, "numeric_decode",
    ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>,
    ruvia::db_column<"ratio", double>,
    ruvia::db_column<"ratios", std::pmr::vector<double>>,
    ruvia::db_column<"counts", std::pmr::vector<std::int64_t>>)

RUVIA_TEST(db_field_and_array_decoding_reject_nonfinite_floats_and_prioritize_format_errors) {
    using namespace ruvia;
    std::vector<test::backend_startup_peer::postgres_reply> replies{
        {.column_ = "val", .rows_ = {"nan"}},
        {.column_ = "val", .rows_ = {"nan(123)"}},
        {.column_ = "val", .rows_ = {"inf"}},
        {.column_ = "val", .rows_ = {"-infinity"}},
        {.column_ = "val", .rows_ = {"999999999999999999999abc"}},
        {.column_ = "val", .rows_ = {"999999999999999999999"}},
        {.column_ = "ratios", .type_ = 1022, .rows_ = {"{nan}"}},
        {.column_ = "ratios", .type_ = 1022, .rows_ = {"{inf}"}},
        {.column_ = "counts", .type_ = 1016, .rows_ = {"{999999999999999999999abc}"}},
        {.column_ = "counts", .type_ = 1016, .rows_ = {"{999999999999999999999}"}},
    };
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle, std::move(replies));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.handle();
        for (int i = 0; i < 4; ++i) {
            const auto rows = co_await handle.query("SELECT 1 AS val");
            RUVIA_CHECK_EQ(rows.size(), std::size_t{1});
            bool invalid_double = false;
            bool invalid_float = false;
            try {
                (void)rows.front()["val"].as<double>();
            } catch (const db_conversion_error& error) {
                invalid_double = error.code() == db_conversion_error::code_type::invalid_format;
            }
            try {
                (void)rows.front()["val"].as<float>();
            } catch (const db_conversion_error& error) {
                invalid_float = error.code() == db_conversion_error::code_type::invalid_format;
            }
            RUVIA_CHECK(invalid_double);
            RUVIA_CHECK(invalid_float);
        }
        {
            const auto rows = co_await handle.query("SELECT 1 AS val");
            bool invalid_format = false;
            try {
                (void)rows.front()["val"].as<std::int64_t>();
            } catch (const db_conversion_error& error) {
                invalid_format = error.code() == db_conversion_error::code_type::invalid_format;
            }
            RUVIA_CHECK(invalid_format);
        }
        {
            const auto rows = co_await handle.query("SELECT 1 AS val");
            bool out_of_range = false;
            try {
                (void)rows.front()["val"].as<std::int64_t>();
            } catch (const db_conversion_error& error) {
                out_of_range = error.code() == db_conversion_error::code_type::out_of_range;
            }
            RUVIA_CHECK(out_of_range);
        }

        auto repo = handle.repository<numeric_decode_entity>();
        for (int i = 0; i < 2; ++i) {
            bool invalid_array_float = false;
            try {
                auto qb = repo.query_builder("n");
                qb.select({"ratios"});
                (void)co_await qb.get_many();
            } catch (const db_conversion_error& error) {
                invalid_array_float = error.code() == db_conversion_error::code_type::invalid_format;
            }
            RUVIA_CHECK(invalid_array_float);
        }
        {
            bool invalid_array_int_format = false;
            try {
                auto qb = repo.query_builder("n");
                qb.select({"counts"});
                (void)co_await qb.get_many();
            } catch (const db_conversion_error& error) {
                invalid_array_int_format = error.code() == db_conversion_error::code_type::invalid_format;
            }
            RUVIA_CHECK(invalid_array_int_format);
        }
        {
            bool array_int_out_of_range = false;
            try {
                auto qb = repo.query_builder("n");
                qb.select({"counts"});
                (void)co_await qb.get_many();
            } catch (const db_conversion_error& error) {
                array_int_out_of_range = error.code() == db_conversion_error::code_type::out_of_range;
            }
            RUVIA_CHECK(array_int_out_of_range);
        }
    });
}
#endif
