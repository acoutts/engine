// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/keyboard_key_channel_handler.h"

#include <windows.h>

#ifdef WINUWP
#include "third_party/cppwinrt/generated/winrt/Windows.System.h"
#include "third_party/cppwinrt/generated/winrt/Windows.UI.Core.h"
#endif

#include <iostream>

#include "flutter/shell/platform/common/json_message_codec.h"

namespace flutter {

namespace {

static constexpr char kChannelName[] = "flutter/keyevent";

static constexpr char kKeyCodeKey[] = "keyCode";
static constexpr char kScanCodeKey[] = "scanCode";
static constexpr char kCharacterCodePointKey[] = "characterCodePoint";
static constexpr char kModifiersKey[] = "modifiers";
static constexpr char kKeyMapKey[] = "keymap";
static constexpr char kTypeKey[] = "type";
static constexpr char kHandledKey[] = "handled";

static constexpr char kWindowsKeyMap[] = "windows";
static constexpr char kKeyUp[] = "keyup";
static constexpr char kKeyDown[] = "keydown";

// The maximum number of pending events to keep before
// emitting a warning on the console about unhandled events.
static constexpr int kMaxPendingEvents = 1000;

// The bit for a scancode indicating the key is extended.
//
// Win32 defines some keys to be "extended", such as ShiftRight, which shares
// the same scancode as its non-extended counterpart, such as ShiftLeft.  In
// Chromium's scancode table, from which Flutter's physical key list is
// derived, these keys are marked with this bit.  See
// https://chromium.googlesource.com/codesearch/chromium/src/+/refs/heads/master/ui/events/keycodes/dom/dom_code_data.inc
static constexpr int kScancodeExtended = 0xe000;

// Re-definition of the modifiers for compatibility with the Flutter framework.
// These have to be in sync with the framework's RawKeyEventDataWindows
// modifiers definition.
// https://github.com/flutter/flutter/blob/19ff596979e407c484a32f4071420fca4f4c885f/packages/flutter/lib/src/services/raw_keyboard_windows.dart#L203
static constexpr int kShift = 1 << 0;
static constexpr int kShiftLeft = 1 << 1;
static constexpr int kShiftRight = 1 << 2;
static constexpr int kControl = 1 << 3;
static constexpr int kControlLeft = 1 << 4;
static constexpr int kControlRight = 1 << 5;
static constexpr int kAlt = 1 << 6;
static constexpr int kAltLeft = 1 << 7;
static constexpr int kAltRight = 1 << 8;
static constexpr int kWinLeft = 1 << 9;
static constexpr int kWinRight = 1 << 10;
static constexpr int kCapsLock = 1 << 11;
static constexpr int kNumLock = 1 << 12;
static constexpr int kScrollLock = 1 << 13;

/// Calls GetKeyState() an all modifier keys and packs the result in an int,
/// with the re-defined values declared above for compatibility with the Flutter
/// framework.
int GetModsForKeyState() {
#ifdef WINUWP
  using namespace winrt::Windows::System;
  using namespace winrt::Windows::UI::Core;

  auto window = CoreWindow::GetForCurrentThread();

  auto key_is_down = [&window](VirtualKey key) {
    auto state = window.GetKeyState(key);
    return (state & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down;
  };

  int mods = 0;

  if (key_is_down(VirtualKey::Shift))
    mods |= kShift;
  if (key_is_down(VirtualKey::LeftShift))
    mods |= kShiftLeft;
  if (key_is_down(VirtualKey::RightShift))
    mods |= kShiftRight;
  if (key_is_down(VirtualKey::Control))
    mods |= kControl;
  if (key_is_down(VirtualKey::LeftControl))
    mods |= kControlLeft;
  if (key_is_down(VirtualKey::RightControl))
    mods |= kControlRight;
  if (key_is_down(VirtualKey::Menu))
    mods |= kAlt;
  if (key_is_down(VirtualKey::LeftMenu))
    mods |= kAltLeft;
  if (key_is_down(VirtualKey::RightMenu))
    mods |= kAltRight;
  if (key_is_down(VirtualKey::LeftWindows))
    mods |= kWinLeft;
  if (key_is_down(VirtualKey::RightWindows))
    mods |= kWinRight;
  if (key_is_down(VirtualKey::CapitalLock))
    mods |= kCapsLock;
  if (key_is_down(VirtualKey::NumberKeyLock))
    mods |= kNumLock;
  if (key_is_down(VirtualKey::Scroll))
    mods |= kScrollLock;
  return mods;
#else
  int mods = 0;

  if (GetKeyState(VK_SHIFT) < 0)
    mods |= kShift;
  if (GetKeyState(VK_LSHIFT) < 0)
    mods |= kShiftLeft;
  if (GetKeyState(VK_RSHIFT) < 0)
    mods |= kShiftRight;
  if (GetKeyState(VK_CONTROL) < 0)
    mods |= kControl;
  if (GetKeyState(VK_LCONTROL) < 0)
    mods |= kControlLeft;
  if (GetKeyState(VK_RCONTROL) < 0)
    mods |= kControlRight;
  if (GetKeyState(VK_MENU) < 0)
    mods |= kAlt;
  if (GetKeyState(VK_LMENU) < 0)
    mods |= kAltLeft;
  if (GetKeyState(VK_RMENU) < 0)
    mods |= kAltRight;
  if (GetKeyState(VK_LWIN) < 0)
    mods |= kWinLeft;
  if (GetKeyState(VK_RWIN) < 0)
    mods |= kWinRight;
  if (GetKeyState(VK_CAPITAL) < 0)
    mods |= kCapsLock;
  if (GetKeyState(VK_NUMLOCK) < 0)
    mods |= kNumLock;
  if (GetKeyState(VK_SCROLL) < 0)
    mods |= kScrollLock;
  return mods;
#endif
}

// Revert the "character" for a dead key to its normal value, or the argument
// unchanged otherwise.
//
// When a dead key is pressed, the WM_KEYDOWN's lParam is mapped to a special
// value: the "normal character" | 0x80000000.  For example, when pressing
// "dead key caret" (one that makes the following e into ê), its mapped
// character is 0x8000005E. "Reverting" it gives 0x5E, which is character '^'.
uint32_t _UndeadChar(uint32_t ch) {
  return ch & ~0x80000000;
}

}  // namespace

KeyboardKeyChannelHandler::KeyboardKeyChannelHandler(
    flutter::BinaryMessenger* messenger)
    : channel_(
          std::make_unique<flutter::BasicMessageChannel<rapidjson::Document>>(
              messenger,
              kChannelName,
              &flutter::JsonMessageCodec::GetInstance())) {}

KeyboardKeyChannelHandler::~KeyboardKeyChannelHandler() = default;

void KeyboardKeyChannelHandler::KeyboardHook(
    int key,
    int scancode,
    int action,
    char32_t character,
    bool extended,
    bool was_down,
    std::function<void(bool)> callback) {
  // TODO: Translate to a cross-platform key code system rather than passing
  // the native key code.
  rapidjson::Document event(rapidjson::kObjectType);
  auto& allocator = event.GetAllocator();
  event.AddMember(kKeyCodeKey, key, allocator);
  event.AddMember(kScanCodeKey, scancode | (extended ? kScancodeExtended : 0),
                  allocator);
  event.AddMember(kCharacterCodePointKey, _UndeadChar(character), allocator);
  event.AddMember(kKeyMapKey, kWindowsKeyMap, allocator);
  event.AddMember(kModifiersKey, GetModsForKeyState(), allocator);

  switch (action) {
    case WM_KEYDOWN:
      event.AddMember(kTypeKey, kKeyDown, allocator);
      break;
    case WM_KEYUP:
      event.AddMember(kTypeKey, kKeyUp, allocator);
      break;
    default:
      std::cerr << "Unknown key event action: " << action << std::endl;
      callback(false);
      return;
  }
  channel_->Send(event, [callback = std::move(callback)](const uint8_t* reply,
                                                         size_t reply_size) {
    auto decoded = flutter::JsonMessageCodec::GetInstance().DecodeMessage(
        reply, reply_size);
    bool handled = (*decoded)[kHandledKey].GetBool();
    callback(handled);
  });
}

}  // namespace flutter
