// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one miniaudio implementation unit of the voice chat engine. The backend selection and the
// MA_NO_* switches come from the mkw_miniaudio target (cmake/VoiceChat.cmake), so every unit that
// includes miniaudio.h sees the same configuration.

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
