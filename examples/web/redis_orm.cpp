// HASH CRUD works with ordinary Redis. Run once with --create-index against
// Redis Search before using GET /users. Index creation is explicit and errors
// if the index already exists; this example never drops existing data/indexes.
// Build with RUVIA_ENABLE_REDIS=ON; configure RUVIA_REDIS_HOST/PORT/USER/PASSWORD.
// Run on port 8091. POST {"id":"1","name":"Ada","age":30} as JSON to
// /users, then GET /users/1. GET /users performs the indexed adult search.
// backend_tls.h defines RUVIA_REDIS_TLS/CA/CERT/KEY for Redis transport.
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <string_view>
#include <utility>

#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/redis/redis_entity.h"
#include "ruvia/web/redis/redis_repository.h"

#include "backend_tls.h"

RUVIA_REDIS_ENTITY(cached_user, "users",
    RUVIA_REDIS_COLUMN(id, ruvia::string, ruvia::redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(name, ruvia::string),
    RUVIA_REDIS_COLUMN(age, std::uint32_t));

const ruvia::redis_repository_config user_redis_config{
    .prefix_ = "ruvia:example:users",
    .indexes_ = {
        {.field_ = "name", .kind_ = ruvia::redis_index_kind::tag, .sortable_ = true},
        {.field_ = "age", .kind_ = ruvia::redis_index_kind::numeric},
    },
};
RUVIA_MODEL(create_cached_user,
    RUVIA_REQUIRED_FIELD(id, ruvia::string, RUVIA_MIN(1, "id is required"),
        RUVIA_MAX(64, "id is too long")),
    RUVIA_REQUIRED_FIELD(name, ruvia::string, RUVIA_MIN(1, "name is required"),
        RUVIA_MAX(120, "name is too long")),
    RUVIA_REQUIRED_FIELD(age, ruvia::uint32, RUVIA_MAX(130, "age is too large")));

RUVIA_MODEL(cached_user_response,
    RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(age, ruvia::uint32));

RUVIA_MODEL(cached_users_response,
    RUVIA_REQUIRED_FIELD(users, ruvia::array<cached_user_response>));

class cached_user_controller final : public ruvia::controller<cached_user_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/users")
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("", create, ruvia::json_body<create_cached_user>);
    RUVIA_GET("", adults);
    RUVIA_GET("/:id", find);
    RUVIA_ROUTES_END

private:
    static void fill(cached_user_response& response, const cached_user& user_value) {
        response.set<"id">(user_value.get<"id">().view());
        response.set<"name">(user_value.get<"name">().view());
        response.set<"age">(ruvia::uint32{user_value.get<"age">()});
    }
    ruvia::task<ruvia::http_response> create(ruvia::context& c) {
        const auto& request = c.req().validated<create_cached_user>();
        cached_user user_value(c.pool());
        user_value.set<"id">(request.get<"id">().view());
        user_value.set<"name">(request.get<"name">().view());
        user_value.set<"age">(static_cast<std::uint32_t>(request.get<"age">()));
        auto users = c.redis().get_repository<cached_user>(user_redis_config);
        const auto result_value = co_await users.insert(user_value, {.ttl_ = std::chrono::hours(1)});
        if (result_value.affected_entities() == 0) {
            co_return c.error({.status_ = ruvia::http_status::conflict, .message_ = "user already exists"});
        }
        cached_user_response response({.resource_ = c.arena()});
        fill(response, user_value);
        c.status(ruvia::http_status::created);
        co_return c.json(response);
    }
    ruvia::task<ruvia::http_response> find(ruvia::context& c) {
        auto user_value = co_await c.redis().get_repository<cached_user>(user_redis_config).find_one({.where_ = cached_user::field<"id">() == c.req().param("id").value_or("")});
        if (!user_value) {
            co_return c.error({.status_ = ruvia::http_status::not_found, .message_ = "user not found"});
        }
        cached_user_response response({.resource_ = c.arena()});
        fill(response, *user_value);
        co_return c.json(response);
    }
    ruvia::task<ruvia::http_response> adults(ruvia::context& c) {
        const ruvia::redis_find_options find_options{
            .where_ = cached_user::field<"age">() >= 18,
            .order_ = {{.field_ = "name", .direction_ = ruvia::redis_order_direction::ascending}},
            .take_ = 20};
        auto users = co_await c.redis().get_repository<cached_user>(user_redis_config).find(find_options);
        cached_users_response response({.resource_ = c.arena()});
        auto& output = response.ensure<"users">();
        for (const auto& user : users) {
            auto& item = output.emplace_back(ruvia::model_options{.resource_ = c.arena()});
            fill(item, user);
        }
        co_return c.json(response);
    }
};

int main(int argc, char** argv) {
    const bool create_index = argc == 2 && std::string_view(argv[1]) == "--create-index";
    bool failed = false;
    auto& app = ruvia::app();
    app.load_dotenv();
    const example::environment env_value(&app.env());
    ruvia::redis_config config;
    config.tls_ = example::backend_tls("RUVIA_REDIS", env_value);
    config.host_ = env_value.get("RUVIA_REDIS_HOST").value_or("127.0.0.1");
    config.port_ = env_value.get<std::uint16_t>("RUVIA_REDIS_PORT").value_or(6379);
    config.username_ = env_value.get("RUVIA_REDIS_USER").value_or("");
    config.password_ = env_value.get("RUVIA_REDIS_PASSWORD").value_or("");
    app.redis({.config_ = config})
        .listen({.address_ = "127.0.0.1", .http_ = 8091})
        .server({.worker_count_ = 1, .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
        .on_start([create_index, &failed] {
            if (!create_index) {
                return;
            }
            const auto workers = ruvia::app().workers();
            std::promise<void> completion;
            auto result_value = completion.get_future();
            const auto posted = workers.front().post([completion = std::move(completion)](ruvia::web_worker_context& worker_value) mutable -> ruvia::task<void> {
                try {
                    co_await worker_value.redis().get_repository<cached_user>(user_redis_config).create_index();
                    completion.set_value();
                } catch (...) {
                    completion.set_exception(std::current_exception());
                }
            });
            if (posted != ruvia::post_status::accepted) {
                failed = true;
            } else {
                // Only the lifecycle caller blocks. Workers are ready before
                // on_start; request admission stays closed until this returns.
                // Abandoning an unstarted job destroys its promise and wakes get().
                try {
                    result_value.get();
                    std::cout << "Redis user index created.\n";
                } catch (const std::exception& error) {
                    std::cerr << error.what() << '\n';
                    failed = true;
                }
            }
            // --create-index is a one-shot operation, never a serving mode.
            ruvia::app().stop();
        })
        .run();
    return failed ? 1 : 0;
}
