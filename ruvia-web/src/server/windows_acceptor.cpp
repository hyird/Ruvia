#include "server/windows_acceptor.h"

#if defined(_WIN32)

#include <mswsock.h>
#include <ws2tcpip.h>

#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/bind_allocator.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/socket_base.hpp>
#include <asio/windows/object_handle.hpp>
#include <asio/windows/overlapped_ptr.hpp>
#if defined(RUVIA_ENABLE_HTTP3)
#include <ngtcp2/ngtcp2.h>
#endif

#include "ruvia/core/async.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/timer.h"

#include "server/connection_count.h"
#include "server/tcp_accept_error.h"
#if defined(RUVIA_ENABLE_HTTP3)
#include "http3/http3_connection_id.h"
#include "http3/http3_udp_receive.h"
#include "http3/http3_worker_directory.h"
#endif

namespace ruvia::detail {
namespace {

// AcceptEx operations kept outstanding per listener: a burst of connections
// is taken without waiting for each completion to reissue the next; the
// listen backlog holds the rest. Product limit.
constexpr std::size_t accepts_per_listener = 8;
// AcceptEx reserves each address with 16 bytes of slack.
constexpr DWORD accept_address_bytes = static_cast<DWORD>(sizeof(sockaddr_storage) + 16);
// The pause after a failed accept, so a persistent resource error does not
// spin the acceptor.
constexpr std::chrono::milliseconds accept_retry_delay{25};
// The acceptor's queue holds the datagrams workers send; each worker keeps
// at most http3_max_datagrams_in_flight in it.
constexpr std::size_t minimum_queue_capacity = 64;

[[nodiscard]] std::error_code last_socket_error() noexcept {
    return {::WSAGetLastError(), asio::system_category()};
}

// No other socket, of this process or another, may bind the endpoint while
// the acceptor holds it.
template <typename socket_type>
void claim_exclusive_address(socket_type& socket) {
    const BOOL enabled = TRUE;
    if (::setsockopt(socket.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&enabled),
            static_cast<int>(sizeof(enabled))) == SOCKET_ERROR) {
        throw std::system_error(last_socket_error(), "failed to enable listener SO_EXCLUSIVEADDRUSE");
    }
}

[[nodiscard]] LPFN_ACCEPTEX resolve_accept_ex(SOCKET listener) {
    GUID identifier = WSAID_ACCEPTEX;
    LPFN_ACCEPTEX accept_ex = nullptr;
    DWORD bytes = 0;
    if (::WSAIoctl(listener, SIO_GET_EXTENSION_FUNCTION_POINTER, &identifier, static_cast<DWORD>(sizeof(identifier)),
            &accept_ex, static_cast<DWORD>(sizeof(accept_ex)), &bytes, nullptr, nullptr) == SOCKET_ERROR) {
        throw std::system_error(last_socket_error(), "failed to resolve listener AcceptEx");
    }
    return accept_ex;
}

#if defined(RUVIA_ENABLE_HTTP3)
template <typename function_type>
[[nodiscard]] function_type resolve_message_extension(SOCKET socket, GUID identifier) {
    function_type function = nullptr;
    DWORD bytes = 0;
    if (::WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &identifier, static_cast<DWORD>(sizeof(identifier)),
            &function, static_cast<DWORD>(sizeof(function)), &bytes, nullptr, nullptr) == SOCKET_ERROR) {
        throw std::system_error(last_socket_error(), "failed to resolve UDP message extension");
    }
    return function;
}

void enable_udp_packet_info(asio::ip::udp::socket& socket) {
    const DWORD enabled = 1;
    const auto native = socket.native_handle();
    const auto enable = [&](int level, int option) {
        return ::setsockopt(native, level, option, reinterpret_cast<const char*>(&enabled),
            static_cast<int>(sizeof(enabled)));
    };
    if (socket.local_endpoint().protocol() == asio::ip::udp::v6()) {
        if (enable(IPPROTO_IPV6, IPV6_PKTINFO) == SOCKET_ERROR) {
            throw std::system_error(last_socket_error(), "failed to enable IPv6 UDP packet information");
        }
        DWORD only_v6 = 0;
        int size = sizeof(only_v6);
        if (::getsockopt(native, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&only_v6), &size) == SOCKET_ERROR) {
            throw std::system_error(last_socket_error(), "failed to inspect UDP dual-stack policy");
        }
        if (only_v6 != 0) {
            return;
        }
    }
    if (enable(IPPROTO_IP, IP_PKTINFO) == SOCKET_ERROR) {
        const auto error = last_socket_error();
        // Winsock documents WSAEINVAL on dual-stack sockets when IPv4 is disabled.
        if (error.value() == WSAEINVAL && socket.local_endpoint().protocol() == asio::ip::udp::v6()) {
            const auto probe = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (probe == INVALID_SOCKET && ::WSAGetLastError() == WSAEAFNOSUPPORT) {
                return;
            }
            if (probe != INVALID_SOCKET) {
                (void)::closesocket(probe);
            }
        }
        throw std::system_error(error, "failed to enable IPv4 UDP packet information");
    }
}

struct udp_packet_destination final {
    asio::ip::udp::endpoint local_;
    std::uint32_t interface_index_;
};

[[nodiscard]] std::optional<udp_packet_destination> packet_destination(
    WSAMSG& message, const asio::ip::udp::endpoint& remote, std::uint16_t port) {
    if ((message.dwFlags & MSG_CTRUNC) != 0) {
        return std::nullopt;
    }
    for (auto* header = WSA_CMSG_FIRSTHDR(&message); header != nullptr; header = WSA_CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_PKTINFO &&
            header->cmsg_len >= WSA_CMSG_LEN(sizeof(IN_PKTINFO))) {
            IN_PKTINFO info{};
            std::memcpy(&info, WSA_CMSG_DATA(header), sizeof(info));
            asio::ip::address_v4::bytes_type bytes{};
            std::memcpy(bytes.data(), &info.ipi_addr, bytes.size());
            asio::ip::address address{asio::ip::address_v4(bytes)};
            if (remote.address().is_v6()) {
                address = asio::ip::make_address_v6(asio::ip::v4_mapped, address.to_v4());
            }
            return udp_packet_destination{{address, port}, info.ipi_ifindex};
        }
        if (header->cmsg_level == IPPROTO_IPV6 && header->cmsg_type == IPV6_PKTINFO &&
            header->cmsg_len >= WSA_CMSG_LEN(sizeof(IN6_PKTINFO))) {
            IN6_PKTINFO info{};
            std::memcpy(&info, WSA_CMSG_DATA(header), sizeof(info));
            asio::ip::address_v6::bytes_type bytes{};
            std::memcpy(bytes.data(), &info.ipi6_addr, bytes.size());
            asio::ip::address_v6 address(bytes);
            if (address.is_link_local() || address.is_multicast_link_local()) {
                address.scope_id(info.ipi6_ifindex);
            }
            return udp_packet_destination{{address, port}, info.ipi6_ifindex};
        }
    }
    // Never invent the destination from a wildcard socket's bind address.
    return std::nullopt;
}
#endif

[[nodiscard]] std::size_t acceptor_queue_capacity(std::size_t worker_count) {
#if defined(RUVIA_ENABLE_HTTP3)
    constexpr auto max_workers =
        ((std::numeric_limits<std::size_t>::max)() - minimum_queue_capacity) / http3_max_datagrams_in_flight;
    if (worker_count > max_workers) {
        return (std::numeric_limits<std::size_t>::max)();
    }
    return minimum_queue_capacity + worker_count * http3_max_datagrams_in_flight;
#else
    (void)worker_count;
    return minimum_queue_capacity;
#endif
}

}  // namespace

windows_accepted_socket::~windows_accepted_socket() {
    if (handle_ != INVALID_SOCKET) {
        (void)::closesocket(handle_);
    }
}

struct windows_acceptor::worker_slot final {
    business_worker target_;
#if defined(RUVIA_ENABLE_HTTP3)
    // Datagrams this worker queued for sending that the acceptor has not
    // sent yet.
    std::atomic<std::size_t> egress_in_flight_{0};
#endif
};

struct windows_acceptor::tcp_listener final {
    tcp_listener(asio::io_context& io, asio::ip::tcp::endpoint endpoint, std::size_t index,
        std::optional<std::size_t> http3_index, std::shared_ptr<inherited_listener_sockets> inherited, std::optional<bool> ipv6_only)
        : socket_(io),
          endpoint_(endpoint),
          ipv6_only_(ipv6_only),
          index_(index),
          http3_index_(http3_index),
          inherited_(std::move(inherited)) {}

    asio::ip::tcp::acceptor socket_;
    asio::ip::tcp::endpoint endpoint_;
    std::optional<bool> ipv6_only_;
    // The listener's index in every business worker.
    std::size_t index_;
    // Its HTTP/3 service's index among udp_listeners_.
    std::optional<std::size_t> http3_index_;
    LPFN_ACCEPTEX accept_ex_{nullptr};
    // Until opening claims its socket.
    std::shared_ptr<inherited_listener_sockets> inherited_;
    // run_accepts() operations of this listener; a removal holds its
    // receipt until none is left.
    std::size_t accepts_{0};
    std::shared_ptr<void> removal_receipt_;
    std::optional<worker_signal> activated_;
    // From opening until removal or the acceptor's stop.
    bool admitting_{false};
};

#if defined(RUVIA_ENABLE_HTTP3)
struct windows_acceptor::udp_transport final {
    explicit udp_transport(asio::io_context& io)
        : socket_(io) {}

    asio::ip::udp::socket socket_;
    std::size_t listeners_{0};
    std::pmr::vector<std::byte> receive_buffer_{http3_max_udp_payload_bytes, process_resource()};
    bool receiving_{false};
    LPFN_WSARECVMSG receive_message_{nullptr};
    LPFN_WSASENDMSG send_message_{nullptr};
};

struct windows_acceptor::udp_listener final {
    udp_listener([[maybe_unused]] asio::io_context& io, asio::ip::udp::endpoint endpoint, std::shared_ptr<const http3_secret> secret,
        std::size_t index, std::shared_ptr<inherited_listener_sockets> inherited, std::optional<bool> ipv6_only)
        : endpoint_(endpoint),
          ipv6_only_(ipv6_only),
          secret_(std::move(secret)),
          index_(index),
          inherited_(std::move(inherited)) {}

    ~udp_listener() {
        release_transport();
    }

    void release_transport() noexcept {
        if (transport_ != nullptr && !std::exchange(transport_released_, true) && --transport_->listeners_ == 0) {
            std::error_code ignored;
            transport_->socket_.close(ignored);
        }
    }

    std::shared_ptr<udp_transport> transport_;
    asio::ip::udp::endpoint endpoint_;
    std::optional<bool> ipv6_only_;
    std::shared_ptr<const http3_secret> secret_;
    // The HTTP/3 endpoint's index in every business worker.
    std::size_t index_;
    // Until opening claims its socket.
    std::shared_ptr<inherited_listener_sockets> inherited_;
    // The last worker releases this logical listener's transport claim.
    // A re-added listener may still need the same exclusive UDP socket.
    std::size_t released_{0};
    bool transport_released_{false};
    std::optional<worker_signal> activated_;
    std::shared_ptr<void> removal_receipt_;
    bool receiving_{false};
};
#endif

windows_acceptor::windows_acceptor(std::span<const http_server_listener_definition> listeners,
    std::size_t worker_count,
#if defined(RUVIA_ENABLE_HTTP3)
    http3_worker_directory* directory,
#endif
    worker_failure_sink failure)
    : runtime_({.queue_capacity_ = acceptor_queue_capacity(worker_count),
          .io_policy_ = worker_io_policy::single_owner}),
      io_(runtime_.io_context()),
      failure_sink_(failure),
      finalize_signal_(runtime_.handle()),
      tasks_(runtime_.handle(), {.resource_ = process_resource()}),
      workers_(process_resource()),
      tcp_listeners_(process_resource())
#if defined(RUVIA_ENABLE_HTTP3)
      ,
      directory_(directory),
      udp_listeners_(process_resource())
#endif
{
    if (worker_count == 0) {
        throw std::invalid_argument("the Windows acceptor requires business workers");
    }
    workers_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers_.push_back(make_pmr_object<worker_slot>(process_resource()));
    }
    tcp_listeners_.reserve(listeners.size());
    for (std::size_t index = 0; index < listeners.size(); ++index) {
        const auto& definition = listeners[index];
        std::optional<std::size_t> http3_index;
#if defined(RUVIA_ENABLE_HTTP3)
        if (definition.http3_.has_value()) {
            if (directory_ == nullptr || definition.http3_->secret_ == nullptr) {
                throw std::logic_error("Windows HTTP/3 acceptor requires the worker directory and a secret");
            }
            http3_index = udp_listeners_.size();
            udp_listeners_.push_back(make_pmr_object<udp_listener>(process_resource(), io_,
                asio::ip::udp::endpoint(definition.endpoint_.address(), definition.http3_->port_),
                definition.http3_->secret_, *http3_index, definition.inherited_sockets_, definition.ipv6_only_));
        }
#endif
        tcp_listeners_.push_back(make_pmr_object<tcp_listener>(
            process_resource(), io_, definition.endpoint_, index, http3_index, definition.inherited_sockets_, definition.ipv6_only_));
    }
    runtime_.configure({
        .startup_ = [this] { asio::co_spawn(io_, ruvia::as_awaitable(run()),
                                 asio::bind_allocator(std::pmr::polymorphic_allocator<std::byte>(process_resource()),
                                     [this](std::exception_ptr failure) noexcept {
                                         if (failure) {
                                             (void)runtime_.fail(failure);
                                         }
                                     })); },
        .serve_ = [this] { serve_on_context(); complete_phase(false); },
        .stop_admission_ = [this] {
            stopping_ = true;
            close_admission();
#if defined(RUVIA_ENABLE_HTTP3)
            for (auto& listener : udp_listeners_) {
                if (listener != nullptr && listener->transport_ != nullptr) {
                    std::error_code ignored;
                    listener->transport_->socket_.close(ignored);
                }
            }
#endif
            runtime_.finalize(); },
        .finalize_ = [this] { finalize_signal_.notify(); },
        .failure_ = [this](std::exception_ptr failure) noexcept {
            complete_phase(true, failure);
            complete_phase(false, failure);
            failure_sink_.notify(failure); },
        .shutdown_ = [this] {
            try { throw std::logic_error("HTTP server acceptor stopped before its phase completed"); }
            catch (...) {
                const auto failure = std::current_exception();
                complete_phase(true, failure);
                complete_phase(false, failure);
            } },
    });
}

windows_acceptor::~windows_acceptor() {
    runtime_.request_stop();
    runtime_.finalize();
    runtime_.join();
}

void windows_acceptor::bind_worker(std::size_t index, business_worker target) {
    if (index >= workers_.size() || !target.worker_.valid() || target.load_ == nullptr || target.target_ == nullptr ||
        target.adopt_ == nullptr) {
        throw std::invalid_argument("invalid Windows acceptor worker");
    }
    workers_[index]->target_ = target;
}

bool windows_acceptor::start() {
    for (const auto& slot : workers_) {
        if (slot->target_.adopt_ == nullptr) {
            throw std::logic_error("Windows acceptor worker was not bound");
        }
    }
    return runtime_.start() && runtime_.wait_until_ready();
}

bool windows_acceptor::serve() {
    runtime_.request_serve();
    return runtime_.wait_until_serving();
}

void windows_acceptor::complete_phase(bool startup, std::exception_ptr failure) noexcept {
    std::optional<runtime_control_completion::receipt> receipt;
    {
        std::lock_guard lock(phase_mutex_);
        auto& pending = startup ? startup_receipt_ : serve_receipt_;
        receipt.swap(pending);
    }
    if (receipt) {
        receipt->complete(std::move(failure));
    }
}

task<void> windows_acceptor::start(worker_handle caller) {
    runtime_control_completion completion(caller);
    {
        std::lock_guard lock(phase_mutex_);
        if (startup_receipt_) {
            throw std::logic_error("HTTP server acceptor is already starting");
        }
        startup_receipt_.emplace(completion.make_receipt());
    }
    try {
        for (const auto& slot : workers_) {
            if (!slot->target_.adopt_) {
                throw std::logic_error("Windows acceptor worker was not bound");
            }
        }
        if (!runtime_.start()) {
            throw std::logic_error("HTTP server acceptor is stopping");
        }
    } catch (...) {
        complete_phase(true, std::current_exception());
    }
    co_await completion.wait();
    if (const auto failure = completion.failure()) {
        std::rethrow_exception(failure);
    }
}

task<void> windows_acceptor::serve(worker_handle caller) {
    runtime_control_completion completion(caller);
    {
        std::lock_guard lock(phase_mutex_);
        if (serve_receipt_) {
            throw std::logic_error("HTTP server acceptor is already opening");
        }
        serve_receipt_.emplace(completion.make_receipt());
    }
    try {
        runtime_.request_serve();
        if (runtime_.state() != worker_runtime_state::running) {
            throw std::logic_error("HTTP server acceptor is stopping");
        }
    } catch (...) {
        complete_phase(false, std::current_exception());
    }
    co_await completion.wait();
    if (const auto failure = completion.failure()) {
        std::rethrow_exception(failure);
    }
}

task<void> windows_acceptor::join(worker_handle caller, asio::io_context& caller_io) {
    if (!caller.is_current()) {
        throw std::logic_error("acceptor join must run on its caller worker");
    }
    const auto native = co_await runtime_.prepare_thread_join(caller);
    if (native != 0) {
        std::unique_ptr<void, decltype(&::CloseHandle)> native_owner(reinterpret_cast<HANDLE>(native), &::CloseHandle);
        asio::windows::object_handle thread(caller_io);
        std::error_code error;
        thread.assign(native_owner.get(), error);
        if (error) {
            throw std::system_error(error, "failed to assign acceptor thread wait handle");
        }
        (void)native_owner.release();
        const auto waited = co_await ruvia::async_asio([&thread](auto handler) { thread.async_wait(std::move(handler)); });
        if (waited.failed()) {
            throw std::system_error(waited.error_code(), "failed to await acceptor thread exit");
        }
    }
    runtime_.join();
}

void windows_acceptor::stop_admission() noexcept {
    // A refused control means the acceptor is already stopping, which closes
    // admission itself.
    (void)runtime_.post_control([this] { close_admission(); });
}

void windows_acceptor::request_stop() noexcept {
    runtime_.request_stop();
}

void windows_acceptor::join() {
    runtime_.join();
}

std::exception_ptr windows_acceptor::failure() noexcept {
    return runtime_.take_failure();
}

task<void> windows_acceptor::run() {
    try {
        // A stop applied before this task first runs binds nothing.
        if (!stopping_) {
            open_listeners();
            // Ignored once stopping: core has already aborted the barrier.
            runtime_.mark_ready();
            complete_phase(true);
        }
    } catch (...) {
        if (!stopping_) {
            (void)runtime_.fail(std::current_exception());
        }
    }
    co_await finalize_signal_.wait();
    try {
        co_await tasks_.join();
    } catch (...) {
        (void)runtime_.fail(std::current_exception());
    }
}

void windows_acceptor::open_listeners() {
    for (auto& listener : tcp_listeners_) {
        open_tcp_listener(*listener);
    }
#if defined(RUVIA_ENABLE_HTTP3)
    for (auto& listener : udp_listeners_) {
        open_udp_listener(*listener);
    }
#endif
    for (auto& listener : tcp_listeners_) {
        prepare_listener_tasks(*listener);
    }
}

void windows_acceptor::open_tcp_listener(tcp_listener& listener) {
    auto& socket = listener.socket_;
    const auto inherited = listener.inherited_ != nullptr ? listener.inherited_->claim_tcp(0) : invalid_native_socket;
    listener.inherited_.reset();
    if (inherited != invalid_native_socket) {
        // Checked to listen on this endpoint when it was handed over; its
        // first association with an I/O completion port is this one.
        std::error_code error;
        socket.assign(listener.endpoint_.protocol(), static_cast<SOCKET>(inherited), error);
        if (error) {
            close_native_socket(inherited);
            throw std::system_error(error, "failed to adopt an inherited TCP listener socket");
        }
    } else {
        socket.open(listener.endpoint_.protocol());
        claim_exclusive_address(socket);
        if (listener.ipv6_only_) {
            socket.set_option(asio::ip::v6_only(*listener.ipv6_only_));
        }
        socket.bind(listener.endpoint_);
        socket.listen(asio::socket_base::max_listen_connections);
    }
    listener.accept_ex_ = resolve_accept_ex(socket.native_handle());
}

#if defined(RUVIA_ENABLE_HTTP3)
void windows_acceptor::open_udp_listener(udp_listener& listener) {
    if (listener.inherited_ == nullptr || listener.inherited_->udp().empty()) {
        for (const auto& existing : udp_listeners_) {
            if (existing != nullptr && existing->endpoint_ == listener.endpoint_ &&
                existing->transport_ != nullptr && existing->transport_->socket_.is_open()) {
                if (listener.ipv6_only_ && existing->transport_->socket_.local_endpoint().address().is_v6()) {
                    asio::ip::v6_only value;
                    existing->transport_->socket_.get_option(value);
                    if (value.value() != *listener.ipv6_only_) {
                        throw std::invalid_argument("shared HTTP/3 socket IPV6_V6ONLY does not match");
                    }
                }
                listener.transport_ = existing->transport_;
                ++listener.transport_->listeners_;
                listener.inherited_.reset();
                return;
            }
        }
    }
    auto transport = std::allocate_shared<udp_transport>(
        std::pmr::polymorphic_allocator<udp_transport>(process_resource()), io_);
    auto& socket = transport->socket_;
    const auto inherited = listener.inherited_ != nullptr ? listener.inherited_->claim_udp()
                                                          : std::pmr::vector<native_socket>(process_resource());
    listener.inherited_.reset();
    if (!inherited.empty()) {
        // listen() admitted exactly one, bound to this endpoint.
        std::error_code error;
        socket.assign(listener.endpoint_.protocol(), static_cast<SOCKET>(inherited.front()), error);
        if (error) {
            close_native_socket(inherited.front());
            throw std::system_error(error, "failed to adopt an inherited UDP listener socket");
        }
    } else {
        socket.open(listener.endpoint_.protocol());
        claim_exclusive_address(socket);
        if (listener.ipv6_only_) {
            socket.set_option(asio::ip::v6_only(*listener.ipv6_only_));
        }
        socket.bind(listener.endpoint_);
    }
    enable_udp_packet_info(socket);
    transport->receive_message_ = resolve_message_extension<LPFN_WSARECVMSG>(socket.native_handle(), WSAID_WSARECVMSG);
    transport->send_message_ = resolve_message_extension<LPFN_WSASENDMSG>(socket.native_handle(), WSAID_WSASENDMSG);
    // Sending runs on this thread for every worker: a full send buffer
    // loses the datagram instead of blocking the acceptor.
    socket.non_blocking(true);
    ++transport->listeners_;
    listener.transport_ = std::move(transport);
}
#endif

void windows_acceptor::serve_on_context() {
    if (stopping_) {
        return;
    }
    admitting_ = true;
    for (auto& listener : tcp_listeners_) {
        start_listener(*listener);
    }
}

void windows_acceptor::prepare_listener_tasks(tcp_listener& listener) {
    listener.activated_.emplace(runtime_.handle());
    for (std::size_t operation = 0; operation < accepts_per_listener; ++operation) {
        tasks_.spawn(run_accepts(listener));
    }
#if defined(RUVIA_ENABLE_HTTP3)
    if (listener.http3_index_.has_value()) {
        auto& udp = *udp_listeners_[*listener.http3_index_];
        if (!udp.transport_->receiving_) {
            udp.activated_.emplace(runtime_.handle());
            tasks_.spawn(run_receives(udp));
            udp.transport_->receiving_ = true;
            udp.receiving_ = true;
        }
    }
#endif
}

void windows_acceptor::start_listener(tcp_listener& listener) noexcept {
    listener.admitting_ = true;
    listener.activated_->notify();
#if defined(RUVIA_ENABLE_HTTP3)
    if (listener.http3_index_) {
        auto& udp = *udp_listeners_[*listener.http3_index_];
        if (udp.activated_) {
            udp.activated_->notify();
        }
    }
#endif
}

void windows_acceptor::prepare_listener(std::size_t index, [[maybe_unused]] std::optional<std::size_t> http3_index,
    const http_server_listener_definition& definition
#if defined(RUVIA_ENABLE_HTTP3)
    ,
    http3_worker_directory* directory
#endif
) {
    if (stopping_) {
        throw std::logic_error("the Windows acceptor is stopping");
    }
    if (tcp_listeners_.size() <= index) {
        tcp_listeners_.resize(index + 1);
    }
    auto tcp = make_pmr_object<tcp_listener>(
        process_resource(), io_, definition.endpoint_, index, http3_index, definition.inherited_sockets_, definition.ipv6_only_);
#if defined(RUVIA_ENABLE_HTTP3)
    std::unique_ptr<udp_listener, pmr_object_deleter<udp_listener>> udp(
        nullptr, pmr_object_deleter<udp_listener>{process_resource()});
    if (http3_index.has_value()) {
        if (udp_listeners_.size() <= *http3_index) {
            udp_listeners_.resize(*http3_index + 1);
        }
        // The application creates the directory with its first HTTP/3 listener.
        if (directory_ == nullptr) {
            directory_ = directory;
        }
        udp = make_pmr_object<udp_listener>(process_resource(), io_,
            asio::ip::udp::endpoint(definition.endpoint_.address(), definition.http3_->port_),
            definition.http3_->secret_, *http3_index, definition.inherited_sockets_, definition.ipv6_only_);
    }
#endif
    open_tcp_listener(*tcp);
#if defined(RUVIA_ENABLE_HTTP3)
    if (udp != nullptr) {
        open_udp_listener(*udp);
        udp_listeners_[*http3_index] = std::move(udp);
    }
#endif
    tcp_listeners_[index] = std::move(tcp);
    try {
        prepare_listener_tasks(*tcp_listeners_[index]);
    } catch (...) {
        discard_listener(index);
        throw;
    }
}

void windows_acceptor::open_listener(std::size_t index) noexcept {
    if (!admitting_ || index >= tcp_listeners_.size() || tcp_listeners_[index] == nullptr) {
        return;
    }
    start_listener(*tcp_listeners_[index]);
}

void windows_acceptor::discard_listener(std::size_t index, std::shared_ptr<void> receipt) noexcept {
    if (index >= tcp_listeners_.size() || tcp_listeners_[index] == nullptr) {
        return;
    }
    remove_listener(index, receipt);
#if defined(RUVIA_ENABLE_HTTP3)
    if (const auto http3_index = tcp_listeners_[index]->http3_index_; http3_index.has_value()) {
        auto& udp = *udp_listeners_[*http3_index];
        if (udp.receiving_) {
            udp.removal_receipt_ = std::move(receipt);
        }
        udp.release_transport();
        if (udp.activated_) {
            udp.activated_->notify();
        }
    }
#endif
}

void windows_acceptor::remove_listener(std::size_t index, std::shared_ptr<void> receipt) noexcept {
    if (index >= tcp_listeners_.size() || tcp_listeners_[index] == nullptr) {
        return;
    }
    auto& listener = *tcp_listeners_[index];
    listener.admitting_ = false;
    if (listener.activated_) {
        listener.activated_->notify();
    }
    // Completes its pending AcceptEx operations as aborted.
    std::error_code ignored;
    listener.socket_.close(ignored);
    if (listener.accepts_ != 0) {
        listener.removal_receipt_ = std::move(receipt);
    }
}

void windows_acceptor::close_admission() noexcept {
    admitting_ = false;
    // Closing a listener completes its pending AcceptEx operations as aborted.
    for (auto& listener : tcp_listeners_) {
        if (listener != nullptr) {
            listener->admitting_ = false;
            if (listener->activated_) {
                listener->activated_->notify();
            }
            std::error_code ignored;
            listener->socket_.close(ignored);
        }
    }
#if defined(RUVIA_ENABLE_HTTP3)
    // The sockets stay open for sending until request_stop().
    for (auto& listener : udp_listeners_) {
        if (listener != nullptr && listener->transport_ != nullptr) {
            std::error_code ignored;
            if (listener->activated_) {
                listener->activated_->notify();
            }
            listener->transport_->socket_.cancel(ignored);
        }
    }
#endif
}

task<void> windows_acceptor::run_accepts(tcp_listener& listener) {
    ++listener.accepts_;
    co_await listener.activated_->wait();
    // AcceptEx writes both addresses here; the frame outlives the operation.
    std::array<std::byte, 2 * accept_address_bytes> addresses{};
    const auto protocol = listener.endpoint_.protocol();
    try {
        while (admitting_ && listener.admitting_) {
            // Not associated with any completion port: the business worker
            // that adopts it binds it to its own.
            windows_accepted_socket accepted(::WSASocketW(protocol.family(), protocol.type(), protocol.protocol(),
                nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT));
            std::error_code error;
            if (accepted.get() == INVALID_SOCKET) {
                error = last_socket_error();
            } else {
                const auto completion = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::windows::overlapped_ptr native(io_, std::move(handler));
                    DWORD bytes = 0;
                    const BOOL accepted_now = listener.accept_ex_(listener.socket_.native_handle(), accepted.get(),
                        addresses.data(), 0, accept_address_bytes, accept_address_bytes, &bytes, native.get());
                    const int accept_error = accepted_now ? 0 : ::WSAGetLastError();
                    if (accepted_now || accept_error == ERROR_IO_PENDING) {
                        // Even a synchronous success posts its completion packet.
                        (void)native.release();
                    } else {
                        native.complete(asio::error_code(accept_error, asio::system_category()), 0);
                    }
                });
                error = completion.error_code();
            }
            // A stop, or this listener's removal, aborted the accept.
            if (!admitting_ || !listener.admitting_) {
                break;
            }
            if (!error) {
                // Gives the socket the listener's properties (getpeername,
                // shutdown); no completion port is involved.
                const SOCKET listening = listener.socket_.native_handle();
                if (::setsockopt(accepted.get(), SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                        reinterpret_cast<const char*>(&listening), static_cast<int>(sizeof(listening))) == SOCKET_ERROR) {
                    error = last_socket_error();
                } else {
                    dispatch(listener.index_, std::move(accepted));
                    continue;
                }
            }
            report_accept_error(error);
            if (co_await ruvia::sleep_for(runtime_.handle(), accept_retry_delay) == timer_sleep_result::stop_requested) {
                break;
            }
        }
    } catch (...) {
        // A fatal listener failure stops the application.
        (void)runtime_.fail(std::current_exception());
    }
    // A removal waits until every accept of the listener ended.
    if (--listener.accepts_ == 0) {
        listener.removal_receipt_.reset();
    }
}

void windows_acceptor::report_accept_error(const std::error_code& error) {
    const auto action = classify_accept_error(error);
    if (action == accept_error_action::retry) {
        return;
    }
    accept_failures_.fetch_add(1, std::memory_order_relaxed);
    if (action == accept_error_action::fail) {
        throw std::system_error(error, "fatal TCP AcceptEx error");
    }
    try {
        throw std::system_error(error, "recoverable TCP AcceptEx error");
    } catch (...) {
        ruvia::report_unhandled_failure("web accept", std::current_exception());
    }
}

void windows_acceptor::dispatch(std::size_t listener_index, windows_accepted_socket accepted) {
    const auto& target = workers_[select_worker()]->target_;
    // Admission is reserved before the callable takes the socket; a refused
    // socket stays here and closes with `accepted`.
    const auto status = target.worker_.post_factory([&]() -> move_only_function<void()> {
        return [adopt = target.adopt_, owner = target.target_, listener_index, owned = std::move(accepted)]() mutable {
            adopt(owner, listener_index, std::move(owned));
        };
    });
    if (status == post_status::queue_full) {
        refused_.fetch_add(1, std::memory_order_relaxed);
    }
}

std::size_t windows_acceptor::select_worker() noexcept {
    // The worker holding the fewest connections; scanning starts after the
    // last choice, so ties (a burst) rotate across workers.
    const auto count = workers_.size();
    auto chosen = next_worker_;
    auto least = workers_[chosen]->target_.load_->active();
    for (std::size_t step = 1; step < count; ++step) {
        const auto index = (next_worker_ + step) % count;
        const auto load = workers_[index]->target_.load_->active();
        if (load < least) {
            least = load;
            chosen = index;
        }
    }
    next_worker_ = (chosen + 1) % count;
    return chosen;
}

#if defined(RUVIA_ENABLE_HTTP3)
task<void> windows_acceptor::run_receives(udp_listener& listener) {
    // A removed logical listener can release its claim while another listener
    // at this address continues receiving on the same exclusive socket.
    const auto transport = listener.transport_;
    auto& socket = transport->socket_;
    auto& buffer = transport->receive_buffer_;
    struct receipt_release final {
        udp_listener& listener_;
        ~receipt_release() {
            listener_.receiving_ = false;
            listener_.removal_receipt_.reset();
        }
    } release{listener};
    co_await listener.activated_->wait();
    try {
        while (admitting_ && socket.is_open()) {
            asio::ip::udp::endpoint remote;
            alignas(WSACMSGHDR) std::array<char,
                WSA_CMSG_SPACE(sizeof(IN6_PKTINFO)) + WSA_CMSG_SPACE(sizeof(IN_PKTINFO))>
                control{};
            WSABUF payload{};
            payload.buf = reinterpret_cast<char*>(buffer.data());
            payload.len = static_cast<ULONG>(buffer.size());
            WSAMSG message{};
            message.name = remote.data();
            message.namelen = static_cast<int>(remote.capacity());
            message.lpBuffers = &payload;
            message.dwBufferCount = 1;
            message.Control.buf = control.data();
            message.Control.len = static_cast<ULONG>(control.size());
            const auto completion = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::windows::overlapped_ptr native(io_, std::move(handler));
                DWORD received = 0;
                const int result = transport->receive_message_(
                    socket.native_handle(), &message, &received, native.get(), nullptr);
                const int error = result == 0 ? 0 : ::WSAGetLastError();
                if (result == 0 || error == WSA_IO_PENDING) {
                    (void)native.release();
                } else {
                    native.complete(asio::error_code(error, asio::system_category()), 0);
                }
            });
            // A stop, or the last worker's release of a removed listener.
            if (!admitting_ || !socket.is_open()) {
                co_return;
            }
            if (const auto error = completion.error_code()) {
                if (http3_ignorable_receive_error(error)) {
                    continue;
                }
                throw std::system_error(error, "HTTP/3 UDP receive failed");
            }
            remote.resize(static_cast<std::size_t>(message.namelen));
            const auto destination = packet_destination(message, remote, listener.endpoint_.port());
            if (destination) {
                steer(listener, std::span<const std::byte>(buffer.data(), completion.result()),
                    remote, destination->local_, destination->interface_index_);
            }
        }
    } catch (...) {
        // The listener cannot receive any more: its connections time out and
        // the failure is reported once.
        ruvia::report_unhandled_failure("HTTP/3 receive", std::current_exception());
    }
}

void windows_acceptor::steer(
    const udp_listener& listener, std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote,
    const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    ngtcp2_version_cid header{};
    const auto decoded = ngtcp2_pkt_decode_version_cid(
        &header, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), http3_connection_id_length);
    if (decoded != 0 && decoded != NGTCP2_ERR_VERSION_NEGOTIATION) {
        return;
    }
    const auto destination = std::span<const std::uint8_t>(header.dcid, header.dcidlen);
    std::optional<std::size_t> owner;
    std::size_t listener_index = listener.index_;
    const udp_listener* initial_listener = &listener;
    for (const auto& candidate : udp_listeners_) {
        if (candidate != nullptr && candidate->transport_ == listener.transport_) {
            if (!candidate->transport_released_) {
                initial_listener = candidate.get();
            }
            if (const auto identified = http3_connection_id_owner(*candidate->secret_, destination, workers_.size())) {
                owner = identified;
                listener_index = candidate->index_;
                break;
            }
        }
    }
    if (!owner.has_value()) {
        // Only a long header can open a connection or earn Version
        // Negotiation; a short header naming no worker is dropped.
        if (header.version == 0) {
            return;
        }
        listener_index = initial_listener->index_;
        owner = http3_initial_worker(*initial_listener->secret_, destination, workers_.size());
    }
    (void)directory_->hand_off(*owner, listener_index, bytes, remote, local, interface_index);
}

bool windows_acceptor::transmit(std::size_t worker_index, std::size_t http3_listener_index,
    const asio::ip::udp::endpoint& remote, std::span<const std::byte> bytes,
    const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept {
    auto token = http3_in_flight_token::reserve(workers_[worker_index]->egress_in_flight_);
    if (!token) {
        return false;
    }
    try {
        const auto status = runtime_.handle().post_factory([&]() -> move_only_function<void()> {
            std::pmr::vector<std::byte> owned(bytes.begin(), bytes.end(), process_resource());
            return [this, http3_listener_index, remote, local, interface_index, owned = std::move(owned),
                       token = std::move(token)]() mutable noexcept {
                send_datagram(http3_listener_index, remote, owned, local, interface_index);
            };
        });
        return status == post_status::accepted;
    } catch (...) {
        return false;
    }
}

void windows_acceptor::send_datagram(std::size_t http3_listener_index, const asio::ip::udp::endpoint& remote,
    std::span<const std::byte> bytes, const asio::ip::udp::endpoint& local,
    std::uint32_t interface_index) noexcept {
    // Errors (a full send buffer, a closed socket, an unreachable peer) lose
    // the datagram, as the network may; QUIC recovery handles the peer.
    if (const auto& listener = udp_listeners_[http3_listener_index]; listener != nullptr && listener->transport_ != nullptr) {
        auto& transport = *listener->transport_;
        if (!transport.socket_.is_open() || local.address().is_unspecified()) {
            return;
        }
        auto destination = remote;
        WSABUF payload{};
        payload.buf = const_cast<char*>(reinterpret_cast<const char*>(bytes.data()));
        payload.len = static_cast<ULONG>(bytes.size());
        alignas(WSACMSGHDR) std::array<char, WSA_CMSG_SPACE(sizeof(IN6_PKTINFO))> control{};
        WSAMSG message{};
        message.name = destination.data();
        message.namelen = static_cast<int>(destination.size());
        message.lpBuffers = &payload;
        message.dwBufferCount = 1;
        message.Control.buf = control.data();
        const auto source = local.address();
        const bool ipv4 = remote.address().is_v4() ||
                          (remote.address().is_v6() && remote.address().to_v6().is_v4_mapped());
        auto* header = reinterpret_cast<WSACMSGHDR*>(control.data());
        if (ipv4) {
            asio::ip::address_v4::bytes_type address{};
            if (source.is_v4()) {
                address = source.to_v4().to_bytes();
            } else if (source.to_v6().is_v4_mapped()) {
                const auto mapped = source.to_v6().to_bytes();
                std::memcpy(address.data(), mapped.data() + 12, address.size());
            } else {
                return;
            }
            IN_PKTINFO info{};
            std::memcpy(&info.ipi_addr, address.data(), address.size());
            info.ipi_ifindex = interface_index;
            message.Control.len = static_cast<ULONG>(WSA_CMSG_SPACE(sizeof(info)));
            header->cmsg_level = IPPROTO_IP;
            header->cmsg_type = IP_PKTINFO;
            header->cmsg_len = WSA_CMSG_LEN(sizeof(info));
            std::memcpy(WSA_CMSG_DATA(header), &info, sizeof(info));
        } else {
            if (!source.is_v6()) {
                return;
            }
            IN6_PKTINFO info{};
            const auto address = source.to_v6().to_bytes();
            std::memcpy(&info.ipi6_addr, address.data(), address.size());
            info.ipi6_ifindex = interface_index == 0
                                    ? static_cast<ULONG>(source.to_v6().scope_id())
                                    : interface_index;
            message.Control.len = static_cast<ULONG>(WSA_CMSG_SPACE(sizeof(info)));
            header->cmsg_level = IPPROTO_IPV6;
            header->cmsg_type = IPV6_PKTINFO;
            header->cmsg_len = WSA_CMSG_LEN(sizeof(info));
            std::memcpy(WSA_CMSG_DATA(header), &info, sizeof(info));
        }
        DWORD sent = 0;
        (void)transport.send_message_(transport.socket_.native_handle(), &message, 0, &sent, nullptr, nullptr);
    }
}

void windows_acceptor::release_http3_listener(std::size_t http3_listener_index) noexcept {
    // A refused control means the acceptor is stopping, which closes every
    // socket itself.
    (void)runtime_.post_control([this, http3_listener_index] {
        auto& listener = *udp_listeners_[http3_listener_index];
        if (++listener.released_ == workers_.size()) {
            listener.release_transport();
        }
    });
}
#endif

}  // namespace ruvia::detail

#endif
