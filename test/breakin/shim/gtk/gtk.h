#pragma once
// Host test shim: just the glib names iambic.c uses. The main loop is simulated by test_breakin.c.
typedef int gboolean;
typedef void *gpointer;
typedef unsigned int guint;
typedef gboolean (*GSourceFunc)(gpointer);
#define G_SOURCE_REMOVE 0
#define GINT_TO_POINTER(i) ((gpointer)(long)(i))
#define GPOINTER_TO_INT(p) ((int)(long)(p))
#define GUINT_TO_POINTER(u) ((gpointer)(unsigned long)(u))
#define GPOINTER_TO_UINT(p) ((guint)(unsigned long)(p))
guint g_idle_add(GSourceFunc fn, gpointer data);
guint g_timeout_add(guint ms, GSourceFunc fn, gpointer data);
