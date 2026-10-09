#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/web/attributes.h"
#include "ruvia/web/db/db_exec_result.h"
#include "ruvia/web/db/db_types.h"
#include "ruvia/web/detail/db/db_backend.h"
#include "ruvia/web/detail/db/db_operation_state.h"

struct st_mysql_res;

namespace ruvia {

class db_rows final {
public:
    db_rows(const db_rows&) = delete;
    db_rows& operator=(const db_rows&) = delete;
    db_rows(db_rows&& other) noexcept;
    db_rows& operator=(db_rows&&) = delete;
    ~db_rows();

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const db_row& operator[](std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_row& operator[](std::size_t index) const&& = delete;
    [[nodiscard]] const db_row* begin() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_row* begin() const&& = delete;
    [[nodiscard]] const db_row* end() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_row* end() const&& = delete;
    [[nodiscard]] const db_row& front() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_row& front() const&& = delete;

private:
    friend struct detail::db_result_access;

    struct no_raw_result_type final {};

    struct owned_raw_result_type final {
        owned_raw_result_type(void* owned_value, void (*owned_release)(void*) noexcept) noexcept
            : value_(owned_value),
              release_(owned_release) {}

        void* value_;
        void (*release_)(void*) noexcept;
    };

    explicit db_rows(std::pmr::memory_resource* resource = nullptr);
    db_rows(detail::resolved_pmr_resource_tag, std::pmr::memory_resource* resource);

    std::pmr::vector<db_row> rows_;
    std::pmr::vector<db_field> fields_;
    std::pmr::vector<std::pmr::string> column_names_;
    std::variant<no_raw_result_type, owned_raw_result_type> raw_result_;
};

class db_stream_result final {
public:
    db_stream_result(const db_stream_result&) = delete;
    db_stream_result& operator=(const db_stream_result&) = delete;
    // Operations borrow the address-stable state owned by this object, so a
    // move transfers the state without invalidating a cold or running frame.
    db_stream_result(db_stream_result&& other) noexcept;
    db_stream_result& operator=(db_stream_result&&) = delete;
    ~db_stream_result();

    [[nodiscard]] bool active() const noexcept;
    scoped_operation<std::optional<db_row>> read() &;
    scoped_operation<std::optional<db_row>> read() && = delete;
    scoped_operation<void> close() &;
    scoped_operation<void> close() && = delete;

private:
    friend class db_handle;
    friend class detail::mariadb_pool;
    friend class detail::postgresql_pool;

    struct lease_type final {
        lease_type(detail::db_pool_ref_type client, std::size_t slot, void* result_value,
            std::pmr::memory_resource* resource, operation_options options) noexcept;

        detail::db_pool_ref_type client_;
        std::size_t slot_;
        void* result_;
        std::pmr::memory_resource* resource_;
        operation_options options_;
    };

    using operation_state_type = detail::db_operation_state<lease_type>;
    using operation_guard_type = detail::db_operation_guard<lease_type>;

    class state_type;
    using state_owner_type = std::unique_ptr<state_type, detail::pmr_object_deleter<state_type>>;

    db_stream_result() noexcept = default;
    db_stream_result(detail::db_pool_ref_type client, std::size_t slot, void* result_value,
        std::pmr::memory_resource* resource, operation_options options);
    void reset() noexcept;
    void bind_operation_scope(::ruvia::operation_scope& scope) noexcept;
    static void expire_capability(void* target) noexcept;
    static task<std::optional<db_row>> read_task(operation_guard_type operation);
    static task<void> close_task(operation_guard_type operation);

    state_owner_type state_;
    scoped_capability_registration registration_;
};

}  // namespace ruvia
