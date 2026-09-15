/*
 * glyphs.h - 8x8 bitmaps for the Chain Mono indicator (declarations).
 *
 * Row/bit layout matches the Chain Mono "full-screen buffer refresh" command
 * (0x31) exactly, so a glyph can be handed straight to setMonoBufferRefresh():
 *
 *   buffer[N] = row N (top row first)
 *   bit7 = X 0 (leftmost)  ...  bit0 = X 7 (rightmost)
 *   1 = LED on, 0 = LED off
 *
 * Source: protocol/Chain_Mono_Protocol.md - "Display_buffer[N] = row N's 8 bits
 * (bit7 -> bit0 = X coord 0 -> 7), 0=off/1=on".
 *
 * v2 change: glyphs are addressed by a small integer id rather than by pointer.
 * The Mono panel state machine compares a (kind, arg) pair to decide whether a
 * repaint is needed, and several panel views are generated at runtime (volume
 * bar, scroll animation) where no static pointer exists. Ids keep the compare
 * uniform, and they also let the layer table stay plain data.
 */

#ifndef FLOW_GLYPHS_H
#define FLOW_GLYPHS_H

#include <Arduino.h>

enum GlyphId : uint8_t {
  G_BLANK = 0,
  G_MIC,        /* raw dictation hold                */
  G_MIC_CLEAN,  /* cleaned dictation hold            */
  G_CHECK,      /* hold released, or host said `pasted` */
  G_CROSS,      /* host said `error`                 */
  G_HOLD,       /* generic "something is held" dot   */
  G_POINTER,    /* mouse pointer                     */
  G_F,          /* layer FLOW                        */
  G_M,          /* layer MEDIA                       */
  G_E,          /* layer EDIT                        */
  G_P,          /* layer MOUSE (P for pointer)       */
  G_ARROW_UP,
  G_ARROW_DOWN,
  G_ARROW_LEFT,
  G_ARROW_RIGHT,
  G_COUNT
};

/* Never returns NULL: an out-of-range id yields GLYPH_BLANK. */
const uint8_t *glyphById(uint8_t id);

/* ---- runtime-generated views ---- */

/* Volume bar graph: `level` (0..8) leftmost columns lit, plus a baseline row. */
void glyphVolumeBar(uint8_t level, uint8_t out[8]);

/* Two horizontal bars marching through the panel; `frame` 0..3, `up` picks the
 * travel direction. Used as the "scroll stick is active" indicator. */
void glyphScrollFrame(uint8_t frame, bool up, uint8_t out[8]);

/* Two opposite 2x2 dots stepped 45 degrees per frame (`frame` 0..3, so one
 * full turn every four frames). Shown while the host reports that it is
 * transcribing or cleaning. */
void glyphSpinnerFrame(uint8_t frame, uint8_t out[8]);

#endif /* FLOW_GLYPHS_H */
