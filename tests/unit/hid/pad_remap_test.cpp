/**
 * @file        tests/unit/hid/pad_remap_test.cpp
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/hid/pad.h>

#include "hid/input_internal.h"

using rex::hid::detail::ParsePadRemap;
using rex::hid::detail::RemapButtons;

REX_TEST_CASE("Pad remap is the identity without pairs", "[hid][remap]") {
  const auto remap = ParsePadRemap("");
  CHECK(RemapButtons(rex::hid::kPadA | rex::hid::kPadStart, remap) ==
        (rex::hid::kPadA | rex::hid::kPadStart));
}

REX_TEST_CASE("Pad remap swaps buttons and keeps the others", "[hid][remap]") {
  const auto remap = ParsePadRemap("A=B, b=a");
  CHECK(RemapButtons(rex::hid::kPadA, remap) == rex::hid::kPadB);
  CHECK(RemapButtons(rex::hid::kPadB, remap) == rex::hid::kPadA);
  CHECK(RemapButtons(rex::hid::kPadA | rex::hid::kPadB, remap) ==
        (rex::hid::kPadA | rex::hid::kPadB));
  CHECK(RemapButtons(rex::hid::kPadX | rex::hid::kPadDPadUp, remap) ==
        (rex::hid::kPadX | rex::hid::kPadDPadUp));
}

REX_TEST_CASE("Pad remap sends two buttons to one", "[hid][remap]") {
  const auto remap = ParsePadRemap("LB=Y,RB=Y");
  CHECK(RemapButtons(rex::hid::kPadLeftShoulder, remap) == rex::hid::kPadY);
  CHECK(RemapButtons(rex::hid::kPadLeftShoulder | rex::hid::kPadRightShoulder, remap) ==
        rex::hid::kPadY);
}

REX_TEST_CASE("Pad remap skips unknown names and malformed pairs", "[hid][remap]") {
  const auto remap = ParsePadRemap("A=Q,Z=B,X,,=Y,Start=Back");
  CHECK(RemapButtons(rex::hid::kPadA, remap) == rex::hid::kPadA);
  CHECK(RemapButtons(rex::hid::kPadX, remap) == rex::hid::kPadX);
  CHECK(RemapButtons(rex::hid::kPadStart, remap) == rex::hid::kPadBack);
}
