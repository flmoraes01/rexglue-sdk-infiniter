/**
 * @file        hid/merge.cpp
 * @brief       Merging the pads of every driver
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <bit>
#include <cctype>
#include <string_view>
#include <cmath>
#include <cstdlib>

#include "hid/input_internal.h"

namespace rex::hid::detail {

namespace {

constexpr f64 kThumbstickMax = 32767.0;

i16 LargerMagnitude(i16 a, i16 b) {
  return std::abs(static_cast<int>(a)) >= std::abs(static_cast<int>(b)) ? a : b;
}

bool PastDeadzone(i16 axis) {
  return std::abs(static_cast<int>(axis)) >= kThumbDeadzone;
}

void ApplyRadial(f64 percentage, i16& axis_x, i16& axis_y) {
  if (percentage <= 0.0 || percentage >= 1.0) {
    return;
  }
  const f64 deadzone = kThumbstickMax * percentage;
  const f64 theta = std::atan2(static_cast<f64>(axis_y), static_cast<f64>(axis_x));
  const f64 cutoff_x = std::cos(theta) * deadzone;
  const f64 cutoff_y = std::sin(theta) * deadzone;
  if (axis_x > -cutoff_x && axis_x < cutoff_x) {
    axis_x = 0;
  }
  if (axis_y > -cutoff_y && axis_y < cutoff_y) {
    axis_y = 0;
  }
}

}  // namespace

void MergeInto(rex::hid::PadState& dst, const rex::hid::PadState& src) {
  dst.buttons = static_cast<u16>(dst.buttons | src.buttons);
  dst.left_trigger = std::max(dst.left_trigger, src.left_trigger);
  dst.right_trigger = std::max(dst.right_trigger, src.right_trigger);
  dst.thumb_lx = LargerMagnitude(dst.thumb_lx, src.thumb_lx);
  dst.thumb_ly = LargerMagnitude(dst.thumb_ly, src.thumb_ly);
  dst.thumb_rx = LargerMagnitude(dst.thumb_rx, src.thumb_rx);
  dst.thumb_ry = LargerMagnitude(dst.thumb_ry, src.thumb_ry);
}

bool IsNeutral(const rex::hid::PadState& state) {
  if (state.buttons != 0) {
    return false;
  }
  if (state.left_trigger >= kTriggerThreshold || state.right_trigger >= kTriggerThreshold) {
    return false;
  }
  return !PastDeadzone(state.thumb_lx) && !PastDeadzone(state.thumb_ly) &&
         !PastDeadzone(state.thumb_rx) && !PastDeadzone(state.thumb_ry);
}

PadRemap ParsePadRemap(std::string_view text) {
  struct Name {
    std::string_view name;
    u16 bit;
  };
  static constexpr Name kNames[] = {
      {"A", rex::hid::kPadA},
      {"B", rex::hid::kPadB},
      {"X", rex::hid::kPadX},
      {"Y", rex::hid::kPadY},
      {"LB", rex::hid::kPadLeftShoulder},
      {"RB", rex::hid::kPadRightShoulder},
      {"Back", rex::hid::kPadBack},
      {"Start", rex::hid::kPadStart},
      {"LS", rex::hid::kPadLeftThumb},
      {"RS", rex::hid::kPadRightThumb},
      {"Up", rex::hid::kPadDPadUp},
      {"Down", rex::hid::kPadDPadDown},
      {"Left", rex::hid::kPadDPadLeft},
      {"Right", rex::hid::kPadDPadRight},
  };
  auto find = [](std::string_view name) -> u16 {
    while (!name.empty() && name.front() == ' ') {
      name.remove_prefix(1);
    }
    while (!name.empty() && name.back() == ' ') {
      name.remove_suffix(1);
    }
    for (const Name& entry : kNames) {
      if (entry.name.size() == name.size() &&
          std::equal(name.begin(), name.end(), entry.name.begin(),
                     [](char a, char b) { return std::tolower(a) == std::tolower(b); })) {
        return entry.bit;
      }
    }
    return 0;
  };
  PadRemap remap;
  for (u32 i = 0; i < remap.size(); ++i) {
    remap[i] = static_cast<u16>(1u << i);
  }
  while (!text.empty()) {
    const size_t comma = text.find(',');
    const std::string_view pair = text.substr(0, comma);
    text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    const size_t equals = pair.find('=');
    if (equals == std::string_view::npos) {
      continue;
    }
    const u16 from = find(pair.substr(0, equals));
    const u16 to = find(pair.substr(equals + 1));
    if (from && to) {
      remap[std::countr_zero(from)] = to;
    }
  }
  return remap;
}

u16 RemapButtons(u16 buttons, const PadRemap& remap) {
  u16 out = 0;
  for (u32 i = 0; i < remap.size(); ++i) {
    if (buttons & (1u << i)) {
      out = static_cast<u16>(out | remap[i]);
    }
  }
  return out;
}

void ApplyDeadzones(rex::hid::PadState& state, f64 left, f64 right) {
  ApplyRadial(left, state.thumb_lx, state.thumb_ly);
  ApplyRadial(right, state.thumb_rx, state.thumb_ry);
}

bool IsSamePad(const rex::hid::PadIdentity& a, const rex::hid::PadIdentity& b) {
  if (a.xinput_slot >= 0 && b.xinput_slot >= 0) {
    return a.xinput_slot == b.xinput_slot;
  }
  return !a.serial.empty() && !b.serial.empty() && a.vendor == b.vendor && a.product == b.product &&
         a.serial == b.serial;
}

void ActiveDeviceTracker::Observe(u32 user, const DeviceKey& key, const rex::hid::PadState& state) {
  if (user >= kMaxUsers || IsNeutral(state)) {
    return;
  }
  active_[user] = key;
  has_[user] = true;
}

bool ActiveDeviceTracker::GetActive(u32 user, DeviceKey& out) const {
  if (user >= kMaxUsers || !has_[user]) {
    return false;
  }
  out = active_[user];
  return true;
}

void ActiveDeviceTracker::Forget(const DeviceKey& key) {
  for (u32 user = 0; user < kMaxUsers; ++user) {
    if (has_[user] && active_[user] == key) {
      has_[user] = false;
    }
  }
}

}  // namespace rex::hid::detail
