#pragma once

#include <stdint.h>

/*
 * Turns a notification's flat 16x16 pixel art into a chunky 3D token: each
 * pixel becomes a block, the sprite gets thickness and bevelled, lit edges,
 * and it rocks gently, in the avatar's faceted style. Plain C, no LVGL, so it
 * builds on a desktop for previews.
 */

#define CHARM_TOKEN_N 16                        /* sprite is N x N, RGB565, 0 = transparent */
#define CHARM_TOKEN_GRID 32                     /* rendered on a GRID x GRID cell grid ... */
#define CHARM_TOKEN_SCALE 5                     /* ... blown up SCALE x */
#define CHARM_TOKEN_PX (CHARM_TOKEN_GRID * CHARM_TOKEN_SCALE)

/* Once per new sprite: precomputes the bevel. */
void charm_token_prepare(const uint16_t *sprite);

/* Each frame: draws the token at time t (s) into out, CHARM_TOKEN_PX square RGB565, black behind. */
void charm_token_render(float t, uint16_t *out);
