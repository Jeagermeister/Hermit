# Hermit logo assets

A hermit crab carrying a terminal window as its shell. The pun is load-bearing rather than
decorative: a hermit crab carries a shell it did not grow, and Hermit wraps a local model in a
confined **shell** — a portable authority boundary that *reduces* authority instead of inheriting
it. The `++` eyes carry the language; the shell carries the terminal.

## Palette

Taken from the artwork as generated, not from the brief — the ground came out lighter than the
`#14171C` originally specified, and the file is internally consistent at this value.

| role | hex |
|---|---|
| ground | `#2C2D32` |
| body | `#F2E7D5` |
| shell frame | `#297072` |
| shell interior | `#204852` |
| `++` and cursor | `#9FD49C` |

## Files

| file | use |
|---|---|
| `hermit-master.png` | 2048², flat `#2C2D32` ground. **The canonical source** — every other file here is derived from it |
| `hermit-transparent.png` | 2048², background removed. **Dark backgrounds only** — see the limitation below |
| `hermit-icon-{512,256,128,64,48,32,16}.png` | square icons, trimmed and re-padded into a square canvas 1.15x the mark's longest side — 6.3% side margin, 11.5% top and bottom |
| `hermit-icon-{512,256,128}-alpha.png` | same, transparent |
| `hermit-badge-512.png` | rounded-square badge, 22% corner radius. The safe choice for any surface whose theme you do not control |
| `hermit-readme-{320,200}.png` | badge at README display sizes |
| `favicon.ico` | 16/32/48 multi-resolution |

## Two limitations, both real

**The transparent version only works on dark backgrounds.** In the artwork the crab has no
drawn contour — what reads as an outline is the charcoal ground showing through the gaps between
its limbs. Remove the background and the cream body (`#F2E7D5`) loses its edge against anything
light. Use `hermit-badge-512.png` on light or theme-variable surfaces; reserve the alpha files
for dark ones.

**16px is a smudge.** The mark carries a terminal frame, a spiral, a cursor, six limbs and two
eyestalks — too much to survive sixteen pixels, and no resampling fixes that. 32px is
serviceable, 48px and up are clean. If the favicon matters, the fix is a hand-drawn simplified
mark at that size, not a better downscale.

## Regenerating

Downscales are Lanczos followed by `-unsharp 0x0.6+0.9+0.02`; the sharpening pass is what
separates the crab's limbs at 32 and 48px, and dropping it visibly muddies both.

Background removal is a **flood-fill from the borders**, never a global `-transparent`. The
spiral and the crab's outlines are charcoal within a few percent of the ground, so a global
replace punches holes straight through them. Keep the fuzz at 4%: background JPEG noise sits a
mean of 0.32% from the ground in the colour cube (99th percentile 0.93%, worst observed 1.77%),
while the shell interior sits 9.86% away. A fuzz of 10% straddles that interior distance and
bleeds into it unevenly, which is exactly the defect this note exists to prevent.

The transparent variant additionally erodes its alpha by one pixel. Without that, roughly half
the boundary pixels are background-coloured blend left behind by the fill — invisible against
the dark ground it was drawn on, a dirty rim on anything else. The same pass removes three
stray single-pixel specks.

```sh
magick hermit-master.png -alpha set -fuzz 4% \
  -fill none -floodfill +0+0 'srgb(44,45,50)' \
  -fill none -floodfill +2047+2047 'srgb(44,45,49)' \
  -channel A -morphology Erode Octagon:1 +channel -strip hermit-transparent.png
```

Sharpening applies **only at 128px and below**. At 256 and up it produces visible ringing —
undershoot darker than any colour in the artwork — and buys nothing, since those sizes are
legible without it.

## Measured contrast

WCAG ratios against the ground `#2C2D32`, computed rather than eyeballed:

| pair | ratio | reading |
|---|---|---|
| crab body on ground | 11.23:1 | strong — the crab carries the whole silhouette |
| accent on shell interior | 5.86:1 | strong — `++` and cursor read at every size |
| shell frame on ground | 2.39:1 | weak |
| **shell interior on ground** | **1.38:1** | **the one real defect** |

The window fill is within a rounding error of the background luminance, so the shell reads as a
*hole* rather than a solid object, and the mark's silhouette is carried almost entirely by the
crab. It looks fine at 128px and up; it costs legibility at 32px and below.

Lifting the interior to `#245C68` (1.84:1) fixes it and can be applied in post, since the region
is flat:

```sh
magick hermit-icon-512.png -fuzz 8% -fill '#245C68' -opaque '#204852' out.png
```

The cost is that the frame and interior converge, so the window frame stops reading as a
separate element. Lifting both — interior to `#245C68`, frame a matching step lighter — is the
version worth regenerating the artwork for rather than patching.

### Badge behaviour against host themes

| host | ratio | reading |
|---|---|---|
| GitHub dark `#0D1117` | 1.38:1 | tile edge invisible — correct |
| Gitea dark `#1E1F22` | 1.20:1 | tile edge invisible — correct |
| light `#FFFFFF` | 13.74:1 | clean tile — correct |

This is the behaviour you want and is **not** a defect: the badge dissolves into dark themes and
presents as a deliberate tile on light ones. The ground colour needs no change.

### Why the transparent version fails on light

The crab body is `#F2E7D5` against white — **1.22:1**. No resampling fixes this, because the
crab has no drawn contour. Giving it an explicit dark outline is the only real fix, and it has
to happen in the artwork.
