/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef VDO_POINTER_MAP_H
#define VDO_POINTER_MAP_H

#include <linux/compiler.h>
#include <linux/types.h>

#include "hash-map.h"

/**
 * DOC: pointer_map
 *
 * An pointer_map associates pointers (void *) with pointers (void *). NULL pointer values are
 * not supported.
 *
 * The map is implemented as hash table, which should provide constant-time insert, query, and
 * remove operations, although the insert may occasionally grow the table, which is linear in the
 * number of entries in the map. The table will grow as needed to hold new entries, but will not
 * shrink as entries are removed.
 *
 * NB: This is temporary. Eventually PointerMap_t1 will go away and so will this
 *     file. The plan is to just have int keys only in hash-map.c
 */

static inline int __must_check
vdo_make_pointer_map(size_t initial_capacity,
		     unsigned int initial_load,
		     pointer_key_compare_fn comparator,
		     pointer_key_hash_fn hasher,
		     struct vdo_hash_map **map_ptr)
{
	return vdo_hash_map_create(initial_capacity, initial_load, comparator, hasher, map_ptr);
}

static inline void vdo_free_pointer_map(struct vdo_hash_map *map)
{
	vdo_hash_map_free(map);
}

static inline size_t vdo_pointer_map_size(const struct vdo_hash_map *map)
{
	return vdo_hash_map_size(map);
}

static inline void *vdo_pointer_map_get(struct vdo_hash_map *map, void *key)
{
	return vdo_hash_map_get(map, &key);
}

static inline int __must_check
vdo_pointer_map_put(struct vdo_hash_map *map, void *key, void *new_value,
		    bool update, void **old_value_ptr)
{
	return vdo_hash_map_put(map, &key, new_value, update, old_value_ptr);
}

static inline void *vdo_pointer_map_remove(struct vdo_hash_map *map, void *key)
{
	return vdo_hash_map_remove(map, &key);
}

#endif /* VDO_POINTER_MAP_H */
