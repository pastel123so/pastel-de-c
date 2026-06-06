#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIMS 14
#define PARTITIONS 256u
#define LEAF_SIZE 32u
#define MAGIC "R26K"
#define VERSION 1u

typedef struct {
    int16_t vector[DIMS];
    uint8_t fraud;
} Ref;

typedef struct {
    int32_t left;
    int32_t right;
    uint32_t start;
    uint32_t count;
    int16_t min[DIMS];
    int16_t max[DIMS];
} Node;

typedef struct {
    uint32_t root;
    uint32_t start;
    uint32_t count;
} Partition;

static Ref *refs;
static uint32_t *ids;
static Node *nodes;
static uint32_t node_count;
static uint32_t node_cap;
static int sort_axis;

static unsigned bin01(int16_t value, unsigned bins) {
    int clamped = value < 0 ? 0 : value > 10000 ? 10000 : value;
    unsigned bin = (unsigned)((uint64_t)clamped * bins / 10001u);
    return bin >= bins ? bins - 1u : bin;
}

static uint32_t partition_key(const int16_t v[DIMS]) {
    uint32_t key = v[5] < 0 ? 1u : 0u;
    key |= (v[9] > 0 ? 1u : 0u) << 1u;
    key |= (v[10] > 0 ? 1u : 0u) << 2u;
    key |= (v[11] > 0 ? 1u : 0u) << 3u;
    key |= bin01(v[12], 4) << 4u;
    key |= (v[2] >= 8500 ? 1u : 0u) << 6u;
    key |= (v[8] >= 4000 ? 1u : 0u) << 7u;
    return key;
}

static int cmp_axis(const void *a, const void *b) {
    uint32_t ia = *(const uint32_t *)a;
    uint32_t ib = *(const uint32_t *)b;
    int16_t va = refs[ia].vector[sort_axis];
    int16_t vb = refs[ib].vector[sort_axis];
    if (va != vb) return (va > vb) - (va < vb);
    return (ia > ib) - (ia < ib);
}

static int32_t new_node(void) {
    if (node_count == node_cap) {
        uint32_t next = node_cap ? node_cap * 2u : 65536u;
        Node *grown = realloc(nodes, (size_t)next * sizeof(*nodes));
        if (!grown) return -1;
        nodes = grown;
        node_cap = next;
    }
    return (int32_t)node_count++;
}

static int32_t build_node(uint32_t start, uint32_t count) {
    int32_t ni = new_node();
    if (ni < 0) return -1;
    Node *n = &nodes[ni];
    n->left = -1;
    n->right = -1;
    n->start = start;
    n->count = count;
    for (int d = 0; d < DIMS; d++) {
        n->min[d] = INT16_MAX;
        n->max[d] = INT16_MIN;
    }
    for (uint32_t p = start; p < start + count; p++) {
        Ref *r = &refs[ids[p]];
        for (int d = 0; d < DIMS; d++) {
            if (r->vector[d] < n->min[d]) n->min[d] = r->vector[d];
            if (r->vector[d] > n->max[d]) n->max[d] = r->vector[d];
        }
    }
    if (count <= LEAF_SIZE) return ni;

    int axis = 0;
    int range = -1;
    for (int d = 0; d < DIMS; d++) {
        int r = (int)n->max[d] - (int)n->min[d];
        if (r > range) {
            range = r;
            axis = d;
        }
    }
    sort_axis = axis;
    qsort(ids + start, count, sizeof(*ids), cmp_axis);
    uint32_t left = count / 2u;
    int32_t left_node = build_node(start, left);
    int32_t right_node = build_node(start + left, count - left);
    n = &nodes[ni];
    n->left = left_node;
    n->right = right_node;
    n->start = 0;
    n->count = 0;
    return ni;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: build-index references.bin references.idx\n");
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    if (!in) return 1;
    char magic[4];
    uint32_t count;
    if (fread(magic, 1, 4, in) != 4 || memcmp(magic, "R26B", 4) || fread(&count, 4, 1, in) != 1) return 1;
    refs = malloc((size_t)count * sizeof(*refs));
    ids = malloc((size_t)count * sizeof(*ids));
    uint32_t *counts = calloc(PARTITIONS, sizeof(*counts));
    uint32_t *offsets = malloc((PARTITIONS + 1u) * sizeof(*offsets));
    uint32_t *pos = malloc(PARTITIONS * sizeof(*pos));
    if (!refs || !ids || !counts || !offsets || !pos) return 1;
    for (uint32_t i = 0; i < count; i++) {
        fread(refs[i].vector, sizeof(int16_t), DIMS, in);
        fread(&refs[i].fraud, 1, 1, in);
        counts[partition_key(refs[i].vector)]++;
    }
    fclose(in);
    offsets[0] = 0;
    for (uint32_t p = 0; p < PARTITIONS; p++) {
        offsets[p + 1u] = offsets[p] + counts[p];
        pos[p] = offsets[p];
    }
    for (uint32_t i = 0; i < count; i++) ids[pos[partition_key(refs[i].vector)]++] = i;
    Partition parts[PARTITIONS];
    for (uint32_t p = 0; p < PARTITIONS; p++) {
        parts[p].start = offsets[p];
        parts[p].count = offsets[p + 1u] - offsets[p];
        parts[p].root = parts[p].count ? (uint32_t)build_node(parts[p].start, parts[p].count) : UINT32_MAX;
    }
    FILE *out = fopen(argv[2], "wb");
    if (!out) return 1;
    fwrite(MAGIC, 1, 4, out);
    uint32_t version = VERSION;
    fwrite(&version, 4, 1, out);
    fwrite(&count, 4, 1, out);
    fwrite(&node_count, 4, 1, out);
    fwrite(parts, sizeof(parts[0]), PARTITIONS, out);
    fwrite(nodes, sizeof(nodes[0]), node_count, out);
    fwrite(ids, sizeof(ids[0]), count, out);
    fwrite(refs, sizeof(refs[0]), count, out);
    fclose(out);
    fprintf(stderr, "wrote %u refs, %u nodes\n", count, node_count);
    return 0;
}
