/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef VDO_FORMATTER_H
#define VDO_FORMATTER_H

#include "types.h"
#include "vdo.h"

int __must_check vdo_is_formatted(struct vdo *vdo, int *formatted);

int __must_check vdo_format(struct vdo *vdo, char **error_ptr);

#endif /* VDO_FORMATTER_H */
