/*
 * GStreamer Funnel element
 *
 * Copyright 2007 Collabora Ltd.
 *  @author: Olivier Crete <olivier.crete@collabora.co.uk>
 * Copyright 2007 Nokia Corp.
 * Copyright 2016 Kurento (http://kurento.org/)
 *  @author: Miguel París <mparisdiaz@gmail.com>
 *
 * gstfunnel.h: Simple Funnel element
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */


#ifndef __GST_FUNNEL_H__
#define __GST_FUNNEL_H__

#include <gst/gst.h>

G_BEGIN_DECLS

/**
 * GstFunnelForwardStickyEventsMode:
 *
 * GST_FUNNEL_FORWARD_STICKY_EVENTS_MODE_NEVER: never forward sticky events (on stream changes)
 *    from sinkpads to srcpad. Only the events from the first sinkpad are propagated downstream.
 * GST_FUNNEL_FORWARD_STICKY_EVENTS_MODE_ONCE: only forward once the same sticky event (on stream changes)
 *    from sinkpads to srcpad.
 * GST_FUNNEL_FORWARD_STICKY_EVENTS_MODE_ALWAYS: always forward sticky events (on stream changes)
 *    from sinkpads to srcpad.
 * RTP_JITTER_BUFFER_MODE_LAST: last mode.
 *
 * The different behaviours for forwarding sticky events.
 */
typedef enum {
  GST_FUNNEL_FORWARD_STICKY_EVENTS_MODE_NEVER,
  GST_FUNNEL_FORWARD_STICKY_EVENTS_MODE_ONCE,
  GST_FUNNEL_FORWARD_STICKY_EVENTS_MODE_ALWAYS,
  GST_FUNNEL_FORWARD_STICKY_EVENTS_MODE_LAST
} GstFunnelForwardStickyEventsMode;

#define GST_TYPE_FUNNEL_FORWARD_STICKY_EVENTS_MODE (gst_funnel_forward_sticky_events_mode_get_type())
GType gst_funnel_forward_sticky_events_mode_get_type (void);

#define GST_TYPE_FUNNEL \
  (gst_funnel_get_type ())
#define GST_FUNNEL(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_FUNNEL,GstFunnel))
#define GST_FUNNEL_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_FUNNEL,GstFunnelClass))
#define GST_IS_FUNNEL(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_FUNNEL))
#define GST_IS_FUNNEL_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_FUNNEL))

typedef struct _GstFunnel          GstFunnel;
typedef struct _GstFunnelClass     GstFunnelClass;

/**
 * GstFunnel:
 *
 * Opaque #GstFunnel data structure.
 */
struct _GstFunnel {
  GstElement      element;

  /*< private >*/
  GstPad         *srcpad;

  GstPad *last_sinkpad;
  GstFunnelForwardStickyEventsMode forward_sticky_events_mode;
};

struct _GstFunnelClass {
  GstElementClass parent_class;
};

G_GNUC_INTERNAL GType   gst_funnel_get_type        (void);

G_END_DECLS

#endif /* __GST_FUNNEL_H__ */
