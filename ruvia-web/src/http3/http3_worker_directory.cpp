#include "http3/http3_worker_directory.h"

#include <array>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#endif
#if defined(__linux__)
#include <linux/filter.h>
#endif

#include <asio/error.hpp>

#include "ruvia/core/memory/pmr_object.h"

namespace ruvia::detail {
#if !defined(_WIN32)
namespace {

class unique_udp_fd final {
public:
    unique_udp_fd() noexcept = default;
    explicit unique_udp_fd(int fd) noexcept
        : fd_(fd) {}
    unique_udp_fd(unique_udp_fd&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}
    unique_udp_fd& operator=(unique_udp_fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    unique_udp_fd(const unique_udp_fd&) = delete;
    unique_udp_fd& operator=(const unique_udp_fd&) = delete;
    ~unique_udp_fd() {
        reset();
    }

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return fd_ >= 0;
    }
    [[nodiscard]] int release() noexcept {
        return std::exchange(fd_, -1);
    }
    void reset() noexcept {
        if (fd_ >= 0) {
            (void)::close(std::exchange(fd_, -1));
        }
    }

private:
    int fd_{-1};
};

unique_udp_fd open_reuseport_udp_fd(
    const asio::ip::udp& protocol, const sockaddr* address, socklen_t address_length, [[maybe_unused]] std::optional<bool> ipv6_only) {
#if !defined(SO_REUSEPORT)
    (void)protocol;
    (void)address;
    (void)address_length;
    throw std::system_error(asio::error::operation_not_supported, "HTTP/3 worker listeners require SO_REUSEPORT");
#else
    int type = SOCK_DGRAM;
#if defined(SOCK_CLOEXEC)
    type |= SOCK_CLOEXEC;
#endif
    unique_udp_fd fd(::socket(protocol.family(), type, protocol.protocol()));
    if (!fd) {
        throw std::system_error(errno, asio::system_category(), "failed to open HTTP/3 UDP listener socket");
    }
#if !defined(SOCK_CLOEXEC) && defined(FD_CLOEXEC)
    const int flags = ::fcntl(fd.get(), F_GETFD);
    if (flags == -1 || ::fcntl(fd.get(), F_SETFD, flags | FD_CLOEXEC) == -1) {
        throw std::system_error(errno, asio::system_category(), "failed to configure HTTP/3 UDP listener FD_CLOEXEC");
    }
#endif
    const int enabled = 1;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0) {
        throw std::system_error(errno, asio::system_category(), "failed to enable HTTP/3 listener SO_REUSEADDR");
    }
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
        throw std::system_error(errno, asio::system_category(), "failed to enable HTTP/3 listener SO_REUSEPORT");
    }
    if (ipv6_only) {
        const int value = *ipv6_only ? 1 : 0;
        if (::setsockopt(fd.get(), IPPROTO_IPV6, IPV6_V6ONLY, &value, sizeof(value)) != 0) {
            throw std::system_error(errno, asio::system_category(), "failed to configure HTTP/3 IPV6_V6ONLY");
        }
    }
    if (::bind(fd.get(), address, address_length) != 0) {
        throw std::system_error(errno, asio::system_category(), "failed to bind HTTP/3 UDP listener socket");
    }
    return fd;
#endif
}

#if defined(__linux__) && defined(SO_ATTACH_REUSEPORT_CBPF)
// Attaches an unprivileged cBPF socket selector that verifies server-issued
// 20-byte QUIC connection IDs (both Short Header 1-RTT packets, including
// post-migration / NAT-rebound paths, and Long Header packets carrying a
// 20-byte server DCID) using the listener secret inlined as immediate words,
// returning the unmasked owner worker index directly in the kernel. Client
// Initial packets and forged IDs fall back to a keyed hash of the packet's
// connection ID bytes so retransmissions stick to one worker while unkeyed
// floods spread across all workers.
void attach_reuseport_cbpf(int fd, const http3_secret& secret, std::size_t worker_count) {
    if (worker_count <= 1 || !std::in_range<std::uint16_t>(worker_count)) {
        return;
    }
    const auto count = static_cast<std::uint32_t>(worker_count);
    const auto k0 = read_http3_be32(secret.data());
    const auto k1 = read_http3_be32(secret.data() + 4);
    const auto k2 = read_http3_be32(secret.data() + 8);
    const auto k3 = read_http3_be32(secret.data() + 12);
    const auto k4 = read_http3_be32(secret.data() + 16);
    constexpr auto p1 = http3_cid_mix_prime_1;
    constexpr auto p2 = http3_cid_mix_prime_2;
    constexpr auto random_offset = static_cast<std::uint32_t>(SKF_AD_OFF + SKF_AD_RANDOM);

    std::array<sock_filter, 79> filter{{
        // 0..10: locate 20-byte server DCID at offset X = 1 (Short Header, len >= 21)
        // or X = 6 (Long Header, len >= 26 and DCID length == 20).
        BPF_STMT(BPF_LD | BPF_W | BPF_LEN, 0),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 21, 0, 60),
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 0),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x80, 2, 0),
        BPF_STMT(BPF_LDX | BPF_W | BPF_IMM, 1),
        BPF_STMT(BPF_JMP | BPF_JA, 5),
        BPF_STMT(BPF_LD | BPF_W | BPF_LEN, 0),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 26, 0, 54),
        BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 5),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, http3_connection_id_length, 0, 52),
        BPF_STMT(BPF_LDX | BPF_W | BPF_IMM, 6),

        // 11..21: load masked worker, tag, and random words w0..w3 from DCID[0..19].
        BPF_STMT(BPF_LD | BPF_H | BPF_IND, 0),
        BPF_STMT(BPF_ST, 0),
        BPF_STMT(BPF_LD | BPF_H | BPF_IND, 2),
        BPF_STMT(BPF_ST, 1),
        BPF_STMT(BPF_LD | BPF_W | BPF_IND, 8),
        BPF_STMT(BPF_ST, 2),
        BPF_STMT(BPF_LD | BPF_W | BPF_IND, 12),
        BPF_STMT(BPF_ST, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_IND, 16),
        BPF_STMT(BPF_ST, 4),
        BPF_STMT(BPF_LD | BPF_W | BPF_IND, 4),

        // 22..40: compute connection_id_digest(secret, dcid) into M[2].
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k0),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p1),
        BPF_STMT(BPF_LDX | BPF_MEM, 2),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k1),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p2),
        BPF_STMT(BPF_LDX | BPF_MEM, 3),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k2),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p1),
        BPF_STMT(BPF_LDX | BPF_MEM, 4),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k3),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p2),
        BPF_STMT(BPF_ST, 2),
        BPF_STMT(BPF_ALU | BPF_RSH | BPF_K, 16),
        BPF_STMT(BPF_LDX | BPF_MEM, 2),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ST, 2),

        // 41..45: unmask worker_index = (digest >> 16) ^ masked_worker and bounds-check.
        BPF_STMT(BPF_ALU | BPF_RSH | BPF_K, 16),
        BPF_STMT(BPF_LDX | BPF_MEM, 0),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, count, 14, 0),
        BPF_STMT(BPF_ST, 0),

        // 46..58: verify connection_id_tag(secret, digest, worker_index).
        BPF_STMT(BPF_LDX | BPF_MEM, 2),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k4),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p1),
        BPF_STMT(BPF_ST, 3),
        BPF_STMT(BPF_ALU | BPF_RSH | BPF_K, 16),
        BPF_STMT(BPF_LDX | BPF_MEM, 3),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0xffffu),
        BPF_STMT(BPF_LDX | BPF_MEM, 1),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_X, 0, 0, 2),
        BPF_STMT(BPF_LD | BPF_MEM, 0),
        BPF_STMT(BPF_RET | BPF_A, 0),

        // 59..61: unauthenticated 20-byte CID: route by keyed digest % worker_count.
        BPF_STMT(BPF_LD | BPF_MEM, 2),
        BPF_STMT(BPF_ALU | BPF_MOD | BPF_K, count),
        BPF_STMT(BPF_RET | BPF_A, 0),

        // 62..75: client Initial with 8..19-byte DCID: keyed hash of DCID[0..7].
        BPF_STMT(BPF_LD | BPF_W | BPF_LEN, 0),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 14, 0, 12),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 10),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k1),
        BPF_STMT(BPF_ST, 0),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 6),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_K, k0),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p1),
        BPF_STMT(BPF_LDX | BPF_MEM, 0),
        BPF_STMT(BPF_ALU | BPF_XOR | BPF_X, 0),
        BPF_STMT(BPF_ALU | BPF_MUL | BPF_K, p2),
        BPF_STMT(BPF_ALU | BPF_RSH | BPF_K, 16),
        BPF_STMT(BPF_ALU | BPF_MOD | BPF_K, count),
        BPF_STMT(BPF_RET | BPF_A, 0),

        // 76..78: sub-minimum datagram fallback.
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, random_offset),
        BPF_STMT(BPF_ALU | BPF_MOD | BPF_K, count),
        BPF_STMT(BPF_RET | BPF_A, 0),
    }};
    const sock_fprog program{
        .len = static_cast<unsigned short>(filter.size()),
        .filter = filter.data(),
    };
    if (::setsockopt(fd, SOL_SOCKET, SO_ATTACH_REUSEPORT_CBPF, &program, sizeof(program)) != 0) {
        if (errno != ENOPROTOOPT && errno != EOPNOTSUPP && errno != EPERM && errno != EACCES) {
            throw std::system_error(errno, asio::system_category(), "failed to attach HTTP/3 SO_ATTACH_REUSEPORT_CBPF");
        }
    }
}
#endif

}  // namespace
#endif

http3_worker_directory::http3_worker_directory(std::size_t worker_count)
    : slots_(process_resource()),
      listener_sockets_(process_resource()) {
    slots_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        slots_.push_back(make_pmr_object<slot>(process_resource()));
    }
}

http3_worker_directory::~http3_worker_directory() {
#if !defined(_WIN32)
    for (auto& group : listener_sockets_) {
        for (int& fd : group) {
            if (fd >= 0) {
                (void)::close(std::exchange(fd, -1));
            }
        }
    }
#endif
}

void http3_worker_directory::bind_worker(std::size_t index, const worker_handle& worker_value) {
    if (index >= slots_.size() || !worker_value.valid()) {
        throw std::invalid_argument("invalid HTTP/3 worker directory slot");
    }
    slots_[index]->worker_ = worker_value;
}

void http3_worker_directory::attach(std::size_t index, void* target, receive_type receive) noexcept {
    auto& value = *slots_[index];
    value.target_ = target;
    value.receive_ = receive;
}

void http3_worker_directory::detach(std::size_t index) noexcept {
    auto& value = *slots_[index];
    value.target_ = nullptr;
    value.receive_ = nullptr;
}

void http3_worker_directory::register_endpoint(std::size_t worker_index, http3_endpoint& endpoint,
    const asio::ip::udp::endpoint& local, std::shared_ptr<const http3_secret> secret,
    std::size_t listener_index) {
    slots_[worker_index]->endpoints_.push_back({&endpoint, local, std::move(secret), listener_index});
}

void http3_worker_directory::unregister_endpoint(std::size_t worker_index, http3_endpoint& endpoint) noexcept {
    for (auto& registration : slots_[worker_index]->endpoints_) {
        if (registration.endpoint_ == &endpoint) {
            registration.endpoint_ = nullptr;
            return;
        }
    }
}

#if !defined(_WIN32)
void http3_worker_directory::open_listener_socket(std::size_t worker_index, std::size_t listener_index,
    const asio::ip::udp::endpoint& configured_endpoint, const http3_secret& secret,
    inherited_listener_sockets* inherited, asio::ip::udp::socket& socket, std::optional<bool> ipv6_only) {
    if (worker_index >= slots_.size()) {
        throw std::invalid_argument("invalid HTTP/3 worker index");
    }
    unique_udp_fd assigned;
    {
        std::lock_guard<std::mutex> lock(bind_mutex_);
        while (listener_sockets_.size() <= listener_index) {
            listener_sockets_.emplace_back();
        }
        auto& group = listener_sockets_[listener_index];
        if (group.empty()) {
            std::pmr::vector<unique_udp_fd> opened(process_resource());
            opened.reserve(slots_.size());
            if (inherited != nullptr && !inherited->udp().empty()) {
                // Validated as bound to this endpoint, one per worker in
                // worker order: the group's members as the kernel numbers them.
                for (const auto fd : inherited->claim_udp()) {
                    opened.emplace_back(fd);
                }
            } else {
                asio::ip::udp::endpoint bound_endpoint = configured_endpoint;
                for (std::size_t index = 0; index < slots_.size(); ++index) {
                    auto fd = open_reuseport_udp_fd(configured_endpoint.protocol(), bound_endpoint.data(),
                        static_cast<socklen_t>(bound_endpoint.size()), ipv6_only);
                    if (index == 0) {
                        auto length = static_cast<socklen_t>(bound_endpoint.capacity());
                        if (::getsockname(fd.get(), bound_endpoint.data(), &length) != 0) {
                            throw std::system_error(errno, asio::system_category(), "failed to query HTTP/3 UDP listener endpoint");
                        }
                        bound_endpoint.resize(length);
                    }
                    opened.push_back(std::move(fd));
                }
            }
            if (opened.size() != slots_.size()) {
                throw std::invalid_argument("HTTP/3 listener sockets must number one per worker");
            }
#if defined(__linux__) && defined(SO_ATTACH_REUSEPORT_CBPF)
            attach_reuseport_cbpf(opened.front().get(), secret, slots_.size());
#else
            (void)secret;
#endif
            group.resize(slots_.size(), -1);
            for (std::size_t index = 0; index < opened.size(); ++index) {
                group[index] = opened[index].release();
            }
        }
        if (group[worker_index] < 0) {
            throw std::logic_error("HTTP/3 listener socket already claimed by worker");
        }
        assigned = unique_udp_fd(std::exchange(group[worker_index], -1));
    }
    socket.assign(configured_endpoint.protocol(), assigned.get());
    (void)assigned.release();
    socket.non_blocking(true);
}

void http3_worker_directory::close_unclaimed_listener_sockets(std::size_t listener_index) noexcept {
    const std::lock_guard<std::mutex> lock(bind_mutex_);
    if (listener_index < listener_sockets_.size()) {
        for (int& fd : listener_sockets_[listener_index]) {
            if (fd >= 0) {
                (void)::close(std::exchange(fd, -1));
            }
        }
    }
}
#endif

bool http3_worker_directory::hand_off(std::size_t owner, std::size_t listener_index,
    std::span<const std::byte> bytes, const asio::ip::udp::endpoint& remote,
    const asio::ip::udp::endpoint& local, std::uint32_t interface_index, std::uint32_t ecn) noexcept {
    if (owner >= slots_.size()) {
        return false;
    }
    auto& target = *slots_[owner];
    auto token = http3_in_flight_token::reserve(target.in_flight_);
    if (!token) {
        return false;
    }
    try {
        const auto status = target.worker_.post_factory([&]() -> move_only_function<void()> {
            std::pmr::vector<std::byte> copied(bytes.begin(), bytes.end(), process_resource());
            http3_handoff_datagram datagram{
                .bytes_ = std::move(copied),
                .remote_ = remote,
                .ecn_ = ecn,
                .local_ = local,
                .interface_index_ = interface_index,
            };
            return [&target, listener_index, owned = std::move(datagram), token = std::move(token)]() mutable noexcept {
                // The target pointer is confined to this, the owner's, thread.
                if (target.receive_ == nullptr) {
                    return;
                }
                for (const auto& registration : target.endpoints_) {
                    if (registration.listener_index_ != listener_index) {
                        continue;
                    }
                    if (registration.endpoint_ != nullptr) {
                        target.receive_(target.target_, listener_index, owned);
                        return;
                    }
                    // This worker may already have released the old endpoint
                    // while another worker still owns one of its connections.
                    for (const auto& fallback : target.endpoints_) {
                        if (fallback.endpoint_ != nullptr && fallback.local_ == registration.local_) {
                            target.receive_(target.target_, fallback.listener_index_, owned);
                            return;
                        }
                    }
                    return;
                }
            };
        });
        return status == post_status::accepted;
    } catch (...) {
        return false;
    }
}

}  // namespace ruvia::detail
