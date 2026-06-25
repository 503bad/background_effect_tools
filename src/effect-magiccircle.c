/* Magic circle line-art. Three patterns, each composed of an ordered list of
 * primitives (segments + arcs). The figure self-draws along that path (描き上げ)
 * over draw_time, holds, then fades out (消去) and loops. All strokes go through
 * the shared glowing-line shader so glow/bloom/emissive are consistent with the
 * rest of the plugin; the whole figure rotates and can react to audio.
 *
 *   0  circle + hexagram + moon
 *   1  circle + pentagram + moon + sun
 *   2  circle + up/down triangles, a small magic circle at each vertex
 */

#include "effect-magiccircle.h"
#include "bgfx-glowline.h"
#include "bgfx-props.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <math.h>

#define PRE "magic"
#define BG_TAU 6.28318530718f
#define MC_MAXPRIM 1600

enum mc_pattern {
	MC_HEXAGRAM = 0,
	MC_PENTAGRAM = 1,
	MC_TRIANGLES = 2,
	MC_SOLAR = 3,   /* sun core + sun/moon ring + octagon + runes     */
	MC_LUNAR = 4,   /* crescent core + moon-phase ring + star/pentagon */
	MC_ASTRAL = 5,  /* {12/5} + {7/3} stars + bead ring + glyph band   */
	MC_RUNIC = 6,   /* heptagram + two rune rings + vertex suns        */
	MC_ZODIAC = 7,  /* 12 spokes, alternating sun/moon/star/rune       */
	MC_SIGIL = 8,   /* dense octagram+heptagram, sun+moon, rune ring   */
	MC_RUNECROWN = 9, /* petal rim + twin rune rings (rune-heavy)      */
	MC_SCALLOP = 10,  /* floral petal/scallop outer rim                */
	MC_GEARED = 11,   /* gear-tooth rim + spokes (mechanical)          */
	MC_TRINITY = 12,  /* three mini seals in a triangle (stacked)      */
	MC_CONSTELLATION = 13, /* central seal + orbiting satellite seals  */
	MC_VESICA = 14,   /* two overlapping seals (vesica piscis)         */
};

struct mc_prim {
	int   kind;   /* 0 segment, 1 arc                    */
	int   accent; /* 0 line colour, 1 accent colour      */
	float x0, y0, x1, y1;          /* segment            */
	float cx, cy, r, a0, a1;       /* arc                */
	int   seg;                     /* arc subdivisions   */
	float len;                     /* path length, px    */
};

struct mc_state {
	gs_effect_t *line;

	int      pattern;
	uint32_t color, accent;
	float    thickness;  /* full stroke width, px           */
	float    rot_speed;  /* degrees per second              */
	float    draw_time;  /* seconds to draw the figure       */
	float    hold_time;  /* seconds held fully drawn         */
	float    fade_in;    /* alpha ramp-in, seconds           */
	float    fade_out;   /* alpha ramp-out (erase), seconds  */
	bool     loop;
	float    flicker;    /* 0..1 brightness flicker depth    */
	float    size;       /* radius as fraction of min(w,h)/2 */
	float    audio_amt;  /* 0..2 audio drive for glow/spin   */
	struct bg_post post; /* glow / bloom / emissive (no flare) */

	/* runtime */
	float c0[4], c1[4];
	float clock;     /* animation timeline, seconds */
	float angle;     /* rotation, radians           */
	float draw_frac; /* 0..1 portion drawn this frame */
	float anim_a;    /* 0..1 fade envelope this frame */
};

/* ---- helpers ------------------------------------------------------------- */

static void add_seg(struct mc_prim *pr, int *n, int accent, float x0, float y0,
		    float x1, float y1)
{
	if (*n >= MC_MAXPRIM)
		return;
	struct mc_prim *p = &pr[(*n)++];
	p->kind = 0;
	p->accent = accent;
	p->x0 = x0;
	p->y0 = y0;
	p->x1 = x1;
	p->y1 = y1;
	float dx = x1 - x0, dy = y1 - y0;
	p->len = sqrtf(dx * dx + dy * dy);
}

static void add_arc(struct mc_prim *pr, int *n, int accent, float cx, float cy,
		    float r, float a0, float a1, int seg)
{
	if (*n >= MC_MAXPRIM)
		return;
	struct mc_prim *p = &pr[(*n)++];
	p->kind = 1;
	p->accent = accent;
	p->cx = cx;
	p->cy = cy;
	p->r = r;
	p->a0 = a0;
	p->a1 = a1;
	p->seg = seg < 1 ? 1 : seg;
	p->len = r * fabsf(a1 - a0);
}

static void add_circle(struct mc_prim *pr, int *n, int accent, float cx,
		       float cy, float r)
{
	int seg = (int)(r * 0.5f);
	if (seg < 24)
		seg = 24;
	if (seg > 96)
		seg = 96;
	add_arc(pr, n, accent, cx, cy, r, 0.0f, BG_TAU, seg);
}

static void add_polygon(struct mc_prim *pr, int *n, int accent, float cx,
			float cy, float r, int sides, float rot)
{
	for (int i = 0; i < sides; ++i) {
		float a = rot + BG_TAU * (float)i / (float)sides;
		float b = rot + BG_TAU * (float)(i + 1) / (float)sides;
		add_seg(pr, n, accent, cx + r * cosf(a), cy + r * sinf(a),
			cx + r * cosf(b), cy + r * sinf(b));
	}
}

/* Star polygon {pts/step}: pentagram (5,2), hexagram (6,2). */
static void add_star(struct mc_prim *pr, int *n, int accent, float cx, float cy,
		     float r, int pts, int step, float rot)
{
	for (int i = 0; i < pts; ++i) {
		float a = rot + BG_TAU * (float)i / (float)pts;
		float b = rot + BG_TAU * (float)((i + step) % pts) / (float)pts;
		add_seg(pr, n, accent, cx + r * cosf(a), cy + r * sinf(a),
			cx + r * cosf(b), cy + r * sinf(b));
	}
}

/* Crescent moon as two circles: a full ring plus an offset inner ring. */
static void add_moon(struct mc_prim *pr, int *n, int accent, float cx, float cy,
		     float r)
{
	add_circle(pr, n, accent, cx, cy, r);
	add_circle(pr, n, accent, cx + r * 0.5f, cy - r * 0.1f, r * 0.82f);
}

/* Sun: a small ring with short radial rays. */
static void add_sun(struct mc_prim *pr, int *n, int accent, float cx, float cy,
		    float r)
{
	add_circle(pr, n, accent, cx, cy, r * 0.55f);
	for (int i = 0; i < 8; ++i) {
		float a = BG_TAU * (float)i / 8.0f;
		add_seg(pr, n, accent, cx + r * 0.7f * cosf(a),
			cy + r * 0.7f * sinf(a), cx + r * cosf(a),
			cy + r * sinf(a));
	}
}

/* A small fixed-segment circle (cheap, for runic dots / motifs). */
static void add_smallcircle(struct mc_prim *pr, int *n, int accent, float cx,
			    float cy, float r)
{
	add_arc(pr, n, accent, cx, cy, r, 0.0f, BG_TAU, 12);
}

/* Radial tick marks between `inner` and `outer` radius (a runic band). */
static void add_tick_ring(struct mc_prim *pr, int *n, int accent, float cx,
			  float cy, float inner, float outer, int count,
			  float rot)
{
	for (int i = 0; i < count; ++i) {
		float a = rot + BG_TAU * (float)i / (float)count;
		float ca = cosf(a), sa = sinf(a);
		add_seg(pr, n, accent, cx + inner * ca, cy + inner * sa,
			cx + outer * ca, cy + outer * sa);
	}
}

/* A ring of small circles (beads) at radius r. */
static void add_dot_ring(struct mc_prim *pr, int *n, int accent, float cx,
			 float cy, float r, float dotr, int count, float rot)
{
	for (int i = 0; i < count; ++i) {
		float a = rot + BG_TAU * (float)i / (float)count;
		add_smallcircle(pr, n, accent, cx + r * cosf(a),
				cy + r * sinf(a), dotr);
	}
}

/* Decorative glyph band: a chain of little diamonds (each with a centre bead)
 * filling the annulus between r0 and r1 — the "文様" in the gaps. */
static void add_glyph_band(struct mc_prim *pr, int *n, int accent, float cx,
			   float cy, float r0, float r1, int count, float rot)
{
	if (count < 1)
		count = 1;
	float rm = (r0 + r1) * 0.5f;
	float tw = (BG_TAU / (float)count) * 0.36f; /* angular half-width */
	for (int i = 0; i < count; ++i) {
		float a = rot + BG_TAU * (float)i / (float)count;
		float ix = cx + r0 * cosf(a), iy = cy + r0 * sinf(a);
		float ox = cx + r1 * cosf(a), oy = cy + r1 * sinf(a);
		float lx = cx + rm * cosf(a - tw), ly = cy + rm * sinf(a - tw);
		float rx = cx + rm * cosf(a + tw), ry = cy + rm * sinf(a + tw);
		add_seg(pr, n, accent, ix, iy, lx, ly);
		add_seg(pr, n, accent, lx, ly, ox, oy);
		add_seg(pr, n, accent, ox, oy, rx, ry);
		add_seg(pr, n, accent, rx, ry, ix, iy);
		add_smallcircle(pr, n, accent, cx + rm * cosf(a),
				cy + rm * sinf(a), (r1 - r0) * 0.14f);
	}
}

/* Radial spokes from `inner` to `outer` at `count` evenly spaced angles. */
static void add_spokes(struct mc_prim *pr, int *n, int accent, float cx,
		       float cy, float inner, float outer, int count, float rot)
{
	for (int i = 0; i < count; ++i) {
		float a = rot + BG_TAU * (float)i / (float)count;
		float ca = cosf(a), sa = sinf(a);
		add_seg(pr, n, accent, cx + inner * ca, cy + inner * sa,
			cx + outer * ca, cy + outer * sa);
	}
}

/* A stack of rotated regular polygons (interlocking lattice). */
static void add_poly_stack(struct mc_prim *pr, int *n, int accent, float cx,
			   float cy, float r, int sides, int layers, float rot)
{
	for (int l = 0; l < layers; ++l)
		add_polygon(pr, n, accent, cx, cy, r,
			    sides, rot + (BG_TAU / (float)sides) *
					  (float)l / (float)layers);
}

/* Pseudo-random rune glyph (文字): a stroke "alphabet" built from a 2×3 node
 * grid. The left stave is always inked; the rest of the edges are chosen by the
 * seed so each slot reads as a distinct arcane letter. `rot` orients the glyph,
 * (gx,gy) is its centre, `h` its half-height. */
static void add_rune(struct mc_prim *pr, int *n, int accent, float gx, float gy,
		     float h, float rot, uint32_t seed)
{
	static const float nx[6] = {-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f};
	static const float ny[6] = {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f};
	static const int E[13][2] = {
		{0, 2}, {2, 4},                 /* left stave (always)     */
		{1, 3}, {3, 5},                 /* right stave             */
		{0, 1}, {2, 3}, {4, 5},         /* cross bars              */
		{0, 3}, {2, 1}, {2, 5}, {3, 4}, /* diagonals               */
		{0, 5}, {1, 4},                 /* long diagonals          */
	};
	float c = cosf(rot), s = sinf(rot);
	float hw = h * 0.6f;
	uint32_t bits = seed * 2654435761u + 1013904223u;
	bits ^= bits >> 15;
	for (int e = 0; e < 13; ++e) {
		bool on = (e < 2) ? true : ((bits >> e) & 1u) != 0;
		if (!on)
			continue;
		float ax = nx[E[e][0]] * hw, ay = ny[E[e][0]] * h;
		float bx = nx[E[e][1]] * hw, by = ny[E[e][1]] * h;
		add_seg(pr, n, accent, gx + ax * c - ay * s,
			gy + ax * s + ay * c, gx + bx * c - by * s,
			gy + bx * s + by * c);
	}
}

/* A ring of rune glyphs facing outward. */
static void add_rune_ring(struct mc_prim *pr, int *n, int accent, float cx,
			  float cy, float r, float h, int count, float rot)
{
	for (int i = 0; i < count; ++i) {
		float a = rot + BG_TAU * (float)i / (float)count;
		add_rune(pr, n, accent, cx + r * cosf(a), cy + r * sinf(a), h,
			 a + BG_TAU * 0.25f, (uint32_t)(i * 7 + 3));
	}
}

/* Outward petal / scallop rim: a chain of bulging semicircles on the ring at
 * radius r (a flower / cloud-edge border). */
static void add_petal_ring(struct mc_prim *pr, int *n, int accent, float cx,
			   float cy, float r, float petalR, int count, float rot)
{
	for (int i = 0; i < count; ++i) {
		float a = rot + BG_TAU * (float)i / (float)count;
		float px = cx + r * cosf(a), py = cy + r * sinf(a);
		add_arc(pr, n, accent, px, py, petalR, a - BG_TAU * 0.25f,
			a + BG_TAU * 0.25f, 8); /* outward-facing semicircle */
	}
}

/* Sawtooth / gear-tooth rim: triangular teeth pointing outward from `inner`
 * to `outer`. */
static void add_teeth_ring(struct mc_prim *pr, int *n, int accent, float cx,
			   float cy, float inner, float outer, int count,
			   float rot)
{
	for (int i = 0; i < count; ++i) {
		float a0 = rot + BG_TAU * (float)i / (float)count;
		float a1 = rot + BG_TAU * (float)(i + 1) / (float)count;
		float am = (a0 + a1) * 0.5f;
		float b0x = cx + inner * cosf(a0), b0y = cy + inner * sinf(a0);
		float b1x = cx + inner * cosf(a1), b1y = cy + inner * sinf(a1);
		float tx = cx + outer * cosf(am), ty = cy + outer * sinf(am);
		add_seg(pr, n, accent, b0x, b0y, tx, ty);
		add_seg(pr, n, accent, tx, ty, b1x, b1y);
	}
}

/* A compact, self-contained mini magic circle (ring + tick band + star + rune
 * ring + a centre icon) used to compose multi-seal layouts. `variant` shifts
 * the star order, spin direction and centre icon so stacked seals differ. */
static void add_mini_seal(struct mc_prim *pr, int *n, float cx, float cy,
			  float r, float t, int variant)
{
	float dir = (variant & 1) ? 1.0f : -1.0f;
	add_circle(pr, n, 0, cx, cy, r);
	add_circle(pr, n, 0, cx, cy, r * 0.84f);
	add_tick_ring(pr, n, 1, cx, cy, r * 0.84f, r, 18, t * 0.3f * dir);
	int sides = 5 + (variant % 4); /* 5..8 */
	int step = sides >= 7 ? 3 : 2;
	add_star(pr, n, 1, cx, cy, r * 0.66f, sides, step,
		 -BG_TAU * 0.25f + t * 0.2f * dir);
	add_rune_ring(pr, n, 1, cx, cy, r * 0.4f, r * 0.13f, 6, -t * 0.25f * dir);
	int ic = variant % 3;
	if (ic == 0)
		add_sun(pr, n, 1, cx, cy, r * 0.2f);
	else if (ic == 1)
		add_moon(pr, n, 1, cx, cy, r * 0.2f);
	else
		add_polygon(pr, n, 1, cx, cy, r * 0.22f, 3, t * 0.3f);
}

/* Shared ornate scaffold wrapped around every pattern's core: nested rings,
 * a runic tick band, a counter-rotating glyph band, a bead ring and spokes.
 * `t` is the animation clock so the layers drift against each other. */
static void add_scaffold(struct mc_state *s, struct mc_prim *pr, int *n,
			 float cx, float cy, float R)
{
	float t = s->clock;
	/* Outer double ring + runic tick band in the gap. */
	add_circle(pr, n, 0, cx, cy, R);
	add_circle(pr, n, 0, cx, cy, R * 0.965f);
	add_tick_ring(pr, n, 1, cx, cy, R * 0.965f, R, 72, t * 0.10f);
	add_tick_ring(pr, n, 0, cx, cy, R * 0.94f, R * 0.965f, 36,
		      -t * 0.18f + 0.04f);

	/* Counter-rotating decorative glyph band (文様). */
	add_circle(pr, n, 0, cx, cy, R * 0.86f);
	add_glyph_band(pr, n, 1, cx, cy, R * 0.74f, R * 0.86f, 24, -t * 0.22f);
	add_circle(pr, n, 0, cx, cy, R * 0.74f);

	/* Bead ring + spokes tying the rings together. */
	add_dot_ring(pr, n, 1, cx, cy, R * 0.70f, R * 0.022f, 36, t * 0.30f);
	add_spokes(pr, n, 0, cx, cy, R * 0.5f, R * 0.70f, 12, t * 0.06f);

	/* Inner ring pair the core sits inside. */
	add_circle(pr, n, 0, cx, cy, R * 0.5f);
	add_circle(pr, n, 0, cx, cy, R * 0.46f);
}

/* Build the chosen pattern into pr[], return primitive count. */
static int build_pattern(struct mc_state *s, struct mc_prim *pr, float cx,
			 float cy, float R)
{
	int n = 0;
	float t = s->clock;
	float Rc = R * 0.46f; /* core radius (inside the scaffold rings) */

	add_scaffold(s, pr, &n, cx, cy, R);

	switch (s->pattern) {
	case MC_PENTAGRAM:
		/* Two counter-rotating pentagrams + a pentagon lattice. */
		add_star(pr, &n, 1, cx, cy, Rc, 5, 2, -BG_TAU * 0.25f + t * 0.12f);
		add_star(pr, &n, 1, cx, cy, Rc * 0.62f, 5, 2,
			 -BG_TAU * 0.25f - t * 0.20f);
		add_poly_stack(pr, &n, 0, cx, cy, Rc * 0.82f, 5, 2, t * 0.05f);
		add_circle(pr, &n, 0, cx, cy, Rc * 0.34f);
		/* moon + sun riding the inner bead ring */
		add_moon(pr, &n, 1, cx + Rc * 0.9f * cosf(t * 0.3f),
			 cy + Rc * 0.9f * sinf(t * 0.3f), R * 0.07f);
		add_sun(pr, &n, 1, cx + Rc * 0.9f * cosf(t * 0.3f + BG_TAU * 0.5f),
			cy + Rc * 0.9f * sinf(t * 0.3f + BG_TAU * 0.5f), R * 0.08f);
		break;
	case MC_TRIANGLES:
		/* Interlocking counter-rotating triangle lattice + vertex sigils. */
		add_polygon(pr, &n, 0, cx, cy, Rc, 3, -BG_TAU * 0.25f + t * 0.10f);
		add_polygon(pr, &n, 1, cx, cy, Rc, 3, BG_TAU * 0.25f - t * 0.10f);
		add_poly_stack(pr, &n, 0, cx, cy, Rc * 0.6f, 3, 3, t * 0.16f);
		add_circle(pr, &n, 0, cx, cy, Rc * 0.3f);
		for (int i = 0; i < 6; ++i) {
			float rot = (i < 3) ? -BG_TAU * 0.25f : BG_TAU * 0.25f;
			float a = rot + BG_TAU * (float)(i % 3) / 3.0f + t * 0.10f;
			float vx = cx + Rc * cosf(a);
			float vy = cy + Rc * sinf(a);
			add_smallcircle(pr, &n, 1, vx, vy, R * 0.05f);
			add_polygon(pr, &n, 1, vx, vy, R * 0.07f, 3, -a - t * 0.4f);
		}
		break;
	case MC_SOLAR: {
		/* Sun core, octagon lattice, alternating sun/crescent ring,
		 * inner rune ring. */
		add_sun(pr, &n, 1, cx, cy, Rc * 0.5f);
		add_poly_stack(pr, &n, 0, cx, cy, Rc * 0.95f, 8, 2, t * 0.05f);
		add_circle(pr, &n, 0, cx, cy, Rc * 0.7f);
		for (int i = 0; i < 8; ++i) {
			float a = t * 0.12f + BG_TAU * (float)i / 8.0f;
			float vx = cx + Rc * 0.72f * cosf(a);
			float vy = cy + Rc * 0.72f * sinf(a);
			if (i & 1)
				add_moon(pr, &n, 1, vx, vy, Rc * 0.13f);
			else
				add_sun(pr, &n, 1, vx, vy, Rc * 0.16f);
		}
		add_rune_ring(pr, &n, 1, cx, cy, Rc * 0.34f, Rc * 0.1f, 8,
			      -t * 0.2f);
		break;
	}
	case MC_LUNAR: {
		/* Big crescent core inside a star + pentagon, a moon-phase ring
		 * and a rune ring. */
		add_moon(pr, &n, 1, cx, cy, Rc * 0.42f);
		add_polygon(pr, &n, 0, cx, cy, Rc * 0.9f, 5,
			    -BG_TAU * 0.25f + t * 0.06f);
		add_star(pr, &n, 1, cx, cy, Rc * 0.9f, 5, 2,
			 -BG_TAU * 0.25f - t * 0.10f);
		add_circle(pr, &n, 0, cx, cy, Rc * 0.6f);
		for (int i = 0; i < 8; ++i) {
			float a = t * 0.15f + BG_TAU * (float)i / 8.0f;
			add_moon(pr, &n, 1, cx + Rc * 0.75f * cosf(a),
				 cy + Rc * 0.75f * sinf(a), Rc * 0.12f);
		}
		add_rune_ring(pr, &n, 1, cx, cy, Rc * 0.3f, Rc * 0.09f, 7,
			      t * 0.18f);
		break;
	}
	case MC_ASTRAL:
		/* Twin star polygons {12/5} & {7/3}, dodecagon lattice, bead +
		 * glyph bands and a central sun. */
		add_star(pr, &n, 1, cx, cy, Rc, 12, 5, t * 0.08f);
		add_star(pr, &n, 1, cx, cy, Rc * 0.66f, 7, 3, -t * 0.14f);
		add_poly_stack(pr, &n, 0, cx, cy, Rc * 0.92f, 12, 2, t * 0.04f);
		add_dot_ring(pr, &n, 1, cx, cy, Rc * 0.5f, R * 0.018f, 12,
			     -t * 0.4f);
		add_glyph_band(pr, &n, 1, cx, cy, Rc * 0.3f, Rc * 0.42f, 12,
			       t * 0.25f);
		add_sun(pr, &n, 1, cx, cy, Rc * 0.22f);
		break;
	case MC_RUNIC:
		/* Heptagram with two concentric rune rings, vertex suns, central
		 * crescent — the most letter-heavy figure. */
		add_star(pr, &n, 1, cx, cy, Rc, 7, 3, -BG_TAU * 0.25f + t * 0.10f);
		add_circle(pr, &n, 0, cx, cy, Rc * 0.82f);
		add_rune_ring(pr, &n, 1, cx, cy, Rc * 0.66f, Rc * 0.16f, 12,
			      t * 0.12f);
		add_rune_ring(pr, &n, 0, cx, cy, Rc * 0.4f, Rc * 0.12f, 9,
			      -t * 0.2f);
		for (int i = 0; i < 7; ++i) {
			float a = -BG_TAU * 0.25f + t * 0.10f +
				  BG_TAU * (float)i / 7.0f;
			add_sun(pr, &n, 1, cx + Rc * cosf(a), cy + Rc * sinf(a),
				Rc * 0.1f);
		}
		add_moon(pr, &n, 1, cx, cy, Rc * 0.2f);
		break;
	case MC_ZODIAC:
		/* A dodecagram with 12 spokes whose tips cycle through
		 * sun / moon / star / rune. */
		add_star(pr, &n, 1, cx, cy, Rc, 12, 5, t * 0.06f);
		add_polygon(pr, &n, 0, cx, cy, Rc * 0.5f, 12, -t * 0.08f);
		add_spokes(pr, &n, 0, cx, cy, Rc * 0.5f, Rc * 0.92f, 12,
			   t * 0.04f);
		for (int i = 0; i < 12; ++i) {
			float a = t * 0.04f + BG_TAU * (float)i / 12.0f;
			float vx = cx + Rc * 0.92f * cosf(a);
			float vy = cy + Rc * 0.92f * sinf(a);
			int m = i % 4;
			if (m == 0)
				add_sun(pr, &n, 1, vx, vy, Rc * 0.11f);
			else if (m == 1)
				add_moon(pr, &n, 1, vx, vy, Rc * 0.09f);
			else if (m == 2)
				add_star(pr, &n, 1, vx, vy, Rc * 0.10f, 5, 2, a);
			else
				add_rune(pr, &n, 1, vx, vy, Rc * 0.10f,
					 a + BG_TAU * 0.25f, (uint32_t)(i * 5 + 1));
		}
		add_sun(pr, &n, 1, cx, cy, Rc * 0.18f);
		break;
	case MC_SIGIL:
		/* The grand seal: interlocked octagram + heptagram, hex/triangle
		 * lattice, a rune ring and a sun/moon pair. */
		add_star(pr, &n, 1, cx, cy, Rc, 8, 3, t * 0.08f);
		add_star(pr, &n, 1, cx, cy, Rc, 7, 3, -t * 0.12f);
		add_poly_stack(pr, &n, 0, cx, cy, Rc * 0.7f, 6, 2, t * 0.05f);
		add_polygon(pr, &n, 1, cx, cy, Rc * 0.7f, 3, -t * 0.05f);
		add_rune_ring(pr, &n, 1, cx, cy, Rc * 0.55f, Rc * 0.13f, 12,
			      t * 0.15f);
		add_sun(pr, &n, 1, cx + Rc * 0.28f, cy, Rc * 0.12f);
		add_moon(pr, &n, 1, cx - Rc * 0.28f, cy, Rc * 0.12f);
		add_dot_ring(pr, &n, 1, cx, cy, Rc * 0.3f, R * 0.015f, 8,
			     -t * 0.5f);
		add_circle(pr, &n, 0, cx, cy, Rc * 0.2f);
		break;

	/* ---- patterns with a different outer rim / composed seals ---- */
	case MC_RUNECROWN:
		/* Petal rim + two big rune rings: a crown of letters. */
		add_circle(pr, &n, 0, cx, cy, R);
		add_circle(pr, &n, 0, cx, cy, R * 0.97f);
		add_petal_ring(pr, &n, 1, cx, cy, R * 0.97f, R * 0.045f, 36,
			       t * 0.06f);
		add_rune_ring(pr, &n, 1, cx, cy, R * 0.85f, R * 0.07f, 24,
			      -t * 0.10f);
		add_circle(pr, &n, 0, cx, cy, R * 0.76f);
		add_rune_ring(pr, &n, 0, cx, cy, R * 0.65f, R * 0.06f, 18,
			      t * 0.14f);
		add_circle(pr, &n, 0, cx, cy, R * 0.54f);
		add_star(pr, &n, 1, cx, cy, Rc, 7, 3, -BG_TAU * 0.25f + t * 0.10f);
		add_rune_ring(pr, &n, 1, cx, cy, Rc * 0.55f, Rc * 0.16f, 9,
			      -t * 0.20f);
		add_sun(pr, &n, 1, cx, cy, Rc * 0.25f);
		break;
	case MC_SCALLOP:
		/* Floral scalloped rim around a pentagram + ten-gon. */
		add_circle(pr, &n, 0, cx, cy, R);
		add_petal_ring(pr, &n, 1, cx, cy, R * 0.9f, R * 0.085f, 18,
			       t * 0.05f);
		add_circle(pr, &n, 0, cx, cy, R * 0.8f);
		add_tick_ring(pr, &n, 0, cx, cy, R * 0.72f, R * 0.8f, 48,
			      -t * 0.10f);
		add_circle(pr, &n, 0, cx, cy, R * 0.72f);
		add_star(pr, &n, 1, cx, cy, Rc, 5, 2, -BG_TAU * 0.25f + t * 0.12f);
		add_polygon(pr, &n, 0, cx, cy, Rc * 0.8f, 10, t * 0.05f);
		add_dot_ring(pr, &n, 1, cx, cy, Rc * 0.5f, R * 0.02f, 10,
			     -t * 0.3f);
		add_moon(pr, &n, 1, cx, cy, Rc * 0.22f);
		break;
	case MC_GEARED:
		/* Gear-tooth rim + many spokes: a mechanical seal. */
		add_circle(pr, &n, 0, cx, cy, R);
		add_teeth_ring(pr, &n, 1, cx, cy, R * 0.9f, R, 30, t * 0.04f);
		add_circle(pr, &n, 0, cx, cy, R * 0.9f);
		add_circle(pr, &n, 0, cx, cy, R * 0.84f);
		add_spokes(pr, &n, 0, cx, cy, R * 0.6f, R * 0.84f, 24, t * 0.03f);
		add_circle(pr, &n, 0, cx, cy, R * 0.6f);
		add_star(pr, &n, 1, cx, cy, Rc, 8, 3, t * 0.08f);
		add_poly_stack(pr, &n, 0, cx, cy, Rc * 0.7f, 8, 2, -t * 0.05f);
		add_dot_ring(pr, &n, 1, cx, cy, Rc * 0.45f, R * 0.018f, 16,
			     t * 0.20f);
		add_sun(pr, &n, 1, cx, cy, Rc * 0.2f);
		break;
	case MC_TRINITY:
		/* Three mini seals in a triangle, linked by an outer ring. */
		add_circle(pr, &n, 0, cx, cy, R);
		add_circle(pr, &n, 0, cx, cy, R * 0.94f);
		add_tick_ring(pr, &n, 1, cx, cy, R * 0.94f, R, 60, t * 0.05f);
		add_polygon(pr, &n, 0, cx, cy, R * 0.5f, 3,
			    -BG_TAU * 0.25f + t * 0.05f);
		for (int i = 0; i < 3; ++i) {
			float a = -BG_TAU * 0.25f + t * 0.05f +
				  BG_TAU * (float)i / 3.0f;
			add_mini_seal(pr, &n, cx + R * 0.5f * cosf(a),
				      cy + R * 0.5f * sinf(a), R * 0.3f, t, i);
		}
		add_star(pr, &n, 1, cx, cy, R * 0.16f, 3, 1, -t * 0.2f);
		break;
	case MC_CONSTELLATION:
		/* Central seal ringed by six small orbiting seals. */
		add_circle(pr, &n, 0, cx, cy, R);
		add_dot_ring(pr, &n, 1, cx, cy, R * 0.96f, R * 0.012f, 48,
			     t * 0.04f);
		add_circle(pr, &n, 0, cx, cy, R * 0.62f);
		for (int i = 0; i < 6; ++i) {
			float a = t * 0.06f + BG_TAU * (float)i / 6.0f;
			add_mini_seal(pr, &n, cx + R * 0.62f * cosf(a),
				      cy + R * 0.62f * sinf(a), R * 0.17f,
				      t * 1.3f, i);
		}
		add_star(pr, &n, 1, cx, cy, R * 0.34f, 6, 2,
			 -BG_TAU * 0.25f - t * 0.10f);
		add_rune_ring(pr, &n, 1, cx, cy, R * 0.2f, R * 0.06f, 8, t * 0.20f);
		add_sun(pr, &n, 1, cx, cy, R * 0.12f);
		break;
	case MC_VESICA: {
		/* Two overlapping mid-size seals (vesica piscis) with a rune
		 * column where they cross. */
		float off = R * 0.34f;
		add_mini_seal(pr, &n, cx - off, cy, R * 0.62f, t, 0);
		add_mini_seal(pr, &n, cx + off, cy, R * 0.62f, t * 1.1f, 1);
		add_star(pr, &n, 1, cx, cy, R * 0.24f, 5, 2,
			 -BG_TAU * 0.25f + t * 0.12f);
		add_rune_ring(pr, &n, 1, cx, cy, R * 0.16f, R * 0.08f, 5,
			      -t * 0.15f);
		break;
	}
	case MC_HEXAGRAM:
	default:
		/* Two counter-rotating hexagrams + a hex lattice + crescent. */
		add_star(pr, &n, 1, cx, cy, Rc, 6, 2, -BG_TAU * 0.25f + t * 0.10f);
		add_star(pr, &n, 1, cx, cy, Rc * 0.7f, 6, 2,
			 -BG_TAU * 0.25f - t * 0.16f);
		add_poly_stack(pr, &n, 0, cx, cy, Rc * 0.85f, 6, 2, t * 0.06f);
		add_dot_ring(pr, &n, 1, cx, cy, Rc * 0.5f, R * 0.02f, 6, -t * 0.5f);
		add_moon(pr, &n, 1, cx, cy, R * 0.13f);
		break;
	}
	return n;
}

/* Emit the path up to draw_frac of its total length. */
static void draw_path(const struct mc_prim *pr, int n, float frac, float hw,
		      float reach, uint32_t cl, uint32_t ca)
{
	float total = 0.0f;
	for (int i = 0; i < n; ++i)
		total += pr[i].len;
	if (total <= 0.0f)
		return;
	float budget = frac * total;
	float acc = 0.0f;
	for (int i = 0; i < n; ++i) {
		float remain = budget - acc;
		if (remain <= 0.0f)
			break;
		const struct mc_prim *p = &pr[i];
		uint32_t col = p->accent ? ca : cl;
		if (p->len <= remain) {
			if (p->kind == 0)
				bg_glow_seg(col, p->x0, p->y0, p->x1, p->y1, hw,
					    reach);
			else
				bg_glow_arc(col, p->cx, p->cy, p->r, p->a0,
					    p->a1, p->seg, hw, reach);
			acc += p->len;
		} else {
			float f = remain / p->len;
			if (p->kind == 0) {
				float ex = p->x0 + (p->x1 - p->x0) * f;
				float ey = p->y0 + (p->y1 - p->y0) * f;
				bg_glow_seg(col, p->x0, p->y0, ex, ey, hw,
					    reach);
			} else {
				float aa = p->a0 + (p->a1 - p->a0) * f;
				int sg = (int)(p->seg * f + 0.5f);
				if (sg < 1)
					sg = 1;
				bg_glow_arc(col, p->cx, p->cy, p->r, p->a0, aa,
					    sg, hw, reach);
			}
			break;
		}
	}
}

/* ---- lifecycle ----------------------------------------------------------- */

static void *mc_create(void)
{
	return bzalloc(sizeof(struct mc_state));
}

static void mc_destroy(void *data)
{
	struct mc_state *s = data;
	if (!s)
		return;
	if (s->line)
		gs_effect_destroy(s->line);
	bfree(s);
}

static void mc_load_graphics(void *data)
{
	struct mc_state *s = data;
	s->line = bg_glowline_load_effect();
}

static double getd(obs_data_t *s, const char *n)
{
	char k[96];
	return obs_data_get_double(s, bg_key(k, sizeof(k), PRE, n));
}

static void mc_update(void *data, obs_data_t *settings)
{
	struct mc_state *s = data;
	char k[96];
	s->pattern = (int)obs_data_get_int(settings,
					   bg_key(k, sizeof(k), PRE, "pattern"));
	s->color = (uint32_t)obs_data_get_int(settings,
					      bg_key(k, sizeof(k), PRE, "color"));
	s->accent = (uint32_t)obs_data_get_int(
		settings, bg_key(k, sizeof(k), PRE, "accent"));
	s->thickness = (float)getd(settings, "thickness");
	s->rot_speed = (float)getd(settings, "speed");
	s->draw_time = (float)getd(settings, "draw_time");
	s->hold_time = (float)getd(settings, "hold_time");
	s->fade_in = (float)getd(settings, "fade_in");
	s->fade_out = (float)getd(settings, "fade_out");
	s->loop = obs_data_get_bool(settings, bg_key(k, sizeof(k), PRE, "loop"));
	s->flicker = (float)getd(settings, "flicker");
	s->size = (float)getd(settings, "size");
	s->audio_amt = (float)getd(settings, "audio");
	bg_post_update(&s->post, settings, PRE); /* glow/bloom/emissive (flare 0) */

	bg_unpack_color(s->color, s->c0);
	bg_unpack_color(s->accent, s->c1);
}

static void mc_reset(void *data, uint32_t seed)
{
	struct mc_state *s = data;
	UNUSED_PARAMETER(seed);
	s->clock = 0.0f;
	s->angle = 0.0f;
}

static void mc_tick(void *data, const struct bg_ctx *ctx, float dt)
{
	struct mc_state *s = data;
	s->clock += dt;

	float spin = s->rot_speed * (BG_TAU / 360.0f);
	if (ctx->audio.enabled && s->audio_amt > 0.0f)
		spin *= 1.0f + s->audio_amt * ctx->audio.level;
	s->angle += spin * dt;

	float draw = s->draw_time < 0.01f ? 0.01f : s->draw_time;
	float cycle = draw + s->hold_time + s->fade_out;
	float ph = s->clock;
	if (s->loop && cycle > 0.0f) {
		ph = fmodf(s->clock, cycle);
	} else if (!s->loop) {
		float end = draw + s->hold_time; /* no erase when not looping */
		if (ph > end)
			ph = end;
	}

	s->draw_frac = ph / draw;
	if (s->draw_frac > 1.0f)
		s->draw_frac = 1.0f;

	float a = 1.0f;
	if (s->fade_in > 0.01f && ph < s->fade_in)
		a = ph / s->fade_in;
	if (s->loop && s->fade_out > 0.01f) {
		float fo_start = draw + s->hold_time;
		if (ph > fo_start)
			a *= 1.0f - (ph - fo_start) / s->fade_out;
	}
	s->anim_a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
}

static void mc_render(void *data, const struct bg_ctx *ctx)
{
	struct mc_state *s = data;
	if (!s->line || s->anim_a <= 0.003f)
		return;

	float w = (float)ctx->width, h = (float)ctx->height;
	float cx = w * 0.5f, cy = h * 0.5f;
	float R = fminf(w, h) * 0.5f * s->size;
	float hw = s->thickness * 0.5f;
	if (hw < 0.5f)
		hw = 0.5f;

	struct mc_prim pr[MC_MAXPRIM];
	int n = build_pattern(s, pr, cx, cy, R);

	/* Brightness flicker (ちらつき). */
	float flick = 1.0f;
	if (s->flicker > 0.0f) {
		float p = s->clock * 7.0f;
		float wob = 0.5f + 0.5f * sinf(p) * cosf(p * 0.37f + 1.3f);
		flick = 1.0f - s->flicker * wob;
	}
	float al = s->c0[3] * s->anim_a * flick;
	float aa = s->c1[3] * s->anim_a * flick;
	uint32_t cl = bg_pack_rgba(s->c0[0], s->c0[1], s->c0[2], al);
	uint32_t ca = bg_pack_rgba(s->c1[0], s->c1[1], s->c1[2], aa);

	/* Audio drive lifts the light. */
	struct bg_post post = s->post;
	bg_glowline_audio_post(&post, ctx, s->audio_amt);

	bg_glowline_set_params(s->line, &post, BG_GLOW_REACH);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE); /* additive light */
	while (gs_effect_loop(s->line, "Draw")) {
		gs_matrix_push();
		gs_matrix_translate3f(cx, cy, 0.0f);
		gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, s->angle);
		gs_matrix_translate3f(-cx, -cy, 0.0f);
		gs_render_start(true);
		draw_path(pr, n, s->draw_frac, hw, BG_GLOW_REACH, cl, ca);
		gs_render_stop(GS_TRIS);
		gs_matrix_pop();
	}
	gs_blend_state_pop();
}

/* ---- properties ---------------------------------------------------------- */

static void mc_properties(obs_properties_t *g, obs_data_t *settings)
{
	char k[96];
	UNUSED_PARAMETER(settings);
	obs_property_t *pat = obs_properties_add_list(
		g, bg_key(k, sizeof(k), PRE, "pattern"),
		obs_module_text("MagicPattern"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(pat, obs_module_text("MagicPatHexagram"),
				  MC_HEXAGRAM);
	obs_property_list_add_int(pat, obs_module_text("MagicPatPentagram"),
				  MC_PENTAGRAM);
	obs_property_list_add_int(pat, obs_module_text("MagicPatTriangles"),
				  MC_TRIANGLES);
	obs_property_list_add_int(pat, obs_module_text("MagicPatSolar"), MC_SOLAR);
	obs_property_list_add_int(pat, obs_module_text("MagicPatLunar"), MC_LUNAR);
	obs_property_list_add_int(pat, obs_module_text("MagicPatAstral"),
				  MC_ASTRAL);
	obs_property_list_add_int(pat, obs_module_text("MagicPatRunic"), MC_RUNIC);
	obs_property_list_add_int(pat, obs_module_text("MagicPatZodiac"),
				  MC_ZODIAC);
	obs_property_list_add_int(pat, obs_module_text("MagicPatSigil"), MC_SIGIL);
	obs_property_list_add_int(pat, obs_module_text("MagicPatRuneCrown"),
				  MC_RUNECROWN);
	obs_property_list_add_int(pat, obs_module_text("MagicPatScallop"),
				  MC_SCALLOP);
	obs_property_list_add_int(pat, obs_module_text("MagicPatGeared"),
				  MC_GEARED);
	obs_property_list_add_int(pat, obs_module_text("MagicPatTrinity"),
				  MC_TRINITY);
	obs_property_list_add_int(pat, obs_module_text("MagicPatConstellation"),
				  MC_CONSTELLATION);
	obs_property_list_add_int(pat, obs_module_text("MagicPatVesica"),
				  MC_VESICA);

	obs_properties_add_color(g, bg_key(k, sizeof(k), PRE, "color"),
				 obs_module_text("MagicColor"));
	obs_properties_add_color(g, bg_key(k, sizeof(k), PRE, "accent"),
				 obs_module_text("MagicAccent"));
	obs_properties_add_float_slider(g,
					bg_key(k, sizeof(k), PRE, "thickness"),
					obs_module_text("MagicThickness"), 1.0,
					20.0, 0.5);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "size"),
					obs_module_text("MagicSize"), 0.1, 1.0,
					0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "speed"),
					obs_module_text("MagicSpeed"), -180.0,
					180.0, 1.0);
	obs_properties_add_float_slider(g,
					bg_key(k, sizeof(k), PRE, "draw_time"),
					obs_module_text("MagicDrawTime"), 0.1,
					15.0, 0.1);
	obs_properties_add_float_slider(g,
					bg_key(k, sizeof(k), PRE, "hold_time"),
					obs_module_text("MagicHoldTime"), 0.0,
					30.0, 0.1);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "fade_in"),
					obs_module_text("MagicFadeIn"), 0.0, 10.0,
					0.1);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "fade_out"),
					obs_module_text("MagicFadeOut"), 0.0,
					10.0, 0.1);
	obs_properties_add_bool(g, bg_key(k, sizeof(k), PRE, "loop"),
				obs_module_text("MagicLoop"));
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "flicker"),
					obs_module_text("MagicFlicker"), 0.0,
					1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "glow"),
					obs_module_text("Glow"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "bloom"),
					obs_module_text("Bloom"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "emissive"),
					obs_module_text("Emissive"), 0.0, 1.0,
					0.01);
	obs_properties_add_float_slider(g, bg_key(k, sizeof(k), PRE, "audio"),
					obs_module_text("MagicAudio"), 0.0, 2.0,
					0.05);
}

static void mc_defaults(obs_data_t *s)
{
	char k[96];
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "pattern"),
				 MC_HEXAGRAM);
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "color"),
				 (long long)0xFFFFDC78); /* cyan */
	obs_data_set_default_int(s, bg_key(k, sizeof(k), PRE, "accent"),
				 (long long)0xFF78D2FF); /* gold */
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "thickness"),
				    3.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "size"), 0.7);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "speed"), 20.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "draw_time"),
				    3.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "hold_time"),
				    2.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "fade_in"),
				    1.0);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "fade_out"),
				    1.5);
	obs_data_set_default_bool(s, bg_key(k, sizeof(k), PRE, "loop"), true);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "flicker"),
				    0.15);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "glow"), 0.6);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "bloom"), 0.4);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "emissive"),
				    0.4);
	obs_data_set_default_double(s, bg_key(k, sizeof(k), PRE, "audio"), 0.5);
}

const struct bg_effect bgfx_magiccircle = {
	.id             = "magiccircle",
	.name_key       = "EffectMagicCircle",
	.create         = mc_create,
	.destroy        = mc_destroy,
	.load_graphics  = mc_load_graphics,
	.update         = mc_update,
	.tick           = mc_tick,
	.render         = mc_render,
	.reset          = mc_reset,
	.get_properties = mc_properties,
	.get_defaults   = mc_defaults,
};
