/*
 * Xournal++
 *
 * Xournal widget which is the "View" widget
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <memory>

#include <gtk/gtk.h>

#include "util/Rectangle.h"

G_BEGIN_DECLS

#define GTK_XOURNAL(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, gtk_xournal_get_type(), GtkXournal)
#define GTK_XOURNAL_CLASS(klass) GTK_CHECK_CLASS_CAST(klass, gtk_xournal_get_type(), GtkXournalClass)
#define GTK_IS_XOURNAL(obj) G_TYPE_CHECK_INSTANCE_TYPE(obj, gtk_xournal_get_type())

class EditSelection;
class SetsquareView;
class Layout;
class XojPageView;
class ScrollHandling;
class XournalView;
class InputContext;


typedef struct _GtkXournal GtkXournal;
typedef struct _GtkXournalClass GtkXournalClass;

struct _GtkXournal {
    GtkWidget widget;

    /**
     * The view class
     */
    XournalView* view;

    /**
     * Scrollbars
     */
    ScrollHandling* scrollHandling;

    /**
     * Visible area
     */
    int x;
    int y;

    Layout* layout;


    /**
     * Selected content, if any
     */
    EditSelection* selection;

    /**
     * Setsquare, if active
     */
    std::unique_ptr<SetsquareView> setsquareView;

    /**
     * Input handling
     */
    InputContext* input = nullptr;
};

struct _GtkXournalClass {
    GtkWidgetClass parent_class;
};

GType gtk_xournal_get_type();

GtkWidget* gtk_xournal_new(XournalView* view, InputContext* inputContext);

Layout* gtk_xournal_get_layout(GtkWidget* widget);

void gtk_xournal_scroll_relative(GtkWidget* widget, double x, double y);

void gtk_xournal_repaint_area(GtkWidget* widget, int x1, int y1, int x2, int y2);

xoj::util::Rectangle<double>* gtk_xournal_get_visible_area(GtkWidget* widget, XojPageView* p);

G_END_DECLS
