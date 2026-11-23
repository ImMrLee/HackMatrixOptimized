#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <wayland-server-core.h>

// wlroots headers have no extern "C" guards of their own.
extern "C" {
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>

// The unstable input-method header names a field `delete`, which is a C++
// keyword. Rename it for the duration of the include and use `delete_` below.
#define delete delete_
#include <wlr/types/wlr_input_method_v2.h>
#undef delete
}

struct WlrServer;
struct wlr_surface;

// Bridges zwp_text_input_v3 clients (GTK/Qt/terminals) with a
// zwp_input_method_v2 client (fcitx5). The compositor owns keyboard focus, so
// the relay is told about focus changes by WaylandApp/keyboard code and forwards
// text-input state to the input method, commits back to the focused surface, and
// routes the IME's keyboard grab.
//
// Candidate popup surfaces are rendered as an ImGui overlay (see renderOverlay)
// because HackMatrix draws its own UI rather than using wlr_scene.
class ImeRelay
{
public:
  ImeRelay(WlrServer* server,
           wlr_seat* seat,
           wlr_text_input_manager_v3* textInputManager,
           wlr_input_method_manager_v2* inputMethodManager);
  ~ImeRelay();

  ImeRelay(const ImeRelay&) = delete;
  ImeRelay& operator=(const ImeRelay&) = delete;

  // Called whenever the seat keyboard focus changes (including nullptr).
  void setFocus(wlr_surface* surface);
  // Called inside an active ImGui frame (GL context current).
  void renderOverlay();

  bool hasInputMethod() const { return inputMethod_ != nullptr; }

private:
  struct TextInput
  {
    ImeRelay* relay = nullptr;
    wlr_text_input_v3* input = nullptr;
    wl_listener enable{};
    wl_listener commit{};
    wl_listener disable{};
    wl_listener destroy{};
  };

  struct Popup
  {
    ImeRelay* relay = nullptr;
    wlr_input_popup_surface_v2* popup = nullptr;
    wl_listener commit{};
    wl_listener destroy{};
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    bool dirty = false;
    bool visible = false;
    bool needsRead = true;
    unsigned int texture = 0;
    unsigned int glFormat = 0x1908; // GL_RGBA
    int uploadedWidth = 0;
    int uploadedHeight = 0;
  };

  void handleNewTextInput(wlr_text_input_v3* input);
  void handleNewInputMethod(wlr_input_method_v2* inputMethod);
  void handleImCommit();
  void handleImGrab(wlr_input_method_keyboard_grab_v2* grab);
  void handleImGrabDestroy();
  void handleImDestroy();
  void handleImPopup(wlr_input_popup_surface_v2* popup);
  void handlePopupCommit(Popup* popup);
  void readPopupPixels(Popup* popup);
  void sendInputMethodState(TextInput* textInput);
  TextInput* focusedTextInput() const;

  static void grabEnter(wlr_seat_keyboard_grab* grab,
                        wlr_surface* surface,
                        const uint32_t* keycodes,
                        size_t numKeycodes,
                        const wlr_keyboard_modifiers* modifiers);
  static void grabClearFocus(wlr_seat_keyboard_grab* grab);
  static void grabKey(wlr_seat_keyboard_grab* grab,
                      uint32_t timeMsec,
                      uint32_t key,
                      uint32_t state);
  static void grabModifiers(wlr_seat_keyboard_grab* grab,
                            const wlr_keyboard_modifiers* modifiers);
  static void grabCancel(wlr_seat_keyboard_grab* grab);

  static const wlr_keyboard_grab_interface keyboardGrabImpl_;

  WlrServer* server_ = nullptr;
  wlr_seat* seat_ = nullptr;
  wlr_text_input_manager_v3* textInputManager_ = nullptr;
  wlr_input_method_manager_v2* inputMethodManager_ = nullptr;
  wlr_input_method_v2* inputMethod_ = nullptr;
  wlr_surface* focusedSurface_ = nullptr;
  std::vector<std::unique_ptr<TextInput>> textInputs_;
  std::vector<std::unique_ptr<Popup>> popups_;

  wlr_seat_keyboard_grab seatGrab_{};
  bool seatGrabActive_ = false;
  wlr_input_method_keyboard_grab_v2* imGrab_ = nullptr;
  wl_listener imGrabDestroy_{};

  wl_listener newTextInput_{};
  wl_listener newInputMethod_{};
  wl_listener imCommit_{};
  wl_listener imGrabKeyboard_{};
  wl_listener imDestroy_{};
  wl_listener imNewPopup_{};

  std::vector<unsigned int> pendingTextureDeletes_;
};
