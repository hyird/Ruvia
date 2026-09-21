// Standalone PostgreSQL + Redis ORM on application-owned loops, without App.
// Read-only demo: use the orm example's orm_demo_device table (run its migration
// first). Redis HASH lookup needs ordinary Redis, not Redis Search.
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/EventLoopPool.h"
#include "ruvia/web/db/DbClient.h"
#include "ruvia/web/redis/RedisClient.h"

namespace {
using namespace ruvia;

RUVIA_DB_ENTITY(Device, "orm_demo_device",
    RUVIA_DB_COLUMN(id, std::int64_t, DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string))

RUVIA_REDIS_ENTITY(CachedDevice, "devices",
    RUVIA_REDIS_COLUMN(id, String, RedisColumnOptions{.primaryKey = true}),
    RUVIA_REDIS_COLUMN(name, String))

// Application composition, not a framework service registry. Each instance
// owns independent pools and memory; only its loop may use these clients.
struct WorkerData final {
    WorkerData(EventLoop owner, const DbConfig& sql, const RedisConfig& cache)
        : loop(std::move(owner)),
          db(loop, sql),
          redis(loop, cache) {}

    Task<void> connect() {
        co_await db.connect();
        co_await redis.connect();
    }

    Task<void> shutdown() {
        std::exception_ptr failure;
        try {
            co_await redis.shutdown();
        } catch (...) {
            failure = std::current_exception();
        }
        try {
            co_await db.shutdown();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    Task<void> read() {
        // Both operations explicitly use ORM; no raw SQL/Redis escape hatch.
        auto devices = db.getRepository<Device>();
        auto cached = redis.getRepository<CachedDevice>({.prefix = "ruvia:example:devices"});
        auto device = co_await devices.findOne({.where = Device::column<"id">() == 1});
        auto cache = co_await cached.findOne({.where = CachedDevice::column<"id">() == "1"});
        // Results stay in this coroutine on the owner loop and die before
        // shutdown. Do not return client-PMR objects to the main thread.
        if (device && cache) {
            (void)(device->get<"name">() == cache->get<"name">().view());
        }
    }

    EventLoop loop;
    DbClient db;
    RedisClient redis;
};

std::string setting(const char* name, const char* fallback) {
    const auto* value = std::getenv(name);
    return value == nullptr ? fallback : value;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 || std::string_view(argv[1]) != "--run") {
        std::cout << "Use --run after creating orm_demo_device with the orm example.\n"
                     "Configure RUVIA_DB_HOST/USER/PASSWORD/DATABASE and RUVIA_REDIS_HOST.\n";
        return 0;
    }
    try {
        DbConfig sql{.driver = DbDriver::kPostgreSql,
            .host = setting("RUVIA_DB_HOST", "127.0.0.1"),
            .port = 5432,
            .username = setting("RUVIA_DB_USER", "postgres"),
            .password = setting("RUVIA_DB_PASSWORD", ""),
            .database = setting("RUVIA_DB_DATABASE", "postgres")};
        RedisConfig cache{.host = setting("RUVIA_REDIS_HOST", "127.0.0.1"),
            .poolSizePerWorker = 1};
        EventLoopPool pool({.loopCount = 2});
        std::pmr::vector<std::unique_ptr<WorkerData>> workers;
        for (std::size_t index = 0; index < pool.loopCount(); ++index) {
            workers.push_back(std::make_unique<WorkerData>(pool.loop(index), sql, cache));
        }
        pool.start();
        std::exception_ptr failure;
        std::pmr::vector<RootTask<void>> tasks;
        auto observe = [&] {
            for (auto& task : tasks) {
                try {
                    task.get();
                } catch (...) {
                    if (!failure) {
                        failure = std::current_exception();
                    }
                    for (auto& worker : workers) {
                        worker->redis.close();
                        worker->db.close();
                    }
                }
            }
            tasks.clear();
        };
        try {
            for (auto& worker : workers) {
                tasks.push_back(worker->loop.start(worker->connect()));
            }
            // All clients must be ready before publishing any business work.
            observe();
            if (!failure) {
                for (auto& worker : workers) {
                    tasks.push_back(worker->loop.start(worker->read()));
                }
                observe();
            }
            // Stop business admission first; loop threads must remain running
            // until every client has cancelled and joined its operations.
            for (auto& worker : workers) {
                tasks.push_back(worker->loop.start(worker->shutdown()));
            }
            observe();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
            // Also covers failure while launching a root task. Loop stop hooks
            // close clients; join drains all previously accepted work.
            pool.stop();
            observe();
        }
        pool.stop();
        pool.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        std::cout << "Both workers completed SQL and Redis ORM lookups.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
