#ifndef ASAS_SECURITY_H
#define ASAS_SECURITY_H

#include "uefi.h"

#define SECURITY_PERMISSION_READ 1
#define SECURITY_PERMISSION_WRITE 2
#define SECURITY_PERMISSION_EXECUTE 4
#define SECURITY_PERMISSION_ADMIN 8

void security_initialize(void);
UINT32 security_current_uid(void);
const char *security_current_user(void);
UINT32 security_current_permissions(void);
int security_can_read(void);
int security_can_write(void);
int security_can_execute(void);
int security_can_admin(void);
int security_switch_user(const char *name);
int security_switch_user_authenticated(const char *name, const char *password);
int security_self_test(void);

#endif
