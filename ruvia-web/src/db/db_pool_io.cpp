#include <mysql.h>

#include <array>
#include <charconv>
#include <chrono>
#include <coroutine>
#include <exception>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include "ruvia/core/async.h"

#include "db/db_mysql_runtime.h"
#include "db/db_pool_operations.h"
#include "db/db_registry.h"
#include "db/db_slot_socket.h"
#include "db/db_sql.h"

namespace ruvia {

namespace {

[[nodiscard]] db_error mysql_socket_error(std::string_view operation, std::error_code error) {
    std::string message(operation);
    if (error) {
        message.append(": ");
        message.append(error.message());
    }
    return db_error(db_error::code_type::io_error, db_driver::mariadb, std::move(message),
        error ? std::optional<std::int64_t>(error.value()) : std::nullopt);
}

}  // namespace

task<detail::db_resolved_addresses_type> detail::mariadb_pool::resolve_host(
    connection_slot_type& slot, const ruvia::operation_timeout& deadline_value) {
    return resolve_db_host(*this, slot, deadline_value, "MariaDB");
}

task<void> detail::mariadb_pool::connect_unlocked(
    connection_slot_type& slot, const ruvia::operation_timeout& operation_timeout_value) {
    if (scheduler_.closing()) {
        throw db_error(db_error::code_type::closing, db_driver::mariadb, "database client is closing");
    }
    if (slot.connected_) {
        co_return;
    }
    throw_if_cancelled(slot);
    const ruvia::operation_timeout deadline_value = operation_timeout_value.constrained_by(config_.connect_timeout_);
    try {
        auto addresses = co_await resolve_host(slot, deadline_value);
        auto resolved_hosts = detail::make_mariadb_resolved_host_list(addresses, resource_);

        detail::ensure_mysql_thread_initialized();
        auto* connection = mysql_init(nullptr);
        if (connection == nullptr) {
            throw db_error(db_error::code_type::connect_failed, db_driver::mariadb, "mysql_init failed");
        }
        slot.connection_ = connection;
        slot.wait_socket_ = detail::make_pmr_object<detail::db_slot_socket>(resource_, io_context_);
        if (mysql_optionsv(slot.connection_, MYSQL_OPT_NONBLOCK, nullptr) != 0) {
            throw mysql_error(*slot.connection_, "enabling MariaDB non-blocking I/O",
                db_error::code_type::connect_failed);
        }
        // Pin the connection charset before connecting. mysql_real_escape_string --
        // the interpolate_sql injection defense -- escapes according to the
        // connection charset, and is bypassable on multibyte charsets where a valid
        // character can end in 0x5C (GBK/Big5/SJIS, the classic backslash-swallowing
        // injection). utf8mb4 keeps 0x5C meaning only backslash, so escaping stays
        // safe regardless of the server or client library default, and it carries
        // full Unicode. Set here (not via SET NAMES) so it also governs the handshake.
        if (mysql_optionsv(slot.connection_, MYSQL_SET_CHARSET_NAME, "utf8mb4") != 0) {
            throw mysql_error(
                *slot.connection_, "configuring MariaDB utf8mb4", db_error::code_type::connect_failed);
        }
        if (!detail::set_mysql_timeout(
                *slot.connection_, MYSQL_OPT_CONNECT_TIMEOUT, config_.connect_timeout_) ||
            !detail::set_mysql_timeout(
                *slot.connection_, MYSQL_OPT_READ_TIMEOUT, config_.read_timeout_) ||
            !detail::set_mysql_timeout(
                *slot.connection_, MYSQL_OPT_WRITE_TIMEOUT, config_.write_timeout_)) {
            throw mysql_error(
                *slot.connection_, "configuring MariaDB timeouts", db_error::code_type::connect_failed);
        }

        const bool use_tls = config_.tls_.mode_ == client_tls_mode::verify_identity;
        const my_bool enforce_tls = use_tls ? 1 : 0;
        if (mysql_optionsv(slot.connection_, MYSQL_OPT_SSL_ENFORCE, &enforce_tls) != 0 ||
            mysql_optionsv(slot.connection_, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &enforce_tls) != 0) {
            throw mysql_error(*slot.connection_, "configuring MariaDB authenticated TLS", db_error::code_type::connect_failed);
        }
        if (use_tls) {
            if (mysql_optionsv(slot.connection_, MYSQL_OPT_TLS_VERSION, "TLSv1.2,TLSv1.3") != 0) {
                throw mysql_error(*slot.connection_, "configuring MariaDB TLS versions", db_error::code_type::connect_failed);
            }
            const auto set_path = [&](mysql_option option, const std::pmr::string& path) {
                if (!path.empty() && mysql_optionsv(slot.connection_, option, path.c_str()) != 0) {
                    throw mysql_error(*slot.connection_, "configuring MariaDB TLS credentials", db_error::code_type::connect_failed);
                }
            };
            set_path(MYSQL_OPT_SSL_CA, config_.tls_.ca_file_);
            set_path(MYSQL_OPT_SSL_CERT, config_.tls_.certificate_file_);
            set_path(MYSQL_OPT_SSL_KEY, config_.tls_.private_key_file_);
        }
        auto& initialized = *slot.connection_;
        const auto client_flags = use_tls ? static_cast<unsigned long>(CLIENT_SSL) : 0UL;
        MYSQL* connected = nullptr;
        int status = mysql_real_connect_start(&connected, &initialized, resolved_hosts.c_str(),
            config_.username_.c_str(), config_.password_.c_str(),
            config_.database_.empty() ? nullptr : config_.database_.c_str(), config_.port_, nullptr,
            client_flags);

        while (status != 0) {
            status = mysql_real_connect_cont(
                &connected, &initialized, co_await wait_for_mysql(slot, status, deadline_value));
        }

        if (connected == nullptr) {
            throw mysql_error(initialized, "mysql_real_connect", db_error::code_type::connect_failed);
        }

        if (use_tls && mysql_get_ssl_cipher(&initialized) == nullptr) {
            throw db_error(db_error::code_type::connect_failed, db_driver::mariadb, "MariaDB refused required TLS");
        }

        // MariaDB's non-blocking API suspends by yielding out of a fibre when
        // the socket reports EAGAIN. Connect leaves that socket in blocking
        // mode, so every later mysql_*_start() ran the entire statement inside
        // the call instead of returning a wait mask: the worker's event loop
        // stopped for as long as the query took -- no other connection on it
        // progressed, no timer fired, and no deadline could be enforced,
        // because nothing was running to enforce it. Switching the descriptor
        // restores the suspend the API is built around.
        const auto native = mysql_get_socket(&initialized);
        using native_socket_type = std::remove_cv_t<decltype(native)>;
        if (native == static_cast<native_socket_type>(MARIADB_INVALID_SOCKET) ||
            slot.wait_socket_ == nullptr) {
            throw mysql_socket_error("MariaDB connection has no waitable socket", {});
        }
        if (const auto error = slot.wait_socket_->ensure_assigned(
                static_cast<detail::db_slot_socket::native_socket_type>(native));
            error) {
            throw mysql_socket_error("binding MariaDB connection socket", error);
        }
        const auto non_blocking_error = slot.wait_socket_->make_non_blocking();
        const auto release_error = slot.wait_socket_->release();
        if (non_blocking_error) {
            throw mysql_socket_error(
                "making MariaDB connection socket non-blocking", non_blocking_error);
        }
        if (release_error) {
            throw mysql_socket_error("detaching MariaDB connection socket", release_error);
        }
        slot.connected_ = true;
    } catch (...) {
        const auto failure = std::current_exception();
        close_slot(slot);
        std::rethrow_exception(failure);
    }
}

task<ruvia::operation_timeout> detail::mariadb_pool::run_mysql_statement(connection_slot_type& slot,
    std::string_view sql, std::span<const db_value> params, std::pmr::memory_resource* resource,
    const ruvia::operation_timeout& operation_timeout_value) {
    throw_if_cancelled(slot);
    if (!slot.connected_) {
        co_await connect_unlocked(slot, operation_timeout_value);
    }
    const ruvia::operation_timeout deadline_value = operation_timeout_value.constrained_by(config_.query_timeout_);
    std::pmr::string interpolated_sql(detail::pmr_resource_or_default(resource));
    if (!params.empty()) {
        interpolated_sql = interpolate_sql(*slot.connection_, sql, params, resource);
        sql = interpolated_sql;
    }

    auto& connection = *slot.connection_;
    validate_mariadb_sql_length(sql.size());
    int query_result = 0;
    int status = mysql_real_query_start(
        &query_result, &connection, sql.data(), static_cast<unsigned long>(sql.size()));
    while (status != 0) {
        status = mysql_real_query_cont(
            &query_result, &connection, co_await wait_for_mysql(slot, status, deadline_value));
    }
    if (query_result != 0) {
        throw mysql_error(connection, "mysql_real_query", db_error::code_type::statement_failed);
    }
    co_return deadline_value;
}

task<st_mysql_res*> detail::mariadb_pool::store_mysql_result(
    connection_slot_type& slot, const ruvia::operation_timeout& deadline_value) {
    auto& connection = *slot.connection_;
    MYSQL_RES* result_value = nullptr;
    int status = mysql_store_result_start(&result_value, &connection);
    while (status != 0) {
        status = mysql_store_result_cont(
            &result_value, &connection, co_await wait_for_mysql(slot, status, deadline_value));
    }
    co_return result_value;
}

task<int> detail::mariadb_pool::wait_for_mysql(
    connection_slot_type& slot, int status, const ruvia::operation_timeout& deadline_value) {
    throw_if_cancelled(slot);
    auto& connection = *slot.connection_;
    const auto timeout = deadline_value.remaining();
    // A db_config deadline is this pool's to enforce, not the driver's. Reporting
    // its expiry to MariaDB as MYSQL_WAIT_TIMEOUT only works where libmariadb
    // has a matching timeout option of its own -- it has one for connect, none
    // for a statement -- so a query deadline was handed over and dropped, and
    // the wait was simply re-entered until the server answered. Failing here
    // instead ends the operation, and the caller closes the connection: the
    // statement may still be running server-side, which is exactly what a
    // client-side timeout means.
    const auto timed_out = [] {
        return db_error(db_error::code_type::timeout, db_driver::mariadb, "MariaDB operation timed out");
    };
    if (timeout.has_value() && timeout->count() <= 0) {
        throw timed_out();
    }
    std::optional<std::chrono::milliseconds> driver_timeout;
    if ((status & MYSQL_WAIT_TIMEOUT) != 0) {
        const auto milliseconds = mysql_get_timeout_value_ms(&connection);
        driver_timeout = std::chrono::milliseconds(milliseconds == 0 ? 1 : milliseconds);
    }
    const auto selected_deadline = detail::select_mysql_wait_deadline(timeout, driver_timeout);
    const auto wants_read = (status & MYSQL_WAIT_READ) != 0;
    const auto wants_write = (status & MYSQL_WAIT_WRITE) != 0;
    const auto wants_exception = (status & MYSQL_WAIT_EXCEPT) != 0;
    if (!wants_read && !wants_write && !wants_exception) {
        if (!selected_deadline.timeout_) {
            throw db_error(db_error::code_type::io_error, db_driver::mariadb,
                "MariaDB requested an unsupported empty wait");
        }
        detail::arm_db_slot_deadline(worker_, slot, *selected_deadline.timeout_, connection_slot_type::deadline_kind_type::sleep);
        struct deadline_awaiter final {
            connection_slot_type& slot_;

            [[nodiscard]] bool await_ready() noexcept {
                return slot_.deadline_.expire(std::chrono::steady_clock::now()).has_value() ||
                       slot_.deadline_.expired();
            }

            void await_suspend(std::coroutine_handle<> handle) noexcept {
                slot_.deadline_continuation_ = handle;
            }

            void await_resume() const noexcept {}
        };
        db_slot_active_wait_guard active_wait(slot);
        co_await deadline_awaiter{slot};
        detail::clear_db_slot_deadline(slot);
        throw_if_cancelled(slot);
        if (slot.close_requested_) {
            throw db_error(
                db_error::code_type::closing, db_driver::mariadb, "database client is closing");
        }
        if (deadline_value.expired() ||
            selected_deadline.source_ == detail::mysql_wait_deadline_source::operation) {
            throw timed_out();
        }
        co_return MYSQL_WAIT_TIMEOUT;
    }

    const auto native = mysql_get_socket(&connection);
    using native_socket_type = std::remove_cv_t<decltype(native)>;
    if (native == static_cast<native_socket_type>(MARIADB_INVALID_SOCKET)) {
        throw mysql_socket_error("MariaDB returned an invalid socket for an active wait",
            std::make_error_code(std::errc::bad_file_descriptor));
    }

    if (slot.wait_socket_ == nullptr) {
        throw mysql_socket_error("MariaDB wait socket is unavailable", {});
    }
    if (const auto error = slot.wait_socket_->ensure_assigned(
            static_cast<detail::db_slot_socket::native_socket_type>(native));
        error) {
        throw mysql_socket_error("binding MariaDB wait socket", error);
    }

    detail::arm_db_slot_deadline(worker_, slot, selected_deadline.timeout_.value_or(std::chrono::milliseconds(0)),
        connection_slot_type::deadline_kind_type::socket);
    struct socket_wait_awaiter final {
        connection_slot_type& slot_;
        detail::db_slot_socket& slot_socket_;
        int status_;
        std::error_code& socket_failure_;
        std::coroutine_handle<> continuation_{};
        int result_{MYSQL_WAIT_TIMEOUT};
        int pending_{0};
        bool result_set_{false};
        std::exception_ptr initiation_failure_;

        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept {
            continuation_ = handle;
#if defined(_WIN32)
            auto& waitable = slot_socket_.socket_;
            constexpr auto read_wait = asio::ip::tcp::socket::wait_read;
            constexpr auto write_wait = asio::ip::tcp::socket::wait_write;
            constexpr auto error_wait = asio::ip::tcp::socket::wait_error;
#else
            auto& waitable = slot_socket_.descriptor_;
            constexpr auto read_wait = asio::posix::stream_descriptor::wait_read;
            constexpr auto write_wait = asio::posix::stream_descriptor::wait_write;
            constexpr auto error_wait = asio::posix::stream_descriptor::wait_error;
#endif

            try {
                // MariaDB returns a bitmask and _cont() accepts the events that
                // actually occurred, so every requested readiness class must
                // participate in this any-of wait. Increment only after Asio
                // accepts an operation: if a later initiation throws, cancel
                // and stay suspended until the already-published handlers have
                // drained before surfacing the original exception.
                if ((status_ & MYSQL_WAIT_READ) != 0) {
                    waitable.async_wait(read_wait, [this](std::error_code wait_ec) noexcept {
                        on_socket(MYSQL_WAIT_READ, wait_ec);
                    });
                    ++pending_;
                }
                if ((status_ & MYSQL_WAIT_WRITE) != 0) {
                    waitable.async_wait(write_wait, [this](std::error_code wait_ec) noexcept {
                        on_socket(MYSQL_WAIT_WRITE, wait_ec);
                    });
                    ++pending_;
                }
                if ((status_ & MYSQL_WAIT_EXCEPT) != 0) {
                    waitable.async_wait(error_wait, [this](std::error_code wait_ec) noexcept {
                        on_socket(MYSQL_WAIT_EXCEPT, wait_ec);
                    });
                    ++pending_;
                }
            } catch (...) {
                initiation_failure_ = std::current_exception();
                result_set_ = true;
                slot_socket_.cancel();
            }
            return pending_ != 0;
        }

        [[nodiscard]] int await_resume() const {
            if (initiation_failure_ != nullptr) {
                std::rethrow_exception(initiation_failure_);
            }
            return result_;
        }

        void on_socket(int flag, std::error_code wait_error) noexcept {
            if (!result_set_) {
                const bool deadline_expired =
                    slot_.deadline_.expire(std::chrono::steady_clock::now()).has_value() ||
                    slot_.deadline_.expired();
                if (deadline_expired) {
                    result_ = MYSQL_WAIT_TIMEOUT;
                } else if (wait_error) {
                    socket_failure_ = wait_error;
                } else {
                    result_ = flag;
                }
                result_set_ = true;
                slot_socket_.cancel();
            }
            finish_one();
        }

        void finish_one() noexcept {
            --pending_;
            if (pending_ == 0 && continuation_) {
                // Asio releases each wait operation before invoking its user
                // handler. Earlier handlers have returned when pending reaches
                // zero, so no outstanding operation still borrows this awaiter.
                continuation_.resume();
            }
        }
    };

    int result_value = MYSQL_WAIT_TIMEOUT;
    bool expired = false;
    std::error_code socket_failure;
    std::exception_ptr wait_failure;
    {
        db_slot_active_wait_guard active_wait(slot);
        try {
            result_value = co_await socket_wait_awaiter{slot, *slot.wait_socket_, status, socket_failure, {},
                MYSQL_WAIT_TIMEOUT, 0, false, {}};
            expired = slot.deadline_.expired();
        } catch (...) {
            wait_failure = std::current_exception();
            expired = slot.deadline_.expired();
        }
    }
    // The driver may reuse or close its socket in mysql_*_cont(). Detach ASIO
    // only after all readiness handlers have drained, and before returning
    // control to that continuation.
    const auto release_error = slot.wait_socket_->release();
    const bool operation_expired =
        deadline_value.expired() ||
        (selected_deadline.source_ == detail::mysql_wait_deadline_source::operation && expired);
    detail::clear_db_slot_deadline(slot);
    throw_if_cancelled(slot);
    if (slot.close_requested_) {
        throw db_error(db_error::code_type::closing, db_driver::mariadb, "database client is closing");
    }
    if (operation_expired) {
        throw timed_out();
    }
    if (wait_failure != nullptr) {
        try {
            std::rethrow_exception(wait_failure);
        } catch (const db_error&) {
            throw;
        } catch (const std::system_error& error) {
            throw db_error(
                db_error::code_type::io_error, db_driver::mariadb, error.what(), error.code().value());
        } catch (const std::runtime_error& error) {
            throw db_error(db_error::code_type::io_error, db_driver::mariadb, error.what());
        }
    }
    if (socket_failure) {
        throw mysql_socket_error("waiting for MariaDB socket", socket_failure);
    }
    if (release_error) {
        throw mysql_socket_error("detaching MariaDB wait socket", release_error);
    }
    co_return result_value;
}

}  // namespace ruvia
