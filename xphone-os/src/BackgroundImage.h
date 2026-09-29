#pragma once

// User-supplied nap-screen background: a bare /background.bmp at the SD card
// root, dithered through the shared ImageThumb pipeline (see ImageThumb.h)
// and cached to /.xphone/background_<w>x<h>.bin so repeat naps just blit it.
//
// BMP ONLY, deliberately. The pipeline can also decode JPEG and PNG (the
// Reader's book covers do), but those run out of a single retained 21-58KB
// decoder scratch block that a BLE-fragmented heap routinely can't satisfy —
// and a full-screen target makes the JPEG MCU band several times larger than
// the cover-sized one it was tuned for. An uncompressed BMP needs no decoder
// library and no scratch at all: a row is already raw pixels, so the decode
// is just "read a row, convert to gray, feed it to the ditherer". That makes
// this the one format that can't fail for heap reasons on a napping device.
//
// Unlike reader::CoverThumb there is no EPUB to extract from — the source
// file sits at a fixed SD-root path — and cache invalidation can't use a
// modify time (HalStorage exposes no mtime accessor), so a small sidecar
// `.meta` file records the source file's byte size; a same-size in-place
// edit is the one case that won't invalidate the cache (acceptable for a
// rarely-changed wallpaper file).

#include <string>

namespace BackgroundImage {

// Cheap existence probe (no decode): true if /background.bmp exists at the
// SD root.
bool available();

// True when ensure(w, h) would be a straight cache hit — i.e. no decode, no
// multi-second wait. Lets the caller put a "preparing" frame on the glass
// only when one is actually warranted.
bool cached(int w, int h);

// Ensure the w x h cache exists (dithering the source if needed, or if its
// byte size has changed since the cache was built), and return its path plus
// the actual encoded pixel size (aspect-fit within w x h, never upscaled —
// so it can be smaller than requested; the caller centers it). False when no
// source file exists or the decode fails; every failure path logs why, since
// the caller can only fall back to the normal sleep face silently.
bool ensure(int w, int h, std::string* outPath, int* outW = nullptr, int* outH = nullptr);

}  // namespace BackgroundImage
