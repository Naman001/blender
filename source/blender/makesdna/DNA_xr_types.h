/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup DNA
 */

#ifndef __DNA_XR_TYPES_H__
#define __DNA_XR_TYPES_H__

typedef struct bXrSessionSettings {
  /** Shading type (OB_SOLID, ...). */
  char shading_type;
  /** View3D draw flags (V3D_OFSDRAW_NONE, V3D_OFSDRAW_SHOW_ANNOTATION, ...). */
  char draw_flags;
  char _pad[2];

  /** Clipping distance. */
  float clip_start, clip_end;

  char _pad2[4];
} bXrSessionSettings;

#endif /* __DNA_XR_TYPES_H__ */
