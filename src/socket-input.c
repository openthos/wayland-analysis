#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>

#include "sockets.h"

#define SOCKET_PATH "/sdcard/weston_socket"
#define SERVER_HELLO "ServerHello"
#define CLIENT_HELLO "ClientHello"

void handle_event_proto(const char *buf, data_length) {
    weston_log("handle_event_proto not implemented\n");
}

static int
socket_input_dispatch(int fd, uint32_t mask, void *data)
{
    uint32_t data_length;
    char *buf;

    if (recv_len(fd, &data_length, sizeof(data_length)) != 0) {
        weston_log("failed to recv data length\n");
        return -1;
    }

    buf = malloc(data_length);
    if (!buf) {
        weston_log("failed to malloc size %d\n", data_length);
    }

    if (recv_len(fd, buf, data_length) != 0) {
        weston_log("failed to recv data length\n");
        return -1;
    }

    if (data_length == strlen(CLIENT_HELLO) &&
            strcmp(buf, CLIENT_HELLO, data_length) == 0) {
        if (send_len(fd, SERVER_HELLO, strlen(SERVER_HELLO) != 0)) {
            weston_log("failed to send server hello\n");
            return -1;
        }
        return 0;
    }

    handle_event_proto(buf, data_length);
    free(buf);
}

static int
socket_input_source_dispatch(int fd, uint32_t mask, void *data)
{
	struct socket_input *input = data;
    struct sockaddr_in cli_addr;
	struct wl_event_loop *loop;
	struct wl_event_source *event_source;
    int len;

    int clientfd = accept(sockfd, (struct sockaddr *)&cli_addr, &len);

	loop = wl_display_get_event_loop(input->compositor->wl_display);
	event_source =
		wl_event_loop_add_fd(loop, clientfd, WL_EVENT_READABLE,
				     socket_input_dispatch, input);

}


int socket_input_create(struct socket_input *input, struct weston_compositor *c,
        const char *seat_id)
{
	struct wl_event_loop *loop;

	memset(input, 0, sizeof *input);

	input->compositor = c;

	input->socket_fd = socket_local_server(SOCKET_PATH,
            ANDROID_SOCKET_NAMESPACE_FILESYSTEM, AF_LOCAL);

	if (input->socket_fd == -1) {
		return -1;
	}

    input->seat_id = strdup(seat_id);

	loop = wl_display_get_event_loop(c->wl_display);
	input->event_source =
		wl_event_loop_add_fd(loop, input->socket_fd,WL_EVENT_READABLE,
				     socket_input_source_dispatch, input);

    return 0;
}
