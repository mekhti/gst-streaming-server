/* GStreamer Streaming Server
 * Copyright (C) 2009-2012 Entropy Wave Inc <info@entropywave.com>
 * Copyright (C) 2009-2012 David Schleef <ds@schleef.org>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"

#include "gss-server.h"
#include "gss-html.h"
#include "gss-session.h"
#include "gss-soup.h"
#include "gss-rtsp.h"
#include "gss-content.h"

enum
{
  PROP_TYPE = 1,
  PROP_WIDTH,
  PROP_HEIGHT,
  PROP_BITRATE
};

#define DEFAULT_TYPE GSS_STREAM_TYPE_UNKNOWN
#define DEFAULT_WIDTH 0
#define DEFAULT_HEIGHT 0
#define DEFAULT_BITRATE 0


#define verbose FALSE

static void msg_wrote_headers (SoupMessage * msg, void *user_data);

void *gss_stream_fd_table[GSS_STREAM_MAX_FDS];

static void gss_stream_finalize (GObject * object);
static void gss_stream_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gss_stream_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GObjectClass *parent_class;


G_DEFINE_TYPE (GssStream, gss_stream, GST_TYPE_OBJECT);

static void
gss_stream_init (GssStream * stream)
{

  stream->metrics = gss_metrics_new ();

  stream->width = 0;
  stream->height = 0;
  stream->bitrate = 0;

  gss_stream_set_type (stream, GSS_STREAM_TYPE_UNKNOWN);

}

static void
gss_stream_class_init (GssStreamClass * stream_class)
{
  G_OBJECT_CLASS (stream_class)->set_property = gss_stream_set_property;
  G_OBJECT_CLASS (stream_class)->get_property = gss_stream_get_property;
  G_OBJECT_CLASS (stream_class)->finalize = gss_stream_finalize;

  g_object_class_install_property (G_OBJECT_CLASS (stream_class),
      PROP_TYPE, g_param_spec_int ("type", "type",
          "type", 0, GSS_STREAM_TYPE_FLV, DEFAULT_TYPE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (stream_class),
      PROP_WIDTH, g_param_spec_int ("width", "Width",
          "Width", 0, 3840, DEFAULT_WIDTH,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (stream_class),
      PROP_HEIGHT, g_param_spec_int ("height", "height",
          "height", 0, 2160, DEFAULT_HEIGHT,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (G_OBJECT_CLASS (stream_class),
      PROP_BITRATE, g_param_spec_int ("bitrate", "Bit Rate",
          "Bit Rate", 0, G_MAXINT, DEFAULT_BITRATE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  parent_class = g_type_class_peek_parent (stream_class);
}

static void
gss_stream_finalize (GObject * object)
{
  GssStream *stream = GSS_STREAM (object);
  int i;

  g_free (stream->playlist_name);
  g_free (stream->codecs);
  g_free (stream->follow_url);

  for (i = 0; i < GSS_STREAM_HLS_CHUNKS; i++) {
    GssHLSSegment *segment = &stream->chunks[i];

    if (segment->buffer) {
      soup_buffer_free (segment->buffer);
      g_free (segment->location);
    }
  }

  if (stream->hls.index_buffer) {
    soup_buffer_free (stream->hls.index_buffer);
  }
#define CLEANUP(x) do { \
  if (x) { \
    if (verbose && GST_OBJECT_REFCOUNT (x) != 1) \
      g_print( #x "refcount %d\n", GST_OBJECT_REFCOUNT (x)); \
    g_object_unref (x); \
  } \
} while (0)

  gss_stream_set_sink (stream, NULL);
  CLEANUP (stream->src);
  CLEANUP (stream->sink);
  CLEANUP (stream->adapter);
  CLEANUP (stream->rtsp_stream);
  if (stream->pipeline) {
    gst_element_set_state (GST_ELEMENT (stream->pipeline), GST_STATE_NULL);
    CLEANUP (stream->pipeline);
  }
  if (stream->adapter)
    g_object_unref (stream->adapter);
  gss_metrics_free (stream->metrics);
}

static void
gss_stream_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GssStream *stream;

  stream = GSS_STREAM (object);

  switch (prop_id) {
    case PROP_TYPE:
      gss_stream_set_type (stream, g_value_get_int (value));
      break;
    case PROP_WIDTH:
      stream->width = g_value_get_int (value);
      break;
    case PROP_HEIGHT:
      stream->height = g_value_get_int (value);
      break;
    case PROP_BITRATE:
      stream->bitrate = g_value_get_int (value);
      break;
    default:
      g_assert_not_reached ();
      break;
  }
}

static void
gss_stream_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GssStream *stream;

  stream = GSS_STREAM (object);

  switch (prop_id) {
    case PROP_TYPE:
      g_value_set_int (value, stream->type);
      break;
    case PROP_WIDTH:
      g_value_set_int (value, stream->width);
      break;
    case PROP_HEIGHT:
      g_value_set_int (value, stream->height);
      break;
    case PROP_BITRATE:
      g_value_set_int (value, stream->bitrate);
      break;
    default:
      g_assert_not_reached ();
      break;
  }
}

const char *
gss_stream_type_get_name (GssStreamType type)
{
  switch (type) {
    case GSS_STREAM_TYPE_OGG:
      return "Ogg/Theora";
    case GSS_STREAM_TYPE_WEBM:
      return "WebM";
    case GSS_STREAM_TYPE_TS:
      return "MPEG-TS";
    case GSS_STREAM_TYPE_TS_MAIN:
      return "MPEG-TS main";
    case GSS_STREAM_TYPE_FLV:
      return "FLV";
    default:
      return "unknown";
  }
}

void
gss_stream_set_type (GssStream * stream, int type)
{
  g_return_if_fail (GSS_IS_STREAM (stream));

  stream->type = type;
  switch (type) {
    case GSS_STREAM_TYPE_UNKNOWN:
      stream->content_type = "unknown/unknown";
      stream->mod = "";
      stream->ext = "";
      break;
    case GSS_STREAM_TYPE_OGG:
      stream->content_type = "video/ogg";
      stream->mod = "";
      stream->ext = "ogv";
      break;
    case GSS_STREAM_TYPE_WEBM:
      stream->content_type = "video/webm";
      stream->mod = "";
      stream->ext = "webm";
      break;
    case GSS_STREAM_TYPE_TS:
      stream->content_type = "video/mp2t";
      stream->mod = "";
      stream->ext = "ts";
      break;
    case GSS_STREAM_TYPE_TS_MAIN:
      stream->content_type = "video/mp2t";
      stream->mod = "-main";
      stream->ext = "ts";
      break;
    case GSS_STREAM_TYPE_FLV:
      stream->content_type = "video/x-flv";
      stream->mod = "";
      stream->ext = "flv";
      break;
    default:
      g_assert_not_reached ();
      break;
  }
}


void
gss_stream_get_stats (GssStream * stream, guint64 * in, guint64 * out)
{
  g_object_get (stream->sink, "bytes-to-serve", in, "bytes-served", out, NULL);
}

static void
client_removed (GstElement * e, int fd, int status, gpointer user_data)
{
  GssStream *stream = user_data;

  if (gss_stream_fd_table[fd]) {
    if (stream) {
      gss_metrics_remove_client (stream->metrics, stream->bitrate);
      gss_metrics_remove_client (stream->program->metrics, stream->bitrate);
      gss_metrics_remove_client (stream->program->server->metrics,
          stream->bitrate);
    }
  }
}

static void
client_fd_removed (GstElement * e, int fd, gpointer user_data)
{
  GssStream *stream = user_data;
  SoupSocket *sock = gss_stream_fd_table[fd];

  if (sock) {
    soup_socket_disconnect (sock);
    gss_stream_fd_table[fd] = NULL;
  } else {
    stream->custom_client_fd_removed (stream, fd, stream->custom_user_data);
  }
}

static void
stream_resource (GssTransaction * t)
{
  GssStream *stream = (GssStream *) t->resource->priv;
  GssConnection *connection;

  if (!stream->program->enable_streaming
      || stream->program->state != GSS_PROGRAM_STATE_RUNNING) {
    soup_message_set_status (t->msg, SOUP_STATUS_NO_CONTENT);
    return;
  }

  if (t->server->metrics->n_clients >= t->server->max_connections ||
      t->server->metrics->bitrate + stream->bitrate >=
      t->server->max_rate * 8000) {
    if (verbose)
      g_print ("n_clients %d max_connections %d\n",
          t->server->metrics->n_clients, t->server->max_connections);
    if (verbose)
      g_print ("current bitrate %" G_GINT64_FORMAT " bitrate %d max_bitrate %d"
          "\n", t->server->metrics->bitrate, stream->bitrate,
          t->server->max_rate * 8000);
    soup_message_set_status (t->msg, SOUP_STATUS_SERVICE_UNAVAILABLE);
    return;
  }

  connection = g_malloc0 (sizeof (GssConnection));
  connection->msg = t->msg;
  connection->client = t->client;
  connection->stream = stream;

  soup_message_set_status (t->msg, SOUP_STATUS_OK);

  soup_message_headers_set_encoding (t->msg->response_headers,
      SOUP_ENCODING_EOF);
  soup_message_headers_replace (t->msg->response_headers, "Content-Type",
      stream->content_type);

  g_signal_connect (t->msg, "wrote-headers", G_CALLBACK (msg_wrote_headers),
      connection);
}

static void
msg_wrote_headers (SoupMessage * msg, void *user_data)
{
  GssConnection *connection = user_data;
  SoupSocket *sock;
  int fd;

  sock = soup_client_context_get_socket (connection->client);
  fd = soup_socket_get_fd (sock);

  if (connection->stream->sink) {
    GssStream *stream = connection->stream;

    g_signal_emit_by_name (connection->stream->sink, "add", fd);

    g_assert (fd < GSS_STREAM_MAX_FDS);
    gss_stream_fd_table[fd] = sock;

    gss_metrics_add_client (stream->metrics, stream->bitrate);
    gss_metrics_add_client (stream->program->metrics, stream->bitrate);
    gss_metrics_add_client (stream->program->server->metrics, stream->bitrate);
  } else {
    soup_socket_disconnect (sock);
  }

  g_free (connection);
}

GssStream *
gss_stream_new (int type, int width, int height, int bitrate)
{
  return g_object_new (GSS_TYPE_STREAM, "type", type,
      "width", width, "height", height, "bitrate", bitrate, NULL);
}

GssStream *
gss_program_add_stream_full (GssProgram * program,
    int type, int width, int height, int bitrate, GstElement * sink)
{
  GssStream *stream;

  stream = gss_stream_new (type, width, height, bitrate);

  gss_program_add_stream (program, stream);

  /* FIXME this should be called before adding the stream, but it fails */
  gss_stream_set_sink (stream, sink);

  return stream;
}

void
gss_stream_add_resources (GssStream * stream)
{
  char *s;

  if (enable_rtsp) {
    if (stream->type == GSS_STREAM_TYPE_OGG) {
      stream->rtsp_stream = gss_rtsp_stream_new (stream);
      gss_rtsp_stream_start (stream->rtsp_stream);
    }
  }

  s = g_strdup_printf ("%s-%dx%d-%dkbps%s.%s",
      GST_OBJECT_NAME (stream->program), stream->width, stream->height,
      stream->bitrate / 1000, stream->mod, stream->ext);
  gst_object_set_name (GST_OBJECT (stream), s);
  g_free (s);
  s = g_strdup_printf ("/%s", GST_OBJECT_NAME (stream));
  gss_server_add_resource (stream->program->server, s, GSS_RESOURCE_HTTP_ONLY,
      stream->content_type, stream_resource, NULL, NULL, stream);
  g_free (s);

  stream->playlist_name = g_strdup_printf ("%s-%dx%d-%dkbps%s-%s.m3u8",
      GST_OBJECT_NAME (stream->program),
      stream->width, stream->height, stream->bitrate / 1000, stream->mod,
      stream->ext);
  s = g_strdup_printf ("/%s", stream->playlist_name);
  gss_server_add_resource (stream->program->server, s, 0,
      "application/x-mpegurl", gss_stream_handle_m3u8, NULL, NULL, stream);
  g_free (s);

  return;
}

void
gss_stream_set_sink (GssStream * stream, GstElement * sink)
{
  if (stream->sink) {
    g_object_unref (stream->sink);
  }

  stream->sink = sink;
  if (stream->sink) {
    g_object_ref (stream->sink);
    g_signal_connect (stream->sink, "client-removed",
        G_CALLBACK (client_removed), stream);
    g_signal_connect (stream->sink, "client-fd-removed",
        G_CALLBACK (client_fd_removed), stream);
    if (stream->type == GSS_STREAM_TYPE_TS ||
        stream->type == GSS_STREAM_TYPE_TS_MAIN) {
      gss_stream_add_hls (stream);
    }
  }
}
