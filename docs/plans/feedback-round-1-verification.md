# Feedback round 1 — what to verify on a real machine

The changes from this round were exercised in the browser layout harness, in
unit tests, and with synthetic videos on the engine. These points need a real
Tauri window, real sources, or a second pair of eyes. Windows with CUDA is the
main target.

## Needs a real window (could not be exercised in the harness)

- Media: dragging the timeline no longer shows "Drop to import media";
  dropping a real file still imports it.
- Media: Shift+←/→ jumps keyframes after dragging the timeline, after clicking
  a filmstrip thumbnail (several jumps in a row), and after clicking the
  viewport. The selected frame stays centred in the filmstrip.
- Jobs bar: a failed job shows its reason.
- Resolution Test → "Continue to Kernel Search" and Kernel Search → "Continue
  to Check" appear after applying, and navigate.
- Right-click on a run in the Runs selector opens the delete confirmation (not
  the webview's own context menu).

## Needs real sources

- Linear light: on the titles known to have been resized in linear light, does
  the matching curve drop the Resolution Test valley clearly below the
  as-encoded run? On synthetic 8-bit video the gap was only about 5× (10-bit:
  about 20×), because quantisation sets the floor.
- Check with a Recipe that carries a curve: values are in line with the
  Resolution Test / Kernel Search results that produced the Recipe.
- Non-proportional descale: pick a height in H-only, a width in W-only, Apply
  with "Also set the src …" checked; then confirm Kernel Search and Check use
  that width (the run summary and Recipe strip show it).
- Blur sweep (e.g. 0.8 → 1.2 step 0.05) on a source believed to use a blurred
  kernel.
- A broad fractional scan (500–1000 px @ 0.1, even base), then Apply: the
  dialog should open with the even base and offer the scan's kernel.

## Behaviour changes to be aware of

- Results hidden as "incompatible" now also cover runs measured with a
  different transfer curve.
- Recipe auto-names render thirds as fractions (`b=1/3`).
- The Runs selector's trash button deletes the runs it lists, not every run of
  that test; Results has per-test delete buttons for that.
- Parameter drafts (range, step, base modes, kernels, scan list) are saved with
  the project.
- "Algorithm Test" is "Kernel Search" in English; the Chinese label is
  unchanged.
- Studio-range video is now analysed on nominal 0..1 samples on every
  backend. Errors from earlier builds without a transfer curve are about 1.17×
  lower; those Runs are hidden from overlays and need re-running to compare.

## Known, deliberately not addressed

- Vulkan Video decode of H.264 that is all-IDR and high bitrate can return
  corrupted frames (FFmpeg/driver level; reproduced without GetNative code).
  CUDA/NVDEC is unaffected.
