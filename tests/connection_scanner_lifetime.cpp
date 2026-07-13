#include <chrono>
#include <optional>
#include <stdexcept>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/detail/ConnectionScanner.h"

int main() {
    asio::io_context ioContext;
    const auto rejects = [&ioContext](
                             ruvia::detail::ConnectionScannerOptions options) {
        try {
            ruvia::detail::ConnectionScanner scanner(
                ioContext.get_executor(),
                std::move(options));
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };

    {
        auto options = ruvia::detail::ConnectionScannerOptions{};
        options.idleTimeout = std::chrono::milliseconds(0);
        if (!rejects(std::move(options))) {
            return 1;
        }
    }
    {
        auto options = ruvia::detail::ConnectionScannerOptions{};
        options.initialReadTimeout = std::chrono::milliseconds(0);
        if (!rejects(std::move(options))) {
            return 2;
        }
    }
    {
        auto options = ruvia::detail::ConnectionScannerOptions{};
        options.payloadReadTimeout = std::chrono::milliseconds(0);
        if (!rejects(std::move(options))) {
            return 3;
        }
    }
    {
        auto options = ruvia::detail::ConnectionScannerOptions{};
        options.writeTimeout = std::chrono::milliseconds(0);
        if (!rejects(std::move(options))) {
            return 4;
        }
    }
    {
        auto options = ruvia::detail::ConnectionScannerOptions{};
        options.scanInterval = std::chrono::milliseconds(0);
        if (!rejects(std::move(options))) {
            return 5;
        }
    }

    asio::ip::tcp::socket socket(ioContext);
    ruvia::detail::ConnectionScanner::Entry firstEntry;
    ruvia::detail::ConnectionScanner::Entry secondEntry;
    std::optional<ruvia::detail::ConnectionScanner::Guard> firstGuard;
    std::optional<ruvia::detail::ConnectionScanner::Guard> secondGuard;

    {
        ruvia::detail::ConnectionScanner scanner(
            ioContext.get_executor(),
            ruvia::detail::ConnectionScannerOptions{});
        firstGuard.emplace(&scanner, firstEntry, socket);
        secondGuard.emplace(&scanner, secondEntry, socket);
    }

    // Scanner teardown must invalidate every coarse timestamp pointer and
    // detach every intrusive entry before longer-lived guards are destroyed.
    firstEntry.touch();
    firstEntry.setPhase(ruvia::detail::ConnectionScanner::Phase::kReadingInitial);
    secondEntry.touch();
    secondEntry.setPhase(ruvia::detail::ConnectionScanner::Phase::kWriting);
    firstGuard.reset();
    secondGuard.reset();
}
