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
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <dlfcn.h>

#include "shared/helpers.h"
#include "compositor.h"
#include "compositor-image.h"
#include "launcher-util.h"
#include "pixman-renderer.h"
#include "libinput-seat.h"
#include "presentation-time-server-protocol.h"
#include "socket-input.h"

struct image_backend {
	struct weston_backend base;
	struct weston_compositor *compositor;
	uint32_t prev_state;

	struct socket_input input;
	uint32_t output_transform;
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
	char *device;
	struct image_screeninfo fb_info;
	void *fb, *fb_tmp;  /* length is fb_info.buffer_length */

	/* pixman details. */
	pixman_image_t *hw_surface;
	uint8_t depth;
	int32_t scale;//new add by ipfgo,copy from x11 ,20170524
};

static const char default_seat[] = "seat0";

static struct image_screeninfo global_screeninfo = {
	.x_resolution = 800,
	.y_resolution = 600,
	.bits_per_pixel = 32,
	.id = "imagescreen",
	.pixel_format = PIXMAN_a8b8g8r8,
	.refresh_rate = 60000,
};


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
	weston_output_finish_frame(output, &ts, WP_PRESENTATION_FEEDBACK_INVALID);
}

static int
image_output_repaint(struct weston_output *base, pixman_region32_t *damage,
		     void *repaint_data)
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
image_query_screen_info(struct image_screeninfo *info)
{
	memcpy(info, &global_screeninfo, sizeof(struct image_screeninfo));

	info->width_mm = info->x_resolution;
	info->height_mm = info->y_resolution;
	info->line_length = info->width_mm * (info->bits_per_pixel / 8);
	info->buffer_length = info->line_length * info->height_mm;

	memcpy(&global_screeninfo, info, sizeof(struct image_screeninfo));
	return 1;
}

static void image_frame_buffer_destroy(struct image_output *output);

static void create_file(const char *path, size_t length) {
	char *buf = zalloc(length);
	if (!buf) {
		weston_log("Failed to create file %s when zalloc %zu\n", path, length);
		return;
	}
//	int fd = open(path, O_RDWR | O_CLOEXEC | O_CREAT);
	int fd = open(path, O_RDWR | O_CLOEXEC);	
	if (fd < 0) {
		weston_log("Failed to create file %s when open\n", path);
		return;
	}
	if (write(fd, buf, length) < 0) {
		weston_log("Failed to create file %s when write\n", path);
		return;
	}
	if (close(fd) < 0) {
		weston_log("Failed to create file %s when close\n", path);
		return;
	}
	free(buf);
	if (chmod(path, 0777) < 0) {
		weston_log("Failed to chmod file %s\n", path);
		return;
	}
	weston_log("Created file %s with size %zu\n", path, length);
}


/* Returns an FD for the frame buffer device. */
static int
image_frame_buffer_open(struct image_output *output, const char *fb_dev,
                        struct image_screeninfo *screen_info)
{
	int fd = -1;

	/* Grab the screen info. */
	if (image_query_screen_info(screen_info) < 0) {
		weston_log("Failed to get frame buffer info: %s\n",
		           strerror(errno));

		close(fd);
		return -1;
	}

	weston_log("Opening image frame buffer.\n");

	create_file(fb_dev, screen_info->buffer_length);

	/* Open the frame buffer device. */
	fd = open(fb_dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		weston_log("Failed to open frame buffer device ‘%s’: %s\n",
		           fb_dev, strerror(errno));
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
	output->fb_tmp = malloc(output->fb_info.buffer_length);	
	if (output->fb == MAP_FAILED || !output->fb_tmp) {
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
image_output_enable(struct weston_output *base)
{
	struct image_output *output = to_image_output(base);
	struct image_backend *backend = to_image_backend(base->compositor);
	int fb_fd;
	struct wl_event_loop *loop;

	/* Create the frame buffer. */
	fb_fd = image_frame_buffer_open(output, output->device, &output->fb_info);
	if (fb_fd < 0) {
		weston_log("Creating frame buffer failed.\n");
		return -1;
	}

	if (image_frame_buffer_map(output, fb_fd) < 0) {
		weston_log("Mapping frame buffer failed.\n");
		return -1;
	}

	output->base.start_repaint_loop = image_output_start_repaint_loop;
	output->base.repaint = image_output_repaint;

	if (pixman_renderer_output_create(&output->base) < 0)
		goto out_hw_surface;

	loop = wl_display_get_event_loop(backend->compositor->wl_display);
	output->finish_frame_timer =
		wl_event_loop_add_timer(loop, finish_frame_handler, output);

	weston_log("image output %d×%d px\n",
	           output->mode.width, output->mode.height);
	weston_log_continue(STAMP_SPACE "guessing %d Hz and 96 dpi\n",
	                    output->mode.refresh / 1000);

	return 0;

out_hw_surface:
	pixman_image_unref(output->hw_surface);
	output->hw_surface = NULL;
	image_frame_buffer_destroy(output);

	return -1;
}

static int
image_output_set_size(struct weston_output *base, int width, int height)
{
	struct image_output *output = to_image_output(base);
	int output_width, output_height;

	//Make sure we have scale set. 
	assert(output->base.scale);

	if (width < 1) {
		weston_log("Invalid width \"%d\" for output %s\n",
			   width, output->base.name);
		return -1;
	}

	if (height < 1) {
		weston_log("Invalid height \"%d\" for output %s\n",
			   height, output->base.name);
		return -1;
	}

	global_screeninfo.x_resolution = width;
	global_screeninfo.y_resolution = height;
	
	output_width = width * output->base.scale;
	output_height = height * output->base.scale;

	output->mode.flags =
		WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;

	output->mode.width = output_width;
	output->mode.height = output_height;
	output->mode.refresh = 60000;
	output->scale = output->base.scale;
	wl_list_init(&output->base.mode_list);
	wl_list_insert(&output->base.mode_list, &output->mode.link);

	output->base.current_mode = &output->mode;
	output->base.make = "weston-image";
	output->base.model = "none";

	output->base.mm_width = width;
	output->base.mm_height = height;

	return 0;
}

static int
image_output_create(struct image_backend *backend,
                    const char *device)
{
	struct image_output *output;
	int fb_fd;

	weston_log("Creating image output.\n");

	output = zalloc(sizeof *output);
	if (output == NULL)
		return -1;

	output->backend = backend;
	output->device = strdup(device);

	/* Create the frame buffer. */
	fb_fd = image_frame_buffer_open(output, device, &output->fb_info);
	if (fb_fd < 0) {
		weston_log("Creating frame buffer failed.\n");
		goto out_free;
	}

	output->base.name = strdup("image");
	output->base.destroy = image_output_destroy;
	output->base.disable = NULL;
	output->base.enable = image_output_enable;

	weston_output_init(&output->base, backend->compositor);

	close(fb_fd);

	weston_compositor_add_pending_output(&output->base, backend->compositor);

	return 0;

out_free:
	free(output->device);
	free(output);

	return -1;
}

static void
image_output_destroy(struct weston_output *base)
{
	struct image_output *output = to_image_output(base);

	weston_log("Destroying image output.\n");

	/* Close the frame buffer. */
	image_output_disable(base);

	if (base->renderer_state != NULL)
		pixman_renderer_output_destroy(base);

	/* Remove the output. */
	weston_output_destroy(&output->base);

	free(output->device);
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
	char *device;

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
		device = strdup(output->device);
		image_output_destroy(&output->base);
		image_output_create(backend, device);
		free(device);

		return 0;
	}

	/* Map the device if it has the same details as before. */
	if (image_frame_buffer_map(output, fb_fd) < 0) {
		weston_log("Mapping frame buffer failed.\n");
		goto err;
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

	weston_log("Disabling image output.\n");

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

	socket_input_destroy(&backend->input);

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

	} else {
		weston_log("leaving VT\n");

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
			output->repaint_needed = false;
		}
	}
}

static void
image_restore(struct weston_compositor *compositor)
{
	weston_launcher_restore(compositor->launcher);
}

static const struct weston_image_output_api api = {
	image_output_set_size,
};

static struct image_backend *
image_backend_create(struct weston_compositor *compositor,
                     struct weston_image_backend_config *param)
{
	struct image_backend *backend;
	const char *seat_id = default_seat;
	int ret;

	weston_log("initializing image backend\n");

	backend = zalloc(sizeof *backend);
	if (backend == NULL)
		return NULL;

	backend->compositor = compositor;
	if (weston_compositor_set_presentation_clock_software(
							compositor) < 0)
		goto out_compositor;

	/* Set up the TTY. */
	backend->session_listener.notify = session_notify;
	wl_signal_add(&compositor->session_signal,
		      &backend->session_listener);
	compositor->launcher =
		weston_launcher_connect(compositor, /* uselses */ 0, "seat0", false);
	if (!compositor->launcher) {
		weston_log("fatal: image backend should be run "
			   "using weston-launch binary or as root\n");
		goto out_compositor;
	}

	backend->base.destroy = image_backend_destroy;
	backend->base.restore = image_restore;

	backend->prev_state = WESTON_COMPOSITOR_ACTIVE;

	weston_setup_vt_switch_bindings(compositor);

	if (pixman_renderer_init(compositor) < 0)
		goto out_launcher;

	if (image_output_create(backend, param->device) < 0)
		goto out_launcher;

	socket_input_init(&backend->input, compositor, seat_id, param->configure_device);
	
	compositor->backend = &backend->base;

	ret = weston_plugin_api_register(compositor, WESTON_IMAGE_OUTPUT_API_NAME,
					 &api, sizeof(api));

	if (ret < 0) {
		weston_log("Failed to register output API.\n");
		goto out_compositor;
	}

	return backend;

out_launcher:
	weston_launcher_destroy(compositor->launcher);

out_compositor:
	weston_compositor_shutdown(compositor);
	free(backend);

	return NULL;
}

static void
config_init_to_defaults(struct weston_image_backend_config *config)
{
	/* TODO: Ideally, available frame buffers should be enumerated using
	 * udev, rather than passing a device node in as a parameter. */
	config->device = "/tmp/image.bin"; /* default frame buffer */
}

WL_EXPORT int
weston_backend_init(struct weston_compositor *compositor,
		    struct weston_backend_config *config_base)
{
	struct image_backend *b;
	struct weston_image_backend_config config = {{ 0, }};

	if (config_base == NULL ||
	    config_base->struct_version != WESTON_IMAGE_BACKEND_CONFIG_VERSION ||
	    config_base->struct_size > sizeof(struct weston_image_backend_config)) {
		weston_log("image backend config structure is invalid\n");
		return -1;
	}

	config_init_to_defaults(&config);
	memcpy(&config, config_base, config_base->struct_size);

	b = image_backend_create(compositor, &config);
	if (b == NULL)
		return -1;
	return 0;
}
