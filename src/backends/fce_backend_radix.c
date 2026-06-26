#include "../internal/fce_internal.h"

static FceStatus radix_add_node(RadixTmpNode **nodes, size_t *count, size_t *cap, uint32_t *out_index) {
    if (*count >= *cap) {
        size_t nc = *cap ? *cap * 2 : 64;
        if (nc < *cap || nc > SIZE_MAX / sizeof(RadixTmpNode)) return FCE_ERR_OUT_OF_MEMORY;
        RadixTmpNode *nn = (RadixTmpNode *)fce_xrealloc(*nodes, nc * sizeof(*nn));
        if (!nn) return FCE_ERR_OUT_OF_MEMORY;
        memset(nn + *cap, 0, (nc - *cap) * sizeof(*nn));
        *nodes = nn;
        *cap = nc;
    }
    if (*count > UINT32_MAX) return FCE_ERR_OUT_OF_MEMORY;
    (*nodes)[*count].value_index = FCE_RADIX_NO_VALUE;
    *out_index = (uint32_t)(*count)++;
    return FCE_OK;
}

static FceStatus radix_find_or_add_edge(RadixTmpNode **nodes_ptr, size_t *node_count, size_t *node_cap, uint32_t node_index, uint8_t label, uint32_t *out_child) {
    RadixTmpNode *nodes = *nodes_ptr;
    RadixTmpNode *node = &nodes[node_index];
    size_t pos = 0;
    while (pos < node->edge_count && node->edges[pos].label < label) pos++;
    if (pos < node->edge_count && node->edges[pos].label == label) {
        *out_child = node->edges[pos].child_index;
        return FCE_OK;
    }
    if (node->edge_count >= node->edge_cap) {
        size_t nc = node->edge_cap ? node->edge_cap * 2 : 4;
        if (nc < node->edge_cap || nc > SIZE_MAX / sizeof(RadixTmpEdge)) return FCE_ERR_OUT_OF_MEMORY;
        RadixTmpEdge *ne = (RadixTmpEdge *)fce_xrealloc(node->edges, nc * sizeof(*ne));
        if (!ne) return FCE_ERR_OUT_OF_MEMORY;
        node->edges = ne;
        node->edge_cap = nc;
    }
    uint32_t child = 0;
    FceStatus st = radix_add_node(nodes_ptr, node_count, node_cap, &child);
    if (st != FCE_OK) return st;
    nodes = *nodes_ptr;
    node = &nodes[node_index];
    if (pos < node->edge_count) {
        memmove(node->edges + pos + 1, node->edges + pos, (node->edge_count - pos) * sizeof(*node->edges));
    }
    node->edges[pos].label = label;
    node->edges[pos].child_index = child;
    node->edge_count++;
    *out_child = child;
    return FCE_OK;
}

static void radix_tmp_free(RadixTmpNode *nodes, size_t count) {
    if (!nodes) return;
    for (size_t i = 0; i < count; i++) fce_free(nodes[i].edges);
    fce_free(nodes);
}

FceStatus freeze_radix(FceBuilder *b) {
    BuildRecord *records = NULL;
    size_t count = 0;
    FceStatus st = dedupe_records(b, &records, &count);
    if (st != FCE_OK) return st;
    SortedEntry *entries = (SortedEntry *)fce_xcalloc(count ? count : 1, sizeof(*entries));
    if (!entries) {
        fce_free(records);
        return FCE_ERR_OUT_OF_MEMORY;
    }
    uint64_t keys_size = 0, values_size = 0;
    st = write_keys_values(b, records, count, entries, &keys_size, &values_size);
    if (st != FCE_OK) {
        fce_free(entries);
        fce_free(records);
        return st;
    }

    RadixTmpNode *tmp_nodes = NULL;
    size_t node_count = 0, node_cap = 0;
    uint32_t root = 0;
    st = radix_add_node(&tmp_nodes, &node_count, &node_cap, &root);
    if (st != FCE_OK) {
        fce_free(entries);
        fce_free(records);
        return st;
    }
    for (size_t i = 0; i < count && st == FCE_OK; i++) {
        uint32_t node = root;
        for (size_t j = 0; j < records[i].key_len; j++) {
            st = radix_find_or_add_edge(&tmp_nodes, &node_count, &node_cap, node, records[i].key[j], &node);
        }
        if (st == FCE_OK) tmp_nodes[node].value_index = (uint32_t)i;
    }
    if (st != FCE_OK || node_count > UINT32_MAX || count > UINT32_MAX) {
        radix_tmp_free(tmp_nodes, node_count);
        fce_free(entries);
        fce_free(records);
        return st == FCE_OK ? FCE_ERR_OUT_OF_MEMORY : st;
    }

    size_t edge_count = 0;
    for (size_t i = 0; i < node_count; i++) edge_count += tmp_nodes[i].edge_count;
    if (edge_count > UINT32_MAX) {
        radix_tmp_free(tmp_nodes, node_count);
        fce_free(entries);
        fce_free(records);
        return FCE_ERR_OUT_OF_MEMORY;
    }
    size_t nodes_bytes = node_count * sizeof(RadixNode);
    size_t edges_bytes = edge_count * sizeof(RadixEdge);
    size_t values_bytes = count * sizeof(RadixValue);
    if (sizeof(RadixHeader) > SIZE_MAX - nodes_bytes ||
        sizeof(RadixHeader) + nodes_bytes > SIZE_MAX - edges_bytes ||
        sizeof(RadixHeader) + nodes_bytes + edges_bytes > SIZE_MAX - values_bytes) {
        radix_tmp_free(tmp_nodes, node_count);
        fce_free(entries);
        fce_free(records);
        return FCE_ERR_OUT_OF_MEMORY;
    }
    size_t total = sizeof(RadixHeader) + nodes_bytes + edges_bytes + values_bytes;
    uint8_t *buf = (uint8_t *)fce_xcalloc(1, total ? total : 1);
    if (!buf) {
        radix_tmp_free(tmp_nodes, node_count);
        fce_free(entries);
        fce_free(records);
        return FCE_ERR_OUT_OF_MEMORY;
    }
    RadixHeader *h = (RadixHeader *)buf;
    RadixNode *nodes = (RadixNode *)(buf + sizeof(RadixHeader));
    RadixEdge *edges = (RadixEdge *)(buf + sizeof(RadixHeader) + nodes_bytes);
    RadixValue *values = (RadixValue *)(buf + sizeof(RadixHeader) + nodes_bytes + edges_bytes);
    h->magic = FCE_RADIX_MAGIC;
    h->node_count = node_count;
    h->edge_count = edge_count;
    h->value_count = count;
    h->nodes_offset = sizeof(RadixHeader);
    h->edges_offset = sizeof(RadixHeader) + nodes_bytes;
    h->values_offset = sizeof(RadixHeader) + nodes_bytes + edges_bytes;
    size_t edge_pos = 0;
    for (size_t i = 0; i < node_count; i++) {
        nodes[i].first_edge = (uint32_t)edge_pos;
        nodes[i].edge_count = (uint32_t)tmp_nodes[i].edge_count;
        nodes[i].value_index = tmp_nodes[i].value_index;
        for (size_t j = 0; j < tmp_nodes[i].edge_count; j++) {
            edges[edge_pos].label = tmp_nodes[i].edges[j].label;
            edges[edge_pos].child_index = tmp_nodes[i].edges[j].child_index;
            edge_pos++;
        }
    }
    for (size_t i = 0; i < count; i++) {
        values[i].key_offset = entries[i].key_offset;
        values[i].key_len = entries[i].key_len;
        values[i].value_offset = entries[i].value_offset;
        values[i].value_len = entries[i].value_len;
    }
    char *ip = join_path_arena(b->arena, b->cache_dir, FCE_INDEX_FILE);
    st = ip ? write_file(ip, buf, total) : FCE_ERR_OUT_OF_MEMORY;
    if (st == FCE_OK) {
        FceManifestInfo m = make_manifest(&b->schema, (uint64_t)count);
        m.index_size = total;
        m.keys_size = keys_size;
        m.values_size = values_size;
        m.backend_meta0 = node_count;
        char *kp = join_path_arena(b->arena, b->cache_dir, FCE_KEYS_FILE);
        char *vp = join_path_arena(b->arena, b->cache_dir, FCE_VALUES_FILE);
        if (!kp || !vp) st = FCE_ERR_OUT_OF_MEMORY;
        else {
            m.index_crc64 = crc64_file(ip);
            m.keys_crc64 = crc64_file(kp);
            m.values_crc64 = crc64_file(vp);
            st = write_manifest(b->cache_dir, &m);
        }
    }
    fce_free(buf);
    radix_tmp_free(tmp_nodes, node_count);
    fce_free(entries);
    fce_free(records);
    return st;
}

int radix_parts(FceReader *r, RadixHeader **out_h, RadixNode **out_nodes, RadixEdge **out_edges, RadixValue **out_values) {
    if (r->index_blob.size < sizeof(RadixHeader)) return 0;
    RadixHeader *h = (RadixHeader *)r->index_blob.data;
    if (h->magic != FCE_RADIX_MAGIC) return 0;
    *out_h = h;
    *out_nodes = (RadixNode *)(r->index_blob.data + h->nodes_offset);
    *out_edges = (RadixEdge *)(r->index_blob.data + h->edges_offset);
    *out_values = (RadixValue *)(r->index_blob.data + h->values_offset);
    return 1;
}

int radix_find_child(RadixNode *nodes, RadixEdge *edges, uint32_t node_index, uint8_t label, uint32_t *out_child) {
    RadixNode *node = &nodes[node_index];
    uint32_t lo = 0, hi = node->edge_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        RadixEdge *e = &edges[node->first_edge + mid];
        if (e->label < label) lo = mid + 1;
        else hi = mid;
    }
    if (lo < node->edge_count) {
        RadixEdge *e = &edges[node->first_edge + lo];
        if (e->label == label) {
            *out_child = e->child_index;
            return 1;
        }
    }
    return 0;
}

FceStatus get_radix(FceReader *r, const void *key, size_t key_len, const void **out_value, size_t *out_value_len) {
    RadixHeader *h;
    RadixNode *nodes;
    RadixEdge *edges;
    RadixValue *values;
    if (!radix_parts(r, &h, &nodes, &edges, &values)) return FCE_ERR_CORRUPT;
    (void)h;
    uint32_t node = 0;
    const uint8_t *p = (const uint8_t *)key;
    for (size_t i = 0; i < key_len; i++) {
        if (!radix_find_child(nodes, edges, node, p[i], &node)) return FCE_ERR_NOT_FOUND;
    }
    if (nodes[node].value_index == FCE_RADIX_NO_VALUE) return FCE_ERR_NOT_FOUND;
    RadixValue *v = &values[nodes[node].value_index];
    if (!key_equal(r->keys_blob.data, r->keys_blob.size, v->key_offset, v->key_len, key, key_len)) return FCE_ERR_NOT_FOUND;
    if (!range_ok(v->value_offset, v->value_len, r->values_blob.size)) return FCE_ERR_CORRUPT;
    *out_value = r->values_blob.data + v->value_offset;
    *out_value_len = v->value_len;
    return FCE_OK;
}

