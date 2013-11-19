/* GStreamer
 * Copyright (C)  2007 Sebastien Moutte <sebastien@moutte.net>
 *
 * gstdshowsrcwrapper.c: 
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstdshowaudiosrc.h"
#include "gstdshowvideosrc.h"

static gboolean
plugin_init (GstPlugin * plugin)
{
  if (!gst_element_register (plugin, "dshowaudiosrc",
          GST_RANK_NONE, GST_TYPE_DSHOWAUDIOSRC) ||
      !gst_element_register (plugin, "dshowvideosrc",
          GST_RANK_NONE, GST_TYPE_DSHOWVIDEOSRC))
    return FALSE;

  return TRUE;
}

/* GStreamer API Version */
#define GST_API_VERSION "1.0"

/* Name of package */
#define PACKAGE "gst-plugins-dshow"

/* package name in plugins */
#define GST_PACKAGE_NAME "GStreamer DirectShow sources"

/* package origin */
#define GST_PACKAGE_ORIGIN "Unknown package origin"

/* Version number of package */
#define VERSION "1.3.0.1"

extern "C"
{
  GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
      GST_VERSION_MINOR,
      dshowsrcwrapper,
      "DirectShow sources wrapper plugin",
      plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)

}
