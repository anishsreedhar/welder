# Keyer updates in Welder 0.2.6

The Keyer converts the selected input layer into a matte and writes it to the selected output channel. A new Keyer uses RGB, writes to `rgba.alpha` in Replace mode, and starts with a linear range of A = 0, B = 1, C = 1, and D = 1. Highlights above 1 stay opaque at this default range. Separate C and D to make a falling ramp and cut off values above D.


Saved `.weld` Keyers without a `response` field retain the original response: smooth rising/falling ramps, the original luminance weights, and zero alpha beyond D (including values above 1 at the default range). Loading writes an explicit `response: "legacy"` marker so subsequent saves and duplicates keep that behavior. New nodes and imported Nuke Keyers use `response: "nuke"`; their linear response and opaque default highlights survive save/reopen. This compatibility field does not change the application version.

![Nuke and updated Welder Keyer panels with before-and-after alpha measurements](../Welder%20Keyer%20update.png)

## Luminance alpha comparison

New Keyers use the luminance weights measured from Nuke 17.1v2's Keyer. The largest previous discrepancy in the test set was 0.0002 alpha on pure green.

| Input | Welder before | Welder after | Nuke 17.1v2 |
| --- | ---: | ---: | ---: |
| Pure red | 0.2126 | 0.2125 | 0.2125 |
| Pure green | 0.7152 | 0.7154 | 0.7154 |
| Pure blue | 0.0722 | 0.0721 | 0.0721 |

Rendered float alpha matched to nine decimal places on all ten synthetic inputs in our comparison. The cases covered black, gray, white, pure RGB, HDR highlights, negative input, a point on the falling ramp, and a point beyond an explicit cutoff. The screenshots show the Keyer panels. The numerical comparison comes from the separate synthetic renders.

## Controls and workflow

- Use the input layer menu and RGB toggles to choose what drives the matte. **Other layers** includes standard presets and layers found in the connected image.
- Choose an existing channel or create one with **New output channel**. Unchecking output bypasses the matte write and preserves the selected channel for re-enabling. **Replace**, **Intersect**, **Union**, and **Invert** control the result.
- Use the **⋯** menus to link input or output settings to another Keyer. Removing a link keeps the effective values.
- Drag the A–D handles or edit the range fields. For new Keyers, the graph shows the linear response and follows the current frame's keyframed range. Use **+**, **−**, and **Fit** to adjust the graph view. Right-click the panel for range animation, reset, revert, user knobs, and node help.
- The **Node** tab includes label formatting, visibility, cache priority, Dope Sheet, bookmarks, postage stamps, and lifetime controls. The graph tile shows the operation and output channel.
- The `.nk` importer reads Keyer operation, input, output, combine, invert, and range settings. The `.nk` exporter writes the corresponding Keyer controls.

Open [Keyer.weld](../Keyer.weld) in Welder and connect a source to Keyer1. Select **A** in the Viewer to inspect the matte.
