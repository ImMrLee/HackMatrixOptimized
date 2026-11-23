#pragma once

#include <wayland-server-core.h>
#include <wlr/types/wlr_input_device.h>

struct WlrServer;

void handle_new_keyboard(WlrServer* server, wlr_input_device* device);
// Headless seats have no physical keyboard. Without one the seat has no keymap
// and neither synthetic key injection nor IME keyboard grabs can work.
void create_virtual_keyboard(WlrServer* server);
