#include <string>

#include "socket-input.h"
#include "weston_proto.pb.h"

using ::std::string;
using ::com::dxmtb::westonapp::InputEventProto;
using ::com::dxmtb::westonapp::MotionEvent;
using ::com::dxmtb::westonapp::KeyEvent;

void handle_key_event(const socket_input *input, const InputEventProto &inputEvent) {
    weston_log("handle_key_event enter\n");
    if (!inputEvent.has_key_event()) {
        weston_log("no KeyEvent in InputEventProto\n");
        return;
    }
    const KeyEvent &keyEvent = inputEvent.key_event();
    weston_seat *seat = &(input->seat->base);
    switch (keyEvent.action_type()) {
        case KeyEvent::ACTION_DOWN:
          notify_key(seat,
                     inputEvent.time(),
                     keyEvent.key(),
                     WL_KEYBOARD_KEY_STATE_PRESSED,
                     STATE_UPDATE_AUTOMATIC);
          break;
        case KeyEvent::ACTION_UP:
          notify_key(seat,
                     inputEvent.time(),
                     keyEvent.key(),
                     WL_KEYBOARD_KEY_STATE_RELEASED,
                     STATE_UPDATE_AUTOMATIC);
          break;
        default:
          weston_log("Unknown action type\n");
          break;
    }
}

void handle_motion_event(const socket_input *input, const InputEventProto &inputEvent) {
    weston_log("handle_motion_event enter\n");
    if (!inputEvent.has_motion_event()) {
        weston_log("no MotionEvent in InputEventProto\n");
        return;
    }
    const MotionEvent &motionEvent = inputEvent.motion_event();

    weston_seat *seat = &(input->seat->base);
    wl_fixed_t wl_x, wl_y;
    wl_x = (double)motionEvent.x();
    wl_y = (double)motionEvent.y();
    struct weston_pointer_axis_event weston_event;
    double value;
    switch (motionEvent.action_type()) {
        case MotionEvent::ACTION_HOVER_MOVE:
          notify_motion_absolute(seat,
                                 inputEvent.time(),
                                 wl_x, wl_y);
          break;
        case MotionEvent::ACTION_BUTTON_PRESS:
          notify_button(seat,
                        inputEvent.time(),
                        motionEvent.button(),
                        WL_POINTER_BUTTON_STATE_PRESSED);
          break;
        case MotionEvent::ACTION_BUTTON_RELEASE:
          notify_button(seat,
                        inputEvent.time(),
                        motionEvent.button(),
                        WL_POINTER_BUTTON_STATE_RELEASED);
          break;
        case MotionEvent::ACTION_SCROLL:
          value = motionEvent.axis();
          weston_event.axis = WL_POINTER_AXIS_VERTICAL_SCROLL;
          weston_event.value = wl_fixed_from_double(10 * value);
          weston_event.discrete = (int)value;
          weston_event.has_discrete = true;
          notify_axis(seat, inputEvent.time(),
                  &weston_event);
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

    weston_log("InputEvenProto %s\n", inputEvent.DebugString().c_str());

    if (inputEvent.type() == InputEventProto::KeyEventType) {
        handle_key_event(input, inputEvent);
    } else if (inputEvent.type() == InputEventProto::MotionEventType) {
        handle_motion_event(input, inputEvent);
    } else {
        weston_log("Unknown event type\n");
    }
}
