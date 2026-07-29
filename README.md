# grainmeter

A small, portable C++/OpenCV command-line tool that counts grains in
flatbed-scanner images and measures per-grain **area, length, width, and
mean color** — the same job GrainScan does, but built as a native
Linux/macOS binary instead of the Windows-only R/GTK app, with batch and
multi-threaded processing built in.

## How it works

1. Grayscale + Otsu threshold, auto-detecting whether grains are darker or
   lighter than the background.
2. Morphological open/close to remove scanner speckle noise.
3. Distance-transform + marker-based watershed to split grains that are
   touching or slightly overlapping (the same class of problem GrainScan's
   segmentation step handles).
4. For each resulting region: pixel area, a rotated bounding box
   (`cv::minAreaRect`) for length (long side) / width (short side)
   converted to mm using the scan's DPI (`px_per_mm = dpi / 25.4`), and
   mean R/G/B sampled from the original image under that grain's mask.
5. A region smaller than `--min-area-mm2` (dust/debris) is dropped. A
   region bigger than `--max-area-mm2` is assumed to be an unsplit
   touching-grain cluster: it gets a forced re-split attempt (progressively
   tighter watershed seed spacing on just that region) rather than being
   discarded; if no split is found at any spacing, it's kept as a single
   oversized region rather than dropped. A region touching the image
   border (partial grain) is excluded by default.
6. Outputs a per-grain CSV, a per-file summary CSV, summary statistics on
   stdout, and an annotated QC image so you can visually check the
   segmentation.

## Build

Requires a C++17 compiler, CMake, and OpenCV (core/imgproc/imgcodecs).

**Linux (Debian/Ubuntu):**
```bash
sudo apt install build-essential cmake libopencv-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

**macOS (Homebrew):**
```bash
brew install cmake opencv
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This produces `build/grainmeter`. The build links only the OpenCV modules
actually used (core/imgproc/imgcodecs, + geometry on OpenCV 5) rather than
every module your local OpenCV happens to have, which keeps process
startup fast even with a "kitchen sink" OpenCV install.

## Usage

**Single image:**
```bash
./build/grainmeter --input scan.jpg --dpi 300
```
Writes `results/scan.csv` and `results/scan_annotated.jpg` (see
"Output files" below).

**Batch (all JPGs in a folder), using all CPU cores:**
```bash
./build/grainmeter --input scans/*.jpg --dpi 300
```
Or, if you want grainmeter itself (rather than your shell) to expand the
pattern — useful with very large folders, or in scripts — quote it:
```bash
./build/grainmeter --input "scans/*.jpg" --dpi 300
```
Both forms work the same way; multiple explicit files also work:
`--input a.jpg b.jpg c.jpg`.

Full options:
```
--input <path> [<path> ...]   One or more images, and/or a glob pattern    [required]
--dpi <n>                     Scan resolution, dots per inch                (300)
--out-dir <path>               Output folder, created if needed             (results)
--out-csv <path>               Per-grain CSV path (single-file runs only)   (see below)
--out-image <path>             Annotated QC image path (single-file only)   (see below)
--summary-csv <path>           Cross-file summary CSV path                  (see below)
--min-area-mm2 <n>             Reject regions smaller than this             (3.0)
--max-area-mm2 <n>              Regions bigger than this get re-split,
                                 not discarded (see "How it works")          (20.0)
--polarity <auto|dark|light>   Grain vs. background brightness              (auto)
--no-watershed                 Disable splitting of touching grains
--include-border                Keep grains touching the image edge
                                 (default: excluded)
--seed-separation-mm <n>        Min distance between distinct watershed
                                 seed peaks                                  (2.0)
--seed-merge-mm <n>             Merge distance for nearby seed points
                                 into one grain                              (2.0)
--crease-close-mm <n>           Pre-seeding gap-closing size, to bridge a
                                 grain's own crease/notch (0 disables)       (0.6)
--threads <n>                   Parallel worker threads for batch
                                 processing                                  (CPU core count)
--debug                         Dump intermediate mask/distance images
                                 for tuning
--help                          Show usage
```

### Output files

By default, everything is written under `--out-dir` (default `results/`,
created automatically):
- `<out-dir>/<name>.csv` — same base name as the input, extension swapped to `.csv`
- `<out-dir>/<name>_annotated.<ext>` — same base name, `_annotated` appended, original extension kept
- `<out-dir>/summary.csv` — **one row per input file** (see below), always written,
  whether you ran on a single image or a whole batch

e.g. `wheat_plot12.jpg` → `results/wheat_plot12.csv` and
`results/wheat_plot12_annotated.jpg`.

The annotated image outlines each kept grain in green with a red dot at
its centroid and its id number in green next to it, so you can visually
cross-check the count and catch any obviously wrong splits/merges.

`--out-csv`/`--out-image` let you override both per-grain output paths
explicitly, but only apply when exactly one input file is given — with
multiple/glob inputs there's no single filename that makes sense for all
of them, so those flags are ignored (with a warning) in favor of the
`--out-dir` convention. `--summary-csv` overrides the summary path in
either mode (single-file or batch).

### CSV columns

**Per-grain CSV** (`<name>.csv`): `id, area_mm2, length_mm, width_mm, mean_r, mean_g, mean_b`

- `length`/`width` are the long/short axis of the rotated bounding box fit
  to each grain outline (the standard measure used by grain-phenotyping
  tools such as GrainScan/SmartGrain).
- `mean_r`/`mean_g`/`mean_b` are the mean red/green/blue channel values
  (0-255) of the pixels inside that grain's mask, sampled from the
  original (unblurred, unthresholded) image — useful as a proxy for grain
  color/brightness traits.

**Summary CSV** (`summary.csv`, one row per input file):
`filename, seed_count, mean_area_mm2, mean_length_mm, mean_width_mm, mean_r, mean_g, mean_b`

- `seed_count` is the number of grains counted (post-filtering) in that file.
- The `mean_*` columns are that file's per-grain averages from its own CSV.
- A file that failed to load gets a row with `seed_count` 0 and blank
  averages rather than being silently dropped, so you can spot it in the summary.

### Batch processing & threads

Any number of `--input` files (explicit list or glob) triggers batch mode:
each file is processed independently (its own CSV + annotated image under
`--out-dir`) and a batch summary is printed at the end. Files are
distributed across a thread pool sized by `--threads` (default: number of
CPU cores, capped at the number of files). Progress lines from different
threads are tagged with the file's name (e.g. `[scan_a] Segmenting...`) and
are never interleaved mid-line, so batch output stays readable even at
high thread counts. Single-file runs are unaffected — they always use one
thread.

## Tuning for your scans

- **`--min-area-mm2`**: anything smaller is dropped as dust/debris noise.
  Run once with a very permissive bound (e.g. `--min-area-mm2 0.1`), sort
  the `area_mm2` column from the CSV, and look for the gap between a
  cluster of clearly-too-small specks and the real population -- set the
  bound just above that gap. The current default (3.0) was tuned this way
  against a real scan of small grains (not idealized wheat) with a black
  backing board, where noise topped out below ~0.7 mm² and the real
  population started at ~2.8 mm².
- **`--max-area-mm2`**: a region bigger than this is assumed to be an
  unsplit touching-grain cluster, not debris -- it gets a forced re-split
  attempt (progressively tighter seed spacing on just that region) rather
  than being discarded. If a genuine split is found, each piece is counted
  individually; if no split is found at any spacing (i.e. it really is one
  large object), it's kept as a single region rather than dropped, and
  counted in "Still oversized after split attempts" in the summary so you
  can see how often that happened. Tune the default (20.0) the same way as
  `--min-area-mm2`: sort `area_mm2` from a permissive run and look for the
  gap between the real population and a handful of much-larger outliers.
  On a real reference image tested during development, that gap sat
  between ~19.5 mm² and 22-29 mm² for the 3 pairs that hadn't separated on
  the first pass.
- **Touching grains**: watershed seeds splits from local maxima of the
  distance transform -- a pixel becomes a seed if it's the peak within
  `--seed-separation-mm` (default 2.0) of itself. These local-maxima
  peaks deliberately never compare one grain against another (global or
  per-blob) -- earlier versions of this tool tried both, and each let
  some grains silently disappear or under-split depending on what else
  was in the scan; every grain's seed now only depends on its own shape.
  Three flags tune this without needing to recompile:
  - **`--seed-merge-mm`** (default 2.0): merges nearby near-tied seed
    points into one. **Try raising this first** if a single grain is
    showing up as 2 (or more) points in `debug_sure_fg.png` -- the
    distance transform's fast approximation can produce a couple of
    separate near-maximal pixels close to a true peak instead of one
    clean point, and this is the most common cause of over-splitting.
  - **`--crease-close-mm`** (default 0.6): a small gap-closing pass run
    only for seed-finding (it never changes the actual measured shape),
    aimed at a different cause of the same 2-points-per-grain symptom --
    a real physical crease (e.g. a wheat kernel's sulcus) that's light
    enough to threshold as background can pinch the binary silhouette
    into two lobes, which genuinely produces two distance-transform
    peaks rather than one. If `--seed-merge-mm` alone doesn't fix a
    persistent 2-seeds-per-grain pattern, raise this instead (or check
    `debug_binary.png` for a visible notch running through each grain).
    Set to 0 to disable if your grains have no such crease and you'd
    rather not risk it bridging anything else.
  - **`--seed-separation-mm`**: lower it if genuinely touching grains are
    being merged into one (missed splits); raise it if one grain is being
    split into more than one for a reason unrelated to the two causes
    above.
  `--no-watershed` disables splitting entirely if your images have no
  touching grains.
- **Debug images**: `--debug` writes `<name>_debug_sure_fg.png` (one blob
  per grain if seeding is working correctly) and `<name>_debug_binary.png`
  (the thresholded silhouette, useful for spotting a crease/notch). If a
  grain you expect to see is missing from your final count, check whether
  it has a seed at all (if not, it's a segmentation/thresholding issue
  upstream, e.g. `--polarity`) versus whether it has a seed but got
  filtered out by `--min-area-mm2` or `--include-border` (or, less often,
  never got a seed to split from in the first place if it's part of an
  oversized region that couldn't be re-split -- check for it under "Still
  oversized after split attempts" in the summary).
  If a grain you expect to see as one is showing as two, see the three
  flags above.
- **Background**: works with either a light (paper) or dark background;
  `--polarity` forces the choice if auto-detection picks the wrong class
  on a low-contrast scan.
- **Scan quality**: as with GrainScan, even, diffuse lighting and a
  background that contrasts clearly with grain colour matters more than
  anything else for accuracy. A dark (black or navy) backing behind the
  grains tends to give the cleanest segmentation on a flatbed scanner.
- Use `--debug` to inspect `<name>_debug_binary.png`,
  `<name>_debug_distance.png`, `<name>_debug_sure_fg.png`,
  `<name>_debug_sure_bg.png` (written next to the executable) and adjust
  from there.

## Validation

Tested against a synthetic scan of 60 ellipse "grains" (known length/width,
some touching) rendered at 300 dpi. For grains with no neighbor closer than
their own size (i.e. truly isolated), measured length and width matched
ground truth to within ~0.05–0.1 mm on average — real scans will have more
noise from grain surface texture and awns, so validate against a handful of
hand-measured grains from your own scanner before trusting it for a full
dataset.

Also tested against a synthetic scan with one large, round outlier grain
(e.g. a stray weed seed or debris) alongside 25 normal, well-separated
wheat-sized grains: earlier seeding logic that compared each grain's
distance-transform peak against a single global (or per-blob) maximum let
the outlier's much larger peak wipe out most of the normal grains' seeds
entirely -- only 1 of 26 grains got a seed in that version, and the missing
25 simply vanished from the count with no error. The current local-maxima
seeding (each grain's peak judged only against its own neighborhood, never
against any other grain) finds all 26 correctly, with length/width accuracy
matching the isolated-grain numbers above.

Also tested against a synthetic scan of 20 grains each with a real
crease-like feature (a lighter line thresholding as background, pinching
the binary silhouette into two lobes) -- without `--crease-close-mm`
(disabled via `--crease-close-mm 0`), every single grain produced exactly
2 seeds (40 from 20 true grains); with the default 0.6mm crease-closing
enabled, seeding matches truth exactly (20/20), with no change to the
actual measured length/width (crease-closing is only used to decide where
to place seeds, never to alter the measured shape).

Also stress-tested on a synthetic full letter-page (2550x3300 px) scan with
600 grains plus 4000 noise specks: several hundred grains measured
correctly in a couple of seconds. The tool prints progress to stderr as it
works (image loaded, segmenting, watershed splitting, measuring N regions,
writing results) so a large scan doesn't look stuck while it's processing.
Batch mode with 3 files (including one full-page-size scan) using 3
threads completed in about 1 second in testing.

## Known limitations vs. GrainScan

- Heavily overlapping grains (>~40% overlap) can still be under-split, or
  a forced re-split of an oversized region can occasionally cut a
  genuinely-single elongated grain if it happens to have two comparably
  strong distance-transform peaks; physically separating grains on the
  scanner bed before scanning remains the most reliable fix, same as with
  GrainScan.
- No built-in per-pixel calibration from a reference object (e.g. a ruler
  or known-size disc in the scan) — DPI is taken as ground truth from the
  scanner setting, which is accurate as long as your scanner driver isn't
  lying about its resolution.


## Acknowledgement
This app used [OpenCV](https://opencv.org/) library and was written with the help of [Claude AI](https://claude.ai/).