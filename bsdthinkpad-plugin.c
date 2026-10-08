/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, wugq
 *
 * bsdthinkpad-plugin -- XFCE panel plugin for what the pulseaudio plugin can
 * not do on FreeBSD:
 *
 *   brightness   slider (scroll on the panel icon too)
 *   speaker      switch: the ThinkPad hardware mute (same as the mute key),
 *                LED included
 *   microphone   switch: the recording level of all sound devices (same as
 *                the mic-mute key), LED included
 *
 * Volume is the pulseaudio plugin's job: its sink volume is the OSS "vol"
 * (and "pcm") of the sound card.  The tooltip, like the pulseaudio plugin's,
 * follows the keys while the pointer is on the icon.
 *
 * This file is only the UI.  The state and every change to the hardware are
 * in ctl.c: render() shows the Ctl's state, the widgets' handlers pass the
 * user's actions to ctl_set_*().
 */

#include <stdio.h>

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

#include "ctl.h"
#include "hw.h"

#define POLL_MS		300
#define SCROLL_STEP	4		/* as the pulseaudio plugin's volume step */

typedef struct {
	XfcePanelPlugin	*plugin;
	Ctl		*ctl;

	GtkWidget	*button, *icon;
	/*
	 * A GtkMenu, as the pulseaudio and power manager plugins have: GTK and
	 * the panel place it, grab keyboard and pointer while it is open (so
	 * the keys do nothing until it closes), and close it on a click
	 * outside, on the panel button, or on Escape.  Built once, popped up
	 * again and again.
	 */
	GtkWidget	*menu;
	gboolean	 hovering;
	gboolean	 dragging;	/* the brightness slider */

	GtkWidget	*bright_row, *bright_scale, *bright_label, *bright_sep;
	GtkWidget	*speaker_row, *speaker_icon, *speaker_switch;
	GtkWidget	*mic_row, *mic_icon, *mic_switch;

	CtlState	 shown;		/* what the widgets show */
	gboolean	 shown_valid;
	char		*tip;		/* current tooltip text */
} Panel;

/* ---- view: state -> widgets --------------------------------------------- */

static void on_brightness(GtkRange *, Panel *);
static void on_speaker(GObject *, GParamSpec *, Panel *);
static void on_mic(GObject *, GParamSpec *, Panel *);

/*
 * Icon names that the common themes (Adwaita, elementary-xfce, breeze) all
 * have.  There are no per-level brightness icons such as
 * "display-brightness-low-symbolic" in any of them.
 */
#define ICON_BRIGHTNESS	"display-brightness-symbolic"
#define ICON_SPEAKER_ON	"audio-volume-high-symbolic"
#define ICON_SPEAKER_OFF "audio-volume-muted-symbolic"
#define ICON_MIC_ON	"audio-input-microphone-symbolic"
#define ICON_MIC_OFF	"microphone-sensitivity-muted-symbolic"

static void
set_percent(GtkWidget *label, int v)
{
	char text[16];

	snprintf(text, sizeof(text), "%d%%", v);
	gtk_label_set_text(GTK_LABEL(label), text);
}

/*
 * A slider the user is dragging is left alone, or it would jump under the
 * pointer (the hardware may round the value, e.g. brightness 50 -> 49);
 * on_slider_released() catches up.
 */
static void
render_slider(Panel *p, GtkWidget *row, GtkWidget *scale, GtkWidget *label,
    gpointer handler, int v)
{
	gtk_widget_set_visible(row, v >= 0);
	if (v < 0 || p->dragging)
		return;
	g_signal_handlers_block_by_func(scale, handler, p);
	gtk_range_set_value(GTK_RANGE(scale), v);
	g_signal_handlers_unblock_by_func(scale, handler, p);
	set_percent(label, v);
}

/*
 * The handler is blocked while setting the switch: the change comes from the
 * hardware, not from the user.
 */
static void
render_switch(Panel *p, GtkWidget *row, GtkWidget *icon, GtkWidget *sw,
    gpointer handler, int muted, const char *icon_on, const char *icon_off)
{
	gtk_widget_set_visible(row, muted >= 0);
	if (muted < 0)
		return;
	g_signal_handlers_block_by_func(sw, handler, p);
	gtk_switch_set_active(GTK_SWITCH(sw), !muted);
	g_signal_handlers_unblock_by_func(sw, handler, p);
	gtk_image_set_from_icon_name(GTK_IMAGE(icon),
	    muted ? icon_off : icon_on, GTK_ICON_SIZE_MENU);
}

static void
render_speaker(Panel *p, int muted)
{
	render_switch(p, p->speaker_row, p->speaker_icon, p->speaker_switch,
	    on_speaker, muted, ICON_SPEAKER_ON, ICON_SPEAKER_OFF);
}

static void
render_mic(Panel *p, int muted)
{
	render_switch(p, p->mic_row, p->mic_icon, p->mic_switch, on_mic, muted,
	    ICON_MIC_ON, ICON_MIC_OFF);
}

/* Start a new tooltip line, unless it is the first */
static void
tip_line(GString *tip)
{
	if (tip->len > 0)
		g_string_append_c(tip, '\n');
}

static void
render_tooltip(Panel *p, const CtlState *s)
{
	GString *tip = g_string_new(NULL);

	if (s->brightness >= 0)
		g_string_append_printf(tip, "Brightness %d%%", s->brightness);
	if (s->speaker_mute >= 0) {
		tip_line(tip);
		g_string_append_printf(tip, "Speaker %s",
		    s->speaker_mute ? "off" : "on");
	}
	if (s->mic_mute >= 0) {
		if (s->speaker_mute >= 0)
			g_string_append(tip, " · ");
		else
			tip_line(tip);
		g_string_append_printf(tip, "Microphone %s",
		    s->mic_mute ? "off" : "on");
	}
	/* Only when it changed, so a visible tooltip does not flicker */
	if (g_strcmp0(tip->str, p->tip) != 0) {
		gtk_widget_set_tooltip_text(p->button, tip->str);
		g_free(p->tip);
		p->tip = g_strdup(tip->str);
	}
	g_string_free(tip, TRUE);
}

/* The Ctl's "changed" callback: show the state, touching only what changed */
static void
render(const CtlState *s, gpointer data)
{
	Panel *p = data;
	CtlState *o = &p->shown;
	gboolean all = !p->shown_valid;

	if (all || s->brightness != o->brightness)
		render_slider(p, p->bright_row, p->bright_scale,
		    p->bright_label, on_brightness, s->brightness);
	if (all || s->speaker_mute != o->speaker_mute)
		render_speaker(p, s->speaker_mute);
	if (all || s->mic_mute != o->mic_mute)
		render_mic(p, s->mic_mute);
	gtk_widget_set_visible(p->bright_sep, s->brightness >= 0 &&
	    (s->speaker_mute >= 0 || s->mic_mute >= 0));
	render_tooltip(p, s);

	*o = *s;
	p->shown_valid = TRUE;
}

/* ---- user actions -> ctl ------------------------------------------------ */

static void
on_brightness(GtkRange *range, Panel *p)
{
	set_percent(p->bright_label, (int)gtk_range_get_value(range));
	ctl_set_brightness(p->ctl, (int)gtk_range_get_value(range));
}

/*
 * The switches show the Ctl's state, not the click: if the request was
 * refused (one already pending, a mixer that can not be changed), the state
 * did not change, render() is not called, and this puts the switch back.
 */

/* Switch on = sound on */
static void
on_speaker(GObject *sw, GParamSpec *pspec, Panel *p)
{
	(void)pspec;
	ctl_set_speaker_mute(p->ctl, !gtk_switch_get_active(GTK_SWITCH(sw)));
	render_speaker(p, ctl_state(p->ctl)->speaker_mute);
}

/* Switch on = microphone on */
static void
on_mic(GObject *sw, GParamSpec *pspec, Panel *p)
{
	(void)pspec;
	ctl_set_mic_mute(p->ctl, !gtk_switch_get_active(GTK_SWITCH(sw)));
	render_mic(p, ctl_state(p->ctl)->mic_mute);
}

static gboolean
render_slider_idle(gpointer data)
{
	Panel *p = data;
	const CtlState *s = ctl_state(p->ctl);

	render_slider(p, p->bright_row, p->bright_scale, p->bright_label,
	    on_brightness, s->brightness);
	return G_SOURCE_REMOVE;
}

/* Scroll on the panel icon: brightness up/down */
static gboolean
on_scroll(GtkWidget *w, GdkEventScroll *ev, Panel *p)
{
	(void)w;
	g_debug("scroll: direction %d", ev->direction);
	if (ev->direction == GDK_SCROLL_UP)
		ctl_step_brightness(p->ctl, SCROLL_STEP);
	else if (ev->direction == GDK_SCROLL_DOWN)
		ctl_step_brightness(p->ctl, -SCROLL_STEP);
	else
		return FALSE;
	return TRUE;
}

/* Poll while the menu is open or the pointer is on the icon */
static void
update_polling(Panel *p)
{
	ctl_set_polling(p->ctl,
	    p->hovering || gtk_widget_get_visible(p->menu) ? POLL_MS : 0);
}

static gboolean
on_enter(GtkWidget *w, GdkEventCrossing *ev, Panel *p)
{
	(void)w;
	(void)ev;
	p->hovering = TRUE;
	update_polling(p);
	return FALSE;
}

static gboolean
on_leave(GtkWidget *w, GdkEventCrossing *ev, Panel *p)
{
	(void)w;
	(void)ev;
	p->hovering = FALSE;
	update_polling(p);
	return FALSE;
}

/* ---- the menu ----------------------------------------------------------- */

/*
 * Left button on the panel icon: open the menu.  Handled on the press, as
 * the pulseaudio and clock plugins do, and not passed on, so the toggle
 * button does not toggle itself.  While the menu is open a click on the icon
 * goes to the menu, which closes; the second branch is only a fallback.
 */
static gboolean
on_button_press(GtkWidget *w, GdkEventButton *ev, Panel *p)
{
	if (ev->button != 1)
		return FALSE;
	if (ev->type != GDK_BUTTON_PRESS)
		return TRUE;			/* double and triple clicks */
	if (gtk_widget_get_visible(p->menu))
		gtk_menu_popdown(GTK_MENU(p->menu));
	else {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), TRUE);
		xfce_panel_plugin_popup_menu(p->plugin, GTK_MENU(p->menu), w,
		    (GdkEvent *)ev);
		/*
		 * GTK does not show the menu, and says nothing, when it can not
		 * grab the pointer: do not leave the button pressed
		 */
		if (!gtk_widget_get_visible(p->menu)) {
			g_debug("menu: not shown (no pointer grab?)");
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), FALSE);
		} else
			g_debug("menu: open");
		update_polling(p);
	}
	return TRUE;
}

static void
on_menu_hide(GtkWidget *menu, Panel *p)
{
	(void)menu;
	g_debug("menu: closed");
	p->dragging = FALSE;
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->button), FALSE);
	update_polling(p);
	g_idle_add(render_slider_idle, p);
}

/* Are x, y (relative to from) on w? */
static gboolean
on_widget(GtkWidget *from, GtkWidget *w, gdouble x, gdouble y)
{
	gint wx, wy;

	return gtk_widget_translate_coordinates(from, w, (gint)x, (gint)y,
	    &wx, &wy) && wx >= 0 && wy >= 0 &&
	    wx < gtk_widget_get_allocated_width(w) &&
	    wy < gtk_widget_get_allocated_height(w);
}

/*
 * In a menu the pointer's events go to the menu item, not to the widgets in
 * it, and a click chooses the item and closes the menu.  So a press on the
 * slider is passed on to it; the slider then grabs the pointer, and GTK
 * gives it the motion and the release itself.  The release still reaches
 * the item, and goes no further, so the menu does not take it as a click.
 */
static gboolean
on_slider_item_press(GtkWidget *item, GdkEventButton *ev, Panel *p)
{
	if (ev->type == GDK_BUTTON_PRESS &&
	    on_widget(item, p->bright_scale, ev->x, ev->y)) {
		g_debug("slider: pressed");
		p->dragging = TRUE;
		gtk_widget_event(p->bright_scale, (GdkEvent *)ev);
	}
	return TRUE;
}

/*
 * Dragging ended: show the value the hardware took (see render_slider()),
 * once the slider has let go
 */
static gboolean
on_slider_item_release(GtkWidget *item, GdkEventButton *ev, Panel *p)
{
	(void)item;
	(void)ev;
	if (p->dragging) {
		g_debug("slider: released");
		p->dragging = FALSE;
		g_idle_add(render_slider_idle, p);
	}
	return TRUE;
}

/* Scrolling on the slider's item, as on the panel icon */
static gboolean
on_slider_item_scroll(GtkWidget *item, GdkEventScroll *ev, Panel *p)
{
	(void)item;
	on_scroll(NULL, ev, p);
	return TRUE;
}

static void
flip_switch(GtkWidget *sw)
{
	g_debug("switch: flipped by its row");
	gtk_switch_set_active(GTK_SWITCH(sw),
	    !gtk_switch_get_active(GTK_SWITCH(sw)));
}

/*
 * A click on a switch's row flips the switch, as one on the switch itself
 * does, and keeps the menu open, so that both can be set at once.  Enter
 * (the item's "activate") flips it too, and closes the menu.
 */
static gboolean
on_switch_item_release(GtkWidget *item, GdkEventButton *ev, GtkWidget *sw)
{
	(void)item;
	(void)ev;
	flip_switch(sw);
	return TRUE;
}

static void
on_switch_item_activate(GtkMenuItem *item, GtkWidget *sw)
{
	(void)item;
	flip_switch(sw);
}

/* ---- building the widgets ----------------------------------------------- */

/*
 * Make a label as wide as the widest text it will show, so the menu does not
 * grow when the value reaches 100% (digits differ in width in most fonts)
 */
static void
fixed_width(GtkWidget *label, const char *widest)
{
	PangoLayout *layout = gtk_widget_create_pango_layout(label, widest);
	int w;

	pango_layout_get_pixel_size(layout, &w, NULL);
	gtk_widget_set_size_request(label, w, -1);
	g_object_unref(layout);
}

/* A menu item holding a row of widgets */
static GtkWidget *
menu_row(GtkWidget *menu, GtkWidget *row, const char *tip)
{
	GtkWidget *item = gtk_menu_item_new();

	gtk_container_add(GTK_CONTAINER(item), row);
	gtk_widget_set_tooltip_text(item, tip);
	gtk_widget_show_all(item);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	return item;
}

/*
 * Icon, slider, percent; no name, as in the pulseaudio plugin's menu: the
 * icon says what it is, and the menu stays narrow
 */
static GtkWidget *
slider_row(GtkWidget *menu, const char *icon, const char *tip,
    GtkWidget **scale, GtkWidget **label)
{
	GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
	/* Less than the other rows: the slider has room of its own at its ends */
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

	*scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100,
	    1);
	gtk_scale_set_draw_value(GTK_SCALE(*scale), FALSE);
	gtk_widget_set_size_request(*scale, 140, -1);
	gtk_widget_set_hexpand(*scale, TRUE);
	*label = gtk_label_new("0%");
	gtk_label_set_xalign(GTK_LABEL(*label), 1.0);
	fixed_width(*label, "100%");

	gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), *scale, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row), *label, FALSE, FALSE, 0);
	return menu_row(menu, row, tip);
}

/* Icon, name, switch */
static GtkWidget *
switch_row(GtkWidget *menu, const char *name, const char *tip,
    GtkWidget **icon, GtkWidget **sw)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *label = gtk_label_new(name);

	*icon = gtk_image_new();
	*sw = gtk_switch_new();
	gtk_widget_set_valign(*sw, GTK_ALIGN_CENTER);

	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_box_pack_start(GTK_BOX(row), *icon, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(row), *sw, FALSE, FALSE, 0);
	return menu_row(menu, row, tip);
}

static void
build_menu(Panel *p)
{
	p->menu = gtk_menu_new();
	gtk_menu_attach_to_widget(GTK_MENU(p->menu), p->button, NULL);
	/* No room for check marks on the left: our items have their own icons */
	gtk_menu_set_reserve_toggle_size(GTK_MENU(p->menu), FALSE);

	p->bright_row = slider_row(p->menu, ICON_BRIGHTNESS,
	    "Brightness\nbacklight(9): " HW_BACKLIGHT_DEV,
	    &p->bright_scale, &p->bright_label);
	/* Display apart from sound, as the pulseaudio plugin's groups */
	p->bright_sep = gtk_separator_menu_item_new();
	gtk_menu_shell_append(GTK_MENU_SHELL(p->menu), p->bright_sep);
	p->speaker_row = switch_row(p->menu, "Speaker",
	    "Hardware mute by the embedded controller (ACPI SSMS),\n"
	    "same as the mute key; LED included",
	    &p->speaker_icon, &p->speaker_switch);
	p->mic_row = switch_row(p->menu, "Microphone",
	    "Recording level of all sound devices, same as the mic-mute key; "
	    "LED included",
	    &p->mic_icon, &p->mic_switch);

	g_signal_connect(p->bright_scale, "value-changed",
	    G_CALLBACK(on_brightness), p);
	gtk_widget_add_events(p->bright_row, GDK_SCROLL_MASK);
	g_signal_connect(p->bright_row, "button-press-event",
	    G_CALLBACK(on_slider_item_press), p);
	g_signal_connect(p->bright_row, "button-release-event",
	    G_CALLBACK(on_slider_item_release), p);
	g_signal_connect(p->bright_row, "scroll-event",
	    G_CALLBACK(on_slider_item_scroll), p);

	g_signal_connect(p->speaker_switch, "notify::active",
	    G_CALLBACK(on_speaker), p);
	g_signal_connect(p->speaker_row, "button-release-event",
	    G_CALLBACK(on_switch_item_release), p->speaker_switch);
	g_signal_connect(p->speaker_row, "activate",
	    G_CALLBACK(on_switch_item_activate), p->speaker_switch);
	g_signal_connect(p->mic_switch, "notify::active", G_CALLBACK(on_mic),
	    p);
	g_signal_connect(p->mic_row, "button-release-event",
	    G_CALLBACK(on_switch_item_release), p->mic_switch);
	g_signal_connect(p->mic_row, "activate",
	    G_CALLBACK(on_switch_item_activate), p->mic_switch);

	g_signal_connect(p->menu, "hide", G_CALLBACK(on_menu_hide), p);
}

/* ---- plugin ------------------------------------------------------------- */

static gboolean
on_size_changed(XfcePanelPlugin *plugin, gint size, Panel *p)
{
	size /= xfce_panel_plugin_get_nrows(plugin);
	gtk_widget_set_size_request(p->button, size, size);
	gtk_image_set_pixel_size(GTK_IMAGE(p->icon),
	    xfce_panel_plugin_get_icon_size(plugin));
	return TRUE;
}

static void
on_free(XfcePanelPlugin *plugin, Panel *p)
{
	(void)plugin;
	ctl_free(p->ctl);
	gtk_widget_destroy(p->menu);
	g_free(p->tip);
	g_free(p);
}

/*
 * Debug messages (g_debug(), log domain "bsdthinkpad-plugin") are off unless
 * asked for, as with the other panel plugins: PANEL_DEBUG=all or
 * PANEL_DEBUG=bsdthinkpad-plugin (a comma separated list, as the panel
 * takes it), or GLib's own G_MESSAGES_DEBUG=bsdthinkpad-plugin.  They go to
 * the panel's standard error.
 */
static void
init_debug(void)
{
	const char *env = g_getenv("PANEL_DEBUG");
	char **domains, *value;
	int i;

	if (env == NULL)
		return;
	domains = g_strsplit(env, ",", -1);
	for (i = 0; domains[i] != NULL; i++) {
		g_strstrip(domains[i]);
		if (g_str_equal(domains[i], "all") ||
		    g_str_equal(domains[i], G_LOG_DOMAIN)) {
			value = g_strjoin(" ", G_LOG_DOMAIN,
			    g_getenv("G_MESSAGES_DEBUG"), NULL);
#if GLIB_CHECK_VERSION(2, 80, 0)
			/*
			 * GLib reads G_MESSAGES_DEBUG once, at the first
			 * message, and the wrapper process has logged already
			 */
			{
				char **list = g_strsplit(value, " ", -1);

				g_log_writer_default_set_debug_domains(
				    (const char * const *)list);
				g_strfreev(list);
			}
#else
			g_setenv("G_MESSAGES_DEBUG", value, TRUE);
#endif
			g_free(value);
			break;
		}
	}
	g_strfreev(domains);
}

static void
construct(XfcePanelPlugin *plugin)
{
	Panel *p = g_new0(Panel, 1);

	init_debug();
	g_debug("version %s", VERSION);

	p->plugin = plugin;
	p->button = xfce_panel_create_toggle_button();
	p->icon = gtk_image_new_from_icon_name(ICON_BRIGHTNESS,
	    GTK_ICON_SIZE_BUTTON);
	gtk_container_add(GTK_CONTAINER(p->button), p->icon);
	g_signal_connect(p->button, "button-press-event",
	    G_CALLBACK(on_button_press), p);
	g_signal_connect(p->button, "enter-notify-event", G_CALLBACK(on_enter),
	    p);
	g_signal_connect(p->button, "leave-notify-event", G_CALLBACK(on_leave),
	    p);
	/*
	 * On the button itself, which receives the pointer's events.  Not
	 * GDK_SMOOTH_SCROLL_MASK: in the panel every wheel notch comes with a
	 * leave and enter, which reset GDK's scroll valuators, so smooth events
	 * carry no delta (0, is_stop).  Without it, GDK passes the X wheel
	 * buttons on as GDK_SCROLL_UP/DOWN.
	 */
	gtk_widget_add_events(p->button, GDK_SCROLL_MASK);
	g_signal_connect(p->button, "scroll-event", G_CALLBACK(on_scroll), p);

	gtk_container_add(GTK_CONTAINER(plugin), p->button);
	xfce_panel_plugin_add_action_widget(plugin, p->button);
	xfce_panel_plugin_set_small(plugin, TRUE);

	build_menu(p);
	/* Widgets exist now: the first "changed" renders everything */
	p->ctl = ctl_new(render, p);

	g_signal_connect(plugin, "size-changed", G_CALLBACK(on_size_changed),
	    p);
	g_signal_connect(plugin, "free-data", G_CALLBACK(on_free), p);
	gtk_widget_show_all(p->button);
}

XFCE_PANEL_PLUGIN_REGISTER(construct);
