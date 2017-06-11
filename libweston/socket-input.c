#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdint.h>

#include "compositor.h"
#include "sockets.h"
#include "socket-input.h"
#include "shared/helpers.h"

#define SERVER_HELLO "ServerHello"
#define CLIENT_HELLO "ClientHello"
char *socket_path = "/tmp/weston_socket";
// proto-helpers.cc
void handle_event_proto(const struct socket_input* input,
        const char *buf, size_t data_length);

static int recv_len(int fd, char *buf, size_t data_length) {
    size_t total_read = 0;
    int ret;
    while (data_length > total_read) {
        ret = recv(fd, buf + total_read, data_length - total_read, 0);
        if (ret == -1)
            return -1;
        total_read += ret;
    }
    return 0;
}

static int send_len(int fd, const char *buf, size_t data_length) {
    size_t total_sent = 0;
    int ret;
    while (data_length > total_sent) {
        ret = send(fd, buf + total_sent, data_length - total_sent, 0);
        if (ret == -1)
            return -1;
        total_sent += ret;
    }
    return 0;
}

static int send_msg(int fd, const char *buf, size_t len) {
    int32_t net_len = htonl(len);
    if (send_len(fd, (char*)&net_len, sizeof(net_len)) != 0) {
        weston_log("failed to send msg length\n");
        return -1;
    }
    if (send_len(fd, buf, len) != 0) {
        weston_log("failed to send msg body\n");
        return -1;
    }
    return 0;
}

static int
socket_input_dispatch(int fd, uint32_t mask, void *data)
{
	struct socket_input *input = data;
    uint32_t data_length;
    char *buf;

    weston_log("try to recv message from %d\n", fd);
    if (recv_len(fd, (char*)(&data_length), sizeof(data_length)) != 0) {
        weston_log("failed to recv data length\n");
        return -1;
    }
    data_length = ntohl(data_length);
    weston_log("message length %d\n", data_length);

    buf = malloc(data_length);
    if (!buf) {
        weston_log("failed to malloc size %d\n", data_length);
    }

    weston_log("try to get message content\n");
    if (recv_len(fd, buf, data_length) != 0) {
        weston_log("failed to recv data length\n");
        return -1;
    }

    if (data_length == strlen(CLIENT_HELLO) &&
            strncmp(buf, CLIENT_HELLO, data_length) == 0) {
        if (send_msg(fd, SERVER_HELLO, strlen(SERVER_HELLO)) != 0) {
            weston_log("failed to send server hello\n");
        }
        weston_log("hand shake success\n");
        return 0;
    }

    weston_log("process input event\n");
    handle_event_proto(input, buf, data_length);
    free(buf);
    return 0;
}

static int
socket_input_source_dispatch(int fd, uint32_t mask, void *data)
{
	struct socket_input *input = data;
	struct wl_event_loop *loop;
	//struct wl_event_source *event_source;

    weston_log("enter socket_input_source_dispatch\n");
    int clientfd = accept(fd, 0, 0);
    if (clientfd == -1)
        return -1;
    weston_log("accepted client connect\n");

	loop = wl_display_get_event_loop(input->compositor->wl_display);
	//event_source =
		wl_event_loop_add_fd(loop, clientfd, WL_EVENT_READABLE,
				     socket_input_dispatch, input);
    return 0;
}

static const char default_seat_name[] = "default";

static void
socket_input_led_update(struct weston_seat *seat, enum weston_led weston_leds) {
    weston_log("socket_input_led_update not implemented\n");
}

static void
socket_input_notify_output_create(struct wl_listener *listener, void *data)
{
    return;
	struct socket_input_seat *seat = container_of(listener, struct socket_input_seat,
					      output_create_listener);
	struct weston_output *output = data;
    seat->base.output = output;
}


static struct socket_input_seat*
socket_input_seat_init(struct socket_input *input, const char *seat_id) {
	struct weston_compositor *c = input->compositor;
	struct socket_input_seat *seat;
	struct weston_seat *seat_base;
	struct weston_pointer *pointer;

	seat = input->seat = zalloc(sizeof *seat);
	if (!seat)
		return NULL;
    seat_base = &(seat->base);

	weston_seat_init(seat_base, c, default_seat_name);
	seat_base->led_update = socket_input_led_update;

	seat->output_create_listener.notify = socket_input_notify_output_create;
	wl_signal_add(&c->output_created_signal,
		      &seat->output_create_listener);

    weston_seat_init_keyboard(seat_base, NULL);
    weston_seat_init_pointer(seat_base);
    weston_seat_init_touch(seat_base);

	pointer = weston_seat_get_pointer(seat_base);
	if (seat_base->output && pointer)
    		weston_pointer_clamp(pointer,
				     &pointer->x,
				     &pointer->y);
	//if (!input->suspended)
    weston_seat_repick(seat_base);

	return seat;
}


int socket_input_init(struct socket_input *input, struct weston_compositor *c,
        const char *seat_id,
	socket_configure_device_t configure_device)
{
	struct wl_event_loop *loop;

	memset(input, 0, sizeof *input);

	input->compositor = c;
	input->configure_device = configure_device;

	input->socket_fd = socket_local_server(socket_path,
            ANDROID_SOCKET_NAMESPACE_FILESYSTEM, AF_LOCAL);

	if (input->socket_fd == -1) {
        weston_log("Failed to listen on %s\n", socket_path);
		return -1;
	}

    weston_log("Socket input listen on %s\n", socket_path);

    socket_input_seat_init(input, seat_id);

	loop = wl_display_get_event_loop(c->wl_display);
	input->event_source =
		wl_event_loop_add_fd(loop, input->socket_fd, WL_EVENT_READABLE,
				     socket_input_source_dispatch, input);


    return 0;
}

void
socket_input_destroy(struct socket_input *input)
{
    weston_seat_release(&input->seat->base);
}
