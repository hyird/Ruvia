#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/web/db/db_handle.h"

#include "client/client_lifecycle.h"
#include "db/db_registry.h"

namespace ruvia::detail {

class db_client_state final : public std::enable_shared_from_this<db_client_state> {
public:
    db_client_state(event_loop loop, const db_config& config);
#ifdef RUVIA_ENABLE_REDIS
    db_client_state(event_loop loop, const db_config& config, const redis_handle& cache_store,
        const db_cache_config& cache_policy);
#endif

    db_client_state(const db_client_state&) = delete;
    db_client_state& operator=(const db_client_state&) = delete;

    void bind_stop() {
        lifecycle_.bind_stop();
    }
    [[nodiscard]] task<void> connect() {
        return lifecycle_.connect();
    }
    void request_close() noexcept {
        lifecycle_.request_close();
    }
    [[nodiscard]] task<void> shutdown() {
        return lifecycle_.shutdown();
    }
    [[nodiscard]] db_handle handle(operation_options options);

    [[nodiscard]] const worker_handle& worker() const noexcept {
        return worker_;
    }

private:
    friend class client_lifecycle<db_client_state>;
    static constexpr std::string_view client_name{"database"};
    [[noreturn]] static void throw_not_ready();
    [[nodiscard]] db_registry& backend() noexcept {
        return databases_;
    }

    event_loop loop_;
    worker_handle worker_;
    worker_memory memory_;
    db_registry databases_;
    // Retires scopes before backend storage and its allocator are destroyed.
    client_lifecycle<db_client_state> lifecycle_;
};
db_client_state::db_client_state(event_loop loop, const db_config& config)
    : loop_(client_lifecycle<db_client_state>::require_loop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      databases_(loop_.io_context(), worker_, memory_.resource(), config),
      lifecycle_(*this, loop_, worker_) {}

#ifdef RUVIA_ENABLE_REDIS
db_client_state::db_client_state(event_loop loop, const db_config& config,
    const redis_handle& cache_store, const db_cache_config& cache_policy)
    : loop_(client_lifecycle<db_client_state>::require_loop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      databases_(loop_.io_context(), worker_, memory_.resource(), config, cache_store, cache_policy),
      lifecycle_(*this, loop_, worker_) {}
#endif

void db_client_state::throw_not_ready() {
    throw std::logic_error("database client is not connected");
}

db_handle db_client_state::handle(operation_options options) {
    lifecycle_.require_ready();
    return databases_.get(lifecycle_.operation_scope(), nullptr).with_options(std::move(options));
}

}  // namespace ruvia::detail

namespace ruvia {

db_client::db_client(event_loop loop, const db_config& config)
    : state_(std::allocate_shared<detail::db_client_state>(
          std::pmr::polymorphic_allocator<detail::db_client_state>(process_resource()),
          std::move(loop), config)) {
    state_->bind_stop();
}

#ifdef RUVIA_ENABLE_REDIS
db_client::db_client(event_loop loop, const db_config& config,
    const redis_handle& cache_store, const db_cache_config& cache_policy)
    : state_(std::allocate_shared<detail::db_client_state>(
          std::pmr::polymorphic_allocator<detail::db_client_state>(process_resource()),
          std::move(loop), config, cache_store, cache_policy)) {
    state_->bind_stop();
}
#endif

db_client::~db_client() {
    state_->request_close();
}

task<void> db_client::connect() & {
    return state_->connect();
}

db_handle db_client::handle(operation_options options) const& {
    return state_->handle(std::move(options));
}

void db_client::close() noexcept {
    state_->request_close();
}

task<void> db_client::shutdown() & {
    return state_->shutdown();
}

const worker_handle& db_client::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
