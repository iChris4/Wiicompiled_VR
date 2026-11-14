/* SPDX-License-Identifier: BSD-3-Clause
 *
 * RNNoise v0.2's vec.h and vec_neon.h include "os_support.h", which that release does not ship
 * (it lives in Opus, where the vector code came from). The only name they use from it is
 * OPUS_CLEAR, RNNoise's own RNN_CLEAR. Same shim as MKW-VoiceChat's third_party/rnnoise.
 */
#ifndef MKW_RNNOISE_OS_SUPPORT_H
#define MKW_RNNOISE_OS_SUPPORT_H

#include "common.h"

#ifndef OPUS_CLEAR
#define OPUS_CLEAR RNN_CLEAR
#endif

#endif
