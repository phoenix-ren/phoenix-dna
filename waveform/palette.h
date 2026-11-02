// SPDX-FileCopyrightText: 2026 Phoenix AI contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QRgb>

/*!
 * \brief The waveform palette, shared by every renderer of the component.
 *
 * Sampled from the tenacity benchmark screenshots in audio_prompt_samples/
 * (see design_docs/WAVEFORM_COMPONENT_1.md): background, then the darker
 * blue for the min/max envelope, then the lighter blue for the mean
 * |level| band painted on top of it (painter's algorithm).
 */
namespace WaveformPalette
{
constexpr QRgb kBackground = qRgb(38, 38, 38);   // #262626
constexpr QRgb kPeakColor = qRgb(32, 126, 197);  // #207EC5 — min/max envelope
constexpr QRgb kMeanColor = qRgb(62, 154, 224);  // #3E9AE0 — mean |level| band
}
