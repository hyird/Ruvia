#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/web/db/db_mapping.h"

#include "backend_client_fixture.h"
#include "test_io_context.h"

#ifdef RUVIA_ENABLE_POSTGRESQL
namespace {
using namespace ruvia;
RUVIA_DB_ENTITY(mapped_user, "users", db_column<"id", int>, db_column<"name", std::string>)

TEST(Ruvia, db_public_mapping_owns_values_and_rejects_missing_or_ambiguous_columns) {
    using peer_type = test::backend_startup_peer;
    const std::string name(128, 'n');
    std::vector<peer_type::postgres_reply> replies;
    replies.push_back({.columns_ = {"name", "id", "ignored"}, .values_ = {{name, "7", "extra"}}});
    replies.push_back({.column_ = "values", .rows_ = {R"({"comma,quote\"slash\\",NULL,"NULL"})"}});
    replies.push_back({.rows_ = {""}, .null_rows_ = {true}});
    replies.push_back({.column_ = "id", .rows_ = {"8"}});
    replies.push_back({.columns_ = {"id", "id", "name"}, .values_ = {{"1", "2", "ambiguous"}}});
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle, std::move(replies));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.db();
        std::optional<mapped_user> owned;
        {
            const auto rows = co_await handle.query("SELECT name, id, ignored FROM users");
            owned.emplace(decode_db_row<mapped_user>(rows.front()));
            const auto batch = decode_db_rows<mapped_user>(rows);
            EXPECT_EQ(batch[0].get<"id">(), 7);
            EXPECT_EQ(batch[0].get<"name">(), name);
        }
        EXPECT_EQ(owned->get<"id">(), 7);
        EXPECT_EQ(owned->get<"name">(), name);
        using array_type = std::vector<std::optional<std::string>>;
        array_type values;
        {
            const auto rows = co_await handle.query("SELECT values FROM arrays");
            values = decode_db_field<array_type>(rows.front()["values"]);
        }
        EXPECT_EQ(values.size(), std::size_t{3});
        if (values.size() == 3) {
            EXPECT_EQ(values[0], "comma,quote\"slash\\");
            EXPECT_FALSE(values[1]);
            EXPECT_EQ(values[2], "NULL");
        }
        {
            const auto rows = co_await handle.query("SELECT NULL AS v");
            EXPECT_FALSE(decode_db_field<std::optional<std::string>>(rows.front()["v"]));
            bool rejected = false;
            try {
                (void)decode_db_field<int>(rows.front()["v"]);
            } catch (const db_conversion_error& error) {
                rejected = error.code() == db_conversion_error::code_type::unexpected_null;
            }
            EXPECT_TRUE(rejected);
        }
        for (const auto expected : {db_conversion_error::code_type::missing_column,
                 db_conversion_error::code_type::invalid_format}) {
            const auto rows = co_await handle.query("SELECT * FROM invalid_mapping");
            bool rejected = false;
            try {
                (void)decode_db_row<mapped_user>(rows.front());
            } catch (const db_conversion_error& error) {
                rejected = error.code() == expected;
            }
            EXPECT_TRUE(rejected);
        }
    });
}
}  // namespace
#endif
