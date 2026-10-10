#include "ruvia/core/connection_scanner.h"

#include <chrono>
#include <memory>
#include <optional>

#include <asio/io_context.hpp>

#include "ruvia/core/worker_runtime_context.h"

#include "test_harness.h"

namespace {

using scanner_type = ruvia::connection_scanner;

struct scan_observation final {
    scanner_type* scanner_;
    int calls_{0};

    static void tick(void* target, std::int64_t) noexcept {
        auto& self = *static_cast<scan_observation*>(target);
        ++self.calls_;
        if (self.scanner_ != nullptr) {
            self.scanner_->stop();
        }
    }
};

enum class retirement_kind { unregister,
    destroy_entry,
    release_guard };

struct entry_retirement final {
    scanner_type* scanner_;
    std::unique_ptr<scanner_type::entry_type>* entry_;
    std::optional<scanner_type::guard_type>* guard_;
    retirement_kind kind_;
    int calls_{0};

    static void tick(void* target, std::int64_t) noexcept {
        auto& self = *static_cast<entry_retirement*>(target);
        ++self.calls_;
        switch (self.kind_) {
            case retirement_kind::unregister:
                self.scanner_->unregister_entry(**self.entry_);
                break;
            case retirement_kind::destroy_entry:
                self.entry_->reset();
                break;
            case retirement_kind::release_guard:
                self.guard_->reset();
                break;
        }
    }
};

struct scanner_restart final {
    scanner_type* scanner_;
    int calls_{0};
    bool restart_failed_{false};

    static void tick(void* target, std::int64_t) noexcept {
        auto& self = *static_cast<scanner_restart*>(target);
        ++self.calls_;
        self.scanner_->stop();
        if (self.calls_ == 1) {
            try {
                self.scanner_->start();
            } catch (...) {
                self.restart_failed_ = true;
            }
        }
    }

    static void check(void* target) noexcept {
        tick(target, 0);
    }
};

struct scanner_destruction final {
    std::unique_ptr<scanner_type>* scanner_;
    int calls_{0};

    static void tick(void* target, std::int64_t) noexcept {
        auto& self = *static_cast<scanner_destruction*>(target);
        ++self.calls_;
        self.scanner_->reset();
    }

    static void check(void* target) noexcept {
        tick(target, 0);
    }
};

void check_entry_retirement(ruvia::testing::test_context& ruvia_ctx, retirement_kind kind, bool retire_current) {
    asio::io_context context;
    ruvia::worker_runtime_context runtime(context, 8);
    scanner_type scanner(runtime.handle(), {.scan_interval_ = std::chrono::milliseconds(1)});
    asio::ip::tcp::socket socket(context);
    scanner_type::entry_type remaining;
    auto retired = std::make_unique<scanner_type::entry_type>();
    scanner_type::entry_type first;
    std::optional<scanner_type::guard_type> guard;
    scanner_type::periodic_check_registration_type remaining_check;
    scanner_type::periodic_check_registration_type retired_check;
    scanner_type::periodic_check_registration_type retirement_check;
    scan_observation remaining_observation{&scanner};
    scan_observation retired_observation{nullptr};
    entry_retirement retirement{&scanner, &retired, &guard, kind};

    scanner.register_entry(remaining);
    if (kind == retirement_kind::release_guard) {
        guard.emplace(&scanner, *retired, socket);
    } else {
        scanner.register_entry(*retired);
    }
    if (!retire_current) {
        scanner.register_entry(first);
    }
    remaining.register_periodic_check(
        remaining_check, &remaining_observation, &scan_observation::tick);
    retired->register_periodic_check(retired_check, &retired_observation, &scan_observation::tick);
    auto& retiring_from = retire_current ? *retired : first;
    retiring_from.register_periodic_check(retirement_check, &retirement, &entry_retirement::tick);
    RUVIA_CHECK(runtime.handle().post([&scanner] { scanner.start(); }).accepted());
    context.run_for(std::chrono::milliseconds(100));
    scanner.stop();
    scanner.unregister_entry(first);
    scanner.unregister_entry(remaining);

    RUVIA_CHECK_EQ(retirement.calls_, 1);
    RUVIA_CHECK_EQ(retired_observation.calls_, 0);
    RUVIA_CHECK_EQ(remaining_observation.calls_, 1);
    RUVIA_CHECK_EQ(retired == nullptr, kind == retirement_kind::destroy_entry);
    RUVIA_CHECK(!guard.has_value());
}

void check_scanner_restart(ruvia::testing::test_context& ruvia_ctx, bool worker_maintenance) {
    asio::io_context context;
    ruvia::worker_runtime_context runtime(context, 8);
    scanner_type scanner(runtime.handle(), {.scan_interval_ = std::chrono::milliseconds(1)});
    scanner_type::entry_type entry;
    scanner_type::periodic_check_registration_type periodic;
    scanner_type::worker_maintenance_registration_type maintenance;
    scanner_restart restart{&scanner};
    if (worker_maintenance) {
        scanner.register_worker_maintenance(maintenance, &restart, &scanner_restart::check);
    } else {
        scanner.register_entry(entry);
        entry.register_periodic_check(periodic, &restart, &scanner_restart::tick);
    }
    RUVIA_CHECK(runtime.handle().post([&scanner] { scanner.start(); }).accepted());
    RUVIA_CHECK(!ruvia::testing::throws_on([&context] {
        context.run_for(std::chrono::milliseconds(100));
    }));
    scanner.stop();
    RUVIA_CHECK(!restart.restart_failed_);
    RUVIA_CHECK_EQ(restart.calls_, 2);
}

void check_scanner_destruction(ruvia::testing::test_context& ruvia_ctx, bool worker_maintenance) {
    asio::io_context context;
    ruvia::worker_runtime_context runtime(context, 8);
    auto scanner = std::make_unique<scanner_type>(
        runtime.handle(), ruvia::connection_scanner_options{.scan_interval_ = std::chrono::milliseconds(1)});
    scanner_type::entry_type later;
    scanner_type::entry_type destroying;
    scanner_type::periodic_check_registration_type later_check;
    scanner_type::periodic_check_registration_type destroying_check;
    scanner_type::worker_maintenance_registration_type maintenance;
    scanner_destruction destruction{&scanner};
    scan_observation later_observation{nullptr};
    // Entries are scanned newest first, so `later` follows the destroying node.
    scanner->register_entry(later);
    scanner->register_entry(destroying);
    later.register_periodic_check(later_check, &later_observation, &scan_observation::tick);
    if (worker_maintenance) {
        scanner->register_worker_maintenance(maintenance, &destruction, &scanner_destruction::check);
    } else {
        destroying.register_periodic_check(destroying_check, &destruction, &scanner_destruction::tick);
    }
    RUVIA_CHECK(runtime.handle().post([&scanner] { scanner->start(); }).accepted());
    RUVIA_CHECK(!ruvia::testing::throws_on([&context] {
        context.run_for(std::chrono::milliseconds(50));
    }));
    RUVIA_CHECK(scanner == nullptr);
    RUVIA_CHECK_EQ(destruction.calls_, 1);
    RUVIA_CHECK_EQ(later_observation.calls_, 0);
    // Teardown detached every node; their own resets stay harmless.
    later.touch();
    destroying.set_phase(scanner_type::phase_type::idle);
    later_check.reset();
    destroying_check.reset();
    maintenance.reset();
}

}  // namespace

RUVIA_TEST(connection_scanner_periodic_check_can_unregister_next_entry) {
    check_entry_retirement(ruvia_ctx, retirement_kind::unregister, false);
}

RUVIA_TEST(connection_scanner_periodic_check_can_unregister_current_entry) {
    check_entry_retirement(ruvia_ctx, retirement_kind::unregister, true);
}

RUVIA_TEST(connection_scanner_periodic_check_can_destroy_next_entry) {
    check_entry_retirement(ruvia_ctx, retirement_kind::destroy_entry, false);
}

RUVIA_TEST(connection_scanner_periodic_check_can_destroy_current_entry) {
    check_entry_retirement(ruvia_ctx, retirement_kind::destroy_entry, true);
}

RUVIA_TEST(connection_scanner_periodic_check_can_release_next_guard) {
    check_entry_retirement(ruvia_ctx, retirement_kind::release_guard, false);
}

RUVIA_TEST(connection_scanner_periodic_check_can_release_current_guard) {
    check_entry_retirement(ruvia_ctx, retirement_kind::release_guard, true);
}

RUVIA_TEST(connection_scanner_periodic_check_can_restart_scanner) {
    check_scanner_restart(ruvia_ctx, false);
}

RUVIA_TEST(connection_scanner_maintenance_can_restart_scanner) {
    check_scanner_restart(ruvia_ctx, true);
}

RUVIA_TEST(connection_scanner_periodic_check_can_destroy_scanner) {
    check_scanner_destruction(ruvia_ctx, false);
}

RUVIA_TEST(connection_scanner_maintenance_can_destroy_scanner) {
    check_scanner_destruction(ruvia_ctx, true);
}
