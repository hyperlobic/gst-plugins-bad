/* GStreamer
 * Copyright (C) 1999-2001 Erik Walthinsen <omega@cse.ogi.edu>
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <string.h>
#include <gst/gst.h>
#include <gst/audio/audio.h>

#include "gstplayondemand.h"

/* in these files, a 'tick' is a discrete unit of time, usually around the 1ms
 * range. a tick is not divisible into smaller units of time. 1ms is probably
 * way beyond what a real computer can actually keep track of, but hey ... */

/* some default values */
#define GST_POD_MAX_PLAYS    100   /* maximum simultaneous plays */
#define GST_POD_BUFFER_TIME  5.0   /* buffer length in seconds */
#define GST_POD_TICK_RATE    1e-6  /* ticks per second */

/* buffer pool fallback values ... use if no buffer pool is available */
#define GST_POD_BUFPOOL_SIZE 4096
#define GST_POD_BUFPOOL_NUM  6


/* element factory information */
static GstElementDetails play_on_demand_details = {
  "Play On Demand",
  "Filter/Audio/Effect",
  "LGPL",
  "Plays a stream at specific times, or when it receives a signal",
  VERSION,
  "Leif Morgan Johnson <leif@ambient.2y.net>",
  "(C) 2002",
};


static GstPadTemplate*
play_on_demand_sink_factory (void)
{
  static GstPadTemplate *template = NULL;

  if (!template) {
    template = gst_pad_template_new
      ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
       gst_caps_append(gst_caps_new ("sink_int",  "audio/raw",
                                     GST_AUDIO_INT_PAD_TEMPLATE_PROPS),
                       gst_caps_new ("sink_float", "audio/raw",
                                     GST_AUDIO_FLOAT_MONO_PAD_TEMPLATE_PROPS)),
       NULL);
  }
  return template;
}


static GstPadTemplate*
play_on_demand_src_factory (void)
{
  static GstPadTemplate *template = NULL;

  if (!template)
    template = gst_pad_template_new
      ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
       gst_caps_append (gst_caps_new ("src_float", "audio/raw",
                                      GST_AUDIO_FLOAT_MONO_PAD_TEMPLATE_PROPS),
                        gst_caps_new ("src_int", "audio/raw",
                                      GST_AUDIO_INT_PAD_TEMPLATE_PROPS)),
       NULL);

  return template;
}


/* GObject functionality */
static void play_on_demand_class_init   (GstPlayOnDemandClass *klass);
static void play_on_demand_init         (GstPlayOnDemand *filter);
static void play_on_demand_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void play_on_demand_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static void play_on_demand_dispose      (GObject *object);

/* GStreamer functionality */
static GstBufferPool*   play_on_demand_get_bufferpool (GstPad *pad);
static GstPadLinkReturn play_on_demand_pad_link       (GstPad *pad, GstCaps *caps);
static void             play_on_demand_loop           (GstElement *elem);
static void             play_on_demand_set_clock      (GstElement *elem, GstClock *clock);

/* signal handlers */
static void play_on_demand_play_handler  (GstElement *elem);
static void play_on_demand_clear_handler (GstElement *elem);
static void play_on_demand_reset_handler (GstElement *elem);

/* utility functions */
static void play_on_demand_add_play_pointer (GstPlayOnDemand *filter, guint pos);
static void play_on_demand_resize_buffer    (GstPlayOnDemand *filter);

GType
gst_play_on_demand_get_type (void)
{
  static GType play_on_demand_type = 0;

  if (! play_on_demand_type) {
    static const GTypeInfo play_on_demand_info = {
      sizeof(GstPlayOnDemandClass),
      NULL,
      NULL,
      (GClassInitFunc) play_on_demand_class_init,
      NULL,
      NULL,
      sizeof(GstPlayOnDemand),
      0,
      (GInstanceInitFunc) play_on_demand_init,
    };
    play_on_demand_type = g_type_register_static(GST_TYPE_ELEMENT,
                                                 "GstPlayOnDemand",
                                                 &play_on_demand_info, 0);
  }
  return play_on_demand_type;
}


/* signals and properties */
enum {
  /* add signals here */
  PLAYED_SIGNAL,
  PLAY_SIGNAL,
  CLEAR_SIGNAL,
  RESET_SIGNAL,
  LAST_SIGNAL
};

enum {
  PROP_0,
  PROP_MUTE,
  PROP_BUFFER_TIME,
  PROP_MAX_PLAYS,
  PROP_TICK_RATE,
  PROP_TOTAL_TICKS,
  PROP_TICKS,
};

static guint gst_pod_filter_signals[LAST_SIGNAL] = { 0 };

static GstElementClass *parent_class = NULL;

static void
play_on_demand_class_init (GstPlayOnDemandClass *klass)
{
  GObjectClass    *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class    = (GObjectClass *)    klass;
  gstelement_class = (GstElementClass *) klass;

  gst_pod_filter_signals[PLAYED_SIGNAL] =
    g_signal_new("played", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(GstPlayOnDemandClass, played),
                 NULL, NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  gst_pod_filter_signals[PLAY_SIGNAL] =
    g_signal_new("play", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(GstPlayOnDemandClass, play),
                 NULL, NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  gst_pod_filter_signals[CLEAR_SIGNAL] =
    g_signal_new("clear", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(GstPlayOnDemandClass, clear),
                 NULL, NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  gst_pod_filter_signals[RESET_SIGNAL] =
    g_signal_new("reset", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                 G_STRUCT_OFFSET(GstPlayOnDemandClass, reset),
                 NULL, NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  klass->play  = play_on_demand_play_handler;
  klass->clear = play_on_demand_clear_handler;
  klass->reset = play_on_demand_reset_handler;

  parent_class = g_type_class_ref(GST_TYPE_ELEMENT);

  gobject_class->set_property = play_on_demand_set_property;
  gobject_class->get_property = play_on_demand_get_property;
  gobject_class->dispose      = play_on_demand_dispose;

  gstelement_class->set_clock = play_on_demand_set_clock;

  g_object_class_install_property(G_OBJECT_CLASS(klass), PROP_MUTE,
    g_param_spec_boolean("mute", "Silence output", "Do not output any sound",
                         FALSE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
  g_object_class_install_property(G_OBJECT_CLASS(klass), PROP_BUFFER_TIME,
    g_param_spec_float("buffer-time", "Buffer length in seconds", "Number of seconds of audio the buffer holds",
                       0.0, G_MAXUINT / GST_AUDIO_MAX_RATE - 10, GST_POD_BUFFER_TIME, G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
  g_object_class_install_property(G_OBJECT_CLASS(klass), PROP_MAX_PLAYS,
    g_param_spec_uint("max-plays", "Maximum simultaneous playbacks", "Maximum allowed number of simultaneous plays from the buffer",
                      1, G_MAXUINT, GST_POD_MAX_PLAYS, G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
  g_object_class_install_property(G_OBJECT_CLASS(klass), PROP_TICK_RATE,
    g_param_spec_float("tick-rate", "Tick rate (ticks/second)", "The rate of musical ticks, the smallest time unit in a song",
                       0, G_MAXFLOAT, GST_POD_TICK_RATE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
  g_object_class_install_property(G_OBJECT_CLASS(klass), PROP_TOTAL_TICKS,
    g_param_spec_uint("total-ticks", "Total number of ticks", "Total number of ticks in the tick array",
                      1, G_MAXUINT, 1, G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
  g_object_class_install_property(G_OBJECT_CLASS(klass), PROP_TICKS,
    g_param_spec_pointer("ticks", "Ticks to play sample on", "An array of ticks (musical times) at which to play the sample",
			 G_PARAM_READWRITE));
}

static void
play_on_demand_init (GstPlayOnDemand *filter)
{
  filter->srcpad = gst_pad_new_from_template(play_on_demand_src_factory(), "src");
  filter->sinkpad = gst_pad_new_from_template(play_on_demand_sink_factory(), "sink");

  gst_pad_set_bufferpool_function(filter->sinkpad, play_on_demand_get_bufferpool);
  gst_pad_set_link_function(filter->sinkpad, play_on_demand_pad_link);

  gst_element_add_pad(GST_ELEMENT(filter), filter->sinkpad);
  gst_element_add_pad(GST_ELEMENT(filter), filter->srcpad);

  gst_element_set_loop_function(GST_ELEMENT(filter), play_on_demand_loop);

  filter->clock = NULL;

  filter->rate = 0;

  filter->ticks = g_new(guint32, filter->total_ticks / 32 + 1);
  filter->plays = g_new(guint, filter->max_plays);

  play_on_demand_resize_buffer(filter);
  play_on_demand_clear_handler(GST_ELEMENT(filter));
  play_on_demand_reset_handler(GST_ELEMENT(filter));
}

static void
play_on_demand_set_property (GObject *object, guint prop_id,
                             const GValue *value, GParamSpec *pspec)
{
  GstPlayOnDemand *filter;
  register guint   i;
  guint            new_size, min_size, *new_plays;
  guint           *new_ticks;

  g_return_if_fail(GST_IS_PLAYONDEMAND(object));
  filter = GST_PLAYONDEMAND(object);

  switch (prop_id) {
  case PROP_MUTE:
    filter->mute = g_value_get_boolean(value);
    break;
  case PROP_BUFFER_TIME:
    filter->buffer_time = g_value_get_float(value);
    play_on_demand_resize_buffer(filter);

    /* clear out now-invalid play pointers */
    for (i = 0; i < filter->max_plays; i++)
      filter->plays[i] = G_MAXUINT;

    break;
  case PROP_MAX_PLAYS:
    new_size = g_value_get_uint(value);
    min_size = (new_size < filter->max_plays) ? new_size : filter->max_plays;

    new_plays = g_new(guint, new_size);
    for (i = 0; i < min_size; i++) new_plays[i] = filter->plays[i];
    for (i = min_size; i < new_size; i++) new_plays[i] = G_MAXUINT;

    g_free(filter->plays);
    filter->plays = new_plays;
    filter->max_plays = new_size;

    break;
  case PROP_TICK_RATE:
    filter->tick_rate = g_value_get_float(value);
    break;
  case PROP_TOTAL_TICKS:
    new_size = g_value_get_uint(value);
    min_size = (new_size < filter->total_ticks) ? new_size : filter->total_ticks;

    new_ticks = g_new(guint32, new_size / 32 + 1);
    for (i = 0; i <= min_size / 32; i++) new_ticks[i] = filter->ticks[i];
    for (i = min_size / 32 + 1; i <= new_size / 32; i++) new_ticks[i] = 0;

    g_free(filter->ticks);
    filter->ticks = new_ticks;
    filter->total_ticks = new_size;

    break;
  case PROP_TICKS:
    new_ticks = (guint *) g_value_get_pointer(value);
    if (new_ticks) {
      g_free(filter->ticks);
      filter->ticks = new_ticks;
    }
    break;
  default:
    break;
  }
}

static void
play_on_demand_get_property (GObject *object, guint prop_id,
                             GValue *value, GParamSpec *pspec)
{
  GstPlayOnDemand *filter;

  g_return_if_fail(GST_IS_PLAYONDEMAND(object));
  filter = GST_PLAYONDEMAND(object);

  switch (prop_id) {
  case PROP_MUTE:
    g_value_set_boolean(value, filter->mute);
    break;
  case PROP_BUFFER_TIME:
    g_value_set_float(value, filter->buffer_time);
    break;
  case PROP_MAX_PLAYS:
    g_value_set_uint(value, filter->max_plays);
    break;
  case PROP_TICK_RATE:
    g_value_set_float(value, filter->tick_rate);
    break;
  case PROP_TOTAL_TICKS:
    g_value_set_uint(value, filter->total_ticks);
    break;
  case PROP_TICKS:
    g_value_set_pointer(value, (gpointer) filter->ticks);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static void
play_on_demand_dispose (GObject *object)
{
  GstPlayOnDemand *filter = GST_PLAYONDEMAND (object);

  G_OBJECT_CLASS (parent_class)->dispose (object);

  g_free (filter->ticks);
  g_free (filter->plays);
  g_free (filter->buffer);
}

static GstBufferPool*
play_on_demand_get_bufferpool (GstPad *pad)
{
  GstPlayOnDemand *filter;
  filter = GST_PLAYONDEMAND(gst_pad_get_parent(pad));
  return gst_pad_get_bufferpool(filter->srcpad);
}

static GstPadLinkReturn
play_on_demand_pad_link (GstPad *pad, GstCaps *caps)
{
  const gchar *format;
  GstPlayOnDemand *filter;

  g_return_val_if_fail(caps != NULL, GST_PAD_LINK_DELAYED);
  g_return_val_if_fail(pad  != NULL, GST_PAD_LINK_DELAYED);

  filter = GST_PLAYONDEMAND(GST_PAD_PARENT(pad));

  gst_caps_get_string(caps, "format", &format);
  gst_caps_get_int(caps, "rate", &filter->rate);
  gst_caps_get_int(caps, "channels", &filter->channels);

  if (strcmp(format, "int") == 0) {
    filter->format = GST_PLAYONDEMAND_FORMAT_INT;
    gst_caps_get_int (caps, "width", &filter->width);
  } else if (strcmp(format, "float") == 0) {
    filter->format = GST_PLAYONDEMAND_FORMAT_FLOAT;
  }

  play_on_demand_resize_buffer(filter);

  if (GST_CAPS_IS_FIXED (caps))
    return gst_pad_try_set_caps (filter->srcpad, caps);
  return GST_PAD_LINK_DELAYED;
}

inline static void
play_on_demand_add_play_pointer (GstPlayOnDemand *filter, guint pos)
{
  register guint i;

  if (filter->rate && ((filter->buffer_time * filter->rate) > pos)) {
    for (i = 0; i < filter->max_plays; i++) {
      if (filter->plays[i] == G_MAXUINT) {
        filter->plays[i] = pos;
        /* emit a signal to indicate a sample being played */
        g_signal_emit(filter, gst_pod_filter_signals[PLAYED_SIGNAL], 0);
        break;
      }
    }
  }
}

static void
play_on_demand_loop (GstElement *elem)
{
  GstPlayOnDemand *filter = GST_PLAYONDEMAND(elem);
  guint            num_in, num_out, num_filter;
  GstBuffer       *in, *out;
  static guint     last_tick = 0;

  g_return_if_fail(filter != NULL);
  g_return_if_fail(GST_IS_PLAYONDEMAND(filter));

  filter->bufpool = gst_pad_get_bufferpool(filter->srcpad);

  if (filter->bufpool == NULL)
    filter->bufpool = gst_buffer_pool_get_default(GST_POD_BUFPOOL_SIZE,
                                                  GST_POD_BUFPOOL_NUM);

  in = gst_pad_pull(filter->sinkpad);

  if (filter->format == GST_PLAYONDEMAND_FORMAT_INT) {
    if (filter->width == 16) {
      gint16 min = 0xffff;
      gint16 max = 0x7fff;
      gint16 zero = 0;
#define _TYPE_ gint16
#include "filter.func"
#undef _TYPE_
    } else if (filter->width == 8) {
      gint8 min = 0xff;
      gint8 max = 0x7f;
      gint8 zero = 0;
#define _TYPE_ gint8
#include "filter.func"
#undef _TYPE_
    }
  } else if (filter->format == GST_PLAYONDEMAND_FORMAT_FLOAT) {
    gfloat min = -1.0;
    gfloat max = 1.0;
    gfloat zero = 0.0;
#define _TYPE_ gfloat
#include "filter.func"
#undef _TYPE_
  }
}

static void
play_on_demand_set_clock (GstElement *elem, GstClock *clock)
{
  GstPlayOnDemand *filter;

  g_return_if_fail(elem != NULL);
  g_return_if_fail(GST_IS_PLAYONDEMAND(elem));
  filter = GST_PLAYONDEMAND(elem);

  filter->clock = clock;
}

static void
play_on_demand_play_handler (GstElement *elem)
{
  GstPlayOnDemand *filter;

  g_return_if_fail(elem != NULL);
  g_return_if_fail(GST_IS_PLAYONDEMAND(elem));
  filter = GST_PLAYONDEMAND(elem);

  play_on_demand_add_play_pointer(filter, 0);
}

static void
play_on_demand_clear_handler (GstElement *elem)
{
  GstPlayOnDemand *filter;
  register guint i;

  g_return_if_fail(elem != NULL);
  g_return_if_fail(GST_IS_PLAYONDEMAND(elem));
  filter = GST_PLAYONDEMAND(elem);

  filter->write = 0;
  filter->eos = FALSE;

  for (i = 0; i < filter->max_plays; i++) filter->plays[i] = G_MAXUINT;
  for (i = 0; i < filter->buffer_bytes; i++) filter->buffer[i] = 0;
}

static void
play_on_demand_reset_handler (GstElement *elem)
{
  GstPlayOnDemand *filter;
  register guint i;

  play_on_demand_clear_handler (elem);

  g_return_if_fail(elem != NULL);
  g_return_if_fail(GST_IS_PLAYONDEMAND(elem));
  filter = GST_PLAYONDEMAND(elem);

  for (i = 0; i <= filter->total_ticks / 32; i++) filter->ticks[i] = 0;
}

static void
play_on_demand_resize_buffer (GstPlayOnDemand *filter)
{
  register guint  i;
  guint           new_size, min_size;
  gchar          *new_buffer;

  /* use a default sample rate of 44100, 1 channel, 1 byte per sample if caps
     haven't been set yet */
  new_size  = (guint) filter->buffer_time;
  new_size *= (filter->rate) ? filter->rate : 44100;
  new_size *= (filter->channels) ? filter->channels : 1;

  if (filter->format && filter->format == GST_PLAYONDEMAND_FORMAT_FLOAT)
    new_size *= sizeof(gfloat);
  else
    new_size *= (filter->width) ? filter->width / 8 : 1;

  min_size = (new_size < filter->buffer_bytes) ? new_size : filter->buffer_bytes;

  new_buffer = g_new(gchar, new_size);
  for (i = 0; i < min_size; i++) new_buffer[i] = filter->buffer[i];
  for (i = min_size; i < new_size; i++) new_buffer[i] = 0;

  g_free(filter->buffer);
  filter->buffer = new_buffer;
  filter->buffer_bytes = new_size;
}

static gboolean
plugin_init (GModule *module, GstPlugin *plugin)
{
  GstElementFactory *factory;

  factory = gst_element_factory_new("playondemand",
                                    GST_TYPE_PLAYONDEMAND,
                                    &play_on_demand_details);
  g_return_val_if_fail(factory != NULL, FALSE);

  gst_element_factory_add_pad_template(factory, play_on_demand_src_factory());
  gst_element_factory_add_pad_template(factory, play_on_demand_sink_factory());

  gst_plugin_add_feature (plugin, GST_PLUGIN_FEATURE(factory));

  return TRUE;
}

GstPluginDesc plugin_desc = {
  GST_VERSION_MAJOR,
  GST_VERSION_MINOR,
  "playondemand",
  plugin_init
};
