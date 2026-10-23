#include "charm_token.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"   /* EXT_RAM_BSS_ATTR: these buffers live in PSRAM */
#else
#define EXT_RAM_BSS_ATTR
#endif

#define N CHARM_TOKEN_N
#define G CHARM_TOKEN_GRID
#define DEPTH 3                 /* thickness, in sprite pixels */
#define CELL 1.55f              /* grid cells per sprite pixel */
#define SUB 4                   /* splats per pixel edge: dense enough to leave no holes when turned */
#define BEVEL 1.5f              /* how strongly the front face rounds off at its rim */

typedef struct {
    float x, y, z;
} v3_t;

static EXT_RAM_BSS_ATTR uint16_t s_sprite[N * N];
static EXT_RAM_BSS_ATTR float s_nx[N * N], s_ny[N * N];   /* front-face tilt, from the distance to the edge */
static EXT_RAM_BSS_ATTR float s_dist[N * N];
static EXT_RAM_BSS_ATTR uint16_t s_fb[G * G];
static EXT_RAM_BSS_ATTR int8_t s_zb[G * G];

static inline v3_t v3(float x, float y, float z)
{
    return (v3_t){ x, y, z };
}

static inline float dot3(v3_t a, v3_t b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline v3_t norm3(v3_t a)
{
    float k = 1.0f / sqrtf(dot3(a, a));
    return v3(a.x * k, a.y * k, a.z * k);
}

static inline bool solid(int x, int y)
{
    return (unsigned)x < N && (unsigned)y < N && s_sprite[y * N + x];
}

void charm_token_prepare(const uint16_t *sprite)
{
    memcpy(s_sprite, sprite, sizeof(s_sprite));

    /* Chebyshev distance to the nearest empty pixel, capped: the bevel's height field. */
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            int d = 0;
            if (solid(x, y)) {
                for (d = 1; d < 4; d++) {
                    bool edge = false;
                    for (int dy = -d; dy <= d && !edge; dy++) {
                        for (int dx = -d; dx <= d; dx++) {
                            if (!solid(x + dx, y + dy)) {
                                edge = true;
                                break;
                            }
                        }
                    }
                    if (edge) {
                        break;
                    }
                }
            }
            s_dist[y * N + x] = (float)d;
        }
    }
    /* The front face leans outwards where the height falls off. */
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            int i = y * N + x;
            float l = x > 0 ? s_dist[i - 1] : 0, r = x < N - 1 ? s_dist[i + 1] : 0;
            float u = y > 0 ? s_dist[i - N] : 0, d = y < N - 1 ? s_dist[i + N] : 0;
            /* Only the outer two pixels round off; the middle stays flat and clean. */
            float k = s_dist[i] <= 2 ? BEVEL / (1.0f + s_dist[i]) : 0.0f;
            s_nx[i] = (l - r) * 0.5f * k;
            s_ny[i] = (u - d) * 0.5f * k;   /* sprite y runs down; normal y runs up */
        }
    }
}

typedef struct {
    float m[3][3];
    v3_t light, half;
} view_t;

static inline v3_t rot(const view_t *v, v3_t p)
{
    return v3(v->m[0][0] * p.x + v->m[0][1] * p.y + v->m[0][2] * p.z,
              v->m[1][0] * p.x + v->m[1][1] * p.y + v->m[1][2] * p.z,
              v->m[2][0] * p.x + v->m[2][1] * p.y + v->m[2][2] * p.z);
}

/* Lambert in four flat bands plus a white glint: faceted, like the avatar. */
static uint16_t shade(const view_t *v, uint16_t c, v3_t n)
{
    float lam = dot3(n, v->light);
    float i = 0.5f + 0.85f * (lam > 0 ? lam : 0);   /* facing us: about true colour */
    i = floorf(i * 4.0f + 0.5f) / 4.0f;
    int r = ((c >> 11) & 31) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
    r = (int)(r * i);
    g = (int)(g * i);
    b = (int)(b * i);
    if (dot3(n, v->half) > 0.96f) {
        r = (r + 255) / 2;
        g = (g + 255) / 2;
        b = (b + 255) / 2;
    }
    r = r > 255 ? 255 : r;
    g = g > 255 ? 255 : g;
    b = b > 255 ? 255 : b;
    uint16_t out = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
    return out ? out : 0x0821;  /* never pure black: that's the background */
}

static inline void splat(const view_t *v, v3_t p, uint16_t color)
{
    v3_t q = rot(v, p);
    int x = (int)floorf(G * 0.5f + q.x * CELL);
    int y = (int)floorf(G * 0.5f - q.y * CELL);
    if ((unsigned)x >= G || (unsigned)y >= G) {
        return;
    }
    int z = (int)(q.z * 8.0f);   /* nearer is bigger */
    z = z < -127 ? -127 : (z > 127 ? 127 : z);
    int i = y * G + x;
    if (z > s_zb[i]) {
        s_zb[i] = (int8_t)z;
        s_fb[i] = color;
    }
}

void charm_token_render(float t, uint16_t *out)
{
    /* A slow rock and a slight nod; never far enough round to show the back. */
    float yaw = 0.62f * sinf(t * 1.1f);
    float pitch = 0.22f + 0.10f * sinf(t * 0.7f);
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    view_t v = {
        .m = {
            { cy, 0, sy },
            { sp * sy, cp, -sp * cy },
            { -cp * sy, sp, cp * cy },
        },
    };
    v.light = norm3(v3(-0.5f, 0.75f, 0.6f));
    v.half = norm3(v3(v.light.x, v.light.y, v.light.z + 1.0f));

    memset(s_fb, 0, sizeof(s_fb));
    memset(s_zb, 0x80, sizeof(s_zb));

    const float h = N * 0.5f, front = DEPTH * 0.5f;
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            int i = y * N + x;
            uint16_t c = s_sprite[i];
            if (!c) {
                continue;
            }
            float px = x - h, py = h - y - 1;   /* pixel's lower-left corner, y up */

            /* Front face, domed by the bevel normal. */
            uint16_t cf = shade(&v, c, rot(&v, norm3(v3(s_nx[i], s_ny[i], 1.0f))));
            for (int a = 0; a < SUB; a++) {
                for (int b = 0; b < SUB; b++) {
                    splat(&v, v3(px + (a + 0.5f) / SUB, py + (b + 0.5f) / SUB, front), cf);
                }
            }

            /* Side walls where a neighbour is empty: darker body colour. */
            static const int DX[4] = { -1, 1, 0, 0 }, DY[4] = { 0, 0, -1, 1 };
            for (int k = 0; k < 4; k++) {
                if (solid(x + DX[k], y + DY[k])) {
                    continue;
                }
                v3_t n = v3((float)DX[k], (float)-DY[k], 0);
                uint16_t cs = shade(&v, c, rot(&v, n));
                for (int a = 0; a < SUB; a++) {
                    float s = (a + 0.5f) / SUB;
                    for (int d = 0; d < DEPTH * SUB; d++) {
                        float z = front - (d + 0.5f) / SUB;
                        v3_t p = DX[k] ? v3(px + (DX[k] > 0 ? 1.0f : 0.0f), py + s, z)
                                       : v3(px + s, py + (DY[k] > 0 ? 0.0f : 1.0f), z);
                        splat(&v, p, cs);
                    }
                }
            }
        }
    }

    /* Nearest-neighbour blow-up, so the cells stay chunky. */
    for (int y = 0; y < CHARM_TOKEN_PX; y++) {
        const uint16_t *row = &s_fb[(y / CHARM_TOKEN_SCALE) * G];
        uint16_t *dst = &out[y * CHARM_TOKEN_PX];
        for (int x = 0; x < CHARM_TOKEN_PX; x++) {
            dst[x] = row[x / CHARM_TOKEN_SCALE];
        }
    }
}
