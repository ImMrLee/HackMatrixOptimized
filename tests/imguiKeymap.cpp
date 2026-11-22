// Unit tests for the XKB keysym -> ImGuiKey mapping used by the debug overlay.

#include <gtest/gtest.h>

#include "imgui_keymap.h"

TEST(ImguiKeymap, LowercaseLetters)
{
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_a), ImGuiKey_A);
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_m), ImGuiKey_M);
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_z), ImGuiKey_Z);
}

TEST(ImguiKeymap, UppercaseLettersMapLikeLowercase)
{
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_A), ImGuiKey_A);
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_Z), ImGuiKey_Z);
}

TEST(ImguiKeymap, Digits)
{
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_0), ImGuiKey_0);
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_5), ImGuiKey_5);
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_9), ImGuiKey_9);
}

TEST(ImguiKeymap, SpecialKeys)
{
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_Return), ImGuiKey_Enter);
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_Escape), ImGuiKey_Escape);
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_BackSpace), ImGuiKey_Backspace);
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_Tab), ImGuiKey_Tab);
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_Left), ImGuiKey_LeftArrow);
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_Right), ImGuiKey_RightArrow);
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_Up), ImGuiKey_UpArrow);
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_Down), ImGuiKey_DownArrow);
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_space), ImGuiKey_Space);
}

TEST(ImguiKeymap, ModifierKeys)
{
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_Shift_L), ImGuiKey_LeftShift);
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_Control_L), ImGuiKey_LeftCtrl);
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_Alt_L), ImGuiKey_LeftAlt);
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_Super_L), ImGuiKey_LeftSuper);
}

TEST(ImguiKeymap, UnknownKeysAreNone)
{
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_NoSymbol), ImGuiKey_None);
  EXPECT_EQ(keysymToImGuiKey(XKB_KEY_F35), ImGuiKey_None);
}
