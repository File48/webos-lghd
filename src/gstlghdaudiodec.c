/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * lghdaudio - GStreamer plugin for LG webOS 24 (tested: OLED C2, webOS 9.2.2)
 *
 * lghdaudiodec: decodes Dolby TrueHD (native 5.1 presentation) and DTS (incl. DTS-HD MA)
 *   with FFmpeg to 8ch S32LE PCM in the slot order LG's DSP expects for multichannel PCM:
 *     slot 0..7 = FL FR LFE FC RL RR 0 0   (LG swaps C/LFE vs WAV order)
 *   Sets "pcm-multi-ch"=TRUE on LG's sink, then tells audiooutputd the pipeline carries
 *   PCM (LG's avconnector reports the source codec, e.g. "true-hd"/8, so audiooutputd
 *   picks stereo): setAudioInputInfo codec=pcm/6 with LG's own pipelineId (argv
 *   -scom.webos.pipeline.<id>) and ADEC resource ~1 s after first output; re-sent if the
 *   eARC mode drifts. The eARC ALSA control is only ever read.
 * lghddvtag: for HEVC from LG's matroskademux, finds Dolby Vision RPU (NAL 62) / EL (NAL 63)
 *   in the first access unit and adds dolby-vision=TRUE, dolby-vision-profile, format=dvh1
 *   to the caps; LG's dvbin/lxvideodec need these to open the DV path (qtdemux sets them,
 *   matroskademux does not).
 *
 * Settings: environment variable, else /media/developer/lghd/lghd.conf (KEY=value lines).
 *   LGHD_BATCH_MS=n      output batch ms (default 32, 0 = per access unit)
 *   LGHD_CH=n            channels reported to audiooutputd (2/6/8, default 6)
 *   LGHD_SWAP_CLFE=0     no C/LFE swap         LGHD_NO_SINKPROP=1   don't set pcm-multi-ch
 *   LGHD_NO_EARC=1       no setAudioInputInfo  LGHD_SET_LATENCY=0   don't report batch latency
 *   LGHD_PIPELINE_ID=_x  pipelineId outside starfish (gst-launch tests)
 *   LGHD_RESOURCE=ADECn  override the ADEC resource
 *   LGHD_DVTAG=0         pass HEVC caps through   LGHD_DV_PROFILE=n  force DV profile
 *   LGHD_DVTAG_REGISTER=0  don't register lghddvtag
 *   LGHD_GST_DEBUG=spec  GStreamer debug spec, output in /tmp/lghd_gst.log
 * Log: /tmp/lghd.log
 *
 * Copyright (C) 2026 lghdaudio contributors. Developed with AI assistance (Claude).
 */
#define GLIB_VERSION_MIN_REQUIRED GLIB_VERSION_2_68
#define GLIB_VERSION_MAX_ALLOWED GLIB_VERSION_2_72

#include <string.h>
#include <stdlib.h>
#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <gst/audio/gstaudiodecoder.h>
#include <alsa/asoundlib.h>
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

GST_DEBUG_CATEGORY_STATIC (lghd_debug);
#define GST_CAT_DEFAULT lghd_debug

#define OUT_CH 8
#define EARC_CTL_NAME  "Sndout EARC Outputtype"
#define EARC_ONOFF     "Sndout EARC OnOff"
#define LGHD_VERSION   "2.4"
#define LOG_PATH       "/tmp/lghd.log"
#define GSTLOG_PATH    "/tmp/lghd_gst.log"
#define CONF_PATH      "/media/developer/lghd/lghd.conf"
#define LUNA_URL       "luna://com.webos.service.audiooutput/quality/setQualityAttribute"

extern char **environ;

typedef struct _GstLgHdAudioDec {
  GstAudioDecoder parent;

  AVCodecContext *ctx;
  AVCodecParserContext *parser;
  AVCodecContext *pctx;       /* separate context for the parser (as libavformat does) */
  gboolean use_parser;
  guint64 n_in, n_out_frames, n_out_samples;
  AVPacket *pkt;
  AVFrame *frame;
  enum AVCodecID codec_id;

  gint out_rate;              /* negotiated output rate, 0 = not yet */
  gboolean swap_clfe;
  gboolean do_earc;
  gboolean do_sinkprop;
  gint report_ch;
  guint batch_bytes_per_ms;   /* computed per rate */
  guint batch_ms;
  guint pending;              /* input frames not yet finished */
  GstElement *sink;           /* LG audio sink (ref) */
  gchar pipeline_id[64];
  gchar resource[16];

  GByteArray *out;            /* decoded, converted output for current input */

  /* pipeline hooks */
  GstBin *top;
  gulong deep_added_id;

  /* eARC manager */
  GThread *earc_thread;
  gint earc_run;              /* atomic */
  gint playing;               /* atomic: set after first output buffer */
} GstLgHdAudioDec;

typedef struct _GstLgHdAudioDecClass {
  GstAudioDecoderClass parent_class;
} GstLgHdAudioDecClass;

#define GST_TYPE_LGHD_AUDIO_DEC (gst_lghd_audio_dec_get_type ())
#define GST_LGHD_AUDIO_DEC(o) ((GstLgHdAudioDec *)(o))
GType gst_lghd_audio_dec_get_type (void);
G_DEFINE_TYPE (GstLgHdAudioDec, gst_lghd_audio_dec, GST_TYPE_AUDIO_DECODER);

static GstStaticPadTemplate sink_tmpl = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-true-hd; audio/x-dts; "
        "audio/x-unknown, codec-id=(string){ A_DTS, A_TRUEHD }"));

static GstStaticPadTemplate src_tmpl = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, format=(string)S32LE, layout=(string)interleaved, "
        "rate=(int)[ 8000, 192000 ], channels=(int)8, channel-mask=(bitmask)0xc3f"));

/* Settings: environment first (gst-launch tests), then CONF_PATH, a file of
 * KEY=value lines using the same names (LG's pipeline process can't be given
 * environment variables). Returns a newly allocated string or NULL. */
static gchar *
cfg_get (const gchar * name)
{
  const gchar *e = g_getenv (name);
  gchar *buf = NULL, **lines, *res = NULL;
  gsize n = strlen (name);
  int i;

  if (e && *e)
    return g_strdup (e);
  if (!g_file_get_contents (CONF_PATH, &buf, NULL, NULL))
    return NULL;
  lines = g_strsplit (buf, "\n", -1);
  for (i = 0; lines[i]; i++) {
    gchar *l = g_strstrip (lines[i]);
    if (strncmp (l, name, n) == 0 && l[n] == '=') {
      g_free (res);
      res = g_strdup (l + n + 1);
    }
  }
  g_strfreev (lines);
  g_free (buf);
  return res;
}

static gint
cfg_int (const gchar * name, gint def)
{
  gchar *v = cfg_get (name);
  gint r = (v && *v) ? atoi (v) : def;
  g_free (v);
  return r;
}

static gboolean
env_on (const gchar * name, gboolean def)
{
  gchar *vv = cfg_get (name);
  const gchar *v = vv;
  gboolean r;
  if (!v || !*v) {
    g_free (vv);
    return def;
  }
  r = !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
  g_free (vv);
  return r;
}

/* ------------------------------------------------------------------------ */
/* ALSA eARC control                                                         */

/* small append-only log, shared by all instances in the process */
static GMutex log_lock;

static void
lghd_log (const gchar * fmt, ...)
{
  va_list ap;
  FILE *f;
  struct timespec ts;
  struct tm tm;
  char tb[32];

  clock_gettime (CLOCK_REALTIME, &ts);
  localtime_r (&ts.tv_sec, &tm);
  strftime (tb, sizeof tb, "%H:%M:%S", &tm);
  g_mutex_lock (&log_lock);
  f = fopen (LOG_PATH, "a");
  if (f) {
    fprintf (f, "%s.%03ld [%d] ", tb, ts.tv_nsec / 1000000, (int) getpid ());
    va_start (ap, fmt);
    vfprintf (f, fmt, ap);
    va_end (ap);
    fputc ('\n', f);
    fclose (f);
  }
  g_mutex_unlock (&log_lock);
}

/* ------------------------------------------------------------------------ */
/* Optional capture of the whole process's GStreamer debug log              */

static FILE *gstlog_f;
static gsize gstlog_bytes;
static GMutex gstlog_lock;

static void
gstlog_fn (GstDebugCategory * cat, GstDebugLevel level, const gchar * file,
    const gchar * function, gint line, GObject * object, GstDebugMessage * message,
    gpointer user_data)
{
  const gchar *msg;
  gchar *obj = NULL;
  GstClockTime t;

  if (level > gst_debug_category_get_threshold (cat))
    return;
  msg = gst_debug_message_get (message);
  if (object && GST_IS_OBJECT (object))
    obj = g_strdup (GST_OBJECT_NAME (object) ? GST_OBJECT_NAME (object) : "");
  t = gst_util_get_timestamp ();
  g_mutex_lock (&gstlog_lock);
  if (gstlog_f && gstlog_bytes < 64 * 1024 * 1024) {
    int w = fprintf (gstlog_f, "%" GST_TIME_FORMAT " %p %s %s %s:%d:%s:<%s> %s\n",
        GST_TIME_ARGS (t), (void *) g_thread_self (), gst_debug_level_get_name (level),
        gst_debug_category_get_name (cat), file, line, function, obj ? obj : "",
        msg ? msg : "");
    if (w > 0)
      gstlog_bytes += w;
    fflush (gstlog_f);
  }
  g_mutex_unlock (&gstlog_lock);
  g_free (obj);
}

static void
maybe_capture_gst_debug (void)
{
  static gboolean done = FALSE;
  gchar *spec;

  if (done)
    return;
  done = TRUE;
  spec = cfg_get ("LGHD_GST_DEBUG");
  if (!spec || !*spec) {
    g_free (spec);
    return;
  }
  gstlog_f = fopen (GSTLOG_PATH, "a");
  if (!gstlog_f) {
    lghd_log ("cannot open %s", GSTLOG_PATH);
    g_free (spec);
    return;
  }
  gst_debug_set_active (TRUE);
  gst_debug_set_threshold_from_string (spec, TRUE);
  gst_debug_add_log_function (gstlog_fn, NULL, NULL);
  lghd_log ("capturing GStreamer debug '%s' to %s", spec, GSTLOG_PATH);
  g_free (spec);
}

/* read-only access to the eARC ALSA controls */
static gboolean
ctl_read (const gchar * name, long *vals, int n)
{
  snd_ctl_t *h = NULL;
  snd_ctl_elem_value_t *v;
  int i, r;

  if (snd_ctl_open (&h, "hw:0", 0) < 0)
    return FALSE;
  snd_ctl_elem_value_alloca (&v);
  snd_ctl_elem_value_set_interface (v, SND_CTL_ELEM_IFACE_MIXER);
  snd_ctl_elem_value_set_name (v, name);
  r = snd_ctl_elem_read (h, v);
  if (r >= 0)
    for (i = 0; i < n; i++)
      vals[i] = snd_ctl_elem_value_get_integer (v, i);
  snd_ctl_close (h);
  return r >= 0;
}

/* pipelineId of this starfish-media-pipeline process:
 * argv contains "-scom.webos.pipeline._XXXXXXXXXXXXXX" */
static gboolean
find_pipeline_id (gchar * out, gsize len)
{
  gchar *env = cfg_get ("LGHD_PIPELINE_ID");
  gchar *buf = NULL;
  gsize n = 0, i;
  const gchar *key = "-scom.webos.pipeline.";
  gboolean ok = FALSE;

  if (env && *env) {
    g_strlcpy (out, env, len);
    g_free (env);
    return TRUE;
  }
  g_free (env);
  if (!g_file_get_contents ("/proc/self/cmdline", &buf, &n, NULL))
    return FALSE;
  for (i = 0; i < n; i += strlen (buf + i) + 1) {
    if (g_str_has_prefix (buf + i, key)) {
      g_strlcpy (out, buf + i + strlen (key), len);
      ok = TRUE;
      break;
    }
  }
  g_free (buf);
  return ok;
}

static gboolean
id_is_safe (const gchar * s)
{
  if (!s || !*s)
    return FALSE;
  for (; *s; s++)
    if (!g_ascii_isalnum (*s) && *s != '_' && *s != '-' && *s != '.')
      return FALSE;
  return TRUE;
}

/* ADEC resource from the LG sink's resource-info ("res, audio-port=(int)N") */
static gboolean
find_resource (GstLgHdAudioDec * self, gchar * out, gsize len)
{
  gchar *env = cfg_get ("LGHD_RESOURCE");
  GParamSpec *ps;
  gint port = -1;
  guint idx = G_MAXUINT;

  if (env && *env) {
    g_strlcpy (out, env, len);
    g_free (env);
    return TRUE;
  }
  g_free (env);
  if (!self->sink)
    return FALSE;
  ps = g_object_class_find_property (G_OBJECT_GET_CLASS (self->sink), "resource-info");
  if (ps && G_PARAM_SPEC_VALUE_TYPE (ps) == GST_TYPE_STRUCTURE) {
    GstStructure *st = NULL;
    g_object_get (self->sink, "resource-info", &st, NULL);
    if (st) {
      gchar *str = gst_structure_to_string (st);
      lghd_log ("sink resource-info: %s", str);
      g_free (str);
      if (!gst_structure_get_int (st, "audio-port", &port))
        port = -1;
      gst_structure_free (st);
    } else {
      lghd_log ("sink resource-info: (null)");
    }
  } else if (ps && G_PARAM_SPEC_VALUE_TYPE (ps) == GST_TYPE_CAPS) {
    GstCaps *c = NULL;
    g_object_get (self->sink, "resource-info", &c, NULL);
    if (c) {
      gchar *str = gst_caps_to_string (c);
      lghd_log ("sink resource-info (caps): %s", str);
      g_free (str);
      if (gst_caps_get_size (c) > 0 &&
          !gst_structure_get_int (gst_caps_get_structure (c, 0), "audio-port", &port))
        port = -1;
      gst_caps_unref (c);
    }
  } else {
    lghd_log ("sink has no structure/caps resource-info property (type %s)",
        ps ? g_type_name (G_PARAM_SPEC_VALUE_TYPE (ps)) : "none");
  }
  if (g_object_class_find_property (G_OBJECT_GET_CLASS (self->sink), "index")) {
    g_object_get (self->sink, "index", &idx, NULL);
    lghd_log ("sink index: %u", idx);
  }
  if (port < 0 && idx < 8) {
    /* resource-info reads back NULL on LG's sink (set-only in practice);
     * the sink's "index" is the same audio index it derives from it */
    port = (gint) idx;
    lghd_log ("using sink index %u for the ADEC resource", idx);
  }
  if (port < 0)
    return FALSE;
  g_snprintf (out, len, "ADEC%d", port);
  return TRUE;
}

static const gchar *
luna_send_path (void)
{
  static const gchar *c[] = { "/usr/bin/luna-send", "/usr/sbin/luna-send", "/bin/luna-send", NULL };
  int i;
  for (i = 0; c[i]; i++)
    if (access (c[i], X_OK) == 0)
      return c[i];
  return NULL;
}

/* run luna-send -n 1 URL JSON; its reply is appended to the log. 5 s timeout. */
static int
luna_call (const gchar * url, const gchar * json)
{
  const gchar *bin = luna_send_path ();
  posix_spawn_file_actions_t fa;
  char *argv[6];
  pid_t pid;
  int r, i, st;

  if (!bin) {
    lghd_log ("luna-send not found");
    return -1;
  }
  argv[0] = (char *) "luna-send";
  argv[1] = (char *) "-n";
  argv[2] = (char *) "1";
  argv[3] = (char *) url;
  argv[4] = (char *) json;
  argv[5] = NULL;

  posix_spawn_file_actions_init (&fa);
  posix_spawn_file_actions_addopen (&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen (&fa, 1, LOG_PATH, O_WRONLY | O_APPEND | O_CREAT, 0644);
  posix_spawn_file_actions_addopen (&fa, 2, LOG_PATH, O_WRONLY | O_APPEND | O_CREAT, 0644);
  r = posix_spawn (&pid, bin, &fa, NULL, argv, environ);
  posix_spawn_file_actions_destroy (&fa);
  if (r != 0) {
    lghd_log ("posix_spawn failed: %d", r);
    return -1;
  }
  for (i = 0; i < 50; i++) {
    pid_t w = waitpid (pid, &st, WNOHANG);
    if (w == pid) {
      return WIFEXITED (st) ? WEXITSTATUS (st) : -2;
    }
    if (w < 0) {
      return errno == ECHILD ? 0 : -3;   /* SIGCHLD ignored: child auto-reaped */
    }
    g_usleep (100 * 1000);
  }
  kill (pid, SIGKILL);
  waitpid (pid, NULL, 0);
  lghd_log ("luna-send timed out (no reply in 5 s)");
  return -4;
}

static void
send_pcm_info (GstLgHdAudioDec * self, const gchar * why)
{
  gchar *json = g_strdup_printf ("{\"categoryType\":\"audioInputInfo\","
      "\"requestType\":\"setAudioInputInfo\",\"activeState\":true,"
      "\"pipelineInfo\":{\"pipelineId\":\"%s\",\"streamType\":\"umimedia\","
      "\"contentType\":\"movie\",\"sourceType\":\"MEDIA\",\"resource\":\"%s\","
      "\"sourceAttribute\":\"MEDIA\",\"reset\":false,\"gameInfo\":{\"isGameMode\":false}},"
      "\"categoryInfo\":{\"codec\":\"pcm\",\"channel\":%d,\"immersive\":\"none\"}}",
      self->pipeline_id, self->resource, self->report_ch);
  int rc;
  lghd_log ("send pcm/%d (%s) pipeline=%s resource=%s", self->report_ch, why,
      self->pipeline_id, self->resource);
  rc = luna_call (LUNA_URL, json);
  lghd_log ("luna-send rc=%d", rc);
  g_free (json);
}

static gboolean
earc_is_multich (long *cur)
{
  return cur[1] == 1 && cur[2] >= 6;
}

static gpointer
earc_thread_fn (gpointer data)
{
  /* ~1 s after the first decoded buffer: tell audiooutputd this pipeline is
   * PCM. Then every second: if on eARC and the output type is not
   * multichannel PCM, tell it again (rate-limited, capped). */
  GstLgHdAudioDec *self = data;
  int ticks = 0, sends = 0, last_send = -100, retries_unreached = 0;
  gboolean reached = FALSE, initial_done = FALSE;
  long last[5] = { -1, -1, -1, -1, -1 };

  while (g_atomic_int_get (&self->earc_run)) {
    long onoff[1] = { 0 }, cur[5] = { 0 };
    g_usleep (250 * 1000);
    if (!g_atomic_int_get (&self->playing))
      continue;
    ticks++;
    if (ticks % 4 != 0)
      continue;                 /* act once per second */

    if (!ctl_read (EARC_ONOFF, onoff, 1) || !ctl_read (EARC_CTL_NAME, cur, 5))
      continue;
    if (memcmp (cur, last, sizeof cur) != 0) {
      lghd_log ("eARC onoff=%ld outputtype=%ld,%ld,%ld,%ld,%ld (t=%ds)", onoff[0],
          cur[0], cur[1], cur[2], cur[3], cur[4], ticks / 4);
      memcpy (last, cur, sizeof cur);
    }
    if (onoff[0] != 1)
      continue;                 /* not on eARC: leave audiooutputd alone */

    if (earc_is_multich (cur))
      reached = TRUE;

    if (sends == 0 && !initial_done) {
      initial_done = TRUE;
      if (earc_is_multich (cur)) {
        lghd_log ("eARC already multichannel PCM: no need to send");
        continue;
      }
      send_pcm_info (self, "initial");
      sends++;
      last_send = ticks;
    } else if (!earc_is_multich (cur) && ticks - last_send >= 12) {
      /* Never reached multichannel: the sink/device may not take it, so retry
       * only twice (each send makes audiooutputd reconfigure the output).
       * Reached it and then lost it (LG re-sent its own info): restore, capped. */
      if ((!reached && retries_unreached < 2) || (reached && sends < 20)) {
        if (!reached)
          retries_unreached++;
        send_pcm_info (self, reached ? "drift" : "retry");
        sends++;
        last_send = ticks;
      }
    }
  }
  lghd_log ("watchdog stop (sends=%d)", sends);
  return NULL;
}

static void
earc_start (GstLgHdAudioDec * self)
{
  if (!self->do_earc || self->earc_thread)
    return;
  if (!id_is_safe (self->pipeline_id) && !(find_pipeline_id (self->pipeline_id,
              sizeof self->pipeline_id) && id_is_safe (self->pipeline_id))) {
    lghd_log ("no pipelineId (not in starfish and LGHD_PIPELINE_ID unset): not sending");
    return;
  }
  if (!find_resource (self, self->resource, sizeof self->resource) ||
      !id_is_safe (self->resource)) {
    lghd_log ("no ADEC resource found: not sending");
    return;
  }
  lghd_log ("pipelineId=%s resource=%s", self->pipeline_id, self->resource);
  g_atomic_int_set (&self->earc_run, 1);
  self->earc_thread = g_thread_new ("lghd-earc", earc_thread_fn, self);
}

static void
earc_stop (GstLgHdAudioDec * self)
{
  if (!self->earc_thread)
    return;
  g_atomic_int_set (&self->earc_run, 0);
  g_thread_join (self->earc_thread);
  self->earc_thread = NULL;
}

/* ------------------------------------------------------------------------ */
/* LG sink property                                                          */

static void
maybe_set_sink_prop (GstLgHdAudioDec * self, GstElement * e)
{
  const gchar *tn = G_OBJECT_TYPE_NAME (e);
  if (tn && strcmp (tn, "GstLgAudioRender") == 0 &&
      g_object_class_find_property (G_OBJECT_GET_CLASS (e), "pcm-multi-ch")) {
    GST_INFO_OBJECT (self, "setting pcm-multi-ch=TRUE on %s", GST_ELEMENT_NAME (e));
    g_object_set (e, "pcm-multi-ch", TRUE, NULL);
    if (!self->sink) {
      self->sink = gst_object_ref (e);
      lghd_log ("found LG sink %s, set pcm-multi-ch", GST_ELEMENT_NAME (e));
    }
  }
}

static void
on_deep_element_added (GstBin * bin, GstBin * sub, GstElement * e, gpointer data)
{
  maybe_set_sink_prop (GST_LGHD_AUDIO_DEC (data), e);
}

static void
hook_pipeline (GstLgHdAudioDec * self)
{
  GstObject *o, *p;
  GstIterator *it;
  GValue item = G_VALUE_INIT;

  if (!self->do_sinkprop || self->top)
    return;

  o = gst_object_ref (GST_OBJECT (self));
  while ((p = gst_object_get_parent (o))) {
    gst_object_unref (o);
    o = p;
  }
  if (!GST_IS_BIN (o) || o == GST_OBJECT (self)) {
    gst_object_unref (o);
    return;
  }
  self->top = GST_BIN (o);      /* keep ref */

  it = gst_bin_iterate_recurse (self->top);
  while (gst_iterator_next (it, &item) == GST_ITERATOR_OK) {
    maybe_set_sink_prop (self, GST_ELEMENT (g_value_get_object (&item)));
    g_value_reset (&item);
  }
  g_value_unset (&item);
  gst_iterator_free (it);

  self->deep_added_id = g_signal_connect (self->top, "deep-element-added",
      G_CALLBACK (on_deep_element_added), self);
}

static void
unhook_pipeline (GstLgHdAudioDec * self)
{
  if (self->sink) {
    gst_object_unref (self->sink);
    self->sink = NULL;
  }
  if (self->top) {
    if (self->deep_added_id)
      g_signal_handler_disconnect (self->top, self->deep_added_id);
    self->deep_added_id = 0;
    gst_object_unref (self->top);
    self->top = NULL;
  }
}

/* ------------------------------------------------------------------------ */
/* decoding                                                                  */

static void
free_codec (GstLgHdAudioDec * self)
{
  if (self->parser) {
    av_parser_close (self->parser);
    self->parser = NULL;
  }
  if (self->pctx)
    avcodec_free_context (&self->pctx);
  if (self->ctx)
    avcodec_free_context (&self->ctx);
}

static gboolean
open_codec (GstLgHdAudioDec * self)
{
  const AVCodec *codec = avcodec_find_decoder (self->codec_id);
  if (!codec)
    return FALSE;
  self->ctx = avcodec_alloc_context3 (codec);
  if (!self->ctx)
    return FALSE;
  if (self->codec_id == AV_CODEC_ID_TRUEHD) {
    /* use the stream's own lossless 5.1 presentation when present */
    if (av_opt_set (self->ctx, "downmix", "5.1(side)", AV_OPT_SEARCH_CHILDREN) < 0)
      GST_WARNING_OBJECT (self, "could not request 5.1(side) presentation");
  }
  if (avcodec_open2 (self->ctx, codec, NULL) < 0) {
    avcodec_free_context (&self->ctx);
    return FALSE;
  }
  if (self->use_parser) {
    self->pctx = avcodec_alloc_context3 (NULL);
    self->parser = av_parser_init (self->codec_id);
  }
  return TRUE;
}

static gboolean
gst_lghd_set_format (GstAudioDecoder * dec, GstCaps * caps)
{
  GstLgHdAudioDec *self = GST_LGHD_AUDIO_DEC (dec);
  const GstStructure *st = gst_caps_get_structure (caps, 0);
  const gchar *name = gst_structure_get_name (st);
  const gchar *cid = gst_structure_get_string (st, "codec-id");
  enum AVCodecID id;

  /* LG's matroskademux emits DTS as audio/x-unknown, codec-id=A_DTS */
  if (g_str_equal (name, "audio/x-true-hd") ||
      (g_str_equal (name, "audio/x-unknown") && cid && g_str_equal (cid, "A_TRUEHD")))
    id = AV_CODEC_ID_TRUEHD;
  else if (g_str_equal (name, "audio/x-dts") ||
      (g_str_equal (name, "audio/x-unknown") && cid && g_str_equal (cid, "A_DTS")))
    id = AV_CODEC_ID_DTS;
  else
    return FALSE;

  GST_INFO_OBJECT (self, "input caps %" GST_PTR_FORMAT, caps);
  {
    gchar *cs = gst_caps_to_string (caps);
    lghd_log ("input caps %s", cs);
    g_free (cs);
  }

  if (self->ctx && self->codec_id == id)
    return TRUE;
  free_codec (self);
  self->codec_id = id;
  self->out_rate = 0;
  if (!open_codec (self)) {
    GST_ELEMENT_ERROR (self, LIBRARY, INIT, (NULL), ("failed to open decoder"));
    return FALSE;
  }
  hook_pipeline (self);
  return TRUE;
}

/* read sample i of channel c from an AVFrame as int32 */
static inline gint32
sample_at (const AVFrame * f, int c, int i, int nch)
{
  switch (f->format) {
    case AV_SAMPLE_FMT_S16:
      return ((gint32) ((const gint16 *) f->data[0])[i * nch + c]) << 16;
    case AV_SAMPLE_FMT_S16P:
      return ((gint32) ((const gint16 *) f->data[c])[i]) << 16;
    case AV_SAMPLE_FMT_S32:
      return ((const gint32 *) f->data[0])[i * nch + c];
    case AV_SAMPLE_FMT_S32P:
      return ((const gint32 *) f->data[c])[i];
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP:{
      float x = (f->format == AV_SAMPLE_FMT_FLT) ?
          ((const float *) f->data[0])[i * nch + c] : ((const float *) f->data[c])[i];
      double d = (double) x * 2147483647.0;
      if (d > 2147483647.0)
        d = 2147483647.0;
      if (d < -2147483648.0)
        d = -2147483648.0;
      return (gint32) d;
    }
    default:
      return 0;
  }
}

static inline gint32
clip32 (gint64 v)
{
  if (v > G_MAXINT32)
    return G_MAXINT32;
  if (v < G_MININT32)
    return G_MININT32;
  return (gint32) v;
}

/* Convert one decoded frame into 8ch interleaved S32LE in LG slot order */
static void
convert_frame (GstLgHdAudioDec * self, const AVFrame * f)
{
  const AVChannelLayout *L = &f->ch_layout;
  int nch = L->nb_channels;
  int fl = av_channel_layout_index_from_channel (L, AV_CHAN_FRONT_LEFT);
  int fr = av_channel_layout_index_from_channel (L, AV_CHAN_FRONT_RIGHT);
  int fc = av_channel_layout_index_from_channel (L, AV_CHAN_FRONT_CENTER);
  int lfe = av_channel_layout_index_from_channel (L, AV_CHAN_LOW_FREQUENCY);
  int bl = av_channel_layout_index_from_channel (L, AV_CHAN_BACK_LEFT);
  int br = av_channel_layout_index_from_channel (L, AV_CHAN_BACK_RIGHT);
  int sl = av_channel_layout_index_from_channel (L, AV_CHAN_SIDE_LEFT);
  int sr = av_channel_layout_index_from_channel (L, AV_CHAN_SIDE_RIGHT);
  int i, slot_c = self->swap_clfe ? 3 : 2, slot_lfe = self->swap_clfe ? 2 : 3;
  gint32 *dst;
  guint old = self->out->len;

  /* unknown/unspecified order: assume native WAV-style order */
  if (L->order != AV_CHANNEL_ORDER_NATIVE && L->order != AV_CHANNEL_ORDER_CUSTOM) {
    fl = nch > 0 ? 0 : -1; fr = nch > 1 ? 1 : -1;
    fc = nch > 2 ? 2 : -1; lfe = nch > 3 ? 3 : -1;
    bl = nch > 4 ? 4 : -1; br = nch > 5 ? 5 : -1; sl = sr = -1;
  }
  if (nch == 1 && fc < 0)
    fc = 0;                     /* mono */

  g_byte_array_set_size (self->out, old + f->nb_samples * OUT_CH * 4);
  dst = (gint32 *) (self->out->data + old);

  for (i = 0; i < f->nb_samples; i++, dst += OUT_CH) {
    gint32 vbl = 0, vbr = 0;
    memset (dst, 0, OUT_CH * 4);
    if (fl >= 0) dst[0] = sample_at (f, fl, i, nch);
    if (fr >= 0) dst[1] = sample_at (f, fr, i, nch);
    if (fc >= 0) dst[slot_c] = sample_at (f, fc, i, nch);
    if (lfe >= 0) dst[slot_lfe] = sample_at (f, lfe, i, nch);

    if (bl >= 0 && sl >= 0) {   /* 7.1 -> fold sides into rears at -3 dB */
      vbl = clip32 (((gint64) sample_at (f, bl, i, nch) + sample_at (f, sl, i, nch)) * 46341 / 65536);
      vbr = clip32 (((gint64) sample_at (f, br, i, nch) + sample_at (f, sr, i, nch)) * 46341 / 65536);
    } else {
      if (bl >= 0) vbl = sample_at (f, bl, i, nch); else if (sl >= 0) vbl = sample_at (f, sl, i, nch);
      if (br >= 0) vbr = sample_at (f, br, i, nch); else if (sr >= 0) vbr = sample_at (f, sr, i, nch);
    }
    dst[4] = vbl;
    dst[5] = vbr;
  }
}

static gboolean
ensure_output_format (GstLgHdAudioDec * self, int rate)
{
  GstAudioInfo info;
  GstAudioChannelPosition pos[OUT_CH] = {
    GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
    GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER, GST_AUDIO_CHANNEL_POSITION_LFE1,
    GST_AUDIO_CHANNEL_POSITION_REAR_LEFT, GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
    GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT, GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT
  };

  if (self->out_rate == rate)
    return TRUE;
  gst_audio_info_init (&info);
  gst_audio_info_set_format (&info, GST_AUDIO_FORMAT_S32LE, rate, OUT_CH, pos);
  if (!gst_audio_decoder_set_output_format (GST_AUDIO_DECODER (self), &info))
    return FALSE;
  self->out_rate = rate;
  GST_INFO_OBJECT (self, "output: S32LE %d Hz, 8ch (LG layout, swap_clfe=%d)", rate,
      self->swap_clfe);
  return TRUE;
}

/* receive all available frames into self->out; returns sample rate seen or 0 */
static int
drain_frames (GstLgHdAudioDec * self)
{
  int rate = 0, r;
  while ((r = avcodec_receive_frame (self->ctx, self->frame)) >= 0) {
    if (self->frame->sample_rate > 0) {
      if (rate && rate != self->frame->sample_rate) {
        /* rate change mid-buffer is not expected; keep first rate */
        GST_WARNING_OBJECT (self, "sample rate changed inside buffer");
      } else {
        rate = self->frame->sample_rate;
      }
      convert_frame (self, self->frame);
      self->n_out_frames++;
      self->n_out_samples += self->frame->nb_samples;
    }
    av_frame_unref (self->frame);
  }
  return rate;
}

static void
decode_packet (GstLgHdAudioDec * self, const guint8 * data, int size)
{
  int r;
  self->pkt->data = (guint8 *) data;
  self->pkt->size = size;
  r = avcodec_send_packet (self->ctx, size ? self->pkt : NULL);
  if (r < 0 && r != AVERROR (EAGAIN) && r != AVERROR_EOF)
    GST_DEBUG_OBJECT (self, "send_packet error %d (skipping)", r);
}

/* Hand the accumulated output to the base class. `force` pushes whatever is
 * there (drain); otherwise only once at least batch_ms of audio is buffered.
 * Input frames are finished together, so the base class keeps timestamps. */
static GstFlowReturn
push_output (GstLgHdAudioDec * self, int rate, gboolean force)
{
  GstBuffer *ob;
  guint frames;
  gsize want;

  if (self->pending == 0)
    return GST_FLOW_OK;
  if (self->out->len == 0 || rate <= 0) {
    if (!force && self->batch_ms > 0)
      return GST_FLOW_OK;       /* nothing decoded yet (decoder delay): keep waiting */
    frames = self->pending;
    self->pending = 0;
    return gst_audio_decoder_finish_frame (GST_AUDIO_DECODER (self), NULL, frames);
  }
  want = (gsize) rate * OUT_CH * 4 * self->batch_ms / 1000;
  if (!force && self->out->len < want)
    return GST_FLOW_OK;

  if (!ensure_output_format (self, rate)) {
    g_byte_array_set_size (self->out, 0);
    self->pending = 0;
    return GST_FLOW_NOT_NEGOTIATED;
  }
  ob = gst_buffer_new_allocate (NULL, self->out->len, NULL);
  gst_buffer_fill (ob, 0, self->out->data, self->out->len);
  g_byte_array_set_size (self->out, 0);
  frames = self->pending;
  self->pending = 0;

  if (!g_atomic_int_get (&self->playing)) {
    long onoff[1] = { 0 }, cur[5] = { 0 };
    if (ctl_read (EARC_ONOFF, onoff, 1) && ctl_read (EARC_CTL_NAME, cur, 5))
      lghd_log ("first output: %d Hz, %u frames/buffer; eARC onoff=%ld outputtype=%ld,%ld,%ld,%ld,%ld",
          rate, frames, onoff[0], cur[0], cur[1], cur[2], cur[3], cur[4]);
    g_atomic_int_set (&self->playing, 1);
    earc_start (self);
  }
  return gst_audio_decoder_finish_frame (GST_AUDIO_DECODER (self), ob, frames);
}

static GstFlowReturn
gst_lghd_handle_frame (GstAudioDecoder * dec, GstBuffer * buf)
{
  GstLgHdAudioDec *self = GST_LGHD_AUDIO_DEC (dec);
  GstMapInfo map;
  int rate = 0, r;

  if (!self->ctx)
    return GST_FLOW_NOT_NEGOTIATED;

  if (!buf) {                   /* drain */
    if (self->parser) {
      guint8 *pd = NULL;
      int ps = 0;
      av_parser_parse2 (self->parser, self->pctx, &pd, &ps, NULL, 0,
          AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
      if (ps > 0) {
        decode_packet (self, pd, ps);
        r = drain_frames (self);
        if (r) rate = r;
      }
    }
    decode_packet (self, NULL, 0);
    r = drain_frames (self);
    if (r) rate = r;
    avcodec_flush_buffers (self->ctx);
    return push_output (self, rate ? rate : self->out_rate, TRUE);
  }

  if (!gst_buffer_map (buf, &map, GST_MAP_READ))
    return GST_FLOW_ERROR;
  if (self->n_in++ == 0)
    GST_INFO_OBJECT (self, "first input pts %" GST_TIME_FORMAT ", %u bytes, parser=%d",
        GST_TIME_ARGS (GST_BUFFER_PTS (buf)), (guint) map.size, self->use_parser);

  if (self->parser) {
    const guint8 *in = map.data;
    int left = map.size;
    while (left > 0) {
      guint8 *pd = NULL;
      int ps = 0;
      int used = av_parser_parse2 (self->parser, self->pctx, &pd, &ps, in, left,
          AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
      if (used < 0)
        break;
      in += used;
      left -= used;
      if (ps > 0) {
        decode_packet (self, pd, ps);
        r = drain_frames (self);
        if (r) rate = r;
      }
      if (used == 0 && ps == 0)
        break;
    }
  } else {
    decode_packet (self, map.data, map.size);
    r = drain_frames (self);
    if (r) rate = r;
  }
  gst_buffer_unmap (buf, &map);

  self->pending++;
  return push_output (self, rate ? rate : self->out_rate, FALSE);
}

static void
gst_lghd_flush (GstAudioDecoder * dec, gboolean hard)
{
  GstLgHdAudioDec *self = GST_LGHD_AUDIO_DEC (dec);
  if (self->ctx)
    avcodec_flush_buffers (self->ctx);
  if (self->parser) {
    av_parser_close (self->parser);
    self->parser = av_parser_init (self->codec_id);
  }
  g_byte_array_set_size (self->out, 0);
  self->pending = 0;            /* base class discards its queued frames on flush */
  GST_INFO_OBJECT (self, "flush: in=%" G_GUINT64_FORMAT " out_frames=%" G_GUINT64_FORMAT
      " out_samples=%" G_GUINT64_FORMAT, self->n_in, self->n_out_frames, self->n_out_samples);
}

static gboolean
gst_lghd_start (GstAudioDecoder * dec)
{
  GstLgHdAudioDec *self = GST_LGHD_AUDIO_DEC (dec);
  self->pkt = av_packet_alloc ();
  self->frame = av_frame_alloc ();
  self->out = g_byte_array_new ();
  self->out_rate = 0;
  self->swap_clfe = env_on ("LGHD_SWAP_CLFE", TRUE);
  self->do_earc = !env_on ("LGHD_NO_EARC", FALSE);
  self->do_sinkprop = !env_on ("LGHD_NO_SINKPROP", FALSE);
  self->use_parser = env_on ("LGHD_PARSER", FALSE);
  self->report_ch = cfg_int ("LGHD_CH", 6);
  if (self->report_ch != 2 && self->report_ch != 6 && self->report_ch != 8)
    self->report_ch = 6;
  self->batch_ms = (guint) cfg_int ("LGHD_BATCH_MS", 32);
  if (self->batch_ms > 100)
    self->batch_ms = 100;
  self->pending = 0;
  self->pipeline_id[0] = self->resource[0] = 0;
  if (self->batch_ms && env_on ("LGHD_SET_LATENCY", TRUE))
    gst_audio_decoder_set_latency (dec, self->batch_ms * GST_MSECOND, self->batch_ms * GST_MSECOND);
  lghd_log ("start v%s: earc=%d sinkprop=%d ch=%d batch=%ums latency=%d swap_clfe=%d", LGHD_VERSION,
      self->do_earc, self->do_sinkprop, self->report_ch, self->batch_ms,
      env_on ("LGHD_SET_LATENCY", TRUE), self->swap_clfe);
  self->n_in = self->n_out_frames = self->n_out_samples = 0;
  g_atomic_int_set (&self->playing, 0);
  gst_audio_decoder_set_drainable (dec, TRUE);
  gst_audio_decoder_set_needs_format (dec, TRUE);
  return self->pkt && self->frame;
}

static gboolean
gst_lghd_stop (GstAudioDecoder * dec)
{
  GstLgHdAudioDec *self = GST_LGHD_AUDIO_DEC (dec);
  GST_INFO_OBJECT (self, "stop: in=%" G_GUINT64_FORMAT " out_frames=%" G_GUINT64_FORMAT
      " out_samples=%" G_GUINT64_FORMAT " (%.1fs @48k)", self->n_in, self->n_out_frames,
      self->n_out_samples, self->n_out_samples / 48000.0);
  lghd_log ("stop: in=%" G_GUINT64_FORMAT " out_samples=%" G_GUINT64_FORMAT " (%.1fs @48k)",
      self->n_in, self->n_out_samples, self->n_out_samples / 48000.0);
  earc_stop (self);
  unhook_pipeline (self);
  free_codec (self);
  if (self->pkt)
    av_packet_free (&self->pkt);
  if (self->frame)
    av_frame_free (&self->frame);
  if (self->out) {
    g_byte_array_unref (self->out);
    self->out = NULL;
  }
  g_atomic_int_set (&self->playing, 0);
  return TRUE;
}

static GstStateChangeReturn
gst_lghd_change_state (GstElement * element, GstStateChange transition)
{
  GstLgHdAudioDec *self = GST_LGHD_AUDIO_DEC (element);
  GstStateChangeReturn ret;
  lghd_log ("state change %s -> %s",
      gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT (transition)),
      gst_element_state_get_name (GST_STATE_TRANSITION_NEXT (transition)));
  /* hook as early as possible so the sink gets pcm-multi-ch before it opens */
  if (transition == GST_STATE_CHANGE_NULL_TO_READY ||
      transition == GST_STATE_CHANGE_READY_TO_PAUSED) {
    self->do_sinkprop = !env_on ("LGHD_NO_SINKPROP", FALSE);
    hook_pipeline (self);
  }
  ret = GST_ELEMENT_CLASS (gst_lghd_audio_dec_parent_class)->change_state (element,
      transition);
  lghd_log ("state change %s -> %s returned %s",
      gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT (transition)),
      gst_element_state_get_name (GST_STATE_TRANSITION_NEXT (transition)),
      gst_element_state_change_return_get_name (ret));
  return ret;
}

static void
gst_lghd_audio_dec_class_init (GstLgHdAudioDecClass * klass)
{
  GstElementClass *ec = GST_ELEMENT_CLASS (klass);
  GstAudioDecoderClass *ac = GST_AUDIO_DECODER_CLASS (klass);

  gst_element_class_add_static_pad_template (ec, &sink_tmpl);
  gst_element_class_add_static_pad_template (ec, &src_tmpl);
  gst_element_class_set_static_metadata (ec, "LG HD audio decoder (TrueHD/DTS)",
      "Codec/Decoder/Audio",
      "Decodes TrueHD/DTS to multichannel PCM for LG eARC output",
      "homebrew");

  ec->change_state = GST_DEBUG_FUNCPTR (gst_lghd_change_state);
  ac->start = GST_DEBUG_FUNCPTR (gst_lghd_start);
  ac->stop = GST_DEBUG_FUNCPTR (gst_lghd_stop);
  ac->set_format = GST_DEBUG_FUNCPTR (gst_lghd_set_format);
  ac->handle_frame = GST_DEBUG_FUNCPTR (gst_lghd_handle_frame);
  ac->flush = GST_DEBUG_FUNCPTR (gst_lghd_flush);
}

static void
gst_lghd_audio_dec_init (GstLgHdAudioDec * self)
{
  lghd_log ("instance created");
  gst_audio_decoder_set_use_default_pad_acceptcaps (GST_AUDIO_DECODER_CAST (self), TRUE);
  GST_PAD_SET_ACCEPT_TEMPLATE (GST_AUDIO_DECODER_SINK_PAD (self));
}


/* ======================================================================== */
/* lghddvtag: mark Dolby Vision HEVC from MKV so LG's dvbin takes its DV path */
/*
 * LG's matroskademux gives DV profile 8 video the same caps as plain HEVC
 * (format=HEVC, no DV fields), so dvbin (libgstlxvideodec) decodes only the
 * HDR10 base layer. dvbin/lxvideodec read caps fields "dolby-vision" (boolean)
 * and "dolby-vision-profile" (int); qtdemux marks DV MP4 with format=dvh1.
 * This element sits where h265parse would (input is already hvc1/au from the
 * demuxer, so no parsing is needed), looks at the first access unit for DV
 * RPU NAL units (type 62; EL = type 63) and, if present, adds those fields.
 * Non-DV streams pass through with caps unchanged (plus parsed=true).
 * Settings: LGHD_DVTAG=0 disables tagging; LGHD_DV_PROFILE=n forces the profile.
 */
GST_DEBUG_CATEGORY_STATIC (dvtag_debug);

typedef struct _GstLgHdDvTag {
  GstElement parent;
  GstPad *sinkpad, *srcpad;
  GstCaps *in_caps;           /* held until the first buffer */
  GList *pending;             /* sticky events held behind the caps */
  gboolean decided, is_dv;
  gint dv_profile;
  guint nal_len_size;
} GstLgHdDvTag;
typedef struct _GstLgHdDvTagClass { GstElementClass parent_class; } GstLgHdDvTagClass;

GType gst_lghd_dvtag_get_type (void);
G_DEFINE_TYPE (GstLgHdDvTag, gst_lghd_dvtag, GST_TYPE_ELEMENT);
#define DVTAG(o) ((GstLgHdDvTag *)(o))

static GstStaticPadTemplate dvtag_sink_tmpl = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h265, stream-format=(string)hvc1, alignment=(string)au"));
static GstStaticPadTemplate dvtag_src_tmpl = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h265, stream-format=(string)hvc1, alignment=(string)au, parsed=(boolean)true"));

static GstCaps *
dvtag_out_caps (GstLgHdDvTag * self, GstCaps * in)
{
  GstCaps *c = gst_caps_copy (in);
  guint i;
  for (i = 0; i < gst_caps_get_size (c); i++) {
    GstStructure *st = gst_caps_get_structure (c, i);
    gst_structure_set (st, "parsed", G_TYPE_BOOLEAN, TRUE, NULL);
    if (self->is_dv)
      gst_structure_set (st, "dolby-vision", G_TYPE_BOOLEAN, TRUE,
          "dolby-vision-profile", G_TYPE_INT, self->dv_profile,
          "format", G_TYPE_STRING, "dvh1", NULL);
  }
  return c;
}

static void
dvtag_read_nal_len (GstLgHdDvTag * self, GstCaps * caps)
{
  const GValue *v;
  GstMapInfo m;
  self->nal_len_size = 4;
  v = gst_structure_get_value (gst_caps_get_structure (caps, 0), "codec_data");
  if (v && GST_VALUE_HOLDS_BUFFER (v)) {
    GstBuffer *b = gst_value_get_buffer (v);
    if (gst_buffer_map (b, &m, GST_MAP_READ)) {
      if (m.size > 21)
        self->nal_len_size = (m.data[21] & 3) + 1;
      gst_buffer_unmap (b, &m);
    }
  }
}

/* scan one hvc1 access unit for DV RPU (62) / EL (63) NAL units */
static void
dvtag_scan (GstLgHdDvTag * self, GstBuffer * buf, gboolean * rpu, gboolean * el)
{
  GstMapInfo m;
  gsize off = 0;
  guint n = self->nal_len_size;
  *rpu = *el = FALSE;
  if (!gst_buffer_map (buf, &m, GST_MAP_READ))
    return;
  while (off + n + 2 <= m.size) {
    guint32 len = 0;
    guint i, type;
    for (i = 0; i < n; i++)
      len = (len << 8) | m.data[off + i];
    off += n;
    if (len == 0 || off + len > m.size)
      break;
    type = (m.data[off] >> 1) & 0x3f;
    if (type == 62) *rpu = TRUE;
    if (type == 63) *el = TRUE;
    off += len;
  }
  gst_buffer_unmap (buf, &m);
}

static gboolean
dvtag_push_held (GstLgHdDvTag * self)
{
  GList *l;
  gboolean ok = TRUE;
  if (self->in_caps) {
    GstCaps *out = dvtag_out_caps (self, self->in_caps);
    gchar *cs = gst_caps_to_string (out);
    lghd_log ("dvtag: caps out %s", cs);
    g_free (cs);
    ok = gst_pad_push_event (self->srcpad, gst_event_new_caps (out));
    gst_caps_unref (out);
  }
  for (l = self->pending; l; l = l->next)
    gst_pad_push_event (self->srcpad, GST_EVENT (l->data));
  g_list_free (self->pending);
  self->pending = NULL;
  return ok;
}

static GstFlowReturn
dvtag_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstLgHdDvTag *self = DVTAG (parent);
  if (!self->decided) {
    gboolean rpu = FALSE, el = FALSE;
    dvtag_scan (self, buf, &rpu, &el);
    self->is_dv = rpu && env_on ("LGHD_DVTAG", TRUE);
    self->dv_profile = cfg_int ("LGHD_DV_PROFILE", el ? 7 : 8);
    self->decided = TRUE;
    lghd_log ("dvtag: first AU: rpu=%d el=%d -> %s (profile %d)", rpu, el,
        self->is_dv ? "DOLBY VISION" : "not DV, unchanged", self->dv_profile);
    if (!dvtag_push_held (self)) {
      gst_buffer_unref (buf);
      return GST_FLOW_NOT_NEGOTIATED;
    }
  }
  return gst_pad_push (self->srcpad, buf);
}

static gboolean
dvtag_sink_event (GstPad * pad, GstObject * parent, GstEvent * ev)
{
  GstLgHdDvTag *self = DVTAG (parent);
  switch (GST_EVENT_TYPE (ev)) {
    case GST_EVENT_CAPS:{
      GstCaps *c;
      gst_event_parse_caps (ev, &c);
      dvtag_read_nal_len (self, c);
      if (!self->decided) {
        gst_caps_replace (&self->in_caps, c);
        gst_event_unref (ev);
        return TRUE;
      } else {
        GstCaps *out = dvtag_out_caps (self, c);
        gboolean r = gst_pad_push_event (self->srcpad, gst_event_new_caps (out));
        gst_caps_unref (out);
        gst_caps_replace (&self->in_caps, c);
        gst_event_unref (ev);
        return r;
      }
    }
    case GST_EVENT_EOS:
    case GST_EVENT_GAP:
      if (!self->decided) {
        self->decided = TRUE;
        dvtag_push_held (self);
      }
      return gst_pad_event_default (pad, parent, ev);
    case GST_EVENT_FLUSH_START:
    case GST_EVENT_FLUSH_STOP:
    case GST_EVENT_STREAM_START:
      return gst_pad_event_default (pad, parent, ev);
    default:
      if (!self->decided && GST_EVENT_IS_SERIALIZED (ev) && self->in_caps) {
        self->pending = g_list_append (self->pending, ev);   /* keep order behind caps */
        return TRUE;
      }
      return gst_pad_event_default (pad, parent, ev);
  }
}

static gboolean
dvtag_sink_query (GstPad * pad, GstObject * parent, GstQuery * q)
{
  GstLgHdDvTag *self = DVTAG (parent);
  switch (GST_QUERY_TYPE (q)) {
    case GST_QUERY_CAPS:{
      GstCaps *filter, *tmpl = gst_pad_get_pad_template_caps (pad), *res;
      gst_query_parse_caps (q, &filter);
      res = filter ? gst_caps_intersect_full (filter, tmpl, GST_CAPS_INTERSECT_FIRST) : gst_caps_ref (tmpl);
      gst_query_set_caps_result (q, res);
      gst_caps_unref (res);
      gst_caps_unref (tmpl);
      return TRUE;
    }
    case GST_QUERY_ACCEPT_CAPS:{
      GstCaps *c, *tmpl = gst_pad_get_pad_template_caps (pad);
      gst_query_parse_accept_caps (q, &c);
      gst_query_set_accept_caps_result (q, gst_caps_can_intersect (c, tmpl));
      gst_caps_unref (tmpl);
      return TRUE;
    }
    default:
      return gst_pad_query_default (pad, parent, q);
  }
  (void) self;
}

static GstStateChangeReturn
dvtag_change_state (GstElement * e, GstStateChange t)
{
  GstLgHdDvTag *self = DVTAG (e);
  GstStateChangeReturn r = GST_ELEMENT_CLASS (gst_lghd_dvtag_parent_class)->change_state (e, t);
  if (t == GST_STATE_CHANGE_PAUSED_TO_READY) {
    self->decided = FALSE;
    self->is_dv = FALSE;
    gst_caps_replace (&self->in_caps, NULL);
    g_list_free_full (self->pending, (GDestroyNotify) gst_event_unref);
    self->pending = NULL;
  }
  return r;
}

static void
dvtag_finalize (GObject * o)
{
  GstLgHdDvTag *self = DVTAG (o);
  gst_caps_replace (&self->in_caps, NULL);
  g_list_free_full (self->pending, (GDestroyNotify) gst_event_unref);
  G_OBJECT_CLASS (gst_lghd_dvtag_parent_class)->finalize (o);
}

static void
gst_lghd_dvtag_class_init (GstLgHdDvTagClass * k)
{
  GstElementClass *ec = GST_ELEMENT_CLASS (k);
  G_OBJECT_CLASS (k)->finalize = dvtag_finalize;
  ec->change_state = dvtag_change_state;
  gst_element_class_add_static_pad_template (ec, &dvtag_sink_tmpl);
  gst_element_class_add_static_pad_template (ec, &dvtag_src_tmpl);
  gst_element_class_set_static_metadata (ec, "LG HD Dolby Vision tagger",
      "Codec/Parser/Converter/Video",
      "Marks Dolby Vision HEVC (from MKV) for LG's dvbin", "homebrew");
}

static void
gst_lghd_dvtag_init (GstLgHdDvTag * self)
{
  self->sinkpad = gst_pad_new_from_static_template (&dvtag_sink_tmpl, "sink");
  gst_pad_set_chain_function (self->sinkpad, dvtag_chain);
  gst_pad_set_event_function (self->sinkpad, dvtag_sink_event);
  gst_pad_set_query_function (self->sinkpad, dvtag_sink_query);
  GST_PAD_SET_PROXY_ALLOCATION (self->sinkpad);
  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);
  self->srcpad = gst_pad_new_from_static_template (&dvtag_src_tmpl, "src");
  gst_pad_use_fixed_caps (self->srcpad);
  gst_element_add_pad (GST_ELEMENT (self), self->srcpad);
  self->nal_len_size = 4;
  self->dv_profile = 8;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  gchar *id = NULL;
  gchar pid[64] = "";
  GST_DEBUG_CATEGORY_INIT (lghd_debug, "lghdaudiodec", 0, "LG HD audio decoder");
  if (find_pipeline_id (pid, sizeof pid))
    id = pid;
  lghd_log ("plugin_init v%s (pipeline %s)", LGHD_VERSION, id ? id : "none");
  maybe_capture_gst_debug ();
  GST_DEBUG_CATEGORY_INIT (dvtag_debug, "lghddvtag", 0, "LG HD DV tagger");
  if (env_on ("LGHD_DVTAG_REGISTER", TRUE) &&
      !gst_element_register (plugin, "lghddvtag", GST_RANK_PRIMARY + 10,
          gst_lghd_dvtag_get_type ()))
    return FALSE;
  return gst_element_register (plugin, "lghdaudiodec", GST_RANK_PRIMARY + 10,
      GST_TYPE_LGHD_AUDIO_DEC);
}

#define PACKAGE "lghdaudio"
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, 18, lghdaudio,
    "TrueHD/DTS to multichannel PCM for LG webOS", plugin_init, LGHD_VERSION,
    "LGPL", "lghdaudio", "homebrew")
