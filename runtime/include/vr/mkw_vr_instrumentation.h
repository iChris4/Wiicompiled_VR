// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

struct CpuContext;

namespace mkw::vr {

// Announces the capabilities supplied by the translated entry observers. It
// is harmless to call repeatedly and does not enable presentation by itself.
void MkwVRInstrumentationInitialize() noexcept;

} // namespace mkw::vr

// Called only from translator-generated PAL RMCP01 function entries. It is a
// read-only observer, not a native replacement: the original translated body
// still runs, and the const context contract is part of translator correctness.
extern "C" void MkwVRObserveTranslatedFunctionEntry(uint32_t address,
                                                      const CpuContext* context) noexcept;
