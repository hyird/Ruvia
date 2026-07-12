#include <optional>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/core/detail/ConnectionScanner.h"

int main() {
    asio::io_context ioContext;
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
