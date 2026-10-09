#include <array>
#include <coroutine>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <system_error>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/web/db/db_query.h"
#include "ruvia/web/detail/db/db_result_access.h"
#include "ruvia/web/detail/db/db_value_access.h"

#include "db/db_config_storage.h"
#include "db/db_query_cache.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {
using namespace ruvia;
using namespace std::chrono_literals;
using namespace std::string_view_literals;
using access_type = detail::db_result_access;

db_rows sample(std::pmr::memory_resource* resource, std::string_view value = "a\0b"sv) {
    auto result_value = access_type::make_result(resource);
    auto row = access_type::owned_row(resource);
    auto& names = access_type::owned_column_names(row);
    auto& fields_value = access_type::owned_fields(row);
    names.emplace_back("value");
    names.emplace_back("empty");
    names.emplace_back("null");
    fields_value.push_back(access_type::owned_field(value, resource));
    fields_value.push_back(access_type::owned_field("", resource));
    fields_value.push_back(access_type::null_field(resource));
    access_type::rows(result_value).push_back(std::move(row));
    return result_value;
}
RUVIA_TEST(db_query_plan_owns_compiled_statements_and_rejects_non_row_sequences) {
    for (auto driver : {db_driver::mariadb, db_driver::postgresql}) {
        test::counting_memory_resource source, resource;
        {
            auto plan = [&] {
                db_query query(&source);
                query.select(query.value(std::string(500, 'x'))).cache(50ms);
                db_query count(&source);
                count.select(count.value(7)).cache(100ms);
                return detail::db_query_plan::prepare(query, &count, driver, &resource, nullptr);
            }();
            RUVIA_CHECK_EQ(source.live_allocations(), std::size_t{0});
            RUVIA_CHECK_EQ(plan.first_.sql_, driver == db_driver::mariadb ? "SELECT ?" : "SELECT $1");
            RUVIA_CHECK_EQ(plan.first_.params_.size(), std::size_t{1});
            RUVIA_CHECK_EQ(detail::db_value_access::text(plan.first_.params_[0]), std::string(500, 'x'));
            RUVIA_CHECK_EQ(plan.first_.cache_duration_, std::optional{50ms});
            RUVIA_CHECK(!plan.first_.cache_key_.has_value());
            RUVIA_CHECK(plan.second_.has_value());
            RUVIA_CHECK_EQ(plan.second_->cache_duration_, std::optional{100ms});
            RUVIA_CHECK_EQ(detail::db_value_access::signed_value(plan.second_->params_[0]), 7);
            db_query rows(&source);
            rows.select(rows.value(1));
            db_query write(&source);
            write.delete_from("records");
            RUVIA_CHECK(testing::throws_on([&] {
                (void)detail::db_query_plan::prepare(write, nullptr, driver, &resource, nullptr);
            }));
            RUVIA_CHECK(testing::throws_on([&] {
                (void)detail::db_query_plan::prepare(rows, &write, driver, &resource, nullptr);
            }));
            RUVIA_CHECK(testing::throws_on([&] {
                (void)detail::db_query_plan::prepare(write, &rows, driver, &resource, nullptr);
            }));
            auto single = detail::db_query_plan::prepare(rows, nullptr, driver, &resource, nullptr);
            RUVIA_CHECK(!single.second_.has_value());
        }
        RUVIA_CHECK_EQ(source.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
}

RUVIA_TEST(db_cache_scope_separates_alias_backend_endpoint_database_and_role) {
    auto* resource = std::pmr::get_default_resource();
#ifdef RUVIA_ENABLE_MARIADB
    db_config config{.driver_ = db_driver::mariadb};
#else
    db_config config{.driver_ = db_driver::postgresql};
#endif
    const detail::db_config_storage base(config, resource);
    const auto scope = detail::db_cache_scope("shared", "primary", base, resource);
    RUVIA_CHECK(scope != detail::db_cache_scope("shared", "replica", base, resource));
    RUVIA_CHECK(scope != detail::db_cache_scope("another", "primary", base, resource));
    for (int dimension = 0; dimension < 5; ++dimension) {
        detail::db_config_storage changed(base, resource);
        switch (dimension) {
            case 0:
                changed.host_ = "127.0.0.2";
                break;
            case 1:
                ++changed.port_;
                break;
            case 2:
                changed.database_ = "another";
                break;
            case 3:
                changed.username_ = "another";
                break;
            case 4:
                changed.driver_ = base.driver_ == db_driver::mariadb ? db_driver::postgresql : db_driver::mariadb;
                break;
        }
        RUVIA_CHECK(scope != detail::db_cache_scope("shared", "primary", changed, resource));
    }
    const auto other = detail::db_cache_scope("shared", "replica", base, resource);
    RUVIA_CHECK(detail::db_cache_key(scope, "explicit-id", {}, {}, config.driver_, resource) !=
                detail::db_cache_key(other, "explicit-id", {}, {}, config.driver_, resource));
}

RUVIA_TEST(db_cache_codec_preserves_binary_empty_null_and_owns_decoded_rows) {
    test::counting_memory_resource resource;
    {
        auto decoded = [&] {
            auto source_value = sample(&resource);
            const auto bytes_value = detail::encode_db_cache_rows(source_value, &resource);
            return detail::decode_db_cache_rows(bytes_value, &resource);
        }();
        const auto baseline = resource.live_allocations();
        for (int index = 0; index < 20; ++index) {
            {
                auto source_value = sample(&resource, std::string(500, 'x'));
                const auto bytes_value = detail::encode_db_cache_rows(source_value, &resource);
                auto next_value = detail::decode_db_cache_rows(bytes_value, &resource);
                RUVIA_CHECK_EQ(next_value[0]["value"].value()->size(), std::size_t{500});
            }
            RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
            RUVIA_CHECK_EQ(*decoded[0]["value"].value(), "a\0b"sv);
            RUVIA_CHECK(decoded[0]["empty"].value()->empty());
            RUVIA_CHECK(!decoded[0]["null"].value());
        }
        auto empty = access_type::make_result(&resource);
        auto bytes_value = detail::encode_db_cache_rows(empty, &resource);
        RUVIA_CHECK(detail::decode_db_cache_rows(bytes_value, &resource).empty());
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}
RUVIA_TEST(db_cache_codec_rejects_truncated_corrupt_and_trailing_data) {
    test::counting_memory_resource resource;
    {
        auto source_value = sample(&resource);
        auto bytes_value = detail::encode_db_cache_rows(source_value, &resource);
        const auto baseline = resource.live_allocations();
        for (std::size_t size = 0; size < bytes_value.size(); ++size) {
            RUVIA_CHECK(testing::throws_on([&] { (void)detail::decode_db_cache_rows(std::string_view(bytes_value).substr(0, size), &resource); }));
            RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
        }
        bytes_value.push_back('x');
        RUVIA_CHECK(testing::throws_on([&] { (void)detail::decode_db_cache_rows(bytes_value, &resource); }));
        bytes_value.assign("RUVIAQC1");
        bytes_value.append(8, '\xff');
        RUVIA_CHECK(testing::throws_on([&] { (void)detail::decode_db_cache_rows(bytes_value, &resource); }));
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}
RUVIA_TEST(db_cache_keys_separate_parameters_types_drivers_and_namespaces) {
    auto* resource = std::pmr::get_default_resource();
    const auto key = [&](std::span<const db_value> values, db_driver driver = db_driver::postgresql,
                         std::string_view ns = "app", std::string_view sql = "SELECT ?") {
        return detail::db_cache_key(ns, {}, sql, values, driver, resource);
    };
    const std::array signed_value{db_value(1)};
    const std::array unsigned_value{db_value(1u)};
    const std::array text_value{db_value("1")};
    const std::array bool_value_value{db_value(true)};
    const std::array null_value{db_value(nullptr)};
    const std::array binary_value{db_value("a\0b"sv)};
    const std::array other_binary{db_value("a")};
    RUVIA_CHECK_EQ(key(signed_value), key(signed_value));
    for (const auto values : {std::span<const db_value>(unsigned_value), std::span<const db_value>(text_value), std::span<const db_value>(bool_value_value), std::span<const db_value>(null_value)}) {
        RUVIA_CHECK(key(signed_value) != key(values));
    }
    RUVIA_CHECK(key(binary_value) != key(other_binary));
    RUVIA_CHECK(key(signed_value) != key(signed_value, db_driver::mariadb));
    RUVIA_CHECK(key(signed_value) != key(signed_value, db_driver::postgresql, "other"));
    RUVIA_CHECK(key(signed_value) != key(signed_value, db_driver::postgresql, "app", "SELECT ? + 1"));
    RUVIA_CHECK_EQ(detail::db_cache_key("app", "users", "first", signed_value, db_driver::postgresql, resource),
        detail::db_cache_key("app", "users", "second", text_value, db_driver::postgresql, resource));
    RUVIA_CHECK(detail::db_cache_key("app", "users", {}, {}, db_driver::postgresql, resource) !=
                detail::db_cache_key("app", "users-count", {}, {}, db_driver::postgresql, resource));
}

#ifdef RUVIA_ENABLE_REDIS
struct gate {
    std::coroutine_handle<> continuation_{};
    bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> value) noexcept {
        continuation_ = value;
    }
    void await_resume() const noexcept {}
    void resume() {
        std::exchange(continuation_, {}).resume();
    }
};
struct timer_gate {
    asio::steady_timer timer_;
    std::coroutine_handle<> continuation_{};

    timer_gate(asio::io_context& context_value, std::chrono::milliseconds delay)
        : timer_(context_value, delay) {}

    bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> value) noexcept {
        continuation_ = value;
        timer_.async_wait([this](const std::error_code& error) {
            if (!error) {
                std::exchange(continuation_, {}).resume();
            }
        });
    }
    void await_resume() const noexcept {}
};
struct store_state {
    std::optional<std::pmr::string> value_{};
    std::optional<redis_error::code_type> get_error_{};
    std::optional<redis_error::code_type> put_error_{};
    gate* gate_{nullptr};
    gate* write_gate_{nullptr};
    gate* db_gate_{nullptr};
    timer_gate* timer_gate_{nullptr};
    timer_gate* timer_write_gate_{nullptr};
    timer_gate* timer_db_gate_{nullptr};
    bool cancel_db_{false};
    bool db_error_{false};
    int reads_{0};
    int writes_{0};
    int queries_{0};
    std::optional<std::chrono::milliseconds> get_timeout_{};
    std::optional<std::chrono::milliseconds> sql_timeout_{};
    std::optional<std::chrono::milliseconds> put_timeout_{};
    std::chrono::milliseconds ttl_{};
    std::pmr::memory_resource* resource_;
};
struct store {
    store_state* state_;
    task<std::optional<std::pmr::string>> get(std::string_view, operation_options options) {
        ++state_->reads_;
        state_->get_timeout_ = options.timeout_;
        if (state_->gate_) {
            co_await *state_->gate_;
        } else if (state_->timer_gate_) {
            co_await *state_->timer_gate_;
        }
        if (state_->get_error_) {
            throw redis_error(*state_->get_error_, "read failed");
        }
        if (state_->value_) {
            co_return std::pmr::string(*state_->value_, state_->resource_);
        }
        co_return std::nullopt;
    }
    task<void> put(std::string_view, std::string_view value, std::chrono::milliseconds ttl,
        operation_options options) {
        ++state_->writes_;
        state_->put_timeout_ = options.timeout_;
        if (state_->write_gate_) {
            co_await *state_->write_gate_;
        } else if (state_->timer_write_gate_) {
            co_await *state_->timer_write_gate_;
        }
        if (state_->put_error_) {
            throw redis_error(*state_->put_error_, "write failed");
        }
        state_->ttl_ = ttl;
        state_->value_.emplace(value, state_->resource_);
        co_return;
    }
};
struct database {
    store_state* state_;
    std::pmr::string value_;

    task<db_rows> operator()(operation_options options) && {
        ++state_->queries_;
        state_->sql_timeout_ = options.timeout_;
        if (state_->db_gate_) {
            co_await *state_->db_gate_;
        } else if (state_->timer_db_gate_) {
            co_await *state_->timer_db_gate_;
        }
        if (state_->cancel_db_) {
            throw db_error(db_error::code_type::cancelled, db_driver::postgresql, "cancelled");
        }
        if (state_->db_error_) {
            throw db_error(db_error::code_type::statement_failed, db_driver::postgresql, "query failed");
        }
        co_return sample(state_->resource_, value_);
    }
};
task<db_rows> operation(store_state& state_value, bool ignore = false,
    std::optional<std::chrono::milliseconds> timeout = std::nullopt) {
    return detail::query_db_cache(store{&state_value}, std::pmr::string(100, 'k', state_value.resource_), 60s, ignore,
        database{&state_value, std::pmr::string(500, 'x', state_value.resource_)}, state_value.resource_,
        operation_options{.timeout_ = timeout});
}
template <typename result_type, typename callback_type>
asio::awaitable<void> await_task(task<result_type> task_value, callback_type callback_value) {
    std::optional<result_type> result;
    std::exception_ptr failure;
    try {
        result.emplace(co_await as_awaitable(std::move(task_value)));
    } catch (...) {
        failure = std::current_exception();
    }
    callback_value(std::move(failure), std::move(result));
    co_return;
}
template <typename result_type, typename callback_type>
void start_task(asio::io_context& context_value, task<result_type> task_value, callback_type callback_value) {
    asio::co_spawn(context_value, await_task(std::move(task_value), std::move(callback_value)), asio::detached);
    context_value.poll();
    context_value.restart();
}
template <typename result_type>
result_type run(task<result_type> task_value) {
    asio::io_context context;
    std::optional<result_type> result;
    std::exception_ptr failure;
    start_task(context, std::move(task_value), [&](std::exception_ptr error, std::optional<result_type> rows) {
        failure = std::move(error);
        if (rows) {
            result.emplace(std::move(*rows));
        }
    });
    context.run();
    if (failure) {
        std::rethrow_exception(failure);
    }
    return std::move(result.value());
}
struct sequence_backend {
    store_state* first_;
    store_state* second_;

    task<db_rows> operator()(detail::db_query_step step, operation_options options,
        const operation_timeout& deadline_value) const {
        auto* state_value = step.sql_.front() == 'x' ? first_ : second_;
        database database_value{state_value, std::move(step.sql_)};
        if (step.cache_key_) {
            co_return co_await detail::query_db_cache(store{state_value}, std::move(*step.cache_key_),
                *step.cache_duration_, false, std::move(database_value), state_value->resource_,
                std::move(options), deadline_value);
        }
        co_return co_await std::move(database_value)(std::move(options));
    }
};

detail::db_query_plan sequence_plan(std::pmr::memory_resource* resource, bool cached) {
    const auto step = [&](char value) {
        return detail::db_query_step{std::pmr::string(500, value, resource),
            std::pmr::vector<db_value>(resource),
            cached ? std::optional{std::pmr::string(100, value, resource)} : std::nullopt,
            60s};
    };
    return detail::db_query_plan{step('x'), step('y')};
}

task<std::pair<db_rows, db_rows>> sequence_operation(store_state& first, store_state& second,
    bool cached, std::optional<std::chrono::milliseconds> timeout = std::nullopt) {
    return detail::execute_db_query_plan<true>(sequence_plan(first.resource_, cached),
        sequence_backend{&first, &second}, operation_options{.timeout_ = timeout});
}

RUVIA_TEST(db_single_query_plan_uses_the_same_cache_and_direct_execution_chain) {
    for (bool cached : {false, true}) {
        test::counting_memory_resource resource;
        {
            store_state first{.resource_ = &resource};
            store_state second{.resource_ = &resource};
            const auto operation = [&] {
                auto plan = sequence_plan(&resource, cached);
                plan.second_.reset();
                return detail::execute_db_query_plan<false>(std::move(plan),
                    sequence_backend{&first, &second}, operation_options{});
            };
            {
                auto cold = operation();
            }
            RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
            auto retained = run(operation());
            {
                auto next_value = run(operation());
                RUVIA_CHECK_EQ(*next_value[0]["value"].value(), std::string(500, 'x'));
            }
            RUVIA_CHECK_EQ(first.queries_, cached ? 1 : 2);
            RUVIA_CHECK_EQ(second.queries_ + second.reads_ + second.writes_, 0);
            RUVIA_CHECK_EQ(*retained[0]["value"].value(), std::string(500, 'x'));
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
}

RUVIA_TEST(db_query_sequence_direct_misses_and_mixed_cache_hits_retain_results) {
    for (bool cached : {false, true}) {
        test::counting_memory_resource resource;
        {
            store_state first{.resource_ = &resource};
            store_state second{.resource_ = &resource};
            {
                auto cold = sequence_operation(first, second, cached);
            }
            RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
            RUVIA_CHECK_EQ(first.reads_ + second.reads_ + first.queries_ + second.queries_, 0);
            auto retained = run(sequence_operation(first, second, cached));
            RUVIA_CHECK_EQ(first.queries_, 1);
            RUVIA_CHECK_EQ(second.queries_, 1);
            RUVIA_CHECK_EQ(first.reads_, cached ? 1 : 0);
            RUVIA_CHECK_EQ(second.reads_, cached ? 1 : 0);
            if (cached) {
                second.value_.reset();
                {
                    auto mixed = run(sequence_operation(first, second, true));
                    RUVIA_CHECK_EQ(first.queries_, 1);
                    RUVIA_CHECK_EQ(second.queries_, 2);
                }
                const auto baseline = resource.live_allocations();
                for (int index = 0; index < 10; ++index) {
                    {
                        auto hits = run(sequence_operation(first, second, true));
                        RUVIA_CHECK_EQ(first.queries_, 1);
                        RUVIA_CHECK_EQ(second.queries_, 2);
                    }
                    RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
                }
            }
            RUVIA_CHECK_EQ(*retained.first[0]["value"].value(), std::string(500, 'x'));
            RUVIA_CHECK_EQ(*retained.second[0]["value"].value(), std::string(500, 'y'));
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
    }
}

RUVIA_TEST(db_query_sequence_first_failure_never_starts_count) {
    for (bool cached : {false, true}) {
        for (bool cache_failure : {false, true}) {
            test::counting_memory_resource resource;
            {
                store_state first{.resource_ = &resource};
                store_state second{.resource_ = &resource};
                if (cached && cache_failure) {
                    first.get_error_ = redis_error::code_type::io_error;
                } else {
                    first.db_error_ = true;
                }
                RUVIA_CHECK(testing::throws_on([&] {
                    auto rows = run(sequence_operation(first, second, cached));
                }));
                RUVIA_CHECK_EQ(second.reads_, 0);
                RUVIA_CHECK_EQ(second.queries_, 0);
                RUVIA_CHECK_EQ(second.writes_, 0);
            }
            RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        }
    }
}

RUVIA_TEST(db_query_sequence_count_receives_only_remaining_overall_timeout) {
    for (bool cached : {false, true}) {
        test::counting_memory_resource resource;
        asio::io_context context;
        timer_gate first_gate(context, 10ms);
        store_state first{.timer_db_gate_ = &first_gate, .resource_ = &resource};
        store_state second{.resource_ = &resource};
        std::exception_ptr failure;
        bool finished = false;
        start_task(context, sequence_operation(first, second, cached, 500ms),
            [&](std::exception_ptr error, std::optional<std::pair<db_rows, db_rows>> rows) {
                failure = std::move(error);
                finished = rows.has_value();
            });
        context.run();
        RUVIA_CHECK(failure == nullptr);
        RUVIA_CHECK(finished);
        RUVIA_CHECK(first.sql_timeout_.has_value());
        RUVIA_CHECK(second.sql_timeout_.has_value());
        RUVIA_CHECK(*second.sql_timeout_ > 0ms);
        RUVIA_CHECK(*second.sql_timeout_ < *first.sql_timeout_);
        if (cached) {
            RUVIA_CHECK(second.get_timeout_.has_value());
            RUVIA_CHECK(*second.get_timeout_ < *first.sql_timeout_);
        }
    }
}

RUVIA_TEST(db_query_sequence_expiration_and_cancellation_do_not_start_count) {
    for (bool cached : {false, true}) {
        for (bool cancel : {false, true}) {
            test::counting_memory_resource resource;
            {
                asio::io_context context;
                timer_gate expired_gate(context, 30ms);
                gate cancel_gate;
                store_state first{.resource_ = &resource};
                store_state second{.resource_ = &resource};
                if (cancel) {
                    first.db_gate_ = &cancel_gate;
                    first.cancel_db_ = true;
                } else {
                    first.timer_db_gate_ = &expired_gate;
                }
                std::exception_ptr failure;
                start_task(context, sequence_operation(first, second, cached, cancel ? 500ms : 5ms),
                    [&](std::exception_ptr error, std::optional<std::pair<db_rows, db_rows>>) {
                        failure = std::move(error);
                    });
                if (cancel) {
                    RUVIA_CHECK(cancel_gate.continuation_ != nullptr);
                    cancel_gate.resume();
                }
                context.run();
                RUVIA_CHECK(failure != nullptr);
                try {
                    std::rethrow_exception(failure);
                } catch (const db_error& error) {
                    RUVIA_CHECK_EQ(error.code(), cancel ? db_error::code_type::cancelled : db_error::code_type::timeout);
                }
                RUVIA_CHECK_EQ(second.reads_, 0);
                RUVIA_CHECK_EQ(second.queries_, 0);
                RUVIA_CHECK_EQ(second.writes_, 0);
            }
            RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        }
    }
}

RUVIA_TEST(db_cache_hit_skips_database_and_repeated_operations_release_temporaries) {
    test::counting_memory_resource resource;
    {
        store_state state_value{.resource_ = &resource};
        auto retained = run(operation(state_value));
        RUVIA_CHECK_EQ(state_value.queries_, 1);
        RUVIA_CHECK_EQ(state_value.writes_, 1);
        RUVIA_CHECK(state_value.ttl_ > 0ms && state_value.ttl_ <= 60s);
        const auto baseline = resource.live_allocations();
        for (int index = 0; index < 20; ++index) {
            {
                auto next_value = run(operation(state_value));
                RUVIA_CHECK_EQ(next_value[0]["value"].value()->size(), std::size_t{500});
            }
            RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
            RUVIA_CHECK_EQ(retained[0]["value"].value()->size(), std::size_t{500});
        }
        RUVIA_CHECK_EQ(state_value.queries_, 1);
        state_value.value_.reset();
        {
            auto refreshed = run(operation(state_value));
        }
        RUVIA_CHECK_EQ(state_value.queries_, 2);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}
RUVIA_TEST(db_cache_ignored_cache_errors_return_and_retain_database_results) {
    test::counting_memory_resource resource;
    store_state state_value{.resource_ = &resource};
    state_value.get_error_ = redis_error::code_type::io_error;
    state_value.put_error_ = redis_error::code_type::io_error;

    auto retained = run(operation(state_value, true));
    const std::string expected(500, 'x');
    RUVIA_CHECK_EQ(state_value.reads_, 1);
    RUVIA_CHECK_EQ(state_value.queries_, 1);
    RUVIA_CHECK_EQ(state_value.writes_, 1);
    RUVIA_CHECK(!state_value.value_.has_value());
    RUVIA_CHECK_EQ(std::string_view(*retained[0]["value"].value()), std::string_view(expected));
    RUVIA_CHECK(retained[0]["empty"].value()->empty());
    RUVIA_CHECK(!retained[0]["null"].value());

    state_value.get_error_.reset();
    state_value.put_error_.reset();
    const auto before_refresh = resource.live_allocations();
    {
        auto next_value = run(operation(state_value, true));
        RUVIA_CHECK_EQ(state_value.reads_, 2);
        RUVIA_CHECK_EQ(state_value.queries_, 2);
        RUVIA_CHECK_EQ(state_value.writes_, 2);
        RUVIA_CHECK_EQ(std::string_view(*next_value[0]["value"].value()), std::string_view(expected));
        const auto cache_baseline = resource.live_allocations();
        {
            auto hit = run(operation(state_value, true));
            RUVIA_CHECK_EQ(state_value.reads_, 3);
            RUVIA_CHECK_EQ(state_value.queries_, 2);
            RUVIA_CHECK_EQ(state_value.writes_, 2);
            RUVIA_CHECK_EQ(std::string_view(*hit[0]["value"].value()), std::string_view(expected));
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), cache_baseline);
    }
    RUVIA_CHECK_EQ(std::string_view(*retained[0]["value"].value()), std::string_view(expected));
    state_value.value_.reset();
    RUVIA_CHECK_EQ(resource.live_allocations(), before_refresh);
}
RUVIA_TEST(db_cache_timeout_budget_is_shared_and_not_ignored) {
    test::counting_memory_resource resource;
    asio::io_context context;
    timer_gate get_gate(context, 5ms);
    timer_gate sql_gate(context, 5ms);
    timer_gate put_gate(context, 200ms);
    store_state state_value{.timer_gate_ = &get_gate, .timer_write_gate_ = &put_gate, .timer_db_gate_ = &sql_gate, .resource_ = &resource};
    std::exception_ptr failure;
    start_task(context, operation(state_value, true, 100ms), [&](std::exception_ptr error, std::optional<db_rows>) {
        failure = std::move(error);
    });
    context.run();

    RUVIA_CHECK(failure != nullptr);
    try {
        std::rethrow_exception(failure);
    } catch (const db_error& error) {
        RUVIA_CHECK_EQ(error.code(), db_error::code_type::timeout);
    } catch (...) {
        RUVIA_CHECK(false);
    }
    RUVIA_CHECK(state_value.get_timeout_.has_value());
    RUVIA_CHECK(state_value.sql_timeout_.has_value());
    RUVIA_CHECK(state_value.put_timeout_.has_value());
    RUVIA_CHECK(*state_value.get_timeout_ > 0ms && *state_value.get_timeout_ <= 100ms);
    RUVIA_CHECK(*state_value.sql_timeout_ > 0ms && *state_value.sql_timeout_ < 100ms);
    RUVIA_CHECK(*state_value.put_timeout_ > 0ms && *state_value.put_timeout_ < 100ms);
    RUVIA_CHECK_EQ(state_value.queries_, 1);
}
RUVIA_TEST(db_cache_get_timeout_is_not_ignored_or_followed_by_database) {
    test::counting_memory_resource resource;
    asio::io_context context;
    timer_gate get_gate(context, 150ms);
    store_state state_value{.timer_gate_ = &get_gate, .resource_ = &resource};
    std::exception_ptr failure;
    start_task(context, operation(state_value, true, 100ms), [&](std::exception_ptr error, std::optional<db_rows>) {
        failure = std::move(error);
    });
    context.run();

    RUVIA_CHECK(failure != nullptr);
    try {
        std::rethrow_exception(failure);
    } catch (const db_error& error) {
        RUVIA_CHECK_EQ(error.code(), db_error::code_type::timeout);
    } catch (...) {
        RUVIA_CHECK(false);
    }
    RUVIA_CHECK(state_value.get_timeout_.has_value());
    RUVIA_CHECK(*state_value.get_timeout_ > 0ms && *state_value.get_timeout_ <= 100ms);
    RUVIA_CHECK_EQ(state_value.queries_, 0);
}
RUVIA_TEST(db_cache_failure_policy_cold_drop_and_cancellation_release_storage) {
    test::counting_memory_resource resource;
    {
        store_state state_value{.resource_ = &resource};
        {
            auto cold = operation(state_value);
            RUVIA_CHECK(resource.live_allocations() > 0);
        }
        RUVIA_CHECK_EQ(state_value.reads_, 0);
        RUVIA_CHECK_EQ(state_value.queries_, 0);
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        for (bool write : {false, true}) {
            for (auto code : {redis_error::code_type::io_error, redis_error::code_type::timeout,
                     redis_error::code_type::cancelled, redis_error::code_type::closing}) {
                for (bool ignore : {false, true}) {
                    state_value.get_error_ = write ? std::nullopt : std::optional{code};
                    state_value.put_error_ = write ? std::optional{code} : std::nullopt;
                    const auto failed = testing::throws_on([&] { auto rows = run(operation(state_value, ignore)); });
                    RUVIA_CHECK_EQ(failed, !ignore || code == redis_error::code_type::cancelled ||
                                               code == redis_error::code_type::closing);
                    state_value.value_.reset();
                    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
                }
            }
        }
        state_value.get_error_.reset();
        state_value.put_error_.reset();
        state_value.db_error_ = true;
        RUVIA_CHECK(testing::throws_on([&] { auto rows = run(operation(state_value, true)); }));
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        state_value.db_error_ = false;
        state_value.value_.emplace("corrupt", &resource);
        RUVIA_CHECK(testing::throws_on([&] { auto rows = run(operation(state_value)); }));
        {
            auto rows = run(operation(state_value, true));
        }
        state_value.value_.reset();
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        gate gate;
        state_value.gate_ = &gate;
        state_value.get_error_ = redis_error::code_type::cancelled;
        asio::io_context context;
        bool cancelled = false;
        start_task(context, operation(state_value, true), [&](std::exception_ptr failure, std::optional<db_rows>) {
            if (failure) {
                try {
                    std::rethrow_exception(failure);
                } catch (const redis_error& error) {
                    cancelled = error.code() == redis_error::code_type::cancelled;
                }
            }
        });
        RUVIA_CHECK(gate.continuation_ != nullptr);
        gate.resume();
        context.run();
        RUVIA_CHECK(cancelled);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
}
RUVIA_TEST(db_cache_suspended_database_and_write_cancellation_release_results) {
    for (bool write : {false, true}) {
        test::counting_memory_resource resource;
        {
            gate gate;
            store_state state_value{.resource_ = &resource};
            if (write) {
                state_value.write_gate_ = &gate;
                state_value.put_error_ = redis_error::code_type::cancelled;
            } else {
                state_value.db_gate_ = &gate;
                state_value.cancel_db_ = true;
            }
            asio::io_context context;
            bool cancelled = false;
            start_task(context, operation(state_value, true), [&](std::exception_ptr failure, std::optional<db_rows>) {
                if (failure) {
                    try {
                        std::rethrow_exception(failure);
                    } catch (const redis_error& error) {
                        cancelled = error.code() == redis_error::code_type::cancelled;
                    } catch (const db_error& error) {
                        cancelled = error.code() == db_error::code_type::cancelled;
                    }
                }
            });
            RUVIA_CHECK(gate.continuation_ != nullptr);
            RUVIA_CHECK(resource.live_allocations() > 0);
            gate.resume();
            context.run();
            RUVIA_CHECK(cancelled);
            RUVIA_CHECK(!state_value.value_);
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());
    }
}
#endif
}  // namespace
