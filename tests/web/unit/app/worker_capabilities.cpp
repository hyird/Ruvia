#include "integration/worker_capabilities.h"

#include <concepts>
#include <memory>
#include <type_traits>

#include <asio/io_context.hpp>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/http/http_limits.h"

#include "context/context_services.h"
#include "test_harness.h"
#include "test_io_context.h"

RUVIA_TEST(worker_capabilities_exposes_one_address_stable_capability_graph) {
    auto& io_context = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 64});
    const auto worker_value = attachment.loop().handle();
    ruvia::worker_memory memory;
    ruvia::detail::worker_capabilities capabilities(io_context, worker_value, memory.resource(), {}, {});
    const ruvia::stop_token stop_token;

    capabilities.initialize_worker_state();
    const auto services = capabilities.make_context_services(stop_token);
    const auto owned_clients = capabilities.client_registries();
    const auto request_clients = services.client_registries();

    RUVIA_CHECK(request_clients.attached());
    RUVIA_CHECK(request_clients == owned_clients);
    RUVIA_CHECK(!ruvia::detail::worker_client_registry_view::detached().attached());
    RUVIA_CHECK(services.worker_states() == &capabilities.worker_states());
    RUVIA_CHECK(services.rate_limiter() == &capabilities.rate_limiter());
    RUVIA_CHECK(&services.worker() == &worker_value);
    RUVIA_CHECK(&services.get_stop_token() == &stop_token);
    RUVIA_CHECK_EQ(services.max_decoded_body_bytes(), ruvia::default_max_buffered_body_bytes);
    RUVIA_CHECK(services.get_blocking_pool() == nullptr);

    capabilities.close_now();
    capabilities.shutdown_worker_state();
}
