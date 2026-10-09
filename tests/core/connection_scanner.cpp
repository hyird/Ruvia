#include <chrono>
#include <memory>
#include <optional>

#include <asio/io_context.hpp>

#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/WorkerRuntimeContext.h"

#include "test_harness.h"

namespace {

using scanner_type = ruvia::ConnectionScanner;

struct scan_observation final {
    scanner_type* scanner;
    int calls{0};

    static void tick(void* target, std::int64_t) noexcept {
        auto& self = *static_cast<scan_observation*>(target);
        ++self.calls;
        if (self.scanner != nullptr) {
            self.scanner->stop();
        }
    }
};

enum class retirement_kind { unregister,
    destroy_entry,
    release_guard };

struct entry_retirement final {
    scanner_type* scanner;
    std::unique_ptr<scanner_type::Entry>* entry;
    std::optional<scanner_type::Guard>* guard;
    retirement_kind kind;
    int calls{0};

    static void tick(void* target, std::int64_t) noexcept {
        auto& self = *static_cast<entry_retirement*>(target);
        ++self.calls;
        switch (self.kind) {
            case retirement_kind::unregister:
                self.scanner->unregisterEntry(**self.entry);
                break;
            case retirement_kind::destroy_entry:
                self.entry->reset();
                break;
            case retirement_kind::release_guard:
                self.guard->reset();
                break;
        }
    }
};

struct scanner_restart final {
    scanner_type* scanner;
    int calls{0};
    bool restart_failed{false};

    static void tick(void* target, std::int64_t) noexcept {
        auto& self = *static_cast<scanner_restart*>(target);
        ++self.calls;
        self.scanner->stop();
        if (self.calls == 1) {
            try {
                self.scanner->start();
            } catch (...) {
                self.restart_failed = true;
            }
        }
    }

    static void check(void* target) noexcept {
        tick(target, 0);
    }
};

void check_entry_retirement(ruvia::testing::TestContext& ruvia_ctx, retirement_kind kind, bool retire_current) {
    asio::io_context context;
    ruvia::WorkerRuntimeContext runtime(context, 8);
    scanner_type scanner(runtime.handle(), {.scanInterval = std::chrono::milliseconds(1)});
    asio::ip::tcp::socket socket(context);
    scanner_type::Entry remaining;
    auto retired = std::make_unique<scanner_type::Entry>();
    scanner_type::Entry first;
    std::optional<scanner_type::Guard> guard;
    scanner_type::PeriodicCheckRegistration remaining_check;
    scanner_type::PeriodicCheckRegistration retired_check;
    scanner_type::PeriodicCheckRegistration retirement_check;
    scan_observation remaining_observation{&scanner};
    scan_observation retired_observation{nullptr};
    entry_retirement retirement{&scanner, &retired, &guard, kind};

    scanner.registerEntry(remaining);
    if (kind == retirement_kind::release_guard) {
        guard.emplace(&scanner, *retired, socket);
    } else {
        scanner.registerEntry(*retired);
    }
    if (!retire_current) {
        scanner.registerEntry(first);
    }
    remaining.registerPeriodicCheck(
        remaining_check, &remaining_observation, &scan_observation::tick);
    retired->registerPeriodicCheck(retired_check, &retired_observation, &scan_observation::tick);
    auto& retiring_from = retire_current ? *retired : first;
    retiring_from.registerPeriodicCheck(retirement_check, &retirement, &entry_retirement::tick);
    RUVIA_CHECK(runtime.handle().post([&scanner] { scanner.start(); }).accepted());
    context.run_for(std::chrono::milliseconds(100));
    scanner.stop();
    scanner.unregisterEntry(first);
    scanner.unregisterEntry(remaining);

    RUVIA_CHECK_EQ(retirement.calls, 1);
    RUVIA_CHECK_EQ(retired_observation.calls, 0);
    RUVIA_CHECK_EQ(remaining_observation.calls, 1);
    RUVIA_CHECK_EQ(retired == nullptr, kind == retirement_kind::destroy_entry);
    RUVIA_CHECK(!guard.has_value());
}

void check_scanner_restart(ruvia::testing::TestContext& ruvia_ctx, bool worker_maintenance) {
    asio::io_context context;
    ruvia::WorkerRuntimeContext runtime(context, 8);
    scanner_type scanner(runtime.handle(), {.scanInterval = std::chrono::milliseconds(1)});
    scanner_type::Entry entry;
    scanner_type::PeriodicCheckRegistration periodic;
    scanner_type::WorkerMaintenanceRegistration maintenance;
    scanner_restart restart{&scanner};
    if (worker_maintenance) {
        scanner.registerWorkerMaintenance(maintenance, &restart, &scanner_restart::check);
    } else {
        scanner.registerEntry(entry);
        entry.registerPeriodicCheck(periodic, &restart, &scanner_restart::tick);
    }
    RUVIA_CHECK(runtime.handle().post([&scanner] { scanner.start(); }).accepted());
    RUVIA_CHECK(!ruvia::testing::throwsOn([&context] {
        context.run_for(std::chrono::milliseconds(100));
    }));
    scanner.stop();
    RUVIA_CHECK(!restart.restart_failed);
    RUVIA_CHECK_EQ(restart.calls, 2);
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
