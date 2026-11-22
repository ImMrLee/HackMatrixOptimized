#pragma once

#include "imgui/imgui.h"
#include <xkbcommon/xkbcommon.h>

// Maps an XKB keysym to the corresponding ImGuiKey.
// Returns ImGuiKey_None when the key has no ImGui equivalent.
inline ImGuiKey
keysymToImGuiKey(xkb_keysym_t sym)
{
  if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) {
    return static_cast<ImGuiKey>(ImGuiKey_A + (sym - XKB_KEY_a));
  }
  if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) {
    return static_cast<ImGuiKey>(ImGuiKey_A + (sym - XKB_KEY_A));
  }
  if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9) {
    return static_cast<ImGuiKey>(ImGuiKey_0 + (sym - XKB_KEY_0));
  }
  switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
      return ImGuiKey_Enter;
    case XKB_KEY_Escape:
      return ImGuiKey_Escape;
    case XKB_KEY_BackSpace:
      return ImGuiKey_Backspace;
    case XKB_KEY_Tab:
      return ImGuiKey_Tab;
    case XKB_KEY_Delete:
      return ImGuiKey_Delete;
    case XKB_KEY_Home:
      return ImGuiKey_Home;
    case XKB_KEY_End:
      return ImGuiKey_End;
    case XKB_KEY_Left:
      return ImGuiKey_LeftArrow;
    case XKB_KEY_Right:
      return ImGuiKey_RightArrow;
    case XKB_KEY_Up:
      return ImGuiKey_UpArrow;
    case XKB_KEY_Down:
      return ImGuiKey_DownArrow;
    case XKB_KEY_space:
      return ImGuiKey_Space;
    case XKB_KEY_Shift_L:
      return ImGuiKey_LeftShift;
    case XKB_KEY_Shift_R:
      return ImGuiKey_RightShift;
    case XKB_KEY_Control_L:
      return ImGuiKey_LeftCtrl;
    case XKB_KEY_Control_R:
      return ImGuiKey_RightCtrl;
    case XKB_KEY_Alt_L:
      return ImGuiKey_LeftAlt;
    case XKB_KEY_Alt_R:
      return ImGuiKey_RightAlt;
    case XKB_KEY_Super_L:
      return ImGuiKey_LeftSuper;
    case XKB_KEY_Super_R:
      return ImGuiKey_RightSuper;
    default:
      return ImGuiKey_None;
  }
}
