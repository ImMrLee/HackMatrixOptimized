#include "wayland/ime.h"

#include <algorithm>
#include <cstdio>
#include <sys/types.h>

#include <drm_fourcc.h>

#include <glad/glad.h>
#include <imgui.h>

extern "C" {
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/util/box.h>
}

#include <spdlog/spdlog.h>

#include "wayland/wlr_compositor.h"

namespace {

void
removeListener(wl_listener& listener)
{
  if (listener.link.prev != nullptr || listener.link.next != nullptr) {
    wl_list_remove(&listener.link);
  }
  listener.link.prev = nullptr;
  listener.link.next = nullptr;
}

bool
sameClient(wlr_text_input_v3* input, wlr_surface* surface)
{
  return input != nullptr && input->resource != nullptr && surface != nullptr &&
         surface->resource != nullptr &&
         wl_resource_get_client(input->resource) ==
           wl_resource_get_client(surface->resource);
}

bool
hasAlpha(uint32_t format)
{
  return format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_ABGR8888;
}

} // namespace

const wlr_keyboard_grab_interface ImeRelay::keyboardGrabImpl_ = {
  .enter = ImeRelay::grabEnter,
  .clear_focus = ImeRelay::grabClearFocus,
  .key = ImeRelay::grabKey,
  .modifiers = ImeRelay::grabModifiers,
  .cancel = ImeRelay::grabCancel,
};

ImeRelay::ImeRelay(WlrServer* server,
                   wlr_seat* seat,
                   wlr_text_input_manager_v3* textInputManager,
                   wlr_input_method_manager_v2* inputMethodManager)
  : server_(server)
  , seat_(seat)
  , textInputManager_(textInputManager)
  , inputMethodManager_(inputMethodManager)
{
  if (textInputManager_ != nullptr) {
    newTextInput_.notify = [](wl_listener* listener, void* data) {
      auto* relay = wl_container_of(
        listener, static_cast<ImeRelay*>(nullptr), newTextInput_);
      relay->handleNewTextInput(static_cast<wlr_text_input_v3*>(data));
    };
    wl_signal_add(&textInputManager_->events.text_input, &newTextInput_);
  }
  if (inputMethodManager_ != nullptr) {
    newInputMethod_.notify = [](wl_listener* listener, void* data) {
      auto* relay = wl_container_of(
        listener, static_cast<ImeRelay*>(nullptr), newInputMethod_);
      relay->handleNewInputMethod(
        static_cast<wlr_input_method_v2*>(data));
    };
    wl_signal_add(&inputMethodManager_->events.input_method, &newInputMethod_);
  }
}

ImeRelay::~ImeRelay()
{
  removeListener(newTextInput_);
  removeListener(newInputMethod_);
  removeListener(imCommit_);
  removeListener(imGrabKeyboard_);
  removeListener(imDestroy_);
  removeListener(imNewPopup_);
  removeListener(imGrabDestroy_);
  if (seatGrabActive_ && seat_ != nullptr) {
    wlr_seat_keyboard_end_grab(seat_);
    seatGrabActive_ = false;
  }
  for (auto& entry : textInputs_) {
    removeListener(entry->enable);
    removeListener(entry->commit);
    removeListener(entry->disable);
    removeListener(entry->destroy);
  }
  textInputs_.clear();
  for (auto& popup : popups_) {
    removeListener(popup->commit);
    removeListener(popup->destroy);
  }
  popups_.clear();
}

void
ImeRelay::setFocus(wlr_surface* surface)
{
  if (surface == focusedSurface_) {
    return;
  }
  focusedSurface_ = surface;
  spdlog::debug("IME: focus surface {}",
                surface != nullptr ? static_cast<void*>(surface) : nullptr);
  bool deactivated = false;

  for (auto& entry : textInputs_) {
    auto* input = entry->input;
    if (input == nullptr) {
      continue;
    }
    if (sameClient(input, surface)) {
      if (inputMethod_ != nullptr && input->focused_surface != surface) {
        wlr_text_input_v3_send_enter(input, surface);
      }
      continue;
    }
    if (input->focused_surface != nullptr) {
      wlr_text_input_v3_send_leave(input);
      if (inputMethod_ != nullptr && !deactivated) {
        wlr_input_method_v2_send_deactivate(inputMethod_);
        deactivated = true;
      }
    }
  }
}

ImeRelay::TextInput*
ImeRelay::focusedTextInput() const
{
  for (const auto& entry : textInputs_) {
    if (entry->input != nullptr && entry->input->focused_surface != nullptr) {
      return entry.get();
    }
  }
  return nullptr;
}

void
ImeRelay::handleNewTextInput(wlr_text_input_v3* input)
{
  if (input == nullptr || input->seat != seat_) {
    return;
  }
  auto entry = std::make_unique<TextInput>();
  entry->relay = this;
  entry->input = input;

  entry->enable.notify = [](wl_listener* listener, void* data) {
    (void)data;
    auto* textInput = wl_container_of(
      listener, static_cast<TextInput*>(nullptr), enable);
    auto* relay = textInput->relay;
    if (relay == nullptr || relay->inputMethod_ == nullptr) {
      return;
    }
    if (relay->focusedSurface_ != nullptr &&
        sameClient(textInput->input, relay->focusedSurface_) &&
        textInput->input->focused_surface == nullptr) {
      wlr_text_input_v3_send_enter(textInput->input, relay->focusedSurface_);
    }
    wlr_input_method_v2_send_activate(relay->inputMethod_);
    relay->sendInputMethodState(textInput);
  };
  wl_signal_add(&input->events.enable, &entry->enable);

  entry->commit.notify = [](wl_listener* listener, void* data) {
    (void)data;
    auto* textInput = wl_container_of(
      listener, static_cast<TextInput*>(nullptr), commit);
    auto* relay = textInput->relay;
    if (relay == nullptr || relay->inputMethod_ == nullptr) {
      return;
    }
    if (textInput->input->focused_surface == nullptr ||
        !textInput->input->current_enabled) {
      return;
    }
    relay->sendInputMethodState(textInput);
  };
  wl_signal_add(&input->events.commit, &entry->commit);

  entry->disable.notify = [](wl_listener* listener, void* data) {
    (void)data;
    auto* textInput = wl_container_of(
      listener, static_cast<TextInput*>(nullptr), disable);
    auto* relay = textInput->relay;
    if (relay == nullptr || relay->inputMethod_ == nullptr) {
      return;
    }
    wlr_input_method_v2_send_deactivate(relay->inputMethod_);
    relay->sendInputMethodState(textInput);
  };
  wl_signal_add(&input->events.disable, &entry->disable);

  entry->destroy.notify = [](wl_listener* listener, void* data) {
    (void)data;
    auto* textInput = wl_container_of(
      listener, static_cast<TextInput*>(nullptr), destroy);
    auto* relay = textInput->relay;
    if (relay == nullptr) {
      return;
    }
    removeListener(textInput->enable);
    removeListener(textInput->commit);
    removeListener(textInput->disable);
    removeListener(textInput->destroy);
    relay->textInputs_.erase(
      std::remove_if(relay->textInputs_.begin(),
                     relay->textInputs_.end(),
                     [textInput](const std::unique_ptr<TextInput>& candidate) {
                       return candidate.get() == textInput;
                     }),
      relay->textInputs_.end());
  };
  wl_signal_add(&input->events.destroy, &entry->destroy);

  bool sentEnter = false;
  if (inputMethod_ != nullptr && focusedSurface_ != nullptr &&
      sameClient(input, focusedSurface_)) {
    wlr_text_input_v3_send_enter(input, focusedSurface_);
    sentEnter = true;
  }

  pid_t pid = 0;
  if (input->resource != nullptr && input->resource->client != nullptr) {
    wl_client_get_credentials(input->resource->client, &pid, nullptr, nullptr);
  }
  spdlog::info("IME: text input created (pid {}), resource={}, enter={}",
               pid,
               input->resource != nullptr ? wl_resource_get_id(input->resource) : 0,
               sentEnter);
  textInputs_.push_back(std::move(entry));
}

void
ImeRelay::handleNewInputMethod(wlr_input_method_v2* inputMethod)
{
  if (inputMethod == nullptr || inputMethod->seat != seat_) {
    return;
  }
  if (inputMethod_ != nullptr) {
    spdlog::warn("IME: rejecting a second input method on this seat");
    wlr_input_method_v2_send_unavailable(inputMethod);
    return;
  }
  inputMethod_ = inputMethod;

  imCommit_.notify = [](wl_listener* listener, void* data) {
    (void)data;
    auto* relay = wl_container_of(
      listener, static_cast<ImeRelay*>(nullptr), imCommit_);
    relay->handleImCommit();
  };
  wl_signal_add(&inputMethod_->events.commit, &imCommit_);

  imGrabKeyboard_.notify = [](wl_listener* listener, void* data) {
    auto* relay = wl_container_of(
      listener, static_cast<ImeRelay*>(nullptr), imGrabKeyboard_);
    relay->handleImGrab(
      static_cast<wlr_input_method_keyboard_grab_v2*>(data));
  };
  wl_signal_add(&inputMethod_->events.grab_keyboard, &imGrabKeyboard_);

  imDestroy_.notify = [](wl_listener* listener, void* data) {
    (void)data;
    auto* relay = wl_container_of(
      listener, static_cast<ImeRelay*>(nullptr), imDestroy_);
    relay->handleImDestroy();
  };
  wl_signal_add(&inputMethod_->events.destroy, &imDestroy_);

  imNewPopup_.notify = [](wl_listener* listener, void* data) {
    auto* relay = wl_container_of(
      listener, static_cast<ImeRelay*>(nullptr), imNewPopup_);
    relay->handleImPopup(static_cast<wlr_input_popup_surface_v2*>(data));
  };
  wl_signal_add(&inputMethod_->events.new_popup_surface, &imNewPopup_);

  spdlog::info("IME: input method connected");
  if (focusedSurface_ != nullptr) {
    for (auto& entry : textInputs_) {
      if (sameClient(entry->input, focusedSurface_)) {
        wlr_text_input_v3_send_enter(entry->input, focusedSurface_);
      }
    }
  }
}

void
ImeRelay::sendInputMethodState(TextInput* textInput)
{
  if (inputMethod_ == nullptr || textInput == nullptr ||
      textInput->input == nullptr) {
    return;
  }
  auto* input = textInput->input;
  if ((input->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT) !=
      0) {
    wlr_input_method_v2_send_surrounding_text(
      inputMethod_,
      input->current.surrounding.text != nullptr
        ? input->current.surrounding.text
        : "",
      input->current.surrounding.cursor,
      input->current.surrounding.anchor);
  }
  wlr_input_method_v2_send_text_change_cause(inputMethod_,
                                             input->current.text_change_cause);
  if ((input->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE) != 0) {
    wlr_input_method_v2_send_content_type(inputMethod_,
                                          input->current.content_type.hint,
                                          input->current.content_type.purpose);
  }
  wlr_input_method_v2_send_done(inputMethod_);
}

void
ImeRelay::handleImCommit()
{
  auto* textInput = focusedTextInput();
  if (textInput == nullptr || textInput->input == nullptr ||
      inputMethod_ == nullptr) {
    return;
  }
  auto* input = textInput->input;
  auto* im = inputMethod_;
  if (im->current.preedit.text != nullptr) {
    spdlog::debug("IME: preedit '{}'", im->current.preedit.text);
    wlr_text_input_v3_send_preedit_string(input,
                                          im->current.preedit.text,
                                          im->current.preedit.cursor_begin,
                                          im->current.preedit.cursor_end);
  } else if (im->current.commit_text != nullptr) {
    // Some clients (winit/alacritty) keep displaying the previous preedit
    // until it is explicitly cleared before the commit.
    wlr_text_input_v3_send_preedit_string(input, "", -1, -1);
  }
  if (im->current.commit_text != nullptr) {
    pid_t pid = 0;
    if (input->resource != nullptr && input->resource->client != nullptr) {
      wl_client_get_credentials(
        input->resource->client, &pid, nullptr, nullptr);
    }
    spdlog::info("IME: commit '{}' -> pid {}", im->current.commit_text, pid);
    wlr_text_input_v3_send_commit_string(input, im->current.commit_text);
  }
  if (im->current.delete_.before_length != 0 ||
      im->current.delete_.after_length != 0) {
    wlr_text_input_v3_send_delete_surrounding_text(
      input,
      im->current.delete_.before_length,
      im->current.delete_.after_length);
  }
  wlr_text_input_v3_send_done(input);
  // Deliver immediately: the IME commit arrives inside Wayland dispatch and the
  // client expects the text before the next roundtrip/frame.
  if (server_ != nullptr && server_->display != nullptr) {
    wl_display_flush_clients(server_->display);
  }
}

void
ImeRelay::handleImGrab(wlr_input_method_keyboard_grab_v2* grab)
{
  if (grab == nullptr || seat_ == nullptr) {
    return;
  }
  if (imGrab_ != nullptr) {
    removeListener(imGrabDestroy_);
  }
  imGrab_ = grab;
  imGrabDestroy_.notify = [](wl_listener* listener, void* data) {
    (void)data;
    auto* relay = wl_container_of(
      listener, static_cast<ImeRelay*>(nullptr), imGrabDestroy_);
    relay->handleImGrabDestroy();
  };
  wl_signal_add(&grab->events.destroy, &imGrabDestroy_);

  wlr_input_method_keyboard_grab_v2_set_keyboard(
    grab, wlr_seat_get_keyboard(seat_));

  seatGrab_.interface = &keyboardGrabImpl_;
  seatGrab_.seat = seat_;
  seatGrab_.data = this;
  wlr_seat_keyboard_start_grab(seat_, &seatGrab_);
  seatGrabActive_ = true;
  spdlog::info("IME: input method grabbed the keyboard");
}

void
ImeRelay::handleImGrabDestroy()
{
  removeListener(imGrabDestroy_);
  imGrab_ = nullptr;
  if (seatGrabActive_ && seat_ != nullptr) {
    wlr_seat_keyboard_end_grab(seat_);
    seatGrabActive_ = false;
    auto* keyboard = wlr_seat_get_keyboard(seat_);
    if (keyboard != nullptr) {
      wlr_seat_set_keyboard(seat_, keyboard);
      wlr_seat_keyboard_notify_modifiers(seat_, &keyboard->modifiers);
    }
  }
  spdlog::info("IME: input method released the keyboard");
}

void
ImeRelay::handleImDestroy()
{
  removeListener(imCommit_);
  removeListener(imGrabKeyboard_);
  removeListener(imDestroy_);
  removeListener(imNewPopup_);
  removeListener(imGrabDestroy_);
  imGrab_ = nullptr;
  inputMethod_ = nullptr;
  if (seatGrabActive_ && seat_ != nullptr) {
    wlr_seat_keyboard_end_grab(seat_);
    seatGrabActive_ = false;
  }
  for (auto& entry : textInputs_) {
    if (entry->input != nullptr && entry->input->focused_surface != nullptr) {
      wlr_text_input_v3_send_leave(entry->input);
    }
  }
  spdlog::info("IME: input method disconnected");
}

void
ImeRelay::handleImPopup(wlr_input_popup_surface_v2* popup)
{
  if (popup == nullptr || popup->surface == nullptr) {
    return;
  }
  auto entry = std::make_unique<Popup>();
  entry->relay = this;
  entry->popup = popup;

  entry->commit.notify = [](wl_listener* listener, void* data) {
    (void)data;
    auto* popup = wl_container_of(
      listener, static_cast<Popup*>(nullptr), commit);
    if (popup->relay != nullptr) {
      popup->relay->handlePopupCommit(popup);
    }
  };
  wl_signal_add(&popup->surface->events.commit, &entry->commit);

  entry->destroy.notify = [](wl_listener* listener, void* data) {
    (void)data;
    auto* popup = wl_container_of(
      listener, static_cast<Popup*>(nullptr), destroy);
    auto* relay = popup->relay;
    if (relay == nullptr) {
      return;
    }
    removeListener(popup->commit);
    removeListener(popup->destroy);
    if (popup->texture != 0) {
      relay->pendingTextureDeletes_.push_back(popup->texture);
      popup->texture = 0;
    }
    relay->popups_.erase(
      std::remove_if(relay->popups_.begin(),
                     relay->popups_.end(),
                     [popup](const std::unique_ptr<Popup>& candidate) {
                       return candidate.get() == popup;
                     }),
      relay->popups_.end());
  };
  wl_signal_add(&popup->events.destroy, &entry->destroy);

  popups_.push_back(std::move(entry));
  spdlog::info("IME: candidate popup created");
}

void
ImeRelay::handlePopupCommit(Popup* popup)
{
  if (popup == nullptr || popup->popup == nullptr ||
      popup->popup->surface == nullptr) {
    return;
  }
  popup->needsRead = true;
  popup->visible = popup->popup->surface->buffer != nullptr;
  if (!popup->visible) {
    popup->rgba.clear();
  }
}

void
ImeRelay::readPopupPixels(Popup* popup)
{
  auto* surface = popup->popup != nullptr ? popup->popup->surface : nullptr;
  if (surface == nullptr || surface->buffer == nullptr) {
    popup->visible = false;
    return;
  }
  const int width = surface->current.width;
  const int height = surface->current.height;
  if (width <= 0 || height <= 0) {
    popup->visible = false;
    return;
  }
  auto* texture = wlr_surface_get_texture(surface);
  if (texture == nullptr) {
    return;
  }

  popup->rgba.resize(static_cast<std::size_t>(width) * height * 4);
  uint32_t format = DRM_FORMAT_ARGB8888;
  wlr_texture_read_pixels_options options{
    popup->rgba.data(),
    format,
    static_cast<uint32_t>(width * 4),
    0,
    0,
    wlr_box{ 0, 0, width, height },
  };
  if (!wlr_texture_read_pixels(texture, &options)) {
    options.format = DRM_FORMAT_ABGR8888;
    if (!wlr_texture_read_pixels(texture, &options)) {
      return;
    }
    format = options.format;
  }
  // Normalize to RGBA so GLES2 does not need GL_BGRA. ARGB/XRGB buffers are
  // laid out as B,G,R,A in memory.
  if (format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888) {
    for (std::size_t i = 0; i + 2 < popup->rgba.size(); i += 4) {
      std::swap(popup->rgba[i], popup->rgba[i + 2]);
    }
  }
  popup->glFormat = GL_RGBA;

  // Wayland buffers are premultiplied; ImGui's blending expects straight
  // alpha. Popups are small, so the per-pixel conversion is cheap.
  if (hasAlpha(format)) {
    for (std::size_t i = 0; i + 3 < popup->rgba.size(); i += 4) {
      const unsigned int alpha = popup->rgba[i + 3];
      if (alpha == 0 || alpha == 255) {
        continue;
      }
      for (int c = 0; c < 3; ++c) {
        unsigned int value = popup->rgba[i + static_cast<std::size_t>(c)];
        value = (value * 255u + alpha / 2u) / alpha;
        popup->rgba[i + static_cast<std::size_t>(c)] =
          static_cast<std::uint8_t>(value > 255u ? 255u : value);
      }
    }
  }
  popup->width = width;
  popup->height = height;
  popup->dirty = true;
  static int popupLogs = 0;
  if (popupLogs < 3) {
    ++popupLogs;
    spdlog::info("IME: popup frame {}x{}", width, height);
  }
  popup->visible = true;
  popup->needsRead = false;
}

void
ImeRelay::renderOverlay()
{
  if (!pendingTextureDeletes_.empty()) {
    glDeleteTextures(static_cast<GLsizei>(pendingTextureDeletes_.size()),
                     pendingTextureDeletes_.data());
    pendingTextureDeletes_.clear();
  }
  if (popups_.empty()) {
    return;
  }

  ImGuiIO& io = ImGui::GetIO();
  float x = io.MousePos.x + 16.0f;
  float y = io.MousePos.y + 20.0f;
  if (io.MousePos.x < 0.0f || io.MousePos.y < 0.0f) {
    x = io.DisplaySize.x * 0.35f;
    y = io.DisplaySize.y * 0.6f;
  }

  const ImGuiWindowFlags flags =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
    ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
    ImGuiWindowFlags_NoBringToFrontOnFocus;

  for (auto& popup : popups_) {
    if (popup->needsRead && popup->visible) {
      readPopupPixels(popup.get());
    }
    if (!popup->visible || popup->rgba.empty() || popup->width <= 0 ||
        popup->height <= 0) {
      continue;
    }
    if (popup->texture == 0) {
      glGenTextures(1, &popup->texture);
      if (popup->texture == 0) {
        continue;
      }
      glBindTexture(GL_TEXTURE_2D, popup->texture);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (popup->dirty) {
      glBindTexture(GL_TEXTURE_2D, popup->texture);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      if (popup->uploadedWidth != popup->width ||
          popup->uploadedHeight != popup->height) {
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGBA,
                     popup->width,
                     popup->height,
                     0,
                     static_cast<GLenum>(popup->glFormat),
                     GL_UNSIGNED_BYTE,
                     popup->rgba.data());
        popup->uploadedWidth = popup->width;
        popup->uploadedHeight = popup->height;
      } else {
        glTexSubImage2D(GL_TEXTURE_2D,
                        0,
                        0,
                        0,
                        popup->width,
                        popup->height,
                        static_cast<GLenum>(popup->glFormat),
                        GL_UNSIGNED_BYTE,
                        popup->rgba.data());
      }
      popup->dirty = false;
    }

    const float width = static_cast<float>(popup->width);
    const float height = static_cast<float>(popup->height);
    const float posX = std::clamp(
      x, 8.0f, std::max(8.0f, io.DisplaySize.x - width - 8.0f));
    const float posY = std::clamp(
      y, 8.0f, std::max(8.0f, io.DisplaySize.y - height - 8.0f));
    ImGui::SetNextWindowPos(ImVec2(posX, posY), ImGuiCond_Always);
    ImGui::Begin("##hackmatrix_ime_candidates", nullptr, flags);
    ImGui::Image(
      (ImTextureID)(intptr_t)popup->texture, ImVec2(width, height));
    ImGui::End();
    y += height + 6.0f;
  }
}

void
ImeRelay::grabEnter(wlr_seat_keyboard_grab* grab,
                    wlr_surface* surface,
                    const uint32_t* keycodes,
                    size_t numKeycodes,
                    const wlr_keyboard_modifiers* modifiers)
{
  (void)grab;
  (void)surface;
  (void)keycodes;
  (void)numKeycodes;
  (void)modifiers;
}

void
ImeRelay::grabClearFocus(wlr_seat_keyboard_grab* grab)
{
  (void)grab;
}

void
ImeRelay::grabKey(wlr_seat_keyboard_grab* grab,
                  uint32_t timeMsec,
                  uint32_t key,
                  uint32_t state)
{
  auto* relay = static_cast<ImeRelay*>(grab != nullptr ? grab->data : nullptr);
  if (relay != nullptr && relay->imGrab_ != nullptr) {
    wlr_input_method_keyboard_grab_v2_send_key(
      relay->imGrab_, timeMsec, key, state);
  }
}

void
ImeRelay::grabModifiers(wlr_seat_keyboard_grab* grab,
                        const wlr_keyboard_modifiers* modifiers)
{
  auto* relay = static_cast<ImeRelay*>(grab != nullptr ? grab->data : nullptr);
  if (relay != nullptr && relay->imGrab_ != nullptr) {
    wlr_input_method_keyboard_grab_v2_send_modifiers(
      relay->imGrab_, const_cast<wlr_keyboard_modifiers*>(modifiers));
  }
}

void
ImeRelay::grabCancel(wlr_seat_keyboard_grab* grab)
{
  (void)grab;
}
