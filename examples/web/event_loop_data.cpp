// Standalone PostgreSQL + Redis ORM on application-owned loops, without application.
// Read-only demo: use the orm example's orm_demo_device table (run its migration
// first). Redis HASH lookup needs ordinary Redis, not Redis Search.
// Build with RUVIA_ENABLE_POSTGRESQL=ON and RUVIA_ENABLE_REDIS=ON.
// Configure the RUVIA_DB_* and RUVIA_REDIS_HOST variables used in main().
// Each service is constructed and shut down on its own loop. Request data
// may be borrowed only while that loop and its resource owners remain alive.
// backend_tls.h defines RUVIA_DB_TLS and RUVIA_REDIS_TLS plus CA/CERT/KEY.
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/event_loop_pool.h"
#include "ruvia/web/db/db_client.h"
#include "ruvia/web/redis/redis_client.h"

#include "backend_tls.h"

namespace {
using namespace ruvia;

RUVIA_DB_ENTITY(device, "orm_demo_device",
    RUVIA_DB_COLUMN(id, std::int64_t, db_column_options{.primary_key_ = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string))

RUVIA_REDIS_ENTITY(cached_device, "devices",
    RUVIA_REDIS_COLUMN(id, string, redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(name, string))

// Application composition, not a framework service registry. Each instance
// owns independent pools and memory; only its loop may use these clients.
struct worker_data final {
    worker_data(event_loop owner_value, const db_config& sql, const redis_config& cache)
        : loop_(std::move(owner_value)),
          db_(loop_, sql),
          redis_(loop_, cache) {}

    task<void> connect() {
        co_await db_.connect();
        co_await redis_.connect();
    }

    task<void> shutdown() {
        std::exception_ptr failure;
        try {
            co_await redis_.shutdown();
        } catch (...) {
            failure = std::current_exception();
        }
        try {
            co_await db_.shutdown();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    task<void> read() {
        // Both operations explicitly use ORM; no raw SQL/Redis escape hatch.
        auto devices = db_.get_repository<device>();
        auto cached = redis_.get_repository<cached_device>({.prefix_ = "ruvia:example:devices"});
        auto device = co_await devices.find_one({.where_ = device::column<"id">() == 1});
        auto cache = co_await cached.find_one({.where_ = cached_device::field<"id">() == "1"});
        // Results stay in this coroutine on the owner loop and die before
        // shutdown. Do not return client-PMR objects to the main thread.
        if (device && cache) {
            (void)(device->get<"name">() == cache->get<"name">().view());
        }
    }

    event_loop loop_;
    db_client db_;
    redis_client redis_;
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
        const example::environment env;
        db_config sql{.driver_ = db_driver::postgresql,
            .host_ = setting("RUVIA_DB_HOST", "127.0.0.1"),
            .port_ = env.get<std::uint16_t>("RUVIA_DB_PORT").value_or(5432),
            .username_ = setting("RUVIA_DB_USER", "postgres"),
            .password_ = setting("RUVIA_DB_PASSWORD", ""),
            .tls_ = example::backend_tls("RUVIA_DB"),
            .database_ = setting("RUVIA_DB_DATABASE", "postgres")};
        redis_config cache{.host_ = setting("RUVIA_REDIS_HOST", "127.0.0.1"),
            .port_ = env.get<std::uint16_t>("RUVIA_REDIS_PORT").value_or(6379),
            .tls_ = example::backend_tls("RUVIA_REDIS"),
            .pool_size_per_worker_ = 1};
        event_loop_pool pool({.loop_count_ = 2});
        std::pmr::vector<std::unique_ptr<worker_data>> workers;
        for (std::size_t index = 0; index < pool.loop_count(); ++index) {
            workers.push_back(std::make_unique<worker_data>(pool.loop(index), sql, cache));
        }
        pool.start();
        std::exception_ptr failure;
        std::pmr::vector<root_task<void>> tasks;
        auto observe = [&] {
            for (auto& task : tasks) {
                try {
                    task.get();
                } catch (...) {
                    if (!failure) {
                        failure = std::current_exception();
                    }
                    for (auto& worker : workers) {
                        worker->redis_.close();
                        worker->db_.close();
                    }
                }
            }
            tasks.clear();
        };
        try {
            for (auto& worker : workers) {
                tasks.push_back(worker->loop_.start(worker->connect()));
            }
            // All clients must be ready before publishing any business work.
            observe();
            if (!failure) {
                for (auto& worker : workers) {
                    tasks.push_back(worker->loop_.start(worker->read()));
                }
                observe();
            }
            // Stop business admission first; loop threads must remain running
            // until every client has cancelled and joined its operations.
            for (auto& worker : workers) {
                tasks.push_back(worker->loop_.start(worker->shutdown()));
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
