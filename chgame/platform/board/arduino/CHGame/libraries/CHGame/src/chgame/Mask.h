/// @file Mask.h
/// @brief 1 bpp scratch masks for outlined, shadowed and gradient lettering and logos.
///
/// Outlined, shadowed or gradient text drawn the naive way (the glyphs nine
/// times over, pixel by pixel from flash) costs ~5 ms a line on this chip.
/// Instead: render the shape once into a bit mask, grow it by one pixel with
/// byte-wide ORs for the outline, and paint every row straight into the
/// framebuffer, two pixels a byte. Still 5-10 ms for big lettering: draw it
/// once onto still screens, not every frame.
///
/// CHGfx 1.3's gfx_textFx() works the same way and draws the same pixels with
/// CHGfx_Tiny3x5, but brings CHGfx's whole GFXfont text path: about 2 KB more
/// flash than this and Draw's 3x5 font.
#pragma once
#include <stdint.h>

/// @defgroup chgame_mask Lettering masks
/// @ingroup lib_chgame
/// @brief Outlined, shadowed and gradient lettering, logos and the cover titles.
///
/// The mask lives in CHGfx's chunk buffers (gfx_chunkScratch, 1 KB), which
/// are idle between gfx_wait() and the next flush - i.e. while drawing.
/// Never keep a Mask across frames or use one outside drawing. Masks paint
/// anywhere on screen: they ignore CHGfx's clip rectangle.
///
/// @code
/// Mask m = maskBegin(text35WidthScaled("WIN", 3), 6 * 3);
/// maskText35(m, 0, 0, "WIN", 3);
/// maskDraw(m, 40, 50, GOLD, INK, WINE);       // fill, outline, shadow
/// @endcode
/// @{

/// @brief A 1 bpp shape being built in the chunk scratch. Made by maskBegin().
struct Mask {
    uint8_t *bits;       ///< MSB-first rows, 1 px margin on every side.
    uint8_t stride;      ///< Bytes per row.
    uint8_t w, h;        ///< Usable size (excluding the margin).
};

/// @brief Start an empty mask in the chunk scratch.
/// @param w,h Usable size in pixels; with the 1 px margin, (w + 2) x (h + 2)
///            must fit in 1 KB of bits (about 8000 px).
/// @return The mask, cleared. Valid until the next flush or the next
///         maskBegin() (or anything else that uses gfx_chunkScratch()).
Mask maskBegin(int w, int h);                   // cleared; (w + 2) x (h + 2) <= ~8000 px

/// @brief Add text in the 3x5 font to a mask, at an integer scale.
/// @param m     The mask.
/// @param x,y   Top-left of the text inside the mask.
/// @param s     The text ('~' is a half space).
/// @param scale Size: each font pixel becomes scale x scale (4*scale px
///              advance, 6*scale rows with the descender).
/// @param dy    Optional: a vertical offset for each character (wavy banners).
void maskText35(Mask &m, int x, int y, const char *s, uint8_t scale = 1, const int8_t *dy = nullptr);
/// @brief The width of 3x5 text at a scale, for sizing a mask.
/// @param s     The text.
/// @param scale The scale given to maskText35().
/// @return text35Width(s) * scale.
int  text35WidthScaled(const char *s, uint8_t scale);
/// @brief Add a 1 bpp bitmap (a logo) to a mask.
/// @param m     A mask begun at w*scale x h*scale.
/// @param bits  The bitmap: MSB-first rows of (w + 7) / 8 bytes, zero padding bits.
/// @param w,h   Its size in pixels.
/// @param scale Each bitmap pixel becomes scale x scale.
void maskBlit1(Mask &m, const uint8_t *bits, uint8_t w, uint8_t h, uint8_t scale = 1);

/// @brief Paint a mask, with an optional outline, shadow and gradient.
/// @param m       The mask.
/// @param x,y     Where its top-left (inside the margin) goes on screen.
/// @param fill    Colour of the shape.
/// @param outline Colour of a 1 px ring around it (all 8 directions), or -1 for none.
/// @param shadow  Colour of a shadow, or -1 for none: the outline moved a pixel
///                down and right, or without an outline the shape moved so.
/// @param ramp    Optional: a fill colour for each mask row, replacing fill
///                (gradient lettering).
void maskDraw(const Mask &m, int x, int y, uint8_t fill, int outline = -1, int shadow = -1,
              const uint8_t *ramp = nullptr);
/// @brief Paint just the shape in one colour (no outline, no shadow).
/// @param m   The mask.
/// @param x,y Where its top-left goes on screen.
/// @param c   Colour.
void maskPaint(const Mask &m, int x, int y, uint8_t c);

/// @brief Paint a title as the covers set it, in the house gold.
/// @details (docs/cover-art.md, "the stack".) The shape extruded `depth` px
/// down and right in `side`, an INK outline round letters and extrusion
/// together, thickened a pixel down and right (the drop shadow), and the face
/// a smooth gradient. A one-pixel gap between letters stays INK. About twice
/// a maskDraw(): draw it onto still screens where the game can.
/// @param m     The mask holding the lettering.
/// @param x,y   Where its top-left goes on screen.
/// @param base  The face's base colour.
/// @param ramp  A byte for each mask row: a colour (high nibble) laid over
///              `base` where the row's dither pattern (low nibble, MSB the
///              first of every 4 pixels from the lettering's left; 0 none,
///              15 all) has bits. tools/titleart.py works the patterns out of
///              a 4x4 ordered dither.
/// @param depth The extrusion in pixels, at most 4.
/// @param side  The extrusion's colour.
void maskTitle(const Mask &m, int x, int y, uint8_t base, const uint8_t *ramp, uint8_t depth, uint8_t side);
/// @brief Paint a game's title from the arrays tools/titleart.py writes from its
///        cover recipe: maskBegin(), maskBlit1() and maskTitle() in one call.
/// @param bits  The 1 bpp lettering (as maskBlit1()).
/// @param w,h   Its size in pixels.
/// @param x,y   Where its top-left goes on screen.
/// @param base  The face's base colour.
/// @param ramp  The gradient, a byte a row (as maskTitle()).
/// @param depth The extrusion in pixels, at most 4.
/// @param side  The extrusion's colour.
void titleArt(const uint8_t *bits, uint8_t w, uint8_t h, int x, int y, uint8_t base, const uint8_t *ramp,
              uint8_t depth, uint8_t side);

/// @}
