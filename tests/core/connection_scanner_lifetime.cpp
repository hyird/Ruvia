#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/detail/worker/worker_dispatcher.h"

namespace {

struct periodic_probe final {
    std::size_t ticks_{0};

    static void tick(void* target, std::int64_t) noexcept {
        ++static_cast<periodic_probe*>(target)->ticks_;
    }
};

struct periodic_reset_probe final {
    ruvia::connection_scanner::periodic_check_registration_type* registration_;
    std::size_t ticks_{0};

    static void tick(void* target, std::int64_t) noexcept {
        auto& probe_value = *static_cast<periodic_reset_probe*>(target);
        ++probe_value.ticks_;
        probe_value.registration_->reset();
    }
};

struct worker_maintenance_probe final {
    std::size_t ticks_{0};

    static void check(void* target) noexcept {
        ++static_cast<worker_maintenance_probe*>(target)->ticks_;
    }
};

struct worker_maintenance_reset_probe final {
    ruvia::connection_scanner::worker_maintenance_registration_type* registration_;
    std::size_t ticks_{0};

    static void check(void* target) noexcept {
        auto& probe_value = *static_cast<worker_maintenance_reset_probe*>(target);
        ++probe_value.ticks_;
        probe_value.registration_->reset();
    }
};

struct blocking_maintenance_probe final {
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_{false};
    bool release_{false};

    static void check(void* target) noexcept {
        auto& probe_value = *static_cast<blocking_maintenance_probe*>(target);
        std::unique_lock lock(probe_value.mutex_);
        probe_value.entered_ = true;
        probe_value.condition_.notify_one();
        probe_value.condition_.wait(lock, [&probe_value] { return probe_value.release_; });
    }
};

}  // namespace

int main() {
    asio::io_context io_context;
    auto dispatcher = std::make_shared<ruvia::detail::worker_dispatcher>(io_context, 16);
    auto worker_value = ruvia::detail::worker_handle_access::make(dispatcher);
    try {
        ruvia::connection_scanner invalid(ruvia::worker_handle{}, {});
        return 100;
    } catch (const std::invalid_argument&) {
    }
    const auto rejects = [&worker_value](ruvia::connection_scanner_options options) {
        try {
            ruvia::connection_scanner scanner(worker_value, std::move(options));
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };

    {
        auto options = ruvia::connection_scanner_options{};
        options.idle_timeout_ = std::chrono::milliseconds(0);
        if (!rejects(std::move(options))) {
            return 1;
        }
    }
    {
        auto options = ruvia::connection_scanner_options{};
        options.initial_read_timeout_ = std::chrono::milliseconds(0);
        if (!rejects(std::move(options))) {
            return 2;
        }
    }
    {
        auto options = ruvia::connection_scanner_options{};
        options.payload_read_timeout_ = std::chrono::milliseconds(0);
        if (!rejects(std::move(options))) {
            return 3;
        }
    }
    {
        auto options = ruvia::connection_scanner_options{};
        options.write_timeout_ = std::chrono::milliseconds(0);
        if (!rejects(std::move(options))) {
            return 4;
        }
    }
    {
        auto options = ruvia::connection_scanner_options{};
        options.scan_interval_ = std::chrono::milliseconds(0);
        if (!rejects(std::move(options))) {
            return 5;
        }
    }

    // A failed start is transactional. Scheduling off the worker is rejected,
    // but it must not poison the lifecycle state and suppress a later valid
    // on-worker retry.
    {
        auto options = ruvia::connection_scanner_options{};
        options.scan_interval_ = std::chrono::milliseconds(1);
        ruvia::connection_scanner scanner(worker_value, std::move(options));
        if (scanner.worker().id() != worker_value.id()) {
            return 101;
        }
        worker_maintenance_probe retry_probe;
        ruvia::connection_scanner::worker_maintenance_registration_type retry_registration;
        scanner.register_worker_maintenance(
            retry_registration, &retry_probe, &worker_maintenance_probe::check);
        bool off_worker_rejected = false;
        try {
            scanner.start();
        } catch (const std::logic_error&) {
            off_worker_rejected = true;
        }
        if (!off_worker_rejected ||
            dispatcher->post([&scanner] { scanner.start(); }) != ruvia::post_status::accepted) {
            return 6;
        }
        io_context.run_for(std::chrono::milliseconds(20));
        if (retry_probe.ticks_ == 0 ||
            dispatcher->post([&scanner] { scanner.stop(); }) != ruvia::post_status::accepted) {
            return 7;
        }
        if (io_context.stopped()) {
            io_context.restart();
        }
        io_context.run_for(std::chrono::milliseconds(5));
    }

    asio::ip::tcp::socket socket(io_context);
    ruvia::connection_scanner::entry_type first_entry;
    ruvia::connection_scanner::entry_type second_entry;
    std::optional<ruvia::connection_scanner::guard_type> first_guard;
    std::optional<ruvia::connection_scanner::guard_type> second_guard;

    {
        ruvia::connection_scanner scanner(worker_value, ruvia::connection_scanner_options{});
        first_guard.emplace(&scanner, first_entry, socket);
        second_guard.emplace(&scanner, second_entry, socket);
    }

    // Scanner teardown must invalidate every coarse timestamp pointer and
    // detach every intrusive entry before longer-lived guards are destroyed.
    first_entry.touch();
    first_entry.set_phase(ruvia::connection_scanner::phase_type::reading_initial);
    second_entry.touch();
    second_entry.set_phase(ruvia::connection_scanner::phase_type::writing);
    first_guard.reset();
    second_guard.reset();

    // Multiplexed protocols can own more long-lived streams than the old fixed
    // eight-slot scanner table. Every checked object supplies its own intrusive
    // node, so registration remains allocation-free without silently dropping
    // the ninth stream.
    io_context.restart();
    {
        auto options = ruvia::connection_scanner_options{};
        options.scan_interval_ = std::chrono::milliseconds(1);
        ruvia::connection_scanner scanner(worker_value, std::move(options));
        ruvia::connection_scanner::entry_type entry;
        ruvia::connection_scanner::guard_type guard_value(&scanner, entry, socket);
        std::array<periodic_probe, 12> probes{};
        std::array<ruvia::connection_scanner::periodic_check_registration_type, 12> registrations{};
        periodic_reset_probe reset_probe{&registrations[11]};
        ruvia::connection_scanner::periodic_check_registration_type reset_registration;
        std::array<worker_maintenance_probe, 8> worker_probes{};
        std::array<ruvia::connection_scanner::worker_maintenance_registration_type, 8>
            worker_registrations{};
        worker_maintenance_reset_probe worker_reset_probe{&worker_registrations[7]};
        ruvia::connection_scanner::worker_maintenance_registration_type worker_reset_registration;
        if (dispatcher->post([&] {
                // start() initially has no work. Registrations added afterward
                // must become visible without any coarse timeout being enabled.
                scanner.start();
                for (std::size_t i = 0; i < registrations.size(); ++i) {
                    entry.register_periodic_check(registrations[i], &probes[i], &periodic_probe::tick);
                }
                entry.register_periodic_check(
                    reset_registration, &reset_probe, &periodic_reset_probe::tick);
                for (std::size_t i = 0; i < worker_registrations.size(); ++i) {
                    scanner.register_worker_maintenance(
                        worker_registrations[i], &worker_probes[i], &worker_maintenance_probe::check);
                }
                scanner.register_worker_maintenance(worker_reset_registration, &worker_reset_probe,
                    &worker_maintenance_reset_probe::check);
                entry.set_phase(ruvia::connection_scanner::phase_type::long_lived);
            }) != ruvia::post_status::accepted) {
            return 6;
        }
        // Windows timer dispatch can occasionally exceed a 10 ms scheduling
        // window under a parallel Debug build. Give the 1 ms scanner enough
        // time to complete at least one deterministic pass.
        io_context.run_for(std::chrono::milliseconds(50));
        if (dispatcher->post([&scanner] { scanner.stop(); }) != ruvia::post_status::accepted) {
            return 7;
        }
        if (io_context.stopped()) {
            io_context.restart();
        }
        io_context.run_for(std::chrono::milliseconds(5));
        for (std::size_t i = 0; i < probes.size(); ++i) {
            if ((i == 11 && probes[i].ticks_ != 0) || (i != 11 && probes[i].ticks_ == 0)) {
                return 8;
            }
        }
        if (reset_probe.ticks_ == 0) {
            return 9;
        }
        for (std::size_t i = 0; i < worker_probes.size(); ++i) {
            if ((i == 7 && worker_probes[i].ticks_ != 0) || (i != 7 && worker_probes[i].ticks_ == 0)) {
                return 10;
            }
        }
        if (worker_reset_probe.ticks_ == 0) {
            return 11;
        }
    }

    // Long-lived sessions own their deadlines while ordinary phases retain
    // scanner timeouts. Periodic liveness checks must still run on idle sessions.
    io_context.restart();
    {
        using scanner_type = ruvia::connection_scanner;
        scanner_type scanner(worker_value, {.scan_interval_ = std::chrono::milliseconds(1),
                                               .idle_timeout_ = std::chrono::milliseconds(1),
                                               .initial_read_timeout_ = std::chrono::milliseconds(1),
                                               .payload_read_timeout_ = std::chrono::milliseconds(1),
                                               .write_timeout_ = std::chrono::milliseconds(1)});
        const std::array phases{scanner_type::phase_type::idle, scanner_type::phase_type::reading_initial,
            scanner_type::phase_type::reading_payload, scanner_type::phase_type::writing,
            scanner_type::phase_type::long_lived, scanner_type::phase_type::long_lived};
        std::array<asio::ip::tcp::socket, 6> sockets{
            asio::ip::tcp::socket(io_context), asio::ip::tcp::socket(io_context),
            asio::ip::tcp::socket(io_context), asio::ip::tcp::socket(io_context),
            asio::ip::tcp::socket(io_context), asio::ip::tcp::socket(io_context)};
        std::array<scanner_type::entry_type, 6> entries;
        std::array<std::optional<scanner_type::guard_type>, 6> guards;
        periodic_probe liveness;
        scanner_type::periodic_check_registration_type liveness_registration;
        for (std::size_t i = 0; i < sockets.size(); ++i) {
            sockets[i].open(asio::ip::tcp::v4());
            guards[i].emplace(&scanner, entries[i], sockets[i]);
            entries[i].set_phase(phases[i]);
        }
        entries[4].register_periodic_check(liveness_registration, &liveness, &periodic_probe::tick);
        if (dispatcher->post([&scanner] { scanner.start(); }) != ruvia::post_status::accepted) {
            return 13;
        }
        io_context.run_for(std::chrono::milliseconds(50));
        for (std::size_t i = 0; i < sockets.size(); ++i) {
            if (sockets[i].is_open() != (phases[i] == scanner_type::phase_type::long_lived)) {
                return 14;
            }
        }
        if (liveness.ticks_ == 0) {
            return 15;
        }
        // Returning to ordinary idle restores its deadline; no exemption leaks
        // from an earlier long-lived phase, with or without periodic checks.
        entries[4].set_phase(scanner_type::phase_type::idle);
        entries[5].set_phase(scanner_type::phase_type::idle);
        io_context.run_for(std::chrono::milliseconds(50));
        if (sockets[4].is_open() || sockets[5].is_open()) {
            return 16;
        }
        if (dispatcher->post([&scanner] { scanner.stop(); }) != ruvia::post_status::accepted) {
            return 17;
        }
        io_context.run_for(std::chrono::milliseconds(5));
    }

    // I/O progress renews inactivity, but cannot renew an absolute phase limit.
    io_context.restart();
    {
        using scanner_type = ruvia::connection_scanner;
        scanner_type scanner(worker_value, {.scan_interval_ = std::chrono::milliseconds(1),
                                               .initial_read_timeout_ = std::chrono::seconds(1),
                                               .payload_read_timeout_ = std::chrono::seconds(1),
                                               .initial_read_completion_timeout_ = std::chrono::milliseconds(5),
                                               .payload_read_completion_timeout_ = std::chrono::milliseconds(5)});
        struct progress final {
            scanner_type::entry_type* entry_;
            scanner_type::phase_type phase_;
            static void tick(void* raw, std::int64_t) noexcept {
                auto& self = *static_cast<progress*>(raw);
                self.entry_->touch();
                self.entry_->set_phase(self.phase_);
            }
        };
        std::array<asio::ip::tcp::socket, 2> sockets{
            asio::ip::tcp::socket(io_context), asio::ip::tcp::socket(io_context)};
        std::array<scanner_type::entry_type, 2> entries;
        std::array<std::optional<scanner_type::guard_type>, 2> guards;
        std::array<scanner_type::periodic_check_registration_type, 2> registrations;
        std::array<progress, 2> progress_value{{{&entries[0], scanner_type::phase_type::reading_initial},
            {&entries[1], scanner_type::phase_type::reading_payload}}};
        for (std::size_t i = 0; i < sockets.size(); ++i) {
            sockets[i].open(asio::ip::tcp::v4());
            guards[i].emplace(&scanner, entries[i], sockets[i]);
            entries[i].set_phase(progress_value[i].phase_);
            entries[i].register_periodic_check(registrations[i], &progress_value[i], &progress::tick);
        }
        if (dispatcher->post([&scanner] { scanner.start(); }) != ruvia::post_status::accepted) {
            return 18;
        }
        io_context.run_for(std::chrono::milliseconds(50));
        if (sockets[0].is_open() || sockets[1].is_open()) {
            return 19;
        }
        if (dispatcher->post([&scanner] { scanner.stop(); }) != ruvia::post_status::accepted) {
            return 20;
        }
        io_context.run_for(std::chrono::milliseconds(5));
    }

    // Entry teardown invalidates registrations that happen to outlive it;
    // their own RAII reset must then be harmless.
    ruvia::connection_scanner::periodic_check_registration_type registration;
    periodic_probe probe;
    {
        ruvia::connection_scanner::entry_type entry;
        entry.register_periodic_check(registration, &probe, &periodic_probe::tick);
    }
    registration.reset();

    // An expiry callback may already be scanning while another thread destroys
    // the scanner. Destruction must wait for that callback instead of letting
    // its recursive re-arm call observe a freed scanner.
    bool scanner_lifetime_safe = true;
    {
        asio::io_context scanner_io;
        auto scanner_dispatcher = std::make_shared<ruvia::detail::worker_dispatcher>(scanner_io, 16);
        auto scanner_worker = ruvia::detail::worker_handle_access::make(scanner_dispatcher);
        auto scanner = std::make_unique<ruvia::connection_scanner>(scanner_worker,
            ruvia::connection_scanner_options{.scan_interval_ = std::chrono::milliseconds(1)});
        blocking_maintenance_probe blocking_probe;
        ruvia::connection_scanner::worker_maintenance_registration_type maintenance;
        scanner->register_worker_maintenance(
            maintenance, &blocking_probe, &blocking_maintenance_probe::check);
        if (scanner_dispatcher->post([&scanner] { scanner->start(); }) !=
            ruvia::post_status::accepted) {
            scanner_lifetime_safe = false;
        }
        std::thread scanner_thread([&] { scanner_io.run(); });
        {
            std::unique_lock lock(blocking_probe.mutex_);
            if (!blocking_probe.condition_.wait_for(lock, std::chrono::milliseconds(100),
                    [&blocking_probe] { return blocking_probe.entered_; })) {
                scanner_lifetime_safe = false;
            }
        }
        std::atomic_bool destroyed{false};
        std::thread destroyer([scanner = std::move(scanner), &destroyed]() mutable {
            scanner.reset();
            destroyed.store(true, std::memory_order_release);
        });
        if (blocking_probe.entered_) {
            const auto wait_until =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            while (!destroyed.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < wait_until) {
                std::this_thread::yield();
            }
            if (destroyed.load(std::memory_order_acquire)) {
                scanner_lifetime_safe = false;
            }
        }
        {
            std::lock_guard lock(blocking_probe.mutex_);
            blocking_probe.release_ = true;
        }
        blocking_probe.condition_.notify_one();
        destroyer.join();
        scanner_io.stop();
        scanner_thread.join();
        scanner_dispatcher->detach_context();
    }
    if (!scanner_lifetime_safe) {
        return 12;
    }

    // Scanner teardown likewise invalidates startup-owned maintenance nodes.
    ruvia::connection_scanner::worker_maintenance_registration_type maintenance_registration;
    worker_maintenance_probe maintenance_probe;
    {
        ruvia::connection_scanner scanner(worker_value, ruvia::connection_scanner_options{});
        scanner.register_worker_maintenance(
            maintenance_registration, &maintenance_probe, &worker_maintenance_probe::check);
    }
    maintenance_registration.reset();
}
