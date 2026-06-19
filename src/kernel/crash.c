#include "crash.h"
#include "logger.h"

#define CRASH_LOG_CAPACITY 16

typedef struct {
    const char *category;
    const char *message;
    UINT64 code;
} ASAS_CRASH_RECORD;

static ASAS_CRASH_RECORD records[CRASH_LOG_CAPACITY];
static UINT32 next_record;
static UINT32 record_count;

static int strings_equal(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (*left != *right) {
            return 0;
        }
        left++;
        right++;
    }
    return *left == *right;
}

void crash_initialize(void)
{
    UINT32 index;

    for (index = 0; index < CRASH_LOG_CAPACITY; index++) {
        records[index].category = "";
        records[index].message = "";
        records[index].code = 0;
    }
    next_record = 0;
    record_count = 0;
    logger_write("INFO", "crash log ring initialized");
}

void crash_record(const char *category, const char *message, UINT64 code)
{
    records[next_record].category = category;
    records[next_record].message = message;
    records[next_record].code = code;
    next_record = (next_record + 1) % CRASH_LOG_CAPACITY;
    if (record_count < CRASH_LOG_CAPACITY) {
        record_count++;
    }
}

UINT32 crash_record_count(void)
{
    return record_count;
}

const char *crash_last_category(void)
{
    UINT32 index;

    if (record_count == 0) {
        return "";
    }
    index = (next_record + CRASH_LOG_CAPACITY - 1) % CRASH_LOG_CAPACITY;
    return records[index].category;
}

const char *crash_last_message(void)
{
    UINT32 index;

    if (record_count == 0) {
        return "";
    }
    index = (next_record + CRASH_LOG_CAPACITY - 1) % CRASH_LOG_CAPACITY;
    return records[index].message;
}

int crash_self_test(void)
{
    crash_record("self-test", "crash analysis sample", 0x41534153ULL);

    if (
        crash_record_count() == 0 ||
        !strings_equal(crash_last_category(), "self-test") ||
        !strings_equal(crash_last_message(), "crash analysis sample")
    ) {
        return 0;
    }

    logger_write("INFO", "crash log analysis verified");
    return 1;
}
