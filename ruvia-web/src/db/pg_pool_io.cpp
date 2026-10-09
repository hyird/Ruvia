#include <libpq-fe.h>

#include <array>
#include <charconv>
#include <coroutine>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/core/async.h"
#include "ruvia/web/detail/db/db_utils.h"

#include "db/db_pool_operations.h"
#include "db/db_postgresql.h"
#include "db/db_registry.h"
#include "db/db_slot_socket.h"

namespace ruvia::detail {

task<db_resolved_addresses_type> postgresql_pool::resolve_host(
    connection_slot_type& slot, const ruvia::operation_timeout& deadline_value) {
    return resolve_db_host(*this, slot, deadline_value, "PostgreSQL");
}

task<void> postgresql_pool::connect_unlocked(
    connection_slot_type& slot, const ruvia::operation_timeout& operation_timeout_value) {
    if (scheduler_.closing()) {
        throw db_error(db_error::code_type::closing, db_driver::postgresql, "database client is closing");
    }
    if (slot.connected_) {
        co_return;
    }
    throw_if_cancelled(slot);
    const ruvia::operation_timeout deadline_value = operation_timeout_value.constrained_by(config_.connect_timeout_);
    try {
        auto addresses = co_await resolve_host(slot, deadline_value);
        auto resolved_hosts = make_postgresql_resolved_host_list(config_.tls_.server_name_.empty() ? config_.host_ : config_.tls_.server_name_, addresses, resource_);
        const auto port = format_db_port(config_.port_, "PostgreSQL");
        // Pin the client encoding to UTF-8. Ruvia's strings are UTF-8 throughout,
        // and query parameters are sent in text format; without this the connection
        // inherits the server/database default encoding, so non-ASCII parameters and
        // result text would be misinterpreted on a non-UTF-8 database (e.g. LATIN1,
        // SQL_ASCII). libpq accepts client_encoding as a connection keyword.
        const bool use_tls = config_.tls_.mode_ == client_tls_mode::verify_identity;
        const std::array<const char*, 14> keywords{
            "host", "hostaddr", "port", "user", "password", "dbname", "client_encoding",
            "sslmode", "sslrootcert", "sslcert", "sslkey", "gssencmode", "ssl_min_protocol_version", nullptr};
        const std::array<const char*, 14> values{resolved_hosts.hosts_.c_str(),
            resolved_hosts.addresses_.c_str(), port.data(), config_.username_.c_str(),
            config_.password_.c_str(), config_.database_.c_str(), "UTF8",
            use_tls ? "verify-full" : "disable",
            use_tls ? (config_.tls_.ca_file_.empty() ? "system" : config_.tls_.ca_file_.c_str()) : "",
            config_.tls_.certificate_file_.c_str(), config_.tls_.private_key_file_.c_str(),
            "disable", "TLSv1.2", nullptr};
        slot.connection_ = PQconnectStartParams(keywords.data(), values.data(), 0);
        if (slot.connection_ == nullptr) {
            throw db_error(db_error::code_type::connect_failed, db_driver::postgresql,
                "PQconnectStartParams failed");
        }
        slot.wait_socket_ = make_pmr_object<db_slot_socket>(resource_, io_context_);
        if (PQstatus(slot.connection_) == CONNECTION_BAD) {
            throw postgresql_error(
                *slot.connection_, "PQconnectStartParams", db_error::code_type::connect_failed);
        }

        auto status = PQconnectPoll(slot.connection_);
        while (status == PGRES_POLLING_READING || status == PGRES_POLLING_WRITING ||
               status == PGRES_POLLING_ACTIVE) {
            if (status == PGRES_POLLING_ACTIVE) {
                status = PQconnectPoll(slot.connection_);
                continue;
            }
            co_await wait_for_postgresql(slot, status == PGRES_POLLING_READING, deadline_value);
            status = PQconnectPoll(slot.connection_);
        }
        if (status != PGRES_POLLING_OK || PQstatus(slot.connection_) != CONNECTION_OK) {
            throw postgresql_error(*slot.connection_, "PQconnectPoll", db_error::code_type::connect_failed);
        }
        if (use_tls && PQsslInUse(slot.connection_) != 1) {
            throw db_error(db_error::code_type::connect_failed, db_driver::postgresql, "PostgreSQL refused required TLS");
        }
        if (PQsetnonblocking(slot.connection_, 1) != 0) {
            throw postgresql_error(
                *slot.connection_, "PQsetnonblocking", db_error::code_type::connect_failed);
        }
        slot.connected_ = true;
    } catch (...) {
        const auto failure = std::current_exception();
        close_slot(slot);
        std::rethrow_exception(failure);
    }
}

task<void> postgresql_pool::wait_for_postgresql(
    connection_slot_type& slot, bool read, const ruvia::operation_timeout& deadline_value) {
    throw_if_cancelled(slot);
    const auto remaining = deadline_value.remaining();
    if (remaining.has_value() && remaining->count() <= 0) {
        throw db_error(
            db_error::code_type::timeout, db_driver::postgresql, "PostgreSQL operation timed out");
    }
    const auto native = PQsocket(slot.connection_);
    if (native < 0 || slot.wait_socket_ == nullptr) {
        throw db_error(db_error::code_type::io_error, db_driver::postgresql,
            "PostgreSQL connection socket is unavailable");
    }
    if (const auto error =
            slot.wait_socket_->ensure_assigned(static_cast<db_slot_socket::native_socket_type>(native));
        error) {
        throw db_error(db_error::code_type::io_error, db_driver::postgresql,
            "binding PostgreSQL connection socket: " + error.message(), error.value());
    }

    if (remaining.has_value()) {
        arm_db_slot_deadline(worker_, slot, *remaining, connection_slot_type::deadline_kind_type::socket);
    } else {
        clear_db_slot_deadline(slot);
    }
    struct socket_wait_awaiter final {
        connection_slot_type& slot_;
        db_slot_socket& socket_;
        bool read_;
        std::coroutine_handle<> continuation_{};
        std::error_code error_;
        std::exception_ptr initiation_failure_;

        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept {
            continuation_ = handle;
#if defined(_WIN32)
            auto& waitable = socket_.socket_;
            const auto wait_type =
                read_ ? asio::ip::tcp::socket::wait_read : asio::ip::tcp::socket::wait_write;
#else
            auto& waitable = socket_.descriptor_;
            const auto wait_type = read_ ? asio::posix::stream_descriptor::wait_read
                                         : asio::posix::stream_descriptor::wait_write;
#endif
            try {
                waitable.async_wait(wait_type, [this](std::error_code wait_error) noexcept {
                    error_ = wait_error;
                    // Asio has released the wait operation before this user
                    // handler runs, so resuming cannot destroy an outstanding
                    // operation that still borrows the awaiter.
                    continuation_.resume();
                });
                return true;
            } catch (...) {
                initiation_failure_ = std::current_exception();
                return false;
            }
        }

        void await_resume() const {
            if (initiation_failure_ != nullptr) {
                std::rethrow_exception(initiation_failure_);
            }
            if (slot_.deadline_.expired()) {
                throw db_error(db_error::code_type::timeout, db_driver::postgresql,
                    "PostgreSQL operation timed out");
            }
            if (error_) {
                throw db_error(db_error::code_type::io_error, db_driver::postgresql,
                    std::system_error(error_, "PostgreSQL socket wait failed").what(),
                    error_.value());
            }
        }
    };

    std::exception_ptr wait_failure;
    {
        db_slot_active_wait_guard active_wait(slot);
        try {
            co_await socket_wait_awaiter{slot, *slot.wait_socket_, read, {}, {}, {}};
        } catch (...) {
            wait_failure = std::current_exception();
        }
    }
    // PQ* may replace or close the native socket during the next poll. Ensure
    // ASIO no longer owns it before returning to libpq.
    const auto release_error = slot.wait_socket_->release();
    const bool operation_expired = deadline_value.expired() || slot.deadline_.expired();
    clear_db_slot_deadline(slot);
    throw_if_cancelled(slot);
    if (slot.close_requested_) {
        throw db_error(db_error::code_type::closing, db_driver::postgresql, "database client is closing");
    }
    if (operation_expired) {
        throw db_error(
            db_error::code_type::timeout, db_driver::postgresql, "PostgreSQL operation timed out");
    }
    if (wait_failure != nullptr) {
        try {
            std::rethrow_exception(wait_failure);
        } catch (const db_error&) {
            throw;
        } catch (const std::system_error& error) {
            throw db_error(
                db_error::code_type::io_error, db_driver::postgresql, error.what(), error.code().value());
        } catch (const std::runtime_error& error) {
            throw db_error(db_error::code_type::io_error, db_driver::postgresql, error.what());
        }
    }
    if (release_error) {
        throw db_error(db_error::code_type::io_error, db_driver::postgresql,
            "detaching PostgreSQL connection socket: " + release_error.message(),
            release_error.value());
    }
}

task<void> postgresql_pool::flush_output(connection_slot_type& slot, const ruvia::operation_timeout& deadline_value) {
    while (true) {
        const auto status = PQflush(slot.connection_);
        if (status == 0) {
            co_return;
        }
        if (status < 0) {
            throw postgresql_error(*slot.connection_, "PQflush", db_error::code_type::io_error);
        }
        co_await wait_for_postgresql(slot, false, deadline_value);
    }
}

task<void> postgresql_pool::wait_until_result_ready(
    connection_slot_type& slot, const ruvia::operation_timeout& deadline_value) {
    while (PQisBusy(slot.connection_) != 0) {
        co_await wait_for_postgresql(slot, true, deadline_value);
        if (PQconsumeInput(slot.connection_) == 0) {
            throw postgresql_error(*slot.connection_, "PQconsumeInput", db_error::code_type::io_error);
        }
    }
}

task<void> postgresql_pool::send_query(connection_slot_type& slot, const std::pmr::string& sql,
    std::span<const db_value> params, const ruvia::operation_timeout& deadline_value, bool single_row) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }
    if ((sql.find('\0') != std::string_view::npos)) {
        throw std::invalid_argument("SQL must not contain NUL bytes");
    }
    if (!std::in_range<int>(params.size())) {
        throw std::invalid_argument("too many PostgreSQL query parameters");
    }
    auto encoded = encode_postgresql_params(params, resource_);
    const auto* values = encoded.values_.empty() ? nullptr : encoded.values_.data();
    const auto* lengths = encoded.lengths_.empty() ? nullptr : encoded.lengths_.data();
    if (PQsendQueryParams(slot.connection_, sql.c_str(), static_cast<int>(params.size()), nullptr,
            values, lengths, nullptr, 0) == 0) {
        throw postgresql_error(
            *slot.connection_, "PQsendQueryParams", db_error::code_type::statement_failed);
    }
    if (single_row && PQsetSingleRowMode(slot.connection_) == 0) {
        throw postgresql_error(
            *slot.connection_, "PQsetSingleRowMode", db_error::code_type::statement_failed);
    }
    co_await flush_output(slot, deadline_value);
}

}  // namespace ruvia::detail
