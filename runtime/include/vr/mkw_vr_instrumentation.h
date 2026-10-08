// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

struct CpuContext;

namespace mkw::vr {

// Announces the capabilities supplied by the translated entry observers. It
// is harmless to call repeatedly and does not enable presentation by itself.
void MkwVRInstrumentationInitialize() noexcept;

// Latches the first-person state for the current guest frame with the frame's
// race camera, exactly as the ScnMgrRace::Draw observer does, for a caller
// that runs earlier in the frame (the character body's ScnRoot::CalcView
// wrapper). Does nothing before RaceCamera::Update has run this frame.
void MkwVRFirstPersonUpdateForCurrentFrame() noexcept;

} // namespace mkw::vr

// Called only from translator-generated PAL RMCP01 function entries. It is a
// read-only observer, not a native replacement: the original translated body
// still runs, and the const context contract is part of translator correctness.
extern "C" void MkwVRObserveTranslatedFunctionEntry(uint32_t address,
                                                      const CpuContext* context) noexcept;
