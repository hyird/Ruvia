#pragma once

#include <span>
#include <utility>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/web/db/db_schema.h"

namespace ruvia::test {

// Runs one migration to completion on a fresh loop, whose blocking pool looks
// a DNS host up, and returns its report or rethrows its failure.
[[nodiscard]] inline db_migration_report run_db_migration(const db_config& config,
    std::span<const db_migration> migrations, const db_migrator_options& options = {},
    operation_options operation = {}) {
    blocking_pool host_lookups({.thread_count_ = 1});
    event_loop_pool loops({.loop_count_ = 1, .blocking_pool_ = &host_lookups});
    const db_migrator migrator(loops.loop(0), config, options);
    auto migration = migrator.migrate(migrations, std::move(operation));
    loops.start();
    auto run = loops.loop(0).start(std::move(migration));
    run.wait();
    loops.join();
    return std::move(run).get();
}

}  // namespace ruvia::test
