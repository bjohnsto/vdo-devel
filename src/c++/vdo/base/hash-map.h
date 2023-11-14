/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef VDO_HASH_MAP_H
#define VDO_HASH_MAP_H

#include <linux/compiler.h>
#include <linux/types.h>

/*
 * A vdo_hash_map associates pointers (void *) with integer keys (u64).
 *
 * The map is implemented as hash table, which should provide constant-time insert, query, and
 * remove operations, although the insert may occasionally grow the table, which is linear in the
 * number of entries in the map. The table will grow as needed to hold new entries, but will not
 * shrink as entries are removed.
 *
 * The value pointers passed to the map are retained and used by the map, but are not owned
 * by the map. Freeing the map does not attempt to free any pointers. The client is entirely
 * responsible for the memory management of the values. The current interface and
 * implementation assume that keys will be properties of the values, or that keys will not be
 * memory managed, or that keys will not need to be freed as a result of being replaced when a key
 * is re-mapped.
 */

struct vdo_hash_map;

int __must_check vdo_hash_map_create(size_t initial_capacity,
				     struct vdo_hash_map **map_ptr);

void vdo_hash_map_free(struct vdo_hash_map *map);

size_t vdo_hash_map_size(const struct vdo_hash_map *map);

void *vdo_hash_map_get(struct vdo_hash_map *map, u64 key);

int __must_check vdo_hash_map_put(struct vdo_hash_map *map,
				  u64 key, void *new_value,
				  bool update, void **old_value_ptr);

void *vdo_hash_map_remove(struct vdo_hash_map *map, u64 key);

#endif /* VDO_HAS_MAP_H */
