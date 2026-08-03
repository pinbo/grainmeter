// grainmeter.cpp
//
// Count wheat (or other) grains in flatbed-scanner JPG/PNG images and
// measure area, length and width per grain, similar in purpose to
// GrainScan (Whan et al. 2014) but implemented in portable C++/OpenCV so
// it builds and runs natively on Linux and macOS.
//
// Pipeline (per image):
//   1. Load image, convert to grayscale.
//   2. Otsu threshold, auto-detecting whether grains are the dark or the
//      light class (whichever is the minority of image area).
//   3. Morphological opening to remove scanner speckle noise.
//   4. Distance-transform + watershed to split grains that touch/overlap.
//   5. Per-grain region: pixel area -> mm^2, and a rotated bounding box
//      (cv::minAreaRect) -> length/width in mm, using the known scan DPI,
//      plus mean R/G/B color sampled from the original image.
//   6. Optional exclusion of grains touching the image border (partial
//      grains bias size statistics) and of blobs outside a plausible
//      area range (dust specks, clumps of >1 unsplit grain, scale bars).
//   7. CSV of per-grain measurements + summary statistics + an annotated
//      QC image with numbered outlines.
//
// Supports batch processing (multiple input files / glob patterns) with
// a thread pool that processes files in parallel.
//
// Build:
//   cmake -B build && cmake --build build
//
// Usage:
//   ./grainmeter --input scan.jpg --dpi 300
//   ./grainmeter --input scans/*.jpg --threads 8
//
// See --help for all options.

// Include specific modules rather than the <opencv2/opencv.hpp> umbrella
// header. On systems with more than one OpenCV install on the include
// path (Homebrew + conda/pip opencv-python is the classic macOS case),
// the umbrella header from the "wrong" install can resolve without
// pulling in imgproc, which is what causes errors like "no member named
// 'DIST_L2'/'contourArea'/'minAreaRect' in namespace 'cv'" even though
// OpenCV is installed and even though CMake links the correct library.
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
// OpenCV 5.0 split several geometry functions (minAreaRect, contourArea,
// moments, convexHull, fitEllipse, etc.) out of imgproc and into a new
// "geometry" module; this header doesn't exist in OpenCV 4.x, hence the
// version guard. See: https://github.com/opencv/opencv/issues/29510
#if defined(CV_VERSION_MAJOR) && CV_VERSION_MAJOR >= 5
#include <opencv2/geometry.hpp>
#endif

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <climits>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

struct Options {
    std::vector<std::string> rawInputs;    // as given on the command line, may include globs
    std::string outDir = "results";        // all outputs are written here by default
    std::string outCsvOverride;             // only honored when exactly one input file resolves
    std::string outImageOverride;           // only honored when exactly one input file resolves
    std::string summaryCsvOverride;         // default: <out-dir>/summary.csv
    double dpi = 300.0;
    double minAreaMm2 = 3.0;    // reject specks/noise smaller than this
    double maxAreaMm2 = 20.0;   // regions bigger than this get re-split (tighter seed spacing); kept as-is if that fails
    bool excludeBorder = true;
    bool useWatershed = true;
    std::string polarity = "auto"; // auto | dark | light  (grain vs background)
    // Watershed seed-finding tuning -- see the comments in watershedSplit()
    // for what each one does and which direction to adjust it.
    double seedSeparationMm = 2.0;  // min distance enforced between distinct seed peaks
    double seedMergeMm = 2.0;       // merge distance for same-peak "salt" -- raise if grains still over-split
    double creaseCloseMm = 0.6;     // pre-seeding closing to bridge creases/notches within one grain
    double minSolidity = 0.0;       // region convexity (area / convex-hull area) below this triggers a forced re-split attempt; 0 disables this check (off by default -- see README for why 0.75 wasn't safe as a default)
    bool colorSeeds = false;        // give each grain a distinct fill/outline color in the annotated image (off by default; a single green outline is easier to read for most uses)
    bool showIds = false;           // draw each grain's id number in the annotated image (off by default; the numbers get busy on dense scans -- the outline/dot/color alone is usually enough for a QC pass)
    bool debug = false;
    unsigned int threads = 0;   // 0 = auto (hardware_concurrency)
};

struct Grain {
    int id;
    double areaPx;
    double lengthPx;
    double widthPx;
    double perimeterPx;
    cv::Point2f centroid;
    bool touchesBorder;
    double meanR, meanG, meanB; // 0-255, sampled from the original image
    double meanH, meanS, meanV; // H 0-360 degrees, S/V 0-100%, sampled from the original image
    double solidity;            // areaPx / convex-hull area; 1.0 = perfectly convex (oval-like), lower = concave (e.g. a "peanut" shape from two touching grains)
    cv::Rect regionBboxGlobal;  // this grain's own mask, for accurate per-grain outline drawing --
    cv::Mat regionMaskLocal;    // NOT the same as re-finding contours on the raw threshold mask, which
                                 // would merge a touching cluster's grains back into one outline even
                                 // after watershed correctly split them into separate counted grains.
};

// All console output goes through this so lines from different worker
// threads (batch mode) never interleave mid-line.
static std::mutex g_ioMutex;
static void say(std::ostream& os, const std::string& s) {
    std::lock_guard<std::mutex> lk(g_ioMutex);
    os << s;
}

static const char* GRAINMETER_VERSION = "1.3.0";

static void printUsage(const char* prog) {
    std::cout <<
        "grainmeter v" << GRAINMETER_VERSION << "\n"
        "Usage: " << prog << " --input <image> [<image> ...] [options]\n"
        "\n"
        "Required:\n"
        "  --input <path> [<path> ...]   One or more scanned images (JPG/PNG/TIFF).\n"
        "                                 Accepts multiple files and/or a glob pattern,\n"
        "                                 e.g. --input scans/*.jpg  or  --input a.jpg b.jpg\n"
        "                                 (quote the pattern, e.g. \"*.jpg\", if you want\n"
        "                                 this program to expand it instead of your shell)\n"
        "\n"
        "Options:\n"
        "  --dpi <n>                Scan resolution in dots per inch (default 300)\n"
        "  --out-dir <path>          Output folder (default \"results\"), created if needed\n"
        "  --out-csv <path>          Per-grain CSV output path. Only used when exactly one\n"
        "                            input file is given; default is <out-dir>/<name>.csv\n"
        "  --out-image <path>        Annotated QC image path. Only used when exactly one\n"
        "                            input file is given; default is\n"
        "                            <out-dir>/<name>_annotated.<ext>\n"
        "  --summary-csv <path>      Cross-file summary CSV (one row per input file:\n"
        "                            seed count and averages). Default is\n"
        "                            <out-dir>/summary.csv\n"
        "  --min-area-mm2 <n>       Reject blobs smaller than this (default 3.0)\n"
        "  --max-area-mm2 <n>       Regions bigger than this are re-split using tighter\n"
        "                            seed spacing (most likely an unsplit touching-grain\n"
        "                            cluster), not discarded; if splitting still isn't\n"
        "                            possible they're kept as one oversized region\n"
        "                            (default 20.0)\n"
        "  --polarity <auto|dark|light>  Are grains darker or lighter than the\n"
        "                            background? (default auto-detect)\n"
        "  --no-watershed           Disable splitting of touching grains\n"
        "  --include-border         Keep grains that touch the image edge\n"
        "                            (default: excluded, since they're partial)\n"
        "  --seed-separation-mm <n>  Min distance between distinct watershed seed\n"
        "                            peaks (default 2.0). Lower if touching grains\n"
        "                            are being merged into one; raise if one grain\n"
        "                            is being split into more than one.\n"
        "  --seed-merge-mm <n>       Distance within which nearby seed points are\n"
        "                            merged into one grain (default 2.0). Raise this\n"
        "                            first if a single grain still shows 2+ points in\n"
        "                            debug_sure_fg.png -- it's the most common fix.\n"
        "  --crease-close-mm <n>     Pre-seeding gap-closing size, to bridge a grain's\n"
        "                            own crease/notch before it can register as two\n"
        "                            peaks (default 0.6). Set to 0 to disable. Raise\n"
        "                            if a visible crease is still splitting grains;\n"
        "                            lower (or disable) if grains that genuinely\n"
        "                            touch are being merged into one.\n"
        "  --min-solidity <n>        Regions less convex/oval-shaped than this\n"
        "                            (contour area / convex-hull area) get a forced\n"
        "                            re-split attempt even if their area looks\n"
        "                            normal -- catches touching pairs that happen to\n"
        "                            sum to a plausible single-grain area (default 0,\n"
        "                            i.e. off; range 0-1). Off by default because\n"
        "                            real single grains commonly measure 0.80-0.95\n"
        "                            due to natural texture/crease/asymmetry, so an\n"
        "                            enabled default risked flagging ordinary grains\n"
        "                            as needing a split; 0.75 is a reasonable value\n"
        "                            to try if you want it on -- see the README.\n"
        "  --color-seeds              Give each grain in the annotated image a\n"
        "                            distinct fill/outline color instead of a plain\n"
        "                            green outline (GrainScan-style; default off)\n"
        "  --show-ids                 Draw each grain's id number in the annotated\n"
        "                            image (default off; the outline/dot/color alone\n"
        "                            is usually enough for a QC pass, and the\n"
        "                            numbers get busy on dense scans)\n"
        "  --threads <n>             Parallel worker threads for batch processing\n"
        "                            (default: number of CPU cores)\n"
        "  --debug                   Write intermediate mask images for tuning\n"
        "  --help                    Show this message\n"
        "  --version                 Show version number and exit\n";
}

static bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << flag << "\n";
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "--input") {
            if (i + 1 >= argc) { std::cerr << "Missing value(s) for --input\n"; std::exit(1); }
            while (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                o.rawInputs.push_back(argv[++i]);
            }
            if (o.rawInputs.empty()) { std::cerr << "Missing value(s) for --input\n"; std::exit(1); }
        }
        else if (a == "--out-dir") o.outDir = next("--out-dir");
        else if (a == "--out-csv") o.outCsvOverride = next("--out-csv");
        else if (a == "--out-image") o.outImageOverride = next("--out-image");
        else if (a == "--summary-csv") o.summaryCsvOverride = next("--summary-csv");
        else if (a == "--dpi") o.dpi = std::stod(next("--dpi"));
        else if (a == "--min-area-mm2") o.minAreaMm2 = std::stod(next("--min-area-mm2"));
        else if (a == "--max-area-mm2") o.maxAreaMm2 = std::stod(next("--max-area-mm2"));
        else if (a == "--polarity") o.polarity = next("--polarity");
        else if (a == "--no-watershed") o.useWatershed = false;
        else if (a == "--include-border") o.excludeBorder = false;
        else if (a == "--seed-separation-mm") o.seedSeparationMm = std::stod(next("--seed-separation-mm"));
        else if (a == "--seed-merge-mm") o.seedMergeMm = std::stod(next("--seed-merge-mm"));
        else if (a == "--crease-close-mm") o.creaseCloseMm = std::stod(next("--crease-close-mm"));
        else if (a == "--min-solidity") o.minSolidity = std::stod(next("--min-solidity"));
        else if (a == "--color-seeds") o.colorSeeds = true;
        else if (a == "--show-ids") o.showIds = true;
        else if (a == "--threads") o.threads = static_cast<unsigned int>(std::stoul(next("--threads")));
        else if (a == "--debug") o.debug = true;
        else if (a == "--help") { printUsage(argv[0]); std::exit(0); }
        else if (a == "--version") { std::cout << "grainmeter v" << GRAINMETER_VERSION << "\n"; std::exit(0); }
        else { std::cerr << "Unknown argument: " << a << "\n"; printUsage(argv[0]); std::exit(1); }
    }
    if (o.rawInputs.empty()) {
        std::cerr << "Error: --input is required.\n";
        printUsage(argv[0]);
        return false;
    }
    return true;
}

// Simple glob-style wildcard match ('*' = any run of characters, '?' = any
// single character) of `name` against `pattern`. Standard greedy-backtrack
// matcher, O(pattern * name) worst case.
static bool wildcardMatch(const std::string& pattern, const std::string& name) {
    size_t p = 0, n = 0, star = std::string::npos, matchFrom = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) {
            ++p; ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            matchFrom = n;
        } else if (star != std::string::npos) {
            p = star + 1;
            n = ++matchFrom;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

// Expand a single --input token: if it contains glob metacharacters, expand
// it against the files in its directory (covers the common case of a
// *quoted* pattern like "*.jpg" that the shell passed through literally,
// and is also required on Windows, where neither cmd.exe nor PowerShell
// expand wildcards for external programs the way Unix shells do -- this
// implementation is what makes --input *.jpg work at all there). Otherwise
// treat the token as a literal path. Unquoted globs on Unix shells (e.g.
// --input *.jpg) are typically already expanded into multiple argv tokens
// by the shell before we ever see them, which also works fine since each
// expanded token then has no wildcard characters left in it.
//
// Deliberately implemented with std::filesystem + a small hand-written
// matcher instead of POSIX glob(3): glob.h isn't available on Windows
// (neither with MSVC nor reliably with MinGW), and this way the exact same
// source builds and behaves identically on Linux, macOS, and Windows with
// no #ifdefs. The one feature this doesn't replicate from glob(3) is `~`
// (home directory) expansion, since that's a shell convention with no
// equivalent on Windows anyway.
static std::vector<std::string> expandOneInput(const std::string& pattern) {
    std::vector<std::string> out;
    bool hasWildcard = pattern.find_first_of("*?") != std::string::npos;
    if (!hasWildcard) {
        std::error_code ec;
        if (fs::exists(pattern, ec)) out.push_back(pattern);
        else std::cerr << "Warning: input file not found, skipping: " << pattern << "\n";
        return out;
    }

    fs::path patternPath(pattern);
    fs::path dir = patternPath.has_parent_path() ? patternPath.parent_path() : fs::path(".");
    std::string filePattern = patternPath.filename().string();

    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::cerr << "Warning: no files matched pattern: " << pattern << "\n";
        return out;
    }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string name = entry.path().filename().string();
        if (wildcardMatch(filePattern, name)) out.push_back(entry.path().string());
    }
    if (out.empty()) {
        std::cerr << "Warning: no files matched pattern: " << pattern << "\n";
    }
    return out;
}

static std::vector<std::string> resolveInputs(const std::vector<std::string>& rawInputs) {
    std::vector<std::string> files;
    for (auto& raw : rawInputs) {
        auto expanded = expandOneInput(raw);
        files.insert(files.end(), expanded.begin(), expanded.end());
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

// Default output paths: <out-dir>/<stem>.csv and <out-dir>/<stem>_annotated<ext>
static void deriveDefaultPaths(const std::string& inputPath, const std::string& outDir,
                                std::string& csvPath, std::string& imgPath) {
    fs::path p(inputPath);
    std::string stem = p.stem().string();
    std::string ext = p.extension().string();
    if (ext.empty()) ext = ".jpg";
    csvPath = (fs::path(outDir) / (stem + ".csv")).string();
    imgPath = (fs::path(outDir) / (stem + "_annotated" + ext)).string();
}

// Otsu-threshold in both directions and pick whichever gives the smaller
// foreground area fraction -- grains are normally the minority class on a
// scanner bed. Can be overridden with --polarity dark|light.
// Also reports which polarity was chosen via outBackgroundIsDark, so the
// caller can pick a contrasting color (e.g. white text on a dark backing
// board) without re-deriving it separately.
static cv::Mat autoThreshold(const cv::Mat& gray, const std::string& polarity, bool debug,
                              const std::string& tag, bool& outBackgroundIsDark) {
    cv::Mat binDark, binLight;
    cv::threshold(gray, binDark, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU); // grains darker than bg (bg light)
    cv::threshold(gray, binLight, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);    // grains lighter than bg (bg dark)

    if (polarity == "dark") { outBackgroundIsDark = false; return binDark; }
    if (polarity == "light") { outBackgroundIsDark = true; return binLight; }

    double fracDark = static_cast<double>(cv::countNonZero(binDark)) / (gray.rows * gray.cols);
    double fracLight = static_cast<double>(cv::countNonZero(binLight)) / (gray.rows * gray.cols);
    if (debug) {
        std::ostringstream ss;
        ss << "[debug] " << tag << " foreground fraction dark-hypothesis=" << fracDark
           << " light-hypothesis=" << fracLight << "\n";
        say(std::cerr, ss.str());
    }
    bool chooseDark = (fracDark < fracLight); // grains darker than background => background is light
    outBackgroundIsDark = !chooseDark;
    return chooseDark ? binDark : binLight;
}

// Split touching grains with a distance-transform + marker-based watershed.
//
// Seeding strategy: a pixel is a seed candidate if it's a local maximum of
// the distance transform within a fixed physical radius (scaled by DPI),
// found via a grayscale max-filter (dilate). This is deliberately NOT a
// threshold expressed as a fraction of some "max distance" value, whether
// global (one number for the whole image) or per-blob (one number per
// pre-watershed blob) -- both of those were tried and both have the same
// underlying problem: comparing a grain's peak against *some other
// region's* peak. If one grain elsewhere is bigger/rounder, a global max
// lets it wipe out normal grains' seeds entirely (they silently vanish --
// this is the bug this function used to have, visible as grains missing
// from debug_sure_fg.png). Per-blob thresholds fix that, but introduce a
// different problem: the per-blob threshold is often *more permissive*
// than the old global one, which lets touching grains' seed regions merge
// into each other and under-splits clusters that used to separate fine.
// A local-maxima peak, in contrast, only ever asks "is this pixel higher
// than everything within R pixels of it?" -- a question answered purely
// from the grain's own shape, with no reference to any other grain, near
// or far, so neither failure mode can occur.
static cv::Mat watershedSplit(const cv::Mat& bgrImage, const cv::Mat& binaryMask, double pxPerMm,
                               double seedSeparationMm, double seedMergeMm, double creaseCloseMm,
                               bool debug, const std::string& tag) {
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));

    // sureBg (and therefore the final measured shape of each grain, via
    // watershed flooding out to this boundary) is derived from the
    // ORIGINAL binary mask -- untouched by the crease-closing below, so
    // that fix only changes how many seeds get placed, never the actual
    // measured outline.
    cv::Mat sureBg;
    cv::dilate(binaryMask, sureBg, kernel, cv::Point(-1, -1), 3);

    // A real physical crease (e.g. the sulcus running down a wheat kernel)
    // can be light enough to get thresholded as background, pinching the
    // binary silhouette into two lobes joined by a thin bridge -- which
    // genuinely produces two separate distance-transform peaks, one on
    // each side. No amount of peak-merging in the seed-finding step below
    // fixes that correctly, because it isn't noise: the two peaks really
    // are that grain's two most-interior points. Bridging small notches
    // like this closed *before* the distance transform, using a mask
    // that's only used for seed-finding (not for the final measured
    // shape above), removes the pinch at the source.
    cv::Mat creaseClosed = binaryMask;
    if (creaseCloseMm > 0) {
        int creaseKernelPx = std::max(3, static_cast<int>(std::lround(creaseCloseMm * pxPerMm)));
        if (creaseKernelPx % 2 == 0) creaseKernelPx += 1;
        cv::Mat creaseKernel = cv::getStructuringElement(cv::MORPH_ELLIPSE,
                                                          cv::Size(creaseKernelPx, creaseKernelPx));
        cv::morphologyEx(binaryMask, creaseClosed, cv::MORPH_CLOSE, creaseKernel, cv::Point(-1, -1), 1);
    }

    cv::Mat dist;
    cv::distanceTransform(creaseClosed, dist, cv::DIST_L2, 5);

    // Smooth the distance map slightly before peak-finding. This is a
    // standard preprocessing step for distance-transform seeding: OpenCV's
    // distanceTransform uses a fast 5x5-mask approximation rather than
    // exact Euclidean distance, which introduces small quantization steps
    // that can otherwise register as several near-tied local maxima a
    // couple of pixels apart instead of one clean peak.
    cv::Mat distSmooth;
    cv::GaussianBlur(dist, distSmooth, cv::Size(0, 0), 1.0);

    // Minimum physical separation enforced between distinct seed peaks.
    int radiusPx = std::max(2, static_cast<int>(std::lround(seedSeparationMm * pxPerMm)));
    int maxFilterSize = 2 * radiusPx + 1;
    cv::Mat maxFilterKernel = cv::getStructuringElement(cv::MORPH_ELLIPSE,
                                                         cv::Size(maxFilterSize, maxFilterSize));

    cv::Mat localMaxImg;
    cv::dilate(distSmooth, localMaxImg, maxFilterKernel);

    // A seed pixel: matches the local max filter (i.e. is a peak within
    // its neighborhood) AND clears a small absolute floor so flat,
    // near-zero background/thin-sliver noise never becomes a seed.
    const double minPeakDistPx = 1.5;
    cv::Mat isPeak = (distSmooth >= localMaxImg) & (distSmooth > minPeakDistPx);
    cv::Mat peakMask;
    isPeak.convertTo(peakMask, CV_8U, 255);

    // A true mathematical maximum is usually a single point, but even
    // after smoothing, near a real peak this can still produce a couple
    // of separate pixels within a few pixels of each other without being
    // 8-connected -- "salt" around the true peak rather than one clean
    // blob. Left alone, connectedComponents treats each speck as its own
    // seed, over-splitting a single grain into several. A small merge
    // dilation (much smaller than the suppression radius above) bridges
    // that salt into one blob per grain, while staying far too small to
    // bridge the several-mm gap between two genuinely distinct (even
    // touching) grains' peaks.
    int mergeRadiusPx = std::max(2, static_cast<int>(std::lround(seedMergeMm * pxPerMm)));
    cv::Mat mergeKernel = cv::getStructuringElement(cv::MORPH_ELLIPSE,
                                                     cv::Size(2 * mergeRadiusPx + 1, 2 * mergeRadiusPx + 1));
    cv::Mat sureFg;
    cv::dilate(peakMask, sureFg, mergeKernel);
    cv::bitwise_and(sureFg, binaryMask, sureFg); // keep seeds within the grain's actual silhouette

    cv::Mat unknown;
    cv::subtract(sureBg, sureFg, unknown);

    cv::Mat markers;
    int nMarkers = cv::connectedComponents(sureFg, markers);
    markers += 1;                 // background becomes 1, not 0
    markers.setTo(0, unknown == 255);

    cv::watershed(bgrImage, markers);

    if (debug) {
        std::ostringstream ss;
        ss << "[debug] " << tag << " watershed seed count=" << (nMarkers - 1)
           << " (seed radius=" << radiusPx << "px)\n";
        say(std::cerr, ss.str());
        double maxDistForDebugImage;
        cv::minMaxLoc(dist, nullptr, &maxDistForDebugImage);
        if (maxDistForDebugImage <= 0) maxDistForDebugImage = 1.0;
        cv::imwrite(tag + "_debug_distance.png", dist * (255.0 / maxDistForDebugImage));
        cv::imwrite(tag + "_debug_sure_fg.png", sureFg);
        cv::imwrite(tag + "_debug_sure_bg.png", sureBg);
    }

    return markers; // CV_32S label image; -1 = watershed boundary, 1 = background
}

// Mean B,G,R (OpenCV's native order) of `bgr` under a binary mask, both
// restricted to the same small ROI, returned as (R, G, B) in that order.
static cv::Vec3d meanColorRGB(const cv::Mat& bgrRoi, const cv::Mat& mask8u) {
    cv::Scalar m = cv::mean(bgrRoi, mask8u);
    return cv::Vec3d(m[2], m[1], m[0]);
}

// Mean H,S,V of `bgr` under a binary mask, both restricted to the same
// small ROI. H is scaled to the conventional 0-360 degrees (OpenCV's 8-bit
// HSV stores it halved, 0-179, to fit a byte); S and V are scaled to 0-100.
// This averages hue linearly rather than circularly, so it under-reports
// for grains whose hue straddles the 0/360 wrap (i.e. red); acceptable
// here since grains are practically never that saturated a red.
static cv::Vec3d meanColorHSV(const cv::Mat& bgrRoi, const cv::Mat& mask8u) {
    cv::Mat hsvRoi;
    cv::cvtColor(bgrRoi, hsvRoi, cv::COLOR_BGR2HSV);
    cv::Scalar m = cv::mean(hsvRoi, mask8u);
    return cv::Vec3d(m[0] * 2.0, m[1] / 255.0 * 100.0, m[2] / 255.0 * 100.0);
}

// A visually distinct BGR color for grain index `i`, for --color-seeds.
// Steps hue by the golden angle (~137.5 degrees) so consecutive indices
// land far apart around the color wheel; unlike splitting the wheel into
// N even slices, this doesn't need to know the total grain count up front
// and still stays well-separated for any number of grains.
static cv::Scalar distinctColorBGR(int i) {
    double hueDeg = std::fmod(i * 137.508, 360.0);
    cv::Mat hsv(1, 1, CV_8UC3, cv::Scalar(hueDeg / 2.0, 200, 255)); // OpenCV 8-bit hue range is 0-179
    cv::Mat bgr;
    cv::cvtColor(hsv, bgr, cv::COLOR_HSV2BGR);
    cv::Vec3b c = bgr.at<cv::Vec3b>(0, 0);
    return cv::Scalar(c[0], c[1], c[2]);
}

// A single labeled region from a watershed marker image, kept as a small
// local crop (bboxGlobal in full-image coordinates) plus a CV_8U 0/255
// mask of exactly that label's pixels within the crop. Working with small
// crops rather than full-image-sized masks per region is what keeps both
// the initial per-label measurement pass and oversized-region re-splitting
// fast on large scans with many grains.
struct GrainRegion {
    cv::Rect bboxGlobal;
    cv::Mat maskLocal;
};

// Extract all labeled regions (label >= 2) from a watershed marker image
// via a single pass, as bounding-box-local crops in full-image
// coordinates. `parentBboxGlobal` is where `markersLocal` sits within the
// full image: Rect(0,0,cols,rows) for the first pass over the whole
// image, or some sub-rectangle when re-splitting an oversized region.
static std::vector<GrainRegion> extractLabelRegions(const cv::Mat& markersLocal, cv::Rect parentBboxGlobal) {
    std::vector<GrainRegion> out;
    double minV, maxV;
    cv::minMaxLoc(markersLocal, &minV, &maxV);
    int maxLabel = static_cast<int>(maxV);
    if (maxLabel < 2) return out;

    std::vector<int> minX(maxLabel + 1, INT_MAX), minY(maxLabel + 1, INT_MAX);
    std::vector<int> maxX(maxLabel + 1, INT_MIN), maxY(maxLabel + 1, INT_MIN);
    std::vector<long long> pixelCount(maxLabel + 1, 0);
    for (int y = 0; y < markersLocal.rows; ++y) {
        const int* row = markersLocal.ptr<int>(y);
        for (int x = 0; x < markersLocal.cols; ++x) {
            int label = row[x];
            if (label < 2 || label > maxLabel) continue;
            pixelCount[label]++;
            if (x < minX[label]) minX[label] = x;
            if (x > maxX[label]) maxX[label] = x;
            if (y < minY[label]) minY[label] = y;
            if (y > maxY[label]) maxY[label] = y;
        }
    }

    for (int label = 2; label <= maxLabel; ++label) {
        if (pixelCount[label] < 4) continue;
        int x0 = std::max(0, minX[label] - 1);
        int y0 = std::max(0, minY[label] - 1);
        int x1 = std::min(markersLocal.cols - 1, maxX[label] + 1);
        int y1 = std::min(markersLocal.rows - 1, maxY[label] + 1);
        cv::Rect bboxLocal(x0, y0, x1 - x0 + 1, y1 - y0 + 1);

        cv::Mat sub = markersLocal(bboxLocal);
        cv::Mat mask = (sub == label);
        mask.convertTo(mask, CV_8U);

        GrainRegion region;
        region.bboxGlobal = cv::Rect(parentBboxGlobal.x + bboxLocal.x, parentBboxGlobal.y + bboxLocal.y,
                                      bboxLocal.width, bboxLocal.height);
        region.maskLocal = mask;
        out.push_back(std::move(region));
    }
    return out;
}

// Measure a single region (contour area, rotated-rect length/width,
// centroid in full-image coordinates, border contact, mean color) into a
// Grain. Returns false for a degenerate/empty region.
static bool measureGrainRegion(const cv::Mat& bgrFull, const GrainRegion& region,
                                int imgCols, int imgRows, Grain& out) {
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(region.maskLocal, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    if (contours.empty()) return false;
    auto biggest = std::max_element(contours.begin(), contours.end(),
        [](const std::vector<cv::Point>& a, const std::vector<cv::Point>& b) {
            return cv::contourArea(a) < cv::contourArea(b);
        });
    double area = cv::contourArea(*biggest);
    if (area < 4) return false;

    cv::RotatedRect rect = cv::minAreaRect(*biggest);
    cv::Moments m = cv::moments(*biggest);
    if (m.m00 <= 0) return false;
    cv::Point2f centroid(static_cast<float>(m.m10 / m.m00) + region.bboxGlobal.x,
                          static_cast<float>(m.m01 / m.m00) + region.bboxGlobal.y);

    bool border = centroid.x < 2 || centroid.y < 2 ||
                  centroid.x > imgCols - 2 || centroid.y > imgRows - 2;
    for (auto& p : *biggest) {
        cv::Point global = p + region.bboxGlobal.tl();
        if (global.x <= 1 || global.y <= 1 || global.x >= imgCols - 2 || global.y >= imgRows - 2) {
            border = true;
            break;
        }
    }

    cv::Vec3d rgb = meanColorRGB(bgrFull(region.bboxGlobal), region.maskLocal);
    cv::Vec3d hsv = meanColorHSV(bgrFull(region.bboxGlobal), region.maskLocal);

    // Solidity: how much of this region's convex hull is actually filled.
    // A single grain, being roughly oval, is close to convex (solidity near
    // 1.0). Two touching grains form a pinched "peanut" shape -- concave at
    // the waist -- which drags solidity down measurably even when the total
    // area looks like a perfectly normal single grain, so this catches
    // touching pairs the area-based check alone would miss.
    std::vector<cv::Point> hull;
    cv::convexHull(*biggest, hull);
    double hullArea = cv::contourArea(hull);
    double solidity = (hullArea > 0) ? (area / hullArea) : 1.0;

    out.areaPx = area;
    out.lengthPx = std::max(rect.size.width, rect.size.height);
    out.widthPx = std::min(rect.size.width, rect.size.height);
    out.perimeterPx = cv::arcLength(*biggest, true);
    out.centroid = centroid;
    out.touchesBorder = border;
    out.meanR = rgb[0]; out.meanG = rgb[1]; out.meanB = rgb[2];
    out.meanH = hsv[0]; out.meanS = hsv[1]; out.meanV = hsv[2];
    out.solidity = solidity;
    out.regionBboxGlobal = region.bboxGlobal;
    out.regionMaskLocal = region.maskLocal.clone();
    return true;
}

// If a region's area exceeds --max-area-mm2, OR its shape is too concave
// to plausibly be one grain (solidity below --min-solidity -- see the
// comment in measureGrainRegion), it's very likely two or more grains that
// the main watershed pass failed to separate (their peaks were too close
// together for --seed-separation-mm). Rather than discarding it, this
// retries watershed on just that region with progressively tighter seed
// spacing until it actually splits, then recurses into each resulting
// piece in case it's a cluster of 3+ grains rather than just 2. If no
// amount of tightening produces a split, it's kept as one region either
// way -- this function always adds something to `outGrains` for a valid
// region, it never silently drops one.
static void processRegionRecursive(const cv::Mat& bgrFull, const GrainRegion& region, double pxPerMm,
                                    const Options& opt, int imgCols, int imgRows, int depth,
                                    std::vector<Grain>& outGrains, int& oversizedBeforeSplitCount,
                                    const std::string& tag) {
    Grain g;
    if (!measureGrainRegion(bgrFull, region, imgCols, imgRows, g)) return;

    double mm2PerPx2 = 1.0 / (pxPerMm * pxPerMm);
    double areaMm2 = g.areaPx * mm2PerPx2;

    static const double kShrinkFactors[] = {1.5, 2.0, 3.0, 4.0, 6.0};
    const int kMaxSplitDepth = 4;

    bool oversized = areaMm2 > opt.maxAreaMm2;
    bool notOvalShaped = opt.minSolidity > 0.0 && g.solidity < opt.minSolidity;

    // Counted here, before the retry loop, so this reflects every region
    // that *started out* oversized -- whether or not the forced re-split
    // below went on to fix it. That's the useful number for deciding
    // whether to raise --max-area-mm2: a high count means many regions
    // are routinely landing above the threshold in the first place, which
    // either means real grains are bigger than the threshold assumes, or
    // (if most of them DO end up splitting successfully) that touching
    // pairs are common in this scan and the threshold is doing its job.
    // Compare against "Still oversized after split attempts" in the
    // summary: a big gap between the two means splitting is usually
    // succeeding; numbers close together mean it usually isn't.
    if (oversized) oversizedBeforeSplitCount++;

    if ((oversized || notOvalShaped) && depth < kMaxSplitDepth) {
        cv::Mat bgrCrop = bgrFull(region.bboxGlobal);
        // Oversized-by-area is a strong, unambiguous signal, so that retry
        // stays as aggressive as possible (crease-closing off, same as
        // before this feature was added). A concave-but-normal-area shape
        // is a softer signal that could also be a genuinely single grain
        // with a pronounced crease -- keep crease-closing on during that
        // retry so a real crease still gets bridged into one seed the same
        // way it would on the initial pass, and only a genuine touching
        // pair (whose "waist" isn't explained by a mm-scale crease) ends
        // up actually splitting.
        double creaseCloseForRetry = oversized ? 0.0 : opt.creaseCloseMm;
        for (double f : kShrinkFactors) {
            double sep = std::max(0.3, opt.seedSeparationMm / f);
            double merge = std::max(0.15, opt.seedMergeMm / f);
            cv::Mat subMarkers = watershedSplit(bgrCrop, region.maskLocal, pxPerMm, sep, merge,
                                                 creaseCloseForRetry, false, tag);
            auto subRegions = extractLabelRegions(subMarkers, region.bboxGlobal);
            if (subRegions.size() >= 2) {
                if (opt.debug && notOvalShaped && !oversized) {
                    std::ostringstream ss;
                    ss << "[debug] " << tag << " SPLIT (solidity-triggered) region at ("
                       << region.bboxGlobal.x << "," << region.bboxGlobal.y << ") solidity="
                       << g.solidity << " into " << subRegions.size() << " pieces\n";
                    say(std::cerr, ss.str());
                }
                for (auto& sr : subRegions) {
                    processRegionRecursive(bgrFull, sr, pxPerMm, opt, imgCols, imgRows, depth + 1, outGrains, oversizedBeforeSplitCount, tag);
                }
                return;
            }
        }
        // Couldn't force a split at any tried spacing -- fall through and
        // keep it as a single region rather than dropping it.
        if (opt.debug && notOvalShaped) {
            std::ostringstream ss;
            ss << "[debug] " << tag << " region at (" << region.bboxGlobal.x << "," << region.bboxGlobal.y
               << ") stayed non-oval after split attempts: solidity=" << g.solidity
               << " area_mm2=" << areaMm2 << "\n";
            say(std::cerr, ss.str());
        }
    }

    outGrains.push_back(g);
}

struct ImageResult {
    bool ok = false;
    int grainCount = 0;
    std::string error;
    double meanAreaMm2 = 0.0;
    double meanLengthMm = 0.0;
    double meanWidthMm = 0.0;
    double meanPerimeterMm = 0.0;
    double meanCircularity = 0.0;
    double meanAspectRatio = 0.0;
    double meanR = 0.0, meanG = 0.0, meanB = 0.0;
    double meanH = 0.0, meanS = 0.0, meanV = 0.0;
    int oversizedBeforeSplit = 0;
};

// Full single-image pipeline: segment, measure, write CSV + annotated image.
static ImageResult processImage(const std::string& inputPath, const Options& opt, bool isBatch) {
    ImageResult result;
    std::string tag = fs::path(inputPath).stem().string();

    cv::Mat bgr = cv::imread(inputPath, cv::IMREAD_COLOR);
    if (bgr.empty()) {
        result.error = "could not read image '" + inputPath + "'";
        say(std::cerr, "Error: " + result.error + "\n");
        return result;
    }

    const double pxPerMm = opt.dpi / 25.4;
    const double mm2PerPx2 = 1.0 / (pxPerMm * pxPerMm);

    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, gray, cv::Size(3, 3), 0);

    {
        std::ostringstream ss;
        ss << "[" << tag << "] Loaded image: " << bgr.cols << "x" << bgr.rows << " px. Segmenting...\n";
        say(std::cerr, ss.str());
    }
    bool backgroundIsDark = false;
    cv::Mat binary = autoThreshold(gray, opt.polarity, opt.debug, tag, backgroundIsDark);

    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
    cv::morphologyEx(binary, binary, cv::MORPH_OPEN, kernel, cv::Point(-1, -1), 1);
    cv::morphologyEx(binary, binary, cv::MORPH_CLOSE, kernel, cv::Point(-1, -1), 1);

    if (opt.debug) cv::imwrite(tag + "_debug_binary.png", binary);

    std::vector<Grain> grains;
    cv::Mat annotated = bgr.clone();
    int oversizedBeforeSplitCount = 0;

    if (opt.useWatershed) {
        say(std::cerr, "[" + tag + "] Splitting touching grains (watershed)...\n");
        cv::Mat markers = watershedSplit(bgr, binary, pxPerMm, opt.seedSeparationMm,
                                          opt.seedMergeMm, opt.creaseCloseMm, opt.debug, tag);
        auto regions = extractLabelRegions(markers, cv::Rect(0, 0, bgr.cols, bgr.rows));
        {
            std::ostringstream ss;
            ss << "[" << tag << "] Measuring " << regions.size() << " candidate region(s)...\n";
            say(std::cerr, ss.str());
        }
        for (auto& region : regions) {
            processRegionRecursive(bgr, region, pxPerMm, opt, bgr.cols, bgr.rows, 0, grains, oversizedBeforeSplitCount, tag);
        }
    } else {
        say(std::cerr, "[" + tag + "] Finding contours (watershed disabled)...\n");
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(binary, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        for (auto& c : contours) {
            double area = cv::contourArea(c);
            if (area < 4) continue;
            cv::RotatedRect rect = cv::minAreaRect(c);
            cv::Moments m = cv::moments(c);
            cv::Point2f centroid(static_cast<float>(m.m10 / m.m00), static_cast<float>(m.m01 / m.m00));
            bool border = false;
            for (auto& p : c) {
                if (p.x <= 1 || p.y <= 1 || p.x >= bgr.cols - 2 || p.y >= bgr.rows - 2) { border = true; break; }
            }

            cv::Rect bbox = cv::boundingRect(c);
            cv::Mat localMask = cv::Mat::zeros(bbox.size(), CV_8U);
            std::vector<std::vector<cv::Point>> shifted(1);
            shifted[0].reserve(c.size());
            for (auto& p : c) shifted[0].push_back(p - bbox.tl());
            cv::drawContours(localMask, shifted, 0, cv::Scalar(255), cv::FILLED);
            cv::Vec3d rgb = meanColorRGB(bgr(bbox), localMask);
            cv::Vec3d hsv = meanColorHSV(bgr(bbox), localMask);
            std::vector<cv::Point> hull;
            cv::convexHull(c, hull);
            double hullArea = cv::contourArea(hull);
            double solidity = (hullArea > 0) ? (area / hullArea) : 1.0;

            Grain g;
            g.areaPx = area;
            g.lengthPx = std::max(rect.size.width, rect.size.height);
            g.widthPx = std::min(rect.size.width, rect.size.height);
            g.perimeterPx = cv::arcLength(c, true);
            g.centroid = centroid;
            g.touchesBorder = border;
            g.meanR = rgb[0]; g.meanG = rgb[1]; g.meanB = rgb[2];
            g.meanH = hsv[0]; g.meanS = hsv[1]; g.meanV = hsv[2];
            g.solidity = solidity;
            g.regionBboxGlobal = bbox;
            g.regionMaskLocal = localMask.clone();
            grains.push_back(g);
        }
    }

    say(std::cerr, "[" + tag + "] Filtering and writing results...\n");
    std::vector<Grain> kept;
    int rejectedSize = 0, rejectedBorder = 0, stillOversized = 0, stillNotOvalShaped = 0;
    for (auto& g : grains) {
        double areaMm2 = g.areaPx * mm2PerPx2;
        // Oversized/non-oval-shaped regions were already given a chance to
        // split in processRegionRecursive() above (watershed branch) --
        // they are no longer rejected here, only counted, since the point
        // of that step is to keep them (split into pieces where possible)
        // rather than discard them. Only undersized regions (dust/debris)
        // are dropped.
        if (areaMm2 < opt.minAreaMm2) { rejectedSize++; continue; }
        if (opt.excludeBorder && g.touchesBorder) { rejectedBorder++; continue; }
        if (areaMm2 > opt.maxAreaMm2) stillOversized++;
        if (opt.minSolidity > 0.0 && g.solidity < opt.minSolidity) stillNotOvalShaped++;
        kept.push_back(g);
    }

    std::string csvPath, imgPath;
    deriveDefaultPaths(inputPath, opt.outDir, csvPath, imgPath);
    if (!isBatch && !opt.outCsvOverride.empty()) csvPath = opt.outCsvOverride;
    if (!isBatch && !opt.outImageOverride.empty()) imgPath = opt.outImageOverride;

    std::error_code ec;
    fs::path csvParent = fs::path(csvPath).parent_path();
    if (!csvParent.empty()) fs::create_directories(csvParent, ec);
    fs::path imgParent = fs::path(imgPath).parent_path();
    if (!imgParent.empty()) fs::create_directories(imgParent, ec);

    std::ofstream csv(csvPath);
    csv << "id,area_mm2,length_mm,width_mm,perimeter_mm,circularity,aspect_ratio,"
           "mean_r,mean_g,mean_b,mean_h,mean_s,mean_v\n";
    csv << std::fixed << std::setprecision(4);

    std::vector<double> areas, lengths, widths, perimeters, circularities, aspectRatios,
        colorsR, colorsG, colorsB, colorsH, colorsS, colorsV;
    int id = 1;
    for (auto& g : kept) {
        g.id = id;
        double areaMm2 = g.areaPx * mm2PerPx2;
        double lengthMm = g.lengthPx / pxPerMm;
        double widthMm = g.widthPx / pxPerMm;
        double perimeterMm = g.perimeterPx / pxPerMm;
        // Circularity: 1.0 for a perfect circle, lower for elongated/irregular
        // shapes. Dimensionless -- computing from the mm-converted area and
        // perimeter here gives the same result as from raw pixels, since the
        // pxPerMm scale factor cancels out of the ratio.
        double circularity = (perimeterMm > 0) ? (4.0 * CV_PI * areaMm2) / (perimeterMm * perimeterMm) : 0.0;
        double aspectRatio = (widthMm > 0) ? (lengthMm / widthMm) : 0.0;

        csv << id << "," << areaMm2 << "," << lengthMm << "," << widthMm << "," << perimeterMm << ","
            << circularity << "," << aspectRatio << ","
            << std::setprecision(1) << g.meanR << "," << g.meanG << "," << g.meanB << ","
            << g.meanH << "," << g.meanS << "," << g.meanV
            << std::setprecision(4) << "\n";

        areas.push_back(areaMm2);
        lengths.push_back(lengthMm);
        widths.push_back(widthMm);
        perimeters.push_back(perimeterMm);
        circularities.push_back(circularity);
        aspectRatios.push_back(aspectRatio);
        colorsR.push_back(g.meanR);
        colorsG.push_back(g.meanG);
        colorsB.push_back(g.meanB);
        colorsH.push_back(g.meanH);
        colorsS.push_back(g.meanS);
        colorsV.push_back(g.meanV);
        ++id;
    }
    csv.close();

    cv::Scalar idTextColor = backgroundIsDark ? cv::Scalar(255, 255, 255) : cv::Scalar(0, 0, 0);

    for (auto& g : kept) {
        // Recover this grain's own contour from its stored mask (not the
        // raw un-split threshold mask), so a touching cluster that
        // watershed correctly split into separate counted grains shows
        // each grain's own boundary here too, instead of one merged
        // outline around the whole original cluster.
        if (!g.regionMaskLocal.empty()) {
            std::vector<std::vector<cv::Point>> regionContours;
            cv::findContours(g.regionMaskLocal, regionContours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
            if (!regionContours.empty()) {
                auto biggest = std::max_element(regionContours.begin(), regionContours.end(),
                    [](const std::vector<cv::Point>& a, const std::vector<cv::Point>& b) {
                        return cv::contourArea(a) < cv::contourArea(b);
                    });
                std::vector<cv::Point> shifted;
                shifted.reserve(biggest->size());
                for (auto& p : *biggest) shifted.push_back(p + g.regionBboxGlobal.tl());
                std::vector<std::vector<cv::Point>> toDraw = {shifted};

                if (opt.colorSeeds) {
                    cv::Scalar color = distinctColorBGR(g.id);
                    // Semi-transparent fill within this grain's own mask
                    // (GrainScan-style per-particle highlight -- original
                    // grain texture stays faintly visible underneath),
                    // plus a solid outline in the same color around it.
                    cv::Mat roi = annotated(g.regionBboxGlobal);
                    cv::Mat colorLayer(roi.size(), roi.type(), color);
                    cv::Mat blended;
                    cv::addWeighted(roi, 0.45, colorLayer, 0.55, 0, blended);
                    blended.copyTo(roi, g.regionMaskLocal);
                    cv::drawContours(annotated, toDraw, -1, color, 2);
                } else {
                    cv::drawContours(annotated, toDraw, -1, cv::Scalar(0, 255, 0), 1);
                }
            }
        }

        cv::circle(annotated, g.centroid, 6, cv::Scalar(0, 0, 255), -1);
        if (opt.showIds) {
            cv::putText(annotated, std::to_string(g.id), g.centroid + cv::Point2f(7, -6),
                        cv::FONT_HERSHEY_SIMPLEX, 0.7, idTextColor, 2, cv::LINE_AA);
        }
    }
    cv::imwrite(imgPath, annotated);

    auto meanOf = [](const std::vector<double>& v) {
        return v.empty() ? 0.0 : std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    };
    auto stdevOf = [&](const std::vector<double>& v, double mean) {
        if (v.size() < 2) return 0.0;
        double s = 0.0;
        for (double x : v) s += (x - mean) * (x - mean);
        return std::sqrt(s / (v.size() - 1));
    };
    auto medianOf = [](std::vector<double> v) {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        size_t n = v.size();
        return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
    };

    double areaMean = meanOf(areas), lenMean = meanOf(lengths), widMean = meanOf(widths);
    double periMean = meanOf(perimeters);
    double circMean = meanOf(circularities), aspectMean = meanOf(aspectRatios);
    double hMean = meanOf(colorsH), sMean = meanOf(colorsS), vMean = meanOf(colorsV);

    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "\n=== " << inputPath << " ===\n";
    out << "Grains detected (raw blobs):        " << grains.size() << "\n";
    out << "Rejected (too small):               " << rejectedSize << "\n";
    if (opt.excludeBorder)
        out << "Rejected (touching image border):   " << rejectedBorder << "\n";
    if (oversizedBeforeSplitCount > 0)
        out << "Oversized before split attempts:    " << oversizedBeforeSplitCount << "\n";
    if (stillOversized > 0)
        out << "Still oversized after split attempts (kept, not discarded): " << stillOversized << "\n";
    if (stillNotOvalShaped > 0)
        out << "Still not oval-shaped after split attempts (kept, not discarded): " << stillNotOvalShaped << "\n";
    out << "Grains counted:                     " << kept.size() << "\n\n";

    out << "                mean      median    stdev     min       max\n";
    auto row = [&](const char* name, const std::vector<double>& v, double mean) {
        if (v.empty()) { out << name << "   (no data)\n"; return; }
        double mn = *std::min_element(v.begin(), v.end());
        double mx = *std::max_element(v.begin(), v.end());
        out << name << mean << "     " << medianOf(v) << "     "
            << stdevOf(v, mean) << "     " << mn << "     " << mx << "\n";
    };
    row("Area (mm2)    ", areas, areaMean);
    row("Length (mm)   ", lengths, lenMean);
    row("Width (mm)    ", widths, widMean);
    row("Perimeter (mm) ", perimeters, periMean);
    row("Circularity   ", circularities, circMean);
    row("Aspect ratio  ", aspectRatios, aspectMean);
    row("Hue (deg)     ", colorsH, hMean);
    row("Saturation (%)", colorsS, sMean);
    row("Value (%)     ", colorsV, vMean);

    out << "\nPer-grain CSV written to: " << csvPath << "\n";
    out << "Annotated QC image written to: " << imgPath << "\n";
    say(std::cout, out.str());

    result.ok = true;
    result.grainCount = static_cast<int>(kept.size());
    result.meanAreaMm2 = areaMean;
    result.meanLengthMm = lenMean;
    result.meanWidthMm = widMean;
    result.meanPerimeterMm = periMean;
    result.meanCircularity = circMean;
    result.meanAspectRatio = aspectMean;
    result.meanR = meanOf(colorsR);
    result.meanG = meanOf(colorsG);
    result.meanB = meanOf(colorsB);
    result.meanH = hMean;
    result.meanS = sMean;
    result.meanV = vMean;
    result.oversizedBeforeSplit = oversizedBeforeSplitCount;
    return result;
}

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 1;

    std::vector<std::string> files = resolveInputs(opt.rawInputs);
    if (files.empty()) {
        std::cerr << "Error: no input files found.\n";
        return 1;
    }

    std::error_code ec;
    fs::create_directories(opt.outDir, ec);

    bool isBatch = files.size() > 1;
    if (isBatch && (!opt.outCsvOverride.empty() || !opt.outImageOverride.empty())) {
        std::cerr << "Note: --out-csv/--out-image are ignored in batch mode ("
                   << files.size() << " input files); using --out-dir (\""
                   << opt.outDir << "\") with per-file default names instead.\n";
    }

    unsigned int hwThreads = opt.threads > 0 ? opt.threads : std::thread::hardware_concurrency();
    if (hwThreads == 0) hwThreads = 1;
    unsigned int numThreads = std::min<unsigned int>(hwThreads, static_cast<unsigned int>(files.size()));

    auto t0 = std::chrono::steady_clock::now();

    std::atomic<size_t> nextIndex{0};
    std::atomic<int> okCount{0}, failCount{0};
    std::atomic<long long> totalGrains{0};
    std::vector<ImageResult> results(files.size()); // one slot per file, indices are disjoint across threads

    auto worker = [&]() {
        while (true) {
            size_t idx = nextIndex.fetch_add(1);
            if (idx >= files.size()) break;
            ImageResult r = processImage(files[idx], opt, isBatch);
            results[idx] = r;
            if (r.ok) { okCount++; totalGrains += r.grainCount; }
            else { failCount++; }
        }
    };

    if (numThreads <= 1) {
        worker();
    } else {
        std::vector<std::thread> pool;
        pool.reserve(numThreads);
        for (unsigned int t = 0; t < numThreads; ++t) pool.emplace_back(worker);
        for (auto& th : pool) th.join();
    }

    auto t1 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    // Cross-file summary: one row per input file with seed count and
    // per-file averages, in the same order the files were given/resolved.
    std::string summaryPath = opt.summaryCsvOverride.empty()
        ? (fs::path(opt.outDir) / "summary.csv").string()
        : opt.summaryCsvOverride;
    {
        std::error_code sec;
        fs::path summaryParent = fs::path(summaryPath).parent_path();
        if (!summaryParent.empty()) fs::create_directories(summaryParent, sec);

        std::ofstream summary(summaryPath);
        summary << "filename,seed_count,mean_area_mm2,mean_length_mm,mean_width_mm,mean_perimeter_mm,"
                   "mean_circularity,mean_aspect_ratio,mean_r,mean_g,mean_b,mean_h,mean_s,mean_v,"
                   "oversized_before_split\n";
        summary << std::fixed;
        for (size_t i = 0; i < files.size(); ++i) {
            const ImageResult& r = results[i];
            // Quote the filename defensively in case a path contains a comma.
            std::string name = files[i];
            std::string escaped;
            escaped.reserve(name.size());
            for (char c : name) { if (c == '"') escaped += '"'; escaped += c; }

            summary << "\"" << escaped << "\",";
            if (!r.ok) {
                summary << "0,,,,,,,,,,,,,\n"; // failed file: seed count 0, blank averages
                continue;
            }
            summary << r.grainCount << ","
                    << std::setprecision(4) << r.meanAreaMm2 << ","
                    << r.meanLengthMm << "," << r.meanWidthMm << "," << r.meanPerimeterMm << ","
                    << r.meanCircularity << "," << r.meanAspectRatio << ","
                    << std::setprecision(1) << r.meanR << "," << r.meanG << "," << r.meanB << ","
                    << r.meanH << "," << r.meanS << "," << r.meanV << ","
                    << r.oversizedBeforeSplit
                    << "\n";
        }
        summary.close();
    }

    if (isBatch) {
        std::ostringstream ss;
        ss << "\n=== Batch summary ===\n"
           << "Files processed: " << okCount.load() << " ok, " << failCount.load() << " failed (of "
           << files.size() << ")\n"
           << "Total grains counted: " << totalGrains.load() << "\n"
           << "Elapsed: " << std::fixed << std::setprecision(2) << elapsed << " s using "
           << numThreads << " thread(s)\n"
           << "Per-file summary written to: " << summaryPath << "\n";
        say(std::cout, ss.str());
    } else {
        say(std::cout, "Per-file summary written to: " + summaryPath + "\n");
    }

    return failCount.load() > 0 ? 1 : 0;
}
