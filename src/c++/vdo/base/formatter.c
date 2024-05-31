// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "formatter.h"

#include <asm/unaligned.h>

#include <linux/blkdev.h>
#include <linux/log2.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "time-utils.h"

#include "constants.h"
#include "encodings.h"
#include "indexer.h"
#include "status-codes.h"
#include "vio.h"

#define RECOVERY_JOURNAL_STARTING_SEQUENCE_NUMBER 1

static const char * const UNITS[] = { "B", "KB", "MB", "GB", "TB", "PB" };

/**
 * get_readable_size() - Given an amount of bytes, continously divides the
 *                       value by 1K until the result is no longer greater
 *                       than 1K. This will give the smallest whole number
 *                       whole number size of type unit (bytes, kb, mb, etc).
 *                       The whole number is returned along with the unit type,
 *                       plus the remaining bytes, if any. This function is used
 *                       for printing out easily read sizes. (2.56MB, etc)
 * @size: The value in bytes to return info on.
 * @whole: The smallest whole number of unit type the size represents.
 * @remaining: The left over number of bytes the whole number.
 * @unit: The unit type the whole number represents.
 */
static void get_readable_size(size_t size, u64 *whole, u64 *remaining,
			      unsigned int *unit)
{
	*unit = 0;
	while ((size >= 1024) && (*unit < ARRAY_SIZE(UNITS) - 1)) {
		*remaining = size % 1024;
		size = size / 1024;
		*unit = *unit + 1;
	};
	*whole = size;
}

/**
 * vdo_log_capacity() - This function logs information about the overall capacity
 *                      of a vdo device once it has been formatted. Depending on
 *                      the slab size information, the vdo may only be able to
 *                      expand to a certain size.
 * @vdo: The vdo to log information about.
 */
static void vdo_log_capacity(struct vdo *vdo)
{
	struct slab_config slab_config = vdo->states.slab_depot.slab_config;
	unsigned int slab_size_shift = ilog2(vdo->states.vdo.config.slab_size);
	slab_count_t slab_count =
		vdo_compute_slab_count(vdo->states.slab_depot.first_block,
				       vdo->states.slab_depot.last_block,
				       slab_size_shift);
	u64 max_total_size = MAX_VDO_SLABS * slab_config.slab_blocks * VDO_BLOCK_SIZE;
	u64 total_size = (u64)slab_count * slab_config.slab_blocks * VDO_BLOCK_SIZE;

	u64 t_whole = 0;
	u64 t_remaining = 0;
	u64 whole = 0;
	u64 remaining = 0;
	unsigned int t_unit = 0;
	unsigned int unit = 0;

	get_readable_size(total_size, &t_whole, &t_remaining, &t_unit);
	get_readable_size(slab_config.slab_blocks * VDO_BLOCK_SIZE, &whole, &remaining,
			  &unit);
	vdo_log_debug("The VDO volume can address %llu.%llu %s in %u data slab(s), each %llu.%llu %s",
		      t_whole, t_remaining, UNITS[t_unit], slab_count, whole, remaining,
		      UNITS[unit]);

	if (slab_count < MAX_VDO_SLABS) {
		get_readable_size(max_total_size, &t_whole, &t_remaining, &t_unit);
		vdo_log_debug("It can grow to address at most %llu.%llu %s of physicalstorage in %u slabs",
			      t_whole, t_remaining, UNITS[t_unit], MAX_VDO_SLABS);
		if (vdo->device_config->slab_bits < MAX_VDO_SLAB_BITS)
			vdo_log_debug("If a larger maximum size might be needed, use bigger slabs");
	} else {
		vdo_log_info("The volume has the maximum number of slabs and so cannot grow");
		if (vdo->device_config->slab_bits < MAX_VDO_SLAB_BITS)
			vdo_log_info("Consider using larger slabs to allow the volume to grow");
	}
}

/**
 * write_block() - Synchronously write a block of data to a vdo's underlying
 *                 block device.
 * @vdo: The vdo device to submit i/os through.
 * @block: The buffer to write.
 * @pbn: The block location to write the buffer to.
 * @type: The type of i/o to write.
 * @prio: The i/o priority to use when writing.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int __must_check write_block(struct vdo *vdo,
				    char *block,
				    physical_block_number_t pbn,
				    enum vio_type type,
				    enum vio_priority prio)
{
	struct vio *vio;
	int result;

	result = create_metadata_vio(vdo, type, prio, NULL, block, &vio);
	if (result != VDO_SUCCESS)
		return result;

	/*
	 * This is only safe because, having not already loaded the geometry, the vdo's
	 * geometry's bio_offset field is 0, so the fact that vio_reset_bio() will
	 * subtract that offset from the supplied pbn is not a problem.
	 */
	result = vio_reset_bio(vio, block, NULL, REQ_OP_WRITE | REQ_PREFLUSH | REQ_FUA,
			       pbn);
	if (result != VDO_SUCCESS) {
		free_vio(vdo_forget(vio));
		return result;
	}

	bio_set_dev(vio->bio, vdo_get_backing_device(vdo));
	submit_bio_wait(vio->bio);
	result = blk_status_to_errno(vio->bio->bi_status);
	free_vio(vdo_forget(vio));
	if (result != 0) {
		vdo_log_error_strerror(result, "failed to write to disk");
		return -EIO;
	}
	return VDO_SUCCESS;
}

/**
 * write_geometry_block() - Synchronously write the geometry block to a vdo's
 *                          underlying block device.
 * @vdo: The vdo device whose geometry block is to be written.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int __must_check write_geometry_block(struct vdo *vdo)
{
	char *block;
	size_t offset = 0;
	u32 checksum;
	int result;

	result = vdo_allocate(VDO_BLOCK_SIZE, u8, __func__, &block);
	if (result != VDO_SUCCESS)
		return result;

	memcpy(block, VDO_GEOMETRY_MAGIC_NUMBER, VDO_GEOMETRY_MAGIC_NUMBER_SIZE);
	offset += VDO_GEOMETRY_MAGIC_NUMBER_SIZE;

	result = vdo_encode_volume_geometry((u8 *)block, &offset, &vdo->geometry,
					    VDO_DEFAULT_GEOMETRY_BLOCK_VERSION);
	if (result != VDO_SUCCESS) {
		vdo_free(block);
		return result;
	}

	checksum = vdo_crc32(block, offset);
	encode_u32_le((u8 *)block, &offset, checksum);

	result = write_block(vdo, block, VDO_GEOMETRY_BLOCK_LOCATION,
			     VIO_TYPE_GEOMETRY, VIO_PRIORITY_HIGH);
	vdo_free(block);
	return result;
}

/**
 * write_super_block() - Synchronously write the super block to a vdo's
 *                       underlying block device.
 * @vdo: The vdo whose super block is to be written.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int __must_check write_super_block(struct vdo *vdo)
{
	char *block;
	int result;

	result = vdo_allocate(VDO_BLOCK_SIZE, u8, __func__, &block);
	if (result != VDO_SUCCESS)
		return result;

	vdo_encode_super_block((u8 *)block, &vdo->states);

	result = write_block(vdo, block, vdo_get_data_region_start(vdo->geometry),
			     VIO_TYPE_SUPER_BLOCK, VIO_PRIORITY_HIGH);
	vdo_free(block);
	return result;
}

/**
 * clear_blocks() - Synchronously write zeroes to a vdo's underlying block
 *                  device.
 * @vdo: The vdo device to submit i/os through.
 * @type: The type of i/o to write.
 * @count: The number of blocks to zero out.
 * @pbn: The location to write zeroes to.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int __must_check clear_blocks(struct vdo *vdo,
				     enum vio_type type,
				     physical_block_number_t pbn,
				     block_count_t count)

{
	char *block;
	int result;

	result = vdo_allocate(VDO_BLOCK_SIZE * count, u8, __func__, &block);
	if (result != VDO_SUCCESS)
		return result;

	result = write_block(vdo, block, pbn, type, VIO_PRIORITY_HIGH);
	vdo_free(block);
	return result;
}

/**
 * clear_parition() - Zero out the space representing a vdo component
 *                    partition.
 * @vdo: The vdo device to submit i/os through.
 * @id: The partition id to zero out.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int __must_check clear_partition(struct vdo *vdo,
					enum partition_id id)
{
	struct partition *partition;
	int result;

	result = vdo_get_partition(&vdo->states.layout, id, &partition);
	if (result != VDO_SUCCESS)
		return result;

	block_count_t buffer_blocks = 1;
	block_count_t n;

	for (n = partition->count; (buffer_blocks < 4096) && ((n & 0x1) == 0);
	     n >>= 1) {
		buffer_blocks <<= 1;
	}

	physical_block_number_t pbn;

	for (pbn = partition->offset;
	     (pbn < partition->offset + partition->count) && (result == VDO_SUCCESS);
	     pbn += buffer_blocks) {
		result = clear_blocks(vdo, VIO_TYPE_PARTITION_COPY, pbn, buffer_blocks);
		if (result != VDO_SUCCESS)
			return result;
	}
	return VDO_SUCCESS;
}

/**
 * clear_uds_index() - Zero out the space representing the index.
 * @vdo: The vdo device to submit i/os through.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int clear_uds_index(struct vdo *vdo)
{
	return clear_blocks(vdo, VIO_TYPE_GEOMETRY, 1, 1);
}

/**
 * clear_volume_geometry() - Zero out the space representing the geometry block.
 * @vdo: The vdo device to submit i/os through.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int clear_volume_geometry(struct vdo *vdo)
{
	return clear_blocks(vdo, VIO_TYPE_GEOMETRY, VDO_GEOMETRY_BLOCK_LOCATION, 1);
}

/**
 * write_vdo() - Write the vdo layout to vdo's underlying storage block device.
 * @vdo: The vdo device whose layout we.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int write_vdo(struct vdo *vdo, char **error_ptr)
{
	int result;

	result = clear_volume_geometry(vdo);
	if (result != VDO_SUCCESS) {
		*error_ptr = "cannot clear volume geometry";
		return result;
	}

	result = clear_uds_index(vdo);
	if (result != VDO_SUCCESS) {
		*error_ptr = "cannot clear index superblock";
		return result;
	}

	result = clear_partition(vdo, VDO_BLOCK_MAP_PARTITION);
	if (result != VDO_SUCCESS) {
		*error_ptr = "cannot clear block map partition";
		return result;
	}

	result = clear_partition(vdo, VDO_RECOVERY_JOURNAL_PARTITION);
	if (result != VDO_SUCCESS) {
		*error_ptr = "cannot clear recovery journal partition";
		return result;
	}

	result = write_super_block(vdo);
	if (result != VDO_SUCCESS)
		return result;

	return write_geometry_block(vdo);
}

/**
 * compute_forest_size() - Compute the approximate number of pages which the
 *                         forest will allocate in order to map the specified
 *                         number of logical blocks. This method assumes that
 *                         the block map is entirely arboreal.
 * @logicalBlocks: The number of blocks to map
 * @rootCount: The number of trees in the forest
 *
 * Return: VDO_SUCCESS or an error code.
 */
static block_count_t __must_check compute_forest_size(block_count_t logicalBlocks,
						      root_count_t  rootCount)
{
	struct boundary newSizes;
	block_count_t approximateNonLeaves =
		vdo_compute_new_forest_pages(rootCount, NULL, logicalBlocks, &newSizes);

	// Exclude the tree roots since those aren't allocated from slabs,
	// and also exclude the super-roots, which only exist in memory.
	approximateNonLeaves -=
		rootCount * (newSizes.levels[VDO_BLOCK_MAP_TREE_HEIGHT - 2] +
			     newSizes.levels[VDO_BLOCK_MAP_TREE_HEIGHT - 1]);

	block_count_t approximateLeaves =
		vdo_compute_block_map_page_count(logicalBlocks - approximateNonLeaves);

	// This can be a slight over-estimate since the tree will never have to
	// address these blocks, so it might be a tiny bit smaller.
	return (approximateNonLeaves + approximateLeaves);
}

/**
 * initialize_patition() - Initialize parition so that it can be written out.
 * @vdo: The vdo whose partition will be initialized.
 * @error_ptr: The reason for any failure during this call
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int initialize_partitions(struct vdo *vdo, char **error_ptr)
{
	struct slab_config slab_config;
	int result = vdo_configure_slab(vdo->states.vdo.config.slab_size,
					vdo->states.vdo.config.slab_journal_blocks,
					&slab_config);
	if (result != VDO_SUCCESS)
		return result;

	struct partition *partition;
	struct layout layout = vdo->states.layout;

	result = vdo_get_partition(&layout, VDO_SLAB_DEPOT_PARTITION, &partition);
	if (result != VDO_SUCCESS) {
		*error_ptr = "no allocator partition";
		return result;
	}

	result = vdo_configure_slab_depot(partition, slab_config, 0, &vdo->states.slab_depot);
	if (result != VDO_SUCCESS) {
		*error_ptr = "failed to intialize allocator partition";
		return result;
	}

	/* Set derived parameters */
	unsigned int slab_size_shift = ilog2(vdo->states.vdo.config.slab_size);
	slab_count_t slab_count =
		vdo_compute_slab_count(vdo->states.slab_depot.first_block,
				       vdo->states.slab_depot.last_block,
				       slab_size_shift);

	if (vdo->states.vdo.config.logical_blocks == 0) {
		block_count_t data_blocks = slab_config.data_blocks * slab_count;

		vdo->states.vdo.config.logical_blocks =
			data_blocks - compute_forest_size(data_blocks,
							  DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT);
	}

	result = vdo_get_partition(&layout, VDO_BLOCK_MAP_PARTITION, &partition);
	if (result != VDO_SUCCESS) {
		*error_ptr = "no block map partition";
		return result;
	}

	vdo->states.block_map = (struct block_map_state_2_0) {
		.flat_page_origin = VDO_BLOCK_MAP_FLAT_PAGE_ORIGIN,
		.flat_page_count = 0,
		.root_origin = partition->offset,
		.root_count = DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT,
	};

	return VDO_SUCCESS;
}

/**
 * initialize_layout() - Initialize data layout so it can be written out.
 * @vdo: The vdo whose layout will be initialized.
 * @nonce: The nonce to use to identify the vdo.
 * @error_ptr: The reason for any failure during this call
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int initialize_layout(struct vdo *vdo, nonce_t nonce, char **error_ptr)
{
	struct device_config *config = vdo->device_config;

	struct vdo_config vdo_config = {
		.logical_blocks        = config->logical_blocks,
		.physical_blocks       = config->physical_blocks,
		.slab_size             = 1 << config->slab_bits,
		.slab_journal_blocks   = DEFAULT_VDO_SLAB_JOURNAL_SIZE,
		.recovery_journal_size = DEFAULT_VDO_RECOVERY_JOURNAL_SIZE,
	};

	vdo->states.vdo.config              = vdo_config;
	vdo->states.vdo.nonce               = nonce;
	vdo->states.volume_version          = VDO_VOLUME_VERSION_67_0;

	// The layout starts 1 block past the beginning of the data region, as the
	// data region contains the super block but the layout does not.
	physical_block_number_t starting_offset =
		vdo_get_data_region_start(vdo->geometry) + 1;
	int result = vdo_initialize_layout(config->physical_blocks,
					   starting_offset,
					   DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT,
					   vdo_config.recovery_journal_size,
					   VDO_SLAB_SUMMARY_BLOCKS,
					   &vdo->states.layout);
	if (result != VDO_SUCCESS) {
		*error_ptr = "failed to initialize layout";
		return result;
	}

	struct recovery_journal_state_7_0 journal = {
		.journal_start = RECOVERY_JOURNAL_STARTING_SEQUENCE_NUMBER,
		.logical_blocks_used   = 0,
		.block_map_data_blocks = 0,
	};
	vdo->states.recovery_journal = journal;

	result = initialize_partitions(vdo, error_ptr);
	if (result != VDO_SUCCESS)
		return result;

	return VDO_SUCCESS;
}

/**********************************************************************/
/**
 * compute_index_blocks() - Compute the size that the indexer will take up.
 * @vdo: The vdo whose index size is calculated.
 * @index_blocks_ptr: The number of blocks the index will use.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int compute_index_blocks(const struct device_config *config,
				block_count_t              *index_blocks_ptr)
{
	int result;
	u64 index_bytes;
	block_count_t index_blocks;
	struct uds_parameters uds_parameters = {
		.memory_size = config->index_memory,
		.sparse = config->index_sparse,
	};

	result = uds_compute_index_size(&uds_parameters, &index_bytes);
	if (result != UDS_SUCCESS)
		return vdo_log_error_strerror(result, "error computing index size");

	index_blocks = index_bytes / VDO_BLOCK_SIZE;
	if ((((u64) index_blocks) * VDO_BLOCK_SIZE) != index_bytes)
		return vdo_log_error_strerror(VDO_PARAMETER_MISMATCH,
					      "index size must be a multiple of block size %d",
					      VDO_BLOCK_SIZE);

	*index_blocks_ptr = index_blocks;
	return VDO_SUCCESS;
}

/**
 * initialize_volume_geometry() - Initialize the volume geometry so it can be
 *                                written out.
 * @vdo: The vdo whose layout will be initialized.
 * @nonce: The nonce to use to identify the vdo.
 * @uuid: The uuid value to use to identify the vdo.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int initialize_volume_geometry(struct vdo *vdo, nonce_t nonce,
				      uuid_t *uuid)
{
	int result;
	struct device_config *config = vdo->device_config;
	block_count_t index_size = 0;

	result = compute_index_blocks(config, &index_size);
	if (result != VDO_SUCCESS)
		return result;

	vdo->geometry = (struct volume_geometry) {
		/* This is for backwards compatibility. */
		.unused = 0,
		.nonce = nonce,
		.bio_offset = 0,
		.regions = {
			[VDO_INDEX_REGION] = {
				.id = VDO_INDEX_REGION,
				.start_block = 1,
			},
			[VDO_DATA_REGION] = {
				.id = VDO_DATA_REGION,
				.start_block = 1 + index_size,
			}
		},
		.index_config = {
			.mem = config->index_memory,
			.sparse = config->index_sparse,
		}
	};

	memcpy((unsigned char *) &(vdo->geometry.uuid), uuid, sizeof(uuid_t));

	return VDO_SUCCESS;
}

/**
 * initialize_vdo() - Initialize the VDO and use it to write the disk layout.
 *
 * @vdo: The vdo to initialize.
 * @error_ptr: The reason for any failure during this call.
 *
 * Return: VDO_SUCCESS or an error code.
 **/
static int initialize_vdo(struct vdo *vdo, char **error_ptr)
{
	// Generate a uuid.
	uuid_t uuid;
#ifdef __KERNEL__
	uuid_gen(&uuid);
#else
	uuid_generate(uuid);
#endif
	nonce_t nonce = current_time_us();
	int result;

	result = initialize_volume_geometry(vdo, nonce, &uuid);
	if (result != VDO_SUCCESS)
		return result;

	result = initialize_layout(vdo, nonce, error_ptr);
	if (result != VDO_SUCCESS)
		return result;

	vdo->states.vdo.state = VDO_NEW;
	return VDO_SUCCESS;
}

/**
 * vdo_format() - Format a block device to function as a new VDO. This function
 *                must be called on a device before a VDO can be loaded for the
 *                first time. Once a device has been formatted, the VDO can be
 *                loaded and shut down repeatedly. If a new VDO is desired, this
 *                function should be called again.
 *
 * @vdo        The vdo to format.
 * @error_ptr: The reason for any failure during this call.
 *
 * Return: VDO_SUCCESS or an error
 **/
int __must_check vdo_format(struct vdo *vdo, char **error_ptr)
{
	int result;

	result = initialize_vdo(vdo, error_ptr);
	if (result != VDO_SUCCESS) {
		*error_ptr = "failed to initialize vdo layout";
		return result;
	}

	result = write_vdo(vdo, error_ptr);
	if (result != VDO_SUCCESS) {
		*error_ptr = "failed to write vdo layout";
		return result;
	}

	vdo_log_capacity(vdo);

	memset(&vdo->geometry, 0, sizeof(struct volume_geometry));
	vdo_destroy_component_states(&vdo->states);

	return VDO_SUCCESS;
}

/**
 * read_block() - Synchronously read a block of data from a vdo's underlying
 *                block device.
 * @vdo: The vdo device to submit i/os through.
 * @pbn: The block location to write the buffer to.
 * @type: The type of i/o to write.
 * @prio: The i/o priority to use when writing.
 * @block: The buffer to read into.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int __must_check read_block(struct vdo *vdo,
				   physical_block_number_t pbn,
				   enum vio_type type,
				   enum vio_priority prio,
				   char *block)
{
	struct vio *vio;
	int result;

	result = create_metadata_vio(vdo, type, prio, NULL, block, &vio);
	if (result != VDO_SUCCESS)
		return result;

	/*
	 * This is only safe because, having not already loaded the geometry, the vdo's
	 geometry's bio_offset field is 0, so the fact that vio_reset_bio() will subtract
	 that offset from the supplied pbn is not a problem.
	 */
	result = vio_reset_bio(vio, block, NULL, REQ_OP_READ, pbn);
	if (result != VDO_SUCCESS) {
		free_vio(vdo_forget(vio));
		return result;
	}

	bio_set_dev(vio->bio, vdo_get_backing_device(vdo));
	submit_bio_wait(vio->bio);
	result = blk_status_to_errno(vio->bio->bi_status);
	free_vio(vdo_forget(vio));
	if (result != 0) {
		vdo_log_error_strerror(result, "failed to write to disk");
		return -EIO;
	}

	return VDO_SUCCESS;
}

/**
 * vdo_is_formatted() - Check to see whether the areas vdo will write to during
 *                      formatting are all zeroes.
 *
 * @vdo        The vdo to format.
 * @formatted: The state of the device. 0 means ok to format, 1 otherwise.
 *
 * Return: VDO_SUCCESS or an error
 **/
int __must_check vdo_is_formatted(struct vdo *vdo, int *formatted)
{
	char *block;
	int result;
	__le64 *data_le;
	__le64 zero = __cpu_to_le64(0);

	result = vdo_allocate(VDO_BLOCK_SIZE, u8, __func__, &block);
	if (result != VDO_SUCCESS)
		return result;

	// Check to make sure geometry block is all zeroes.
	result = read_block(vdo,
			    VDO_GEOMETRY_BLOCK_LOCATION,
			    VIO_TYPE_GEOMETRY,
			    VIO_PRIORITY_HIGH,
			    block);
	if (result != VDO_SUCCESS) {
		vdo_log_error_strerror(result, "failed to read from disk");
		vdo_free(block);
		return result;
	}

	// Following dm-thin-metadata.c logic
	unsigned int block_size = VDO_BLOCK_SIZE / sizeof(__le64);

	data_le = (__le64 *)block;

	*formatted = 0;
	for (unsigned int i = 0; i < block_size; i++) {
		if (data_le[i] != zero) {
			*formatted = 1;
			break;
		}
	}
	vdo_free(block);
	return VDO_SUCCESS;
}
