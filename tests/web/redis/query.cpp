#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/redis/redis_repository.h"

#include "memory_resource_fixture.h"
#include "test_exceptions.h"

namespace {
RUVIA_REDIS_ENTITY(redis_query_user, "query-user",
    RUVIA_REDIS_COLUMN(id, ruvia::string, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(email, ruvia::string),
    RUVIA_REDIS_COLUMN(age, std::int32_t));
}

TEST(Ruvia, redis_predicate_composition_moves_source_and_preserves_range) {
    ruvia::test::counting_memory_resource resource;
    struct default_resource_guard {
        std::pmr::memory_resource* previous_;
        ~default_resource_guard() {
            std::pmr::set_default_resource(previous_);
        }
    } guard_value{std::pmr::set_default_resource(&resource)};
    {
        std::pmr::string text(200, 'x', ruvia::process_resource());
        auto filter = redis_query_user::column<"email">() == text;
        text.assign(200, 'y');
        auto range = redis_query_user::column<"age">().between(18, 65);
        ruvia::redis_find_options options{.where_ = std::move(filter) && range};
        EXPECT_TRUE(filter.empty());
        EXPECT_TRUE(!options.where_.empty());
        EXPECT_TRUE(!range.empty());
    }
    EXPECT_EQ(resource.allocation_count(), std::size_t{0});
}

TEST(Ruvia, redis_predicate_supports_copy_and_where_or_where_combination) {
    ruvia::test::counting_memory_resource resource;
    struct default_resource_guard {
        std::pmr::memory_resource* previous_;
        ~default_resource_guard() {
            std::pmr::set_default_resource(previous_);
        }
    } guard_value{std::pmr::set_default_resource(&resource)};
    {
        auto base = redis_query_user::column<"email">() == "alice@example.com";
        auto copied = base;
        EXPECT_TRUE(!base.empty());
        EXPECT_TRUE(!copied.empty());

        ruvia::redis_predicate combined;
        combined.and_where(copied);
        combined.or_where(redis_query_user::column<"age">() >= 21);
        EXPECT_TRUE(!combined.empty());
    }
    EXPECT_EQ(resource.allocation_count(), std::size_t{0});
}

namespace {
RUVIA_REDIS_ENTITY(redis_unsigned_pk_entity, "unsigned-pk",
    RUVIA_REDIS_COLUMN(id, std::uint64_t, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(name, ruvia::string));

RUVIA_REDIS_ENTITY(redis_signed_pk_entity, "signed-pk",
    RUVIA_REDIS_COLUMN(id, std::int64_t, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(name, ruvia::string));
}  // namespace

TEST(Ruvia, redis_predicate_holds_signed_and_unsigned_primary_key_comparisons) {
    const auto unsigned_eq = redis_unsigned_pk_entity::column<"id">() == std::uint64_t{42};
    const auto signed_eq = redis_signed_pk_entity::column<"id">() == std::int64_t{-7};
    EXPECT_TRUE(!unsigned_eq.empty());
    EXPECT_TRUE(!signed_eq.empty());
}

TEST(Ruvia, redis_predicate_self_combination_preserves_operands) {
    for (const bool use_or : {false, true}) {
        auto predicate = redis_signed_pk_entity::column<"id">() == std::int64_t{1};
        if (use_or) {
            predicate.or_where(predicate);
        } else {
            predicate.and_where(predicate);
        }
        EXPECT_TRUE(!predicate.empty());
    }
}

TEST(Ruvia, redis_predicate_negation_and_entity_column_name) {
    static_assert(redis_query_user::column_name<"email">() == "email");
    const auto pred = !(redis_query_user::column<"email">() == "alice@example.com");
    EXPECT_TRUE(!pred.empty());
    const ruvia::redis_predicate empty;
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)!empty; }));
}

namespace {
const ruvia::redis_repository_config search_config{
    .indexes_ = {
        {.column_ = "email", .kind_ = ruvia::redis_index_kind::tag, .sortable_ = true},
        {.column_ = "age", .kind_ = ruvia::redis_index_kind::numeric},
    },
};

template <typename command_type>
concept borrows_temporary_command = requires(command_type command) {
    std::move(command).arguments();
};
static_assert(!borrows_temporary_command<ruvia::redis_compiled_command>);
}  // namespace

TEST(Ruvia, redis_search_compilation_owns_configuration_and_binary_predicates) {
    ruvia::test::counting_memory_resource resource;
    auto command = [&] {
        std::string prefix = "offline:users";
        std::string column = "email";
        const ruvia::redis_repository_config config{
            .prefix_ = prefix,
            .indexes_ = {{.column_ = column, .kind_ = ruvia::redis_index_kind::tag, .sortable_ = true}},
        };
        const auto mapping = ruvia::redis_search_mapping::create<redis_query_user>(config, &resource);
        prefix.assign("changed");
        column.assign("age");
        std::string text("a|{ }\0", 6);
        std::string order = "email";
        ruvia::redis_find_options options{
            .where_ = redis_query_user::column<"email">() == text,
            .order_ = ruvia::redis_find_order{.column_ = order, .direction_ = ruvia::redis_order_direction::desc},
            .skip_ = 2,
            .take_ = 3,
        };
        text.assign("changed");
        return mapping.compile_find(options);
    }();
    const auto args = command.arguments();
    ASSERT_EQ(args.size(), std::size_t{11});
    EXPECT_EQ(args[1], "offline:users:idx");
    EXPECT_EQ(args[2], "@email:{x617c7b207d00}");
    EXPECT_EQ(args[4], "2");
    EXPECT_EQ(args[5], "3");
    EXPECT_EQ(args[7], "__ruvia_sort_email");
    EXPECT_EQ(args[8], "DESC");
    for (const auto& argument : args) {
        EXPECT_EQ(argument.get_allocator().resource(), &resource);
    }
}

TEST(Ruvia, redis_search_mapping_rejects_invalid_schema_and_query_fields_offline) {
    using mapping_type = ruvia::redis_search_mapping;
    for (const ruvia::redis_repository_config& config : {
             ruvia::redis_repository_config{.indexes_ = {{.column_ = "missing"}}},
             ruvia::redis_repository_config{.indexes_ = {{.column_ = "age", .kind_ = ruvia::redis_index_kind::tag}}},
             ruvia::redis_repository_config{.indexes_ = {{.column_ = "email"}, {.column_ = "email"}}}}) {
        EXPECT_THROW((void)mapping_type::create<redis_query_user>(config), std::invalid_argument);
    }
    const auto mapping = mapping_type::create<redis_query_user>(search_config);
    EXPECT_THROW((void)mapping.compile_find({.where_ = redis_query_user::column<"id">() == "id"}), std::invalid_argument);
    EXPECT_THROW((void)mapping.compile_find({.where_ = redis_signed_pk_entity::column<"id">() == 1}), std::invalid_argument);
    EXPECT_THROW((void)mapping.compile_find({.where_ = redis_query_user::column<"age">() == true}), std::invalid_argument);
    EXPECT_THROW((void)mapping.compile_find({.where_ = redis_query_user::column<"age">() == std::uint64_t{9007199254740992}}), std::invalid_argument);
    EXPECT_THROW((void)mapping.compile_find({.order_ = ruvia::redis_find_order{.column_ = "missing"}}, ruvia::redis_search_mode::count), std::invalid_argument);
    EXPECT_THROW((void)mapping.compile_find({.order_ = ruvia::redis_find_order{.column_ = "age"}}), std::invalid_argument);
    const auto no_indexes = mapping_type::create<redis_query_user>();
    EXPECT_THROW((void)no_indexes.compile_create_index(), std::invalid_argument);
}

TEST(Ruvia, redis_search_compilation_preserves_empty_tag_and_zero_first_page) {
    const auto mapping = ruvia::redis_search_mapping::create<redis_query_user>(search_config);
    const auto empty = mapping.compile_find({.where_ = redis_query_user::column<"email">() == ""});
    const auto missing = mapping.compile_find({.where_ = redis_query_user::column<"email">().is_null()});
    EXPECT_EQ(empty.arguments()[2], "@email:{x}");
    EXPECT_EQ(missing.arguments()[2], "-@__ruvia_present_email:{1}");
    const auto first = mapping.compile_find({.skip_ = 9, .take_ = 0}, ruvia::redis_search_mode::first);
    EXPECT_EQ(first.arguments()[4], "9");
    EXPECT_EQ(first.arguments()[5], "0");
    const auto count = mapping.compile_find({.skip_ = 9, .take_ = 4}, ruvia::redis_search_mode::count);
    EXPECT_EQ(count.arguments()[4], "0");
    EXPECT_EQ(count.arguments()[5], "0");
}

TEST(Ruvia, redis_search_mapping_and_commands_rebind_equivalent_resource_owners) {
    ruvia::test::counting_memory_resource upstream;
    ruvia::test::equivalent_tracking_resource source_resource(upstream);
    ruvia::test::equivalent_tracking_resource destination_resource(upstream);
    {
        auto destination = ruvia::redis_search_mapping::create<redis_query_user>({}, &destination_resource);
        auto command = destination.compile_drop_index();
        {
            auto source = ruvia::redis_search_mapping::create<redis_query_user>(search_config, &source_resource);
            const auto copy = source;
            EXPECT_EQ(copy.resource(), &source_resource);
            const auto copied_command = copy.compile_create_index();
            const auto command_copy = copied_command;
            EXPECT_EQ(command_copy.resource(), &source_resource);
            destination = source;
            destination = std::move(source);
            command = copied_command;
            auto moved_command = copy.compile_create_index();
            command = std::move(moved_command);
        }
        source_resource.retire();
        EXPECT_EQ(destination.resource(), &destination_resource);
        EXPECT_EQ(command.resource(), &destination_resource);
        const auto recreated = destination.compile_create_index();
        ASSERT_EQ(command.arguments().size(), recreated.arguments().size());
        for (std::size_t i = 0; i != command.arguments().size(); ++i) {
            EXPECT_EQ(command.arguments()[i], recreated.arguments()[i]);
            EXPECT_EQ(command.arguments()[i].get_allocator().resource(), &destination_resource);
        }
    }
    EXPECT_EQ(source_resource.callbacks_after_retirement(), std::size_t{0});
    EXPECT_EQ(upstream.live_allocations(), std::size_t{0});
}

TEST(Ruvia, redis_search_assignment_failure_preserves_destination_schema_and_command) {
    ruvia::test::counting_memory_resource resource;
    auto destination = ruvia::redis_search_mapping::create<redis_query_user>(
        {.prefix_ = "destination"}, &resource);
    auto command = destination.compile_drop_index();
    const auto source = ruvia::redis_search_mapping::create<redis_query_user>(search_config);
    const auto source_command = source.compile_create_index();
    resource.reject_allocations();
    EXPECT_THROW(destination = source, std::bad_alloc);
    EXPECT_THROW(command = source_command, std::bad_alloc);
    resource.reject_allocations(false);
    const auto retained = destination.compile_drop_index();
    EXPECT_EQ(retained.arguments()[1], "destination:idx");
    EXPECT_EQ(command.arguments()[1], "destination:idx");
}

TEST(Ruvia, redis_search_index_compilation_keeps_tag_shadows_and_documents) {
    const auto mapping = ruvia::redis_search_mapping::create<redis_query_user>({
        .prefix_ = "a:b",
        .indexes_ = {{.column_ = "email", .kind_ = ruvia::redis_index_kind::tag, .sortable_ = true}},
    });
    const auto create = mapping.compile_create_index();
    const std::string_view expected[]{
        "FT.CREATE",
        "a:b:idx",
        "ON",
        "HASH",
        "PREFIX",
        "1",
        "ruvia:orm:613a62:",
        "SCHEMA",
        "__ruvia_tag_email",
        "AS",
        "email",
        "TAG",
        "CASESENSITIVE",
        "__ruvia_present_email",
        "AS",
        "__ruvia_present_email",
        "TAG",
        "CASESENSITIVE",
        "email",
        "AS",
        "__ruvia_sort_email",
        "TAG",
        "SORTABLE",
        "NOINDEX",
    };
    ASSERT_EQ(create.arguments().size(), std::size(expected));
    for (std::size_t i = 0; i != std::size(expected); ++i) {
        EXPECT_EQ(create.arguments()[i], expected[i]);
    }
    const auto drop = mapping.compile_drop_index();
    ASSERT_EQ(drop.arguments().size(), std::size_t{2});
    EXPECT_EQ(drop.arguments()[0], "FT.DROPINDEX");
    EXPECT_EQ(drop.arguments()[1], "a:b:idx");
}
