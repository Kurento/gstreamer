/* GStreamer
 *
 * Copyright (C) 2005 Stefan Kost <ensonic at users dot sf dot net>
 *               2007 Sebastian Dröge <slomo@circular-chaos.org>
 *               2011 Stefan Sauer <ensonic at users dot sf dot net>
 *
 * gstcontroller.c: dynamic parameter control subsystem
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
 * SECTION:gstcontroller
 * @short_description: dynamic parameter control subsystem
 *
 * The controller subsystem offers a lightweight way to adjust gobject
 * properties over stream-time. It works by using time-stamped value pairs that
 * are queued for element-properties. At run-time the elements continously pull
 * values changes for the current stream-time.
 *
 * What needs to be changed in a #GstElement?
 * Very little - it is just two steps to make a plugin controllable!
 * <orderedlist>
 *   <listitem><para>
 *     mark gobject-properties paramspecs that make sense to be controlled,
 *     by GST_PARAM_CONTROLLABLE.
 *   </para></listitem>
 *   <listitem><para>
 *     when processing data (get, chain, loop function) at the beginning call
 *     gst_object_sync_values(element,timestamp).
 *     This will made the controller to update all gobject properties that are under
 *     control with the current values based on timestamp.
 *   </para></listitem>
 * </orderedlist>
 *
 * What needs to be done in applications?
 * Again its not a lot to change.
 * <orderedlist>
 *   <listitem><para>
 *     first put some properties under control, by calling
 *     controller = gst_object_control_properties (object, "prop1", "prop2",...);
 *   </para></listitem>
 *   <listitem><para>
 *     create a #GstControlSource.
 *     csource = gst_interpolation_control_source_new ();
 *     gst_interpolation_control_source_set_interpolation_mode(csource, mode);
 *   </para></listitem>
 *   <listitem><para>
 *     Attach the #GstControlSource on the controller to a property.
 *     gst_controller_set_control_source (controller, "prop1", csource);
 *   </para></listitem>
 *   <listitem><para>
 *     Set the control values
 *     gst_interpolation_control_source_set (csource,0 * GST_SECOND, value1);
 *     gst_interpolation_control_source_set (csource,1 * GST_SECOND, value2);
 *   </para></listitem>
 *   <listitem><para>
 *     start your pipeline
 *   </para></listitem>
 * </orderedlist>
 */

#include "gst_private.h"

#include "gstobject.h"
#include "gstclock.h"
#include "gstinfo.h"
#include "gstcontroller.h"
#include "gstcontrolsource.h"
#include "gstparamspecs.h"

#define GST_CAT_DEFAULT controller_debug
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

static GObjectClass *parent_class = NULL;

/* property ids */
enum
{
  PROP_CONTROL_RATE = 1
};

struct _GstControllerPrivate
{
  GstClockTime control_rate;
  GstClockTime last_sync;
};

/* helper */

/*
 * GstControlledProperty:
 */
typedef struct _GstControlledProperty
{
  GParamSpec *pspec;            /* GParamSpec for this property */
  const gchar *name;            /* name of the property */
  GstControlSource *csource;    /* GstControlSource for this property */
  gboolean disabled;
  GValue last_value;
} GstControlledProperty;

#define GST_CONTROLLED_PROPERTY(obj)    ((GstControlledProperty *)(obj))

/*
 * gst_controlled_property_new:
 * @object: for which object the controlled property should be set up
 * @name: the name of the property to be controlled
 *
 * Private method which initializes the fields of a new controlled property
 * structure.
 *
 * Returns: a freshly allocated structure or %NULL
 */
static GstControlledProperty *
gst_controlled_property_new (GstObject * object, const gchar * name)
{
  GstControlledProperty *prop = NULL;
  GParamSpec *pspec;

  GST_INFO ("trying to put property '%s' under control", name);

  /* check if the object has a property of that name */
  if ((pspec =
          g_object_class_find_property (G_OBJECT_GET_CLASS (object), name))) {
    GST_DEBUG ("  psec->flags : 0x%08x", pspec->flags);

    /* check if this param is witable && controlable && !construct-only */
    g_return_val_if_fail ((pspec->flags & (G_PARAM_WRITABLE |
                GST_PARAM_CONTROLLABLE | G_PARAM_CONSTRUCT_ONLY)) ==
        (G_PARAM_WRITABLE | GST_PARAM_CONTROLLABLE), NULL);

    if ((prop = g_slice_new (GstControlledProperty))) {
      prop->pspec = pspec;
      prop->name = pspec->name;
      prop->csource = NULL;
      prop->disabled = FALSE;
      memset (&prop->last_value, 0, sizeof (GValue));
      g_value_init (&prop->last_value, G_PARAM_SPEC_VALUE_TYPE (prop->pspec));
    }
  } else {
    GST_WARNING ("class '%s' has no property '%s'", G_OBJECT_TYPE_NAME (object),
        name);
  }
  return prop;
}

/*
 * gst_controlled_property_free:
 * @prop: the object to free
 *
 * Private method which frees all data allocated by a #GstControlledProperty
 * instance.
 */
static void
gst_controlled_property_free (GstControlledProperty * prop)
{
  if (prop->csource)
    g_object_unref (prop->csource);
  g_value_unset (&prop->last_value);
  g_slice_free (GstControlledProperty, prop);
}

/*
 * gst_controller_find_controlled_property:
 * @self: the controller object to search for a property in
 * @name: the gobject property name to look for
 *
 * Searches the list of properties under control.
 *
 * Returns: a #GstControlledProperty object of %NULL if the property is not
 * being controlled.
 */
static GstControlledProperty *
gst_controller_find_controlled_property (GstController * self,
    const gchar * name)
{
  GstControlledProperty *prop;
  GList *node;

  for (node = self->properties; node; node = g_list_next (node)) {
    prop = node->data;
    /* FIXME: eventually use GQuark to speed it up */
    if (!strcmp (prop->name, name)) {
      return prop;
    }
  }
  GST_DEBUG ("controller does not (yet) manage property '%s'", name);

  return NULL;
}

/*
 * gst_controller_add_property:
 * @self: the controller object
 * @name: name of projecty in @object
 *
 * Creates a new #GstControlledProperty if there is none for property @name yet.
 *
 * Returns: %TRUE if the property has been added to the controller
 */
static gboolean
gst_controller_add_property (GstController * self, const gchar * name)
{
  gboolean res = TRUE;

  /* test if this property isn't yet controlled */
  if (!gst_controller_find_controlled_property (self, name)) {
    GstControlledProperty *prop;

    /* create GstControlledProperty and add to self->properties list */
    if ((prop = gst_controlled_property_new (self->object, name))) {
      self->properties = g_list_prepend (self->properties, prop);
      GST_DEBUG_OBJECT (self->object, "property %s added", name);
    } else
      res = FALSE;
  } else {
    GST_WARNING_OBJECT (self->object, "trying to control property %s again",
        name);
  }
  return res;
}

/*
 * gst_controller_remove_property:
 * @self: the controller object
 * @name: name of projecty in @object
 *
 * Removes a #GstControlledProperty for property @name.
 *
 * Returns: %TRUE if the property has been removed from the controller
 */
static gboolean
gst_controller_remove_property (GstController * self, const gchar * name)
{
  gboolean res = TRUE;
  GstControlledProperty *prop;

  if ((prop = gst_controller_find_controlled_property (self, name))) {
    self->properties = g_list_remove (self->properties, prop);
    //g_signal_handler_disconnect (self->object, prop->notify_handler_id);
    gst_controlled_property_free (prop);
    GST_DEBUG_OBJECT (self->object, "property %s removed", name);
  } else {
    res = FALSE;
  }
  return res;
}

/* methods */

/**
 * gst_controller_new_valist:
 * @object: the object of which some properties should be controlled
 * @var_args: %NULL terminated list of property names that should be controlled
 *
 * Creates a new GstController for the given object's properties
 *
 * Returns: the new controller.
 */
GstController *
gst_controller_new_valist (GstObject * object, va_list var_args)
{
  GstController *self;
  gchar *name;

  g_return_val_if_fail (G_IS_OBJECT (object), NULL);

  /* FIXME: storing the controller into the object is ugly
   * we'd like to make the controller object completely internal
   */
  self = g_object_newv (GST_TYPE_CONTROLLER, 0, NULL);
  self->object = g_object_ref (object);
  object->ctrl = g_object_ref (self);

  /* create GstControlledProperty for each property */
  while ((name = va_arg (var_args, gchar *))) {
    gst_controller_add_property (self, name);
  }
  va_end (var_args);

  return self;
}

/**
 * gst_controller_new_list:
 * @object: the object of which some properties should be controlled
 * @list: (transfer none) (element-type utf8): list of property names
 *   that should be controlled
 *
 * Creates a new GstController for the given object's properties
 *
 * Rename to: gst_controller_new
 *
 * Returns: the new controller.
 */
GstController *
gst_controller_new_list (GstObject * object, GList * list)
{
  GstController *self;
  gchar *name;
  GList *node;

  g_return_val_if_fail (G_IS_OBJECT (object), NULL);

  self = g_object_newv (GST_TYPE_CONTROLLER, 0, NULL);
  self->object = g_object_ref (object);
  object->ctrl = g_object_ref (self);

  /* create GstControlledProperty for each property */
  for (node = list; node; node = g_list_next (node)) {
    name = (gchar *) node->data;
    gst_controller_add_property (self, name);
  }

  return self;
}

/**
 * gst_controller_new:
 * @object: the object of which some properties should be controlled
 * @...: %NULL terminated list of property names that should be controlled
 *
 * Creates a new GstController for the given object's properties
 *
 * Returns: the new controller.
 */
GstController *
gst_controller_new (GstObject * object, ...)
{
  GstController *self;
  va_list var_args;

  g_return_val_if_fail (G_IS_OBJECT (object), NULL);

  va_start (var_args, object);
  self = gst_controller_new_valist (object, var_args);
  va_end (var_args);

  return self;
}

// FIXME: docs
gboolean
gst_controller_add_properties_valist (GstController * self, va_list var_args)
{
  gboolean res = TRUE;
  gchar *name;

  g_return_val_if_fail (GST_IS_CONTROLLER (self), FALSE);

  while ((name = va_arg (var_args, gchar *))) {
    /* find the property in the properties list of the controller, remove and free it */
    g_mutex_lock (self->lock);
    res &= gst_controller_add_property (self, name);
    g_mutex_unlock (self->lock);
  }

  return res;
}

// FIXME: docs
gboolean
gst_controller_add_properties_list (GstController * self, GList * list)
{
  gboolean res = TRUE;
  gchar *name;
  GList *tmp;

  g_return_val_if_fail (GST_IS_CONTROLLER (self), FALSE);

  for (tmp = list; tmp; tmp = g_list_next (tmp)) {
    name = (gchar *) tmp->data;

    /* find the property in the properties list of the controller, remove and free it */
    g_mutex_lock (self->lock);
    res &= gst_controller_add_property (self, name);
    g_mutex_unlock (self->lock);
  }

  return res;
}

// FIXME: docs
gboolean
gst_controller_add_properties (GstController * self, ...)
{
  gboolean res;
  va_list var_args;

  g_return_val_if_fail (GST_IS_CONTROLLER (self), FALSE);

  va_start (var_args, self);
  res = gst_controller_add_properties_valist (self, var_args);
  va_end (var_args);

  return res;
}

/**
 * gst_controller_remove_properties_valist:
 * @self: the controller object from which some properties should be removed
 * @var_args: %NULL terminated list of property names that should be removed
 *
 * Removes the given object properties from the controller
 *
 * Returns: %FALSE if one of the given property isn't handled by the controller, %TRUE otherwise
 */
gboolean
gst_controller_remove_properties_valist (GstController * self, va_list var_args)
{
  gboolean res = TRUE;
  gchar *name;

  g_return_val_if_fail (GST_IS_CONTROLLER (self), FALSE);

  while ((name = va_arg (var_args, gchar *))) {
    /* find the property in the properties list of the controller, remove and free it */
    g_mutex_lock (self->lock);
    res &= gst_controller_remove_property (self, name);
    g_mutex_unlock (self->lock);
  }

  return res;
}

/**
 * gst_controller_remove_properties_list:
 * @self: the controller object from which some properties should be removed
 * @list: (transfer none) (element-type utf8): #GList of property names that
 *   should be removed
 *
 * Removes the given object properties from the controller
 *
 * Rename to: gst_controller_remove_properties
 *
 * Returns: %FALSE if one of the given property isn't handled by the controller, %TRUE otherwise
 */
gboolean
gst_controller_remove_properties_list (GstController * self, GList * list)
{
  gboolean res = TRUE;
  gchar *name;
  GList *tmp;

  g_return_val_if_fail (GST_IS_CONTROLLER (self), FALSE);

  for (tmp = list; tmp; tmp = g_list_next (tmp)) {
    name = (gchar *) tmp->data;

    /* find the property in the properties list of the controller, remove and free it */
    g_mutex_lock (self->lock);
    res &= gst_controller_remove_property (self, name);
    g_mutex_unlock (self->lock);
  }

  return res;
}

/**
 * gst_controller_remove_properties:
 * @self: the controller object from which some properties should be removed
 * @...: %NULL terminated list of property names that should be removed
 *
 * Removes the given object properties from the controller
 *
 * Returns: %FALSE if one of the given property isn't handled by the controller, %TRUE otherwise
 */
gboolean
gst_controller_remove_properties (GstController * self, ...)
{
  gboolean res;
  va_list var_args;

  g_return_val_if_fail (GST_IS_CONTROLLER (self), FALSE);

  va_start (var_args, self);
  res = gst_controller_remove_properties_valist (self, var_args);
  va_end (var_args);

  return res;
}

/**
 * gst_controller_is_active:
 * @self: the #GstController which should be disabled
 *
 * Check if the controller is active. It is active if it has at least one
 * controlled property that is not disabled.
 *
 * Returns: %TRUE if the controller is active
 */
gboolean
gst_controller_is_active (GstController * self)
{
  gboolean active = FALSE;
  GList *node;
  GstControlledProperty *prop;

  g_return_val_if_fail (GST_IS_CONTROLLER (self), FALSE);

  g_mutex_lock (self->lock);
  for (node = self->properties; node; node = node->next) {
    prop = node->data;
    active |= !prop->disabled;
  }
  g_mutex_unlock (self->lock);

  return active;
}

/**
 * gst_controller_set_property_disabled:
 * @self: the #GstController which should be disabled
 * @property_name: property to disable
 * @disabled: boolean that specifies whether to disable the controller
 * or not.
 *
 * This function is used to disable the #GstController on a property for
 * some time, i.e. gst_controller_sync_values() will do nothing for the
 * property.
 */
void
gst_controller_set_property_disabled (GstController * self,
    const gchar * property_name, gboolean disabled)
{
  GstControlledProperty *prop;

  g_return_if_fail (GST_IS_CONTROLLER (self));
  g_return_if_fail (property_name);

  g_mutex_lock (self->lock);
  if ((prop = gst_controller_find_controlled_property (self, property_name))) {
    prop->disabled = disabled;
  }
  g_mutex_unlock (self->lock);
}

/**
 * gst_controller_set_disabled:
 * @self: the #GstController which should be disabled
 * @disabled: boolean that specifies whether to disable the controller
 * or not.
 *
 * This function is used to disable all properties of the #GstController
 * for some time, i.e. gst_controller_sync_values() will do nothing.
 */

void
gst_controller_set_disabled (GstController * self, gboolean disabled)
{
  GList *node;
  GstControlledProperty *prop;

  g_return_if_fail (GST_IS_CONTROLLER (self));

  g_mutex_lock (self->lock);
  for (node = self->properties; node; node = node->next) {
    prop = node->data;
    prop->disabled = disabled;
  }
  g_mutex_unlock (self->lock);
}

/**
 * gst_controller_set_control_source:
 * @self: the controller object
 * @property_name: name of the property for which the #GstControlSource should be set
 * @csource: the #GstControlSource that should be used for the property
 *
 * Sets the #GstControlSource for @property_name. If there already was a #GstControlSource
 * for this property it will be unreferenced.
 *
 * Returns: %FALSE if the given property isn't handled by the controller or the new #GstControlSource
 * couldn't be bound to the property, %TRUE if everything worked as expected.
 */
gboolean
gst_controller_set_control_source (GstController * self,
    const gchar * property_name, GstControlSource * csource)
{
  GstControlledProperty *prop;
  gboolean ret = FALSE;

  g_mutex_lock (self->lock);
  if ((prop = gst_controller_find_controlled_property (self, property_name))) {
    GstControlSource *old = prop->csource;

    if (csource && (ret = gst_control_source_bind (csource, prop->pspec))) {
      g_object_ref (csource);
      prop->csource = csource;
    } else if (!csource) {
      ret = TRUE;
      prop->csource = NULL;
    }

    if (ret && old)
      g_object_unref (old);
  }
  g_mutex_unlock (self->lock);

  return ret;
}

/**
 * gst_controller_get_control_source:
 * @self: the controller object
 * @property_name: name of the property for which the #GstControlSource should be get
 *
 * Gets the corresponding #GstControlSource for the property. This should be unreferenced
 * again after use.
 *
 * Returns: (transfer full): the #GstControlSource for @property_name or NULL if
 * the property is not controlled by this controller or no #GstControlSource was
 * assigned yet.
 */
GstControlSource *
gst_controller_get_control_source (GstController * self,
    const gchar * property_name)
{
  GstControlledProperty *prop;
  GstControlSource *ret = NULL;

  g_return_val_if_fail (GST_IS_CONTROLLER (self), NULL);
  g_return_val_if_fail (property_name, NULL);

  g_mutex_lock (self->lock);
  if ((prop = gst_controller_find_controlled_property (self, property_name))) {
    ret = prop->csource;
  }
  g_mutex_unlock (self->lock);

  if (ret)
    g_object_ref (ret);

  return ret;
}

/**
 * gst_controller_get:
 * @self: the controller object which handles the properties
 * @property_name: the name of the property to get
 * @timestamp: the time the control-change should be read from
 *
 * Gets the value for the given controller-handled property at the requested
 * time.
 *
 * Returns: the GValue of the property at the given time, or %NULL if the
 * property isn't handled by the controller
 */
GValue *
gst_controller_get (GstController * self, const gchar * property_name,
    GstClockTime timestamp)
{
  GstControlledProperty *prop;
  GValue *val = NULL;

  g_return_val_if_fail (GST_IS_CONTROLLER (self), NULL);
  g_return_val_if_fail (property_name, NULL);
  g_return_val_if_fail (GST_CLOCK_TIME_IS_VALID (timestamp), NULL);

  g_mutex_lock (self->lock);
  if ((prop = gst_controller_find_controlled_property (self, property_name))) {
    val = g_new0 (GValue, 1);
    g_value_init (val, G_PARAM_SPEC_VALUE_TYPE (prop->pspec));
    if (prop->csource) {
      gboolean res;

      /* get current value via control source */
      res = gst_control_source_get_value (prop->csource, timestamp, val);
      if (!res) {
        g_free (val);
        val = NULL;
      }
    } else {
      g_object_get_property ((GObject *) self->object, prop->name, val);
    }
  }
  g_mutex_unlock (self->lock);

  return val;
}

/**
 * gst_controller_suggest_next_sync:
 * @self: the controller that handles the values
 *
 * Returns a suggestion for timestamps where buffers should be split
 * to get best controller results.
 *
 * Returns: Returns the suggested timestamp or %GST_CLOCK_TIME_NONE
 * if no control-rate was set.
 */
GstClockTime
gst_controller_suggest_next_sync (GstController * self)
{
  GstClockTime ret;

  g_return_val_if_fail (GST_IS_CONTROLLER (self), GST_CLOCK_TIME_NONE);
  g_return_val_if_fail (self->priv->control_rate != GST_CLOCK_TIME_NONE,
      GST_CLOCK_TIME_NONE);

  g_mutex_lock (self->lock);

  /* TODO: Implement more logic, depending on interpolation mode
   * and control points
   * FIXME: we need playback direction
   */
  ret = self->priv->last_sync + self->priv->control_rate;

  g_mutex_unlock (self->lock);

  return ret;
}

/**
 * gst_controller_sync_values:
 * @self: the controller that handles the values
 * @timestamp: the time that should be processed
 *
 * Sets the properties of the element, according to the controller that (maybe)
 * handles them and for the given timestamp.
 *
 * If this function fails, it is most likely the application developers fault.
 * Most probably the control sources are not setup correctly.
 *
 * Returns: %TRUE if the controller values could be applied to the object
 * properties, %FALSE otherwise
 */
gboolean
gst_controller_sync_values (GstController * self, GstClockTime timestamp)
{
  GstControlledProperty *prop;
  GList *node;
  gboolean ret = TRUE, val_ret;
  GValue value = { 0, };

  g_return_val_if_fail (GST_IS_CONTROLLER (self), FALSE);
  g_return_val_if_fail (GST_CLOCK_TIME_IS_VALID (timestamp), FALSE);

  GST_LOG ("sync_values");

  g_mutex_lock (self->lock);
  g_object_freeze_notify ((GObject *) self->object);
  /* go over the controlled properties of the controller */
  for (node = self->properties; node; node = g_list_next (node)) {
    prop = node->data;

    if (!prop->csource || prop->disabled)
      continue;

    GST_LOG ("property '%s' at ts=%" G_GUINT64_FORMAT, prop->name, timestamp);

    /* we can make this faster
     * http://bugzilla.gnome.org/show_bug.cgi?id=536939
     */
    g_value_init (&value, G_PARAM_SPEC_VALUE_TYPE (prop->pspec));
    val_ret = gst_control_source_get_value (prop->csource, timestamp, &value);
    if (G_LIKELY (val_ret)) {
      /* always set the value for first time, but then only if it changed
       * this should limit g_object_notify invocations.
       * FIXME: can we detect negative playback rates?
       */
      if ((timestamp < self->priv->last_sync) ||
          gst_value_compare (&value, &prop->last_value) != GST_VALUE_EQUAL) {
        g_object_set_property ((GObject *) self->object, prop->name, &value);
        g_value_copy (&value, &prop->last_value);
      }
    } else {
      GST_DEBUG ("no control value for param %s", prop->name);
    }
    g_value_unset (&value);
    ret &= val_ret;
  }
  self->priv->last_sync = timestamp;
  g_object_thaw_notify ((GObject *) self->object);

  g_mutex_unlock (self->lock);

  return ret;
}

/**
 * gst_controller_get_value_arrays:
 * @self: the controller that handles the values
 * @timestamp: the time that should be processed
 * @value_arrays: list to return the control-values in
 *
 * Function to be able to get an array of values for one or more given element
 * properties.
 *
 * All fields of the %GstValueArray in the list must be filled correctly.
 * Especially the GstValueArray->values arrays must be big enough to keep
 * the requested amount of values.
 *
 * The types of the values in the array are the same as the property's type.
 *
 * <note><para>This doesn't modify the controlled GObject properties!</para></note>
 *
 * Returns: %TRUE if the given array(s) could be filled, %FALSE otherwise
 */
gboolean
gst_controller_get_value_arrays (GstController * self,
    GstClockTime timestamp, GSList * value_arrays)
{
  gboolean res = TRUE;
  GSList *node;

  g_return_val_if_fail (GST_IS_CONTROLLER (self), FALSE);
  g_return_val_if_fail (GST_CLOCK_TIME_IS_VALID (timestamp), FALSE);
  g_return_val_if_fail (value_arrays, FALSE);

  for (node = value_arrays; (res && node); node = g_slist_next (node)) {
    res = gst_controller_get_value_array (self, timestamp, node->data);
  }

  return (res);
}

/**
 * gst_controller_get_value_array:
 * @self: the controller that handles the values
 * @timestamp: the time that should be processed
 * @value_array: array to put control-values in
 *
 * Function to be able to get an array of values for one element property.
 *
 * All fields of @value_array must be filled correctly. Especially the
 * @value_array->values array must be big enough to keep the requested amount
 * of values (as indicated by the nbsamples field).
 *
 * The type of the values in the array is the same as the property's type.
 *
 * <note><para>This doesn't modify the controlled GObject property!</para></note>
 *
 * Returns: %TRUE if the given array could be filled, %FALSE otherwise
 */
gboolean
gst_controller_get_value_array (GstController * self, GstClockTime timestamp,
    GstValueArray * value_array)
{
  gboolean res = FALSE;
  GstControlledProperty *prop;

  g_return_val_if_fail (GST_IS_CONTROLLER (self), FALSE);
  g_return_val_if_fail (GST_CLOCK_TIME_IS_VALID (timestamp), FALSE);
  g_return_val_if_fail (value_array, FALSE);
  g_return_val_if_fail (value_array->property_name, FALSE);
  g_return_val_if_fail (value_array->values, FALSE);

  g_mutex_lock (self->lock);

  if ((prop =
          gst_controller_find_controlled_property (self,
              value_array->property_name))) {
    /* get current value_array via control source */

    if (!prop->csource)
      goto out;

    res =
        gst_control_source_get_value_array (prop->csource, timestamp,
        value_array);
  }

out:
  g_mutex_unlock (self->lock);
  return res;
}

/* gobject handling */

static void
_gst_controller_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstController *self = GST_CONTROLLER (object);

  switch (property_id) {
    case PROP_CONTROL_RATE:{
      /* FIXME: don't change if element is playing, controller works for GObject
         so this wont work

         GstState c_state, p_state;
         GstStateChangeReturn ret;

         ret = gst_element_get_state (self->object, &c_state, &p_state, 0);
         if ((ret == GST_STATE_CHANGE_SUCCESS &&
         (c_state == GST_STATE_NULL || c_state == GST_STATE_READY)) ||
         (ret == GST_STATE_CHANGE_ASYNC &&
         (p_state == GST_STATE_NULL || p_state == GST_STATE_READY))) {
       */
      g_value_set_uint64 (value, self->priv->control_rate);
      /*
         }
         else {
         GST_WARNING ("Changing the control rate is only allowed if the elemnt"
         " is in NULL or READY");
         }
       */
    }
      break;
    default:{
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
      break;
  }
}

/* sets the given properties for this object */
static void
_gst_controller_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstController *self = GST_CONTROLLER (object);

  switch (property_id) {
    case PROP_CONTROL_RATE:{
      self->priv->control_rate = g_value_get_uint64 (value);
    }
      break;
    default:{
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
      break;
  }
}

static void
_gst_controller_dispose (GObject * object)
{
  GstController *self = GST_CONTROLLER (object);

  if (self->object != NULL) {
    g_mutex_lock (self->lock);
    /* free list of properties */
    if (self->properties) {
      GList *node;

      for (node = self->properties; node; node = g_list_next (node)) {
        GstControlledProperty *prop = node->data;

        gst_controlled_property_free (prop);
      }
      g_list_free (self->properties);
      self->properties = NULL;
    }

    g_object_unref (self->object);
    self->object = NULL;
    g_mutex_unlock (self->lock);
  }

  if (G_OBJECT_CLASS (parent_class)->dispose)
    (G_OBJECT_CLASS (parent_class)->dispose) (object);
}

static void
_gst_controller_finalize (GObject * object)
{
  GstController *self = GST_CONTROLLER (object);

  g_mutex_free (self->lock);

  if (G_OBJECT_CLASS (parent_class)->finalize)
    (G_OBJECT_CLASS (parent_class)->finalize) (object);
}

static void
_gst_controller_init (GTypeInstance * instance, gpointer g_class)
{
  GstController *self = GST_CONTROLLER (instance);

  self->lock = g_mutex_new ();
  self->priv =
      G_TYPE_INSTANCE_GET_PRIVATE (self, GST_TYPE_CONTROLLER,
      GstControllerPrivate);
  self->priv->last_sync = GST_CLOCK_TIME_NONE;
  self->priv->control_rate = 100 * GST_MSECOND;
}

static void
_gst_controller_class_init (GstControllerClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  parent_class = g_type_class_peek_parent (klass);
  g_type_class_add_private (klass, sizeof (GstControllerPrivate));

  gobject_class->set_property = _gst_controller_set_property;
  gobject_class->get_property = _gst_controller_get_property;
  gobject_class->dispose = _gst_controller_dispose;
  gobject_class->finalize = _gst_controller_finalize;

  /* register properties */
  g_object_class_install_property (gobject_class, PROP_CONTROL_RATE,
      g_param_spec_uint64 ("control-rate",
          "control rate",
          "Controlled properties will be updated at least every control-rate nanoseconds",
          1, G_MAXUINT, 100 * GST_MSECOND,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "gstcontroller", 0,
      "dynamic parameter control for gstreamer elements");
}

GType
gst_controller_get_type (void)
{
  static volatile gsize type = 0;

  if (g_once_init_enter (&type)) {
    GType _type;
    static const GTypeInfo info = {
      sizeof (GstControllerClass),
      NULL,                     /* base_init */
      NULL,                     /* base_finalize */
      (GClassInitFunc) _gst_controller_class_init,      /* class_init */
      NULL,                     /* class_finalize */
      NULL,                     /* class_data */
      sizeof (GstController),
      0,                        /* n_preallocs */
      (GInstanceInitFunc) _gst_controller_init, /* instance_init */
      NULL                      /* value_table */
    };
    _type = g_type_register_static (G_TYPE_OBJECT, "GstController", &info, 0);
    g_once_init_leave (&type, _type);
  }
  return type;
}