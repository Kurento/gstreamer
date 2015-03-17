/* GStreamer
 * Copyright (C) 2009 Wim Taymans <wim.taymans@gmail.com>
 * Copyright (C) 2015 Sebastian Dröge <sebastian@centricular.com>
 *
 * gsttaskpool.c: Pool for streaming threads
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

/**
 * SECTION:gsttaskpool
 * @short_description: Pool of GStreamer streaming threads
 * @see_also: #GstTask, #GstPad
 *
 * This object provides an abstraction for creating threads. The default
 * implementation uses a regular GThreadPool to start tasks.
 *
 * Subclasses can be made to create custom threads.
 */

#include "gst_private.h"

#include "gstinfo.h"
#include "gsttaskpool.h"

GST_DEBUG_CATEGORY_STATIC (taskpool_debug);
#define GST_CAT_DEFAULT (taskpool_debug)

struct _GstTaskPoolPrivate
{
  gint max_threads;
  gboolean exclusive;

  gint need_schedule_thread;
  GMainContext *schedule_context;
  GMainLoop *schedule_loop;
  GThread *schedule_thread;
  GMutex schedule_lock;
  GCond schedule_cond;
};

static void gst_task_pool_finalize (GObject * object);

#define _do_init \
{ \
  GST_DEBUG_CATEGORY_INIT (taskpool_debug, "taskpool", 0, "Thread pool"); \
}

#define GST_TASK_POOL_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_TASK_POOL, GstTaskPoolPrivate))

G_DEFINE_TYPE_WITH_CODE (GstTaskPool, gst_task_pool, GST_TYPE_OBJECT, _do_init);

typedef struct
{
  GstTaskPoolFunction func;
  gpointer user_data;
} TaskData;

static void
default_func (TaskData * tdata, GstTaskPool * pool)
{
  GstTaskPoolFunction func;
  gpointer user_data;

  func = tdata->func;
  user_data = tdata->user_data;
  g_slice_free (TaskData, tdata);

  func (user_data);
}

static void
default_prepare (GstTaskPool * pool, GError ** error)
{
  GST_OBJECT_LOCK (pool);
  pool->pool =
      g_thread_pool_new ((GFunc) default_func, pool, pool->priv->max_threads,
      pool->priv->exclusive, NULL);
  GST_OBJECT_UNLOCK (pool);
}

static void
default_cleanup (GstTaskPool * pool)
{
  GST_OBJECT_LOCK (pool);
  if (pool->pool) {
    /* Shut down all the threads, we still process the ones scheduled
     * because the unref happens in the thread function.
     * Also wait for currently running ones to finish. */
    g_thread_pool_free (pool->pool, FALSE, TRUE);
    pool->pool = NULL;
  }
  GST_OBJECT_UNLOCK (pool);
}

static gpointer
default_push (GstTaskPool * pool, GstTaskPoolFunction func,
    gpointer user_data, GError ** error)
{
  TaskData *tdata;

  tdata = g_slice_new (TaskData);
  tdata->func = func;
  tdata->user_data = user_data;

  GST_OBJECT_LOCK (pool);
  if (pool->pool)
    g_thread_pool_push (pool->pool, tdata, error);
  else {
    g_slice_free (TaskData, tdata);
  }
  GST_OBJECT_UNLOCK (pool);

  return NULL;
}

static void
default_join (GstTaskPool * pool, gpointer id)
{
  /* we do nothing here, we can't join from the pools */
}

static void
gst_task_pool_class_init (GstTaskPoolClass * klass)
{
  GObjectClass *gobject_class;
  GstTaskPoolClass *gsttaskpool_class;

  gobject_class = (GObjectClass *) klass;
  gsttaskpool_class = (GstTaskPoolClass *) klass;

  g_type_class_add_private (gobject_class, sizeof (GstTaskPoolPrivate));

  gobject_class->finalize = gst_task_pool_finalize;

  gsttaskpool_class->prepare = default_prepare;
  gsttaskpool_class->cleanup = default_cleanup;
  gsttaskpool_class->push = default_push;
  gsttaskpool_class->join = default_join;
}

static void
gst_task_pool_init (GstTaskPool * pool)
{
  pool->priv = GST_TASK_POOL_GET_PRIVATE (pool);

  pool->priv->max_threads = -1;
  pool->priv->exclusive = FALSE;

  pool->priv->need_schedule_thread = 0;
  pool->priv->schedule_context = NULL;
  pool->priv->schedule_loop = NULL;
  pool->priv->schedule_thread = NULL;
  g_mutex_init (&pool->priv->schedule_lock);
  g_cond_init (&pool->priv->schedule_cond);

  /* clear floating flag */
  gst_object_ref_sink (pool);
}

static void
gst_task_pool_finalize (GObject * object)
{
  GstTaskPool *pool = GST_TASK_POOL (object);

  GST_DEBUG_OBJECT (pool, "taskpool finalize");

  g_mutex_clear (&pool->priv->schedule_lock);
  g_cond_clear (&pool->priv->schedule_cond);

  G_OBJECT_CLASS (gst_task_pool_parent_class)->finalize (object);
}

/**
 * gst_task_pool_new:
 *
 * Create a new default task pool. The default task pool will use a regular
 * GThreadPool for threads.
 *
 * Returns: (transfer full): a new #GstTaskPool. gst_object_unref() after usage.
 */
GstTaskPool *
gst_task_pool_new (void)
{
  GstTaskPool *pool;

  pool = g_object_newv (GST_TYPE_TASK_POOL, 0, NULL);

  return pool;
}

GstTaskPool *
gst_task_pool_new_full (gint max_threads, gboolean exclusive)
{
  GstTaskPool *pool;

  pool = g_object_newv (GST_TYPE_TASK_POOL, 0, NULL);

  pool->priv->max_threads = max_threads;
  pool->priv->exclusive = exclusive;

  return pool;
}

/**
 * gst_task_pool_prepare:
 * @pool: a #GstTaskPool
 * @error: an error return location
 *
 * Prepare the taskpool for accepting gst_task_pool_push() operations.
 *
 * MT safe.
 */
void
gst_task_pool_prepare (GstTaskPool * pool, GError ** error)
{
  GstTaskPoolClass *klass;

  g_return_if_fail (GST_IS_TASK_POOL (pool));

  klass = GST_TASK_POOL_GET_CLASS (pool);

  if (klass->prepare)
    klass->prepare (pool, error);
}

/**
 * gst_task_pool_cleanup:
 * @pool: a #GstTaskPool
 *
 * Wait for all tasks to be stopped. This is mainly used internally
 * to ensure proper cleanup of internal data structures in test suites.
 *
 * MT safe.
 */
void
gst_task_pool_cleanup (GstTaskPool * pool)
{
  GstTaskPoolClass *klass;

  g_return_if_fail (GST_IS_TASK_POOL (pool));

  klass = GST_TASK_POOL_GET_CLASS (pool);

  if (klass->cleanup)
    klass->cleanup (pool);
}

/**
 * gst_task_pool_push:
 * @pool: a #GstTaskPool
 * @func: (scope async): the function to call
 * @user_data: (closure): data to pass to @func
 * @error: return location for an error
 *
 * Start the execution of a new thread from @pool.
 *
 * Returns: (transfer none) (nullable): a pointer that should be used
 * for the gst_task_pool_join function. This pointer can be %NULL, you
 * must check @error to detect errors.
 */
gpointer
gst_task_pool_push (GstTaskPool * pool, GstTaskPoolFunction func,
    gpointer user_data, GError ** error)
{
  GstTaskPoolClass *klass;

  g_return_val_if_fail (GST_IS_TASK_POOL (pool), NULL);

  klass = GST_TASK_POOL_GET_CLASS (pool);

  if (klass->push == NULL)
    goto not_supported;

  return klass->push (pool, func, user_data, error);

  /* ERRORS */
not_supported:
  {
    g_warning ("pushing tasks on pool %p is not supported", pool);
    return NULL;
  }
}

/**
 * gst_task_pool_join:
 * @pool: a #GstTaskPool
 * @id: the id
 *
 * Join a task and/or return it to the pool. @id is the id obtained from 
 * gst_task_pool_push().
 */
void
gst_task_pool_join (GstTaskPool * pool, gpointer id)
{
  GstTaskPoolClass *klass;

  g_return_if_fail (GST_IS_TASK_POOL (pool));

  klass = GST_TASK_POOL_GET_CLASS (pool);

  if (klass->join)
    klass->join (pool, id);
}

static gboolean
main_loop_running_cb (gpointer user_data)
{
  GstTaskPool *pool = GST_TASK_POOL (user_data);

  g_mutex_lock (&pool->priv->schedule_lock);
  g_cond_signal (&pool->priv->schedule_cond);
  g_mutex_unlock (&pool->priv->schedule_lock);

  return G_SOURCE_REMOVE;
}

static gpointer
gst_task_pool_schedule_func (gpointer data)
{
  GstTaskPool *pool = GST_TASK_POOL (data);
  GSource *source;

  source = g_idle_source_new ();
  g_source_set_callback (source, (GSourceFunc) main_loop_running_cb, pool,
      NULL);
  g_source_attach (source, pool->priv->schedule_context);
  g_source_unref (source);

  g_main_loop_run (pool->priv->schedule_loop);

  return NULL;
}

gboolean
gst_task_pool_need_schedule_thread (GstTaskPool * pool, gboolean needed)
{
  gboolean ret;

  g_return_val_if_fail (GST_IS_TASK_POOL (pool), FALSE);
  g_return_val_if_fail (needed || pool->priv->need_schedule_thread > 0, FALSE);

  /* We don't allow this for the default pool */
  if (pool == gst_task_pool_get_default ()) {
    gst_object_unref (pool);
    return FALSE;
  }

  g_mutex_lock (&pool->priv->schedule_lock);
  if (needed) {
    ret = TRUE;
    if (pool->priv->need_schedule_thread == 0) {
      pool->priv->schedule_context = g_main_context_new ();
      pool->priv->schedule_loop =
          g_main_loop_new (pool->priv->schedule_context, FALSE);

      pool->priv->schedule_thread =
          g_thread_new (GST_OBJECT_NAME (pool), gst_task_pool_schedule_func,
          pool);

      g_cond_wait (&pool->priv->schedule_cond, &pool->priv->schedule_lock);
    }
    pool->priv->need_schedule_thread++;
  } else {
    ret = FALSE;
    pool->priv->need_schedule_thread--;
    if (pool->priv->need_schedule_thread == 0) {
      g_main_loop_quit (pool->priv->schedule_loop);
      g_thread_join (pool->priv->schedule_thread);
      g_main_loop_unref (pool->priv->schedule_loop);
      pool->priv->schedule_loop = NULL;
      g_main_context_unref (pool->priv->schedule_context);
      pool->priv->schedule_context = NULL;
      pool->priv->schedule_thread = NULL;
    }
  }
  g_mutex_unlock (&pool->priv->schedule_lock);

  return ret;
}

GMainContext *
gst_task_pool_get_schedule_context (GstTaskPool * pool)
{
  GMainContext *context;

  g_return_val_if_fail (GST_IS_TASK_POOL (pool), NULL);
  g_return_val_if_fail (pool->priv->need_schedule_thread > 0, NULL);

  g_mutex_lock (&pool->priv->schedule_lock);
  context = pool->priv->schedule_context;
  if (context)
    g_main_context_ref (context);
  g_mutex_unlock (&pool->priv->schedule_lock);

  return context;
}

GstTaskPool *
gst_task_pool_get_default (void)
{
  static GstTaskPool *pool = NULL;

  if (g_once_init_enter (&pool)) {
    GstTaskPool *_pool = gst_task_pool_new ();

    gst_task_pool_prepare (_pool, NULL);
    g_once_init_leave (&pool, _pool);
  }

  return gst_object_ref (pool);
}
