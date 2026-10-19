#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <system_error>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"
#include "socket.hpp"

namespace net
{

// udp_socket sends and receives datagrams. A connected one (connect()), e.g. a client's, exchanges them with one peer,
// with read() and write(): a read is one datagram (cut short, the rest dropped, if it's longer than the buffer; an
// empty one reads as io::errc::end_of_stream, which doesn't end anything here), and a write sends one (an empty write
// sends nothing). A bound one (bind()), e.g. a server's, exchanges them with anyone, with receive_from() and send_to().
// The socket's deadlines, timeouts and stop token apply to all of them.
class udp_socket : public socket
{
public:
    // datagram is what receive_from() read: how many bytes, and who sent them.
    struct datagram
    {
        std::size_t   size = 0;
        ip_addr       addr;
        std::uint16_t port = 0;
    };

    udp_socket(io::scheduler* scheduler, io::handle fd);

    // connect makes a socket of addr's family, connected to addr and port: its writes go to them, and it only reads
    // their datagrams (and an ICMP refusal of one it sent fails its next read, with std::errc::connection_refused).
    // Nothing is sent, so it doesn't wait. It fails with the OS's errors (e.g. std::errc::network_unreachable).
    static std::expected<udp_socket, std::error_code>
    connect(io::scheduler* scheduler, const ip_addr& addr, std::uint16_t port) noexcept;

    // bind makes a socket of addr's family, bound to addr and port (port 0 lets the OS choose one: see local_port()),
    // to receive datagrams sent there. An unspecified address (ipv4_addr{} or ipv6_addr{}) receives them on all of the
    // machine's addresses of its family (and, for IPv6, IPv4's too where the OS's default allows it). It fails with the
    // OS's errors (e.g. std::errc::address_in_use).
    static std::expected<udp_socket, std::error_code>
    bind(io::scheduler* scheduler, const ip_addr& addr, std::uint16_t port) noexcept;

    // receive_from reads a datagram into data (cut short, the rest dropped, if it's longer), and says who sent it. An
    // empty datagram is read as one, of size 0. data must outlive the read.
    coro::task<std::expected<datagram, std::error_code>> receive_from(std::span<std::byte> data);

    // send_to sends data as one datagram (even if it's empty) to addr and port, which must be of the socket's family,
    // returning its size. It fails with the OS's errors, e.g. std::errc::message_size if it's too long. data must
    // outlive the write.
    coro::task<std::expected<std::size_t, std::error_code>>
    send_to(std::span<const std::byte> data, ip_addr addr, std::uint16_t port);
};

}
