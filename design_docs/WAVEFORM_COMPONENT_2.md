<!--
SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
SPDX-License-Identifier: BSD-2-Clause
-->

# Design for Waveform Component (2)

Next, we are going to introduce the notion of audio *buffers*, which are how
sound are handled in real life in a realtime context.

There are several complications that come from moving to a buffered, realtime
scenario.

1. We must zero out the waveform until we have enough samples to fill the output
   bitmap.
2. The computation of the output arrays must be chunked.
3. We need to use a mutex, because the UI renders on the UI thread; meanwhile,
   the audio computations of the three 1D arrays takes place on the audio 
   thread.