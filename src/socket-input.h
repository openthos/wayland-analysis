#ifndef SOCKET_INPUT_H
#define SOCKET_INPUT_H

struct socket_input {
	struct weston_compositor *compositor;
	int suspended;
  int socket_fd;
  char *seat_id;
	struct wl_event_source *event_source;
};

int socket_input_create(struct socket_input *input, struct weston_compositor *c,
        const char *seat_id);

#endif /* SOCKET-INPUT_H */
