/*
 * Copyright © 2009  Red Hat, Inc.
 * Copyright © 2009  Keith Stribley
 *
 *  This is part of HarfBuzz, a text shaping library.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and its documentation for any purpose, provided that the
 * above copyright notice and the following two paragraphs appear in
 * all copies of this software.
 *
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
 * IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 * Red Hat Author(s): Behdad Esfahbod
 */

#include "hb-private.hh"

#include "hb-ft.h"

#include "hb-font-private.hh"

#include FT_ADVANCES_H
#include FT_TRUETYPE_TABLES_H



#ifndef HB_DEBUG_FT
#define HB_DEBUG_FT (HB_DEBUG+0)
#endif


/* TODO:
 *
 * In general, this file does a fine job of what it's supposed to do.
 * There are, however, things that need more work:
 *
 *   - We don't handle any load_flags.  That definitely has API implications. :(
 *     I believe hb_ft_font_create() should take load_flags input.
 *     In particular, FT_Get_Advance() without the NO_HINTING flag seems to be
 *     buggy.
 *
 *   - We don't handle / allow for emboldening / obliqueing.
 *
 *   - Rounding, etc?
 *
 *   - In the future, we should add constructors to create fonts in font space.
 *
 *   - I believe transforms are not correctly implemented.  FreeType does not
 *     provide any API to get to the transform/delta set on the face. :(
 *
 *   - Always use FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH?
 *
 *   - FT_Load_Glyph() is exteremely costly.  Do something about it?
 */


static hb_bool_t
hb_ft_get_glyph (hb_font_t *font HB_UNUSED,
		 void *font_data,
		 hb_codepoint_t unicode,
		 hb_codepoint_t variation_selector,
		 hb_codepoint_t *glyph,
		 void *user_data HB_UNUSED)

{
  FT_Face ft_face = (FT_Face) font_data;

#ifdef HAVE_FT_FACE_GETCHARVARIANTINDEX
  if (unlikely (variation_selector)) {
    *glyph = FT_Face_GetCharVariantIndex (ft_face, unicode, variation_selector);
    if (*glyph)
      return TRUE;
  }
#endif

  *glyph = FT_Get_Char_Index (ft_face, unicode);
  return *glyph != 0;
}

static hb_position_t
hb_ft_get_glyph_h_advance (hb_font_t *font HB_UNUSED,
			   void *font_data,
			   hb_codepoint_t glyph,
			   void *user_data HB_UNUSED)
{
  FT_Face ft_face = (FT_Face) font_data;
  int load_flags = FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING;
  FT_Fixed v;

  if (unlikely (FT_Get_Advance (ft_face, glyph, load_flags, &v)))
    return 0;

  return v >> 10;
}

static hb_position_t
hb_ft_get_glyph_v_advance (hb_font_t *font HB_UNUSED,
			   void *font_data,
			   hb_codepoint_t glyph,
			   void *user_data HB_UNUSED)
{
  FT_Face ft_face = (FT_Face) font_data;
  int load_flags = FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING | FT_LOAD_VERTICAL_LAYOUT;
  FT_Fixed v;

  if (unlikely (FT_Get_Advance (ft_face, glyph, load_flags, &v)))
    return 0;

  /* Note: FreeType's vertical metrics grows downward while other FreeType coordinates
   * have a Y growing upward.  Hence the extra negation. */
  return -v >> 10;
}

static hb_bool_t
hb_ft_get_glyph_h_origin (hb_font_t *font HB_UNUSED,
			  void *font_data HB_UNUSED,
			  hb_codepoint_t glyph HB_UNUSED,
			  hb_position_t *x HB_UNUSED,
			  hb_position_t *y HB_UNUSED,
			  void *user_data HB_UNUSED)
{
  /* We always work in the horizontal coordinates. */
  return TRUE;
}

static hb_bool_t
hb_ft_get_glyph_v_origin (hb_font_t *font HB_UNUSED,
			  void *font_data,
			  hb_codepoint_t glyph,
			  hb_position_t *x,
			  hb_position_t *y,
			  void *user_data HB_UNUSED)
{
  FT_Face ft_face = (FT_Face) font_data;
  int load_flags = FT_LOAD_DEFAULT;

  if (unlikely (FT_Load_Glyph (ft_face, glyph, load_flags)))
    return FALSE;

  /* Note: FreeType's vertical metrics grows downward while other FreeType coordinates
   * have a Y growing upward.  Hence the extra negation. */
  *x = ft_face->glyph->metrics.horiBearingX -   ft_face->glyph->metrics.vertBearingX;
  *y = ft_face->glyph->metrics.horiBearingY - (-ft_face->glyph->metrics.vertBearingY);

  return TRUE;
}

static hb_position_t
hb_ft_get_glyph_h_kerning (hb_font_t *font HB_UNUSED,
			   void *font_data,
			   hb_codepoint_t left_glyph,
			   hb_codepoint_t right_glyph,
			   void *user_data HB_UNUSED)
{
  FT_Face ft_face = (FT_Face) font_data;
  FT_Vector kerningv;

  if (FT_Get_Kerning (ft_face, left_glyph, right_glyph, FT_KERNING_DEFAULT, &kerningv))
    return 0;

  return kerningv.x;
}

static hb_position_t
hb_ft_get_glyph_v_kerning (hb_font_t *font HB_UNUSED,
			   void *font_data HB_UNUSED,
			   hb_codepoint_t top_glyph HB_UNUSED,
			   hb_codepoint_t bottom_glyph HB_UNUSED,
			   void *user_data HB_UNUSED)
{
  /* FreeType API doesn't support vertical kerning */
  return 0;
}

static hb_bool_t
hb_ft_get_glyph_extents (hb_font_t *font HB_UNUSED,
			 void *font_data,
			 hb_codepoint_t glyph,
			 hb_glyph_extents_t *extents,
			 void *user_data HB_UNUSED)
{
  FT_Face ft_face = (FT_Face) font_data;
  int load_flags = FT_LOAD_DEFAULT;

  if (unlikely (FT_Load_Glyph (ft_face, glyph, load_flags)))
    return FALSE;

  extents->x_bearing = ft_face->glyph->metrics.horiBearingX;
  extents->y_bearing = ft_face->glyph->metrics.horiBearingY;
  extents->width = ft_face->glyph->metrics.width;
  extents->height = ft_face->glyph->metrics.height;
  return TRUE;
}

static hb_bool_t
hb_ft_get_glyph_contour_point (hb_font_t *font HB_UNUSED,
			       void *font_data,
			       hb_codepoint_t glyph,
			       unsigned int point_index,
			       hb_position_t *x,
			       hb_position_t *y,
			       void *user_data HB_UNUSED)
{
  FT_Face ft_face = (FT_Face) font_data;
  int load_flags = FT_LOAD_DEFAULT;

  if (unlikely (FT_Load_Glyph (ft_face, glyph, load_flags)))
      return FALSE;

  if (unlikely (ft_face->glyph->format != FT_GLYPH_FORMAT_OUTLINE))
      return FALSE;

  if (unlikely (point_index >= (unsigned int) ft_face->glyph->outline.n_points))
      return FALSE;

  *x = ft_face->glyph->outline.points[point_index].x;
  *y = ft_face->glyph->outline.points[point_index].y;

  return TRUE;
}

static hb_font_funcs_t ft_ffuncs = {
  HB_OBJECT_HEADER_STATIC,

  TRUE, /* immutable */

  {
    hb_ft_get_glyph,
    hb_ft_get_glyph_h_advance,
    hb_ft_get_glyph_v_advance,
    hb_ft_get_glyph_h_origin,
    hb_ft_get_glyph_v_origin,
    hb_ft_get_glyph_h_kerning,
    hb_ft_get_glyph_v_kerning,
    hb_ft_get_glyph_extents,
    hb_ft_get_glyph_contour_point,
  }
};

static hb_font_funcs_t *
_hb_ft_get_font_funcs (void)
{
  return &ft_ffuncs;
}


static hb_blob_t *
reference_table  (hb_face_t *face HB_UNUSED, hb_tag_t tag, void *user_data)
{
  FT_Face ft_face = (FT_Face) user_data;
  FT_Byte *buffer;
  FT_ULong  length = 0;
  FT_Error error;

  /* Note: FreeType like HarfBuzz uses the NONE tag for fetching the entire blob */

  error = FT_Load_Sfnt_Table (ft_face, tag, 0, NULL, &length);
  if (error)
    return NULL;

  buffer = (FT_Byte *) malloc (length);
  if (buffer == NULL)
    return NULL;

  error = FT_Load_Sfnt_Table (ft_face, tag, 0, buffer, &length);
  if (error)
    return NULL;

  return hb_blob_create ((const char *) buffer, length,
			 HB_MEMORY_MODE_WRITABLE,
			 buffer, free);
}


hb_face_t *
hb_ft_face_create (FT_Face           ft_face,
		   hb_destroy_func_t destroy)
{
  hb_face_t *face;

  if (ft_face->stream->read == NULL) {
    hb_blob_t *blob;

    blob = hb_blob_create ((const char *) ft_face->stream->base,
			   (unsigned int) ft_face->stream->size,
			   /* TODO: We assume that it's mmap()'ed, but FreeType code
			    * suggests that there are cases we reach here but font is
			    * not mmapped.  For example, when mmap() fails.  No idea
			    * how to deal with it better here. */
			   HB_MEMORY_MODE_READONLY_MAY_MAKE_WRITABLE,
			   ft_face, destroy);
    face = hb_face_create (blob, ft_face->face_index);
    hb_blob_destroy (blob);
  } else {
    face = hb_face_create_for_tables (reference_table, ft_face, destroy);
  }

  hb_face_set_index (face, ft_face->face_index);
  hb_face_set_upem (face, ft_face->units_per_EM);

  return face;
}

static void
hb_ft_face_finalize (FT_Face ft_face)
{
  hb_face_destroy ((hb_face_t *) ft_face->generic.data);
}

hb_face_t *
hb_ft_face_create_cached (FT_Face ft_face)
{
  if (unlikely (!ft_face->generic.data || ft_face->generic.finalizer != (FT_Generic_Finalizer) hb_ft_face_finalize))
  {
    if (ft_face->generic.finalizer)
      ft_face->generic.finalizer (ft_face);

    ft_face->generic.data = hb_ft_face_create (ft_face, NULL);
    ft_face->generic.finalizer = (FT_Generic_Finalizer) hb_ft_face_finalize;
  }

  return hb_face_reference ((hb_face_t *) ft_face->generic.data);
}

static void
_do_nothing (void)
{
}


hb_font_t *
hb_ft_font_create (FT_Face           ft_face,
		   hb_destroy_func_t destroy)
{
  hb_font_t *font;
  hb_face_t *face;

  face = hb_ft_face_create (ft_face, destroy);
  font = hb_font_create (face);
  hb_face_destroy (face);
  hb_font_set_funcs (font,
		     _hb_ft_get_font_funcs (),
		     ft_face, (hb_destroy_func_t) _do_nothing);
  hb_font_set_scale (font,
		     ((uint64_t) ft_face->size->metrics.x_scale * (uint64_t) ft_face->units_per_EM) >> 16,
		     ((uint64_t) ft_face->size->metrics.y_scale * (uint64_t) ft_face->units_per_EM) >> 16);
  hb_font_set_ppem (font,
		    ft_face->size->metrics.x_ppem,
		    ft_face->size->metrics.y_ppem);

  return font;
}




static FT_Library ft_library;
static hb_bool_t ft_library_initialized;
static struct ft_library_destructor {
  ~ft_library_destructor (void) {
    if (ft_library)
      FT_Done_FreeType (ft_library);
  }
} static_ft_library_destructor;

static FT_Library
_get_ft_library (void)
{
  if (unlikely (!ft_library_initialized)) {
    FT_Init_FreeType (&ft_library);
    ft_library_initialized = TRUE;
  }

  return ft_library;
}

static void
_release_blob (FT_Face ft_face)
{
  hb_blob_destroy ((hb_blob_t *) ft_face->generic.data);
}

void
hb_ft_font_set_funcs (hb_font_t *font)
{
  hb_blob_t *blob = hb_face_reference_blob (font->face);
  unsigned int blob_length;
  const char *blob_data = hb_blob_get_data (blob, &blob_length);
  if (unlikely (!blob_length))
    DEBUG_MSG (FT, font, "Font face has empty blob");

  FT_Face ft_face = NULL;
  FT_Error err = FT_New_Memory_Face (_get_ft_library (),
				     (const FT_Byte *) blob_data,
				     blob_length,
				     hb_face_get_index (font->face),
				     &ft_face);

  if (unlikely (err)) {
    hb_blob_destroy (blob);
    DEBUG_MSG (FT, font, "Font face FT_New_Memory_Face() failed");
    return;
  }

  FT_Select_Charmap (ft_face, FT_ENCODING_UNICODE);

  FT_Set_Char_Size (ft_face,
		    font->x_scale, font->y_scale,
		    font->x_ppem * 72 * 64 / font->x_scale,
		    font->y_ppem * 72 * 64 / font->y_scale);

  ft_face->generic.data = blob;
  ft_face->generic.finalizer = (FT_Generic_Finalizer) _release_blob;

  hb_font_set_funcs (font,
		     _hb_ft_get_font_funcs (),
		     ft_face,
		     (hb_destroy_func_t) FT_Done_Face);
}

FT_Face
hb_ft_font_get_face (hb_font_t *font)
{
  if (font->destroy == (hb_destroy_func_t) FT_Done_Face ||
      font->destroy == (hb_destroy_func_t) _do_nothing)
    return (FT_Face) font->user_data;

  return NULL;
}
