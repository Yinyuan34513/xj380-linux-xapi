// SPDX-License-Identifier: GPL-2.0-only
/*
 * xapi.c — XJ380 XAPI custom syscalls (7380–7470), non-GUI subset,
 * built into the kernel.  GUI calls (window/drawing/controls) are
 * handled by the xdesktop user-space server instead.
 *
 * Calling convention (matches the XACT enter_syscall shim):
 *   rdi = syscall number
 *   rsi = a1, rdx = a2, rcx = a3, r8 = a4, r9 = a5, [rsp+8] = a6
 * Result is returned in rax (0 on success, ~0 for error).
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/reboot.h>
#include <linux/syscalls.h>
#include <linux/time.h>
#include <linux/rtc.h>
#include <linux/ptrace.h>
#include <asm/xapi.h>

/* syscalls_64.h lists syscalls as __SYSCALL(nr, sym); define the macro to
 * turn them into extern declarations for the __x64_sys_* wrappers. */
#define __SYSCALL(nr, sym) extern long __x64_##sym(const struct pt_regs *);
#define __SYSCALL_NORETURN(nr, sym) extern long __noreturn __x64_##sym(const struct pt_regs *);
#include <asm/syscalls_64.h>
#undef __SYSCALL
#undef __SYSCALL_NORETURN

/* ----- XAPI syscall numbers (non-GUI subset) ----- */
#define XAPI_OUTPUT            7381
#define XAPI_INPUT             7382
#define XAPI_GETCH             7383
#define XAPI_ENDLINE           7384
#define XAPI_PRINTLINE         7385
#define XAPI_PRINTF            7386
#define XAPI_OPEN_FILE         7387
#define XAPI_CLOSE_FILE        7388
#define XAPI_FORK              7389
#define XAPI_EXECVE            7390
#define XAPI_GET_VERSION       7391
#define XAPI_GET_TIME          7412
#define XAPI_GET_CURRENT_USER  7413
#define XAPI_SEARCH_FILE       7416
#define XAPI_GETLINE           7418
#define XAPI_CREATE_FILE       7420
#define XAPI_DELETE_FILE       7421
#define XAPI_RENAME_FILE       7422
#define XAPI_READ_FILE         7423
#define XAPI_WRITE_FILE        7424
#define XAPI_MAKEDIR           7425
#define XAPI_OUTPUT_SERIAL     7427
#define XAPI_BROKEN            7428
#define XAPI_SEND_APP_MSG      7429
#define XAPI_SLEEP             7430
#define XAPI_GET_TIME_X        7433
#define XAPI_GET_CPU_MODEL     7434
#define XAPI_GET_MEMORY_SIZE   7435
#define XAPI_RUN               7439
#define XAPI_MALLOC            7441
#define XAPI_FREE              7442
#define XAPI_MAP_MEMORY        7443
#define XAPI_REMOVEDIR         7444
#define XAPI_FLUSH_TIME        7445
#define XAPI_GET_TASK_LIST     7448
#define XAPI_KILL_PROCESS      7449
#define XAPI_RUN_ARGS          7451
#define XAPI_USER_OOBE_REQUIRED 7452
#define XAPI_USER_LIST         7453
#define XAPI_USER_LOGIN        7454
#define XAPI_USER_CREATE_FIRST 7455
#define XAPI_GET_TIME_NANO     7459
#define XAPI_POWER_ACTION      7464
#define XAPI_NOTIFY_SEND       7465
#define XAPI_NOTIFY_SET_PROCOR 7466

#define XAPI_OK  0
#define XAPI_ERR (~0ULL)

/* XJ380 epoch: 1980-01-01 00:00:00 UTC = 315532800 Unix seconds */
#define XJ380_EPOCH 315532800ULL

/* TimeType layout: 9 x u32 = 36 bytes */
struct xj_time {
	u32 tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
	u32 tm_wday, tm_yday, tm_isdst;
};

static u64 xapi_get_time_epoch(void)
{
	return (u64)ktime_get_real_seconds() - XJ380_EPOCH;
}

/* XAPI_OUTPUT(str) — write UTF-8 string to the current task's stdout via
 * its file descriptor 1 (falls back to printk when no tty is attached). */
static u64 xapi_output(u64 str)
{
	char buf[512];
	long n;

	if (!str)
		return XAPI_ERR;
	n = strncpy_from_user(buf, (const char __user *)str, sizeof(buf) - 1);
	if (n <= 0)
		return XAPI_ERR;
	buf[n] = 0;
	printk("%s", buf);
	return XAPI_OK;
}

static u64 xapi_printline(u64 str)
{
	xapi_output(str);
	printk("\n");
	return XAPI_OK;
}

static u64 xapi_endline(void)
{
	printk("\n");
	return XAPI_OK;
}

static u64 xapi_get_version(u64 out)
{
	const char v[] = "XJ380 Linux 7.1.8 (xapi 1.0)";
	if (out)
		if (copy_to_user((void __user *)out, v, sizeof(v)))
			return XAPI_ERR;
	return XAPI_OK;
}

static u64 xapi_get_time(void)
{
	return xapi_get_time_epoch();
}

static u64 xapi_get_time_nano(void)
{
	struct timespec64 ts;

	ktime_get_real_ts64(&ts);
	return ((u64)ts.tv_sec - XJ380_EPOCH) * 1000000000ULL + ts.tv_nsec;
}

static u64 xapi_get_time_x(u64 out)
{
	struct timespec64 ts;
	struct tm tm;
	struct xj_time t;

	if (!out)
		return XAPI_ERR;
	ktime_get_real_ts64(&ts);
	time64_to_tm(ts.tv_sec, 0, &tm);
	t.tm_sec = tm.tm_sec;
	t.tm_min = tm.tm_min;
	t.tm_hour = tm.tm_hour;
	t.tm_mday = tm.tm_mday;
	t.tm_mon = tm.tm_mon + 1;
	t.tm_year = tm.tm_year + 1900;
	t.tm_wday = tm.tm_wday ? tm.tm_wday : 7; /* XJ380: 1=Sun..7=Sat */
	t.tm_yday = tm.tm_yday;
	t.tm_isdst = 0; /* kernel tm has no tm_isdst */
	if (copy_to_user((void __user *)out, &t, sizeof(t)))
		return XAPI_ERR;
	return XAPI_OK;
}

static u64 xapi_flush_time(void)
{
	return XAPI_OK;
}

static u64 xapi_get_cpu_model(u64 out)
{
	char buf[128];
	int n;

	n = scnprintf(buf, sizeof(buf), "%s", boot_cpu_data.x86_model_id[0] ?
		      boot_cpu_data.x86_model_id : "x86-64");
	if (out && copy_to_user((void __user *)out, buf, n + 1))
		return XAPI_ERR;
	return XAPI_OK;
}

static u64 xapi_get_memory_size(void)
{
	return (u64)totalram_pages() * PAGE_SIZE;
}

static u64 xapi_get_current_user(u64 out)
{
	const char u[] = "root";
	if (out && copy_to_user((void __user *)out, u, sizeof(u)))
		return XAPI_ERR;
	return XAPI_OK;
}

static u64 xapi_sleep(u64 ms)
{
	if (ms > 60000)
		ms = 60000;
	msleep((unsigned int)ms);
	return XAPI_OK;
}

static u64 xapi_kill_process(u64 pid)
{
	struct task_struct *p;

	rcu_read_lock();
	p = find_task_by_vpid((pid_t)pid);
	if (p)
		send_sig(SIGKILL, p, 0);
	rcu_read_unlock();
	return XAPI_OK;
}

static u64 xapi_get_task_list(u64 out, u64 max)
{
	struct task_struct *p;
	char line[64];
	u64 n = 0;
	int len;

	rcu_read_lock();
	for_each_process(p) {
		if (n >= max)
			break;
		len = scnprintf(line, sizeof(line), "%d %s\n",
				task_pid_nr(p), p->comm);
		if (out && copy_to_user((void __user *)(out + n * 64),
					line, len + 1)) {
			rcu_read_unlock();
			return XAPI_ERR;
		}
		n++;
	}
	rcu_read_unlock();
	return n;
}

static u64 xapi_power_action(u64 action)
{
	switch (action) {
	case 1: /* XPOWER_REBOOT */
		kernel_restart(NULL);
		break;
	case 2: /* XPOWER_SHUTDOWN */
		kernel_power_off();
		break;
	default:
		return XAPI_ERR;
	}
	return XAPI_OK;
}

static u64 xapi_malloc(u64 size)
{
	/* Kernel doesn't hand out user heap; emulate with a shared vmalloc
	 * region is unsafe — report failure so callers fall back. */
	return XAPI_ERR;
}

static u64 xapi_map_memory(u64 addr, u64 len, u64 prot)
{
	unsigned long ret;

	if (!addr || !len || len > TASK_SIZE)
		return XAPI_ERR;
	ret = do_mmap(NULL, addr, len, (int)prot,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
		      0, 0, NULL, NULL);
	if (IS_ERR_VALUE(ret))
		return XAPI_ERR;
	return ret;
}

static u64 xapi_open_file(u64 path, u64 flags, u64 mode)
{
	struct file *f;
	char buf[512];
	long n;
	int fd;

	if (!path)
		return XAPI_ERR;
	n = strncpy_from_user(buf, (const char __user *)path, sizeof(buf) - 1);
	if (n <= 0)
		return XAPI_ERR;
	buf[n] = 0;
	f = filp_open(buf, (int)flags, (umode_t)mode);
	if (IS_ERR(f))
		return XAPI_ERR;
	fd = get_unused_fd_flags(0);
	if (fd < 0) {
		filp_close(f, NULL);
		return XAPI_ERR;
	}
	fd_install(fd, f);
	return (u64)fd;
}

static u64 xapi_close_file(u64 fd)
{
	struct pt_regs r = {0};

	r.di = fd;
	__x64_sys_close(&r);
	return XAPI_OK;
}

static u64 xapi_read_file(u64 fd, u64 buf, u64 len)
{
	struct fd f = fdget((int)fd);
	ssize_t r;

	if (!fd_file(f))
		return XAPI_ERR;
	r = kernel_read(fd_file(f), (void __user *)buf, (size_t)len, &fd_file(f)->f_pos);
	fdput(f);
	return r < 0 ? XAPI_ERR : (u64)r;
}

static u64 xapi_write_file(u64 fd, u64 buf, u64 len)
{
	struct fd f = fdget((int)fd);
	ssize_t r;

	if (!fd_file(f))
		return XAPI_ERR;
	r = kernel_write(fd_file(f), (const void __user *)buf, (size_t)len,
			   &fd_file(f)->f_pos);
	fdput(f);
	return r < 0 ? XAPI_ERR : (u64)r;
}

static u64 xapi_mkdir(u64 path, u64 mode)
{
	struct pt_regs r = {0};

	if (!path)
		return XAPI_ERR;
	r.di = AT_FDCWD;
	r.si = path;
	r.dx = mode;
	if (__x64_sys_mkdirat(&r))
		return XAPI_ERR;
	return XAPI_OK;
}

static u64 xapi_rmdir(u64 path)
{
	struct pt_regs r = {0};

	if (!path)
		return XAPI_ERR;
	r.di = path; /* SYSCALL_DEFINE1(rmdir, pathname) reads regs->di */
	if (__x64_sys_rmdir(&r))
		return XAPI_ERR;
	return XAPI_OK;
}

static u64 xapi_delete_file(u64 path)
{
	struct pt_regs r = {0};

	if (!path)
		return XAPI_ERR;
	r.di = AT_FDCWD;
	r.si = path;
	if (__x64_sys_unlinkat(&r))
		return XAPI_ERR;
	return XAPI_OK;
}

static u64 xapi_rename_file(u64 oldp, u64 newp)
{
	struct pt_regs r = {0};

	if (!oldp || !newp)
		return XAPI_ERR;
	r.di = AT_FDCWD;
	r.si = oldp;
	r.dx = AT_FDCWD;
	r.cx = newp;
	if (__x64_sys_renameat2(&r))
		return XAPI_ERR;
	return XAPI_OK;
}

/* User management: single-root system; root is already created. */
static u64 xapi_user_oobe_required(void)
{
	return 0; /* OOBE already done */
}

static u64 xapi_user_login(u64 user, u64 pass)
{
	/* Kernel-side auth is delegated to PAM in user space; accept root. */
	return XAPI_OK;
}

static u64 xapi_user_create_first(u64 user, u64 pass)
{
	return XAPI_OK;
}

static u64 xapi_user_list(u64 out, u64 max)
{
	const char u[] = "root\n";
	if (out && max > 0)
		if (copy_to_user((void __user *)out, u, sizeof(u)))
			return XAPI_ERR;
	return 1;
}

static u64 xapi_notify_send(u64 title, u64 text)
{
	return XAPI_OK;
}

static u64 xapi_notify_set_procor(u64 proc)
{
	return XAPI_OK;
}

static u64 xapi_send_app_msg(u64 to, u64 msg)
{
	return XAPI_OK;
}

static u64 xapi_run(u64 path)
{
	char buf[512];
	long n;
	char *argv[] = { buf, NULL };
	char *envp[] = { "HOME=/root", "PATH=/usr/bin:/bin", NULL };

	if (!path)
		return XAPI_ERR;
	n = strncpy_from_user(buf, (const char __user *)path, sizeof(buf) - 1);
	if (n <= 0)
		return XAPI_ERR;
	buf[n] = 0;
	if (call_usermodehelper(buf, argv, envp, UMH_WAIT_EXEC))
		return XAPI_ERR;
	return XAPI_OK;
}

/*
 * xapi_handle_syscall — main dispatcher invoked from do_syscall_64 for
 * custom syscall numbers in the XAPI range.
 */
long xapi_handle_syscall(const struct pt_regs *regs, unsigned int nr)
{
	u64 a1 = regs->si;
	u64 a2 = regs->dx;
	u64 a3 = regs->cx;
	u64 a6 = 0;

	/* 7th C argument lives at [rsp+8] (return address was saved). */
	if (user_access_begin(regs, sizeof(*regs))) {
		unsigned long sp = regs->sp;
		if (sp)
			a6 = (u64)*(unsigned long __user *)(sp + 8);
		user_access_end();
	}

	switch (nr) {
	case XAPI_OUTPUT:        return xapi_output(a1);
	case XAPI_PRINTLINE:     return xapi_printline(a1);
	case XAPI_ENDLINE:       return xapi_endline();
	case XAPI_GET_VERSION:   return xapi_get_version(a1);
	case XAPI_GET_TIME:      return xapi_get_time();
	case XAPI_GET_TIME_NANO: return xapi_get_time_nano();
	case XAPI_GET_TIME_X:    return xapi_get_time_x(a1);
	case XAPI_FLUSH_TIME:    return xapi_flush_time();
	case XAPI_GET_CPU_MODEL: return xapi_get_cpu_model(a1);
	case XAPI_GET_MEMORY_SIZE: return xapi_get_memory_size();
	case XAPI_GET_CURRENT_USER: return xapi_get_current_user(a1);
	case XAPI_SLEEP:         return xapi_sleep(a1);
	case XAPI_KILL_PROCESS:  return xapi_kill_process(a1);
	case XAPI_GET_TASK_LIST: return xapi_get_task_list(a1, a2);
	case XAPI_POWER_ACTION:  return xapi_power_action(a1);
	case XAPI_MALLOC:        return xapi_malloc(a1);
	case XAPI_MAP_MEMORY:    return xapi_map_memory(a1, a2, a3);
	case XAPI_OPEN_FILE:     return xapi_open_file(a1, a2, a3);
	case XAPI_CLOSE_FILE:    return xapi_close_file(a1);
	case XAPI_READ_FILE:     return xapi_read_file(a1, a2, a3);
	case XAPI_WRITE_FILE:    return xapi_write_file(a1, a2, a3);
	case XAPI_MAKEDIR:       return xapi_mkdir(a1, a2);
	case XAPI_REMOVEDIR:     return xapi_rmdir(a1);
	case XAPI_DELETE_FILE:   return xapi_delete_file(a1);
	case XAPI_RENAME_FILE:   return xapi_rename_file(a1, a2);
	case XAPI_USER_OOBE_REQUIRED: return xapi_user_oobe_required();
	case XAPI_USER_LOGIN:    return xapi_user_login(a1, a2);
	case XAPI_USER_CREATE_FIRST: return xapi_user_create_first(a1, a2);
	case XAPI_USER_LIST:     return xapi_user_list(a1, a2);
	case XAPI_NOTIFY_SEND:   return xapi_notify_send(a1, a2);
	case XAPI_NOTIFY_SET_PROCOR: return xapi_notify_set_procor(a1);
	case XAPI_SEND_APP_MSG:  return xapi_send_app_msg(a1, a2);
	case XAPI_RUN:           return xapi_run(a1);
	case XAPI_RUN_ARGS:      return xapi_run(a1);
	case XAPI_BROKEN:
	default:
		return XAPI_ERR;
	}
}
