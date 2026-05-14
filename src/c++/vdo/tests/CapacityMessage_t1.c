/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "constants.h"
#include "encodings.h"
#include "status-codes.h"

#include "linux/log2.h"

#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  RESULT_BUFFER_SIZE = 1024,
};

struct capacity_result {
  unsigned int version;
  unsigned long long logical_blocks;
  unsigned long long data_blocks;
  unsigned long long max_data_blocks;
  unsigned int slab_count;
  unsigned int max_slab_count;
};

struct capacity_error {
  unsigned int version;
  int error;
  char reason[256];
};

/**
 * Call the deviceless message handler with the given arguments.
 *
 * @param argc  The argument count (including "capacity")
 * @param argv  The arguments
 * @param buf   The result buffer
 *
 * @return the return value from the handler
 **/
static int callCapacityMessage(unsigned int argc, char **argv,
                               char *buf, unsigned int buflen)
{
  CU_ASSERT_TRUE(vdoTargetType != NULL);
  CU_ASSERT_TRUE(vdoTargetType->deviceless_message != NULL);
  return vdoTargetType->deviceless_message(argc, argv, buf, buflen);
}

/**********************************************************************/
static int parseSuccessResponse(const char *buf, struct capacity_result *result)
{
  return sscanf(buf,
                "{ version : %u, "
                "logical_blocks : %llu, "
                "data_blocks : %llu, "
                "max_data_blocks : %llu, "
                "slab_count : %u, "
                "max_slab_count : %u }",
                &result->version,
                &result->logical_blocks,
                &result->data_blocks,
                &result->max_data_blocks,
                &result->slab_count,
                &result->max_slab_count);
}

/**********************************************************************/
static int parseErrorResponse(const char *buf, struct capacity_error *err)
{
  return sscanf(buf,
                "{ version : %u, error : %d, reason : \"%[^\"]\" }",
                &err->version,
                &err->error,
                err->reason);
}

/**********************************************************************/
static void testBasicCapacity(void)
{
  char buf[RESULT_BUFFER_SIZE];
  struct capacity_result result;
  int ret;

  char *argv[] = { "capacity", "0", "524288", "32768", "0", "off" };
  ret = callCapacityMessage(6, argv, buf, sizeof(buf));
  CU_ASSERT_EQUAL(ret, 1);

  CU_ASSERT_EQUAL(parseSuccessResponse(buf, &result), 6);
  CU_ASSERT_EQUAL(result.version, 1);
  CU_ASSERT_TRUE(result.logical_blocks > 0);
  CU_ASSERT_TRUE(result.data_blocks > 0);
  CU_ASSERT_TRUE(result.data_blocks <= result.max_data_blocks);
  CU_ASSERT_TRUE(result.slab_count > 0);
  CU_ASSERT_EQUAL(result.max_slab_count, MAX_VDO_SLABS);
  CU_ASSERT_TRUE(result.slab_count <= result.max_slab_count);
}

/**********************************************************************/
static void testDefaultLogicalBlocks(void)
{
  char buf[RESULT_BUFFER_SIZE];
  struct capacity_result result;
  int ret;

  char *argv[] = { "capacity", "0", "524288", "32768", "0", "off" };
  ret = callCapacityMessage(6, argv, buf, sizeof(buf));
  CU_ASSERT_EQUAL(ret, 1);
  CU_ASSERT_EQUAL(parseSuccessResponse(buf, &result), 6);

  /*
   * With logical_blocks=0 the handler should compute a default that is
   * positive but less than data_blocks (forest overhead is subtracted).
   */
  CU_ASSERT_TRUE(result.logical_blocks > 0);
  CU_ASSERT_TRUE(result.logical_blocks < result.data_blocks);
}

/**********************************************************************/
static void testExplicitLogicalBlocks(void)
{
  char buf[RESULT_BUFFER_SIZE];
  struct capacity_result with_default, with_explicit;
  int ret;

  char *argv_default[] = { "capacity", "0", "524288", "32768", "0", "off" };
  ret = callCapacityMessage(6, argv_default, buf, sizeof(buf));
  CU_ASSERT_EQUAL(ret, 1);
  CU_ASSERT_EQUAL(parseSuccessResponse(buf, &with_default), 6);

  /* Pass the same physical config but with an explicit logical size. */
  char *argv_explicit[] = { "capacity", "1000", "524288", "32768", "0", "off" };
  ret = callCapacityMessage(6, argv_explicit, buf, sizeof(buf));
  CU_ASSERT_EQUAL(ret, 1);
  CU_ASSERT_EQUAL(parseSuccessResponse(buf, &with_explicit), 6);

  CU_ASSERT_EQUAL(with_explicit.logical_blocks, 1000);
  /* Physical layout should be the same regardless of logical size. */
  CU_ASSERT_EQUAL(with_explicit.data_blocks, with_default.data_blocks);
  CU_ASSERT_EQUAL(with_explicit.slab_count, with_default.slab_count);
}

/**********************************************************************/
static void testSlabCountAndDataBlocks(void)
{
  char buf[RESULT_BUFFER_SIZE];
  struct capacity_result result;
  struct vdo_config vdo_config;
  struct index_config index_config;
  struct volume_geometry *geometry;
  struct vdo_component_states *states;
  struct slab_config *slab_config;
  slab_count_t expected_slab_count;
  block_count_t expected_data_blocks;
  uuid_t test_uuid = "capacity test !";
  int ret;

  char *argv[] = { "capacity", "0", "524288", "32768", "0", "off" };
  ret = callCapacityMessage(6, argv, buf, sizeof(buf));
  CU_ASSERT_EQUAL(ret, 1);
  CU_ASSERT_EQUAL(parseSuccessResponse(buf, &result), 6);

  /*
   * Independently compute the expected slab count and data blocks using
   * the same encodings functions the handler uses internally. This
   * validates that the handler is wiring everything together correctly.
   */
  vdo_config = (struct vdo_config) {
    .logical_blocks        = 0,
    .physical_blocks       = 524288,
    .slab_size             = 32768,
    .slab_journal_blocks   = DEFAULT_VDO_SLAB_JOURNAL_SIZE,
    .recovery_journal_size = DEFAULT_VDO_RECOVERY_JOURNAL_SIZE,
  };

  index_config = (struct index_config) {
    .mem    = 0,
    .sparse = false,
  };

  VDO_ASSERT_SUCCESS(vdo_allocate(1, __func__, &geometry));
  VDO_ASSERT_SUCCESS(vdo_initialize_volume_geometry(0, &test_uuid,
                                                    &index_config, geometry));
  VDO_ASSERT_SUCCESS(vdo_allocate(1, __func__, &states));
  VDO_ASSERT_SUCCESS(vdo_initialize_component_states(&vdo_config, geometry,
                                                     geometry->nonce, states));

  slab_config = &states->slab_depot.slab_config;
  expected_slab_count = vdo_compute_slab_count(states->slab_depot.first_block,
                                               states->slab_depot.last_block,
                                               ilog2(vdo_config.slab_size));
  expected_data_blocks =
    (block_count_t)expected_slab_count * slab_config->data_blocks;

  CU_ASSERT_EQUAL(result.slab_count, expected_slab_count);
  CU_ASSERT_EQUAL(result.data_blocks, expected_data_blocks);
  CU_ASSERT_EQUAL(result.max_data_blocks,
                  (unsigned long long)MAX_VDO_SLABS * slab_config->data_blocks);

  vdo_uninitialize_layout(&states->layout);
  vdo_free(states);
  vdo_free(geometry);
}

/**********************************************************************/
static void testWrongArgCount(void)
{
  char buf[RESULT_BUFFER_SIZE];
  struct capacity_error err;
  int ret;

  char *argv[] = { "capacity", "0", "524288" };
  ret = callCapacityMessage(3, argv, buf, sizeof(buf));
  CU_ASSERT_EQUAL(ret, 1);
  CU_ASSERT_EQUAL(parseErrorResponse(buf, &err), 3);
  CU_ASSERT_EQUAL(err.version, 1);
  CU_ASSERT_TRUE(err.error != 0);
}

/**********************************************************************/
static void testInvalidSlabSize(void)
{
  char buf[RESULT_BUFFER_SIZE];
  struct capacity_error err;
  int ret;

  /* 1000 is not a power of 2 */
  char *argv[] = { "capacity", "0", "524288", "1000", "0", "off" };
  ret = callCapacityMessage(6, argv, buf, sizeof(buf));
  CU_ASSERT_EQUAL(ret, 1);
  CU_ASSERT_EQUAL(parseErrorResponse(buf, &err), 3);
  CU_ASSERT_CONTAINS_SUBSTRING(err.reason, "slab size");
}

/**********************************************************************/
static void testPhysicalTooSmall(void)
{
  char buf[RESULT_BUFFER_SIZE];
  struct capacity_error err;
  int ret;

  char *argv[] = { "capacity", "0", "16", "32768", "0", "off" };
  ret = callCapacityMessage(6, argv, buf, sizeof(buf));
  CU_ASSERT_EQUAL(ret, 1);
  CU_ASSERT_EQUAL(parseErrorResponse(buf, &err), 3);
  CU_ASSERT_CONTAINS_SUBSTRING(err.reason, "Not enough space");
}

/**********************************************************************/
static void testLogicalExceedsMax(void)
{
  char buf[RESULT_BUFFER_SIZE];
  struct capacity_error err;
  int ret;

  /* MAXIMUM_VDO_LOGICAL_BLOCKS + 1 = 1099511627777 */
  char *argv[] = { "capacity", "1099511627777", "524288", "32768", "0", "off" };
  ret = callCapacityMessage(6, argv, buf, sizeof(buf));
  CU_ASSERT_EQUAL(ret, 1);
  CU_ASSERT_EQUAL(parseErrorResponse(buf, &err), 3);
  CU_ASSERT_CONTAINS_SUBSTRING(err.reason, "logical");
}

/**********************************************************************/
static void testNotCapacityMessage(void)
{
  char buf[RESULT_BUFFER_SIZE];
  int ret;

  char *argv[] = { "something_else" };
  ret = callCapacityMessage(1, argv, buf, sizeof(buf));
  CU_ASSERT_EQUAL(ret, -EINVAL);
}

/**********************************************************************/
static void testSparseIndex(void)
{
  char buf[RESULT_BUFFER_SIZE];
  struct capacity_result dense, sparse;
  int ret;

  char *argv_dense[]  = { "capacity", "0", "524288", "32768", "0", "off" };
  char *argv_sparse[] = { "capacity", "0", "524288", "32768", "0", "on" };

  ret = callCapacityMessage(6, argv_dense, buf, sizeof(buf));
  CU_ASSERT_EQUAL(ret, 1);
  CU_ASSERT_EQUAL(parseSuccessResponse(buf, &dense), 6);

  ret = callCapacityMessage(6, argv_sparse, buf, sizeof(buf));
  CU_ASSERT_EQUAL(ret, 1);
  CU_ASSERT_EQUAL(parseSuccessResponse(buf, &sparse), 6);

  /*
   * A sparse index uses less physical space for the index, leaving more
   * room for data slabs.
   */
  CU_ASSERT_TRUE(sparse.data_blocks >= dense.data_blocks);
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "basic capacity query",             testBasicCapacity      },
  { "default logical blocks",           testDefaultLogicalBlocks },
  { "explicit logical blocks",          testExplicitLogicalBlocks },
  { "slab count and data blocks",       testSlabCountAndDataBlocks },
  { "wrong argument count",             testWrongArgCount      },
  { "invalid slab size",                testInvalidSlabSize    },
  { "physical too small",               testPhysicalTooSmall   },
  { "logical exceeds maximum",          testLogicalExceedsMax  },
  { "non-capacity message rejected",    testNotCapacityMessage },
  { "sparse vs dense index",            testSparseIndex        },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name                     = "Deviceless capacity message (CapacityMessage_t1)",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = NULL,
  .tests                    = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
