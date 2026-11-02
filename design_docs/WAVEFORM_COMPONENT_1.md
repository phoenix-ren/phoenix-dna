<!--
SPDX-FileCopyrightText: 2026 Phoenix AI contributors
SPDX-License-Identifier: BSD-2-Clause
-->

# Design for Waveform Component

This document describes the functionality, and implementation approach, for
our waveform display.

## Complications in creating a Waveform Component

This is a complicated component because:

1. It involves realtime input.
2. It is animated.
3. It depends on unknown details, such as noise levels during the recording --
   requiring robust methods to ensure correct operation.
4. The length (in seconds) can vary widely, from only a few seconds, to 
   minutes. 
5. Meanwhile, the waveform display has fixed length of, say, `n_pixels`.

Due to the complexity involved in implementing this component, we will need 
tackle it step by step, starting with the simplest possible version of the 
component.

## Benchmark Images

Before we discuss how we implement the component, take a look at the `.png`
screenshots of *tenacity*'s rendered waveforms, which are in the 
`audio_prompt_samples` folder. The screenshots show:
- a short sample (several seconds)
- a zoomed-in (i.e. large image), 46 second "long" sample
- a zoomed-out (i.e. small image) 46 second, "long" sample

Note that:
- there are three colors: 
  1. background color
  2. a light blue color for the mean (absolute) level
  3. a darker blue color for the maximum and minimum levels
- the lighter color is sandwiched within the darker color, so the darker color
  is probably drawn first, then the lighter color, on top, using a painter's
  algorithm
- note that color 2 is symmetric - unsurprising given how it is calculated as
  a moving average of the absolute level.
- so, even though we are rendering a 2D bitmap, the mean absolute level can be 
  "captured" as a 1D array, whose length is simply the number of horizontal 
  pixels in our 2D output image.
- similarly, the darker color consists of two 1D arrays, both of the 
  same, aforementioned length
- we need two arrays, because the max and min levels **aren't** the same, i.e.
  the part of the wave showing the maximum and minimum levels isn't symmetric.
- a convenient way of calculating the mean we are referring to is by using an
  exponentially weighted moving average of the absolute value of the signal
- care must be taken in selecting the smoothing coefficient in our moving 
  average (the alpha parameter). Its value depends on the sampling frequency
- furthermore, we must be careful when performing moving average calculations 
  on the accumulator of the exponentially weighted moving average of the absolute
  level. we must clamp the accumulator to zero when it reaches some low threshold,
  in order to avoid denormalled computations, which incur a serious performance 
  penalty.
- the three 1D arrays together can easily be converted into 2D bitmap, which
  we can render to static `.png` files for human and AI inspection.

## Creating rendered waveforms for static files

In the same folder as the screenshots, we have three `.wav` files, of differing
lengths. We will begin by rendering three `.png` images with a bespoke C++
test program; one for each waveform.
