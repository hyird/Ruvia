#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_client_types.h"

#include "client/http_client_config_storage.h"
#include "context/context_access.h"
#include "context_services_fixture.h"
#include "integration/worker_capabilities.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "websocket/websocket_access.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db.h"
#include "ruvia/web/db/db_types.h"
#include "ruvia/web/detail/db/db_result_access.h"

#include "db/db_config_storage.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis.h"
#include "ruvia/web/redis/redis_types.h"

#include "redis/redis_config_storage.h"
#include "redis/redis_types_access.h"
#endif

#include <array>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <asio/io_context.hpp>

namespace {

template <typename fn_type>
void check_operation_does_not_use_request_arena(
    ruvia::testing::test_context& ruvia_ctx, ruvia::request_memory& memory, fn_type&& fn) {
    auto* const before = static_cast<std::byte*>(memory.resource()->allocate(1, 1));
    fn();
    auto* const after = static_cast<std::byte*>(memory.resource()->allocate(1, 1));
    RUVIA_CHECK(after == before + 1);
}

ruvia::task<std::optional<ruvia::websocket_message>> read_websocket(void*) {
    co_return std::nullopt;
}

ruvia::task<void> write_websocket(void*, ruvia::websocket_opcode, std::string_view, bool) {
    co_return;
}

ruvia::task<void> close_websocket(void*, ruvia::websocket_close_options) {
    co_return;
}

struct context_fixture final {
    context_fixture()
        : worker_(),
          request_memory_(worker_, std::span<std::byte>(request_buffer_)),
          request_(make_request(request_memory_)),
#ifdef RUVIA_ENABLE_DATABASE
          db_definitions_{make_db_definition("default"), make_db_definition("reporting")},
#endif
#ifdef RUVIA_ENABLE_REDIS
          redis_definitions_{make_redis_definition("default"), make_redis_definition("cache")},
#endif
          http_definitions_{make_http_definition("default"), make_http_definition("upstream")},
          capabilities_(io_context_, ruvia::test::test_worker_handle(), worker_.resource(), definitions(),
              {}) {
    }

    [[nodiscard]] ruvia::detail::worker_capability_definitions definitions() {
        return {
#ifdef RUVIA_ENABLE_DATABASE
            .databases_ = db_definitions_,
#endif
#ifdef RUVIA_ENABLE_REDIS
            .redis_ = redis_definitions_,
#endif
            .http_clients_ = http_definitions_,
        };
    }

#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] static ruvia::db_config database_config() {
#ifdef RUVIA_ENABLE_MARIADB
        return ruvia::db_config{.driver_ = ruvia::db_driver::mariadb};
#else
        return ruvia::db_config{.driver_ = ruvia::db_driver::postgresql};
#endif
    }
#endif

    [[nodiscard]] static ruvia::http_request make_request(ruvia::request_memory& memory) {
        auto [request, error] = ruvia::make_parsed_http_request("GET", "/", {}, {}, memory.resource());
        if (error) {
            throw std::runtime_error("invalid test request");
        }
        return std::move(request);
    }

    [[nodiscard]] static ruvia::http_client_config http_config() {
        return ruvia::http_client_config{
            .scheme_ = ruvia::http_scheme::http,
            .host_ = "127.0.0.1",
            .port_ = 1,
        };
    }

#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] ruvia::detail::db_definition make_db_definition(std::string_view alias) {
        return {std::pmr::string(alias, worker_.resource()),
            ruvia::detail::db_config_storage(database_config(), worker_.resource())};
    }
#endif

#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] ruvia::detail::redis_definition_type make_redis_definition(std::string_view alias) {
        return {std::pmr::string(alias, worker_.resource()),
            ruvia::detail::redis_config_storage({}, worker_.resource())};
    }
#endif

    [[nodiscard]] ruvia::detail::http_client_definition_type make_http_definition(
        std::string_view alias) {
        return {std::pmr::string(alias, worker_.resource()),
            ruvia::detail::http_client_config_storage(http_config(), worker_.resource())};
    }

    asio::io_context io_context_;
    ruvia::worker_memory worker_;
    std::array<std::byte, 64 * 1024> request_buffer_{};
    ruvia::request_memory request_memory_;
    ruvia::http_request request_;
    ruvia::stop_token stop_token_;
#ifdef RUVIA_ENABLE_DATABASE
    std::array<ruvia::detail::db_definition, 2> db_definitions_;
#endif
#ifdef RUVIA_ENABLE_REDIS
    std::array<ruvia::detail::redis_definition_type, 2> redis_definitions_;
#endif
    std::array<ruvia::detail::http_client_definition_type, 2> http_definitions_;
    ruvia::detail::worker_capabilities capabilities_;
};

}  // namespace

RUVIA_TEST(context_response_scratch_does_not_remain_in_request_arena) {
    ruvia::worker_memory worker;
    const auto measure = [&](auto&& make_response, std::string_view header_value) {
        alignas(std::max_align_t) std::array<std::byte, 64 * 1024> buffer;
        ruvia::request_memory memory(worker, buffer);
        auto [request, error] = ruvia::make_parsed_http_request("GET", "/", {}, {}, memory.resource());
        if (error) {
            throw std::runtime_error("invalid test request");
        }
        auto context_value = ruvia::detail::context_access::make(
            memory, request, ruvia::test::test_context_services());
        const auto response = make_response(context_value);
        const auto* end = static_cast<std::byte*>(memory.resource()->allocate(1, 1));
        return std::pair{end - buffer.data(), std::string(response.header(header_value).value())};
    };

    const std::string location = "/" + std::string(1024, ' ');
    std::string encoded_location = "/";
    for (int index = 0; index != 1024; ++index) {
        encoded_location += "%20";
    }
    const auto encoded = measure([&](ruvia::context& context_value) {
        return context_value.redirect({.location_ = location});
    },
        "Location");
    const auto verbatim = measure([&](ruvia::context& context_value) {
        return context_value.redirect({.location_ = encoded_location});
    },
        "Location");
    RUVIA_CHECK_EQ(encoded.second, encoded_location);
    RUVIA_CHECK_EQ(encoded.second, verbatim.second);
    RUVIA_CHECK_EQ(encoded.first, verbatim.first);

    const std::string name(128, 'n');
    const std::string value(1024, 'v');
    const ruvia::cookie_options attributes{
        .path_ = "/",
        .prefix_ = ruvia::cookie_prefix::host,
        .secure_ = ruvia::cookie_attribute_policy::emit,
    };
    const auto signed_cookie = measure([&](ruvia::context& context_value) {
        context_value.set_signed_cookie({.name_ = name, .value_ = value, .secret_ = "response-scratch-test-secret", .attributes_ = attributes});
        return context_value.text("ok");
    },
        "Set-Cookie");
    const auto value_start = signed_cookie.second.find('=') + 1;
    const auto value_end = signed_cookie.second.find(';', value_start);
    const auto signed_value = std::string_view(signed_cookie.second).substr(value_start, value_end - value_start);
    const auto plain_cookie = measure([&](ruvia::context& context_value) {
        context_value.set_cookie({.name_ = name, .value_ = signed_value, .attributes_ = attributes});
        return context_value.text("ok");
    },
        "Set-Cookie");
    RUVIA_CHECK_EQ(signed_cookie.second, plain_cookie.second);
    RUVIA_CHECK_EQ(signed_cookie.first, plain_cookie.first);
}

RUVIA_TEST(context_operation_clients_keep_parameters_out_of_request_arena) {
    context_fixture fixture;
    auto context_value = ruvia::detail::context_access::make(
        fixture.request_memory_, fixture.request_,
        fixture.capabilities_.make_context_services(fixture.stop_token_));

    RUVIA_CHECK(context_value.arena() == fixture.request_memory_.resource());
    RUVIA_CHECK(context_value.pool() == fixture.worker_.resource());

    std::pmr::string handshake("websocket-handshake", fixture.request_memory_.allocator<char>());
    std::pmr::string retained_operation_value(2048, 'r', context_value.pool());
    const std::string expected_operation_value(2048, 'r');
#ifdef RUVIA_ENABLE_DATABASE
    auto database_value = context_value.db();
#endif
#ifdef RUVIA_ENABLE_REDIS
    auto redis = context_value.redis();
#endif
    auto http_client_value = context_value.get_http_client();
    auto websocket_value = ruvia::detail::websocket_access::make(
        *context_value.pool(), nullptr, &read_websocket, &write_websocket, &close_websocket);

    for (int index = 0; index != 2000; ++index) {
        const std::string value(2048, static_cast<char>('a' + index % 26));
        const std::string target = "/" + value;
        std::pmr::string transient_operation_value(value, context_value.pool());
        RUVIA_CHECK_EQ(handshake, std::string_view("websocket-handshake"));
        RUVIA_CHECK_EQ(retained_operation_value, std::string_view(expected_operation_value));
#ifdef RUVIA_ENABLE_DATABASE
        check_operation_does_not_use_request_arena(ruvia_ctx, fixture.request_memory_, [&] {
            auto operation = database_value.query("SELECT ?", value);
            static_cast<void>(operation);
        });
        check_operation_does_not_use_request_arena(ruvia_ctx, fixture.request_memory_, [&] {
            auto operation = context_value.db("reporting").execute("UPDATE items SET value=?", value);
            static_cast<void>(operation);
        });
        check_operation_does_not_use_request_arena(ruvia_ctx, fixture.request_memory_, [&] {
            auto operation = context_value.db().query_stream("SELECT ?", value);
            static_cast<void>(operation);
        });
#endif
#ifdef RUVIA_ENABLE_REDIS
        check_operation_does_not_use_request_arena(ruvia_ctx, fixture.request_memory_, [&] {
            auto operation = redis.command("SET", "key", value);
            static_cast<void>(operation);
        });
        check_operation_does_not_use_request_arena(ruvia_ctx, fixture.request_memory_, [&] {
            auto operation = context_value.redis("cache").set("key", value);
            static_cast<void>(operation);
        });
#endif
        check_operation_does_not_use_request_arena(ruvia_ctx, fixture.request_memory_, [&] {
            ruvia::http_client_request_view request{.method_ = "GET", .target_ = target};
            auto operation = http_client_value.send(request);
            static_cast<void>(operation);
        });
        check_operation_does_not_use_request_arena(ruvia_ctx, fixture.request_memory_, [&] {
            ruvia::http_client_request_view request{.method_ = "GET", .target_ = target};
            auto operation = context_value.get_http_client("upstream").send(request);
            static_cast<void>(operation);
        });

        {
            ruvia::detail::context_websocket_binding binding(context_value, websocket_value);
#ifdef RUVIA_ENABLE_DATABASE
            check_operation_does_not_use_request_arena(ruvia_ctx, fixture.request_memory_, [&] {
                auto operation = database_value.query("SELECT ?", std::string_view(transient_operation_value));
                static_cast<void>(operation);
            });
            check_operation_does_not_use_request_arena(ruvia_ctx, fixture.request_memory_, [&] {
                auto operation = context_value.db().query("SELECT ?", std::string_view(transient_operation_value));
                static_cast<void>(operation);
            });
#endif
#ifdef RUVIA_ENABLE_REDIS
            check_operation_does_not_use_request_arena(ruvia_ctx, fixture.request_memory_, [&] {
                auto operation = redis.command("SET", "key", transient_operation_value);
                static_cast<void>(operation);
            });
            check_operation_does_not_use_request_arena(ruvia_ctx, fixture.request_memory_, [&] {
                auto operation = context_value.redis().command("SET", "key", transient_operation_value);
                static_cast<void>(operation);
            });
#endif
            check_operation_does_not_use_request_arena(ruvia_ctx, fixture.request_memory_, [&] {
                auto operation = http_client_value.send(
                    ruvia::http_client_request_view{.method_ = "GET", .target_ = target});
                static_cast<void>(operation);
            });
            check_operation_does_not_use_request_arena(ruvia_ctx, fixture.request_memory_, [&] {
                auto operation = context_value.get_http_client().send(
                    ruvia::http_client_request_view{.method_ = "GET", .target_ = target});
                static_cast<void>(operation);
            });
        }
    }

    RUVIA_CHECK_EQ(handshake, std::string_view("websocket-handshake"));
    RUVIA_CHECK_EQ(retained_operation_value, std::string_view(expected_operation_value));
}

#if defined(RUVIA_ENABLE_DATABASE) || defined(RUVIA_ENABLE_REDIS)
RUVIA_TEST(context_operation_results_release_transient_pmr_storage) {
#ifdef RUVIA_ENABLE_DATABASE
    ruvia::test::counting_memory_resource db_resource;
    {
        auto& resource = db_resource;
        auto make_rows = [&](std::string_view value) {
            auto result_value = ruvia::detail::db_result_access::make_result(&resource);
            auto& rows = ruvia::detail::db_result_access::rows(result_value);
            auto row = ruvia::detail::db_result_access::owned_row(&resource);
            auto& fields_value = ruvia::detail::db_result_access::owned_fields(row);
            auto& column_names = ruvia::detail::db_result_access::owned_column_names(row);
            column_names.emplace_back("value");
            fields_value.push_back(ruvia::detail::db_result_access::owned_field(value, &resource));
            rows.push_back(std::move(row));
            return result_value;
        };

        const std::string expected(2048, 'd');
        auto retained = make_rows(expected);
        const auto baseline = resource.live_allocations();
        RUVIA_CHECK(baseline > 0);
        for (int index = 0; index != 100; ++index) {
            const std::string value(2048, static_cast<char>('a' + index % 26));
            {
                auto transient = make_rows(value);
                RUVIA_CHECK_EQ(transient.front()["value"].value(),
                    std::optional<std::string_view>(value));
            }
            RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
            RUVIA_CHECK_EQ(retained.front()["value"].value(),
                std::optional<std::string_view>(expected));
        }
    }
    RUVIA_CHECK_EQ(db_resource.live_allocations(), std::size_t{0});
#endif

#ifdef RUVIA_ENABLE_REDIS
    ruvia::test::counting_memory_resource redis_resource;
    {
        auto& resource = redis_resource;
        const std::string expected(2048, 'r');
        auto retained = ruvia::detail::redis_types_access::string_value(expected, &resource);
        const auto baseline = resource.live_allocations();
        RUVIA_CHECK(baseline > 0);
        for (int index = 0; index != 100; ++index) {
            const std::string value(2048, static_cast<char>('a' + index % 26));
            {
                auto transient = ruvia::detail::redis_types_access::string_value(value, &resource);
                RUVIA_CHECK_EQ(transient.string(), std::string_view(value));
            }
            RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
            RUVIA_CHECK_EQ(retained.string(), std::string_view(expected));
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
        RUVIA_CHECK_EQ(retained.string(), std::string_view(expected));
    }
    RUVIA_CHECK_EQ(redis_resource.live_allocations(), std::size_t{0});
#endif
}
#endif

RUVIA_TEST(context_operation_client_failures_do_not_leave_request_arena_state) {
    context_fixture fixture;
    auto context_value = ruvia::detail::context_access::make(
        fixture.request_memory_, fixture.request_,
        fixture.capabilities_.make_context_services(fixture.stop_token_));

#ifdef RUVIA_ENABLE_DATABASE
    bool db_rejected = false;
#endif
#ifdef RUVIA_ENABLE_REDIS
    bool redis_rejected = false;
#endif
    bool http_rejected = false;
#ifdef RUVIA_ENABLE_DATABASE
    try {
        static_cast<void>(context_value.db("missing"));
    } catch (const ruvia::db_error&) {
        db_rejected = true;
    }
#endif
#ifdef RUVIA_ENABLE_REDIS
    try {
        static_cast<void>(context_value.redis("missing"));
    } catch (const ruvia::redis_error&) {
        redis_rejected = true;
    }
#endif
    try {
        static_cast<void>(context_value.get_http_client("missing"));
    } catch (const ruvia::http_client_error&) {
        http_rejected = true;
    }

#ifdef RUVIA_ENABLE_DATABASE
    RUVIA_CHECK(db_rejected);
#endif
#ifdef RUVIA_ENABLE_REDIS
    RUVIA_CHECK(redis_rejected);
#endif
    RUVIA_CHECK(http_rejected);

    check_operation_does_not_use_request_arena(ruvia_ctx, fixture.request_memory_, [&] {
        auto operation = context_value.get_http_client().send(
            ruvia::http_client_request_view{.method_ = "GET", .target_ = "/health"});
        static_cast<void>(operation);
    });
}
