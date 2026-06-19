#include "security.h"
#include "logger.h"

typedef struct {
    UINT32 uid;
    const char *name;
    const char *password;
    UINT32 permissions;
} ASAS_SECURITY_USER;

static const ASAS_SECURITY_USER users[] = {
    { 0, "root", "asas-root", SECURITY_PERMISSION_READ | SECURITY_PERMISSION_WRITE | SECURITY_PERMISSION_EXECUTE | SECURITY_PERMISSION_ADMIN },
    { 1000, "guest", "", SECURITY_PERMISSION_READ | SECURITY_PERMISSION_EXECUTE }
};

static const ASAS_SECURITY_USER *current_user;

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

void security_initialize(void)
{
    current_user = &users[0];
    logger_write("INFO", "security users initialized");
}

UINT32 security_current_uid(void)
{
    return current_user->uid;
}

const char *security_current_user(void)
{
    return current_user->name;
}

UINT32 security_current_permissions(void)
{
    return current_user->permissions;
}

int security_can_read(void)
{
    return (current_user->permissions & SECURITY_PERMISSION_READ) != 0;
}

int security_can_write(void)
{
    return (current_user->permissions & SECURITY_PERMISSION_WRITE) != 0;
}

int security_can_execute(void)
{
    return (current_user->permissions & SECURITY_PERMISSION_EXECUTE) != 0;
}

int security_can_admin(void)
{
    return (current_user->permissions & SECURITY_PERMISSION_ADMIN) != 0;
}

int security_switch_user_authenticated(const char *name, const char *password)
{
    UINT32 index;

    for (index = 0; index < sizeof(users) / sizeof(users[0]); index++) {
        if (strings_equal(users[index].name, name)) {
            if (
                (users[index].permissions & SECURITY_PERMISSION_ADMIN) != 0 &&
                !strings_equal(users[index].password, password)
            ) {
                return 0;
            }
            current_user = &users[index];
            return 1;
        }
    }
    return 0;
}

int security_switch_user(const char *name)
{
    return security_switch_user_authenticated(name, "");
}

int security_self_test(void)
{
    if (
        !strings_equal(security_current_user(), "root") ||
        security_current_uid() != 0 ||
        !security_can_read() ||
        !security_can_write() ||
        !security_can_execute() ||
        !security_can_admin()
    ) {
        return 0;
    }

    if (
        !security_switch_user("guest") ||
        security_current_uid() != 1000 ||
        !security_can_read() ||
        security_can_write() ||
        !security_can_execute() ||
        security_can_admin()
    ) {
        (void)security_switch_user("root");
        return 0;
    }

    if (security_switch_user("root")) {
        return 0;
    }
    if (!security_switch_user_authenticated("root", "asas-root")) {
        return 0;
    }

    current_user = &users[0];

    logger_write("INFO", "security permissions verified");
    return 1;
}
