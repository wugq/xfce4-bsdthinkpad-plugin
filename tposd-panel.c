/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, wugq
 *
 * tposd-panel -- XFCE panel plugin for what the pulseaudio plugin can not do
 * on FreeBSD:
 *
 *   brightness   slider (scroll on the panel icon too)
 *   PCM (OSS)    slider: the OSS "pcm" level of the default mixer
 *   speaker      switch: the ThinkPad hardware mute (same as the mute key),
 *                LED included
 *   microphone   switch: the recording level of all sound devices (same as
 *                the mic-mute key), LED included
 *
 * Volume is the pulseaudio plugin's job: its sink volume is the OSS "vol" of
 * the sound card.  It does not reliably manage "pcm", which still limits
 * what you hear; hence the PCM slider.  The tooltip shows both levels and,
 * like the pulseaudio plugin's, follows the keys while the pointer is on the
 * icon.
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
#define SCROLL_STEP	5

typedef struct {
	XfcePanelPlugin	*plugin;
	Ctl		*ctl;

	GtkWidget	*button, *icon;
	/*
	 * A window placed next to the button (a GtkPopover would be clipped to
	 * the plugin's small window).  Like the pulseaudio plugin's popup it
	 * grabs keyboard and pointer while open; a click outside closes it,
	 * and closing always releases the grab.
	 */
	GtkWidget	*popup;
	gboolean	 grabbed;
	gboolean	 hovering;

	GtkWidget	*bright_row, *bright_scale, *bright_label;
	GtkWidget	*pcm_row, *pcm_scale, *pcm_label;
	GtkWidget	*speaker_row, *speaker_icon, *speaker_switch;
	GtkWidget	*mic_row, *mic_icon, *mic_switch;

	CtlState	 shown;		/* what the widgets show */
	gboolean	 shown_valid;
	char		*tip;		/* current tooltip text */
} Panel;

/* ---- view: state -> widgets ------------------------------------------------------ */

static void on_brightness(GtkRange *, Panel *);
static void on_pcm(GtkRange *, Panel *);
static void on_speaker(GObject *, GParamSpec *, Panel *);
static void on_mic(GObject *, GParamSpec *, Panel *);

static const char *
brightness_icon(int v)
{
	if (v < 34)
		return "display-brightness-low-symbolic";
	if (v < 67)
		return "display-brightness-medium-symbolic";
	return "display-brightness-high-symbolic";
}

static void
set_percent(GtkWidget *label, int v)
{
	char text[16];

	snprintf(text, sizeof(text), "%d%%", v);
	gtk_label_set_text(GTK_LABEL(label), text);
}

/* A slider the user is dragging is left alone */
static void
render_slider(Panel *p, GtkWidget *row, GtkWidget *scale, GtkWidget *label,
    gpointer handler, int v)
{
	gtk_widget_set_visible(row, v >= 0);
	if (v < 0 || gtk_widget_has_grab(scale))
		return;
	g_signal_handlers_block_by_func(scale, handler, p);
	gtk_range_set_value(GTK_RANGE(scale), v);
	g_signal_handlers_unblock_by_func(scale, handler, p);
	set_percent(label, v);
}

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
render_tooltip(Panel *p, const CtlState *s)
{
	GString *tip = g_string_new(NULL);

	if (s->brightness >= 0)
		g_string_append_printf(tip, "Brightness %d%%", s->brightness);
	if (s->speaker_mute >= 0)
		g_string_append_printf(tip, "\nSpeaker %s",
		    s->speaker_mute ? "off" : "on");
	if (s->mic_mute >= 0)
		g_string_append_printf(tip, "%sMicrophone %s",
		    s->speaker_mute >= 0 ? " · " : "\n",
		    s->mic_mute ? "off" : "on");
	if (s->oss_unit >= 0) {
		g_string_append_printf(tip, "\nOSS mixer%d:", s->oss_unit);
		if (s->oss_vol >= 0)
			g_string_append_printf(tip, " vol %.2f", s->oss_vol);
		if (s->oss_pcm >= 0)
			g_string_append_printf(tip, "%s pcm %.2f",
			    s->oss_vol >= 0 ? " ·" : "", s->oss_pcm);
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

	if (all || s->brightness != o->brightness) {
		gtk_image_set_from_icon_name(GTK_IMAGE(p->icon),
		    brightness_icon(s->brightness < 0 ? 100 : s->brightness),
		    GTK_ICON_SIZE_BUTTON);
		render_slider(p, p->bright_row, p->bright_scale,
		    p->bright_label, on_brightness, s->brightness);
	}
	if (all || s->pcm != o->pcm)
		render_slider(p, p->pcm_row, p->pcm_scale, p->pcm_label,
		    on_pcm, s->pcm);
	if (all || s->speaker_mute != o->speaker_mute)
		render_switch(p, p->speaker_row, p->speaker_icon,
		    p->speaker_switch, on_speaker, s->speaker_mute,
		    "audio-volume-high-symbolic", "audio-volume-muted-symbolic");
	if (all || s->mic_mute != o->mic_mute)
		render_switch(p, p->mic_row, p->mic_icon, p->mic_switch,
		    on_mic, s->mic_mute, "audio-input-microphone-symbolic",
		    "microphone-sensitivity-muted-symbolic");
	render_tooltip(p, s);

	*o = *s;
	p->shown_valid = TRUE;
}

/* ---- user actions -> ctl ---------------------------------------------------------- */

static void
on_brightness(GtkRange *range, Panel *p)
{
	set_percent(p->bright_label, (int)gtk_range_get_value(range));
	ctl_set_brightness(p->ctl, (int)gtk_range_get_value(range));
}

static void
on_pcm(GtkRange *range, Panel *p)
{
	set_percent(p->pcm_label, (int)gtk_range_get_value(range));
	ctl_set_pcm(p->ctl, (int)gtk_range_get_value(range));
}

/* Switch on = sound on */
static void
on_speaker(GObject *sw, GParamSpec *pspec, Panel *p)
{
	(void)pspec;
	ctl_set_speaker_mute(p->ctl, !gtk_switch_get_active(GTK_SWITCH(sw)));
	/* Refused (e.g. one already pending): put the switch back */
	render_switch(p, p->speaker_row, p->speaker_icon, p->speaker_switch,
	    on_speaker, ctl_state(p->ctl)->speaker_mute,
	    "audio-volume-high-symbolic", "audio-volume-muted-symbolic");
}

/* Switch on = microphone on */
static void
on_mic(GObject *sw, GParamSpec *pspec, Panel *p)
{
	(void)pspec;
	ctl_set_mic_mute(p->ctl, !gtk_switch_get_active(GTK_SWITCH(sw)));
}

/* Scroll on the panel icon: brightness up/down */
static gboolean
on_scroll(GtkWidget *w, GdkEventScroll *ev, Panel *p)
{
	(void)w;
	if (ev->direction == GDK_SCROLL_UP)
		ctl_step_brightness(p->ctl, SCROLL_STEP);
	else if (ev->direction == GDK_SCROLL_DOWN)
		ctl_step_brightness(p->ctl, -SCROLL_STEP);
	else
		return FALSE;
	return TRUE;
}

/* Poll while the popup is open or the pointer is on the icon */
static void
update_polling(Panel *p)
{
	ctl_set_polling(p->ctl,
	    p->hovering || gtk_widget_get_visible(p->popup) ? POLL_MS : 0);
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

/* ---- popup window ------------------------------------------------------------------- */

static void
popup_show(Panel *p)
{
	gint x, y;

	xfce_panel_plugin_block_autohide(p->plugin, TRUE);
	gtk_widget_realize(p->popup);
	xfce_panel_plugin_position_widget(p->plugin, p->popup, p->button,
	    &x, &y);
	gtk_window_move(GTK_WINDOW(p->popup), x, y);
	gtk_window_present_with_time(GTK_WINDOW(p->popup),
	    gtk_get_current_event_time());
	update_polling(p);
}

static void
popup_hide(Panel *p)
{
	if (p->grabbed) {
		gdk_seat_ungrab(gdk_display_get_default_seat(
		    gtk_widget_get_display(p->popup)));
		p->grabbed = FALSE;
	}
	if (gtk_widget_get_visible(p->popup)) {
		gtk_widget_hide(p->popup);
		xfce_panel_plugin_block_autohide(p->plugin, FALSE);
	}
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->button)))
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->button),
		    FALSE);
	update_polling(p);
}

static void
on_toggled(GtkToggleButton *button, Panel *p)
{
	if (gtk_toggle_button_get_active(button))
		popup_show(p);
	else
		popup_hide(p);
}

/* Grab once the window is on screen (a grab needs a viewable window) */
static gboolean
on_popup_map(GtkWidget *w, GdkEvent *ev, Panel *p)
{
	GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(w));

	(void)ev;
	p->grabbed = gdk_seat_grab(seat, gtk_widget_get_window(w),
	    GDK_SEAT_CAPABILITY_ALL, TRUE, NULL, NULL, NULL, NULL) ==
	    GDK_GRAB_SUCCESS;
	return FALSE;
}

/*
 * With the grab, a click on another program arrives at the popup's own
 * window, outside its area: close.  (Clicks on our widgets go to them, and a
 * click on the panel button toggles it off.)
 */
static gboolean
on_popup_button(GtkWidget *w, GdkEventButton *ev, Panel *p)
{
	if (ev->window != gtk_widget_get_window(w) ||
	    (ev->x >= 0 && ev->y >= 0 &&
	    ev->x < gtk_widget_get_allocated_width(w) &&
	    ev->y < gtk_widget_get_allocated_height(w)))
		return FALSE;
	popup_hide(p);
	return TRUE;
}

/* Another program took the grab away (e.g. a menu): close */
static gboolean
on_popup_grab_broken(GtkWidget *w, GdkEventGrabBroken *ev, Panel *p)
{
	(void)w;
	if (ev->grab_window == NULL) {
		p->grabbed = FALSE;
		popup_hide(p);
	}
	return FALSE;
}

static gboolean
on_popup_key(GtkWidget *w, GdkEventKey *ev, Panel *p)
{
	(void)w;
	if (ev->keyval != GDK_KEY_Escape)
		return FALSE;
	popup_hide(p);
	return TRUE;
}

static gboolean
on_popup_delete(GtkWidget *w, GdkEvent *ev, Panel *p)
{
	(void)w;
	(void)ev;
	popup_hide(p);
	return TRUE;
}

/* ---- building the widgets ------------------------------------------------------------ */

/*
 * Make a label as wide as the widest text it will show, so the popup does not
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

/* Name column, the same width in every row */
static GtkWidget *
name_label(GtkSizeGroup *names, const char *name)
{
	GtkWidget *label = gtk_label_new(name);

	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_size_group_add_widget(names, label);
	return label;
}

/* Icon, name, slider, percent */
static GtkWidget *
slider_row(GtkGrid *grid, int top, GtkSizeGroup *names, const char *icon,
    const char *name, const char *tip, GtkWidget **scale, GtkWidget **label)
{
	GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

	*scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
	gtk_scale_set_draw_value(GTK_SCALE(*scale), FALSE);
	gtk_widget_set_size_request(*scale, 200, -1);
	gtk_widget_set_hexpand(*scale, TRUE);
	*label = gtk_label_new("0%");
	gtk_label_set_xalign(GTK_LABEL(*label), 1.0);
	fixed_width(*label, "100%");

	gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), name_label(names, name),
	    FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), *scale, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row), *label, FALSE, FALSE, 0);
	gtk_widget_set_tooltip_text(row, tip);
	gtk_grid_attach(grid, row, 0, top, 1, 1);
	return row;
}

/* Icon, name, switch */
static GtkWidget *
switch_row(GtkGrid *grid, int top, GtkSizeGroup *names, const char *name,
    const char *tip, GtkWidget **icon, GtkWidget **sw)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *label = name_label(names, name);

	*icon = gtk_image_new();
	*sw = gtk_switch_new();
	gtk_widget_set_valign(*sw, GTK_ALIGN_CENTER);

	gtk_box_pack_start(GTK_BOX(row), *icon, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(row), *sw, FALSE, FALSE, 0);
	gtk_widget_set_tooltip_text(row, tip);
	gtk_grid_attach(grid, row, 0, top, 1, 1);
	return row;
}

static void
build_popup(Panel *p)
{
	GtkWidget *grid;
	GtkSizeGroup *names = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

	p->popup = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_decorated(GTK_WINDOW(p->popup), FALSE);
	gtk_window_set_resizable(GTK_WINDOW(p->popup), FALSE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(p->popup), TRUE);
	gtk_window_set_skip_pager_hint(GTK_WINDOW(p->popup), TRUE);
	gtk_window_set_keep_above(GTK_WINDOW(p->popup), TRUE);
	gtk_window_set_type_hint(GTK_WINDOW(p->popup),
	    GDK_WINDOW_TYPE_HINT_UTILITY);

	grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
	g_object_set(grid, "margin", 12, NULL);

	p->bright_row = slider_row(GTK_GRID(grid), 0, names,
	    "display-brightness-symbolic", "Brightness",
	    "backlight(9): " HW_BACKLIGHT_DEV,
	    &p->bright_scale, &p->bright_label);
	p->pcm_row = slider_row(GTK_GRID(grid), 1, names,
	    "audio-card-symbolic", "PCM (OSS)",
	    "OSS \"pcm\" level of the default mixer, as mixer(8) pcm.volume.\n"
	    "Not managed by PulseAudio, whose volume is the OSS \"vol\".\n"
	    "Usually left at 100%.", &p->pcm_scale, &p->pcm_label);
	p->speaker_row = switch_row(GTK_GRID(grid), 2, names, "Speaker",
	    "Hardware mute by the embedded controller (ACPI SSMS),\n"
	    "same as the mute key; LED included",
	    &p->speaker_icon, &p->speaker_switch);
	p->mic_row = switch_row(GTK_GRID(grid), 3, names, "Microphone",
	    "Recording level of all sound devices, same as the mic-mute key; "
	    "LED included",
	    &p->mic_icon, &p->mic_switch);
	g_object_unref(names);

	g_signal_connect(p->bright_scale, "value-changed",
	    G_CALLBACK(on_brightness), p);
	g_signal_connect(p->pcm_scale, "value-changed", G_CALLBACK(on_pcm), p);
	g_signal_connect(p->speaker_switch, "notify::active",
	    G_CALLBACK(on_speaker), p);
	g_signal_connect(p->mic_switch, "notify::active", G_CALLBACK(on_mic),
	    p);

	gtk_container_add(GTK_CONTAINER(p->popup), grid);
	gtk_widget_show_all(grid);
	gtk_widget_add_events(p->popup, GDK_BUTTON_PRESS_MASK);
	g_signal_connect(p->popup, "map-event", G_CALLBACK(on_popup_map), p);
	g_signal_connect(p->popup, "button-press-event",
	    G_CALLBACK(on_popup_button), p);
	g_signal_connect(p->popup, "grab-broken-event",
	    G_CALLBACK(on_popup_grab_broken), p);
	g_signal_connect(p->popup, "key-press-event", G_CALLBACK(on_popup_key),
	    p);
	g_signal_connect(p->popup, "delete-event", G_CALLBACK(on_popup_delete),
	    p);
}

/* ---- plugin --------------------------------------------------------------------------- */

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
	gtk_widget_destroy(p->popup);
	g_free(p->tip);
	g_free(p);
}

static void
construct(XfcePanelPlugin *plugin)
{
	Panel *p = g_new0(Panel, 1);

	p->plugin = plugin;
	p->button = xfce_panel_create_toggle_button();
	p->icon = gtk_image_new();
	gtk_container_add(GTK_CONTAINER(p->button), p->icon);
	g_signal_connect(p->button, "toggled", G_CALLBACK(on_toggled), p);
	g_signal_connect(p->button, "enter-notify-event", G_CALLBACK(on_enter),
	    p);
	g_signal_connect(p->button, "leave-notify-event", G_CALLBACK(on_leave),
	    p);
	gtk_widget_add_events(GTK_WIDGET(plugin), GDK_SCROLL_MASK);
	g_signal_connect(plugin, "scroll-event", G_CALLBACK(on_scroll), p);

	gtk_container_add(GTK_CONTAINER(plugin), p->button);
	xfce_panel_plugin_add_action_widget(plugin, p->button);
	xfce_panel_plugin_set_small(plugin, TRUE);

	build_popup(p);
	/* Widgets exist now: the first "changed" renders everything */
	p->ctl = ctl_new(render, p);

	g_signal_connect(plugin, "size-changed", G_CALLBACK(on_size_changed), p);
	g_signal_connect(plugin, "free-data", G_CALLBACK(on_free), p);
	gtk_widget_show_all(p->button);
}

XFCE_PANEL_PLUGIN_REGISTER(construct);
