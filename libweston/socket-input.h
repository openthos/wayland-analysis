#ifndef SOCKET_INPUT_H
#define SOCKET_INPUT_H

#include "compositor.h"
typedef void (*socket_configure_device_t)(struct weston_compositor *compositor);

struct socket_input {
	struct weston_compositor *compositor;
	int suspended;
 	int socket_fd;
  	struct socket_input_seat *seat;
	struct wl_event_source *event_source;
	socket_configure_device_t configure_device;
};

struct socket_input_seat {
  struct weston_seat base;
	struct wl_listener output_create_listener;
};



int socket_input_init(struct socket_input *input, struct weston_compositor *c,
        const char *seat_id,
	socket_configure_device_t configure_device);

void socket_input_destroy(struct socket_input *input);

#endif /* SOCKET-INPUT_H */
