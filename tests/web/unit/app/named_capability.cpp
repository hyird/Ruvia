#include "integration/named_capability.h"

#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <asio/io_context.hpp>

#include "client/http_client_registry.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::test::counting_memory_resource;
using ruvia::test::rejecting_memory_resource;

struct entry final {
    std::string alias_;
};

struct config_source final {
    std::string value_;
};

struct config_storage final {
    config_storage(const config_source& source_value, std::pmr::memory_resource* resource)
        : value_(source_value.value_, resource) {}

    std::pmr::string value_;
};

using definition_type = ruvia::detail::named_capability_definition<config_storage>;

[[nodiscard]] std::string validation_failure(const std::vector<entry>& entries) {
    try {
        ruvia::detail::validate_capability_aliases(entries, "alias is empty", "alias is duplicated");
    } catch (const std::invalid_argument& error) {
        return std::string(error.what());
    }
    return {};
}

}  // namespace

RUVIA_TEST(named_capability_alias_validation_checks_the_complete_set_without_owner_state) {
    bool rejected_empty_alias = false;
    try {
        ruvia::detail::validate_capability_alias({}, "alias is empty");
    } catch (const std::invalid_argument& error) {
        rejected_empty_alias = std::string_view(error.what()) == "alias is empty";
    }
    RUVIA_CHECK(rejected_empty_alias);

    RUVIA_CHECK_EQ(
        validation_failure({{"first"}, {""}, {"third"}}), std::string_view("alias is empty"));
    RUVIA_CHECK_EQ(validation_failure({{"first"}, {"second"}, {"first"}}),
        std::string_view("alias is duplicated"));
    RUVIA_CHECK(validation_failure({{"first"}, {"second"}, {"third"}}).empty());
}

RUVIA_TEST(named_capability_upsert_validates_normalizes_and_preserves_alias_order) {
    rejecting_memory_resource rejecting_resource;
    rejecting_resource.reject_allocations();
    std::pmr::vector<definition_type> invalid_definitions(std::pmr::new_delete_resource());
    bool rejected_before_normalization = false;
    try {
        ruvia::detail::upsert_named_capability_definition(invalid_definitions, {},
            config_source{std::string(80, 'x')}, "alias is empty", &rejecting_resource);
    } catch (const std::invalid_argument& error) {
        rejected_before_normalization = std::string_view(error.what()) == "alias is empty";
    }
    RUVIA_CHECK(rejected_before_normalization);
    RUVIA_CHECK_EQ(rejecting_resource.allocation_count(), std::size_t{0});
    RUVIA_CHECK(invalid_definitions.empty());

    std::pmr::unsynchronized_pool_resource resource;
    std::pmr::vector<definition_type> definitions(&resource);
    ruvia::detail::upsert_named_capability_definition(
        definitions, "cache", config_source{"first"}, "alias is empty", &resource);
    ruvia::detail::upsert_named_capability_definition(
        definitions, "events", config_source{"second"}, "alias is empty", &resource);
    ruvia::detail::upsert_named_capability_definition(
        definitions, "cache", config_source{"replacement"}, "alias is empty", &resource);

    RUVIA_CHECK_EQ(definitions.size(), std::size_t{2});
    RUVIA_CHECK_EQ(definitions[0].alias_, std::string_view("cache"));
    RUVIA_CHECK_EQ(definitions[0].config_.value_, std::string_view("replacement"));
    RUVIA_CHECK_EQ(definitions[1].alias_, std::string_view("events"));
    RUVIA_CHECK_EQ(definitions[1].config_.value_, std::string_view("second"));
    RUVIA_CHECK(definitions[0].alias_.get_allocator().resource() == &resource);
    RUVIA_CHECK(definitions[0].config_.value_.get_allocator().resource() == &resource);
}

RUVIA_TEST(http_client_registry_rejects_alias_set_before_pool_allocation) {
    auto* source_resource = std::pmr::new_delete_resource();
    const ruvia::http_client_config config{
        .scheme_ = ruvia::http_scheme::http,
        .host_ = "example.test",
    };
    const ruvia::detail::http_client_definition_type definitions[]{
        {std::pmr::string("duplicate", source_resource),
            ruvia::detail::http_client_config_storage(config, source_resource)},
        {std::pmr::string("duplicate", source_resource),
            ruvia::detail::http_client_config_storage(config, source_resource)},
    };
    asio::io_context io_context;
    ruvia::worker_handle worker;
    counting_memory_resource owner_resource;

    bool rejected_as_config = false;
    try {
        (void)ruvia::detail::http_client_registry(io_context, worker, &owner_resource, definitions);
    } catch (const std::invalid_argument& error) {
        rejected_as_config = std::string_view(error.what()) == "duplicate HTTP client alias";
    }

    RUVIA_CHECK(rejected_as_config);
    RUVIA_CHECK_EQ(owner_resource.allocation_count(), std::size_t{0});
}

RUVIA_TEST(named_capability_index_owns_aliases_and_preserves_registration_indices) {
    std::vector<entry> entries{{"zeta"}, {"default"}, {"alpha"}, {"alpha-long"}};
    const auto entry_count = entries.size();
    ruvia::detail::named_capability_index index(std::pmr::new_delete_resource());
    index.build(entries);
    for (auto& entry : entries) {
        entry.alias_.assign(entry.alias_.size(), 'x');
    }
    entries.clear();

    RUVIA_CHECK_EQ(index.find("zeta").value_or(entry_count), std::size_t{0});
    RUVIA_CHECK_EQ(index.default_index().value_or(entry_count), std::size_t{1});
    RUVIA_CHECK_EQ(index.find("alpha").value_or(entry_count), std::size_t{2});
    RUVIA_CHECK_EQ(index.find("alpha-long").value_or(entry_count), std::size_t{3});
    RUVIA_CHECK(!index.find("alp").has_value());
    RUVIA_CHECK(!index.find("missing").has_value());
}

RUVIA_TEST(named_capability_index_rejects_rebuild_after_entry_set_is_finalized) {
    ruvia::detail::named_capability_index index(std::pmr::new_delete_resource());
    const std::vector<entry> initial_value{{"old"}};
    index.build(initial_value);
    RUVIA_CHECK(index.find("old").has_value());
    RUVIA_CHECK(!index.default_index().has_value());

    const std::vector<entry> replacement{{"second"}, {"first"}};
    bool rejected = false;
    try {
        index.build(replacement);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}
