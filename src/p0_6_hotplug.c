/* P0.6 — Hotplug stub.
 * Listens for DRM uevents on the udev netlink monitor; on a hotplug event
 * re-runs connector enumeration. No master required. Run it, then plug/
 * unplug a monitor (or QEMU device_add/device_del a virtio display). */
#define _GNU_SOURCE
#include "common.h"
#include "phases.h"

#include <errno.h>
#include <libudev.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static void on_sigint(int s) { (void)s; running = 0; }

int p0_6_hotplug_run(int argc, char **argv)
{
	signal(SIGINT, on_sigint);

	int fd = drm_open(argc > 1 ? argv[1] : NULL);
	if (fd < 0)
		return 1;

	struct udev *udev = udev_new();
	if (!udev) {
		LOG_ERR("udev_new failed");
		return 1;
	}
	struct udev_monitor *mon = udev_monitor_new_from_netlink(udev, "udev");
	if (!mon) {
		LOG_ERR("udev_monitor_new_from_netlink failed");
		return 1;
	}
	udev_monitor_filter_add_match_subsystem_devtype(mon, "drm", NULL);
	udev_monitor_enable_receiving(mon);
	int mfd = udev_monitor_get_fd(mon);

	LOG_INFO("initial topology:");
	print_connectors(fd);
	LOG_INFO("watching for DRM hotplug (Ctrl-C to stop)...");

	while (running) {
		struct pollfd pfd = { .fd = mfd, .events = POLLIN };
		if (poll(&pfd, 1, 1000) <= 0)
			continue;
		struct udev_device *dev = udev_monitor_receive_device(mon);
		if (!dev)
			continue;
		const char *action = udev_device_get_action(dev);
		const char *hotplug =
		        udev_device_get_property_value(dev, "HOTPLUG");
		LOG_INFO("uevent: action=%s hotplug=%s", action ? action : "?",
		         hotplug ? hotplug : "0");
		print_connectors(fd);
		udev_device_unref(dev);
	}

	udev_monitor_unref(mon);
	udev_unref(udev);
	close(fd);
	return 0;
}
