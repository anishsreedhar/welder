# ColorCorrect

The ColorCorrect tab has master, shadows, midtones, and highlights sections.
Each section provides saturation, contrast, gamma, gain, and offset.

- A compact value changes all four components. Click **4** for separate
  R/G/B/A sliders. Collapsing with **1** keeps their values; editing the compact
  value then sets them all to the entered value.
- Correction sliders use numbered ticks and a vertical rectangular handle.
  The default sits at the center: 1 for saturation, contrast, gamma and gain;
  0 for offset. Left decreases and right increases, with negative values
  available on every correction slider. Double-click the slider to reset it.
  Negative gamma uses the reciprocal power on positive pixels; gamma 0 leaves
  the gamma operation unchanged to avoid division by zero.
- The rainbow button opens an embedded color wheel beside the channel sliders.
  The swatch opens the full color picker. These expansion states are saved in
  the comp and the supplied `.nk` `*_panelDropped` settings are imported.
- Value fields, sliders, and wheels support undo. The diamond opens the
  animation menu; Alt-clicking a value sets a key. Expressions use the existing
  numeric-knob expression controls.
- The channels row selects the layer and components to affect. Other channels
  pass through, including when saturation or unpremultiplication reads RGB.

The **Ranges** tab provides:

- Shadow and highlight curves: click to add points, drag to move them, or
  double-click a point to remove it. Reset restores the selected curve.
  Shift-drag constrains movement horizontally; Ctrl-drag constrains it vertically.
- A read-only midtone curve, calculated as `1 - shadow - highlight`.
- Test mode: black, quarter-grey, and white identify the dominant range;
  green and magenta indicate a transition between ranges.
- A mask channel menu, enable checkbox, inject to `mask.a`, invert, and fringe.
  A newly connected mask uses alpha unless a channel was explicitly selected.
- `(un)premult by`: divide by the selected channel before correction and
  multiply by the original channel afterwards. Zero alpha stays black.
- Mix luminance and its enable checkbox, plus the final effect mix slider.

Contrast uses a 0.18 pivot. Saturation and tonal weights use the classic
0.2125/0.7154/0.0721 luminance coefficients. Luminance mixing uses the classic
sRGB-encoded, R/G/B-ordered calculation. Representative numerical values were
checked against a separate Nuke 15.0v5 process, including HDR, partial luminance
mix, contrast, correction order, alpha, and range diagnostics.

The **Node** tab retains Welder's common label, font size, tile color, hide-input,
disable, and lifetime controls.

`.nk` import preserves correction values, animation, range control points,
mask and luminance settings, and expanded wheels. Imported custom range tangents
use Welder's smooth monotone interpolation; the importer reports that approximation.
Export samples the range curves and reports that sampling. Exact Welder-to-Welder
clipboard copies retain the original data.

Open `examples/colorcorrect.weld` for a connected example. The focused rendering
checks are registered as the CTest `colorcorrect` test; importer regressions are
in `tests/test_nk2strata.py`.
