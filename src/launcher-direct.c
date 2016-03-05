/*
 * Copyright © 2012 Benjamin Franzke
 * Copyright © 2013 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include "compositor.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/vt.h>
#include <linux/kd.h>
#include <linux/major.h>

#include "launcher-impl.h"

#define DRM_MAJOR 226

#ifndef KDSKBMUTE
#define KDSKBMUTE	0x4B51
#endif

#ifdef HAVE_LIBDRM

#include <xf86drm.h>

static inline int
is_drm_master(int drm_fd)
{
	drm_magic_t magic;

	return drmGetMagic(drm_fd, &magic) == 0 &&
		drmAuthMagic(drm_fd, magic) == 0;
}

#else

static inline int
drmDropMaster(int drm_fd)
{
	return 0;
}

static inline int
drmSetMaster(int drm_fd)
{
	return 0;
}

static inline int
is_drm_master(int drm_fd)
{
	return 0;
}

#endif

struct launcher_direct {
	struct weston_launcher base;
	struct weston_compositor *compositor;
	int drm_fd;
};

static int
launcher_direct_open(struct weston_launcher *launcher_base, const char *path, int flags)
{
	struct launcher_direct *launcher = wl_container_of(launcher_base, launcher, base);
	struct stat s;
	int fd;

    weston_log("Try to open %s\n", path);
	fd = open(path, flags | O_CLOEXEC);
	if (fd == -1)
		return -1;

	if (fstat(fd, &s) == -1) {
		close(fd);
		return -1;
	}

	if (major(s.st_rdev) == DRM_MAJOR) {
		launcher->drm_fd = fd;
		if (!is_drm_master(fd)) {
			weston_log("drm fd not master\n");
			close(fd);
			return -1;
		}
	}

    weston_log("Success open %s\n", path);

	return fd;
}

static void
launcher_direct_close(struct weston_launcher *launcher_base, int fd)
{
	close(fd);
}

static void
launcher_direct_restore(struct weston_launcher *launcher_base)
{
    weston_log("launcher_direct_restore called\n");
}

static int
launcher_direct_activate_vt(struct weston_launcher *launcher_base, int vt)
{
    weston_log("launcher_direct_activate_vt called\n");
    return 0;
}

static int
launcher_direct_connect(struct weston_launcher **out, struct weston_compositor *compositor,
			int tty, const char *seat_id, bool sync_drm)
{
	struct launcher_direct *launcher;

    weston_log("geteuid: %d\n", geteuid);
//	if (geteuid() != 0)
//		return -EINVAL;

	launcher = zalloc(sizeof(*launcher));
	if (launcher == NULL)
		return -ENOMEM;

	launcher->base.iface = &launcher_direct_iface;
	launcher->compositor = compositor;

	* (struct launcher_direct **) out = launcher;
	return 0;
}

static void
launcher_direct_destroy(struct weston_launcher *launcher_base)
{
	struct launcher_direct *launcher = wl_container_of(launcher_base, launcher, base);

	launcher_direct_restore(&launcher->base);

	free(launcher);
}

struct launcher_interface launcher_direct_iface = {
	launcher_direct_connect,
	launcher_direct_destroy,
	launcher_direct_open,
	launcher_direct_close,
	launcher_direct_activate_vt,
	launcher_direct_restore,
};
