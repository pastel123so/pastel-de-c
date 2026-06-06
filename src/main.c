#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
typedef SOCKET socket_handle_t;
#define CLOSE_SOCKET closesocket
#define SOCKET_IS_INVALID(s) ((s) == INVALID_SOCKET)
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
typedef int socket_handle_t;
#define CLOSE_SOCKET close
#define SOCKET_IS_INVALID(s) ((s) < 0)
#endif

#define VECTOR_DIMS 14
#define K_NEIGHBORS 5
#define READ_BUFFER_SIZE 32768
#define RESPONSE_BUFFER_SIZE 512
#define VECTOR_SCALE 10000.0f
#define INDEX_BUCKETS 131072u
#define KD_PARTITIONS 256u
#define DEFAULT_CANDIDATE_LIMIT 3000u
#define MAX_EPOLL_EVENTS 512
#define MAX_EPOLL_FDS 65536
#define PROFILE_BUCKETS 32

typedef struct {
    int16_t vector[VECTOR_DIMS];
    uint8_t fraud;
} Reference;

typedef struct {
    int32_t left;
    int32_t right;
    uint32_t start;
    uint32_t count;
    int16_t min[VECTOR_DIMS];
    int16_t max[VECTOR_DIMS];
} KdNode;

typedef struct {
    uint32_t root;
    uint32_t start;
    uint32_t count;
} KdPartition;

typedef struct {
    Reference *items;
    uint32_t *bucket_offsets;
    uint32_t *kd_indices;
    KdNode *kd_nodes;
    KdPartition kd_partitions[KD_PARTITIONS];
    uint32_t kd_node_count;
    size_t count;
    size_t capacity;
} ReferenceSet;

typedef struct {
    socket_handle_t server;
    const ReferenceSet *references;
} WorkerArgs;

typedef struct {
    uint64_t dist[K_NEIGHBORS];
    uint8_t fraud[K_NEIGHBORS];
    int worst;
} NeighborSet;

#ifndef _WIN32
typedef struct {
    char buffer[READ_BUFFER_SIZE];
    size_t used;
} EpollConn;
#endif

typedef struct {
    double amount;
    int installments;
    char requested_at[32];
    double customer_avg_amount;
    int tx_count_24h;
    char known_merchants[32][32];
    size_t known_merchants_count;
    char merchant_id[32];
    char merchant_mcc[8];
    double merchant_avg_amount;
    bool is_online;
    bool card_present;
    double km_from_home;
    bool has_last_transaction;
    char last_timestamp[32];
    double km_from_current;
} Transaction;

static volatile sig_atomic_t running = 1;
static uint32_t g_candidate_limit = DEFAULT_CANDIDATE_LIMIT;
static float g_approval_threshold = 0.6f;
static uint64_t g_profile_every = 0;
static uint64_t g_profile_count = 0;
static uint64_t g_profile_total_ns = 0;
static uint64_t g_profile_parse_ns = 0;
static uint64_t g_profile_vector_ns = 0;
static uint64_t g_profile_search_ns = 0;
static uint64_t g_profile_send_ns = 0;
static uint64_t g_profile_total_hist[PROFILE_BUCKETS];
static uint64_t g_profile_search_hist[PROFILE_BUCKETS];
static uint64_t g_profile_send_hist[PROFILE_BUCKETS];

typedef struct {
    const char *data;
    size_t len;
} StaticResponse;

#define STATIC_RESPONSE(value) { value, sizeof(value) - 1u }

static const StaticResponse READY_RESPONSES[2] = {
    STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 18\r\nConnection: close\r\n\r\n{\"status\":\"ready\"}"),
    STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 18\r\nConnection: keep-alive\r\n\r\n{\"status\":\"ready\"}")
};

static const StaticResponse FRAUD_RESPONSES[2][K_NEIGHBORS + 1] = {
    {
        STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 35\r\nConnection: close\r\n\r\n{\"approved\":true,\"fraud_score\":0.0}"),
        STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 35\r\nConnection: close\r\n\r\n{\"approved\":true,\"fraud_score\":0.2}"),
        STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 35\r\nConnection: close\r\n\r\n{\"approved\":true,\"fraud_score\":0.4}"),
        STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 36\r\nConnection: close\r\n\r\n{\"approved\":false,\"fraud_score\":0.6}"),
        STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 36\r\nConnection: close\r\n\r\n{\"approved\":false,\"fraud_score\":0.8}"),
        STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 36\r\nConnection: close\r\n\r\n{\"approved\":false,\"fraud_score\":1.0}")
    },
    {
        STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 35\r\nConnection: keep-alive\r\n\r\n{\"approved\":true,\"fraud_score\":0.0}"),
        STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 35\r\nConnection: keep-alive\r\n\r\n{\"approved\":true,\"fraud_score\":0.2}"),
        STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 35\r\nConnection: keep-alive\r\n\r\n{\"approved\":true,\"fraud_score\":0.4}"),
        STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 36\r\nConnection: keep-alive\r\n\r\n{\"approved\":false,\"fraud_score\":0.6}"),
        STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 36\r\nConnection: keep-alive\r\n\r\n{\"approved\":false,\"fraud_score\":0.8}"),
        STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 36\r\nConnection: keep-alive\r\n\r\n{\"approved\":false,\"fraud_score\":1.0}")
    }
};

static const StaticResponse FAST_READY_RESPONSE =
    STATIC_RESPONSE("HTTP/1.1 200 OK\r\nContent-Length: 18\r\n\r\n{\"status\":\"ready\"}");

static const int DISTANCE_DIM_ORDER[VECTOR_DIMS] = {0, 2, 7, 13, 8, 12, 5, 6, 3, 4, 1, 9, 10, 11};

static void on_signal(int signum) {
    (void)signum;
    running = 0;
}

static uint64_t now_ns(void) {
#ifdef _WIN32
    return 0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
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

static void profile_add(uint64_t total_ns, uint64_t parse_ns, uint64_t vector_ns, uint64_t search_ns, uint64_t send_ns) {
    if (!g_profile_every) {
        return;
    }
    uint64_t count = __atomic_add_fetch(&g_profile_count, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_profile_total_ns, total_ns, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_profile_parse_ns, parse_ns, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_profile_vector_ns, vector_ns, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_profile_search_ns, search_ns, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_profile_send_ns, send_ns, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_profile_total_hist[profile_bucket(total_ns)], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_profile_search_hist[profile_bucket(search_ns)], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_profile_send_hist[profile_bucket(send_ns)], 1, __ATOMIC_RELAXED);
    if (count % g_profile_every == 0) {
        uint64_t total = __atomic_exchange_n(&g_profile_total_ns, 0, __ATOMIC_RELAXED);
        uint64_t parse = __atomic_exchange_n(&g_profile_parse_ns, 0, __ATOMIC_RELAXED);
        uint64_t vector = __atomic_exchange_n(&g_profile_vector_ns, 0, __ATOMIC_RELAXED);
        uint64_t search = __atomic_exchange_n(&g_profile_search_ns, 0, __ATOMIC_RELAXED);
        uint64_t send_time = __atomic_exchange_n(&g_profile_send_ns, 0, __ATOMIC_RELAXED);
        uint64_t total_hist[PROFILE_BUCKETS];
        uint64_t search_hist[PROFILE_BUCKETS];
        uint64_t send_hist[PROFILE_BUCKETS];
        uint64_t window = 0;
        for (unsigned i = 0; i < PROFILE_BUCKETS; i++) {
            total_hist[i] = __atomic_exchange_n(&g_profile_total_hist[i], 0, __ATOMIC_RELAXED);
            search_hist[i] = __atomic_exchange_n(&g_profile_search_hist[i], 0, __ATOMIC_RELAXED);
            send_hist[i] = __atomic_exchange_n(&g_profile_send_hist[i], 0, __ATOMIC_RELAXED);
            window += total_hist[i];
        }
        fprintf(stderr,
                "profile n=%llu avg_us total=%.2f parse=%.2f vector=%.2f search=%.2f send=%.2f other=%.2f "
                "p_us total=%llu/%llu/%llu search=%llu/%llu/%llu send=%llu/%llu/%llu\n",
                (unsigned long long)count,
                (double)total / (double)g_profile_every / 1000.0,
                (double)parse / (double)g_profile_every / 1000.0,
                (double)vector / (double)g_profile_every / 1000.0,
                (double)search / (double)g_profile_every / 1000.0,
                (double)send_time / (double)g_profile_every / 1000.0,
                (double)(total - parse - vector - search - send_time) / (double)g_profile_every / 1000.0,
                (unsigned long long)profile_percentile_us(total_hist, window, 50),
                (unsigned long long)profile_percentile_us(total_hist, window, 95),
                (unsigned long long)profile_percentile_us(total_hist, window, 99),
                (unsigned long long)profile_percentile_us(search_hist, window, 50),
                (unsigned long long)profile_percentile_us(search_hist, window, 95),
                (unsigned long long)profile_percentile_us(search_hist, window, 99),
                (unsigned long long)profile_percentile_us(send_hist, window, 50),
                (unsigned long long)profile_percentile_us(send_hist, window, 95),
                (unsigned long long)profile_percentile_us(send_hist, window, 99));
    }
}

static float clamp01(double value) {
    if (value < 0.0) {
        return 0.0f;
    }
    if (value > 1.0) {
        return 1.0f;
    }
    return (float)value;
}

static int16_t quantize(float value) {
    int scaled = (int)lroundf(value * VECTOR_SCALE);
    if (scaled < -10000) {
        return -10000;
    }
    if (scaled > 10000) {
        return 10000;
    }
    return (int16_t)scaled;
}

static void quantize_vector(const float input[VECTOR_DIMS], int16_t output[VECTOR_DIMS]) {
    for (int i = 0; i < VECTOR_DIMS; i++) {
        output[i] = quantize(input[i]);
    }
}

static unsigned bin_01(int16_t value, unsigned bins) {
    int clamped = value;
    if (clamped < 0) {
        clamped = 0;
    }
    if (clamped > 10000) {
        clamped = 10000;
    }
    unsigned bin = (unsigned)((uint64_t)clamped * bins / 10001u);
    return bin >= bins ? bins - 1 : bin;
}

static uint32_t bucket_key_from_bins(unsigned amount, unsigned hour, unsigned tx_count,
                                     unsigned online, unsigned card_present,
                                     unsigned unknown_merchant, unsigned mcc) {
    return ((((((amount * 8u + hour) * 8u + tx_count) * 2u + online) * 2u + card_present) * 2u + unknown_merchant) * 8u + mcc);
}

static uint32_t bucket_key(const int16_t vector[VECTOR_DIMS]) {
    unsigned amount = bin_01(vector[0], 32);
    unsigned hour = bin_01(vector[3], 8);
    unsigned tx_count = bin_01(vector[8], 8);
    unsigned online = vector[9] >= 5000 ? 1u : 0u;
    unsigned card = vector[10] >= 5000 ? 1u : 0u;
    unsigned unknown = vector[11] >= 5000 ? 1u : 0u;
    unsigned mcc = bin_01(vector[12], 8);
    return bucket_key_from_bins(amount, hour, tx_count, online, card, unknown, mcc);
}

static uint32_t kd_partition_key(const int16_t vector[VECTOR_DIMS]) {
    uint32_t key = vector[5] < 0 ? 1u : 0u;
    key |= (vector[9] > 0 ? 1u : 0u) << 1u;
    key |= (vector[10] > 0 ? 1u : 0u) << 2u;
    key |= (vector[11] > 0 ? 1u : 0u) << 3u;
    key |= bin_01(vector[12], 4) << 4u;
    key |= (vector[2] >= 8500 ? 1u : 0u) << 6u;
    key |= (vector[8] >= 4000 ? 1u : 0u) << 7u;
    return key;
}

static bool build_index(ReferenceSet *set) {
    uint32_t *counts = calloc(INDEX_BUCKETS, sizeof(*counts));
    uint32_t *positions = malloc(INDEX_BUCKETS * sizeof(*positions));
    uint32_t *offsets = malloc((INDEX_BUCKETS + 1u) * sizeof(*offsets));
    Reference *ordered = malloc(set->count * sizeof(*ordered));
    if (!counts || !positions || !offsets || !ordered) {
        free(counts);
        free(positions);
        free(offsets);
        free(ordered);
        set->bucket_offsets = NULL;
        return false;
    }

    for (uint32_t i = 0; i < set->count; i++) {
        counts[bucket_key(set->items[i].vector)]++;
    }

    offsets[0] = 0;
    for (uint32_t i = 0; i < INDEX_BUCKETS; i++) {
        offsets[i + 1u] = offsets[i] + counts[i];
        positions[i] = offsets[i];
    }

    for (uint32_t i = 0; i < set->count; i++) {
        uint32_t key = bucket_key(set->items[i].vector);
        ordered[positions[key]++] = set->items[i];
    }

    free(set->items);
    free(counts);
    free(positions);
    set->items = ordered;
    set->bucket_offsets = offsets;
    return true;
}

static uint32_t configured_candidate_limit(void) {
    const char *env = getenv("CANDIDATE_LIMIT");
    if (!env || env[0] == '\0') {
        return DEFAULT_CANDIDATE_LIMIT;
    }
    long value = strtol(env, NULL, 10);
    if (value <= 0) {
        return DEFAULT_CANDIDATE_LIMIT;
    }
    if (value > 1000000) {
        return 1000000;
    }
    return (uint32_t)value;
}

static float configured_approval_threshold(void) {
    const char *env = getenv("APPROVAL_THRESHOLD");
    if (!env || env[0] == '\0') {
        return 0.6f;
    }
    float value = strtof(env, NULL);
    if (value <= 0.0f || value > 1.0f) {
        return 0.6f;
    }
    return value;
}

static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static const char *find_key(const char *json, const char *key) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(json, pattern);
}

static bool read_number_after_key(const char *json, const char *key, double *out) {
    const char *p = find_key(json, key);
    if (!p) {
        return false;
    }
    p = strchr(p, ':');
    if (!p) {
        return false;
    }
    p = skip_ws(p + 1);
    char *end = NULL;
    errno = 0;
    double value = strtod(p, &end);
    if (errno != 0 || end == p) {
        return false;
    }
    *out = value;
    return true;
}

static bool read_int_after_key(const char *json, const char *key, int *out) {
    double value = 0.0;
    if (!read_number_after_key(json, key, &value)) {
        return false;
    }
    *out = (int)value;
    return true;
}

static bool read_bool_after_key(const char *json, const char *key, bool *out) {
    const char *p = find_key(json, key);
    if (!p) {
        return false;
    }
    p = strchr(p, ':');
    if (!p) {
        return false;
    }
    p = skip_ws(p + 1);
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool read_string_after_key(const char *json, const char *key, char *out, size_t out_size) {
    const char *p = find_key(json, key);
    if (!p) {
        return false;
    }
    p = strchr(p, ':');
    if (!p) {
        return false;
    }
    p = skip_ws(p + 1);
    if (*p != '"') {
        return false;
    }
    p++;
    const char *end = strchr(p, '"');
    if (!end) {
        return false;
    }
    size_t len = (size_t)(end - p);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static bool read_section(const char *json, const char *key, const char **start, const char **end) {
    const char *p = find_key(json, key);
    if (!p) {
        return false;
    }
    p = strchr(p, ':');
    if (!p) {
        return false;
    }
    p = skip_ws(p + 1);
    if (strncmp(p, "null", 4) == 0) {
        *start = p;
        *end = p + 4;
        return true;
    }
    if (*p != '{') {
        return false;
    }
    int depth = 0;
    bool in_string = false;
    for (const char *q = p; *q; q++) {
        if (*q == '"' && (q == p || q[-1] != '\\')) {
            in_string = !in_string;
        }
        if (in_string) {
            continue;
        }
        if (*q == '{') {
            depth++;
        } else if (*q == '}') {
            depth--;
            if (depth == 0) {
                *start = p;
                *end = q + 1;
                return true;
            }
        }
    }
    return false;
}

static void parse_known_merchants(const char *customer, Transaction *tx) {
    const char *p = find_key(customer, "known_merchants");
    if (!p) {
        return;
    }
    p = strchr(p, '[');
    if (!p) {
        return;
    }
    p++;
    while (*p && *p != ']' && tx->known_merchants_count < 32) {
        p = skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '"') {
            break;
        }
        p++;
        const char *end = strchr(p, '"');
        if (!end) {
            break;
        }
        size_t len = (size_t)(end - p);
        if (len > 31) {
            len = 31;
        }
        memcpy(tx->known_merchants[tx->known_merchants_count], p, len);
        tx->known_merchants[tx->known_merchants_count][len] = '\0';
        tx->known_merchants_count++;
        p = end + 1;
    }
}

static bool parse_transaction(const char *body, Transaction *tx) {
    memset(tx, 0, sizeof(*tx));

    const char *transaction = NULL;
    const char *customer = NULL;
    const char *merchant = NULL;
    const char *terminal = NULL;
    const char *last = NULL;
    const char *section_end = NULL;

    if (!read_section(body, "transaction", &transaction, &section_end) ||
        !read_number_after_key(transaction, "amount", &tx->amount) ||
        !read_int_after_key(transaction, "installments", &tx->installments) ||
        !read_string_after_key(transaction, "requested_at", tx->requested_at, sizeof(tx->requested_at))) {
        return false;
    }

    if (!read_section(body, "customer", &customer, &section_end) ||
        !read_number_after_key(customer, "avg_amount", &tx->customer_avg_amount) ||
        !read_int_after_key(customer, "tx_count_24h", &tx->tx_count_24h)) {
        return false;
    }
    parse_known_merchants(customer, tx);

    if (!read_section(body, "merchant", &merchant, &section_end) ||
        !read_string_after_key(merchant, "id", tx->merchant_id, sizeof(tx->merchant_id)) ||
        !read_string_after_key(merchant, "mcc", tx->merchant_mcc, sizeof(tx->merchant_mcc)) ||
        !read_number_after_key(merchant, "avg_amount", &tx->merchant_avg_amount)) {
        return false;
    }

    if (!read_section(body, "terminal", &terminal, &section_end) ||
        !read_bool_after_key(terminal, "is_online", &tx->is_online) ||
        !read_bool_after_key(terminal, "card_present", &tx->card_present) ||
        !read_number_after_key(terminal, "km_from_home", &tx->km_from_home)) {
        return false;
    }

    if (!read_section(body, "last_transaction", &last, &section_end)) {
        return false;
    }
    tx->has_last_transaction = strncmp(skip_ws(last), "null", 4) != 0;
    if (tx->has_last_transaction &&
        (!read_string_after_key(last, "timestamp", tx->last_timestamp, sizeof(tx->last_timestamp)) ||
         !read_number_after_key(last, "km_from_current", &tx->km_from_current))) {
        return false;
    }

    return true;
}

static int iso_weekday_monday0(int year, int month, int day) {
    if (month < 3) {
        month += 12;
        year--;
    }
    int k = year % 100;
    int j = year / 100;
    int h = (day + (13 * (month + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    int sunday0 = (h + 6) % 7;
    return (sunday0 + 6) % 7;
}

static long days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468L;
}

static inline int two_digits(const char *value) {
    unsigned a = (unsigned)(value[0] - '0');
    unsigned b = (unsigned)(value[1] - '0');
    return (a <= 9u && b <= 9u) ? (int)(a * 10u + b) : -1;
}

static bool parse_iso_utc(const char *value, int *year, int *month, int *day, int *hour, int *minute) {
    if (!value ||
        value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value[19] != 'Z') {
        return false;
    }

    int century = two_digits(value);
    int y = two_digits(value + 2);
    int mo = two_digits(value + 5);
    int d = two_digits(value + 8);
    int h = two_digits(value + 11);
    int mi = two_digits(value + 14);
    int sec = two_digits(value + 17);
    if (century < 0 || y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
        h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 60) {
        return false;
    }

    *year = century * 100 + y;
    *month = mo;
    *day = d;
    *hour = h;
    *minute = mi;
    return true;
}

static double minutes_between_iso(const char *older, const char *newer) {
    int y1, mo1, d1, h1, mi1;
    int y2, mo2, d2, h2, mi2;
    if (!parse_iso_utc(older, &y1, &mo1, &d1, &h1, &mi1) ||
        !parse_iso_utc(newer, &y2, &mo2, &d2, &h2, &mi2)) {
        return 1440.0;
    }
    long days1 = days_from_civil(y1, (unsigned)mo1, (unsigned)d1);
    long days2 = days_from_civil(y2, (unsigned)mo2, (unsigned)d2);
    long total1 = days1 * 1440L + h1 * 60L + mi1;
    long total2 = days2 * 1440L + h2 * 60L + mi2;
    long diff = total2 - total1;
    return diff < 0 ? 0.0 : (double)diff;
}

static float mcc_risk(const char *mcc) {
    if (strcmp(mcc, "5411") == 0) return 0.15f;
    if (strcmp(mcc, "5812") == 0) return 0.30f;
    if (strcmp(mcc, "5912") == 0) return 0.20f;
    if (strcmp(mcc, "5944") == 0) return 0.45f;
    if (strcmp(mcc, "7801") == 0) return 0.80f;
    if (strcmp(mcc, "7802") == 0) return 0.75f;
    if (strcmp(mcc, "7995") == 0) return 0.85f;
    if (strcmp(mcc, "4511") == 0) return 0.35f;
    if (strcmp(mcc, "5311") == 0) return 0.25f;
    if (strcmp(mcc, "5999") == 0) return 0.50f;
    return 0.50f;
}

static bool merchant_is_known(const Transaction *tx) {
    for (size_t i = 0; i < tx->known_merchants_count; i++) {
        if (strcmp(tx->known_merchants[i], tx->merchant_id) == 0) {
            return true;
        }
    }
    return false;
}

static bool vectorize(const Transaction *tx, float out[VECTOR_DIMS]) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    if (!parse_iso_utc(tx->requested_at, &year, &month, &day, &hour, &minute)) {
        return false;
    }

    out[0] = clamp01(tx->amount / 10000.0);
    out[1] = clamp01((double)tx->installments / 12.0);
    out[2] = tx->customer_avg_amount <= 0.0 ? 1.0f : clamp01((tx->amount / tx->customer_avg_amount) / 10.0);
    out[3] = clamp01((double)hour / 23.0);
    out[4] = clamp01((double)iso_weekday_monday0(year, month, day) / 6.0);
    if (tx->has_last_transaction) {
        out[5] = clamp01(minutes_between_iso(tx->last_timestamp, tx->requested_at) / 1440.0);
        out[6] = clamp01(tx->km_from_current / 1000.0);
    } else {
        out[5] = -1.0f;
        out[6] = -1.0f;
    }
    out[7] = clamp01(tx->km_from_home / 1000.0);
    out[8] = clamp01((double)tx->tx_count_24h / 20.0);
    out[9] = tx->is_online ? 1.0f : 0.0f;
    out[10] = tx->card_present ? 1.0f : 0.0f;
    out[11] = merchant_is_known(tx) ? 0.0f : 1.0f;
    out[12] = mcc_risk(tx->merchant_mcc);
    out[13] = clamp01(tx->merchant_avg_amount / 10000.0);
    return true;
}

static bool references_push(ReferenceSet *set, const Reference *reference) {
    if (set->count == set->capacity) {
        size_t next = set->capacity == 0 ? 1024 : set->capacity * 2;
        Reference *items = realloc(set->items, next * sizeof(*items));
        if (!items) {
            return false;
        }
        set->items = items;
        set->capacity = next;
    }
    set->items[set->count++] = *reference;
    return true;
}

static uint32_t configured_max_references(uint32_t total) {
    const char *env = getenv("MAX_REFERENCES");
    if (!env || env[0] == '\0') {
        return total;
    }
    long value = strtol(env, NULL, 10);
    if (value <= 0 || (uint32_t)value > total) {
        return total;
    }
    return (uint32_t)value;
}

static bool load_binary_references(const char *path, ReferenceSet *set) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return false;
    }

    char magic[4];
    uint32_t count = 0;
    if (fread(magic, 1, sizeof(magic), file) != sizeof(magic) ||
        memcmp(magic, "R26B", 4) != 0 ||
        fread(&count, sizeof(count), 1, file) != 1) {
        fclose(file);
        return false;
    }

    uint32_t target = configured_max_references(count);
    uint32_t stride = count / target;
    if (stride == 0) {
        stride = 1;
    }

    set->items = malloc((size_t)target * sizeof(*set->items));
    if (!set->items) {
        fclose(file);
        return false;
    }
    set->capacity = target;
    set->count = 0;

    for (uint32_t i = 0; i < count; i++) {
        Reference ref;
        if (fread(ref.vector, sizeof(int16_t), VECTOR_DIMS, file) != VECTOR_DIMS ||
            fread(&ref.fraud, sizeof(uint8_t), 1, file) != 1) {
            fclose(file);
            free(set->items);
            memset(set, 0, sizeof(*set));
            return false;
        }
        if ((i % stride) == 0 && set->count < target) {
            set->items[set->count++] = ref;
        }
    }

    fclose(file);
    return set->count > 0;
}

static bool load_kd_index(const char *path, ReferenceSet *set) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    char magic[4];
    uint32_t version = 0;
    uint32_t count = 0;
    uint32_t node_count = 0;
    if (fread(magic, 1, 4, file) != 4 ||
        memcmp(magic, "R26K", 4) != 0 ||
        fread(&version, 4, 1, file) != 1 ||
        fread(&count, 4, 1, file) != 1 ||
        fread(&node_count, 4, 1, file) != 1 ||
        version != 1u) {
        fclose(file);
        return false;
    }
    set->count = count;
    set->capacity = count;
    set->kd_node_count = node_count;
    set->kd_nodes = malloc((size_t)node_count * sizeof(*set->kd_nodes));
    set->kd_indices = malloc((size_t)count * sizeof(*set->kd_indices));
    set->items = malloc((size_t)count * sizeof(*set->items));
    if (!set->kd_nodes || !set->kd_indices || !set->items) {
        fclose(file);
        free(set->kd_nodes);
        free(set->kd_indices);
        free(set->items);
        memset(set, 0, sizeof(*set));
        return false;
    }
    if (fread(set->kd_partitions, sizeof(set->kd_partitions[0]), KD_PARTITIONS, file) != KD_PARTITIONS ||
        fread(set->kd_nodes, sizeof(set->kd_nodes[0]), node_count, file) != node_count ||
        fread(set->kd_indices, sizeof(set->kd_indices[0]), count, file) != count ||
        fread(set->items, sizeof(set->items[0]), count, file) != count) {
        fclose(file);
        free(set->kd_nodes);
        free(set->kd_indices);
        free(set->items);
        memset(set, 0, sizeof(*set));
        return false;
    }
    fclose(file);
    return true;
}

static bool load_json_references(const char *path, ReferenceSet *set) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror("fopen references");
        return false;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        fclose(file);
        return false;
    }

    char *data = malloc((size_t)size + 1);
    if (!data) {
        fclose(file);
        return false;
    }
    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return false;
    }
    data[size] = '\0';
    fclose(file);

    const char *p = data;
    while ((p = strstr(p, "\"vector\"")) != NULL) {
        Reference ref;
        memset(&ref, 0, sizeof(ref));
        p = strchr(p, '[');
        if (!p) {
            break;
        }
        p++;
        for (int i = 0; i < VECTOR_DIMS; i++) {
            p = skip_ws(p);
            char *end = NULL;
            ref.vector[i] = quantize(strtof(p, &end));
            if (end == p) {
                free(data);
                return false;
            }
            p = end;
            while (*p && *p != ',' && *p != ']') {
                p++;
            }
            if (*p == ',') {
                p++;
            }
        }
        const char *label = strstr(p, "\"label\"");
        if (!label) {
            free(data);
            return false;
        }
        ref.fraud = strstr(label, "\"fraud\"") != NULL && strstr(label, "\"fraud\"") < strstr(label, "}");
        if (!references_push(set, &ref)) {
            free(data);
            return false;
        }
        p = label + 7;
    }

    free(data);
    return set->count > 0;
}

static bool load_references(const char *path, ReferenceSet *set) {
    if (load_kd_index(path, set)) {
        return true;
    }
    if (load_binary_references(path, set)) {
        build_index(set);
        return true;
    }
    if (load_json_references(path, set)) {
        build_index(set);
        return true;
    }
    return false;
}

static uint64_t squared_distance_limited(const int16_t a[VECTOR_DIMS], const int16_t b[VECTOR_DIMS], uint64_t limit) {
    uint64_t sum = 0;
    for (int order = 0; order < VECTOR_DIMS; order++) {
        int i = DISTANCE_DIM_ORDER[order];
        int diff = (int)a[i] - (int)b[i];
        sum += (uint64_t)(diff * diff);
        if (sum >= limit) {
            return sum;
        }
    }
    return sum;
}

static void neighbors_init(NeighborSet *neighbors) {
    for (int i = 0; i < K_NEIGHBORS; i++) {
        neighbors->dist[i] = UINT64_MAX;
        neighbors->fraud[i] = 0;
    }
    neighbors->worst = 0;
}

static void neighbors_refresh_worst(NeighborSet *neighbors) {
    int worst = 0;
    for (int k = 1; k < K_NEIGHBORS; k++) {
        if (neighbors->dist[k] > neighbors->dist[worst]) {
            worst = k;
        }
    }
    neighbors->worst = worst;
}

static void consider_neighbor(const Reference *reference, const int16_t query[VECTOR_DIMS], NeighborSet *neighbors) {
    int worst = neighbors->worst;
    uint64_t dist = squared_distance_limited(query, reference->vector, neighbors->dist[worst]);
    if (dist < neighbors->dist[worst]) {
        neighbors->dist[worst] = dist;
        neighbors->fraud[worst] = reference->fraud;
        neighbors_refresh_worst(neighbors);
    }
}

static int fraud_count_from_neighbors(const NeighborSet *neighbors) {
    int frauds = 0;
    for (int i = 0; i < K_NEIGHBORS; i++) {
        if (neighbors->dist[i] != UINT64_MAX) {
            frauds += neighbors->fraud[i] ? 1 : 0;
        }
    }
    return frauds;
}

static uint64_t kd_bounds_distance(const int16_t query[VECTOR_DIMS], const int16_t min[VECTOR_DIMS],
                                   const int16_t max[VECTOR_DIMS], uint64_t limit) {
    uint64_t sum = 0;
    for (int order = 0; order < VECTOR_DIMS; order++) {
        int i = DISTANCE_DIM_ORDER[order];
        int diff = 0;
        if (query[i] < min[i]) {
            diff = (int)min[i] - (int)query[i];
        } else if (query[i] > max[i]) {
            diff = (int)query[i] - (int)max[i];
        }
        sum += (uint64_t)(diff * diff);
        if (sum >= limit) {
            return sum;
        }
    }
    return sum;
}

static void kd_search_node(const ReferenceSet *set, int32_t node_index, const int16_t query[VECTOR_DIMS],
                           NeighborSet *neighbors) {
    if (node_index < 0 || (uint32_t)node_index >= set->kd_node_count) {
        return;
    }

    const KdNode *node = &set->kd_nodes[node_index];
    uint64_t worst = neighbors->dist[neighbors->worst];
    if (kd_bounds_distance(query, node->min, node->max, worst) > worst) {
        return;
    }

    if (node->count > 0) {
        uint32_t end = node->start + node->count;
        for (uint32_t pos = node->start; pos < end; pos++) {
            uint32_t ref_index = set->kd_indices[pos];
            if (ref_index < set->count) {
                consider_neighbor(&set->items[ref_index], query, neighbors);
            }
        }
        return;
    }

    int32_t left = node->left;
    int32_t right = node->right;
    if (left < 0) {
        kd_search_node(set, right, query, neighbors);
        return;
    }
    if (right < 0) {
        kd_search_node(set, left, query, neighbors);
        return;
    }

    const KdNode *left_node = &set->kd_nodes[left];
    const KdNode *right_node = &set->kd_nodes[right];
    uint64_t left_dist = kd_bounds_distance(query, left_node->min, left_node->max, neighbors->dist[neighbors->worst]);
    uint64_t right_dist = kd_bounds_distance(query, right_node->min, right_node->max, neighbors->dist[neighbors->worst]);
    if (right_dist < left_dist) {
        kd_search_node(set, right, query, neighbors);
        kd_search_node(set, left, query, neighbors);
    } else {
        kd_search_node(set, left, query, neighbors);
        kd_search_node(set, right, query, neighbors);
    }
}

static void kd_search(const ReferenceSet *set, const int16_t query[VECTOR_DIMS], NeighborSet *neighbors) {
    uint32_t primary = kd_partition_key(query);
    if (primary < KD_PARTITIONS && set->kd_partitions[primary].root != UINT32_MAX) {
        kd_search_node(set, (int32_t)set->kd_partitions[primary].root, query, neighbors);
    }

    for (uint32_t p = 0; p < KD_PARTITIONS; p++) {
        if (p == primary || set->kd_partitions[p].root == UINT32_MAX) {
            continue;
        }
        int32_t root = (int32_t)set->kd_partitions[p].root;
        const KdNode *node = &set->kd_nodes[root];
        uint64_t worst = neighbors->dist[neighbors->worst];
        if (kd_bounds_distance(query, node->min, node->max, worst) <= worst) {
            kd_search_node(set, root, query, neighbors);
        }
    }
}

static size_t scan_bucket(const ReferenceSet *set, uint32_t key, const int16_t query[VECTOR_DIMS],
                          NeighborSet *neighbors,
                          size_t remaining) {
    size_t scanned = 0;
    if (!set->bucket_offsets || key >= INDEX_BUCKETS || remaining == 0) {
        return 0;
    }
    uint32_t begin = set->bucket_offsets[key];
    uint32_t end = set->bucket_offsets[key + 1u];
    for (uint32_t index = begin; index < end; index++) {
        consider_neighbor(&set->items[index], query, neighbors);
        scanned++;
        if (scanned >= remaining) {
            break;
        }
    }
    return scanned;
}

static int fraud_count_for_vector(const ReferenceSet *set, const float vector[VECTOR_DIMS]) {
    int16_t query[VECTOR_DIMS];
    quantize_vector(vector, query);

    NeighborSet neighbors;
    neighbors_init(&neighbors);

    if (set->kd_nodes) {
        kd_search(set, query, &neighbors);
        return fraud_count_from_neighbors(&neighbors);
    }

    if (set->bucket_offsets) {
        unsigned amount = bin_01(query[0], 32);
        unsigned hour = bin_01(query[3], 8);
        unsigned tx_count = bin_01(query[8], 8);
        unsigned online = query[9] >= 5000 ? 1u : 0u;
        unsigned card = query[10] >= 5000 ? 1u : 0u;
        unsigned unknown = query[11] >= 5000 ? 1u : 0u;
        unsigned mcc = bin_01(query[12], 8);

        size_t scanned = 0;
        size_t limit = g_candidate_limit;
        for (int radius = 0; radius <= 1; radius++) {
            for (int da = -radius; da <= radius; da++) {
                int ba = (int)amount + da;
                if (ba < 0 || ba >= 32) continue;
                for (int dh = -radius; dh <= radius; dh++) {
                    int bh = (int)hour + dh;
                    if (bh < 0 || bh >= 8) continue;
                    for (int dt = -radius; dt <= radius; dt++) {
                        int bt = (int)tx_count + dt;
                        if (bt < 0 || bt >= 8) continue;
                        for (int dm = -radius; dm <= radius; dm++) {
                            int bm = (int)mcc + dm;
                            if (bm < 0 || bm >= 8) continue;
                            if (radius > 0 && da == 0 && dh == 0 && dt == 0 && dm == 0) continue;
                            uint32_t key = bucket_key_from_bins((unsigned)ba, (unsigned)bh, (unsigned)bt,
                                                                online, card, unknown, (unsigned)bm);
                            scanned += scan_bucket(set, key, query, &neighbors, limit - scanned);
                            if (scanned >= limit) {
                                goto done_bucket_scan;
                            }
                        }
                    }
                }
            }
        }

done_bucket_scan:
        if (scanned < 2048) {
            uint32_t stride = (uint32_t)(set->count / 4096u);
            if (stride == 0) stride = 1;
            for (uint32_t i = bucket_key(query) % stride; i < set->count; i += stride) {
                consider_neighbor(&set->items[i], query, &neighbors);
            }
        }

        return fraud_count_from_neighbors(&neighbors);
    }

    for (size_t i = 0; i < set->count; i++) {
        consider_neighbor(&set->items[i], query, &neighbors);
    }

    return fraud_count_from_neighbors(&neighbors);
}

static void send_response(socket_handle_t client, int status, const char *status_text, const char *content_type, const char *body, bool keep_alive) {
    char response[RESPONSE_BUFFER_SIZE];
    int body_len = (int)strlen(body);
    int len = snprintf(response, sizeof(response),
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Type: %s\r\n"
                       "Content-Length: %d\r\n"
                       "Connection: %s\r\n"
                       "\r\n"
                       "%s",
                       status, status_text, content_type, body_len,
                       keep_alive ? "keep-alive" : "close", body);
    if (len > 0) {
        send(client, response, (int)len, 0);
    }
}

static inline void send_static_response(socket_handle_t client, const StaticResponse *response) {
    send(client, response->data, (int)response->len, 0);
}

static const char *request_body(char *buffer) {
    char *body = strstr(buffer, "\r\n\r\n");
    return body ? body + 4 : "";
}

static bool wants_close(const char *buffer) {
    const char *connection = strstr(buffer, "\r\nConnection:");
    if (!connection) {
        connection = strstr(buffer, "\r\nconnection:");
    }
    return connection && strstr(connection, "close") && strstr(connection, "close") < strstr(connection, "\r\n\r\n");
}

static int content_length(const char *buffer) {
    const char *header = strstr(buffer, "\r\nContent-Length:");
    if (!header) {
        header = strstr(buffer, "\r\ncontent-length:");
    }
    if (!header) {
        return 0;
    }
    header = strchr(header + 2, ':');
    if (!header) {
        return 0;
    }
    return atoi(header + 1);
}

static bool read_request(socket_handle_t client, char *buffer, size_t buffer_size) {
    size_t used = 0;
    int expected_body = -1;

    while (used + 1 < buffer_size) {
        int received = recv(client, buffer + used, (int)(buffer_size - used - 1), 0);
        if (received <= 0) {
            return false;
        }
        used += (size_t)received;
        buffer[used] = '\0';

        char *headers_end = strstr(buffer, "\r\n\r\n");
        if (!headers_end) {
            continue;
        }
        if (expected_body < 0) {
            expected_body = content_length(buffer);
        }
        size_t header_bytes = (size_t)(headers_end + 4 - buffer);
        if (used >= header_bytes + (size_t)expected_body) {
            return true;
        }
    }

    return false;
}

static void configure_client_socket(socket_handle_t client) {
    int yes = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof(yes));

#ifndef _WIN32
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

static bool handle_request(socket_handle_t client, const ReferenceSet *references, char *buffer) {
    bool prof = g_profile_every != 0;
    uint64_t t0 = prof ? now_ns() : 0;
    uint64_t parse_ns = 0;
    uint64_t vector_ns = 0;
    uint64_t search_ns = 0;
    uint64_t send_ns = 0;
    bool keep_alive = !wants_close(buffer);

    if (strncmp(buffer, "GET /ready ", 11) == 0) {
        uint64_t s0 = prof ? now_ns() : 0;
        send_static_response(client, &READY_RESPONSES[keep_alive ? 1 : 0]);
        if (prof) {
            send_ns = now_ns() - s0;
            profile_add(now_ns() - t0, 0, 0, 0, send_ns);
        }
        return keep_alive;
    }

    if (strncmp(buffer, "POST /fraud-score ", 18) == 0) {
        Transaction tx;
        float vector[VECTOR_DIMS];
        const char *body = request_body(buffer);
        uint64_t p0 = prof ? now_ns() : 0;
        bool parsed = parse_transaction(body, &tx);
        if (prof) parse_ns = now_ns() - p0;
        uint64_t v0 = prof ? now_ns() : 0;
        bool vectorized = parsed && vectorize(&tx, vector);
        if (prof) vector_ns = now_ns() - v0;
        if (!parsed || !vectorized) {
            uint64_t s0 = prof ? now_ns() : 0;
            send_response(client, 400, "Bad Request", "application/json", "{\"error\":\"invalid payload\"}", keep_alive);
            if (prof) {
                send_ns = now_ns() - s0;
                profile_add(now_ns() - t0, parse_ns, vector_ns, 0, send_ns);
            }
            return keep_alive;
        }

        uint64_t k0 = prof ? now_ns() : 0;
        int frauds = fraud_count_for_vector(references, vector);
        if (prof) search_ns = now_ns() - k0;
        if (frauds < 0) frauds = 0;
        if (frauds > K_NEIGHBORS) frauds = K_NEIGHBORS;
        uint64_t s0 = prof ? now_ns() : 0;
        send_static_response(client, &FRAUD_RESPONSES[keep_alive ? 1 : 0][frauds]);
        if (prof) {
            send_ns = now_ns() - s0;
            profile_add(now_ns() - t0, parse_ns, vector_ns, search_ns, send_ns);
        }
        return keep_alive;
    }

    uint64_t s0 = prof ? now_ns() : 0;
    send_response(client, 404, "Not Found", "application/json", "{\"error\":\"not found\"}", keep_alive);
    if (prof) {
        send_ns = now_ns() - s0;
        profile_add(now_ns() - t0, 0, 0, 0, send_ns);
    }
    return keep_alive;
}

static void handle_client(socket_handle_t client, const ReferenceSet *references) {
    configure_client_socket(client);

    char buffer[READ_BUFFER_SIZE];
    for (;;) {
        if (!read_request(client, buffer, sizeof(buffer))) {
            break;
        }
        if (!handle_request(client, references, buffer)) {
            break;
        }
    }
    CLOSE_SOCKET(client);
}

#ifndef _WIN32
static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

static int listen_on_unix_socket(const char *path) {
    int server = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (server < 0) {
        perror("unix socket");
        return -1;
    }
    unlink(path);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("unix bind");
        close(server);
        return -1;
    }
    if (listen(server, 256) < 0) {
        perror("unix listen");
        close(server);
        return -1;
    }
    return server;
}

static int recv_passed_fd(int control) {
    char byte = 0;
    struct iovec iov = {.iov_base = &byte, .iov_len = 1};
    char control_buf[CMSG_SPACE(sizeof(int))];
    memset(control_buf, 0, sizeof(control_buf));
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control_buf;
    msg.msg_controllen = sizeof(control_buf);
    ssize_t n = recvmsg(control, &msg, MSG_DONTWAIT);
    if (n <= 0) {
        return -1;
    }
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
        return -1;
    }
    int fd = -1;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    return fd;
}

static bool try_process_epoll_request(int fd, EpollConn *conn, const ReferenceSet *references) {
    conn->buffer[conn->used] = '\0';
    char *headers_end = strstr(conn->buffer, "\r\n\r\n");
    if (!headers_end) {
        return true;
    }
    size_t header_bytes = (size_t)(headers_end + 4 - conn->buffer);

    if (memcmp(conn->buffer, "GET /ready ", 11) == 0) {
        send_static_response(fd, &FAST_READY_RESPONSE);
        if (conn->used > header_bytes) {
            memmove(conn->buffer, conn->buffer + header_bytes, conn->used - header_bytes);
        }
        conn->used -= header_bytes;
        return true;
    }

    int expected_body = content_length(conn->buffer);
    size_t total = header_bytes + (size_t)expected_body;
    if (conn->used < total) {
        return true;
    }

    char saved = conn->buffer[total];
    conn->buffer[total] = '\0';
    bool keep_alive = handle_request(fd, references, conn->buffer);
    conn->buffer[total] = saved;
    if (!keep_alive) {
        return false;
    }
    if (conn->used > total) {
        memmove(conn->buffer, conn->buffer + total, conn->used - total);
    }
    conn->used -= total;
    return true;
}

static void close_epoll_client(int epfd, int fd, EpollConn *conns) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    if (fd >= 0 && fd < MAX_EPOLL_FDS) {
        conns[fd].used = 0;
    }
    close(fd);
}

static bool add_epoll_client(int epfd, int fd, EpollConn *conns) {
    if (fd < 0 || fd >= MAX_EPOLL_FDS) {
        close(fd);
        return false;
    }
    set_nonblocking(fd);
    configure_client_socket(fd);
    conns[fd].used = 0;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        conns[fd].used = 0;
        close(fd);
        return false;
    }
    return true;
}

static int run_fd_epoll_server(const char *path, const ReferenceSet *references) {
    int server = listen_on_unix_socket(path);
    if (server < 0) {
        return 1;
    }
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        close(server);
        return 1;
    }
    EpollConn *conns = calloc(MAX_EPOLL_FDS, sizeof(*conns));
    bool *is_control = calloc(MAX_EPOLL_FDS, sizeof(*is_control));
    if (!conns || !is_control) {
        close(epfd);
        close(server);
        free(conns);
        free(is_control);
        return 1;
    }
    if (server < MAX_EPOLL_FDS) {
        is_control[server] = true;
    }
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = server;
    epoll_ctl(epfd, EPOLL_CTL_ADD, server, &ev);

    struct epoll_event events[MAX_EPOLL_EVENTS];
    while (running) {
        int n = epoll_wait(epfd, events, MAX_EPOLL_EVENTS, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == server) {
                for (;;) {
                    int control = accept4(server, NULL, NULL, SOCK_NONBLOCK);
                    if (control < 0) break;
                    if (control < MAX_EPOLL_FDS) is_control[control] = true;
                    ev.events = EPOLLIN | EPOLLRDHUP;
                    ev.data.fd = control;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, control, &ev);
                }
                continue;
            }
            if (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
                if (fd >= 0 && fd < MAX_EPOLL_FDS && is_control[fd]) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    is_control[fd] = false;
                    close(fd);
                } else {
                    close_epoll_client(epfd, fd, conns);
                }
                continue;
            }
            if (fd >= 0 && fd < MAX_EPOLL_FDS && is_control[fd]) {
                for (;;) {
                    int client = recv_passed_fd(fd);
                    if (client < 0) break;
                    add_epoll_client(epfd, client, conns);
                }
                continue;
            }

            EpollConn *conn = (fd >= 0 && fd < MAX_EPOLL_FDS) ? &conns[fd] : NULL;
            if (!conn) {
                close(fd);
                continue;
            }
            bool alive = true;
            for (;;) {
                if (conn->used + 1 >= READ_BUFFER_SIZE) {
                    alive = false;
                    break;
                }
                ssize_t r = recv(fd, conn->buffer + conn->used, READ_BUFFER_SIZE - conn->used - 1, 0);
                if (r > 0) {
                    conn->used += (size_t)r;
                    while (alive && conn->used > 0) {
                        size_t before = conn->used;
                        alive = try_process_epoll_request(fd, conn, references);
                        if (conn->used == before) break;
                    }
                    continue;
                }
                if (r == 0) alive = false;
                if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) alive = false;
                break;
            }
            if (!alive) {
                close_epoll_client(epfd, fd, conns);
            }
        }
    }
    close(epfd);
    close(server);
    unlink(path);
    free(conns);
    free(is_control);
    return 0;
}
#endif

static socket_handle_t listen_on(uint16_t port) {
    socket_handle_t server = socket(AF_INET, SOCK_STREAM, 0);
    if (SOCKET_IS_INVALID(server)) {
        perror("socket");
        return (socket_handle_t)-1;
    }

    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        CLOSE_SOCKET(server);
        return (socket_handle_t)-1;
    }

    if (listen(server, 8192) < 0) {
        perror("listen");
        CLOSE_SOCKET(server);
        return (socket_handle_t)-1;
    }

    return server;
}

static unsigned configured_workers(void) {
    const char *workers_env = getenv("WORKERS");
    if (!workers_env || workers_env[0] == '\0') {
        return 192;
    }
    long workers = strtol(workers_env, NULL, 10);
    if (workers < 1) {
        return 1;
    }
    if (workers > 512) {
        return 512;
    }
    return (unsigned)workers;
}

static void accept_loop(socket_handle_t server, const ReferenceSet *references) {
    while (running) {
        socket_handle_t client = accept(server, NULL, NULL);
        if (SOCKET_IS_INVALID(client)) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }
        handle_client(client, references);
    }
}

#ifndef _WIN32
static void *worker_main(void *arg) {
    WorkerArgs *worker = (WorkerArgs *)arg;
    accept_loop(worker->server, worker->references);
    return NULL;
}
#endif

int main(void) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    const char *port_env = getenv("PORT");
    uint16_t port = port_env ? (uint16_t)atoi(port_env) : 9999;
    const char *references_path = getenv("REFERENCES_PATH");
    if (!references_path) {
        references_path = "resources/references.idx";
    }
    g_candidate_limit = configured_candidate_limit();
    g_approval_threshold = configured_approval_threshold();
    const char *profile_env = getenv("RINHA_PROFILE_EVERY");
    if (profile_env && profile_env[0] != '\0') {
        g_profile_every = strtoull(profile_env, NULL, 10);
    }

    ReferenceSet references = {0};
    if (!load_references(references_path, &references)) {
        fprintf(stderr, "failed to load references from %s\n", references_path);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

#ifndef _WIN32
    const char *fd_socket = getenv("RINHA_FD_SOCKET");
    if (fd_socket && fd_socket[0] != '\0') {
        fprintf(stderr, "rinha-api epoll fd socket %s with %zu references\n", fd_socket, references.count);
        return run_fd_epoll_server(fd_socket, &references);
    }
#endif

    socket_handle_t server = listen_on(port);
    if (SOCKET_IS_INVALID(server)) {
        free(references.kd_nodes);
        free(references.kd_indices);
        free(references.items);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    unsigned workers = configured_workers();
    fprintf(stderr, "rinha-api listening on :%u with %zu references, %u workers, %u candidates and %.2f threshold\n",
            port, references.count, workers, g_candidate_limit, g_approval_threshold);

#ifdef _WIN32
    accept_loop(server, &references);
#else
    pthread_t *threads = calloc(workers, sizeof(*threads));
    WorkerArgs args = {.server = server, .references = &references};
    if (!threads) {
        CLOSE_SOCKET(server);
        free(references.kd_nodes);
        free(references.kd_indices);
        free(references.items);
        return 1;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 128 * 1024);
    for (unsigned i = 0; i < workers; i++) {
        if (pthread_create(&threads[i], &attr, worker_main, &args) != 0) {
            perror("pthread_create");
            running = 0;
            workers = i;
            break;
        }
    }
    pthread_attr_destroy(&attr);
    for (unsigned i = 0; i < workers; i++) {
        pthread_join(threads[i], NULL);
    }
    free(threads);
#endif

    CLOSE_SOCKET(server);
    free(references.kd_nodes);
    free(references.kd_indices);
    free(references.bucket_offsets);
    free(references.items);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
