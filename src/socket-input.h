#ifndef SOCKET_INPUT_H
#define SOCKET_INPUT_H

struct socket_input {
	struct weston_compositor *compositor;
	int suspended;
  int socket_fd;
  socket_input_seat *seat;
	struct wl_event_source *event_source;
};

struct socket_input_seat {
  weston_seat base;
	struct wl_listener output_create_listener;
}

int socket_input_init(struct socket_input *input, struct weston_compositor *c,
        const char *seat_id);

#endif /* SOCKET-INPUT_H */
