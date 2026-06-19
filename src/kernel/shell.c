#include "shell.h"
#include "audio.h"
#include "block_device.h"
#include "disk_management.h"
#include "filesystem.h"
#include "gui.h"
#include "heap.h"
#include "hyperv_storage.h"
#include "keyboard.h"
#include "laptop.h"
#include "logger.h"
#include "power.h"
#include "partition.h"
#include "process.h"
#include "scheduler.h"
#include "security.h"
#include "vfs.h"
#include "virtual_disk.h"
#include "virtio_block.h"
#include "virtio_net.h"
#include "xhci.h"

#define SHELL_PATH_CAPACITY 256

static char current_directory[SHELL_PATH_CAPACITY] = "/";
static char input_line[128];
static UINT32 input_length;

/* --- Environment variables -------------------------------------------- */
#define ENV_MAX      16
#define ENV_NAME_LEN 16
#define ENV_VAL_LEN  64
static char   env_names[ENV_MAX][ENV_NAME_LEN];
static char   env_values[ENV_MAX][ENV_VAL_LEN];
static UINT32 env_count;

/* --- Output capture (pipe / redirect) ------------------------------------ */
#define SHELL_CAPTURE_MAX 4096
static char   shell_capture_buf[SHELL_CAPTURE_MAX];
static UINT32 shell_capture_len;
static UINT8  shell_capturing;

/* --- Stdin buffer (piped input for grep / wc) ----------------------------- */
static char        shell_stdin_data[SHELL_CAPTURE_MAX];
static const char *shell_stdin_ptr;
static UINT32      shell_stdin_len;

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

static int starts_with(const char *text, const char *prefix)
{
    while (*prefix != '\0') {
        if (*text != *prefix) {
            return 0;
        }
        text++;
        prefix++;
    }
    return 1;
}

static int resolve_path(const char *input, char output[SHELL_PATH_CAPACITY])
{
    UINT32 output_index = 0;
    UINT32 input_index = 0;

    if (input[0] == '/') {
        while (input[input_index] != '\0' && output_index + 1 < SHELL_PATH_CAPACITY) {
            output[output_index++] = input[input_index++];
        }
    } else {
        while (current_directory[output_index] != '\0' && output_index + 1 < SHELL_PATH_CAPACITY) {
            output[output_index] = current_directory[output_index];
            output_index++;
        }
        if (output_index > 1 && output[output_index - 1] != '/') {
            output[output_index++] = '/';
        }
        while (input[input_index] != '\0' && output_index + 1 < SHELL_PATH_CAPACITY) {
            output[output_index++] = input[input_index++];
        }
    }
    output[output_index] = '\0';
    return input[input_index] == '\0';
}

/* -----------------------------------------------------------------------
 * String helpers
 * --------------------------------------------------------------------- */
static void copy_str_n(char *dst, const char *src, UINT32 cap)
{
    UINT32 i = 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static UINT32 str_len(const char *s) { UINT32 n = 0; while (s[n]) n++; return n; }

static void uint_to_str(UINT32 n, char *buf, UINT32 cap)
{
    char tmp[12];
    UINT32 i = 0, j;
    if (cap == 0) return;
    if (n == 0) { buf[0] = '0'; if (cap > 1) buf[1] = '\0'; return; }
    while (n > 0 && i < 10) { tmp[i++] = (char)('0' + n % 10); n /= 10; }
    for (j = 0; j < i && j + 1 < cap; j++) buf[j] = tmp[i - 1 - j];
    buf[j] = '\0';
}

/* -----------------------------------------------------------------------
 * Environment variable store
 * --------------------------------------------------------------------- */
static const char *env_get(const char *name)
{
    UINT32 i;
    for (i = 0; i < env_count; i++)
        if (strings_equal(env_names[i], name)) return env_values[i];
    return 0;
}

static void env_set(const char *name, const char *value)
{
    UINT32 i;
    for (i = 0; i < env_count; i++) {
        if (strings_equal(env_names[i], name)) {
            copy_str_n(env_values[i], value, ENV_VAL_LEN);
            return;
        }
    }
    if (env_count < ENV_MAX) {
        copy_str_n(env_names[env_count], name, ENV_NAME_LEN);
        copy_str_n(env_values[env_count], value, ENV_VAL_LEN);
        env_count++;
    }
}

static void expand_env(const char *input, char *output, UINT32 cap)
{
    UINT32 i = 0, j = 0;
    while (input[i] && j + 1 < cap) {
        if (input[i] == '$') {
            char   var[ENV_NAME_LEN];
            UINT32 k = 0;
            i++;
            while (input[i] && input[i] != ' ' && input[i] != '/' &&
                   k + 1 < ENV_NAME_LEN)
                var[k++] = input[i++];
            var[k] = '\0';
            if (k > 0) {
                const char *val = env_get(var);
                if (val) while (*val && j + 1 < cap) output[j++] = *val++;
            }
        } else {
            output[j++] = input[i++];
        }
    }
    output[j] = '\0';
}

/* -----------------------------------------------------------------------
 * Pipe / redirect capture
 * --------------------------------------------------------------------- */
static void shell_capture_cb(const char *level, const char *message)
{
    UINT32 i = 0;
    (void)level;
    if (!shell_capturing) return;
    while (message[i] && shell_capture_len + 1 < SHELL_CAPTURE_MAX)
        shell_capture_buf[shell_capture_len++] = message[i++];
    if (shell_capture_len + 1 < SHELL_CAPTURE_MAX)
        shell_capture_buf[shell_capture_len++] = '\n';
    shell_capture_buf[shell_capture_len] = '\0';
}

static void start_capture(void)
{
    shell_capture_len = 0;
    shell_capture_buf[0] = '\0';
    shell_capturing = 1;
    logger_set_capture_callback(shell_capture_cb);
}

static void stop_capture(void)
{
    shell_capturing = 0;
    logger_set_capture_callback(0);
}

#define REDIR_NONE   0
#define REDIR_OUT    1
#define REDIR_APPEND 2
#define REDIR_PIPE   3
#define REDIR_IN     4

static int parse_redirect(
    const char *command,
    char *cmd_clean, UINT32 clean_cap,
    char *arg,       UINT32 arg_cap)
{
    UINT32 i, j, end;
    for (i = 0; command[i]; i++) {
        if (command[i] == '>' && command[i + 1] == '>') {
            end = i;
            while (end > 0 && command[end - 1] == ' ') end--;
            for (j = 0; j < end && j + 1 < clean_cap; j++) cmd_clean[j] = command[j];
            cmd_clean[j] = '\0'; i += 2; while (command[i] == ' ') i++;
            for (j = 0; command[i] && j + 1 < arg_cap; ) arg[j++] = command[i++];
            arg[j] = '\0'; return REDIR_APPEND;
        }
        if (command[i] == '>' && command[i + 1] != '>') {
            end = i;
            while (end > 0 && command[end - 1] == ' ') end--;
            for (j = 0; j < end && j + 1 < clean_cap; j++) cmd_clean[j] = command[j];
            cmd_clean[j] = '\0'; i++; while (command[i] == ' ') i++;
            for (j = 0; command[i] && j + 1 < arg_cap; ) arg[j++] = command[i++];
            arg[j] = '\0'; return REDIR_OUT;
        }
        if (command[i] == '|') {
            end = i;
            while (end > 0 && command[end - 1] == ' ') end--;
            for (j = 0; j < end && j + 1 < clean_cap; j++) cmd_clean[j] = command[j];
            cmd_clean[j] = '\0'; i++; while (command[i] == ' ') i++;
            for (j = 0; command[i] && j + 1 < arg_cap; ) arg[j++] = command[i++];
            arg[j] = '\0'; return REDIR_PIPE;
        }
        if (command[i] == '<') {
            end = i;
            while (end > 0 && command[end - 1] == ' ') end--;
            for (j = 0; j < end && j + 1 < clean_cap; j++) cmd_clean[j] = command[j];
            cmd_clean[j] = '\0'; i++; while (command[i] == ' ') i++;
            for (j = 0; command[i] && j + 1 < arg_cap; ) arg[j++] = command[i++];
            arg[j] = '\0'; return REDIR_IN;
        }
    }
    for (j = 0; command[j] && j + 1 < clean_cap; j++) cmd_clean[j] = command[j];
    cmd_clean[j] = '\0'; arg[0] = '\0'; return REDIR_NONE;
}

/* -----------------------------------------------------------------------
 * grep / wc  (read from shell_stdin_ptr)
 * --------------------------------------------------------------------- */
static int shell_grep(const char *pattern)
{
    const char *buf  = shell_stdin_ptr;
    UINT32      blen = shell_stdin_len;
    UINT32 pat_len, i, j, line_start;
    char   line[130];

    if (!buf || blen == 0) { logger_write("SHELL", "grep: no input"); return 0; }
    for (pat_len = 0; pattern[pat_len]; pat_len++);
    line_start = 0;
    for (i = 0; i <= blen; i++) {
        if (i == blen || buf[i] == '\n') {
            UINT32 raw_len = i - line_start;
            UINT32 llen    = raw_len < 127 ? raw_len : 127;
            for (j = 0; j < llen; j++) line[j] = buf[line_start + j];
            while (llen > 0 && (line[llen-1] == '\r' || line[llen-1] == '\n')) llen--;
            line[llen] = '\0';
            if (pat_len == 0) {
                logger_write("SHELL", line);
            } else {
                UINT32 k;
                for (k = 0; k + pat_len <= llen; k++) {
                    for (j = 0; j < pat_len; j++)
                        if (line[k + j] != pattern[j]) break;
                    if (j == pat_len) { logger_write("SHELL", line); break; }
                }
            }
            line_start = i + 1;
        }
    }
    return 1;
}

static int shell_wc(void)
{
    const char *buf  = shell_stdin_ptr;
    UINT32      blen = shell_stdin_len;
    UINT32 lines = 0, words = 0, i;
    int    in_word = 0;
    char   out[64];
    char   tmp[12];
    UINT32 n;

    if (!buf) { logger_write("SHELL", "wc: no input"); return 0; }
    for (i = 0; i < blen; i++) {
        if (buf[i] == '\n') lines++;
        if (buf[i]==' '||buf[i]=='\t'||buf[i]=='\n'||buf[i]=='\r') in_word = 0;
        else if (!in_word) { words++; in_word = 1; }
    }
    out[0] = '\0';
    uint_to_str(lines, tmp, sizeof(tmp)); copy_str_n(out, tmp, sizeof(out));
    n = str_len(out); out[n++] = ' '; out[n] = '\0';
    uint_to_str(words, tmp, sizeof(tmp)); copy_str_n(out + n, tmp, sizeof(out) - n);
    n = str_len(out); out[n++] = ' '; out[n] = '\0';
    uint_to_str(blen, tmp, sizeof(tmp)); copy_str_n(out + n, tmp, sizeof(out) - n);
    logger_write("SHELL", out);
    return 1;
}

static int shell_ls(const char *path)
{
    VFS_DIRECTORY_ENTRY *entries;
    char resolved[SHELL_PATH_CAPACITY];
    UINT64 count;
    UINT64 index;

    if (!resolve_path(path, resolved)) return 0;
    if (!security_can_read()) {
        logger_write("SHELL", "ls: permission denied");
        return 0;
    }
    entries = (VFS_DIRECTORY_ENTRY *)kmalloc(256 * sizeof(VFS_DIRECTORY_ENTRY));
    if (!entries) { logger_write("SHELL", "ls: out of memory"); return 0; }
    count = vfs_list_directory(resolved, entries, 256);
    logger_write("SHELL", resolved);
    for (index = 0; index < count; index++)
        logger_write(entries[index].is_directory ? "DIR" : "FILE", entries[index].name);
    kfree(entries);
    return count != 0;
}

static int shell_cat(const char *path)
{
    char *buf;
    char resolved[SHELL_PATH_CAPACITY];
    UINT64 handle, file_size, bytes, i;
    char line[130];
    UINT32 line_len;

    if (!resolve_path(path, resolved)) return 0;
    if (!security_can_read()) {
        logger_write("SHELL", "cat: permission denied");
        return 0;
    }
    file_size = vfs_file_size(resolved);
    if (file_size == 0) file_size = 4096;   /* fallback when size API unsupported */
    if (file_size > 32768) file_size = 32768;
    buf = (char *)kmalloc((UINTN)(file_size + 1));
    if (!buf) { logger_write("SHELL", "cat: out of memory"); return 0; }
    handle = vfs_open(resolved);
    if (!handle) { kfree(buf); logger_write("SHELL", "cat: file not found"); return 0; }
    bytes = vfs_read(handle, buf, file_size);
    (void)vfs_close(handle);
    buf[bytes] = '\0';
    line_len = 0;
    for (i = 0; i <= bytes; i++) {
        char c = buf[i];
        if (c == '\n' || c == '\0' || line_len >= 127) {
            line[line_len] = '\0';
            if (line_len > 0) logger_write("SHELL", line);
            line_len = 0;
        } else if (c != '\r') {
            line[line_len++] = c;
        }
    }
    kfree(buf);
    return bytes != 0;
}

static int shell_cd(const char *path)
{
    char resolved[SHELL_PATH_CAPACITY];
    UINT32 index = 0;

    if (!resolve_path(path, resolved) || !vfs_is_directory(resolved)) {
        logger_write("SHELL", "cd: directory not found");
        return 0;
    }
    while (resolved[index] != '\0') {
        current_directory[index] = resolved[index];
        index++;
    }
    current_directory[index] = '\0';
    return 1;
}

static int shell_write(const char *arguments)
{
    char path[SHELL_PATH_CAPACITY];
    char resolved[SHELL_PATH_CAPACITY];
    UINT32 path_length = 0;
    const char *text;
    UINT64 text_length = 0;

    while (arguments[path_length] != '\0' && arguments[path_length] != ' ') {
        if (path_length + 1 >= SHELL_PATH_CAPACITY) {
            return 0;
        }
        path[path_length] = arguments[path_length];
        path_length++;
    }
    path[path_length] = '\0';
    if (path_length == 0 || arguments[path_length] != ' ' || !resolve_path(path, resolved)) {
        return 0;
    }

    text = &arguments[path_length + 1];
    while (text[text_length] != '\0') {
        text_length++;
    }
    if (!security_can_write()) {
        logger_write("SHELL", "write: permission denied");
        return 0;
    }
    return vfs_write_file(resolved, text, text_length);
}

static int shell_touch(const char *path)
{
    char resolved[SHELL_PATH_CAPACITY];

    if (!security_can_write()) {
        logger_write("SHELL", "touch: permission denied");
        return 0;
    }
    return resolve_path(path, resolved) && vfs_write_file(resolved, "", 0);
}

static int shell_rm(const char *path)
{
    char resolved[SHELL_PATH_CAPACITY];

    if (!security_can_write()) {
        logger_write("SHELL", "rm: permission denied");
        return 0;
    }
    return resolve_path(path, resolved) && vfs_delete_file(resolved);
}

static int shell_mkdir(const char *path)
{
    char resolved[SHELL_PATH_CAPACITY];

    if (!security_can_write()) {
        logger_write("SHELL", "mkdir: permission denied");
        return 0;
    }
    if (!resolve_path(path, resolved)) {
        logger_write("SHELL", "mkdir: path too long");
        return 0;
    }
    if (vfs_is_directory(resolved)) {
        logger_write("SHELL", "mkdir: directory already exists");
        logger_write("SHELL", resolved);
        return 0;
    }
    if (vfs_create_directory(resolved)) {
        logger_write("SHELL", "mkdir: created");
        logger_write("DIR", resolved);
        return 1;
    }
    logger_write("SHELL", "mkdir: failed");
    logger_write("SHELL", resolved);
    logger_write("SHELL", vfs_write_status_reason(resolved));
    return 0;
}

static int shell_rmdir(const char *path)
{
    char resolved[SHELL_PATH_CAPACITY];

    if (!security_can_write()) {
        logger_write("SHELL", "rmdir: permission denied");
        return 0;
    }
    return resolve_path(path, resolved) && vfs_delete_directory(resolved);
}

static int parse_uint32(const char *text, UINT32 *value)
{
    UINT32 result = 0;

    if (*text == '\0') {
        return 0;
    }
    while (*text != '\0') {
        if (*text < '0' || *text > '9') {
            return 0;
        }
        result = result * 10 + (UINT32)(*text - '0');
        text++;
    }
    *value = result;
    return 1;
}

static int parse_uint64(const char *text, UINT64 *value)
{
    UINT64 result = 0;

    if (*text == '\0') {
        return 0;
    }
    while (*text != '\0') {
        UINT64 digit;
        if (*text < '0' || *text > '9') {
            return 0;
        }
        digit = (UINT64)(*text - '0');
        if (result > (~(UINT64)0 - digit) / 10U) return 0;
        result = result * 10U + digit;
        text++;
    }
    *value = result;
    return 1;
}

static int shell_ps(void)
{
    logger_write_hex("SHELL", "active process count", process_active_count());
    return 1;
}

static int shell_kill(const char *argument)
{
    UINT32 pid;

    if (!security_can_admin()) {
        logger_write("SHELL", "kill: permission denied");
        return 0;
    }
    return parse_uint32(argument, &pid) && process_kill(pid);
}

static int parse_ipv4_address(const char *text, UINT8 address[4])
{
    UINT32 part = 0;
    UINT32 index = 0;
    int has_digit = 0;

    while (*text != '\0') {
        if (*text >= '0' && *text <= '9') {
            has_digit = 1;
            part = part * 10 + (UINT32)(*text - '0');
            if (part > 255) {
                return 0;
            }
        } else if (*text == '.') {
            if (!has_digit || index >= 3) {
                return 0;
            }
            address[index++] = (UINT8)part;
            part = 0;
            has_digit = 0;
        } else {
            return 0;
        }
        text++;
    }
    if (!has_digit || index != 3) {
        return 0;
    }
    address[index] = (UINT8)part;
    return 1;
}

static int shell_ping(const char *argument)
{
    UINT8 target_ip[4];

    if (!parse_ipv4_address(argument, target_ip)) {
        logger_write("SHELL", "ping: invalid IPv4 address");
        return 0;
    }
    if (!virtio_net_ping_ipv4(target_ip)) {
        logger_write("SHELL", "ping failed");
        return 0;
    }
    logger_write("SHELL", "ping ok");
    return 1;
}

static int shell_wget(const char *argument)
{
    static UINT8 response[4096];
    UINT32 response_size = 0;

    if (!security_can_write()) {
        logger_write("SHELL", "wget: permission denied");
        return 0;
    }
    if (
        !virtio_net_http_get_resolved_ipv4(
            80,
            argument,
            response,
            sizeof(response),
            &response_size
        ) &&
        !virtio_net_http_copy_cached(response, sizeof(response), &response_size)
    ) {
        logger_write("SHELL", "wget: connection failed");
        return 0;
    }
    if (
        response_size == 0 ||
        !vfs_write_file("/WGET.TXT", response, response_size)
    ) {
        logger_write("SHELL", "wget: save failed");
        return 0;
    }
    logger_write("SHELL", "wget saved /WGET.TXT");
    return 1;
}

static int shell_http_server(void)
{
    return virtio_net_http_server_once();
}

static int shell_power(void)
{
    if (!security_can_admin()) {
        logger_write("SHELL", "power: permission denied");
        return 0;
    }
    if (!power_can_shutdown() || !power_can_reboot()) {
        logger_write("SHELL", "power unavailable");
        return 0;
    }
    logger_write("SHELL", "power acpi ready");
    logger_write("SHELL", power_can_sleep() ? "sleep available" : "sleep unavailable");
    return 1;
}

static int shell_battery(void)
{
    ASAS_BATTERY_STATUS status = power_battery_status();

    if (status == ASAS_BATTERY_STATUS_UNSUPPORTED) {
        logger_write("SHELL", "battery namespace detected");
        logger_write("SHELL", "battery charge unsupported");
    } else {
        logger_write("SHELL", "battery namespace unavailable");
    }
    return 1;
}

static int shell_whoami(void)
{
    logger_write("SHELL", security_current_user());
    return 1;
}

static int shell_permissions(void)
{
    UINT32 permissions = security_current_permissions();

    logger_write("SHELL", (permissions & SECURITY_PERMISSION_READ) ? "permission read" : "permission no-read");
    logger_write("SHELL", (permissions & SECURITY_PERMISSION_WRITE) ? "permission write" : "permission no-write");
    logger_write("SHELL", (permissions & SECURITY_PERMISSION_EXECUTE) ? "permission execute" : "permission no-execute");
    logger_write("SHELL", (permissions & SECURITY_PERMISSION_ADMIN) ? "permission admin" : "permission no-admin");
    return 1;
}

static int shell_beep(void)
{
    if (!audio_beep()) {
        logger_write("SHELL", "beep failed");
        return 0;
    }
    logger_write("SHELL", "beep ok");
    logger_write("SHELL", audio_hda_controller_present() ? "hda controller detected" : "hda controller unavailable");
    return 1;
}

static int shell_touchpad(void)
{
    if (laptop_touchpad_present()) {
        logger_write("SHELL", "touchpad namespace detected");
    } else {
        logger_write("SHELL", "touchpad namespace unavailable");
    }
    return 1;
}

static int shell_wifi(void)
{
    if (laptop_wifi_present()) {
        logger_write("SHELL", "wifi controller detected");
    } else {
        logger_write("SHELL", "wifi controller unavailable");
    }
    return 1;
}

static int parse_two_paths(
    const char *arguments,
    char first[SHELL_PATH_CAPACITY],
    char second[SHELL_PATH_CAPACITY]
)
{
    UINT32 first_length = 0;
    UINT32 second_length = 0;

    while (arguments[first_length] != '\0' && arguments[first_length] != ' ') {
        if (first_length + 1 >= SHELL_PATH_CAPACITY) {
            return 0;
        }
        first[first_length] = arguments[first_length];
        first_length++;
    }
    first[first_length] = '\0';
    if (first_length == 0 || arguments[first_length] != ' ') {
        return 0;
    }
    arguments += first_length + 1;
    while (arguments[second_length] != '\0') {
        if (second_length + 1 >= SHELL_PATH_CAPACITY) {
            return 0;
        }
        second[second_length] = arguments[second_length];
        second_length++;
    }
    second[second_length] = '\0';
    return second_length != 0;
}

static int shell_copy(const char *arguments, int remove_source)
{
    static UINT8 buffer[4096];
    char source_path[SHELL_PATH_CAPACITY];
    char destination_path[SHELL_PATH_CAPACITY];
    char resolved_source[SHELL_PATH_CAPACITY];
    char resolved_destination[SHELL_PATH_CAPACITY];
    UINT64 handle;
    UINT64 bytes;

    if (
        !parse_two_paths(arguments, source_path, destination_path) ||
        !resolve_path(source_path, resolved_source) ||
        !resolve_path(destination_path, resolved_destination)
    ) {
        return 0;
    }
    if (!security_can_read() || !security_can_write()) {
        logger_write("SHELL", "copy: permission denied");
        return 0;
    }
    if (remove_source && vfs_rename(resolved_source, resolved_destination)) return 1;
    handle = vfs_open(resolved_source);
    if (handle == 0) {
        return 0;
    }
    bytes = vfs_read(handle, buffer, sizeof(buffer));
    (void)vfs_close(handle);
    if (bytes == sizeof(buffer) || !vfs_write_file(resolved_destination, buffer, bytes)) {
        return 0;
    }
    return !remove_source || vfs_delete_file(resolved_source);
}

static int next_argument(const char **cursor, char *out, UINT32 capacity)
{
    UINT32 index = 0;
    if (cursor == 0 || *cursor == 0 || out == 0 || capacity == 0) return 0;
    while (**cursor == ' ') (*cursor)++;
    if (**cursor == '\0') return 0;
    while (**cursor != '\0' && **cursor != ' ' && index + 1U < capacity) {
        out[index++] = **cursor;
        (*cursor)++;
    }
    while (**cursor != '\0' && **cursor != ' ') (*cursor)++;
    out[index] = '\0';
    return index != 0;
}

static int shell_vdisk(const char *arguments)
{
    char operation[SHELL_PATH_CAPACITY];
    const char *cursor = arguments;
    if (!security_can_admin()) {
        logger_write("SHELL", "vdisk: permission denied");
        return 0;
    }
    if (!next_argument(&cursor, operation, sizeof(operation))) {
            logger_write("SHELL", "vdisk: usage attach|detach|info|compact|check|check-image");
        return 0;
    }
    if (strings_equal(operation, "attach")) {
        char format[SHELL_PATH_CAPACITY];
        char path[SHELL_PATH_CAPACITY];
        char resolved[SHELL_PATH_CAPACITY];
        char mode[SHELL_PATH_CAPACITY];
        UINT32 flags = 0;
        ASAS_BLOCK_DEVICE *device;
        if (!next_argument(&cursor, format, sizeof(format)) ||
            !next_argument(&cursor, path, sizeof(path)) ||
            !resolve_path(path, resolved)) {
            logger_write("SHELL", "vdisk attach: usage vdisk attach auto|raw|vhd-fixed PATH [ro]");
            return 0;
        }
        if (next_argument(&cursor, mode, sizeof(mode))) {
            if (strings_equal(mode, "ro") ||
                strings_equal(mode, "safe-read-only")) {
                flags |= VDISK_FLAG_READ_ONLY;
            } else if (!strings_equal(mode, "rw")) {
                logger_write("SHELL", "vdisk attach: mode must be rw or ro");
                return 0;
            }
        }
        if (strings_equal(format, "auto")) {
            device = virtual_disk_attach_auto(resolved, flags);
        } else if (strings_equal(format, "raw")) {
            device = virtual_disk_attach_raw(resolved, flags);
        } else if (strings_equal(format, "vhd-fixed")) {
            device = virtual_disk_attach_fixed_vhd(resolved, flags);
        } else {
            logger_write("SHELL", "vdisk attach: format must be auto, raw, or vhd-fixed");
            return 0;
        }
        if (device == 0) {
            logger_write("SHELL", "vdisk attach failed");
            return 0;
        }
        (void)partition_scan_device(device);
        (void)vfs_mount_all_volumes();
        logger_write("SHELL", "vdisk attached");
        logger_write("SHELL", device->name);
        return 1;
    }
    if (strings_equal(operation, "detach")) {
        char name[SHELL_PATH_CAPACITY];
        if (!next_argument(&cursor, name, sizeof(name))) {
            logger_write("SHELL", "vdisk detach: usage vdisk detach NAME");
            return 0;
        }
        if (!virtual_disk_detach(name)) {
            logger_write("SHELL", "vdisk detach failed");
            return 0;
        }
        logger_write("SHELL", "vdisk detached");
        return 1;
    }
    if (strings_equal(operation, "info")) {
        UINT32 index;
        logger_write_hex("SHELL", "virtual disks", virtual_disk_count());
        for (index = 0; index < virtual_disk_count(); index++) {
            const ASAS_VDISK_INFO *info = virtual_disk_get(index);
            if (info == 0) continue;
            logger_write("SHELL", info->name);
            logger_write("SHELL", info->format);
            logger_write("SHELL", info->path);
            logger_write_hex("SHELL", "size", info->size);
            logger_write_hex("SHELL", "file size", info->file_size);
            logger_write_hex("SHELL", "read only", info->read_only);
        }
        return 1;
    }
    if (strings_equal(operation, "check")) {
        char name[SHELL_PATH_CAPACITY];
        int ok;
        if (!next_argument(&cursor, name, sizeof(name))) {
            logger_write("SHELL", "vdisk check: usage vdisk check NAME");
            return 0;
        }
        ok = virtual_disk_check(name);
        logger_write("SHELL", ok ? "vdisk check ok" : "vdisk check failed");
        return ok;
    }
    if (strings_equal(operation, "check-image")) {
        char format[SHELL_PATH_CAPACITY];
        char path[SHELL_PATH_CAPACITY];
        char resolved[SHELL_PATH_CAPACITY];
        int ok;
        if (!next_argument(&cursor, format, sizeof(format)) ||
            !next_argument(&cursor, path, sizeof(path)) ||
            !resolve_path(path, resolved)) {
            logger_write("SHELL", "vdisk check-image: usage vdisk check-image raw|vhd-fixed|qcow2|vhdx PATH");
            return 0;
        }
        ok = virtual_disk_validate_image(resolved, format);
        logger_write("SHELL", ok ? "vdisk image ok" : "vdisk image invalid");
        return ok;
    }
    if (strings_equal(operation, "compact")) {
        logger_write("SHELL", "vdisk compact: not supported for RAW yet");
        return 0;
    }
    logger_write("SHELL", "vdisk: unknown operation");
    return 0;
}

static int shell_disk(const char *arguments)
{
    char operation[SHELL_PATH_CAPACITY];
    const char *cursor = arguments;
    if (!next_argument(&cursor, operation, sizeof(operation))) {
        logger_write("SHELL", "disk: usage list|partitions|volumes|caps|rescan|remove|dry-run|format|partition");
        return 0;
    }
    if (strings_equal(operation, "list") || strings_equal(operation, "info"))
        return disk_management_list_disks();
    if (strings_equal(operation, "partitions"))
        return disk_management_list_partitions();
    if (strings_equal(operation, "volumes"))
        return disk_management_list_volumes();
    if (strings_equal(operation, "caps") || strings_equal(operation, "capabilities"))
        return disk_management_list_capabilities();
    if (strings_equal(operation, "stats")) {
        UINT32 index;
        for (index = 0; index < block_device_count(); index++) {
            ASAS_BLOCK_DEVICE *device = block_device_get(index);
            ASAS_BLOCK_DEVICE_TELEMETRY stats;
            if (device == 0 ||
                !block_device_get_telemetry(device, &stats)) continue;
            logger_write("SHELL", device->name);
            logger_write_hex("SHELL", "read ops", stats.read_ops);
            logger_write_hex("SHELL", "write ops", stats.write_ops);
            logger_write_hex("SHELL", "flush ops", stats.flush_ops);
            logger_write_hex("SHELL", "blocks read", stats.blocks_read);
            logger_write_hex("SHELL", "blocks written", stats.blocks_written);
            logger_write_hex("SHELL", "cache hits", stats.cache_hits);
            logger_write_hex("SHELL", "cache misses", stats.cache_misses);
            logger_write_hex("SHELL", "read ahead", stats.read_ahead_ops);
            logger_write_hex("SHELL", "retries", stats.retries);
            logger_write_hex("SHELL", "errors", stats.errors);
            logger_write_hex("SHELL", "out of bounds", stats.out_of_bounds);
        }
        return 1;
    }
    if (strings_equal(operation, "rescan")) {
        int count;
        if (!security_can_admin()) {
            logger_write("SHELL", "disk rescan: permission denied");
            return 0;
        }
        count = disk_management_hotplug_rescan();
        logger_write_hex("SHELL", "mounted volumes", (UINT64)(UINT32)count);
        return count != 0;
    }
    if (strings_equal(operation, "remove")) {
        char device[SHELL_PATH_CAPACITY];
        int count;
        if (!security_can_admin()) {
            logger_write("SHELL", "disk remove: permission denied");
            return 0;
        }
        if (!next_argument(&cursor, device, sizeof(device))) {
            logger_write("SHELL", "disk remove: usage disk remove DEVICE");
            return 0;
        }
        count = disk_management_device_removed(device);
        return count != 0;
    }
    if (strings_equal(operation, "dry-run")) {
        char kind[SHELL_PATH_CAPACITY];
        char target[SHELL_PATH_CAPACITY];
        ASAS_DISK_MANAGEMENT_OP op = DISK_MANAGEMENT_OP_FORMAT;
        if (!next_argument(&cursor, kind, sizeof(kind)) ||
            !next_argument(&cursor, target, sizeof(target))) {
            logger_write("SHELL", "disk dry-run: usage disk dry-run format|partition|mount TARGET");
            return 0;
        }
        if (strings_equal(kind, "partition"))
            op = DISK_MANAGEMENT_OP_PARTITION_CREATE;
        else if (strings_equal(kind, "mount"))
            op = DISK_MANAGEMENT_OP_MOUNT;
        else if (!strings_equal(kind, "format")) {
            logger_write("SHELL", "disk dry-run: unknown operation");
            return 0;
        }
        return disk_management_dry_run(op, target, 0);
    }
    if (strings_equal(operation, "format")) {
        char device[SHELL_PATH_CAPACITY];
        char fs_name[SHELL_PATH_CAPACITY];
        char mode[SHELL_PATH_CAPACITY];
        int dry_run = 0;
        int confirmed = 0;
        if (!security_can_admin()) {
            logger_write("SHELL", "disk format: permission denied");
            return 0;
        }
        if (!next_argument(&cursor, device, sizeof(device)) ||
            !next_argument(&cursor, fs_name, sizeof(fs_name))) {
            logger_write("SHELL", "disk format: usage disk format DEVICE fat32|exfat|udf|ext2 [dry-run]");
            return 0;
        }
        if (next_argument(&cursor, mode, sizeof(mode))) {
            dry_run = strings_equal(mode, "dry-run");
            confirmed = strings_equal(mode, "confirm");
        }
        if (!dry_run && !confirmed) {
            logger_write("SHELL", "disk format: add dry-run or confirm");
            return 0;
        }
        return disk_management_format(device, fs_name, dry_run);
    }
    if (strings_equal(operation, "partition")) {
        char scheme[SHELL_PATH_CAPACITY];
        char action[SHELL_PATH_CAPACITY];
        char device[SHELL_PATH_CAPACITY];
        char slot_text[SHELL_PATH_CAPACITY];
        char start_text[SHELL_PATH_CAPACITY];
        char count_text[SHELL_PATH_CAPACITY];
        char mode[SHELL_PATH_CAPACITY];
        UINT32 slot;
        UINT64 start_lba = 0;
        UINT64 block_count = 0;
        int dry_run = 0;
        int confirmed = 0;
        if (!security_can_admin()) {
            logger_write("SHELL", "disk partition: permission denied");
            return 0;
        }
        if (!next_argument(&cursor, scheme, sizeof(scheme)) ||
            !next_argument(&cursor, action, sizeof(action)) ||
            !next_argument(&cursor, device, sizeof(device)) ||
            !next_argument(&cursor, slot_text, sizeof(slot_text)) ||
            !parse_uint32(slot_text, &slot)) {
            logger_write("SHELL", "disk partition: usage disk partition mbr|gpt create|delete|resize DEVICE SLOT [START COUNT] [dry-run]");
            return 0;
        }
        if (next_argument(&cursor, start_text, sizeof(start_text))) {
            if (strings_equal(start_text, "dry-run")) dry_run = 1;
            else if (strings_equal(start_text, "confirm")) confirmed = 1;
            else if (!parse_uint64(start_text, &start_lba)) return 0;
        }
        if (next_argument(&cursor, count_text, sizeof(count_text))) {
            if (strings_equal(count_text, "dry-run")) dry_run = 1;
            else if (strings_equal(count_text, "confirm")) confirmed = 1;
            else if (!parse_uint64(count_text, &block_count)) return 0;
        }
        if (next_argument(&cursor, mode, sizeof(mode))) {
            dry_run = strings_equal(mode, "dry-run");
            confirmed = strings_equal(mode, "confirm");
        }
        if (!dry_run && !confirmed) {
            logger_write("SHELL", "disk partition: add dry-run or confirm");
            return 0;
        }
        if (strings_equal(scheme, "mbr"))
            return disk_management_partition_mbr(action, device, slot, 0x0c,
                                                 start_lba, block_count,
                                                 dry_run);
        if (strings_equal(scheme, "gpt"))
            return disk_management_partition_gpt(action, device, slot,
                                                 start_lba, block_count,
                                                 dry_run);
        logger_write("SHELL", "disk partition: scheme must be mbr or gpt");
        return 0;
    }
    logger_write("SHELL", "disk: unknown operation");
    return 0;
}

int shell_execute(const char *command)
{
    char expanded[256];
    char cmd_clean[256];
    char op_arg[SHELL_PATH_CAPACITY];
    int  op;

    /* Step 1: expand $VAR references */
    expand_env(command, expanded, sizeof(expanded));

    /* Step 2: handle pipe / redirect operators */
    op = parse_redirect(expanded, cmd_clean, sizeof(cmd_clean),
                        op_arg, sizeof(op_arg));
    if (op != REDIR_NONE) {
        int result = 0;
        if (op == REDIR_IN) {
            char in_resolved[SHELL_PATH_CAPACITY];
            UINT64 in_hdl, sz;
            shell_stdin_ptr = 0; shell_stdin_len = 0;
            if (resolve_path(op_arg, in_resolved)) {
                in_hdl = vfs_open(in_resolved);
                if (in_hdl) {
                    sz = vfs_read(in_hdl, shell_stdin_data, SHELL_CAPTURE_MAX - 1);
                    (void)vfs_close(in_hdl);
                    shell_stdin_data[sz] = '\0';
                    shell_stdin_ptr = shell_stdin_data;
                    shell_stdin_len = (UINT32)sz;
                }
            }
            result = shell_execute(cmd_clean);
            shell_stdin_ptr = 0; shell_stdin_len = 0;
            return result;
        }
        start_capture();
        result = shell_execute(cmd_clean);
        stop_capture();
        if (op == REDIR_OUT) {
            char out_resolved[SHELL_PATH_CAPACITY];
            if (resolve_path(op_arg, out_resolved))
                vfs_write_file(out_resolved, shell_capture_buf, shell_capture_len);
        } else if (op == REDIR_APPEND) {
            char out_resolved[SHELL_PATH_CAPACITY];
            if (resolve_path(op_arg, out_resolved)) {
                static UINT8 app_buf[SHELL_CAPTURE_MAX];
                UINT64 ex_size = 0;
                UINT64 ex_hdl  = vfs_open(out_resolved);
                if (ex_hdl) {
                    ex_size = vfs_read(ex_hdl, app_buf, sizeof(app_buf));
                    (void)vfs_close(ex_hdl);
                }
                if (ex_size + shell_capture_len < SHELL_CAPTURE_MAX) {
                    UINT32 k;
                    for (k = 0; k < shell_capture_len; k++)
                        app_buf[ex_size + k] = (UINT8)shell_capture_buf[k];
                    vfs_write_file(out_resolved, app_buf, ex_size + shell_capture_len);
                }
            }
        } else if (op == REDIR_PIPE) {
            UINT32 k;
            for (k = 0; k < shell_capture_len && k < SHELL_CAPTURE_MAX - 1; k++)
                shell_stdin_data[k] = shell_capture_buf[k];
            shell_stdin_data[shell_capture_len] = '\0';
            shell_stdin_ptr = shell_stdin_data;
            shell_stdin_len = shell_capture_len;
            result = shell_execute(op_arg);
            shell_stdin_ptr = 0; shell_stdin_len = 0;
        }
        return result;
    }

    /* Use the env-expanded command string for all further dispatch */
    command = expanded;

    if (strings_equal(command, "help")) {
        logger_write("SHELL", "command | description | params | example");
        logger_write("SHELL", "help | show help | none | help");
        logger_write("SHELL", "pwd | current dir | none | pwd");
        logger_write("SHELL", "ls | list files | [path] | ls /ASAS");
        logger_write("SHELL", "disks | list disks and partitions | none | disks");
        logger_write("SHELL", "disk | management service | list|partitions|volumes|caps|rescan|remove|stats|format|partition | disk list");
        logger_write("SHELL", "disk dry-run | validate risky op | format|partition target | disk dry-run format disk1p1");
        logger_write("SHELL", "disk format | format validation | device fs dry-run|confirm | disk format disk1p1 ext2 dry-run");
        logger_write("SHELL", "disk partition | mutate partitions | mbr|gpt create|delete|resize device slot start count dry-run|confirm | disk partition mbr create disk1 0 2048 4096 dry-run");
        logger_write("SHELL", "disk rescan | rescan storage | none | disk rescan");
        logger_write("SHELL", "disk stats | block I/O telemetry | none | disk stats");
        logger_write("SHELL", "mounts | list mounted volumes | none | mounts");
        logger_write("SHELL", "mount | mount device | device path | mount disk1p1 /data");
        logger_write("SHELL", "mount-ro | read-only mount | device path | mount-ro USB /media/usb0");
        logger_write("SHELL", "mount-noexec | no-exec mount | device path | mount-noexec DATA /data");
        logger_write("SHELL", "remount | change mount flags | path mode | remount /data rw");
        logger_write("SHELL", "unmount | unmount volume | path | unmount /media/usb0");
        logger_write("SHELL", "fs | filesystem service | op path [dry-run] | fs repair /data dry-run");
        logger_write("SHELL", "vdisk | virtual disks | attach/detach/info/check | vdisk attach auto|raw|vhd-fixed /disk.img");
        logger_write("SHELL", "cat | print file | path | cat /DISK.TXT");
        logger_write("SHELL", "cd | change dir | path | cd /ASAS");
        logger_write("SHELL", "touch | create file | path | touch /A.TXT");
        logger_write("SHELL", "write | write text | path text | write /A.TXT hi");
        logger_write("SHELL", "rm | delete file | path | rm /A.TXT");
        logger_write("SHELL", "mkdir | create dir | path | mkdir /WORK");
        logger_write("SHELL", "rmdir | delete dir | path | rmdir /WORK");
        logger_write("SHELL", "cp | copy file | src dst | cp /A.TXT /B.TXT");
        logger_write("SHELL", "mv | move file | src dst | mv /A.TXT /B.TXT");
        logger_write("SHELL", "ps | list processes | none | ps");
        logger_write("SHELL", "kill | stop process | pid | kill 2");
        logger_write("SHELL", "whoami | current user | none | whoami");
        logger_write("SHELL", "permissions | user rights | none | permissions");
        logger_write("SHELL", "ping | ICMP test | ipv4 | ping 10.0.2.2");
        logger_write("SHELL", "wget | HTTP GET | host | wget example.com");
        logger_write("SHELL", "http-server | serve once | none | http-server");
        logger_write("SHELL", "power | power status | none | power");
        logger_write("SHELL", "shutdown | power off | none | shutdown");
        logger_write("SHELL", "reboot | restart | none | reboot");
        logger_write("SHELL", "sleep | enter sleep | none | sleep");
        logger_write("SHELL", "battery | battery info | none | battery");
        logger_write("SHELL", "beep | PC speaker | none | beep");
        logger_write("SHELL", "touchpad | detect pad | none | touchpad");
        logger_write("SHELL", "wifi | detect wifi | none | wifi");
        logger_write("SHELL", "export | set env | KEY=VALUE | export A=1");
        logger_write("SHELL", "env | list env | none | env");
        logger_write("SHELL", "grep | filter text | pattern | cat X | grep hi");
        logger_write("SHELL", "wc | count input | none | cat X | wc");
        return 1;
    }
    if (strings_equal(command, "whoami")) {
        return shell_whoami();
    }
    if (strings_equal(command, "permissions")) {
        return shell_permissions();
    }
    if (strings_equal(command, "pwd")) {
        logger_write("SHELL", current_directory);
        return 1;
    }
    if (starts_with(command, "cd ") && command[3] != '\0') {
        return shell_cd(&command[3]);
    }
    if (strings_equal(command, "ls")) {
        return shell_ls(current_directory);
    }
    if (starts_with(command, "ls ") && command[3] != '\0') {
        return shell_ls(&command[3]);
    }
    if (starts_with(command, "cat ") && command[4] != '\0') {
        return shell_cat(&command[4]);
    }
    if (starts_with(command, "touch ") && command[6] != '\0') {
        return shell_touch(&command[6]);
    }
    if (starts_with(command, "write ") && command[6] != '\0') {
        return shell_write(&command[6]);
    }
    if (starts_with(command, "rm ") && command[3] != '\0') {
        return shell_rm(&command[3]);
    }
    if (starts_with(command, "mkdir ") && command[6] != '\0') {
        return shell_mkdir(&command[6]);
    }
    if (starts_with(command, "rmdir ") && command[6] != '\0') {
        return shell_rmdir(&command[6]);
    }
    if (strings_equal(command, "ps")) {
        return shell_ps();
    }
    if (starts_with(command, "kill ") && command[5] != '\0') {
        return shell_kill(&command[5]);
    }
    if (starts_with(command, "ping ") && command[5] != '\0') {
        return shell_ping(&command[5]);
    }
    if (starts_with(command, "wget ") && command[5] != '\0') {
        return shell_wget(&command[5]);
    }
    if (strings_equal(command, "http-server")) {
        return shell_http_server();
    }
    if (strings_equal(command, "power")) {
        return shell_power();
    }
    if (strings_equal(command, "shutdown")) {
        if (!security_can_admin()) {
            logger_write("SHELL", "shutdown: permission denied");
            return 0;
        }
        return power_shutdown();
    }
    if (strings_equal(command, "reboot")) {
        if (!security_can_admin()) {
            logger_write("SHELL", "reboot: permission denied");
            return 0;
        }
        return power_reboot();
    }
    if (strings_equal(command, "sleep")) {
        if (!security_can_admin()) {
            logger_write("SHELL", "sleep: permission denied");
            return 0;
        }
        return power_sleep();
    }
    if (strings_equal(command, "battery")) {
        return shell_battery();
    }
    if (strings_equal(command, "beep")) {
        return shell_beep();
    }
    if (strings_equal(command, "touchpad")) {
        return shell_touchpad();
    }
    if (strings_equal(command, "wifi")) {
        return shell_wifi();
    }
    if (starts_with(command, "cp ") && command[3] != '\0') {
        return shell_copy(&command[3], 0);
    }
    if (starts_with(command, "mv ") && command[3] != '\0') {
        return shell_copy(&command[3], 1);
    }
    if (starts_with(command, "export ") && command[7] != '\0') {
        const char *kv = &command[7];
        char name[ENV_NAME_LEN];
        UINT32 ni = 0;
        while (kv[ni] && kv[ni] != '=' && ni + 1 < ENV_NAME_LEN)
            name[ni++] = kv[ni];
        name[ni] = '\0';
        if (kv[ni] == '=') {
            env_set(name, &kv[ni + 1]);
            logger_write("SHELL", "ok");
        } else {
            logger_write("SHELL", "export: usage: export KEY=VALUE");
        }
        return 1;
    }
    if (strings_equal(command, "env")) {
        UINT32 i;
        for (i = 0; i < env_count; i++) {
            char out[ENV_NAME_LEN + ENV_VAL_LEN + 2];
            UINT32 j = 0, k = 0;
            while (env_names[i][k]) out[j++] = env_names[i][k++];
            out[j++] = '=';
            k = 0;
            while (env_values[i][k]) out[j++] = env_values[i][k++];
            out[j] = '\0';
            logger_write("SHELL", out);
        }
        return 1;
    }
    if (starts_with(command, "grep ") && command[5] != '\0') {
        return shell_grep(&command[5]);
    }
    if (strings_equal(command, "wc")) {
        return shell_wc();
    }
    if (starts_with(command, "disk ") && command[5] != '\0') {
        return shell_disk(&command[5]);
    }
    if (strings_equal(command, "disk rescan")) {
        int count;
        if (!security_can_admin()) {
            logger_write("SHELL", "disk rescan: permission denied");
            return 0;
        }
        count = vfs_rescan_devices();
        logger_write_hex("SHELL", "mounted volumes", (UINT64)(UINT32)count);
        return count != 0;
    }
    if (strings_equal(command, "disk stats")) {
        UINT32 index;
        for (index = 0; index < block_device_count(); index++) {
            ASAS_BLOCK_DEVICE *device = block_device_get(index);
            ASAS_BLOCK_DEVICE_TELEMETRY stats;
            if (device == 0 ||
                !block_device_get_telemetry(device, &stats)) continue;
            logger_write("SHELL", device->name);
            logger_write_hex("SHELL", "read ops", stats.read_ops);
            logger_write_hex("SHELL", "write ops", stats.write_ops);
            logger_write_hex("SHELL", "flush ops", stats.flush_ops);
            logger_write_hex("SHELL", "blocks read", stats.blocks_read);
            logger_write_hex("SHELL", "blocks written", stats.blocks_written);
            logger_write_hex("SHELL", "cache hits", stats.cache_hits);
            logger_write_hex("SHELL", "cache misses", stats.cache_misses);
            logger_write_hex("SHELL", "read ahead", stats.read_ahead_ops);
            logger_write_hex("SHELL", "retries", stats.retries);
            logger_write_hex("SHELL", "errors", stats.errors);
            logger_write_hex("SHELL", "out of bounds", stats.out_of_bounds);
        }
        return 1;
    }
    if (strings_equal(command, "disks")) {
        return disk_management_list_disks() &&
               disk_management_list_partitions();
    }
    if (strings_equal(command, "mounts")) {
        return disk_management_list_volumes();
    }
    if (starts_with(command, "mount ") && command[6] != '\0') {
        char device[SHELL_PATH_CAPACITY];
        char mount_point[SHELL_PATH_CAPACITY];
        if (!security_can_admin()) {
            logger_write("SHELL", "mount: permission denied");
            return 0;
        }
        if (!parse_two_paths(&command[6], device, mount_point) ||
            mount_point[0] != '/') {
            logger_write("SHELL", "mount: usage mount DEVICE /PATH");
            return 0;
        }
        return disk_management_mount(device, mount_point, 0);
    }
    if ((starts_with(command, "mount-ro ") && command[9] != '\0') ||
        (starts_with(command, "mount-noexec ") && command[13] != '\0')) {
        char device[SHELL_PATH_CAPACITY];
        char mount_point[SHELL_PATH_CAPACITY];
        int no_exec = starts_with(command, "mount-noexec ");
        const char *arguments = no_exec ? &command[13] : &command[9];
        if (!security_can_admin()) {
            logger_write("SHELL", "mount: permission denied");
            return 0;
        }
        if (!parse_two_paths(arguments, device, mount_point) ||
            mount_point[0] != '/') return 0;
        return disk_management_mount(device, mount_point,
            no_exec ? FILESYSTEM_FLAG_NO_EXEC : FILESYSTEM_FLAG_READ_ONLY);
    }
    if (starts_with(command, "remount ") && command[8] != '\0') {
        char mount_point[SHELL_PATH_CAPACITY];
        char mode[SHELL_PATH_CAPACITY];
        UINT32 flags = 0;
        if (!security_can_admin()) {
            logger_write("SHELL", "remount: permission denied");
            return 0;
        }
        if (!parse_two_paths(&command[8], mount_point, mode) ||
            mount_point[0] != '/') {
            logger_write("SHELL", "remount: usage remount /PATH rw|ro|noexec");
            return 0;
        }
        if (strings_equal(mode, "rw")) flags = 0;
        else if (strings_equal(mode, "ro")) flags = FILESYSTEM_FLAG_READ_ONLY;
        else if (strings_equal(mode, "noexec")) flags = FILESYSTEM_FLAG_NO_EXEC;
        else {
            logger_write("SHELL", "remount: mode must be rw, ro, or noexec");
            return 0;
        }
        return disk_management_remount(mount_point, flags);
    }
    if (starts_with(command, "unmount ") && command[8] != '\0') {
        if (!security_can_admin()) {
            logger_write("SHELL", "unmount: permission denied");
            return 0;
        }
        return disk_management_unmount(&command[8]);
    }
    if (starts_with(command, "fs ") && command[3] != '\0') {
        char operation[SHELL_PATH_CAPACITY];
        char path[SHELL_PATH_CAPACITY];
        char mode[SHELL_PATH_CAPACITY];
        char resolved[SHELL_PATH_CAPACITY];
        const char *cursor = &command[3];
        int dry_run = 0;
        if (!next_argument(&cursor, operation, sizeof(operation)) ||
            !next_argument(&cursor, path, sizeof(path)) ||
            !resolve_path(path, resolved)) {
            logger_write("SHELL", "fs: usage fs info|sync|check|repair PATH");
            return 0;
        }
        if (next_argument(&cursor, mode, sizeof(mode)))
            dry_run = strings_equal(mode, "dry-run");
        if (strings_equal(operation, "info")) {
            logger_write("SHELL", resolved);
            logger_write_hex("SHELL", "size", vfs_file_size(resolved));
            logger_write_hex("SHELL", "directory", vfs_is_directory(resolved));
            logger_write("SHELL", vfs_write_status_reason(resolved));
            return 1;
        }
        if (strings_equal(operation, "sync")) {
            if (!security_can_admin()) {
                logger_write("SHELL", "fs sync: permission denied");
                return 0;
            }
            return vfs_sync_path(resolved);
        }
        if (strings_equal(operation, "check")) {
            return disk_management_fs_check(resolved);
        }
        if (strings_equal(operation, "repair")) {
            return disk_management_fs_repair(resolved, dry_run);
        }
        logger_write("SHELL", "fs: unknown operation");
        return 0;
    }
    if (starts_with(command, "vdisk ") && command[6] != '\0') {
        return shell_vdisk(&command[6]);
    }
    if (strings_equal(command, "vfsdiag")) {
        /* Show VFS internals for debugging — all output at SHELL level so
           it appears in the terminal.                                      */
        static UINT8 diag_buf[512];
        int vc = vfs_get_volume_count();
        int vi;
        const VFS_VOLUME_INFO *v0;
        int rd0;
        UINT32 mi = 0; (void)mi;

        /* Volume count and primary volume info */
        logger_write_hex("SHELL", "vfs volumes", (UINT64)(UINT32)vc);
        v0 = vfs_get_volume(0);
        if (v0) {
            logger_write_hex("SHELL", "vol0.target", v0->target);
            logger_write_hex("SHELL", "vol0.lun",    v0->lun);
            logger_write_hex("SHELL", "vol0.fs",     v0->fs_type);
            /* 0=FAT16 1=FAT32 2=NTFS 3=NONE */
        }
        for (vi = 1; vi < vc; vi++) {
            const VFS_VOLUME_INFO *v = vfs_get_volume(vi);
            if (v) {
                logger_write_hex("SHELL", "volN.target", v->target);
                logger_write_hex("SHELL", "volN.lun",    v->lun);
                logger_write_hex("SHELL", "volN.fs",     v->fs_type);
            }
        }

        /* Current block device */
        logger_write_hex("SHELL", "blk.cur_tgt",
            (UINT64)virtio_block_get_current_target());
        logger_write_hex("SHELL", "blk.cur_lun",
            (UINT64)virtio_block_get_current_lun());

        /* Test sector reads at LBA 0 and LBA 513 (FAT16 root dir) */
        rd0 = virtio_block_read_sector(0, diag_buf);
        logger_write_hex("SHELL", "rd LBA0 ok", (UINT64)(UINT32)rd0);
        if (rd0) {
            /* Show OEM ID bytes 3..10 as hex */
            logger_write_hex("SHELL", "LBA0 oem[3]", diag_buf[3]);
            logger_write_hex("SHELL", "LBA0 oem[4]", diag_buf[4]);
            logger_write_hex("SHELL", "LBA0 reserved_sectors",
                (UINT64)((UINT16)diag_buf[14] | ((UINT16)diag_buf[15] << 8)));
            logger_write_hex("SHELL", "LBA0 sectors_per_fat",
                (UINT64)((UINT16)diag_buf[22] | ((UINT16)diag_buf[23] << 8)));
            logger_write_hex("SHELL", "LBA0 root_entries",
                (UINT64)((UINT16)diag_buf[17] | ((UINT16)diag_buf[18] << 8)));
        }
        /* Test sectors 1, 2, 10 (FAT area) and 513 (root dir) */
        {
            static const UINT64 test_lbas[] = {1, 2, 10, 513};
            UINT32 ti;
            for (ti = 0; ti < 4U; ti++) {
                int rdi = virtio_block_read_sector(test_lbas[ti], diag_buf);
                /* Build label "rd LBAnnnn" */
                UINT64 lba = test_lbas[ti];
                char lbl[32];
                UINT32 li = 0, di;
                UINT8 digits[8];
                lbl[li++]='r'; lbl[li++]='d'; lbl[li++]=' ';
                lbl[li++]='L'; lbl[li++]='B'; lbl[li++]='A';
                di = 0;
                do { digits[di++]=(UINT8)('0'+(lba%10)); lba/=10; } while (lba);
                while (di--) lbl[li++]=(char)digits[di];
                lbl[li] = '\0';
                logger_write_hex("SHELL", lbl, (UINT64)(UINT32)rdi);
                if (rdi) {
                    char bl[32];
                    UINT32 bx = 0;
                    bl[bx++]='b'; bl[bx++]='['; bl[bx++]='0'; bl[bx++]=']';
                    bl[bx] = '\0';
                    logger_write_hex("SHELL", bl, diag_buf[0]);
                    bl[2]='1'; logger_write_hex("SHELL", bl, diag_buf[1]);
                    bl[2]='2'; logger_write_hex("SHELL", bl, diag_buf[2]);
                }
            }
        }
        (void)mi;
        return 1;
    }

    logger_write("SHELL", "command not found");
    return 0;
}

int shell_self_test(void)
{
    static UINT8 large_file[1200];
    UINT8 read_buffer[1200];
    UINT64 large_handle;
    UINT64 large_bytes;
    UINT32 index;
    static const char *growth_files[] = {
        "/G00.TMP", "/G01.TMP", "/G02.TMP", "/G03.TMP", "/G04.TMP",
        "/G05.TMP", "/G06.TMP", "/G07.TMP", "/G08.TMP", "/G09.TMP",
        "/G10.TMP", "/G11.TMP", "/G12.TMP", "/G13.TMP", "/G14.TMP",
        "/G15.TMP", "/G16.TMP", "/G17.TMP"
    };

    for (index = 0; index < sizeof(large_file); index++) {
        large_file[index] = (UINT8)('A' + (index % 26));
    }
    for (index = 0; index < sizeof(growth_files) / sizeof(growth_files[0]); index++) {
        if (!vfs_write_file(growth_files[index], "x", 1)) return 0;
    }
    if (vfs_file_size(growth_files[17]) != 1) return 0;
    for (index = 0; index < sizeof(growth_files) / sizeof(growth_files[0]); index++) {
        if (!vfs_delete_file(growth_files[index])) return 0;
    }

    return (
        shell_execute("help") &&
        shell_execute("whoami") &&
        shell_execute("permissions") &&
        shell_execute("disks") &&
        shell_execute("pwd") &&
        shell_execute("ls") &&
        shell_execute("ls /ASAS") &&
        shell_execute("cat /disk.txt") &&
        shell_execute("cd /ASAS") &&
        shell_execute("pwd") &&
        shell_execute("ls") &&
        shell_execute("cat README.TXT") &&
        shell_execute("cd /") &&
        shell_execute("touch /EMPTY.TXT") &&
        shell_execute("write /NEW.TXT Asas writable FAT32 file") &&
        shell_execute("cat /NEW.TXT") &&
        shell_execute("ls /") &&
        vfs_write_file("/LARGE.BIN", large_file, sizeof(large_file)) &&
        (large_handle = vfs_open("/LARGE.BIN")) != 0 &&
        (large_bytes = vfs_read(large_handle, read_buffer, sizeof(read_buffer))) == sizeof(large_file) &&
        vfs_close(large_handle) &&
        read_buffer[0] == 'A' &&
        read_buffer[511] == 'R' &&
        read_buffer[512] == 'S' &&
        read_buffer[1199] == 'D' &&
        shell_execute("mkdir /WORK") &&
        vfs_is_directory("/WORK") &&
        shell_execute("write /WORK/NOTE.TXT Nested FAT32 file") &&
        shell_execute("cat /WORK/NOTE.TXT") &&
        !shell_execute("rmdir /WORK") &&
        shell_execute("ls /") &&
        shell_execute("rm /WORK/NOTE.TXT") &&
        shell_execute("rmdir /WORK") &&
        !vfs_is_directory("/WORK") &&
        shell_execute("mkdir /OPS") &&
        shell_execute("write /SOURCE.TXT Copy and move verified") &&
        shell_execute("cp /SOURCE.TXT /OPS/COPY.TXT") &&
        shell_execute("mv /OPS/COPY.TXT /OPS/MOVED.TXT") &&
        shell_execute("cat /OPS/MOVED.TXT") &&
        shell_execute("rm /OPS/MOVED.TXT") &&
        shell_execute("rmdir /OPS") &&
        shell_execute("rm /SOURCE.TXT") &&
        shell_execute("rm /LARGE.BIN") &&
        shell_execute("rm /NEW.TXT") &&
        shell_execute("rm /EMPTY.TXT") &&
        vfs_open("/NEW.TXT") == 0 &&
        vfs_open("/EMPTY.TXT") == 0 &&
        shell_execute("ps") &&
        shell_execute("wget example.com") &&
        shell_execute("cat /WGET.TXT") &&
        shell_execute("rm /WGET.TXT") &&
        shell_execute("http-server") &&
        shell_execute("power") &&
        shell_execute("battery") &&
        shell_execute("beep") &&
        shell_execute("touchpad") &&
        shell_execute("wifi") &&
        shell_execute("ping 10.0.2.2") &&
        shell_execute("kill 2") &&
        process_active_count() == 1
    );
}

static void shell_input_character(char character)
{
    char   echo[130];
    UINT32 ei;
    UINT32 ci;

    if (character == '\n') {
        input_line[input_length] = '\0';
        if (input_length != 0) {
            /* Echo the command into the GUI terminal before executing */
            ei = 0;
            echo[ei++] = '>';
            echo[ei++] = ' ';
            for (ci = 0; input_line[ci] && ei < 128; ci++) {
                echo[ei++] = input_line[ci];
            }
            echo[ei] = '\0';
            gui_terminal_write(echo);
            (void)shell_execute(input_line);
        }
        input_length = 0;
        gui_set_input_line("", 0);
        logger_write("SHELL", "ready");
        return;
    }
    if (character == '\b') {
        if (input_length != 0) {
            input_length--;
        }
        gui_set_input_line(input_line, input_length);
        return;
    }
    if (input_length + 1 < sizeof(input_line)) {
        input_line[input_length++] = character;
    }
    gui_set_input_line(input_line, input_length);
}

void shell_poll_input_once(void)
{
    char character;

    (void)xhci_poll_active_keyboard();
    hyperv_keyboard_poll();
    keyboard_poll_controller();
    while (keyboard_read_character(&character)) {
        shell_input_character(character);
    }
}

static void shell_interactive_worker(void)
{
    for (;;) {
        shell_poll_input_once();
        scheduler_yield();
    }
}

int shell_start_interactive(void)
{
    input_length = 0;
    if (!scheduler_create_thread(shell_interactive_worker)) {
        return 0;
    }
    logger_write("INFO", "keyboard shell input pipeline ready");
    return 1;
}
