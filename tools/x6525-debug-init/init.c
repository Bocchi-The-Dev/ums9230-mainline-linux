// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal PID 1 for the X6525 debugging initramfs.
 *
 * There is no UART on this unit, so the only way to learn whether the mainline
 * kernel reaches userspace is to leave a trace the stock system can read back
 * afterwards. pstore/ramoops (console+record+pmsg regions at 0xfff80000, see
 * the board DTS) survives a warm reset, and the stock 5.4 kernel exposes it at
 * /sys/fs/pstore, so this init writes a marker to /dev/pmsg0 and reboots.
 *
 * Build (CI does this):
 *   aarch64-linux-gnu-gcc -static -Os -o init init.c
 * Pack:
 *   find . | cpio -o -H newc | gzip -9 > initramfs.cpio.gz
 *
 * Boot it with a repacked boot_a.img (repack.py --ramdisk ...).
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/reboot.h>

static void put(const char *path, const char *msg)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		return;
	(void)write(fd, msg, strlen(msg));
	close(fd);
}

int main(void)
{
	mkdir("/dev", 0755);
	mkdir("/proc", 0755);
	mkdir("/sys", 0755);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);

	put("/dev/kmsg", "<0>X6525-DEBUG: init reached, kernel is alive\n");
	put("/dev/pmsg0", "X6525-DEBUG: init reached, kernel is alive\n");

	/* Leave the message on the panel long enough to be seen/photographed. */
	sync();
	sleep(8);

	put("/dev/kmsg", "<0>X6525-DEBUG: rebooting\n");
	sync();
	reboot(RB_AUTOBOOT);
	for (;;)
		pause();
}
