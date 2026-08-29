#include "test_harness.h"
#include "memory_resource_fixture.h"

#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <asio/io_context.hpp>

#include "ruvia/web/detail/client/HttpClientRegistry.h"
#include "ruvia/web/detail/integration/NamedCapability.h"

namespace {

using ruvia::test::CountingMemoryResource;
using ruvia::test::RejectingMemoryResource;

struct Entry final {
    std::string alias;
};

struct ConfigSource final {
    std::string value;
};

struct ConfigStorage final {
    ConfigStorage(const ConfigSource& source, std::pmr::memory_resource* resource)
        : value(source.value, resource) {}

    std::pmr::string value;
};

using Definition = ruvia::detail::NamedCapabilityDefinition<ConfigStorage>;

template <typename Index>
concept FindsAgainstExternalEntries = requires(
    const Index& index, const std::vector<Entry>& entries) { index.find(entries, "alias"); };

static_assert(!FindsAgainstExternalEntries<ruvia::detail::NamedCapabilityIndex>);
static_assert(!std::constructible_from<ruvia::detail::HttpClientPool, asio::io_context&,
    ruvia::WorkerHandle&&, ruvia::detail::HttpClientConfigStorage, std::pmr::memory_resource*>);
static_assert(!std::constructible_from<ruvia::detail::HttpClientRegistry, asio::io_context&,
    ruvia::WorkerHandle&&, std::pmr::memory_resource*, const ruvia::HttpClientConfig&>);
static_assert(!std::constructible_from<ruvia::detail::HttpClientRegistry, asio::io_context&,
    ruvia::WorkerHandle&&, std::pmr::memory_resource*,
    std::span<const ruvia::detail::HttpClientDefinition>>);

[[nodiscard]] std::string validationFailure(const std::vector<Entry>& entries) {
    try {
        ruvia::detail::validateCapabilityAliases(entries, "alias is empty", "alias is duplicated");
    } catch (const std::invalid_argument& error) {
        return std::string(error.what());
    }
    return {};
}

}  // namespace

RUVIA_TEST(named_capability_alias_validation_checks_the_complete_set_without_owner_state) {
    bool rejectedEmptyAlias = false;
    try {
        ruvia::detail::validateCapabilityAlias({}, "alias is empty");
    } catch (const std::invalid_argument& error) {
        rejectedEmptyAlias = std::string_view(error.what()) == "alias is empty";
    }
    RUVIA_CHECK(rejectedEmptyAlias);

    RUVIA_CHECK_EQ(
        validationFailure({{"first"}, {""}, {"third"}}), std::string_view("alias is empty"));
    RUVIA_CHECK_EQ(validationFailure({{"first"}, {"second"}, {"first"}}),
        std::string_view("alias is duplicated"));
    RUVIA_CHECK(validationFailure({{"first"}, {"second"}, {"third"}}).empty());
}

RUVIA_TEST(named_capability_upsert_validates_normalizes_and_preserves_alias_order) {
    RejectingMemoryResource rejectingResource;
    rejectingResource.rejectAllocations();
    std::pmr::vector<Definition> invalidDefinitions(std::pmr::new_delete_resource());
    bool rejectedBeforeNormalization = false;
    try {
        ruvia::detail::upsertNamedCapabilityDefinition(invalidDefinitions, {},
            ConfigSource{std::string(80, 'x')}, "alias is empty", &rejectingResource);
    } catch (const std::invalid_argument& error) {
        rejectedBeforeNormalization = std::string_view(error.what()) == "alias is empty";
    }
    RUVIA_CHECK(rejectedBeforeNormalization);
    RUVIA_CHECK_EQ(rejectingResource.allocationCount(), std::size_t{0});
    RUVIA_CHECK(invalidDefinitions.empty());

    std::pmr::unsynchronized_pool_resource resource;
    std::pmr::vector<Definition> definitions(&resource);
    ruvia::detail::upsertNamedCapabilityDefinition(
        definitions, "cache", ConfigSource{"first"}, "alias is empty", &resource);
    ruvia::detail::upsertNamedCapabilityDefinition(
        definitions, "events", ConfigSource{"second"}, "alias is empty", &resource);
    ruvia::detail::upsertNamedCapabilityDefinition(
        definitions, "cache", ConfigSource{"replacement"}, "alias is empty", &resource);

    RUVIA_CHECK_EQ(definitions.size(), std::size_t{2});
    RUVIA_CHECK_EQ(definitions[0].alias, std::string_view("cache"));
    RUVIA_CHECK_EQ(definitions[0].config.value, std::string_view("replacement"));
    RUVIA_CHECK_EQ(definitions[1].alias, std::string_view("events"));
    RUVIA_CHECK_EQ(definitions[1].config.value, std::string_view("second"));
    RUVIA_CHECK(definitions[0].alias.get_allocator().resource() == &resource);
    RUVIA_CHECK(definitions[0].config.value.get_allocator().resource() == &resource);
}

RUVIA_TEST(http_client_registry_rejects_alias_set_before_pool_allocation) {
    auto* sourceResource = std::pmr::new_delete_resource();
    const ruvia::HttpClientConfig config{
        .scheme = ruvia::HttpScheme::kHttp,
        .host = "example.test",
    };
    const ruvia::detail::HttpClientDefinition definitions[]{
        {std::pmr::string("duplicate", sourceResource),
            ruvia::detail::HttpClientConfigStorage(config, sourceResource)},
        {std::pmr::string("duplicate", sourceResource),
            ruvia::detail::HttpClientConfigStorage(config, sourceResource)},
    };
    asio::io_context ioContext;
    ruvia::WorkerHandle worker;
    CountingMemoryResource ownerResource;

    bool rejectedAsConfig = false;
    try {
        (void)ruvia::detail::HttpClientRegistry(ioContext, worker, &ownerResource, definitions);
    } catch (const std::invalid_argument& error) {
        rejectedAsConfig = std::string_view(error.what()) == "duplicate HTTP client alias";
    }

    RUVIA_CHECK(rejectedAsConfig);
    RUVIA_CHECK_EQ(ownerResource.allocationCount(), std::size_t{0});
}

RUVIA_TEST(named_capability_index_owns_aliases_and_preserves_registration_indices) {
    std::vector<Entry> entries{{"zeta"}, {"default"}, {"alpha"}, {"alpha-long"}};
    const auto entryCount = entries.size();
    ruvia::detail::NamedCapabilityIndex index(std::pmr::new_delete_resource());
    index.build(entries);
    for (auto& entry : entries) {
        entry.alias.assign(entry.alias.size(), 'x');
    }
    entries.clear();

    RUVIA_CHECK_EQ(index.find("zeta").value_or(entryCount), std::size_t{0});
    RUVIA_CHECK_EQ(index.defaultIndex().value_or(entryCount), std::size_t{1});
    RUVIA_CHECK_EQ(index.find("alpha").value_or(entryCount), std::size_t{2});
    RUVIA_CHECK_EQ(index.find("alpha-long").value_or(entryCount), std::size_t{3});
    RUVIA_CHECK(!index.find("alp").has_value());
    RUVIA_CHECK(!index.find("missing").has_value());
}

RUVIA_TEST(named_capability_index_rejects_rebuild_after_entry_set_is_finalized) {
    ruvia::detail::NamedCapabilityIndex index(std::pmr::new_delete_resource());
    const std::vector<Entry> initial{{"old"}};
    index.build(initial);
    RUVIA_CHECK(index.find("old").has_value());
    RUVIA_CHECK(!index.defaultIndex().has_value());

    const std::vector<Entry> replacement{{"second"}, {"first"}};
    bool rejected = false;
    try {
        index.build(replacement);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}
