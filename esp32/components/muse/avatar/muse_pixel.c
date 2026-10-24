// Copyright (c) Meta Platforms, Inc. and affiliates.
/*
 * GUPPI: Andrew's assistant, drawn as a hovering crystal.
 *
 * Shape: the compound of a regular dodecahedron and a regular icosahedron
 * (the dual compound: edges cross at their midpoints), so it reads as a
 * symmetric star-ball of triangles and pentagons. Not a face, not a robot.
 * It's real 3D: 32 flat faces, rotated each frame, rasterised with a depth
 * buffer (the part-mask buffer reused, one byte per pixel) and flat-shaded
 * from a top-left light, so the edges come from shade changes alone.
 *
 * Colours: deep navy, mid blue, light blue and cyan-blue highlights on black;
 * the ramp is the mode's scheme, so ERROR turns red-violet and OFF fades to a
 * navy silhouette. A soft blue glow lies on an invisible floor beneath it.
 *
 * Personality: calm and precise. It floats, turns lazily, leans in to listen,
 * tumbles while it thinks, hops and sparkles when a reply is done.
 *
 * Bit, from Tron: speaking, it morphs into a yellow-orange octahedron (Bit's
 * "Yes"); petted (a tap), it turns red and morphs into the second stellation
 * of the icosahedron (Bit's "No", Wenninger W27, charm_shapes.h). A morph
 * shrinks one shape while the next grows through it, with a quick spin.
 *
 * The palette blend, set_size/scale and sparkle code are the default
 * avatar's (Apache-2.0, Meta); the crystal is new.
 */

#include "muse_pixel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W MUSE_PX_W
#define H MUSE_PX_H
#define TAU 6.2831853f

/* ---------------------------------------------------------------------------
 * Palette
 * ------------------------------------------------------------------------- */

enum {
    C_BG = 0,
    C_NAVY,      /* facets in shadow */
    C_F3,        /* facet ramp from the mode's scheme, deep ... */
    C_F2,
    C_F1,
    C_F0,        /* ... bright */
    C_HI,        /* specular highlight */
    C_RIM,       /* lit silhouette edge */
    C_AURA1,
    C_AURA2,
    C_SPK,
    C_ACC,
    C_WHITE,
    C_COUNT,
};

typedef struct {
    float r, g, b;
} rgb_t;

/* Per-mode facet ramp (bright -> deep) and accent. */
typedef struct {
    uint32_t f[4];
    uint32_t acc;
} scheme_t;

static const scheme_t SCHEMES[MUSE_MODE_COUNT] = {
    [MUSE_MODE_BOOT]      = { { 0xd8f2ff, 0x7cc4ff, 0x3a7ff0, 0x1a3c9e }, 0x8fd0ff },
    [MUSE_MODE_IDLE]      = { { 0xa8e4ff, 0x52a6ff, 0x2a62d8, 0x163890 }, 0x5aa8ff },
    [MUSE_MODE_LISTENING] = { { 0x9ef2ff, 0x4fd2f6, 0x2290dc, 0x13509c }, 0x5ff0ff },
    [MUSE_MODE_THINKING]  = { { 0x96d2ff, 0x468ef0, 0x2654c0, 0x132c7c }, 0x4a86e8 },
    [MUSE_MODE_SPEAKING]  = { { 0xfff0a8, 0xffc23c, 0xf08414, 0x8c3f06 }, 0xffb02e },   /* Bit's "Yes" */
    [MUSE_MODE_ERROR]     = { { 0xffc8ec, 0xd668d8, 0x9a2aa8, 0x4a1460 }, 0xe05cc0 },
    [MUSE_MODE_OFF]       = { { 0x24306a, 0x18224e, 0x111a3e, 0x0c1330 }, 0x1a2450 },
};

/* Petted: Bit's "No". */
static const scheme_t PET_SCHEME = { { 0xffb4a0, 0xff4a36, 0xc8160e, 0x640606 }, 0xff3a24 };

static const uint32_t FIXED[C_COUNT] = {
    [C_BG] = 0x000000,
    [C_WHITE] = 0xffffff,
};

static rgb_t s_scheme[5];      /* live, blended: f0..f3, acc */
static bool s_scheme_init;
static uint16_t s_pal[C_COUNT];
static uint16_t s_pal_dim[C_COUNT];

static uint8_t s_fb[W * H];
static uint8_t s_zbuf[W * H];  /* 0 = empty, else depth (bigger is nearer) */

static const uint8_t BAYER4[4][4] = {
    { 0, 8, 2, 10 },
    { 12, 4, 14, 6 },
    { 3, 11, 1, 9 },
    { 15, 7, 13, 5 },
};

static inline float bayer(int x, int y)
{
    return (BAYER4[y & 3][x & 3] + 0.5f) / 16.0f;
}

static inline rgb_t hex_rgb(uint32_t c)
{
    return (rgb_t){ (float)((c >> 16) & 0xff), (float)((c >> 8) & 0xff), (float)(c & 0xff) };
}

static inline rgb_t mix(rgb_t a, rgb_t b, float t)
{
    return (rgb_t){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

static inline rgb_t scale_rgb(rgb_t a, float k)
{
    return (rgb_t){ a.r * k, a.g * k, a.b * k };
}

static inline uint16_t to565(rgb_t c)
{
    int r = (int)(c.r + 0.5f), g = (int)(c.g + 0.5f), b = (int)(c.b + 0.5f);
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

uint32_t muse_pixel_accent(muse_mode_t mode)
{
    return SCHEMES[mode < MUSE_MODE_COUNT ? mode : MUSE_MODE_IDLE].acc;
}

static void update_palette(const scheme_t *target, float dt)
{
    rgb_t tgt[5];
    for (int i = 0; i < 4; i++) {
        tgt[i] = hex_rgb(target->f[i]);
    }
    tgt[4] = hex_rgb(target->acc);

    float k = s_scheme_init ? 1.0f - expf(-dt * 7.0f) : 1.0f;
    for (int i = 0; i < 5; i++) {
        s_scheme[i] = mix(s_scheme[i], tgt[i], k);
    }
    s_scheme_init = true;

    rgb_t pal[C_COUNT];
    for (int i = 0; i < C_COUNT; i++) {
        pal[i] = hex_rgb(FIXED[i]);
    }
    rgb_t acc = s_scheme[4];
    pal[C_F0] = s_scheme[0];
    pal[C_F1] = s_scheme[1];
    pal[C_F2] = s_scheme[2];
    pal[C_F3] = s_scheme[3];
    pal[C_NAVY] = scale_rgb(s_scheme[3], 0.36f);   /* shadow: the deepest shade, darker (navy on blue) */
    pal[C_HI] = mix(s_scheme[0], pal[C_WHITE], 0.45f);
    pal[C_ACC] = acc;
    pal[C_RIM] = mix(s_scheme[1], acc, 0.5f);
    pal[C_AURA1] = scale_rgb(acc, 0.16f);
    pal[C_AURA2] = scale_rgb(acc, 0.34f);
    pal[C_SPK] = mix(acc, pal[C_WHITE], 0.45f);

    for (int i = 0; i < C_COUNT; i++) {
        s_pal[i] = to565(pal[i]);
        /* The block edge shade gives the enlarged pixels a faint grid texture. */
        s_pal_dim[i] = to565(scale_rgb(pal[i], 0.72f));
    }
}

/* ---------------------------------------------------------------------------
 * Primitive helpers
 * ------------------------------------------------------------------------- */

static inline void px(int x, int y, uint8_t c)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        s_fb[y * W + x] = c;
    }
}

static inline int iround(float v)
{
    return (int)floorf(v + 0.5f);
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Eases a smoothed value toward its target, frame-rate independent. */
static inline float ease(float cur, float target, float dt, float rate)
{
    return cur + (target - cur) * (1.0f - expf(-dt * rate));
}

/* ---------------------------------------------------------------------------
 * Scene layers
 * ------------------------------------------------------------------------- */

/* The light it casts on an invisible floor: a flat, dithered ellipse. */
static void draw_floor_glow(float cx, float y, float half_w, float strength)
{
    if (strength <= 0.02f) {
        return;
    }
    for (int row = -2; row <= 2; row++) {
        float hw = half_w * (1.0f - row * row * 0.16f);
        for (int x = iround(cx - hw); x <= iround(cx + hw); x++) {
            float edge = fabsf(x + 0.5f - cx) / hw;
            float i = (1.0f - edge * edge) * (1.0f - fabsf((float)row) * 0.3f) * strength;
            float b = bayer(x, iround(y) + row);
            if (i > 0.62f + b * 0.3f) {
                px(x, iround(y) + row, C_AURA2);
            } else if (i > b * 0.85f) {
                px(x, iround(y) + row, C_AURA1);
            }
        }
    }
}

static void draw_sparkle(int x, int y, float twinkle, bool front)
{
    uint8_t arm = front ? C_ACC : C_AURA2;
    uint8_t core = front ? C_WHITE : C_SPK;
    if (twinkle > 0.8f) {
        px(x, y, core);
        for (int k = 1; k <= 2; k++) {
            uint8_t c = k == 1 ? (front ? C_SPK : arm) : arm;
            px(x + k, y, c);
            px(x - k, y, c);
            px(x, y + k, c);
            px(x, y - k, c);
        }
    } else if (twinkle > 0.45f) {
        px(x, y, front ? C_SPK : arm);
        px(x + 1, y, arm);
        px(x - 1, y, arm);
        px(x, y + 1, arm);
        px(x, y - 1, arm);
    } else if (twinkle > 0.15f) {
        px(x, y, arm);
    }
}

/* ---------------------------------------------------------------------------
 * The crystal: dodecahedron + icosahedron, dual compound
 * ------------------------------------------------------------------------- */

#define NV_D 20                 /* dodecahedron vertices */
#define NV_I 12                 /* icosahedron vertices */
#define NV (NV_D + NV_I)
#define NF (12 + 20)            /* 12 pentagons, 20 triangles */
#define SHADES 6                /* C_NAVY .. C_HI */

typedef struct {
    float x, y, z;
} v3_t;

typedef struct {
    uint8_t n;                  /* 3 to 5 */
    uint8_t v[5];               /* corners in order around it */
    v3_t nrm;                   /* outward unit normal */
    float order;                /* 0..1: when it assembles at boot */
} face_t;

static v3_t s_vert[NV];
static face_t s_face[NF];

/* A shape to draw: vertices times `radius`, convex faces. */
typedef struct {
    const v3_t *v;
    const face_t *f;
    int nv, nf;
    float radius;
} mesh_t;

#include "charm_shapes.h"        /* W27, Bit's "No" */

#define NV_OCT 6
static v3_t s_oct_v[NV_OCT];
static face_t s_oct_f[8];

enum { SHAPE_CRYSTAL, SHAPE_YES, SHAPE_NO };
static const mesh_t SHAPES[] = {
    [SHAPE_CRYSTAL] = { s_vert, s_face, NV, NF, 1.0f },
    [SHAPE_YES] = { s_oct_v, s_oct_f, NV_OCT, 8, 1.0f },
    [SHAPE_NO] = { W27_V, W27_F, W27_NV, W27_NF, 2.1f },   /* spike tips; the core is a third of that */
};
#define MAX_V (W27_NV > NV ? W27_NV : NV)

static inline v3_t v3(float x, float y, float z)
{
    return (v3_t){ x, y, z };
}

static inline float dot3(v3_t a, v3_t b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline v3_t cross3(v3_t a, v3_t b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

static inline v3_t norm3(v3_t a)
{
    float k = 1.0f / sqrtf(dot3(a, a));
    return v3(a.x * k, a.y * k, a.z * k);
}

/*
 * Each face of one solid sits under a vertex of the other: a pentagon of the
 * dodecahedron under each icosahedron vertex, a triangle of the icosahedron
 * under each dodecahedron vertex. Its corners are the other solid's vertices
 * nearest that direction, sorted around it.
 */
static void build_face(face_t *f, const v3_t *verts, v3_t dir, int first, int count, int n)
{
    v3_t nrm = norm3(dir);
    float best[5];
    int idx[5];
    int k = 0;
    for (int i = first; i < first + count; i++) {
        float d = dot3(verts[i], nrm);
        if (k < n) {
            best[k] = d;
            idx[k++] = i;
            continue;
        }
        int lo = 0;
        for (int j = 1; j < n; j++) {
            if (best[j] < best[lo]) {
                lo = j;
            }
        }
        if (d > best[lo]) {
            best[lo] = d;
            idx[lo] = i;
        }
    }
    v3_t u = norm3(cross3(nrm, fabsf(nrm.y) < 0.9f ? v3(0, 1, 0) : v3(1, 0, 0)));
    v3_t w = cross3(nrm, u);
    float ang[5];
    for (int i = 0; i < n; i++) {
        v3_t p = verts[idx[i]];
        ang[i] = atan2f(dot3(p, w), dot3(p, u));
    }
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && ang[j] < ang[j - 1]; j--) {
            float ta = ang[j]; ang[j] = ang[j - 1]; ang[j - 1] = ta;
            int ti = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = ti;
        }
    }
    f->n = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        f->v[i] = (uint8_t)idx[i];
    }
    f->nrm = nrm;
}

static void init_crystal(void)
{
    const float P = 1.6180340f, IP = 0.6180340f;
    int k = 0;
    /* Dodecahedron: cube corners plus three golden rectangles. */
    for (int i = 0; i < 8; i++) {
        s_vert[k++] = v3(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1);
    }
    for (int i = 0; i < 4; i++) {
        float a = i & 1 ? P : -P, b = i & 2 ? IP : -IP;
        s_vert[k++] = v3(0, a, b);
        s_vert[k++] = v3(b, 0, a);
        s_vert[k++] = v3(a, b, 0);
    }
    /* Icosahedron, turned so its edges cross the dodecahedron's at their midpoints. */
    for (int i = 0; i < 4; i++) {
        float a = i & 1 ? 1 : -1, b = i & 2 ? P : -P;
        s_vert[k++] = v3(0, a, b);
        s_vert[k++] = v3(a, b, 0);
        s_vert[k++] = v3(b, 0, a);
    }
    int f = 0;
    for (int i = 0; i < NV_I; i++) {
        build_face(&s_face[f++], s_vert, s_vert[NV_D + i], 0, NV_D, 5);
    }
    for (int i = 0; i < NV_D; i++) {
        build_face(&s_face[f++], s_vert, s_vert[i], NV_D, NV_I, 3);
    }
    /* Boot order: a stable shuffle, so the facets arrive scattered. */
    for (int i = 0; i < NF; i++) {
        uint32_t h = (uint32_t)(i + 1) * 2654435761u;
        h ^= h >> 15;
        s_face[i].order = (float)(h & 1023) / 1023.0f;
    }
    /* Bit's "Yes": an octahedron as tall as the crystal's tips; a face under each cube corner. */
    const float R = 1.9f;
    for (int i = 0; i < 3; i++) {
        float a[3] = { 0, 0, 0 };
        a[i] = R;
        s_oct_v[2 * i] = v3(a[0], a[1], a[2]);
        s_oct_v[2 * i + 1] = v3(-a[0], -a[1], -a[2]);
    }
    for (int i = 0; i < 8; i++) {
        build_face(&s_oct_f[i], s_oct_v, v3(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1), 0, NV_OCT, 3);
    }
}

typedef struct {
    float cx, cy;           /* screen centre */
    float size;             /* px per model unit */
    float yaw, pitch, roll;
    float bright;           /* shade offset, in ramp steps */
    float fade;             /* 1 lit .. 0 silhouette */
    float assemble;         /* boot: faces with order < assemble are drawn; >1 = all */
} crystal_t;

/* Projected vertices, 1/16 px, and rotated depth: the mesh being drawn. In
 * PSRAM on the device: W27's 182 vertices would cost internal RAM. */
#ifndef EXT_RAM_BSS_ATTR
#if __has_include("esp_attr.h")
#include "esp_attr.h"
#endif
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif
static EXT_RAM_BSS_ATTR int32_t s_sx[MAX_V], s_sy[MAX_V];
static EXT_RAM_BSS_ATTR float s_sz[MAX_V];

/* Rasterises one convex face into s_fb/s_zbuf with a 2-colour ordered dither. */
static void raster_face(const face_t *f, uint8_t lo, uint8_t hi, int thr)
{
    int n = f->n;
    int32_t xs[5], ys[5];
    int32_t minx = INT32_MAX, maxx = INT32_MIN, miny = INT32_MAX, maxy = INT32_MIN;
    for (int i = 0; i < n; i++) {
        xs[i] = s_sx[f->v[i]];
        ys[i] = s_sy[f->v[i]];
        minx = xs[i] < minx ? xs[i] : minx;
        maxx = xs[i] > maxx ? xs[i] : maxx;
        miny = ys[i] < miny ? ys[i] : miny;
        maxy = ys[i] > maxy ? ys[i] : maxy;
    }
    int64_t area = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += (int64_t)xs[i] * ys[j] - (int64_t)xs[j] * ys[i];
    }
    if (area > -64 && area < 64) {
        return;                  /* edge-on */
    }
    int sgn = area > 0 ? 1 : -1;

    int x0 = (minx >> 4) < 0 ? 0 : (minx >> 4);
    int x1 = (maxx >> 4) > W - 1 ? W - 1 : (maxx >> 4);
    int y0 = (miny >> 4) < 0 ? 0 : (miny >> 4);
    int y1 = (maxy >> 4) > H - 1 ? H - 1 : (maxy >> 4);
    if (x0 > x1 || y0 > y1) {
        return;
    }

    /* Edge functions at the first pixel centre and their steps, all integer. */
    int32_t e0[5], ex[5], ey[5];
    int32_t px0 = x0 * 16 + 8, py0 = y0 * 16 + 8;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        int32_t dx = xs[j] - xs[i], dy = ys[j] - ys[i];
        e0[i] = sgn * ((py0 - ys[i]) * dx - (px0 - xs[i]) * dy);
        ex[i] = -sgn * dy * 16;
        ey[i] = sgn * dx * 16;
    }

    /* Depth plane through three corners, in screen space (Q8 depth units). */
    float ax = xs[0] / 16.0f, ay = ys[0] / 16.0f, az = s_sz[f->v[0]];
    float bx = xs[1] / 16.0f - ax, by = ys[1] / 16.0f - ay, bz = s_sz[f->v[1]] - az;
    float cx = xs[2] / 16.0f - ax, cy = ys[2] / 16.0f - ay, cz = s_sz[f->v[2]] - az;
    float det = bx * cy - by * cx;
    if (fabsf(det) < 1e-4f) {
        return;
    }
    float dzdx = (bz * cy - by * cz) / det, dzdy = (bx * cz - bz * cx) / det;
    const float ZS = 52.0f * 256.0f;          /* model z -> depth byte, Q8 */
    int32_t zx = (int32_t)(dzdx * ZS), zy = (int32_t)(dzdy * ZS);
    int32_t zrow = (int32_t)((128.0f + (az + (x0 + 0.5f - ax) * dzdx + (y0 + 0.5f - ay) * dzdy) * 52.0f) * 256.0f);

    for (int y = y0; y <= y1; y++) {
        int32_t e[5];
        for (int i = 0; i < n; i++) {
            e[i] = e0[i];
        }
        int32_t z = zrow;
        const uint8_t *bay = BAYER4[y & 3];
        for (int x = x0; x <= x1; x++) {
            bool in = true;
            for (int i = 0; i < n; i++) {
                in &= e[i] >= 0;
                e[i] += ex[i];
            }
            if (in) {
                int32_t zb = z >> 8;
                zb = zb < 1 ? 1 : (zb > 255 ? 255 : zb);
                if (zb >= s_zbuf[y * W + x]) {
                    s_zbuf[y * W + x] = (uint8_t)zb;
                    s_fb[y * W + x] = bay[x & 3] < thr ? hi : lo;
                }
            }
            z += zx;
        }
        for (int i = 0; i < n; i++) {
            e0[i] += ey[i];
        }
        zrow += zy;
    }
}

static void draw_crystal(const crystal_t *c, const mesh_t *mesh, float scale)
{
    /* Rotation: yaw about y, then pitch about x, then roll about z. */
    float cyw = cosf(c->yaw), syw = sinf(c->yaw);
    float cp = cosf(c->pitch), sp = sinf(c->pitch);
    float cr = cosf(c->roll), sr = sinf(c->roll);
    float m[3][3] = {
        { cr * cyw - sr * sp * syw, -sr * cp, cr * syw + sr * sp * cyw },
        { sr * cyw + cr * sp * syw, cr * cp, sr * syw - cr * sp * cyw },
        { -cp * syw, sp, cp * cyw },
    };
#define ROT(v) v3(m[0][0] * (v).x + m[0][1] * (v).y + m[0][2] * (v).z, \
                  m[1][0] * (v).x + m[1][1] * (v).y + m[1][2] * (v).z, \
                  m[2][0] * (v).x + m[2][1] * (v).y + m[2][2] * (v).z)

    float k = mesh->radius * scale;
    for (int i = 0; i < mesh->nv; i++) {
        v3_t sv = v3(mesh->v[i].x * k, mesh->v[i].y * k, mesh->v[i].z * k);
        v3_t r = ROT(sv);
        float persp = 1.0f / (1.0f - r.z * 0.07f);   /* a touch of perspective */
        s_sx[i] = (int32_t)((c->cx + r.x * c->size * persp) * 16.0f);
        s_sy[i] = (int32_t)((c->cy - r.y * c->size * persp) * 16.0f);
        s_sz[i] = r.z;
    }

    const v3_t L = norm3(v3(-0.45f, 0.78f, 0.55f));     /* top-left, toward us */
    const v3_t HV = norm3(v3(L.x, L.y, L.z + 1.0f));    /* half vector for the shine */
    static const uint8_t RAMP[SHADES] = { C_NAVY, C_F3, C_F2, C_F1, C_F0, C_HI };

    for (int i = 0; i < mesh->nf; i++) {
        const face_t *f = &mesh->f[i];
        if (f->order >= c->assemble) {
            continue;
        }
        v3_t n = ROT(f->nrm);
        if (n.z < -0.25f) {
            continue;            /* well behind: the depth test would hide it anyway */
        }
        float lam = dot3(n, L);
        float lev = 0.3f + (lam > 0 ? lam * 2.6f : lam * 0.5f);
        float spec = dot3(n, HV);
        if (spec > 0.93f) {
            lev += (spec - 0.93f) * 24.0f;          /* bright cyan crowns on the upper facets */
        }
        /* A face still arriving at boot glows in from the dark. */
        float arrive = clampf((c->assemble - f->order) * 6.0f, 0, 1);
        lev = (lev + c->bright) * c->fade * arrive;
        lev = clampf(lev, 0, SHADES - 1.001f);
        int lo = (int)lev;
        float frac = lev - lo;
        /* Flat colour per facet, so the facets stay bold, not noisy. */
        int thr = frac < 0.5f ? 0 : 16;
        raster_face(f, RAMP[lo], RAMP[lo + 1], thr);
    }
#undef ROT
}

/* Lit silhouette edge: pixels of the crystal whose upper or left neighbour is empty. */
static void draw_rim(float strength)
{
    if (strength < 0.3f) {
        return;
    }
    for (int y = 1; y < H; y++) {
        for (int x = 1; x < W; x++) {
            int i = y * W + x;
            if (s_zbuf[i] && (!s_zbuf[i - W] || !s_zbuf[i - 1]) && s_fb[i] != C_NAVY) {
                s_fb[i] = C_RIM;
            }
        }
    }
}

/* Sparkles on the icosahedron's outermost tips (happy). */
static void draw_tip_sparkles(const crystal_t *c, float t, float amount)
{
    for (int i = NV_D; i < NV; i++) {
        if (s_sz[i] < 0.1f) {
            continue;
        }
        float sx = s_sx[i] / 16.0f - c->cx, sy = s_sy[i] / 16.0f - c->cy;
        if (sx * sx + sy * sy < (c->size * 1.45f) * (c->size * 1.45f)) {
            continue;            /* only the points on the outline */
        }
        float tw = (0.5f + 0.5f * sinf(t * 11.0f + i * 2.3f)) * amount * 1.2f;
        draw_sparkle(iround(c->cx + sx * 1.08f), iround(c->cy + sy * 1.08f), tw, true);
    }
}

/* ---------------------------------------------------------------------------
 * Frame
 * ------------------------------------------------------------------------- */

/*
 * Screen pixel -> grid cell, with 0x80 set on a cell's last screen pixel. When
 * cells are 3+ pixels, that edge is drawn dimmer so the pixel grid shows.
 */
#define MAP_MAX 512
static uint8_t s_map[MAP_MAX];
static int s_size;

void muse_pixel_set_size(int px)
{
    s_size = px < MAP_MAX ? px : MAP_MAX;
    bool grid = s_size >= 3 * W;
    for (int i = 0; i < s_size; i++) {
        int cell = i * W / s_size;
        bool edge = grid && (i + 1) * W / s_size != cell;
        s_map[i] = (uint8_t)(cell | (edge ? 0x80 : 0));
    }
}

void muse_pixel_scale(uint16_t *dst, int stride_px, int x0, int x1, int y0, int y1)
{
    int n = x1 - x0 + 1;
    const uint8_t *xmap = &s_map[x0];
    const uint16_t *prev = NULL;
    uint8_t prev_m = 0;
    for (int y = y0; y <= y1; y++, dst += stride_px) {
        uint8_t m = s_map[y];
        if (prev && m == prev_m) {
            memcpy(dst, prev, n * sizeof(uint16_t));
            continue;
        }
        const uint8_t *row = &s_fb[(m & 0x7f) * W];
        if (m & 0x80) {
            for (int i = 0; i < n; i++) {
                dst[i] = s_pal_dim[row[xmap[i] & 0x7f]];
            }
        } else {
            for (int i = 0; i < n; i++) {
                uint8_t xm = xmap[i];
                uint8_t c = row[xm & 0x7f];
                dst[i] = xm & 0x80 ? s_pal_dim[c] : s_pal[c];
            }
        }
        prev = dst;
        prev_m = m;
    }
}

/* Motion that carries across frames and modes, eased so nothing pops. */
static struct {
    float last_t;
    float rot;          /* yaw, accumulated */
    float rot_speed;
    float bob_amp;
    float scale;
    float glow;
    float bright;
    float roll;
} s_m = { .last_t = -1.0f, .rot = 0.6f, .rot_speed = 0.35f, .bob_amp = 2.5f, .scale = 1.0f, .glow = 0.6f };

/* The shape morph: from s_from to s_shape as s_morph runs 0 -> 1. */
#define MORPH_S 0.45f
static int s_shape = SHAPE_CRYSTAL, s_from = SHAPE_CRYSTAL;
static float s_morph = 1.0f;

static void morph_toward(int target, float dt)
{
    if (target != s_shape) {
        /* Mid-morph, the shape on screen is whichever is more than half there. */
        s_from = s_morph < 0.5f ? s_from : s_shape;
        s_shape = target;
        s_morph = s_from == target ? 1.0f : 0.0f;
    }
    s_morph = clampf(s_morph + dt / MORPH_S, 0, 1);
}

void muse_pixel_render(const muse_pose_t *p)
{
    static bool s_init;
    if (!s_init) {
        init_crystal();
        s_init = true;
    }
    float dt = s_m.last_t >= 0 ? clampf(p->t - s_m.last_t, 0, 0.2f) : 0.04f;
    s_m.last_t = p->t;

    muse_mode_t mode = p->mode < MUSE_MODE_COUNT ? p->mode : MUSE_MODE_IDLE;
    float t = p->t, mt = p->mode_t, level = clampf(p->level, 0, 1);
    float happy = mode == MUSE_MODE_ERROR ? 0.0f : clampf(p->happy, 0, 1);
    bool pet = p->pet > 0 && mode != MUSE_MODE_ERROR && mode != MUSE_MODE_OFF && mode != MUSE_MODE_BOOT;
    float fade = mode == MUSE_MODE_OFF ? clampf(1.0f - mt / 1.3f, 0, 1) : 1.0f;

    update_palette(pet ? &PET_SCHEME : &SCHEMES[mode], dt);
    morph_toward(pet ? SHAPE_NO : mode == MUSE_MODE_SPEAKING ? SHAPE_YES : SHAPE_CRYSTAL, dt);
    memset(s_fb, C_BG, sizeof(s_fb));
    memset(s_zbuf, 0, sizeof(s_zbuf));

    /* ---- per-mode targets ---- */
    float rot_speed = 0.35f, bob_amp = 2.5f, scale = 1.0f, glow = 0.6f, bright = 0.0f, roll = 0.0f;
    float dx = 0, dy = 0, pitch_wobble = 0.10f * sinf(t * 0.5f);
    switch (mode) {
    case MUSE_MODE_BOOT:
        rot_speed = 0.9f;
        break;
    case MUSE_MODE_LISTENING:
        /* Still, leaning in, glow up, pulsing with the voice it hears. */
        rot_speed = 0.12f;
        bob_amp = 0.0f;
        scale = 1.06f;
        glow = 1.0f + level * 0.5f;
        bright = 0.15f + (0.5f + 0.5f * sinf(t * 4.2f)) * 0.3f + level * 0.5f;
        break;
    case MUSE_MODE_THINKING:
        /* Tumbling in place; glow dims a touch. */
        rot_speed = 1.7f;
        bob_amp = 1.0f;
        glow = 0.42f;
        pitch_wobble = 0.35f * sinf(t * 1.3f);
        break;
    case MUSE_MODE_SPEAKING: {
        /* Small quick bounces in an uneven rhythm; brightness rides them. */
        float b = fabsf(sinf(t * 7.7f)) * 0.6f + fabsf(sinf(t * 12.3f + 1.1f)) * 0.4f;
        rot_speed = 0.5f;
        bob_amp = 0.6f;
        dy = -b * (1.0f + level * 1.6f);
        bright = b * 0.45f + level * 0.25f;
        glow = 0.65f + b * 0.3f;
        break;
    }
    case MUSE_MODE_ERROR: {
        /* A hard shudder, then a nervous tremble; tilted. */
        float k = mt < 0.6f ? 1.0f : 0.35f;
        rot_speed = 0.0f;
        bob_amp = 0.0f;
        roll = 0.24f;
        dx = sinf(t * 47.0f) * 1.6f * k;
        dy = sinf(t * 39.0f + 0.7f) * 0.6f * k;
        glow = 0.5f;
        break;
    }
    case MUSE_MODE_OFF:
        rot_speed = 0.0f;
        bob_amp = 0.0f;
        glow = 0.0f;
        break;
    default:
        break;
    }
    if (pet) {
        /* Bit's "No": a brisk spin in place, bright and glowing red. */
        rot_speed = 1.4f;
        bob_amp = 0.4f;
        dy = 0;
        bright = 0.35f;
        glow = 0.9f;
    }

    float k_fast = 1.0f - expf(-dt * 6.0f);
    s_m.rot_speed += (rot_speed - s_m.rot_speed) * k_fast;
    s_m.rot += (s_m.rot_speed + 5.0f * sinf(3.1416f * s_morph)) * dt;   /* a whirl through each morph */
    if (s_m.rot > TAU) {
        s_m.rot -= TAU;
    }
    s_m.bob_amp = ease(s_m.bob_amp, bob_amp, dt, 4.0f);
    s_m.scale = ease(s_m.scale, scale, dt, 8.0f);
    s_m.glow = ease(s_m.glow, glow * fade, dt, 6.0f);
    s_m.bright = ease(s_m.bright, bright, dt, 14.0f);
    s_m.roll = ease(s_m.roll, roll, dt, 10.0f);

    /* Boot: facets arrive scattered, a brightening pulse, then it settles. */
    float assemble = 2.0f, boot_scale = 1.0f, boot_pulse = 0.0f;
    if (mode == MUSE_MODE_BOOT) {
        assemble = clampf((mt - 0.1f) / 0.8f, 0, 2.0f);
        float g = clampf(mt / 0.9f, 0, 1);
        boot_scale = 0.78f + 0.22f * g + sinf(g * 3.1416f) * 0.05f;
        float d = mt - 1.05f;
        boot_pulse = 1.6f * expf(-d * d / 0.04f);
    }

    /* Happy: one or two excited hops and the brightest blue of all. */
    float hop = fabsf(sinf(t * 4.2f)) * 4.0f * happy;

    float bob = sinf(t * 1.5f) * s_m.bob_amp;
    crystal_t c;
    c.cx = 32.0f + dx;
    c.cy = 29.0f + bob + dy - hop;
    c.size = 12.4f * s_m.scale * boot_scale;
    c.yaw = s_m.rot;
    c.pitch = 0.42f + pitch_wobble;
    c.roll = s_m.roll;
    c.bright = s_m.bright + boot_pulse + happy * 1.1f;
    c.fade = 0.18f + 0.82f * fade;
    c.assemble = assemble;

    /* ---- background: halo and the glow on the floor ---- */
    float lift = (29.0f - c.cy) * 0.05f;            /* higher = smaller, fainter floor glow */
    float glow_s = s_m.glow + happy * 0.4f;
    draw_floor_glow(32.0f, 59.5f, 15.0f * (1.0f - lift), glow_s * 1.5f * (1.0f - lift));

    /* ---- the crystal: mid-morph, the old shape shrinks as the new one grows through it ---- */
    float k = s_morph * s_morph * (3 - 2 * s_morph);
    if (k < 0.98f) {
        draw_crystal(&c, &SHAPES[s_from], 1.0f - 0.65f * k);
    }
    if (k > 0.02f) {
        draw_crystal(&c, &SHAPES[s_shape], 0.35f + 0.65f * k);
    }
    draw_rim(fade * clampf(assemble, 0, 1));

    /* Sparkles sit on the crystal's tips, so only once it's whole again. */
    if (happy > 0.05f && s_shape == SHAPE_CRYSTAL && k >= 0.98f) {
        draw_tip_sparkles(&c, t, happy);
    }
}
