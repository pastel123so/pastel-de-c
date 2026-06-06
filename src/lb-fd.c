#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef SO_REUSEPORT
#define SO_REUSEPORT 15
#endif

#ifndef TCP_DEFER_ACCEPT
#define TCP_DEFER_ACCEPT 9
#endif

#define BACKEND_COUNT 2

typedef struct {
    const char *path;
    int fd;
} Backend;

#define PROFILE_BUCKETS 32

static uint64_t g_profile_every = 0;
static uint64_t g_profile_count = 0;
static uint64_t g_profile_total_ns = 0;
static uint64_t g_profile_tune_ns = 0;
static uint64_t g_profile_send_ns = 0;
static uint64_t g_profile_close_ns = 0;
static uint64_t g_profile_total_hist[PROFILE_BUCKETS];
static uint64_t g_profile_send_hist[PROFILE_BUCKETS];

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static unsigned profile_bucket(uint64_t ns) {
    uint64_t us = ns / 1000u;
    unsigned bucket = 0;
    while (us > 1u && bucket + 1u < PROFILE_BUCKETS) {
        us >>= 1u;
        bucket++;
    }
    return bucket;
}

static uint64_t profile_percentile_us(uint64_t hist[PROFILE_BUCKETS], uint64_t total, unsigned percentile) {
    if (total == 0) {
        return 0;
    }
    uint64_t target = (total * percentile + 99u) / 100u;
    uint64_t seen = 0;
    for (unsigned i = 0; i < PROFILE_BUCKETS; i++) {
        seen += hist[i];
        if (seen >= target) {
            return i == 0 ? 1u : (1ull << i);
        }
    }
    return 1ull << (PROFILE_BUCKETS - 1u);
}

static void profile_add(uint64_t total_ns, uint64_t tune_ns, uint64_t send_ns, uint64_t close_ns) {
    if (!g_profile_every) {
        return;
    }
    uint64_t count = __atomic_add_fetch(&g_profile_count, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_profile_total_ns, total_ns, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_profile_tune_ns, tune_ns, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_profile_send_ns, send_ns, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_profile_close_ns, close_ns, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_profile_total_hist[profile_bucket(total_ns)], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_profile_send_hist[profile_bucket(send_ns)], 1, __ATOMIC_RELAXED);
    if (count % g_profile_every == 0) {
        uint64_t total = __atomic_exchange_n(&g_profile_total_ns, 0, __ATOMIC_RELAXED);
        uint64_t tune = __atomic_exchange_n(&g_profile_tune_ns, 0, __ATOMIC_RELAXED);
        uint64_t send_time = __atomic_exchange_n(&g_profile_send_ns, 0, __ATOMIC_RELAXED);
        uint64_t close_time = __atomic_exchange_n(&g_profile_close_ns, 0, __ATOMIC_RELAXED);
        uint64_t total_hist[PROFILE_BUCKETS];
        uint64_t send_hist[PROFILE_BUCKETS];
        uint64_t window = 0;
        for (unsigned i = 0; i < PROFILE_BUCKETS; i++) {
            total_hist[i] = __atomic_exchange_n(&g_profile_total_hist[i], 0, __ATOMIC_RELAXED);
            send_hist[i] = __atomic_exchange_n(&g_profile_send_hist[i], 0, __ATOMIC_RELAXED);
            window += total_hist[i];
        }
        fprintf(stderr,
                "lb_profile n=%llu avg_us total=%.2f tune=%.2f sendfd=%.2f close=%.2f other=%.2f "
                "p_us total=%llu/%llu/%llu sendfd=%llu/%llu/%llu\n",
                (unsigned long long)count,
                (double)total / (double)g_profile_every / 1000.0,
                (double)tune / (double)g_profile_every / 1000.0,
                (double)send_time / (double)g_profile_every / 1000.0,
                (double)close_time / (double)g_profile_every / 1000.0,
                (double)(total - tune - send_time - close_time) / (double)g_profile_every / 1000.0,
                (unsigned long long)profile_percentile_us(total_hist, window, 50),
                (unsigned long long)profile_percentile_us(total_hist, window, 95),
                (unsigned long long)profile_percentile_us(total_hist, window, 99),
                (unsigned long long)profile_percentile_us(send_hist, window, 50),
                (unsigned long long)profile_percentile_us(send_hist, window, 95),
                (unsigned long long)profile_percentile_us(send_hist, window, 99));
    }
}

static int connect_backend(const char *path) {
    for (;;) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return -1;
        }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return fd;
        }

        close(fd);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000};
        nanosleep(&ts, NULL);
    }
}

static bool send_fd_once(int control_fd, int client_fd) {
    char byte = 1;
    struct iovec iov = {.iov_base = &byte, .iov_len = 1};
    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &client_fd, sizeof(client_fd));

    return sendmsg(control_fd, &msg, MSG_NOSIGNAL) >= 0;
}

static bool send_fd_backend(Backend *backend, int client_fd) {
    if (send_fd_once(backend->fd, client_fd)) {
        return true;
    }

    close(backend->fd);
    backend->fd = connect_backend(backend->path);
    return backend->fd >= 0 && send_fd_once(backend->fd, client_fd);
}

static void tune_client(int fd) {
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &yes, sizeof(yes));
}

static int listen_on(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

    int yes = 1;
    int defer_accept = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &defer_accept, sizeof(defer_accept));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 65535) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(void) {
    Backend backends[BACKEND_COUNT] = {
        {.path = "/sockets/api1.sock", .fd = -1},
        {.path = "/sockets/api2.sock", .fd = -1},
    };

    for (size_t i = 0; i < BACKEND_COUNT; i++) {
        backends[i].fd = connect_backend(backends[i].path);
        if (backends[i].fd < 0) {
            return 1;
        }
    }

    const char *port_env = getenv("PORT");
    int port = port_env ? atoi(port_env) : 9999;
    const char *profile_env = getenv("RINHA_LB_PROFILE_EVERY");
    if (profile_env && profile_env[0] != '\0') {
        g_profile_every = strtoull(profile_env, NULL, 10);
    }
    int server = listen_on(port);
    if (server < 0) {
        perror("listen");
        return 1;
    }

    fprintf(stderr, "rinha-fd-lb listening on :%d\n", port);
    uint32_t next = 0;

    for (;;) {
        int client = accept4(server, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
                nanosleep(&ts, NULL);
                continue;
            }
            continue;
        }

        bool prof = g_profile_every != 0;
        uint64_t t0 = prof ? now_ns() : 0;
        uint64_t x0 = prof ? now_ns() : 0;
        tune_client(client);
        uint64_t tune_ns = prof ? now_ns() - x0 : 0;
        uint32_t first = next++ & 1u;
        x0 = prof ? now_ns() : 0;
        if (!send_fd_backend(&backends[first], client)) {
            (void)send_fd_backend(&backends[first ^ 1u], client);
        }
        uint64_t send_ns = prof ? now_ns() - x0 : 0;
        x0 = prof ? now_ns() : 0;
        close(client);
        if (prof) {
            uint64_t close_ns = now_ns() - x0;
            profile_add(now_ns() - t0, tune_ns, send_ns, close_ns);
        }
    }
}
