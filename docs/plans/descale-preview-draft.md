# Descale / rescale preview — draft plan

Status: draft, not scheduled. Written 2026-10-05 after the first external
feedback round; nothing here is implemented.

## Why

The feedback's last point: after GetNative VF proposes a resolution and kernel,
the user still copies the numbers into a `.vpy` script to *look* at the descale,
because a low numerical error does not rule out visible artifacts (ringing,
haloing with some Bicubic kernels). Until the result can be inspected in the
app, the workflow is not complete.

## Two stages

### Stage A — "Copy VapourSynth snippet" (small, no engine work)

A button on the Recipe (summary strip and review dialog) that copies a ready
`descale` call built from what the Recipe already stores:

- geometry: canvas width/height, `src_left`, `src_top`, `src_width`,
  `src_height`, base width/height;
- kernel: id and parameters (`b`, `c`, `taps`, `blur`);
- transfer curve, as a comment or a linearise/delinearise wrapper when not
  `none`.

Open points:

- Which plugin API to target (`core.descale.Decustom`/`Debicubic` vs.
  `vskernels`/`vodesfunc`). The snippet must reproduce the engine's geometry
  exactly, including fractional `src_*` and the base-size parity rule.
- Whether to emit the rescale + difference lines too, so the snippet is
  directly viewable.
- A conformance check: run the emitted snippet against the engine on a fixture
  and compare errors (the repo already carries `upstream/` reference checkouts).

Estimate: under a day including the snippet test.

### Stage B — built-in preview (engine work)

Worker protocol v1 has no command that returns a descaled or rescaled image;
`media_preview_begin` only renders the decoded source frame.

Engine:

- New command, e.g. `preview_recipe_begin`: one frame (asset or media
  reference) + one Recipe (geometry, kernel, transfer) → PNG assets for
  `descaled`, `rescaled`, and `difference` (absolute error, optionally
  amplified), plus the frame's error value.
- CPU first: `descale_2d_f32` and `reconstruct_2d_f32` already exist and are
  what the metric is computed from, so the preview shows exactly what was
  measured. GPU backends are not needed for a single frame.
- Luma only to start; chroma handling is a separate decision (see below).
- Reuse the media asset cache naming and eviction.

App:

- Check page: a preview pane next to the frame error curve. Selecting a frame
  (curve point, Top-N row) loads source / rescaled / difference with a toggle
  or a split slider, at 1:1 with pan and zoom.
- Kernel Search and Resolution Test: "Preview" on the selected candidate, using
  the same command with an ad-hoc recipe.

Open points:

- Luma-only vs. full colour. Luma shows ringing clearly and needs no colour
  pipeline; full colour needs matrix/range/transfer handling the engine does
  not have today.
- Difference scaling: fixed gain, auto-range, or log.
- Whether the preview should also show the descaled native-resolution image
  upscaled with a neutral kernel for side-by-side viewing.

Estimate: several days; engine command and tests first, then UI.

## Related decision to make first: sample range convention

Found while adding transfer curves. It affects what a preview should display
and every stored error value, so it should be settled before Stage B.

- Software decode, NVDEC/CUDA and Vulkan produce F32 luma on the code scale
  (`code * 2^(16-depth) / 65535`; 8-bit black = 16/256).
- VideoToolbox/Metal stretches studio range to nominal 0..1 (16..235 → 0..1).
- Consequence: with no transfer curve, the same video measures 255.996 / 219 ≈
  1.169× higher on Metal, and the pixel-exclusion threshold means a different
  thing. With a curve, all four paths stretch and agree exactly (measured).
- Evidence about intent: upstream `getnative` feeds a `GRAYS` clip, which zimg
  produces on the nominal scale; the Metal conformance test asserts the
  stretched values; the CUDA kernel receives `limited_range` but ignores it
  (`(void)limited_range;`). So the nominal scale looks like the intended
  convention, implemented on Metal only.

Options:

1. Make software/CUDA/Vulkan stretch too (matches upstream and the Metal
   test). Every existing result shifts by 1.169× and old runs must be marked
   incompatible rather than overlaid; cached F32 assets need a new name.
2. Make Metal stop stretching (smallest change; existing CUDA results stay
   valid), accepting a constant offset from upstream's numbers.

Not changed in this round because either option reverses something that was
written deliberately.
