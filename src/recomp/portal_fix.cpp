// Room portal culling fix (docs/RENDERING.md, "Rooms vanishing behind doorways").
//
// The game draws the room Luigi is in plus every room seen through a doorway ("portal").
// fn_8001333C computes a portal's screen rectangle: it transforms the doorway's four corners
// (fn_80018238) by the camera's projection*view matrix, divides by w and takes the bounding
// box, intersected with the parent room's rectangle. An empty result means the room behind
// the portal is not drawn this frame.
//
// It never clips against the near plane. When the camera is close to a doorway and some of
// its corners are behind the camera (w < 0), dividing by a negative w mirrors those corners
// to the opposite side of the screen, the box lands off screen and the whole room behind the
// doorway disappears (e.g. the upper foyer during the cutscene when Luigi first enters).
// The game only rejects the case where all four corners are behind.
//
// This wrapper runs the original and, only when the doorway straddles the near plane, replaces
// the rectangle with the box of the doorway quad clipped to the near plane (z >= -w in GX clip
// space), mapped to the screen and intersected with the parent the same way.
#include "port/settings.h"
#include "recomp/guest_util.h"
#include "recomp/hle.h"
#include "recomp/version.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace recomp {
namespace {

struct Clip {
    double x, y, z, w;
};

s16 rdS16(u32 a) { return s16(rd16(a)); }
void wrRect(u32 out, const s16 r[4]) {
    for (int i = 0; i < 4; i++) wr16(out + 2 * i, u16(r[i]));
}

}  // namespace

// fn_8001333C(portal, out rect, parent rect): rects are s16 {x0, y0, x1, y1}.
HLE(fn_8001333C) {
    const u32 portal = c.r[3], out = c.r[4], parent = c.r[5];
    callOriginal(c, "fn_8001333C");
    // LM_PORTAL_FIX=0/1 overrides the setting (A/B testing).
    static const char* env = std::getenv("LM_PORTAL_FIX");
    if (env ? env[0] == '0' : !port::settings().enhancements.portalClipFix) return;

    // A closed portal (door shut) stays empty: the game checks this first (fn_8001B278).
    if (s8(gu::call(c, GU_SYM("fn_8001B278"), {rd32(portal + 4)})) != 0) return;

    static const u32 corners = hleAlloc(4 * 12);
    gu::call(c, GU_SYM("fn_80018238"), {portal, corners});

    // Camera state the original uses: the screen rectangle {x0, y0, x1, y1} at +0 and the
    // projection*view matrix at +0x258 of the same block (fn_80014228 builds it).
    const u32 cam = VA(0x803A3C98);
    const u32 mtx = cam + 0x258;
    Clip v[4];
    int behind = 0;
    for (int i = 0; i < 4; i++) {
        const double p[3] = {gu::rdf(corners + 12 * i), gu::rdf(corners + 12 * i + 4), gu::rdf(corners + 12 * i + 8)};
        double o[4];
        for (int r = 0; r < 4; r++) {
            const u32 row = mtx + 16 * r;
            o[r] = gu::rdf(row) * p[0] + gu::rdf(row + 4) * p[1] + gu::rdf(row + 8) * p[2] + gu::rdf(row + 12);
        }
        v[i] = {o[0], o[1], o[2], o[3]};
        if (o[2] + o[3] < 0.0) behind++;
    }
    // Entirely in front: the original's result is right. Entirely behind: nothing to see.
    if (behind == 0 || behind == 4) return;

    // Box of the quad clipped by z + w >= 0: the corners in front plus where every corner pair
    // crosses the plane (pairs, not just edges, so the corner order doesn't matter; the extra
    // points lie inside the clipped quad, so the box is the same).
    const double sx0 = rdS16(cam), sy0 = rdS16(cam + 2), sx1 = rdS16(cam + 4), sy1 = rdS16(cam + 6);
    double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
    auto add = [&](const Clip& p) {
        const double w = std::max(p.w, 1e-6);
        const double x = sx0 + (sx1 - sx0) * 0.5 * (p.x / w + 1.0);
        const double y = sy0 + (sy1 - sy0) * 0.5 * (1.0 - p.y / w);
        minX = std::min(minX, x), maxX = std::max(maxX, x);
        minY = std::min(minY, y), maxY = std::max(maxY, y);
    };
    for (int i = 0; i < 4; i++) {
        const double di = v[i].z + v[i].w;
        if (di >= 0.0) add(v[i]);
        for (int j = i + 1; j < 4; j++) {
            const double dj = v[j].z + v[j].w;
            if ((di < 0.0) == (dj < 0.0)) continue;
            const double t = di / (di - dj);
            add({v[i].x + (v[j].x - v[i].x) * t, v[i].y + (v[j].y - v[i].y) * t, v[i].z + (v[j].z - v[i].z) * t,
                 v[i].w + (v[j].w - v[i].w) * t});
        }
    }

    // Same margins as the game (one pixel each side), then intersect with the parent.
    const auto clampS16 = [](double d) { return s16(std::clamp(std::floor(d), -30000.0, 30000.0)); };
    s16 r[4] = {clampS16(minX - 1), clampS16(minY - 1), clampS16(maxX + 1), clampS16(maxY + 1)};
    const s16 px0 = rdS16(parent), py0 = rdS16(parent + 2), px1 = rdS16(parent + 4), py1 = rdS16(parent + 6);
    const s16 empty[4] = {0, 0, 0, 0};
    if (r[0] > px1 || r[1] > py1 || r[2] < px0 || r[3] < py0) return wrRect(out, empty);
    r[0] = std::max(r[0], px0), r[1] = std::max(r[1], py0);
    r[2] = std::min(r[2], px1), r[3] = std::min(r[3], py1);
    if (r[0] >= r[2] || r[1] >= r[3]) return wrRect(out, empty);
    wrRect(out, r);
}

}  // namespace recomp
