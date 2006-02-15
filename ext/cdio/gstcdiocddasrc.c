/* GStreamer
 * Copyright (C) 2006 Tim-Philipp Müller <tim centricular net>
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

/**
 * SECTION:element-cdiocddasrc
 * @short_description: Reads raw audio from an Audio CD
 * @see_also: GstCdParanoiaSrc, GstCddaBaseSrc
 *
 * <refsect2>
 * <para>
 * cdiocddasrc reads and extracts raw audio from Audio CDs. It can operate
 * in one of two modes:
 * <itemizedlist>
 *  <listitem><para>
 *    treat each track as a separate stream, counting time from the start
 *    of the track to the end of the track and posting EOS at the end of
 *    a track, or
 *  </para></listitem>
 *  <listitem><para>
 *    treat the entire disc as one stream, counting time from the start of
 *    the first track to the end of the last track, posting EOS only at
 *    the end of the last track.
 *  </para></listitem>
 * </itemizedlist>
 * </para>
 * <para>
 * With a recent-enough version of libcdio, the element will extract
 * CD-TEXT if this is supported by the CD-drive and CD-TEXT information
 * is available on the CD. The information will be posted on the bus in
 * form of a tag message.
 * </para>
 * <para>
 * When opened, the element will also calculate a CDDB disc ID and a
 * MusicBrainz disc ID, which applications can use to query online
 * databases for artist/title information. These disc IDs will also be
 * posted on the bus as part of the tag messages.
 * </para>
 * <para>
 * cdiocddasrc supports the GstUriHandler interface, so applications can use
 * playbin with cdda://&lt;track-number&gt; URIs for playback (they will have
 * to connect to playbin's notify::source signal and set the device on the
 * cd source in the notify callback if they want to set the device property).
 * Applications should use seeks in "track" format to switch between different
 * tracks of the same CD (passing a new cdda:// URI to playbin involves opening
 * and closing the CD device, which is much slower).
 * </para>
 * <title>Example launch line</title>
 * <para>
 * <programlisting>
 * gst-launch cdiocddasrc track=5 device=/dev/cdrom ! audioconvert ! vorbisenc ! oggmux ! filesink location=track5.ogg
 * </programlisting>
 * This pipeline extracts track 5 of the audio CD and encodes it into an
 * Ogg/Vorbis file.
 * </para>
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstcdio.h"
#include "gstcdiocddasrc.h"

#include <gst/gst.h>
#include "gst/gst-i18n-plugin.h"

#include <sys/types.h>
#include <string.h>
#include <errno.h>

enum
{
  PROP0 = 0,
  PROP_READ_SPEED
};

static GstElementDetails gst_cdio_cdda_src_details = {
  "CD Audio (cdda) Source",
  "Source/File",
  "Read audio from CD using libcdio",
  "Tim-Philipp Müller <tim centricular net>",
};

GST_BOILERPLATE (GstCdioCddaSrc, gst_cdio_cdda_src, GstCddaBaseSrc,
    GST_TYPE_CDDA_BASE_SRC);

static void gst_cdio_cdda_src_finalize (GObject * obj);
static void gst_cdio_cdda_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_cdio_cdda_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gchar *gst_cdio_cdda_src_get_default_device (GstCddaBaseSrc * src);
static GstBuffer *gst_cdio_cdda_src_read_sector (GstCddaBaseSrc * src,
    gint sector);
static gboolean gst_cdio_cdda_src_open (GstCddaBaseSrc * src,
    const gchar * device);
static void gst_cdio_cdda_src_close (GstCddaBaseSrc * src);

#define DEFAULT_READ_SPEED   -1

static void
gst_cdio_cdda_src_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_set_details (element_class, &gst_cdio_cdda_src_details);
}

static gchar *
gst_cdio_cdda_src_get_default_device (GstCddaBaseSrc * cddabasesrc)
{
  GstCdioCddaSrc *src;
  gchar *default_device, *ret;

  src = GST_CDIO_CDDA_SRC (cddabasesrc);

  /* src->cdio may be NULL here */
  default_device = cdio_get_default_device (src->cdio);

  ret = g_strdup (default_device);
  free (default_device);

  GST_LOG_OBJECT (src, "returning default device: %s", GST_STR_NULL (ret));

  return ret;
}

static gchar **
gst_cdio_cdda_src_probe_devices (GstCddaBaseSrc * cddabasesrc)
{
  char **devices, **ret, **d;

  /* FIXME: might return the same hardware device twice, e.g.
   * as /dev/cdrom and /dev/dvd - gotta do something more sophisticated */
  devices = cdio_get_devices (DRIVER_DEVICE);

  if (devices == NULL) {
    GST_DEBUG_OBJECT (cddabasesrc, "no devices found");
    return NULL;
  }

  if (*devices == NULL) {
    GST_DEBUG_OBJECT (cddabasesrc, "no devices found");
    free (devices);
    return NULL;
  }

  ret = g_strdupv (devices);
  for (d = devices; *d != NULL; ++d) {
    GST_DEBUG_OBJECT (cddabasesrc, "device: %s", GST_STR_NULL (*d));
    free (*d);
  }
  free (devices);

  return ret;
}

static GstBuffer *
gst_cdio_cdda_src_read_sector (GstCddaBaseSrc * cddabasesrc, gint sector)
{
  GstFlowReturn flowret;
  GstCdioCddaSrc *src;
  GstBuffer *buf;

  src = GST_CDIO_CDDA_SRC (cddabasesrc);

  flowret = gst_pad_alloc_buffer (GST_BASE_SRC (src)->srcpad,
      GST_BUFFER_OFFSET_NONE, CDIO_CD_FRAMESIZE_RAW,
      GST_PAD_CAPS (GST_BASE_SRC (src)->srcpad), &buf);

  if (flowret != GST_FLOW_OK) {
    GST_DEBUG_OBJECT (src, "gst_pad_alloc_buffer() failed! (ret=%d)", flowret);
    return NULL;
  }

  if (cdio_read_audio_sector (src->cdio, GST_BUFFER_DATA (buf), sector) != 0) {
    GST_WARNING_OBJECT (src, "read at sector %d failed!", sector);
    GST_ELEMENT_ERROR (src, RESOURCE, READ,
        (_("Could not read from CD.")),
        ("cdio_read_audio_sector at %d failed: %s", sector,
            g_strerror (errno)));
    gst_buffer_unref (buf);
    return NULL;
  }

  return buf;
}

static gboolean
notcdio_track_is_audio_track (const CdIo * p_cdio, track_t i_track)
{
  return (cdio_get_track_format (p_cdio, i_track) == TRACK_FORMAT_AUDIO);
}

#if (LIBCDIO_VERSION_NUM >= 76)

static GstTagList *
gst_cdio_cdda_src_get_cdtext (GstCdioCddaSrc * src, track_t i_track)
{
  GstTagList *tags = NULL;
  GstObject *obj;
  cdtext_t *t;

  t = cdio_get_cdtext (src->cdio, i_track);
  if (t == NULL) {
    GST_DEBUG_OBJECT (src, "no CD-TEXT for track %u", i_track);
    return NULL;
  }

  obj = GST_OBJECT (src);
  gst_cdio_add_cdtext_field (obj, t, CDTEXT_PERFORMER, GST_TAG_ARTIST, &tags);
  gst_cdio_add_cdtext_field (obj, t, CDTEXT_TITLE, GST_TAG_TITLE, &tags);

  return tags;
}

#else

static GstTagList *
gst_cdio_cdda_src_get_cdtext (GstCdioCddaSrc * src, track_t i_track)
{
  GST_DEBUG_OBJECT (src, "This libcdio version (%u) does not support "
      "CDTEXT (want >= 76)", LIBCDIO_VERSION_NUM);
  return NULL;
}

#endif

static gboolean
gst_cdio_cdda_src_open (GstCddaBaseSrc * cddabasesrc, const gchar * device)
{
  GstCdioCddaSrc *src;
  discmode_t discmode;
  gint first_track, num_tracks, i;

  src = GST_CDIO_CDDA_SRC (cddabasesrc);

  g_assert (device != NULL);
  g_assert (src->cdio == NULL);

  GST_LOG_OBJECT (src, "trying to open device %s", device);

  src->cdio = cdio_open (device, DRIVER_UNKNOWN);

  if (src->cdio == NULL) {
    GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ,
        (_("Could not open CD device for reading.")),
        ("cdio_open() failed: %s", g_strerror (errno)));
    return FALSE;
  }

  discmode = cdio_get_discmode (src->cdio);
  GST_LOG_OBJECT (src, "discmode: %d", (gint) discmode);

  if (discmode != CDIO_DISC_MODE_CD_DA && discmode != CDIO_DISC_MODE_CD_MIXED) {
    GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ,
        (_("Disc is not an Audio CD.")), ("discmode: %d", (gint) discmode));
    return FALSE;
  }

  first_track = cdio_get_first_track_num (src->cdio);
  num_tracks = cdio_get_num_tracks (src->cdio);

  if (num_tracks <= 0 || first_track < 0)
    return TRUE;                /* base class will generate 'has no tracks' error */

  if (src->read_speed != -1) {
#if (LIBCDIO_VERSION_NUM >= 76)
    cdio_set_speed (src->cdio, src->read_speed);
#else
    GST_WARNING_OBJECT (src, "This libcdio version (%u) does not support "
        "setting the drive reading speed (want >= 76)", LIBCDIO_VERSION_NUM);
#endif
  }

  GST_LOG_OBJECT (src, "%u tracks, first track: %d", num_tracks, first_track);

  for (i = 0; i < num_tracks; ++i) {
    GstCddaBaseSrcTrack track = { 0, };
    gint len_sectors;

    len_sectors = cdio_get_track_sec_count (src->cdio, i + first_track);

    track.num = i + first_track;
    track.is_audio = notcdio_track_is_audio_track (src->cdio, i + first_track);

    /* Note: LSN/LBA confusion all around us; in any case, this does
     * the right thing here (for cddb id calculations etc. as well) */
    track.start = cdio_get_track_lsn (src->cdio, i + first_track);
    track.end = track.start + len_sectors - 1;  /* -1? */
    track.tags = gst_cdio_cdda_src_get_cdtext (src, i + first_track);

    gst_cdda_base_src_add_track (GST_CDDA_BASE_SRC (src), &track);
  }

  return TRUE;
}

static void
gst_cdio_cdda_src_close (GstCddaBaseSrc * cddabasesrc)
{
  GstCdioCddaSrc *src = GST_CDIO_CDDA_SRC (cddabasesrc);

  g_assert (src->cdio != NULL);

  if (src->cdio) {
    cdio_destroy (src->cdio);
    src->cdio = NULL;
  }
}

static void
gst_cdio_cdda_src_init (GstCdioCddaSrc * src, GstCdioCddaSrcClass * klass)
{
  src->read_speed = DEFAULT_READ_SPEED; /* don't need atomic access here */
  src->cdio = NULL;
}

static void
gst_cdio_cdda_src_finalize (GObject * obj)
{
  if (G_OBJECT_CLASS (parent_class)->finalize)
    G_OBJECT_CLASS (parent_class)->finalize (obj);
}

static void
gst_cdio_cdda_src_class_init (GstCdioCddaSrcClass * klass)
{
  GstCddaBaseSrcClass *cddabasesrc_class = GST_CDDA_BASE_SRC_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->set_property = gst_cdio_cdda_src_set_property;
  gobject_class->get_property = gst_cdio_cdda_src_get_property;
  gobject_class->finalize = gst_cdio_cdda_src_finalize;

  cddabasesrc_class->open = gst_cdio_cdda_src_open;
  cddabasesrc_class->close = gst_cdio_cdda_src_close;
  cddabasesrc_class->read_sector = gst_cdio_cdda_src_read_sector;
  cddabasesrc_class->probe_devices = gst_cdio_cdda_src_probe_devices;
  cddabasesrc_class->get_default_device = gst_cdio_cdda_src_get_default_device;

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_READ_SPEED,
      g_param_spec_int ("read-speed", "Read speed",
          "Read from device at the specified speed (-1 = default)", -1, 100,
          DEFAULT_READ_SPEED, G_PARAM_READWRITE));
}

static void
gst_cdio_cdda_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstCdioCddaSrc *src = GST_CDIO_CDDA_SRC (object);

  switch (prop_id) {
    case PROP_READ_SPEED:{
      gint speed;

      speed = g_value_get_int (value);
      gst_atomic_int_set (&src->read_speed, speed);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_cdio_cdda_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstCdioCddaSrc *src = GST_CDIO_CDDA_SRC (object);

  switch (prop_id) {
    case PROP_READ_SPEED:{
      gint speed;

      speed = g_atomic_int_get (&src->read_speed);
      g_value_set_int (value, speed);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}