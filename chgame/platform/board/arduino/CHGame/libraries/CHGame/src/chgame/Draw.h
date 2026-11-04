/// @file Draw.h
/// @brief The house drawing: rounded rects and panels, span sprites (plain,
///        scaled, flipped, rotated), dithering, an in-place colour remap,
///        convex polygons, column glyphs and PPOT's 3x5 font.
///
/// CHGfx 1.3 has its own versions of several of these (gfx_fillRoundRect,
/// gfx_sprite4, gfx_dither, gfx_textFx with CHGfx_Tiny3x5, the same glyphs),
/// written for generality; these are the casino games' smaller ones, the text
/// about 2 KB smaller in flash. Both work on the same framebuffer: use either.
///
/// Everything here draws inside CHGfx's clip rectangle (gfx_setClip), like
/// CHGfx's own primitives.
///
/// The rule on this chip: code runs from flash with 3 wait states, so a
/// function call per pixel costs ~2-3 us. Everything here is built from
/// gfx_hline spans (word stores, from SRAM) or tight byte and word loops, the
/// hottest from SRAM.
#pragma once
#include <stdint.h>
#include <CHGfx.h>

/// @defgroup chgame_draw Drawing
/// @ingroup lib_chgame
/// @brief Panels, span sprites, dithering, polygons and the 3x5 font, drawn
///        into CHGfx's framebuffer.
///
/// Colours are palette indices (0-15; see @ref chgame_palette). Coordinates
/// are pixels, (0, 0) top left of the 128x128 screen; anything may lie partly
/// or wholly off screen. Draw between gfx_wait() and the next flush.
/// @{

// ---------------------------------------------------------------------------
// Shapes
// ---------------------------------------------------------------------------
/// @brief Fill a rectangle with rounded corners.
/// @param x,y Top-left corner.
/// @param w,h Size in pixels.
/// @param r   Corner radius, 0-4 (larger is 4; never more than half of h):
///            pixel-art corners.
/// @param c   Colour.
void fillRound(int x, int y, int w, int h, uint8_t r, uint8_t c);   // r <= 4: pixel-art corners
/// @brief Outline a rectangle with rounded corners, 1 px wide.
/// @param x,y Top-left corner.
/// @param w,h Size in pixels.
/// @param r   Corner radius, 0-4 as for fillRound(); 0 is a plain rectangle.
/// @param c   Colour.
void roundRect(int x, int y, int w, int h, uint8_t r, uint8_t c);
/// @brief A rounded panel: fillRound() in `fill`, then roundRect() in `edge`.
///        Cards, plates, panels.
/// @param x,y  Top-left corner.
/// @param w,h  Size in pixels.
/// @param r    Corner radius, 0-4.
/// @param fill Inside colour.
/// @param edge Edge colour.
void panel(int x, int y, int w, int h, uint8_t r, uint8_t fill, uint8_t edge);
/// @brief A panel lit from above.
/// @details A 1 px INK drop shadow outside (down and right), the edge, then
/// inside a lighter top line and a darker bottom one (from LIGHTER and
/// DARKER). The shadow makes it one pixel wider and taller than w x h.
/// @param x,y  Top-left corner.
/// @param w,h  Size in pixels, not counting the shadow.
/// @param r    Corner radius, 0-4.
/// @param fill Inside colour (a house colour, so LIGHTER/DARKER know its shades).
/// @param edge Edge colour.
void panelLit(int x, int y, int w, int h, uint8_t r, uint8_t fill, uint8_t edge);
/// @brief A raised rectangle's rim: light along the top and left, dark along the
///        bottom and right.
/// @param x,y Top-left corner.
/// @param w,h Size in pixels.
/// @param c   The surface colour: the rim is LIGHTER[c] and DARKER[c].
void bevel(int x, int y, int w, int h, uint8_t c);
/// @brief A raised rectangle's rim in two given colours.
/// @param x,y   Top-left corner.
/// @param w,h   Size in pixels.
/// @param light Colour of the top and left edges.
/// @param dark  Colour of the bottom and right edges.
void bevel(int x, int y, int w, int h, uint8_t light, uint8_t dark);
/** @var DARKER
 *  @brief One step darker for each house colour (@ref chgame_palette): the
 *         shading that gives panels, cards and chips their depth.
 *  @details `DARKER[FELT]` is FELT_DK, for instance. */
/** @var LIGHTER
 *  @brief One step lighter for each house colour (@ref chgame_palette).
 *  @see DARKER */
extern const uint8_t DARKER[16], LIGHTER[16];

/// @brief A 50% checkerboard of one colour over a rectangle; the pixels in
///        between are left alone (darkened backdrops, shadows).
/// @param x,y   Top-left corner.
/// @param w,h   Size in pixels.
/// @param c     Colour.
/// @param phase 0 paints the pixels where x + y is even, 1 where it is odd.
void dither(int x, int y, int w, int h, uint8_t c, uint8_t phase);      // 50% checker
/// @brief A round spotlight: a disc of dots, on 25% of its pixels (every other
///        pixel of every other row).
/// @param cx,cy Centre.
/// @param r     Radius in pixels.
/// @param c     Colour of the dots (WHITE behind the dealer, for instance).
void spotlight(int cx, int cy, int r, uint8_t c);
/// @brief A soft shadow (a 50% INK checker) 2 px below and right of a box.
/// @param x,y Top-left corner of the box casting it.
/// @param w,h Size of the box.
void dropShadow(int x, int y, int w, int h);
/// @brief Recolour a rectangle in place: pixel = remap[pixel] (dimming, highlighting).
/// @param x,y   Top-left corner.
/// @param w,h   Size in pixels.
/// @param remap Sixteen colours, the new colour for each old one.
void remapRect(int x, int y, int w, int h, const uint8_t *remap);
/// @brief Fill a convex polygon.
/// @details Filled by pixel centres, so polygons sharing an edge neither
/// overlap nor leave a gap. The corners may go in either winding order.
/// @param xy     n corners as x, y pairs, in 1/16 px.
/// @param n      Number of corners, at most 8.
/// @param c      Colour.
/// @param dither -1 (default): solid. 0 or 1: a 50% checker of c instead,
///               with that phase as dither()'s.
void fillConvex(const int16_t *xy, uint8_t n, uint8_t c, int dither = -1);

// ---------------------------------------------------------------------------
// Span sprites
// ---------------------------------------------------------------------------
/// @brief sprite4() flags.
enum : uint8_t {
    SPR_FLIP_V = 1,     ///< Upside down.
    SPR_FLIP_H = 2      ///< Mirrored left to right (1:1 scale only).
};
/// @brief Draw span4 art, through a remap and at a scale.
/// @details span4 art (tools/assets.py pack_span4): w, h, then per row a count
/// and (len-1)<<4|colour bytes, colour 15 = skip. Every pixel goes through
/// the remap, so one image serves many looks (a team colour, a hit flash).
/// @param data  The art.
/// @param x,y   Where its top-left corner goes.
/// @param remap A colour for each of 0..14; nullptr or RM_ID: as drawn.
/// @param scale Q8: 256 = 1:1 (the fast path), 512 = double, 128 = half.
/// @param flip  0, or SPR_FLIP_V and/or SPR_FLIP_H.
void sprite4(const uint8_t *data, int x, int y, const uint8_t *remap = nullptr, int scale = 256,
             uint8_t flip = 0);
/// @brief The identity remap (every colour to itself).
extern const uint8_t RM_ID[16];                     // the identity remap
/// @brief Draw span4 art turned and scaled about a pivot.
/// @details Decodes into the CHGfx chunk scratch, so it is for drawing time
/// only (between gfx_wait() and the flush), and the art must unpack to 1 KB
/// at most (e.g. 32x60). Larger art draws nothing.
/// @param data   The art (as sprite4()).
/// @param ax,ay  The pivot, a pixel of the art.
/// @param px,py  Where the pivot lands on screen.
/// @param angle  The turn, 256 = one full turn, clockwise on screen.
/// @param scale  Q8, 256 = 1:1.
/// @param remap  As sprite4() (nullptr: as drawn).
void spriteRot(const uint8_t *data, int ax, int ay, int px, int py, uint8_t angle, int scale,
               const uint8_t *remap);
/// @brief spriteRot() for raw 4 bpp art the caller has put in the chunk scratch.
/// @param w,h    Size of the art in gfx_chunkScratch(): rows of (w + 1) / 2
///               bytes, low nibble first, 15 = clear.
/// @param ax,ay  The pivot, a pixel of the art.
/// @param px,py  Where the pivot lands on screen.
/// @param angle  The turn, 256 = one full turn.
/// @param scale  Q8, 256 = 1:1.
/// @param remap  As sprite4() (nullptr: as drawn).
void rotRaw(int w, int h, int ax, int ay, int px, int py, uint8_t angle, int scale,
            const uint8_t *remap);

// ---------------------------------------------------------------------------
// Glyphs and the 3x5 font
// ---------------------------------------------------------------------------
/// @brief Draw a column-major 1 bpp glyph (bit 0 = top row, up to 8 rows).
/// @param x,y   Top-left corner.
/// @param cols  One byte per column.
/// @param ncols Number of columns.
/// @param c     Colour of the set bits; clear bits are left alone.
void glyph(int x, int y, const uint8_t *cols, uint8_t ncols, uint8_t c);
/// @brief Draw a row-major 1 bpp glyph up to 16 wide (the leftmost pixel in bit 15).
/// @param x,y   Top-left corner.
/// @param rows  One uint16_t per row.
/// @param nrows Number of rows.
/// @param c     Colour of the set bits; clear bits are left alone.
void glyph16(int x, int y, const uint16_t *rows, uint8_t nrows, uint8_t c);

/// @brief Draw text in PPOT's 3x5 font.
/// @details Capitals, lower case (with descenders), figures and punctuation.
/// 4 px advance, '~' = a 2 px space, '\\n' = 7 px down (back to x).
/// Characters the font lacks are skipped (a 4 px gap).
/// @param x,y Top-left corner of the first character.
/// @param str The text.
/// @param c   Colour.
/// @return The advance of the last line in pixels (4 a character, 2 a '~').
int  text35(int x, int y, const char *str, uint8_t c);
/// @brief The width of text in the 3x5 font, without the trailing gap: for centring.
/// @param str The text; with '\\n', the widest line.
/// @return Its width in pixels (0 for "").
int  text35Width(const char *str);
/// @brief text35() over its own shade a pixel down and right (embossed).
/// @param x,y   Top-left corner.
/// @param str   The text.
/// @param c     Colour of the letters.
/// @param shade Colour of the shade (default INK).
/// @return As text35().
int  text35s(int x, int y, const char *str, uint8_t c, uint8_t shade = 0);
/// @brief The 3x5 font doubled (8 px advance, 12 rows with the descender): menus.
/// @details Each font pixel is a 2x2 block; '\\n' goes 14 px down. Drawn from
/// flash: for menus and titles, not for text redrawn every frame.
/// @param x,y Top-left corner.
/// @param str The text.
/// @param c   Colour.
void text35x2(int x, int y, const char *str, uint8_t c);
/// @brief text35x2() over its own shade a pixel down and right.
/// @param x,y   Top-left corner.
/// @param str   The text.
/// @param c     Colour of the letters.
/// @param shade Colour of the shade (default INK).
void text35x2s(int x, int y, const char *str, uint8_t c, uint8_t shade = 0);
/// @brief The width of text drawn with text35x2().
/// @param str The text.
/// @return Its width in pixels: twice text35Width().
inline int text35x2Width(const char *str) { return text35Width(str) * 2; }
/// @brief A character of the 3x5 font, for drawing it yourself with glyph().
/// @param ch The character.
/// @return Its three column bytes, or nullptr if the font has no such character.
const uint8_t *glyph35(char ch);                     // its three column bytes, nullptr = none

/// @}
