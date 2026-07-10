#include "lltg/order_entry/codec.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace lltg::order_entry;

namespace {

using Frame = std::array<std::byte, kFrameSize>;
using Clock = std::chrono::steady_clock;

struct Options {
    const char* host = "127.0.0.1";
    int port = 9000;
    int messages = 100000;
    int warmup = 1000;
};

Options parse_options(int argc, char** argv) {
    Options opts;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--host") == 0)
            opts.host = argv[i + 1];
        else if (std::strcmp(argv[i], "--port") == 0)
            opts.port = std::atoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--messages") == 0)
            opts.messages = std::atoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--warmup") == 0)
            opts.warmup = std::atoi(argv[i + 1]);
    }
    return opts;
}

void send_recv(int fd, Frame& frame) {
    ::send(fd, frame.data(), frame.size(), 0);
    ::recv(fd, frame.data(), frame.size(), MSG_WAITALL);
}

void encode_order(Frame& frame, std::uint64_t seq) {
    MessageHeader h{};
    h.sequence_number = seq;
    h.session_id = 1;

    NewOrder order{};
    order.client_order_id = seq;
    order.side = Side::Buy;
    order.price = 10000;
    order.quantity = 1;

    encode_new_order(h, order, frame);
}

void print_percentile(const std::vector<std::int64_t>& sorted, double p, const char* label) {
    const std::size_t idx = static_cast<std::size_t>(
        static_cast<double>(sorted.size() - 1) * p);
    std::printf("  %-10s = %'10ld ns\n", label, sorted[idx]);
}

} // namespace

int main(int argc, char** argv) {
    const Options opts = parse_options(argc, argv);

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(opts.port));
    ::inet_pton(AF_INET, opts.host, &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("connect");
        return 1;
    }

    Frame frame{};
    std::uint64_t seq = 1;

    // Warmup
    for (int i = 0; i < opts.warmup; ++i) {
        encode_order(frame, seq++);
        send_recv(fd, frame);
    }

    // Measure
    std::vector<std::int64_t> rtts(opts.messages);

    for (int i = 0; i < opts.messages; ++i) {
        encode_order(frame, seq++);
        const auto t0 = Clock::now();
        send_recv(fd, frame);
        const auto t1 = Clock::now();
        rtts[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    }

    ::close(fd);

    // Stats
    std::sort(rtts.begin(), rtts.end());

    std::int64_t sum = 0;
    for (auto v : rtts) sum += v;

    std::printf("epoll gateway RTT benchmark\n");
    std::printf("  host=%s  port=%d  messages=%d  warmup=%d\n\n",
                opts.host, opts.port, opts.messages, opts.warmup);

    print_percentile(rtts, 0.0,   "min");
    print_percentile(rtts, 0.50,  "p50");
    print_percentile(rtts, 0.90,  "p90");
    print_percentile(rtts, 0.95,  "p95");
    print_percentile(rtts, 0.99,  "p99");
    print_percentile(rtts, 0.999, "p999");
    print_percentile(rtts, 1.0,   "max");
    std::printf("  %-10s = %'10ld ns\n", "mean", sum / opts.messages);

    return 0;
}
