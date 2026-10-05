/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <map>

// joan's FTM4 driver has no swipe_available node to ask, and its
// swipe_enable takes a single 0 or 1 for all four directions, so the
// gestures are listed here and switched on together from init.joan.rc.
// The keycodes are the KEY_GESTURE_SLIDE_* the lge_touch core reports.
static const std::map<int32_t, GestureInfo> device_kGestureInfoMap = {
        {0, {0, 249, "Swipe Up"}},
        {1, {1, 250, "Swipe Down"}},
        {2, {2, 251, "Swipe Right"}},
        {3, {3, 252, "Swipe Left"}},
};
