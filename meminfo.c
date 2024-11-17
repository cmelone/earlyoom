// SPDX-License-Identifier: MIT

/* Parse /proc/meminfo
 * Returned values are in kiB */

#include <errno.h>
#include <signal.h>
#include <stddef.h> // for size_t
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "globals.h"
#include "meminfo.h"
#include "msg.h"
#include "proc_pid.h"

/* Parse the contents of /proc/meminfo (in buf), return value of "name"
 * (example: "MemTotal:")
 * Returns -errno if the entry cannot be found. */
static long long get_entry(const char* name, const char* buf)
{
    char* hit = strstr(buf, name);
    if (hit == NULL) {
        return -ENODATA;
    }

    errno = 0;
    long long val = strtoll(hit + strlen(name), NULL, 10);
    if (errno != 0) {
        int strtoll_errno = errno;
        warn("%s: strtol() failed: %s", __func__, strerror(errno));
        return -strtoll_errno;
    }
    return val;
}

/* Like get_entry(), but exit if the value cannot be found */
static long long get_entry_fatal(const char* name, const char* buf)
{
    long long val = get_entry(name, buf);
    if (val < 0) {
        warn("%s: fatal error, dumping buffer for later diagnosis:\n%s", __func__, buf);
        fatal(104, "could not find entry '%s' in /proc/meminfo: %s\n", name, strerror((int)-val));
    }
    return val;
}

/* If the kernel does not provide MemAvailable (introduced in Linux 3.14),
 * approximate it using other data we can get */
static long long available_guesstimate(const char* buf)
{
    long long Cached = get_entry_fatal("Cached:", buf);
    long long MemFree = get_entry_fatal("MemFree:", buf);
    long long Buffers = get_entry_fatal("Buffers:", buf);
    long long Shmem = get_entry_fatal("Shmem:", buf);

    return MemFree + Cached + Buffers - Shmem;
}

/* get total_rss and total_inactive_file from /sys/fs/cgroup/memory/memory.stat */
static memory_stat_t parse_mem_stat() {
    FILE *file = fopen("/sys/fs/cgroup/memory/memory.stat", "r");
    if (!file) {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    memory_stat_t stats = {0};
    char line[256];

    while (fgets(line, sizeof(line), file)) {
        // 9 is the length of total_rss plus a space
        // get the value after the space
        // convert to KiB from bytes
        if (strncmp(line, "total_rss", 9) == 0) {
            sscanf(line, "total_rss %lld", &stats.total_rss);
        } else if (strncmp(line, "total_inactive_file", 19) == 0) {
            sscanf(line, "total_inactive_file %lld", &stats.total_inactive_file);
        }
    }

    // convert to KiB from bytes
    stats.total_rss /= 1024;
    stats.total_inactive_file /= 1024;

    fclose(file);
    return stats;
}

/* given a path, return the value in the file
 * assumes the file contains a single integer (butes)
*/
static long long parse_cgroup_mem_info(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) {
        perror("fopen");
        return -1;
    }

    long long number;
    if (fscanf(file, "%lld", &number) != 1) {
        perror("fscanf");
        fclose(file);
        return -1;
    }

    fclose(file);
    return number / 1024; // convert bytes to KiB
}


/* returns the memory usage of the cgroup in KiB
 * because it will be used by the OOM killer, we return
 * the max of (working set, total_rss)
 * see https://stackoverflow.com/a/63081415
*/
static long long get_usage() {
    long long usage = parse_cgroup_mem_info("/sys/fs/cgroup/memory/memory.usage_in_bytes");
    memory_stat_t stats = parse_mem_stat();
    long long working_set = usage - stats.total_inactive_file;

    // debug print
    debug("usage: %lld\n", usage);
    debug("total_rss: %lld\n", stats.total_rss);
    debug("total_inactive_file: %lld\n", stats.total_inactive_file);
    debug("working_set: %lld\n", working_set);

    // return bigger of working set and stats.total_rss
    if (working_set > stats.total_rss) {
        return working_set;
    } else {
        return stats.total_rss;
    }
}


/* Parse /proc/meminfo.
 * This function either returns valid data or kills the process
 * with a fatal error.
 */
meminfo_t parse_meminfo()
{
    // Note that we do not need to close static FDs that we ensure to
    // `fopen()` maximally once.
    static FILE* fd;
    static int guesstimate_warned = 0;
    // On Linux 5.3, "wc -c /proc/meminfo" counts 1391 bytes.
    // 8192 should be enough for the foreseeable future.
    char buf[8192] = { 0 };
    meminfo_t m = { 0 };

    if (fd == NULL) {
        char buf[PATH_LEN] = { 0 };
        snprintf(buf, sizeof(buf), "%s/%s", procdir_path, "meminfo");
        fd = fopen(buf, "r");
    }
    if (fd == NULL) {
        fatal(102, "could not open /proc/meminfo: %s\n", strerror(errno));
    }
    rewind(fd);

    size_t len = fread(buf, 1, sizeof(buf) - 1, fd);
    if (ferror(fd)) {
        fatal(103, "could not read /proc/meminfo: %s\n", strerror(errno));
    }
    if (len == 0) {
        fatal(103, "could not read /proc/meminfo: 0 bytes returned\n");
    }

    m.MemTotalKiB = parse_cgroup_mem_info("/sys/fs/cgroup/memory/memory.limit_in_bytes");
    if (m.MemTotalKiB < 0) {
        fatal(104, "could not read cgroup memory limit\n");
    }
    m.SwapTotalKiB = get_entry_fatal("SwapTotal:", buf);
    m.AnonPagesKiB = get_entry_fatal("AnonPages:", buf);
    m.SwapFreeKiB = get_entry_fatal("SwapFree:", buf);

    // debug print
    debug("swap total: %lld\n", m.SwapTotalKiB);
    debug("anon pages: %lld\n", m.AnonPagesKiB);
    debug("swap free: %lld\n", m.SwapFreeKiB);

    long long usage = get_usage();
    if (usage < 0) {
        fatal(104, "could not read used memory\n");
    }

    m.MemAvailableKiB = m.MemTotalKiB - usage;

    // Calculated values
    m.UserMemTotalKiB = m.MemAvailableKiB + m.AnonPagesKiB;

    // Calculate percentages
    m.MemAvailablePercent = (double)m.MemAvailableKiB * 100 / (double)m.UserMemTotalKiB;
    if (m.SwapTotalKiB > 0) {
        m.SwapFreePercent = (double)m.SwapFreeKiB * 100 / (double)m.SwapTotalKiB;
    } else {
        m.SwapFreePercent = 0;
    }

    return m;
}

bool is_alive(int pid)
{
    // whole process group (-g flag)?
    if (pid < 0) {
        // signal 0 does nothing, but we do get an error when the process
        // group does not exist.
        int res = kill(pid, 0);
        if (res == 0) {
            return true;
        }
        return false;
    }

    pid_stat_t stat;
    if (!parse_proc_pid_stat(&stat, pid)) {
        return false;
    }

    debug("%s: state=%c num_threads=%ld\n", __func__, stat.state, stat.num_threads);
    if (stat.state == 'Z' && stat.num_threads == 1) {
        // A zombie process without subthreads does not use any memory. Consider it dead.
        return false;
    }
    return true;
}

/* Read /proc/[pid]/[name] and convert to integer.
 * As the value may legitimately be < 0 (think oom_score_adj),
 * it is stored in the `out` pointer, and the return value is either
 * 0 (success) or -errno (failure).
 */
static int read_proc_file_integer(const int pid, const char* name, int* out)
{
    char path[PATH_LEN] = { 0 };
    snprintf(path, sizeof(path), "%s/%d/%s", procdir_path, pid, name);
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        return -errno;
    }
    int matches = fscanf(f, "%d", out);
    fclose(f);
    if (matches != 1) {
        return -ENODATA;
    }
    return 0;
}

/* Read /proc/[pid]/oom_score.
 * Returns the value (>= 0) or -errno on error.
 */
int get_oom_score(const int pid)
{
    int out = 0;
    int res = read_proc_file_integer(pid, "oom_score", &out);
    if (res < 0) {
        return res;
    }
    return out;
}

/* Read /proc/[pid]/oom_score_adj.
 * As the value may legitimately be negative, the return value is
 * only used for error indication, and the value is stored in
 * the `out` pointer.
 * Returns 0 on success and -errno on error.
 */
int get_oom_score_adj(const int pid, int* out)
{
    return read_proc_file_integer(pid, "oom_score_adj", out);
}

/* Read /proc/[pid]/comm (process name truncated to 16 bytes).
 * Returns 0 on success and -errno on error.
 */
int get_comm(int pid, char* out, size_t outlen)
{
    char path[PATH_LEN] = { 0 };
    snprintf(path, sizeof(path), "%s/%d/comm", procdir_path, pid);
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        return -errno;
    }
    size_t n = fread(out, 1, outlen - 1, f);
    if (ferror(f)) {
        int fread_errno = errno;
        perror("get_comm: fread() failed");
        fclose(f);
        return -fread_errno;
    }
    fclose(f);
    // Process name may be empty, but we should get at least a newline
    // Example for empty process name: perl -MPOSIX -e '$0=""; pause'
    if (n < 1) {
        return -ENODATA;
    }
    // Strip trailing space
    out[n - 1] = 0;
    fix_truncated_utf8(out);
    return 0;
}

/* Read /proc/[pid]/cmdline (process command line truncated to 256 bytes).
 * The null bytes are replaced by space.
 * Returns 0 on success and -errno on error.
 */
int get_cmdline(int pid, char* out, size_t outlen)
{
    char path[PATH_LEN] = { 0 };
    snprintf(path, sizeof(path), "%s/%d/cmdline", procdir_path, pid);
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        return -errno;
    }
    size_t n = fread(out, 1, outlen - 1, f);
    if (ferror(f)) {
        int fread_errno = errno;
        perror("get_cmdline: fread() failed");
        fclose(f);
        return -fread_errno;
    }
    fclose(f);
    /* replace null character with space */
    for (size_t i = 0; i < n; i++) {
        if (out[i] == '\0') {
            out[i] = ' ';
        }
    }
    // Strip trailing space
    out[n - 1] = 0;
    fix_truncated_utf8(out);
    return 0;
}

// Get the effective uid (EUID) of `pid`.
// Returns the uid (>= 0) or -errno on error.
int get_uid(int pid)
{
    char path[PATH_LEN] = { 0 };
    snprintf(path, sizeof(path), "%s/%d", procdir_path, pid);
    struct stat st = { 0 };
    int res = stat(path, &st);
    if (res < 0) {
        return -errno;
    }
    return (int)st.st_uid;
}

/* Print a status line like
 *   mem avail: 5259 MiB (67 %), swap free: 0 MiB (0 %)"
 * as an informational message to stdout (default), or
 * as a warning to stderr.
 */
void print_mem_stats(int __attribute__((format(printf, 1, 2))) (*out_func)(const char* fmt, ...), const meminfo_t m)
{
    out_func("mem avail: %5lld of %5lld MiB (" PRIPCT "), swap free: %4lld of %4lld MiB (" PRIPCT ")\n",
        m.MemAvailableKiB / 1024,
        m.MemTotalKiB / 1024,
        m.MemAvailablePercent,
        m.SwapFreeKiB / 1024,
        m.SwapTotalKiB / 1024,
        m.SwapFreePercent);
}
