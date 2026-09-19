/* Regression coverage for the only startup repair accepted by WAL recovery. */
#include <errno.h>
#include <fcntl.h>
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "../src/tidesdb.h"
#include "test_macros.h"
#include "test_utils.h"

#define ZERO_WAL_DB_PATH "./test_zero_wal_recovery"

static int tests_passed = 0;
static int tests_failed = 0;

#ifndef _WIN32
/* Hold all but `leave_open` descriptors. A reopen then has exactly enough descriptors for its
 * database lock and CF manifest/WAL, but not for the CF recovery directory scan. */
static int *exhaust_file_descriptors(size_t leave_open, size_t *held_count)
{
    size_t capacity = 16;
    size_t count = 0;
    int *fds = malloc(capacity * sizeof(*fds));
    if (!fds) return NULL;

    for (;;)
    {
        const int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) break;
        if (count == capacity)
        {
            capacity *= 2;
            int *grown = realloc(fds, capacity * sizeof(*fds));
            if (!grown)
            {
                close(fd);
                while (count > 0) close(fds[--count]);
                free(fds);
                return NULL;
            }
            fds = grown;
        }
        fds[count++] = fd;
    }

    if (errno != EMFILE || count < leave_open)
    {
        while (count > 0) close(fds[--count]);
        free(fds);
        return NULL;
    }

    while (leave_open > 0)
    {
        close(fds[--count]);
        leave_open--;
    }
    *held_count = count;
    return fds;
}

static void release_file_descriptors(int *fds, size_t held_count)
{
    while (held_count > 0) close(fds[--held_count]);
    free(fds);
}
#endif

static void cleanup_zero_wal_db(void)
{
    remove_directory(ZERO_WAL_DB_PATH);
}

static void create_and_replace_active_wal(const unsigned char *bytes, size_t size)
{
    cleanup_zero_wal_db();

    tidesdb_config_t config = tidesdb_default_config();
    config.db_path = ZERO_WAL_DB_PATH;
    config.log_level = TDB_LOG_NONE;

    tidesdb_t *db = NULL;
    ASSERT_EQ(tidesdb_open(&config, &db), TDB_SUCCESS);
    tidesdb_column_family_config_t cf_config = tidesdb_default_column_family_config();
    cf_config.compression_algorithm = TDB_COMPRESS_NONE;
    ASSERT_EQ(tidesdb_create_column_family(db, "brain", &cf_config), TDB_SUCCESS);
    ASSERT_EQ(tidesdb_close(db), TDB_SUCCESS);

    FILE *wal = fopen(ZERO_WAL_DB_PATH "/brain/wal_0.log", "wb");
    ASSERT_TRUE(wal != NULL);
    ASSERT_EQ(fwrite(bytes, 1, size, wal), size);
    ASSERT_EQ(fclose(wal), 0);
}

static void overwrite_current_header_only_wal(const unsigned char *bytes, size_t size)
{
    FILE *wal = fopen(ZERO_WAL_DB_PATH "/brain/wal_1.log", "r+b");
    ASSERT_TRUE(wal != NULL);
    ASSERT_EQ(fseek(wal, 0, SEEK_END), 0);
    ASSERT_EQ(ftell(wal), 8L);
    ASSERT_EQ(fseek(wal, 0, SEEK_SET), 0);
    ASSERT_EQ(fwrite(bytes, 1, size, wal), size);
    ASSERT_EQ(fclose(wal), 0);
}

static void test_reopen_repairs_only_exact_empty_zero_wal(void)
{
    const unsigned char zero_header[8] = {0};
    create_and_replace_active_wal(zero_header, sizeof(zero_header));

    tidesdb_config_t config = tidesdb_default_config();
    config.db_path = ZERO_WAL_DB_PATH;
    config.log_level = TDB_LOG_NONE;
    tidesdb_t *db = NULL;
    ASSERT_EQ(tidesdb_open(&config, &db), TDB_SUCCESS);
    ASSERT_TRUE(tidesdb_get_column_family(db, "brain") != NULL);
    ASSERT_EQ(tidesdb_close(db), TDB_SUCCESS);

    unsigned char repaired[8] = {0};
    FILE *wal = fopen(ZERO_WAL_DB_PATH "/brain/wal_0.log", "rb");
    ASSERT_TRUE(wal != NULL);
    ASSERT_EQ(fread(repaired, 1, sizeof(repaired), wal), sizeof(repaired));
    ASSERT_EQ(fclose(wal), 0);
    const unsigned char expected[8] = {0x42, 0x44, 0x54, 0x07, 0, 0, 0, 0};
    ASSERT_TRUE(memcmp(repaired, expected, sizeof(expected)) == 0);
    cleanup_zero_wal_db();
}

static void test_reopen_refuses_nonzero_or_larger_zero_wal(void)
{
    const unsigned char nonzero_header[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    create_and_replace_active_wal(nonzero_header, sizeof(nonzero_header));
    tidesdb_config_t config = tidesdb_default_config();
    config.db_path = ZERO_WAL_DB_PATH;
    config.log_level = TDB_LOG_NONE;
    tidesdb_t *db = NULL;
    ASSERT_EQ(tidesdb_open(&config, &db), TDB_ERR_IO);
    cleanup_zero_wal_db();

    const unsigned char larger_zero_header[9] = {0};
    create_and_replace_active_wal(larger_zero_header, sizeof(larger_zero_header));
    db = NULL;
    ASSERT_EQ(tidesdb_open(&config, &db), TDB_ERR_IO);
    cleanup_zero_wal_db();
}

static void test_normal_runtime_create_does_not_repair_zero_wal(void)
{
    cleanup_zero_wal_db();
    tidesdb_config_t config = tidesdb_default_config();
    config.db_path = ZERO_WAL_DB_PATH;
    config.log_level = TDB_LOG_NONE;
    tidesdb_t *db = NULL;
    ASSERT_EQ(tidesdb_open(&config, &db), TDB_SUCCESS);

    ASSERT_EQ(mkdir(ZERO_WAL_DB_PATH "/brain", 0755), 0);
    const unsigned char zero_header[8] = {0};
    FILE *wal = fopen(ZERO_WAL_DB_PATH "/brain/wal_0.log", "wb");
    ASSERT_TRUE(wal != NULL);
    ASSERT_EQ(fwrite(zero_header, 1, sizeof(zero_header), wal), sizeof(zero_header));
    ASSERT_EQ(fclose(wal), 0);

    tidesdb_column_family_config_t cf_config = tidesdb_default_column_family_config();
    cf_config.compression_algorithm = TDB_COMPRESS_NONE;
    ASSERT_EQ(tidesdb_create_column_family(db, "brain", &cf_config), TDB_ERR_IO);

    unsigned char unchanged[8] = {1};
    wal = fopen(ZERO_WAL_DB_PATH "/brain/wal_0.log", "rb");
    ASSERT_TRUE(wal != NULL);
    ASSERT_EQ(fread(unchanged, 1, sizeof(unchanged), wal), sizeof(unchanged));
    ASSERT_EQ(fclose(wal), 0);
    ASSERT_TRUE(memcmp(unchanged, zero_header, sizeof(zero_header)) == 0);
    ASSERT_EQ(tidesdb_close(db), TDB_SUCCESS);
    cleanup_zero_wal_db();
}

static void test_reopen_repairs_zero_header_without_losing_flushed_snapshot(void)
{
    cleanup_zero_wal_db();
    tidesdb_config_t config = tidesdb_default_config();
    config.db_path = ZERO_WAL_DB_PATH;
    config.log_level = TDB_LOG_NONE;

    tidesdb_t *db = NULL;
    ASSERT_EQ(tidesdb_open(&config, &db), TDB_SUCCESS);
    tidesdb_column_family_config_t cf_config = tidesdb_default_column_family_config();
    cf_config.compression_algorithm = TDB_COMPRESS_NONE;
    ASSERT_EQ(tidesdb_create_column_family(db, "brain", &cf_config), TDB_SUCCESS);
    tidesdb_column_family_t *cf = tidesdb_get_column_family(db, "brain");
    ASSERT_TRUE(cf != NULL);

    const uint8_t key[] = "synthetic-brain-snapshot";
    const uint8_t value[] = "snapshot-survives-zero-header-repair";
    tidesdb_txn_t *txn = NULL;
    ASSERT_EQ(tidesdb_txn_begin(db, &txn), TDB_SUCCESS);
    ASSERT_EQ(tidesdb_txn_put(txn, cf, (uint8_t *)key, sizeof(key), (uint8_t *)value,
                              sizeof(value), 0),
              TDB_SUCCESS);
    ASSERT_EQ(tidesdb_txn_commit(txn), TDB_SUCCESS);
    tidesdb_txn_free(txn);
    ASSERT_EQ(tidesdb_flush_memtable(cf), TDB_SUCCESS);
    usleep(200000);
    ASSERT_EQ(tidesdb_close(db), TDB_SUCCESS);

    const unsigned char zero_header[8] = {0};
    overwrite_current_header_only_wal(zero_header, sizeof(zero_header));

    db = NULL;
    ASSERT_EQ(tidesdb_open(&config, &db), TDB_SUCCESS);
    cf = tidesdb_get_column_family(db, "brain");
    ASSERT_TRUE(cf != NULL);
    ASSERT_EQ(tidesdb_txn_begin(db, &txn), TDB_SUCCESS);
    uint8_t *read_value = NULL;
    size_t read_size = 0;
    ASSERT_EQ(tidesdb_txn_get(txn, cf, (uint8_t *)key, sizeof(key), &read_value, &read_size),
              TDB_SUCCESS);
    ASSERT_EQ(read_size, sizeof(value));
    ASSERT_TRUE(memcmp(read_value, value, sizeof(value)) == 0);
    free(read_value);
    tidesdb_txn_free(txn);
    ASSERT_EQ(tidesdb_close(db), TDB_SUCCESS);
    cleanup_zero_wal_db();
}

static void test_reopen_fails_when_created_cf_recovery_cannot_open_directory(void)
{
#ifdef _WIN32
    return;
#else
    cleanup_zero_wal_db();
    tidesdb_config_t config = tidesdb_default_config();
    config.db_path = ZERO_WAL_DB_PATH;
    config.log_level = TDB_LOG_NONE;
    config.log_to_file = 0;

    tidesdb_t *db = NULL;
    ASSERT_EQ(tidesdb_open(&config, &db), TDB_SUCCESS);
    tidesdb_column_family_config_t cf_config = tidesdb_default_column_family_config();
    cf_config.compression_algorithm = TDB_COMPRESS_NONE;
    ASSERT_EQ(tidesdb_create_column_family(db, "brain", &cf_config), TDB_SUCCESS);
    ASSERT_EQ(tidesdb_close(db), TDB_SUCCESS);

    struct rlimit saved_limit;
    ASSERT_EQ(getrlimit(RLIMIT_NOFILE, &saved_limit), 0);
    struct rlimit limited_limit = saved_limit;
    limited_limit.rlim_cur = saved_limit.rlim_cur < 64 ? saved_limit.rlim_cur : 64;
    ASSERT_EQ(setrlimit(RLIMIT_NOFILE, &limited_limit), 0);

    size_t held_count = 0;
    int *held_fds = exhaust_file_descriptors(4, &held_count);
    if (!held_fds)
    {
        ASSERT_EQ(setrlimit(RLIMIT_NOFILE, &saved_limit), 0);
        ASSERT_TRUE(held_fds != NULL);
    }
    db = NULL;
    const int reopen_result = tidesdb_open(&config, &db);
    release_file_descriptors(held_fds, held_count);
    ASSERT_EQ(setrlimit(RLIMIT_NOFILE, &saved_limit), 0);

    ASSERT_EQ(reopen_result, TDB_ERR_IO);
    ASSERT_TRUE(db == NULL);
    cleanup_zero_wal_db();
#endif
}

int main(int argc, char **argv)
{
    INIT_TEST_FILTER(argc, argv);
    RUN_TEST(test_reopen_repairs_only_exact_empty_zero_wal, tests_passed);
    RUN_TEST(test_reopen_refuses_nonzero_or_larger_zero_wal, tests_passed);
    RUN_TEST(test_normal_runtime_create_does_not_repair_zero_wal, tests_passed);
    RUN_TEST(test_reopen_repairs_zero_header_without_losing_flushed_snapshot, tests_passed);
    RUN_TEST(test_reopen_fails_when_created_cf_recovery_cannot_open_directory, tests_passed);
    PRINT_TEST_RESULTS(tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
