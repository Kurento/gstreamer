/*
 * Copyright (c) 2015 Sebastian Dröge <sebastian@centricular.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <gst/gst.h>

static GMainLoop *loop;
static GstElement *pipeline;
#define N_SINKS 10
static GstElement *src, *tee, *queue[N_SINKS], *sink[N_SINKS];

static GstTaskPool *pool;

static gboolean
message_cb (GstBus * bus, GstMessage * message, gpointer user_data)
{
  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:{
      GError *err = NULL;
      gchar *name, *debug = NULL;

      name = gst_object_get_path_string (message->src);
      gst_message_parse_error (message, &err, &debug);

      g_printerr ("ERROR: from element %s: %s\n", name, err->message);
      if (debug != NULL)
        g_printerr ("Additional debug info:\n%s\n", debug);

      g_error_free (err);
      g_free (debug);
      g_free (name);

      g_main_loop_quit (loop);
      break;
    }
    case GST_MESSAGE_WARNING:{
      GError *err = NULL;
      gchar *name, *debug = NULL;

      name = gst_object_get_path_string (message->src);
      gst_message_parse_warning (message, &err, &debug);

      g_printerr ("ERROR: from element %s: %s\n", name, err->message);
      if (debug != NULL)
        g_printerr ("Additional debug info:\n%s\n", debug);

      g_error_free (err);
      g_free (debug);
      g_free (name);
      break;
    }
    case GST_MESSAGE_EOS:
      g_print ("Got EOS\n");
      g_main_loop_quit (loop);
      break;
    default:
      break;
  }

  return TRUE;
}

static void
stream_status_cb (GstBus * bus, GstMessage * msg, gpointer user_data)
{
  GstStreamStatusType type;

  gst_message_parse_stream_status (msg, &type, NULL);
  if (type == GST_STREAM_STATUS_TYPE_CREATE) {
    GstTask *task = GST_PAD_TASK (GST_MESSAGE_SRC (msg));
    gboolean ret;

    gst_task_set_pool (task, pool);
    ret = gst_task_set_scheduleable (task, TRUE);
    g_assert (ret);
  }
}

static void
handoff_cb (GstElement * sink, GstBuffer * buf, GstPad * pad,
    gpointer user_data)
{
  g_print ("%s: handoff thread %p timestamp %" GST_TIME_FORMAT "\n",
      GST_OBJECT_NAME (sink), g_thread_self (),
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buf)));
}

int
main (int argc, char **argv)
{
  GstBus *bus;
  gint i;

  gst_init (&argc, &argv);

  pool = gst_task_pool_new_full (2, FALSE);
  gst_task_pool_prepare (pool, NULL);

  pipeline = gst_pipeline_new (NULL);
  src = gst_element_factory_make ("fakesrc", NULL);
  g_object_set (src, "is-live", TRUE, "format", GST_FORMAT_TIME, "num-buffers",
      1000, NULL);
  tee = gst_element_factory_make ("tee", NULL);

  gst_bin_add_many (GST_BIN (pipeline), src, tee, NULL);
  gst_element_link_pads (src, "src", tee, "sink");

  for (i = 0; i < N_SINKS; i++) {
    GstPad *srcpad, *sinkpad;

    queue[i] = gst_element_factory_make ("queue", NULL);
    sink[i] = gst_element_factory_make ("fakesink", NULL);

    gst_bin_add_many (GST_BIN (pipeline), queue[i], sink[i], NULL);

    g_object_set (sink[i], "async", FALSE, "signal-handoffs", TRUE, NULL);
    g_signal_connect (sink[i], "handoff", G_CALLBACK (handoff_cb), NULL);

    srcpad = gst_element_get_request_pad (tee, "src_%u");
    sinkpad = gst_element_get_static_pad (queue[i], "sink");
    gst_pad_link (srcpad, sinkpad);
    gst_object_unref (srcpad);
    gst_object_unref (sinkpad);

    gst_element_link_pads (queue[i], "src", sink[i], "sink");
  }

  if (!pipeline || !src || !tee) {
    g_error ("Failed to create elements");
    return -1;
  }

  loop = g_main_loop_new (NULL, FALSE);

  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  gst_bus_add_signal_watch (bus);
  g_signal_connect (G_OBJECT (bus), "message", G_CALLBACK (message_cb), NULL);
  gst_bus_enable_sync_message_emission (bus);
  g_signal_connect (G_OBJECT (bus), "sync-message::stream-status",
      G_CALLBACK (stream_status_cb), NULL);
  gst_object_unref (GST_OBJECT (bus));

  if (gst_element_set_state (pipeline,
          GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    g_error ("Failed to go into PLAYING state");
    return -3;
  }

  g_main_loop_run (loop);

  gst_element_set_state (pipeline, GST_STATE_NULL);

  g_main_loop_unref (loop);
  gst_object_unref (pipeline);

  gst_task_pool_cleanup (pool);
  gst_object_unref (pool);

  return 0;
}
