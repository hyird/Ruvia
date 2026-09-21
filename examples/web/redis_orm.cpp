// HASH CRUD works with ordinary Redis. Run once with --create-index against
// Redis Search before using GET /users. Index creation is explicit and errors
// if the index already exists; this example never drops existing data/indexes.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/redis/RedisEntity.h"
#include "ruvia/web/redis/RedisRepository.h"

RUVIA_REDIS_ENTITY(CachedUser, "users",
    RUVIA_REDIS_COLUMN(id, ruvia::String, ruvia::RedisColumnOptions{.primaryKey = true}),
    RUVIA_REDIS_COLUMN(name, ruvia::String),
    RUVIA_REDIS_COLUMN(age, std::uint32_t));

const ruvia::RedisRepositoryConfig userRedisConfig{
    .prefix = "ruvia:example:users",
    .indexes = {
        {.column = "name", .kind = ruvia::RedisIndexKind::kTag, .sortable = true},
        {.column = "age", .kind = ruvia::RedisIndexKind::kNumeric},
    },
};
RUVIA_MODEL(CreateCachedUser,
    RUVIA_REQUIRED_FIELD(id, ruvia::String, RUVIA_MIN(1, "id is required"),
        RUVIA_MAX(64, "id is too long")),
    RUVIA_REQUIRED_FIELD(name, ruvia::String, RUVIA_MIN(1, "name is required"),
        RUVIA_MAX(120, "name is too long")),
    RUVIA_REQUIRED_FIELD(age, ruvia::UInt32, RUVIA_MAX(130, "age is too large")));

RUVIA_MODEL(CachedUserResponse,
    RUVIA_REQUIRED_FIELD(id, ruvia::String),
    RUVIA_REQUIRED_FIELD(name, ruvia::String),
    RUVIA_REQUIRED_FIELD(age, ruvia::UInt32));

RUVIA_MODEL(CachedUsersResponse,
    RUVIA_REQUIRED_FIELD(users, ruvia::Array<CachedUserResponse>));

class CachedUserController final : public ruvia::Controller<CachedUserController> {
public:
    RUVIA_CONTROLLER_GROUP("/users")
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("", create, ruvia::JsonBody<CreateCachedUser>);
    RUVIA_GET("", adults);
    RUVIA_GET("/:id", find);
    RUVIA_ROUTES_END

private:
    static void fill(CachedUserResponse& response, const CachedUser& user) {
        response.set<"id">(user.get<"id">().view());
        response.set<"name">(user.get<"name">().view());
        response.set<"age">(ruvia::UInt32{user.get<"age">()});
    }
    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& c) {
        const auto& request = c.req().validated<CreateCachedUser>();
        CachedUser user(c.pool());
        user.set<"id">(request.get<"id">().view());
        user.set<"name">(request.get<"name">().view());
        user.set<"age">(static_cast<std::uint32_t>(request.get<"age">()));
        auto users = c.redis().getRepository<CachedUser>(userRedisConfig);
        const auto result = co_await users.insert(user, {.ttl = std::chrono::hours(1)});
        if (result.affectedRows() == 0) {
            co_return c.error({.status = ruvia::http_status::kConflict, .message = "user already exists"});
        }
        CachedUserResponse response({.resource = c.arena()});
        fill(response, user);
        c.status(ruvia::http_status::kCreated);
        co_return c.json(response);
    }
    ruvia::Task<ruvia::HttpResponse> find(ruvia::Context& c) {
        auto user = co_await c.redis().getRepository<CachedUser>(userRedisConfig).findOne({.where = CachedUser::column<"id">() == c.req().param("id").value_or("")});
        if (!user) {
            co_return c.error({.status = ruvia::http_status::kNotFound, .message = "user not found"});
        }
        CachedUserResponse response({.resource = c.arena()});
        fill(response, *user);
        co_return c.json(response);
    }
    ruvia::Task<ruvia::HttpResponse> adults(ruvia::Context& c) {
        const ruvia::DbFindOptions findOptions{
            .where = CachedUser::column<"age">() >= 18,
            .order = {{.column = "name", .direction = ruvia::DbOrderDirection::kAsc}},
            .take = 20};
        auto users = co_await c.redis().getRepository<CachedUser>(userRedisConfig).find(findOptions);
        CachedUsersResponse response({.resource = c.arena()});
        auto& output = response.ensure<"users">();
        for (const auto& user : users) {
            auto& item = output.emplace_back(ruvia::ModelOptions{.resource = c.arena()});
            fill(item, user);
        }
        co_return c.json(response);
    }
};

int main(int argc, char** argv) {
    const bool createIndex = argc == 2 && std::string_view(argv[1]) == "--create-index";
    std::atomic<bool> failed{false};
    auto& app = ruvia::app();
    app.loadDotenv();
    ruvia::RedisConfig config;
    config.host = app.env().get("RUVIA_REDIS_HOST").value_or("127.0.0.1");
    config.port = app.env().get<std::uint16_t>("RUVIA_REDIS_PORT").value_or(6379);
    config.username = app.env().get("RUVIA_REDIS_USER").value_or("");
    config.password = app.env().get("RUVIA_REDIS_PASSWORD").value_or("");
    app.redis({.config = config})
        .listen({.address = "127.0.0.1", .http = 8091})
        .server({.workerCount = 1, .processSignalHandlers = ruvia::ProcessSignalHandlerPolicy::kInstall})
        .onStart([createIndex, &failed] {
            if (!createIndex) {
                return;
            }
            const auto workers = ruvia::app().workers();
            const auto posted = workers.front().post([&failed](ruvia::WebWorkerContext& worker) -> ruvia::Task<void> {
                try {
                    co_await worker.redis().getRepository<CachedUser>(userRedisConfig).createIndex();
                    std::cout << "Redis user index created.\n";
                } catch (const std::exception& error) {
                    std::cerr << error.what() << '\n';
                    failed.store(true);
                }
                ruvia::app().stop();
            });
            if (posted != ruvia::PostStatus::kAccepted) {
                failed.store(true);
                ruvia::app().stop();
            }
        })
        .run();
    return failed.load() ? 1 : 0;
}
