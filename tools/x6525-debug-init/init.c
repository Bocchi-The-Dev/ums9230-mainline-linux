// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal PID 1 for the X6525 debugging initramfs.
 *
 * There is no UART on this unit, so the only way to learn whether the mainline
 * kernel reaches userspace is to leave a trace the stock system can read back
 * afterwards. pstore/ramoops (console+record+pmsg regions at 0xfff80000, see
 * the board DTS) survives a warm reset and the stock 5.4 kernel exposes it at
 * /sys/fs/pstore, so this init writes a marker to /dev/pmsg0 and reboots.
 *
 * The program is freestanding: raw aarch64 syscalls, no libc and no headers,
 * so a bare aarch64-linux-gnu-gcc is enough to build it.
 *
 * Build (CI does this):
 *   aarch64-linux-gnu-gcc -nostdlib -ffreestanding -fno-builtin -no-pie \
 *       -static -Os -o init init.c
 * Pack:
 *   find . | cpio -o -H newc | gzip -9 > initramfs.cpio.gz
 *
 * Boot it with a repacked boot_a.img (repack.py --ramdisk ...).
 */
#define SYS_mkdirat	34
#define SYS_mount	40
#define SYS_openat	56
#define SYS_close	57
#define SYS_write	64
#define SYS_sync	81
#define SYS_nanosleep	101
#define SYS_reboot	142

#define AT_FDCWD	(-100)
#define O_WRONLY	1
#define O_CLOEXEC	02000000

#define REBOOT_MAGIC1	0xfee1dead
#define REBOOT_MAGIC2	672274793	/* 0x28121969 */
#define RB_AUTOBOOT	0x01234567

struct timespec {
	long tv_sec;
	long tv_nsec;
};

static long sys(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;

	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			 : "memory");
	return x0;
}

static long str_len(const char *s)
{
	long n = 0;

	while (s[n])
		n++;
	return n;
}

static void put(const char *path, const char *msg)
{
	long fd;

	fd = sys(SYS_openat, AT_FDCWD, (long)path, O_WRONLY | O_CLOEXEC, 0, 0, 0);
	if (fd < 0)
		return;
	(void)sys(SYS_write, fd, (long)msg, str_len(msg), 0, 0, 0);
	(void)sys(SYS_close, fd, 0, 0, 0, 0, 0);
}

static void sleep_sec(long sec)
{
	struct timespec ts;

	ts.tv_sec = sec;
	ts.tv_nsec = 0;
	while (sys(SYS_nanosleep, (long)&ts, 0, 0, 0, 0, 0) < 0)
		;
}

void _start(void)
{
	(void)sys(SYS_mkdirat, AT_FDCWD, (long)"/dev", 0755, 0, 0, 0);
	(void)sys(SYS_mkdirat, AT_FDCWD, (long)"/proc", 0755, 0, 0, 0);
	(void)sys(SYS_mkdirat, AT_FDCWD, (long)"/sys", 0755, 0, 0, 0);
	(void)sys(SYS_mount, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs",
		  0, 0, 0);
	(void)sys(SYS_mount, (long)"proc", (long)"/proc", (long)"proc", 0, 0, 0);
	(void)sys(SYS_mount, (long)"sysfs", (long)"/sys", (long)"sysfs",
		  0, 0, 0);

	put("/dev/kmsg", "<0>X6525-DEBUG: init reached, kernel is alive\n");
	put("/dev/pmsg0", "X6525-DEBUG: init reached, kernel is alive\n");

	/* Leave the message on the panel long enough to be photographed. */
	(void)sys(SYS_sync, 0, 0, 0, 0, 0, 0);
	sleep_sec(8);

	put("/dev/kmsg", "<0>X6525-DEBUG: rebooting\n");
	(void)sys(SYS_sync, 0, 0, 0, 0, 0, 0);
	(void)sys(SYS_reboot, REBOOT_MAGIC1, REBOOT_MAGIC2, RB_AUTOBOOT, 0, 0, 0);
	for (;;)
		;
}
