/*
 * glyphs.cpp - the actual 8x8 bitmaps, plus the two generated panel views.
 *
 * Column bit values, for editing by hand:
 *   X0=0x80  X1=0x40  X2=0x20  X3=0x10  X4=0x08  X5=0x04  X6=0x02  X7=0x01
 */

#include "glyphs.h"

/* ........ */
static const uint8_t GLYPH_BLANK[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

/* Microphone - shown while the raw-dictation hold (F8) is active.
 *   ..####..
 *   ..#..#..
 *   ..#..#..
 *   ..#..#..
 *   ..####..
 *   .#.##.#.
 *   ...##...
 *   .######.
 */
static const uint8_t GLYPH_MIC[8] = {0x3C, 0x24, 0x24, 0x24, 0x3C, 0x5A, 0x18, 0x7E};

/* Microphone + sparkle - shown while the cleaned-dictation hold (F9) is active.
 *   .#####..
 *   #.#..#..
 *   .##..#..
 *   ..#..#..
 *   ..####..
 *   .#.##.#.
 *   ...##...
 *   .######.
 */
static const uint8_t GLYPH_MIC_CLEAN[8] = {0x7C, 0xA4, 0x64, 0x24, 0x3C, 0x5A, 0x18, 0x7E};

/* Checkmark - flashed for a moment when a hold is released.
 *   ........
 *   .......#
 *   ......#.
 *   .#...#..
 *   ..#.#...
 *   ...#....
 *   ........
 *   ........
 */
static const uint8_t GLYPH_CHECK[8] = {0x00, 0x01, 0x02, 0x44, 0x28, 0x10, 0x00, 0x00};

/* Cross - shown when the host reports that a dictation failed.
 *   ........
 *   .#....#.
 *   ..#..#..
 *   ...##...
 *   ...##...
 *   ..#..#..
 *   .#....#.
 *   ........
 */
static const uint8_t GLYPH_CROSS[8] = {0x00, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x00};

/* Generic "a hold is active" dot, for hold actions with no dedicated glyph.
 *   ........
 *   ........
 *   ...##...
 *   ..####..
 *   ..####..
 *   ...##...
 *   ........
 *   ........
 */
static const uint8_t GLYPH_HOLD[8] = {0x00, 0x00, 0x18, 0x3C, 0x3C, 0x18, 0x00, 0x00};

/* Mouse pointer - shown while a mouse-button hold is active.
 *   #.......
 *   ##......
 *   ###.....
 *   ####....
 *   #####...
 *   ###.....
 *   #.##....
 *   ...##...
 */
static const uint8_t GLYPH_POINTER[8] = {0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xE0, 0xB0, 0x18};

/* Layer icons (the enum ids kept their letter names; the bitmaps are icons
 * now - the layer NAME scrolls first, then the icon stays as the idle view). */

/* Feather - layer FETHR (same silhouette as the app's tray mark)
 *   .....##.
 *   ....###.
 *   ...####.
 *   ...####.
 *   ..####..
 *   ..###...
 *   .##.....
 *   #.......
 */
static const uint8_t GLYPH_F[8] = {0x06, 0x0E, 0x1E, 0x1E, 0x3C, 0x38, 0x60, 0x80};

/* Eighth note - layer MEDIA
 *   ...####.
 *   ...#..#.
 *   ...#....
 *   ...#....
 *   ...#....
 *   .###....
 *   ####....
 *   .##.....
 */
static const uint8_t GLYPH_M[8] = {0x1E, 0x12, 0x10, 0x10, 0x10, 0x70, 0xF0, 0x60};

/* Pencil - layer EDIT
 *   ......##
 *   .....#.#
 *   ....#.#.
 *   ...#.#..
 *   ..#.#...
 *   .#.#....
 *   ##......
 *   #.......
 */
static const uint8_t GLYPH_E[8] = {0x03, 0x05, 0x0A, 0x14, 0x28, 0x50, 0xC0, 0x80};

/* Cursor arrow - layer MOUSE (same bitmap as the mouse-hold pointer) */
static const uint8_t GLYPH_P[8] = {0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xE0, 0xB0, 0x18};

/* Direction arrows - shown only while the nav stick is actually deflected. */
static const uint8_t GLYPH_ARROW_UP[8]    = {0x18, 0x3C, 0x7E, 0xFF, 0x18, 0x18, 0x18, 0x18};
static const uint8_t GLYPH_ARROW_DOWN[8]  = {0x18, 0x18, 0x18, 0x18, 0xFF, 0x7E, 0x3C, 0x18};
static const uint8_t GLYPH_ARROW_LEFT[8]  = {0x00, 0x20, 0x60, 0xFF, 0xFF, 0x60, 0x20, 0x00};
static const uint8_t GLYPH_ARROW_RIGHT[8] = {0x00, 0x04, 0x06, 0xFF, 0xFF, 0x06, 0x04, 0x00};

/* Index order MUST match enum GlyphId. */
static const uint8_t *const GLYPH_TABLE[G_COUNT] = {
    GLYPH_BLANK,     /* G_BLANK       */
    GLYPH_MIC,       /* G_MIC         */
    GLYPH_MIC_CLEAN, /* G_MIC_CLEAN   */
    GLYPH_CHECK,     /* G_CHECK       */
    GLYPH_CROSS,     /* G_CROSS       */
    GLYPH_HOLD,      /* G_HOLD        */
    GLYPH_POINTER,   /* G_POINTER     */
    GLYPH_F,         /* G_F           */
    GLYPH_M,         /* G_M           */
    GLYPH_E,         /* G_E           */
    GLYPH_P,         /* G_P           */
    GLYPH_ARROW_UP,  /* G_ARROW_UP    */
    GLYPH_ARROW_DOWN,
    GLYPH_ARROW_LEFT,
    GLYPH_ARROW_RIGHT,
};

const uint8_t *glyphById(uint8_t id)
{
  if (id >= (uint8_t)G_COUNT) return GLYPH_BLANK;
  return GLYPH_TABLE[id];
}

void glyphVolumeBar(uint8_t level, uint8_t out[8])
{
  if (level > 8) level = 8;

  uint8_t mask = 0;
  for (uint8_t c = 0; c < level; c++) mask |= (uint8_t)(0x80 >> c);

  out[0] = 0x00;
  out[1] = 0x00;
  out[2] = mask;
  out[3] = mask;
  out[4] = mask;
  out[5] = mask;
  out[6] = 0x00;
  out[7] = 0xFF; /* baseline, so an empty bar is still visibly "the volume bar" */
}

void glyphScrollFrame(uint8_t frame, bool up, uint8_t out[8])
{
  for (uint8_t i = 0; i < 8; i++) out[i] = 0x00;

  uint8_t f  = (uint8_t)(frame & 0x03);
  uint8_t y0 = up ? (uint8_t)((3 - f) * 2) : (uint8_t)(f * 2);

  out[y0]                        = 0x7E;
  out[(uint8_t)((y0 + 4) & 0x07)] = 0x7E;
}

/* Top-left corner of the leading dot for each frame, on a 7x7 field whose
 * centre is (3,3): N, NE, E, SE. The trailing dot is the point reflection
 * (6-x, 6-y), so the pair reads as one object turning rather than two
 * blinking, and four frames make exactly half a turn of the pair - which is a
 * full turn of the figure. */
static const uint8_t SPIN_POS[4][2] = {{3, 0}, {5, 1}, {6, 3}, {5, 5}};

static void spinDot(uint8_t out[8], uint8_t x, uint8_t y)
{
  for (uint8_t dy = 0; dy < 2; dy++) {
    for (uint8_t dx = 0; dx < 2; dx++) {
      out[y + dy] |= (uint8_t)(0x80 >> (x + dx));
    }
  }
}

void glyphSpinnerFrame(uint8_t frame, uint8_t out[8])
{
  for (uint8_t i = 0; i < 8; i++) out[i] = 0x00;

  uint8_t f = (uint8_t)(frame & 0x03);
  uint8_t x = SPIN_POS[f][0];
  uint8_t y = SPIN_POS[f][1];

  spinDot(out, x, y);
  spinDot(out, (uint8_t)(6 - x), (uint8_t)(6 - y));
}
