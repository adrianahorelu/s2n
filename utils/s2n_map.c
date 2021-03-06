/*
 * Copyright 2016 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <string.h>
#include <stdio.h>

#include "error/s2n_errno.h"

#include "crypto/s2n_hash.h"

#include "utils/s2n_safety.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_map.h"

#include <s2n.h>

#define S2N_INITIAL_TABLE_SIZE 1024

struct s2n_map_entry {
    struct s2n_blob key;
    struct s2n_blob value;
};

struct s2n_map {
    /* The total capacity of the table, in number of elements. */
    uint32_t capacity;

    /* The total number of elements currently in the table. Used for measuring the load factor */
    uint32_t size;

    /* Once a map has been looked up, it is considered immutable */
    int      immutable;

    /* Pointer to the hash-table, should be capacity * sizeof(struct s2n_map_entry) */
    struct s2n_map_entry *table;

    struct s2n_hash_state sha256;
};

static uint32_t s2n_map_slot(struct s2n_map *map, struct s2n_blob *key)
{
    union {
        uint8_t u8[32];
        uint32_t u32[8];
    } digest;

    GUARD(s2n_hash_update(&map->sha256, key->data, key->size));
    GUARD(s2n_hash_digest(&map->sha256, digest.u8, sizeof(digest)));

    GUARD(s2n_hash_reset(&map->sha256));

    return digest.u32[0] % map->capacity;
}

static int s2n_map_embiggen(struct s2n_map *map, uint32_t capacity)
{
    struct s2n_blob mem;
    struct s2n_map tmp;

    S2N_ERROR_IF(map->immutable, S2N_ERR_MAP_IMMUTABLE);

    GUARD(s2n_alloc(&mem, (capacity * sizeof(struct s2n_map_entry))));
    GUARD(s2n_blob_zero(&mem));

    tmp.capacity = capacity;
    tmp.size = 0;
    tmp.table = (void *) mem.data;
    tmp.immutable = 0;
    tmp.sha256 = map->sha256;

    for (int i = 0; i < map->capacity; i++) {
        if (map->table[i].key.size) {
            GUARD(s2n_map_add(&tmp, &map->table[i].key, &map->table[i].value));
            GUARD(s2n_free(&map->table[i].key));
            GUARD(s2n_free(&map->table[i].value));
        }
    }

    /* Free the old memory */
    mem.data = (void *) map->table;
    mem.size = map->capacity * sizeof(struct s2n_map_entry);
    GUARD(s2n_free(&mem));

    /* Clone the temporary map */
    map->capacity = tmp.capacity;
    map->size = tmp.size;
    map->table = tmp.table;
    map->immutable = 0;
    map->sha256 = tmp.sha256;

    return 0;
}

struct s2n_map *s2n_map_new()
{
    struct s2n_blob mem;
    struct s2n_map *map;

    GUARD_PTR(s2n_alloc(&mem, sizeof(struct s2n_map)));

    map = (void *) mem.data;
    map->capacity = 0;
    map->size = 0;
    map->immutable = 0;
    map->table = NULL;

    GUARD_PTR(s2n_hash_new(&map->sha256));
    GUARD_PTR(s2n_hash_init(&map->sha256, S2N_HASH_SHA256));

    GUARD_PTR(s2n_map_embiggen(map, S2N_INITIAL_TABLE_SIZE));

    return map;
}

int s2n_map_add(struct s2n_map *map, struct s2n_blob *key, struct s2n_blob *value)
{
    S2N_ERROR_IF(map->immutable, S2N_ERR_MAP_IMMUTABLE);

    if (map->capacity < (map->size * 2)) {
        /* Embiggen the map */
        GUARD(s2n_map_embiggen(map, map->capacity * 2));
    }

    uint32_t slot = s2n_map_slot(map, key);

    /* Linear probing until we find an empty slot */
    while(map->table[slot].key.size) {
        if (key->size != map->table[slot].key.size ||
            memcmp(key->data,  map->table[slot].key.data, key->size)) {
            slot++;
            slot %= map->capacity;
            continue;
        }

        /* We found a duplicate key */
        S2N_ERROR(S2N_ERR_MAP_DUPLICATE);
    }

    GUARD(s2n_dup(key, &map->table[slot].key));
    GUARD(s2n_dup(value, &map->table[slot].value));
    map->size++;

    return 0;
}

int s2n_map_complete(struct s2n_map *map)
{
    map->immutable = 1;

    return 0;
}

int s2n_map_lookup(struct s2n_map *map, struct s2n_blob *key, struct s2n_blob *value)
{
    S2N_ERROR_IF(!map->immutable, S2N_ERR_MAP_MUTABLE);

    uint32_t slot = s2n_map_slot(map, key);

    while(map->table[slot].key.size) {
        if (key->size != map->table[slot].key.size ||
            memcmp(key->data,  map->table[slot].key.data, key->size)) {
            slot++;
            slot %= map->capacity;
            continue;
        }

        /* We found a match */
        value->data = map->table[slot].value.data;
        value->size = map->table[slot].value.size;

        return 1;
    }

    return 0;
}

int s2n_map_free(struct s2n_map *map)
{
    struct s2n_blob mem;

    /* Free the keys and values */
    for (int i = 0; i < map->capacity; i++) {
        if (map->table[i].key.size) {
            GUARD(s2n_free(&map->table[i].key));
            GUARD(s2n_free(&map->table[i].value));
        }
    }

    GUARD(s2n_hash_free(&map->sha256));

    /* Free the table */
    mem.data = (void *) map->table;
    mem.size = map->capacity * sizeof(struct s2n_map_entry);
    GUARD(s2n_free(&mem));

    /* And finally the map */
    mem.data = (void *) map;
    mem.size = sizeof(struct s2n_map);
    GUARD(s2n_free(&mem));

    return 0;
}
