/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, wugq
 *
 * tposd-panel -- XFCE panel plugin for what the pulseaudio plugin can not do
 * on FreeBSD:
 *
 *   brightness   slider (scroll on the panel icon too)
 *   speaker      switch: the ThinkPad hardware mute (same as the mute key),
 *                LED included
 *   microphone   switch; the LED follows
 *
 * Volume is the pulseaudio plugin's job: its sink volume is the OSS "vol" of
 * the sound card.  The tooltip shows the OSS "vol" and "pcm" levels, read
 * only, since pulseaudio does not reliably manage "pcm".
 *
 * Like the pulseaudio plugin, the tooltip follows changes made with the keys
 * while the pointer is on the icon.
 */

#include <stdio.h>

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

#include "hw.h"

#define REFRESH_MS	300
#define SCROLL_STEP	5

typedef struct {
	XfcePanelPlugin	*plugin;
	GtkWidget	*button, *icon;
	/*
	 * A plain window placed next to the button.  Not a GtkPopover (it is
	 * clipped to the plugin's small window) and no keyboard grab (it would
	 * stop the brightness and volume keys); it closes when it loses focus.
	 */
	GtkWidget	*popup;
	GtkWidget	*bright_row, *bright_scale, *bright_label;
	GtkWidget	*speaker_row, *speaker_icon, *speaker_switch;
	GtkWidget	*mic_row, *mic_icon, *mic_switch;
	guint		 timer;		/* refresh while the popup is open */
	guint		 hover_timer;	/* tooltip while the pointer is on it */
	char		*tip;		/* current tooltip text */
	gint64		 hidden_at;	/* when the popup last closed */
	gboolean	 updating;	/* setting widgets from the hardware */
} Panel;

/* ---- showing the current state --------------------------------------------- */

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

/* Panel icon and its tooltip */
static void
update_button(Panel *p)
{
	int b = hw_get_brightness(), hw = hw_get_hwmute();
	int mic = hw_get_micmute(), unit;
	float vol, pcm;
	GString *tip = g_string_new(NULL);

	gtk_image_set_from_icon_name(GTK_IMAGE(p->icon),
	    brightness_icon(b < 0 ? 100 : b), GTK_ICON_SIZE_BUTTON);
	gtk_image_set_pixel_size(GTK_IMAGE(p->icon),
	    xfce_panel_plugin_get_icon_size(p->plugin));

	if (b >= 0)
		g_string_append_printf(tip, "Brightness %d%%", b);
	if (hw >= 0)
		g_string_append_printf(tip, "\nSpeaker %s", hw ? "off" : "on");
	if (mic >= 0)
		g_string_append_printf(tip, "%sMicrophone %s",
		    hw >= 0 ? " · " : "\n", mic ? "off" : "on");
	if (hw_get_oss_levels(&unit, &vol, &pcm) == 0) {
		g_string_append_printf(tip, "\nOSS mixer%d:", unit);
		if (vol >= 0)
			g_string_append_printf(tip, " vol %.2f", vol);
		if (pcm >= 0)
			g_string_append_printf(tip, "%s pcm %.2f",
			    vol >= 0 ? " ·" : "", pcm);
	}
	/* Only when it changed, so a visible tooltip does not flicker */
	if (g_strcmp0(tip->str, p->tip) != 0) {
		gtk_widget_set_tooltip_text(p->button, tip->str);
		g_free(p->tip);
		p->tip = g_strdup(tip->str);
	}
	g_string_free(tip, TRUE);
}

static void
show_slider(GtkWidget *row, GtkWidget *scale, GtkWidget *label, int v)
{
	gtk_widget_set_visible(row, v >= 0);
	if (v >= 0) {
		gtk_range_set_value(GTK_RANGE(scale), v);
		set_percent(label, v);
	}
}

/* muted: 0/1, or -1 when not available */
static void
show_switch(GtkWidget *row, GtkWidget *icon, GtkWidget *sw, int muted,
    const char *icon_on, const char *icon_off)
{
	gtk_widget_set_visible(row, muted >= 0);
	if (muted >= 0) {
		gtk_switch_set_active(GTK_SWITCH(sw), !muted);
		gtk_switch_set_state(GTK_SWITCH(sw), !muted);
		gtk_image_set_from_icon_name(GTK_IMAGE(icon),
		    muted ? icon_off : icon_on, GTK_ICON_SIZE_MENU);
	}
}

/* Popup widgets from the hardware; also called by the timer while open */
static gboolean
refresh(gpointer data)
{
	Panel *p = data;

	p->updating = TRUE;
	show_slider(p->bright_row, p->bright_scale, p->bright_label,
	    hw_get_brightness());
	show_switch(p->speaker_row, p->speaker_icon, p->speaker_switch,
	    hw_get_hwmute(), "audio-volume-high-symbolic",
	    "audio-volume-muted-symbolic");
	show_switch(p->mic_row, p->mic_icon, p->mic_switch, hw_get_micmute(),
	    "audio-input-microphone-symbolic",
	    "microphone-sensitivity-muted-symbolic");
	p->updating = FALSE;
	update_button(p);
	return G_SOURCE_CONTINUE;
}

/* ---- changing the hardware ----------------------------------------------------- */
/* The refresh timer brings icons (and a switch whose change failed) in step. */

static void
on_brightness(GtkRange *range, Panel *p)
{
	int v = (int)gtk_range_get_value(range);

	set_percent(p->bright_label, v);
	if (!p->updating)
		hw_set_brightness(v);
}

/* Switch on = sound on.  SSMS mutes and sets the LED together. */
static gboolean
on_speaker(GtkSwitch *sw, gboolean active, Panel *p)
{
	if (!p->updating)
		hw_set_hwmute(!active);
	gtk_switch_set_state(sw, active);
	return TRUE;
}

/* Switch on = microphone on.  The LED is set here and by tposd. */
static gboolean
on_mic(GtkSwitch *sw, gboolean active, Panel *p)
{
	if (!p->updating && hw_set_micmute(!active) == 0)
		hw_set_led("mic", !active);
	gtk_switch_set_state(sw, active);
	return TRUE;
}

/* Scroll on the panel icon: brightness up/down */
static gboolean
on_scroll(GtkWidget *w, GdkEventScroll *ev, Panel *p)
{
	int b = hw_get_brightness(), step = 0;

	(void)w;
	if (b < 0)
		return FALSE;
	if (ev->direction == GDK_SCROLL_UP)
		step = SCROLL_STEP;
	else if (ev->direction == GDK_SCROLL_DOWN)
		step = -SCROLL_STEP;
	if (step == 0)
		return FALSE;
	hw_set_brightness(b + step);
	update_button(p);
	return TRUE;
}

/* ---- tooltip while hovering ------------------------------------------------------- */

static gboolean
hover_tick(gpointer data)
{
	update_button(data);
	return G_SOURCE_CONTINUE;
}

static gboolean
on_enter(GtkWidget *w, GdkEventCrossing *ev, Panel *p)
{
	(void)w;
	(void)ev;
	update_button(p);
	if (p->hover_timer == 0)
		p->hover_timer = g_timeout_add(REFRESH_MS, hover_tick, p);
	return FALSE;
}

static gboolean
on_leave(GtkWidget *w, GdkEventCrossing *ev, Panel *p)
{
	(void)w;
	(void)ev;
	if (p->hover_timer != 0) {
		g_source_remove(p->hover_timer);
		p->hover_timer = 0;
	}
	return FALSE;
}

/* ---- popup window ------------------------------------------------------------------- */

static void
popup_show(Panel *p)
{
	gint x, y;

	refresh(p);
	xfce_panel_plugin_block_autohide(p->plugin, TRUE);
	gtk_widget_realize(p->popup);
	xfce_panel_plugin_position_widget(p->plugin, p->popup, p->button,
	    &x, &y);
	gtk_window_move(GTK_WINDOW(p->popup), x, y);
	gtk_window_present_with_time(GTK_WINDOW(p->popup),
	    gtk_get_current_event_time());
	if (p->timer == 0)
		p->timer = g_timeout_add(REFRESH_MS, refresh, p);
}

static void
popup_hide(Panel *p)
{
	if (p->timer != 0) {
		g_source_remove(p->timer);
		p->timer = 0;
	}
	if (gtk_widget_get_visible(p->popup)) {
		gtk_widget_hide(p->popup);
		p->hidden_at = g_get_monotonic_time();
	}
	xfce_panel_plugin_block_autohide(p->plugin, FALSE);
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->button)))
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->button),
		    FALSE);
	update_button(p);
}

static void
on_toggled(GtkToggleButton *button, Panel *p)
{
	if (!gtk_toggle_button_get_active(button)) {
		popup_hide(p);
		return;
	}
	/*
	 * Clicking the button while the popup is open first takes the focus
	 * away (closing it), then toggles the button: keep it closed.
	 */
	if (g_get_monotonic_time() - p->hidden_at < 300000) {
		gtk_toggle_button_set_active(button, FALSE);
		return;
	}
	popup_show(p);
}

static gboolean
on_popup_focus_out(GtkWidget *w, GdkEventFocus *ev, Panel *p)
{
	(void)w;
	(void)ev;
	popup_hide(p);
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

/* ---- building the popup ------------------------------------------------------------- */

/* Icon, slider, percent */
static GtkWidget *
slider_row(GtkGrid *grid, int top, const char *icon, const char *tip,
    GtkWidget **scale, GtkWidget **label)
{
	GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

	*scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
	gtk_scale_set_draw_value(GTK_SCALE(*scale), FALSE);
	gtk_widget_set_size_request(*scale, 200, -1);
	gtk_widget_set_hexpand(*scale, TRUE);
	*label = gtk_label_new("0%");
	gtk_label_set_width_chars(GTK_LABEL(*label), 4);
	gtk_label_set_xalign(GTK_LABEL(*label), 1.0);

	gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), *scale, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row), *label, FALSE, FALSE, 0);
	gtk_widget_set_tooltip_text(row, tip);
	gtk_grid_attach(grid, row, 0, top, 1, 1);
	return row;
}

/* Icon, name, switch */
static GtkWidget *
switch_row(GtkGrid *grid, int top, const char *name, const char *tip,
    GtkWidget **icon, GtkWidget **sw)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *label = gtk_label_new(name);

	*icon = gtk_image_new();
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
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

	p->bright_row = slider_row(GTK_GRID(grid), 0,
	    "display-brightness-symbolic", "Screen brightness",
	    &p->bright_scale, &p->bright_label);
	p->speaker_row = switch_row(GTK_GRID(grid), 1, "Speaker",
	    "Hardware mute, same as the mute key", &p->speaker_icon,
	    &p->speaker_switch);
	p->mic_row = switch_row(GTK_GRID(grid), 2, "Microphone",
	    "Microphone mute, same as the mic-mute key", &p->mic_icon,
	    &p->mic_switch);

	g_signal_connect(p->bright_scale, "value-changed",
	    G_CALLBACK(on_brightness), p);
	g_signal_connect(p->speaker_switch, "state-set",
	    G_CALLBACK(on_speaker), p);
	g_signal_connect(p->mic_switch, "state-set", G_CALLBACK(on_mic), p);

	gtk_container_add(GTK_CONTAINER(p->popup), grid);
	gtk_widget_show_all(grid);
	g_signal_connect(p->popup, "focus-out-event",
	    G_CALLBACK(on_popup_focus_out), p);
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
	update_button(p);
	return TRUE;
}

static void
on_free(XfcePanelPlugin *plugin, Panel *p)
{
	(void)plugin;
	if (p->timer != 0)
		g_source_remove(p->timer);
	if (p->hover_timer != 0)
		g_source_remove(p->hover_timer);
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
	update_button(p);

	g_signal_connect(plugin, "size-changed", G_CALLBACK(on_size_changed), p);
	g_signal_connect(plugin, "free-data", G_CALLBACK(on_free), p);
	gtk_widget_show_all(p->button);
}

XFCE_PANEL_PLUGIN_REGISTER(construct);
