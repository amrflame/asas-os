#include "aslr.h"
#include "logger.h"

#define ASLR_DEFAULT_SEED 0x4153415341534C52ULL
#define ASLR_USER_STACK_BASE 0x0000600000400000ULL
#define ASLR_USER_STACK_STRIDE 0x20000ULL
#define ASLR_USER_STACK_SLOTS 256ULL

static UINT64 aslr_state;

static UINT64 aslr_next(void)
{
    aslr_state = aslr_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return aslr_state;
}

void aslr_initialize(UINT64 seed)
{
    aslr_state = seed ^ ASLR_DEFAULT_SEED;
    if (aslr_state == 0) {
        aslr_state = ASLR_DEFAULT_SEED;
    }
    logger_write("INFO", "ASLR seed initialized");
}

UINT64 aslr_user_stack_address(void)
{
    UINT64 slot = (aslr_next() >> 12) % ASLR_USER_STACK_SLOTS;

    return ASLR_USER_STACK_BASE + (slot * ASLR_USER_STACK_STRIDE);
}

int aslr_self_test(void)
{
    UINT64 first = aslr_user_stack_address();
    UINT64 second = aslr_user_stack_address();

    if (
        first < ASLR_USER_STACK_BASE ||
        second < ASLR_USER_STACK_BASE ||
        first >= ASLR_USER_STACK_BASE + (ASLR_USER_STACK_STRIDE * ASLR_USER_STACK_SLOTS) ||
        second >= ASLR_USER_STACK_BASE + (ASLR_USER_STACK_STRIDE * ASLR_USER_STACK_SLOTS) ||
        (first & 0xFFFULL) != 0 ||
        (second & 0xFFFULL) != 0 ||
        first == second
    ) {
        return 0;
    }

    logger_write("INFO", "ASLR user stack randomization verified");
    return 1;
}
