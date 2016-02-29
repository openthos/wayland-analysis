/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
 * Copyright © 2012 Raspberry Pi Foundation
 * Copyright © 2013 Philip Withnall
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/input.h>

#include <libudev.h>

#include "shared/helpers.h"
#include "compositor.h"
#include "launcher-util.h"
#include "pixman-renderer.h"
#include "libinput-seat.h"
#include "gl-renderer.h"
#include "presentation_timing-server-protocol.h"

struct image_backend {
	struct weston_backend base;
	struct weston_compositor *compositor;
	uint32_t prev_state;

	struct udev *udev;
	struct udev_input input;
	int use_pixman;
	struct wl_listener session_listener;
};

struct image_screeninfo {
	unsigned int x_resolution; /* pixels, visible area */
	unsigned int y_resolution; /* pixels, visible area */
	unsigned int width_mm; /* visible screen width in mm */
	unsigned int height_mm; /* visible screen height in mm */
	unsigned int bits_per_pixel;

	size_t buffer_length; /* length of frame buffer memory in bytes */
	size_t line_length; /* length of a line in bytes */
	char id[16]; /* screen identifier */

	pixman_format_code_t pixel_format; /* frame buffer pixel format */
	unsigned int refresh_rate; /* Hertz */
};

struct image_output {
	struct image_backend *backend;
	struct weston_output base;

	struct weston_mode mode;
	struct wl_event_source *finish_frame_timer;

	/* Frame buffer details. */
	const char *device; /* ownership shared with image_parameters */
	struct image_screeninfo fb_info;
	void *fb; /* length is fb_info.buffer_length */

	/* pixman details. */
	pixman_image_t *hw_surface;
	uint8_t depth;
};

struct image_parameters {
	int tty;
	char *device;
	int use_gl;
};

struct gl_renderer_interface *gl_renderer;

static const char default_seat[] = "seat0";

static inline struct image_output *
to_image_output(struct weston_output *base)
{
	return container_of(base, struct image_output, base);
}

static inline struct image_backend *
to_image_backend(struct weston_compositor *base)
{
	return container_of(base->backend, struct image_backend, base);
}

static void
image_output_start_repaint_loop(struct weston_output *output)
{
	struct timespec ts;

	weston_compositor_read_presentation_clock(output->compositor, &ts);
	weston_output_finish_frame(output, &ts, PRESENTATION_FEEDBACK_INVALID);
}

static void
image_output_repaint_pixman(struct weston_output *base, pixman_region32_t *damage)
{
	struct image_output *output = to_image_output(base);
	struct weston_compositor *ec = output->base.compositor;

	/* Repaint the damaged region onto the back buffer. */
	pixman_renderer_output_set_buffer(base, output->hw_surface);
	ec->renderer->repaint_output(base, damage);

	/* Update the damage region. */
	pixman_region32_subtract(&ec->primary_plane.damage,
	                         &ec->primary_plane.damage, damage);

	/* Schedule the end of the frame. We do not sync this to the frame
	 * buffer clock because users who want that should be using the DRM
	 * compositor. FBIO_WAITFORVSYNC blocks and FB_ACTIVATE_VBL requires
	 * panning, which is broken in most kernel drivers.
	 *
	 * Finish the frame synchronised to the specified refresh rate. The
	 * refresh rate is given in mHz and the interval in ms. */
	wl_event_source_timer_update(output->finish_frame_timer,
	                             1000000 / output->mode.refresh);
}

static int
image_output_repaint(struct weston_output *base, pixman_region32_t *damage)
{
	struct image_output *output = to_image_output(base);
	struct image_backend *fbb = output->backend;
	struct weston_compositor *ec = fbb->compositor;

	if (fbb->use_pixman) {
		image_output_repaint_pixman(base,damage);
	} else {
		ec->renderer->repaint_output(base, damage);
		/* Update the damage region. */
		pixman_region32_subtract(&ec->primary_plane.damage,
	                         &ec->primary_plane.damage, damage);

		wl_event_source_timer_update(output->finish_frame_timer,
	                             1000000 / output->mode.refresh);
	}

	return 0;
}

static int
finish_frame_handler(void *data)
{
	struct image_output *output = data;
	struct timespec ts;

	weston_compositor_read_presentation_clock(output->base.compositor, &ts);
	weston_output_finish_frame(&output->base, &ts, 0);

	return 1;
}

static int
image_query_screen_info(struct image_output *output, int fd,
                        struct image_screeninfo *info)
{
	/* Store the pertinent data. */
	info->x_resolution = 800;
	info->y_resolution = 600;
	info->width_mm = 800;
	info->height_mm = 600;
	info->bits_per_pixel = 24;

	info->buffer_length = 5763072;
	info->line_length = info->width_mm * (info->bits_per_pixel / 8);
        strcpy(info->id, "imagescreen");

	info->pixel_format = PIXMAN_r8g8b8;
	info->refresh_rate = 60000;

	return 1;
}

//static int
//image_set_screen_info(struct image_output *output, int fd,
//                      struct image_screeninfo *info)
//{
//	struct fb_var_screeninfo varinfo;
//
//	/* Grab the current screen information. */
//	if (ioctl(fd, FBIOGET_VSCREENINFO, &varinfo) < 0) {
//		return -1;
//	}
//
//	/* Update the information. */
//	varinfo.xres = info->x_resolution;
//	varinfo.yres = info->y_resolution;
//	varinfo.width = info->width_mm;
//	varinfo.height = info->height_mm;
//	varinfo.bits_per_pixel = info->bits_per_pixel;
//
//	/* Try to set up an ARGB (x8r8g8b8) pixel format. */
//	varinfo.grayscale = 0;
//	varinfo.transp.offset = 24;
//	varinfo.transp.length = 0;
//	varinfo.transp.msb_right = 0;
//	varinfo.red.offset = 16;
//	varinfo.red.length = 8;
//	varinfo.red.msb_right = 0;
//	varinfo.green.offset = 8;
//	varinfo.green.length = 8;
//	varinfo.green.msb_right = 0;
//	varinfo.blue.offset = 0;
//	varinfo.blue.length = 8;
//	varinfo.blue.msb_right = 0;
//
//	/* Set the device's screen information. */
//	if (ioctl(fd, FBIOPUT_VSCREENINFO, &varinfo) < 0) {
//		return -1;
//	}
//
//	return 1;
//}

static void image_frame_buffer_destroy(struct image_output *output);

/* Returns an FD for the frame buffer device. */
static int
image_frame_buffer_open(struct image_output *output, const char *fb_dev,
                        struct image_screeninfo *screen_info)
{
	int fd = -1;

	weston_log("Opening image frame buffer.\n");

	/* Open the frame buffer device. */
	fd = open(fb_dev, O_RDWR | O_CLOEXEC | O_CREAT);
	if (fd < 0) {
		weston_log("Failed to open frame buffer device ‘%s’: %s\n",
		           fb_dev, strerror(errno));
		return -1;
	}

	/* Grab the screen info. */
	if (image_query_screen_info(output, fd, screen_info) < 0) {
		weston_log("Failed to get frame buffer info: %s\n",
		           strerror(errno));

		close(fd);
		return -1;
	}

	return fd;
}

/* Closes the FD on success or failure. */
static int
image_frame_buffer_map(struct image_output *output, int fd)
{
	int retval = -1;

	weston_log("Mapping image frame buffer.\n");

	/* Map the frame buffer. Write-only mode, since we don't want to read
	 * anything back (because it's slow). */
	output->fb = mmap(NULL, output->fb_info.buffer_length,
	                  PROT_WRITE, MAP_SHARED, fd, 0);
	if (output->fb == MAP_FAILED) {
		weston_log("Failed to mmap frame buffer: %s\n",
		           strerror(errno));
		goto out_close;
	}

	/* Create a pixman image to wrap the memory mapped frame buffer. */
	output->hw_surface =
		pixman_image_create_bits(output->fb_info.pixel_format,
		                         output->fb_info.x_resolution,
		                         output->fb_info.y_resolution,
		                         output->fb,
		                         output->fb_info.line_length);
	if (output->hw_surface == NULL) {
		weston_log("Failed to create surface for frame buffer.\n");
		goto out_unmap;
	}

	/* Success! */
	retval = 0;

out_unmap:
	if (retval != 0 && output->fb != NULL)
		image_frame_buffer_destroy(output);

out_close:
	if (fd >= 0)
		close(fd);

	return retval;
}

static void
image_frame_buffer_destroy(struct image_output *output)
{
	weston_log("Destroying image frame buffer.\n");

	if (munmap(output->fb, output->fb_info.buffer_length) < 0)
		weston_log("Failed to munmap frame buffer: %s\n",
		           strerror(errno));

	output->fb = NULL;
}

static void image_output_destroy(struct weston_output *base);
static void image_output_disable(struct weston_output *base);

static int
image_output_create(struct image_backend *backend,
                    const char *device)
{
	struct image_output *output;
	struct weston_config_section *section;
	int fb_fd;
	struct wl_event_loop *loop;
	uint32_t config_transform;
	char *s;

	weston_log("Creating image output.\n");

	output = zalloc(sizeof *output);
	if (output == NULL)
		return -1;

	output->backend = backend;
	output->device = device;

	/* Create the frame buffer. */
	fb_fd = image_frame_buffer_open(output, device, &output->fb_info);
	if (fb_fd < 0) {
		weston_log("Creating frame buffer failed.\n");
		goto out_free;
	}
	if (backend->use_pixman) {
		if (image_frame_buffer_map(output, fb_fd) < 0) {
			weston_log("Mapping frame buffer failed.\n");
			goto out_free;
		}
	} else {
		close(fb_fd);
	}

	output->base.start_repaint_loop = image_output_start_repaint_loop;
	output->base.repaint = image_output_repaint;
	output->base.destroy = image_output_destroy;

	/* only one static mode in list */
	output->mode.flags =
		WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
	output->mode.width = output->fb_info.x_resolution;
	output->mode.height = output->fb_info.y_resolution;
	output->mode.refresh = output->fb_info.refresh_rate;
	wl_list_init(&output->base.mode_list);
	wl_list_insert(&output->base.mode_list, &output->mode.link);

	output->base.current_mode = &output->mode;
	output->base.subpixel = WL_OUTPUT_SUBPIXEL_UNKNOWN;
	output->base.make = "unknown";
	output->base.model = output->fb_info.id;
	output->base.name = strdup("image");

	section = weston_config_get_section(backend->compositor->config,
					    "output", "name",
					    output->base.name);
	weston_config_section_get_string(section, "transform", &s, "normal");
	if (weston_parse_transform(s, &config_transform) < 0)
		weston_log("Invalid transform \"%s\" for output %s\n",
			   s, output->base.name);
	free(s);

	weston_output_init(&output->base, backend->compositor,
	                   0, 0, output->fb_info.width_mm,
	                   output->fb_info.height_mm,
	                   config_transform,
			   1);

	if (backend->use_pixman) {
		if (pixman_renderer_output_create(&output->base) < 0)
			goto out_hw_surface;
	} else {
		setenv("HYBRIS_EGLPLATFORM", "wayland", 1);
		if (gl_renderer->output_create(&output->base,
					       (EGLNativeWindowType)NULL, NULL,
					       gl_renderer->opaque_attribs,
					       NULL, 0) < 0) {
			weston_log("gl_renderer_output_create failed.\n");
			goto out_hw_surface;
		}
	}

	loop = wl_display_get_event_loop(backend->compositor->wl_display);
	output->finish_frame_timer =
		wl_event_loop_add_timer(loop, finish_frame_handler, output);

	weston_compositor_add_output(backend->compositor, &output->base);

	weston_log("image output %d×%d px\n",
	           output->mode.width, output->mode.height);
	weston_log_continue(STAMP_SPACE "guessing %d Hz and 96 dpi\n",
	                    output->mode.refresh / 1000);

	return 0;

out_hw_surface:
	pixman_image_unref(output->hw_surface);
	output->hw_surface = NULL;
	weston_output_destroy(&output->base);
	image_frame_buffer_destroy(output);
out_free:
	free(output);

	return -1;
}

static void
image_output_destroy(struct weston_output *base)
{
	struct image_output *output = to_image_output(base);
	struct image_backend *backend = output->backend;

	weston_log("Destroying image output.\n");

	/* Close the frame buffer. */
	image_output_disable(base);

	if (backend->use_pixman) {
		if (base->renderer_state != NULL)
			pixman_renderer_output_destroy(base);
	} else {
		gl_renderer->output_destroy(base);
	}

	/* Remove the output. */
	weston_output_destroy(&output->base);

	free(output);
}

/* strcmp()-style return values. */
static int
compare_screen_info (const struct image_screeninfo *a,
                     const struct image_screeninfo *b)
{
	if (a->x_resolution == b->x_resolution &&
	    a->y_resolution == b->y_resolution &&
	    a->width_mm == b->width_mm &&
	    a->height_mm == b->height_mm &&
	    a->bits_per_pixel == b->bits_per_pixel &&
	    a->pixel_format == b->pixel_format &&
	    a->refresh_rate == b->refresh_rate)
		return 0;

	return 1;
}

static int
image_output_reenable(struct image_backend *backend,
                      struct weston_output *base)
{
	struct image_output *output = to_image_output(base);
	struct image_screeninfo new_screen_info;
	int fb_fd;
	const char *device;

	weston_log("Re-enabling image output.\n");

	/* Create the frame buffer. */
	fb_fd = image_frame_buffer_open(output, output->device,
	                                &new_screen_info);
	if (fb_fd < 0) {
		weston_log("Creating frame buffer failed.\n");
		goto err;
	}

	/* Check whether the frame buffer details have changed since we were
	 * disabled. */
	if (compare_screen_info (&output->fb_info, &new_screen_info) != 0) {
		/* Perform a mode-set to restore the old mode. */
//		if (image_set_screen_info(output, fb_fd,
//		                          &output->fb_info) < 0) {
//			weston_log("Failed to restore mode settings. "
//			           "Attempting to re-open output anyway.\n");
//		}

		close(fb_fd);

		/* Remove and re-add the output so that resources depending on
		 * the frame buffer X/Y resolution (such as the shadow buffer)
		 * are re-initialised. */
		device = output->device;
		image_output_destroy(base);
		image_output_create(backend, device);

		return 0;
	}

	/* Map the device if it has the same details as before. */
	if (backend->use_pixman) {
		if (image_frame_buffer_map(output, fb_fd) < 0) {
			weston_log("Mapping frame buffer failed.\n");
			goto err;
		}
	}

	return 0;

err:
	return -1;
}

/* NOTE: This leaves output->fb_info populated, caching data so that if
 * image_output_reenable() is called again, it can determine whether a mode-set
 * is needed. */
static void
image_output_disable(struct weston_output *base)
{
	struct image_output *output = to_image_output(base);
	struct image_backend *backend = output->backend;

	weston_log("Disabling image output.\n");

	if ( ! backend->use_pixman) return;

	if (output->hw_surface != NULL) {
		pixman_image_unref(output->hw_surface);
		output->hw_surface = NULL;
	}

	image_frame_buffer_destroy(output);
}

static void
image_backend_destroy(struct weston_compositor *base)
{
	struct image_backend *backend = to_image_backend(base);

	udev_input_destroy(&backend->input);

	/* Destroy the output. */
	weston_compositor_shutdown(base);

	/* Chain up. */
	weston_launcher_destroy(base->launcher);

	free(backend);
}

static void
session_notify(struct wl_listener *listener, void *data)
{
	struct weston_compositor *compositor = data;
	struct image_backend *backend = to_image_backend(compositor);
	struct weston_output *output;

	if (compositor->session_active) {
		weston_log("entering VT\n");
		compositor->state = backend->prev_state;

		wl_list_for_each(output, &compositor->output_list, link) {
			image_output_reenable(backend, output);
		}

		weston_compositor_damage_all(compositor);

		udev_input_enable(&backend->input);
	} else {
		weston_log("leaving VT\n");
		udev_input_disable(&backend->input);

		wl_list_for_each(output, &compositor->output_list, link) {
			image_output_disable(output);
		}

		backend->prev_state = compositor->state;
		weston_compositor_offscreen(compositor);

		/* If we have a repaint scheduled (from the idle handler), make
		 * sure we cancel that so we don't try to pageflip when we're
		 * vt switched away.  The OFFSCREEN state will prevent
		 * further attempts at repainting.  When we switch
		 * back, we schedule a repaint, which will process
		 * pending frame callbacks. */

		wl_list_for_each(output,
				 &compositor->output_list, link) {
			output->repaint_needed = 0;
		}
	}
}

static void
image_restore(struct weston_compositor *compositor)
{
	weston_launcher_restore(compositor->launcher);
}

static struct image_backend *
image_backend_create(struct weston_compositor *compositor, int *argc, char *argv[],
                     struct weston_config *config,
                     struct image_parameters *param)
{
	struct image_backend *backend;
	const char *seat_id = default_seat;

	weston_log("initializing image backend\n");

	backend = zalloc(sizeof *backend);
	if (backend == NULL)
		return NULL;

	backend->compositor = compositor;
	if (weston_compositor_set_presentation_clock_software(
							compositor) < 0)
		goto out_compositor;

	backend->udev = udev_new();
	if (backend->udev == NULL) {
		weston_log("Failed to initialize udev context.\n");
		goto out_compositor;
	}

	/* Set up the TTY. */
	backend->session_listener.notify = session_notify;
	wl_signal_add(&compositor->session_signal,
		      &backend->session_listener);
	compositor->launcher =
		weston_launcher_connect(compositor, param->tty, "seat0", false);
	if (!compositor->launcher) {
		weston_log("fatal: image backend should be run "
			   "using weston-launch binary or as root\n");
		goto out_udev;
	}

	backend->base.destroy = image_backend_destroy;
	backend->base.restore = image_restore;

	backend->prev_state = WESTON_COMPOSITOR_ACTIVE;
	backend->use_pixman = !param->use_gl;

	weston_setup_vt_switch_bindings(compositor);

	if (backend->use_pixman) {
		if (pixman_renderer_init(compositor) < 0)
			goto out_launcher;
	} else {
		gl_renderer = weston_load_module("gl-renderer.so",
						 "gl_renderer_interface");
		if (!gl_renderer) {
			weston_log("could not load gl renderer\n");
			goto out_launcher;
		}

		if (gl_renderer->create(compositor, NO_EGL_PLATFORM,
					EGL_DEFAULT_DISPLAY,
					gl_renderer->opaque_attribs,
					NULL, 0) < 0) {
			weston_log("gl_renderer_create failed.\n");
			goto out_launcher;
		}
	}

	if (image_output_create(backend, param->device) < 0)
		goto out_launcher;

	udev_input_init(&backend->input, compositor, backend->udev, seat_id);

	compositor->backend = &backend->base;
	return backend;

out_launcher:
	weston_launcher_destroy(compositor->launcher);

out_udev:
	udev_unref(backend->udev);

out_compositor:
	weston_compositor_shutdown(compositor);
	free(backend);

	return NULL;
}

WL_EXPORT int
backend_init(struct weston_compositor *compositor, int *argc, char *argv[],
	     struct weston_config *config,
	     struct weston_backend_config *config_base)
{
	struct image_backend *b;
	/* TODO: Ideally, available frame buffers should be enumerated using
	 * udev, rather than passing a device node in as a parameter. */
	struct image_parameters param = {
		.tty = 0, /* default to current tty */
		.device = "/tmp/image.ppm", /* default frame buffer */
		.use_gl = 0,
	};

	const struct weston_option image_options[] = {
		{ WESTON_OPTION_INTEGER, "tty", 0, &param.tty },
		{ WESTON_OPTION_STRING, "device", 0, &param.device },
		{ WESTON_OPTION_BOOLEAN, "use-gl", 0, &param.use_gl },
	};

	parse_options(image_options, ARRAY_LENGTH(image_options), argc, argv);

	b = image_backend_create(compositor, argc, argv, config, &param);
	if (b == NULL)
		return -1;
	return 0;
}
