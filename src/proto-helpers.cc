#include <string>

#include "socket-input.h"
#include "weston_proto.pb.h"

using ::std::string;
using ::com::dxmtb::westonapp::InputEventProto;
using ::com::dxmtb::westonapp::MotionEvent;

void handle_key_event(const socket_input *input, const InputEventProto &inputEvent) {
    weston_log("handle_key_event not implemented\n");
}

void handle_motion_event(const socket_input *input, const InputEventProto &inputEvent) {
    weston_log("handle_motion_event enter\n");
    if (!inputEvent.has_motion_event()) {
        weston_log("no MotionEvent in InputEventProto\n");
        return;
    }
    const MotionEvent &motionEvent = inputEvent.motion_event();
    weston_seat *seat = &(input->seat->base);
    switch (motionEvent.action_type()) {
        case MotionEvent::ACTION_DOWN:
          notify_button(seat,
                        inputEvent.time(),
                        0x110, //BTN_LEFT
                        WL_POINTER_BUTTON_STATE_PRESSED);
          break;
        case MotionEvent::ACTION_UP:
          notify_button(seat,
                        inputEvent.time(),
                        0x110, //BTN_LEFT
                        WL_POINTER_BUTTON_STATE_RELEASED);
          break;
        default:
          weston_log("Unknown action type\n");
          break;
    }
}

extern "C"
void handle_event_proto(const socket_input *input, const char *buf, size_t data_length) {
    weston_log("handle_event_proto enter\n");

    InputEventProto inputEvent;
    if (!inputEvent.ParseFromString(string(buf, data_length))) {
        weston_log("failed to parse proto, event ignored\n");
        return;
    }

    if (inputEvent.type() == InputEventProto::KeyEventType) {
        handle_key_event(input, inputEvent);
    } else if (inputEvent.type() == InputEventProto::MotionEventType) {
        handle_motion_event(input, inputEvent);
    } else {
        weston_log("Unknown event type\n");
    }
}
