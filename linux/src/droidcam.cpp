/* DroidCam & DroidCamX (C) 2010-
 * https://github.com/aramg
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * Use at your own risk. See README file for more details.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <cerrno>
extern "C" {
	#include <sys/stat.h>
	#include <gtk/gtk.h>
}


#include "droidcam.h"
#include "common.h"
#include "connection.h"
#include "decoder.h"
#include "icon.h"
#include "context.h"
#include "ipcam.h"
#include "util.h"


/* Globals */
GtkWidget *menu;
GThread *hVideoThread;
Callback thread_cmd = CB_BUTTON; // doesn't really matter
int wifi_srvr_mode = 0;

/* Helper Functions */
void ShowError(const char *title, const char *msg)
{
	if (hVideoThread != nullptr)
		gdk_threads_enter();

	GtkWidget *dialog = gtk_message_dialog_new(nullptr, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
	                                           "%s", msg);
	gtk_window_set_title(GTK_WINDOW(dialog), title);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);

	if (hVideoThread != nullptr)
		gdk_threads_leave();
}

static int CheckAdbDevices(unsigned int port)
{
	char buf[256];
	int haveDevice = 0;

	system("adb start-server");
	FILE *pipe = popen("adb devices", "r");
	if (!pipe) {
		goto _exit;
	}

	while (!feof(pipe)) {
		dbgprint("->");
		if (fgets(buf, sizeof(buf), pipe) == nullptr) break;
		dbgprint("Got line: %s\n", buf);

		if (strstr(buf, "List of") != nullptr) {
			haveDevice = 2;
			continue;
		}
		if (haveDevice == 2) {
			if (strstr(buf, "offline") != nullptr) {
				haveDevice = 4;
				break;
			}
			if (strstr(buf, "device") != nullptr && strstr(buf, "??") == nullptr) {
				haveDevice = 8;
				break;
			}
		}
	}
	pclose(pipe);
#define TAIL "Please refer to the website for manual adb setup info."
	if (haveDevice == 0) {
		MSG_ERROR("adb program not detected. " TAIL);
	} else if (haveDevice == 2) {
		MSG_ERROR("No devices detected. " TAIL);
	} else if (haveDevice == 4) {
		system("adb kill-server");
		MSG_ERROR("Device is offline. Try re-attaching device.");
	} else if (haveDevice == 8) {
		sprintf(buf, "adb forward tcp:%d tcp:%d", port, port);
		system(buf);
	}
	_exit:
	dbgprint("haveDevice = %d\n", haveDevice);
	return haveDevice;
}

static void loadSettings(DCContext *context)
{
	char buf[PATH_MAX];
	struct stat st = {0};
	FILE *fp;

	int version;

	// Set Defaults
	context->settings.inputMode = IM_DROIDCAM;
	context->settings.connection = CB_RADIO_WIFI;
	context->settings.hostName = strdup("");
	context->settings.port = 4747;

	dbgprint("set defaults...\n");
	gtk_entry_set_text(GTK_ENTRY(context->ipEntry), "");
	gtk_entry_set_text(GTK_ENTRY(context->portEntry), "4747");


	snprintf(buf, sizeof(buf), "%s/.droidcam", getenv("HOME"));
	if (stat(buf, &st) == -1) {
		return; // don't create on load
	}

	snprintf(buf, sizeof(buf), "%s/.droidcam/settings", getenv("HOME"));
	fp = fopen(buf, "r");
	if (!fp) return;

	{
		version = 0;
		if (fgets(buf, sizeof(buf), fp)) {
			sscanf(buf, "v%d", &version);
		}

		if (version != 1 && version != 2) {
			return;
		}

		if (fgets(buf, sizeof(buf), fp)) {
			buf[strlen(buf) - 1] = '\0';

			FREE_OBJECT(context->settings.hostName, free);
			context->settings.hostName = strdup(buf);

		}

		if (fgets(buf, sizeof(buf), fp)) {
			buf[strlen(buf) - 1] = '\0';
			char *end;
			context->settings.port = strtol(buf, &end, 10);
			if (end==buf) { context->settings.port = 4747; }
		}

		if (fgets(buf, sizeof(buf), fp)) {
			buf[strlen(buf) - 1] = '\0';
			if (strcmp(buf, "ipcam") == 0) {
				context->settings.inputMode = IM_IPCAM;
			}
		}
	}

	if(context->settings.inputMode==IM_IPCAM) {
		gtk_toggle_button_set_state(GTK_TOGGLE_BUTTON(context->mode_ipcam), (!(0)));
	}

	gtk_entry_set_text(GTK_ENTRY(context->ipEntry), context->settings.hostName);

	char portText[8];
	memset(portText, 0, 8);
	snprintf(portText, 7, "%d", context->settings.port);
	gtk_entry_set_text(GTK_ENTRY(context->portEntry), portText);

	fclose(fp);
}

static void updateSettingsFromContext(DCContext *context) {
	Settings *settings = &(context->settings);
	char *end;
	const gchar *entryText = gtk_entry_get_text(GTK_ENTRY(context->portEntry));
	unsigned int portNo = strtol(entryText, &end, 10);
	if (end != entryText && *end == '\0' && errno != ERANGE) {
		settings->port = portNo;
	}

	const gchar *host = gtk_entry_get_text(GTK_ENTRY(context->ipEntry));

	if (settings->hostName == nullptr ||
	    strcmp(host, settings->hostName)!=0) {
		if(settings->hostName!=nullptr) { FREE_OBJECT(settings->hostName, free); }
		settings->hostName = strdup(host);
	}
}

static void saveSettings(Settings *settings)
{
	char buf[PATH_MAX];
	struct stat st = {0};
	FILE *fp;

	int version;

	// Set Defaults
	snprintf(buf, sizeof(buf), "%s/.droidcam", getenv("HOME"));
	if (stat(buf, &st) == -1) {
		mkdir(buf, 0700);
	}

	snprintf(buf, sizeof(buf), "%s/.droidcam/settings", getenv("HOME"));
	fp = fopen(buf, "w");
	if (!fp) return;

	{
		char portText[8];
		memset(portText, 0, 8);
		snprintf(portText, 7, "%d", settings->port);


		fprintf(fp, "v1\n%s\n%s\n%s\n",
		        settings->hostName,
		        portText,
		        settings->inputMode == IM_IPCAM ? "ipcam" : "droidcam");
	}
	fclose(fp);
}

/* Video Thread */
void *DroidcamVideoThreadProc(void *args)
{
	SOCKET videoSocket;
	DCContext *context;
	Decoder *decoder;
	{
		ThreadArgs *threadArgs = (ThreadArgs *) args;
		videoSocket = threadArgs->socket;
		context = threadArgs->context;
		decoder = context->decoder;

		delete args;
		args = 0;
	}
	char buf[32];
	int keep_waiting = 0;
	dbgprint("Video Thread Started s=%d\n", videoSocket);
	context->running = 1;

	server_wait:
	if (videoSocket == INVALID_SOCKET) {
		videoSocket = accept_connection(context->settings.port, &(context->running));
		if (videoSocket == INVALID_SOCKET) {
			throw DroidcamException("Invalid socket");
		}
		keep_waiting = 1;
	}

	if (decoder->outputMode() == OutputMode::OM_V4LLOOPBACK) {
		decoder->initLoopback(1280, 720);

	} else if (decoder->outputMode() != OutputMode::OM_DROIDCAM) {
		throw DroidcamException("Droidcam not in proper output mode");
	}

	int len = snprintf(buf, sizeof(buf), VIDEO_REQ, decoder->loopbackWidth(), decoder->loopbackHeight());
	if (sendToSocket(buf, len, videoSocket) <= 0) {
		MSG_ERROR("Error sending request, DroidCam might be busy with another client.");
		goto early_out;
	}


	memset(buf, 0, sizeof(buf));
	if (recvFromSocket(buf, 5, videoSocket) <= 0) {
		MSG_ERROR("Connection reset by app!\nDroidCam is probably busy with another client");
		goto early_out;
	}

	if (decoder->prepareVideo(buf) == false) {
		goto early_out;
	}

	while (context->running) {
		if (thread_cmd != CB_NONE) {
			int len = sprintf(buf, OTHER_REQ, thread_cmd -10);
			sendToSocket(buf, len, videoSocket);
			thread_cmd = CB_NONE;
		}

		Buffer *f = decoder->getNextFrame();
		if (recvFromSocket(buf, 4, videoSocket) == false) break;
		unsigned int frameLen = make_int4(buf[0], buf[1], buf[2], buf[3]);
		f->data_length = frameLen;
		char *p = (char *) f->data;
		while (frameLen > 4096) {
			if (recvFromSocket(p, 4096, videoSocket) == false) goto early_out;
			frameLen -= 4096;
			p += 4096;
		}
		if (recvFromSocket(p, frameLen, videoSocket) == false) break;
	}

	early_out:
	dbgprint("disconnect\n");
	disconnect(videoSocket);
	decoder->cleanupJpeg();

	if (context->running && keep_waiting) {
		videoSocket = INVALID_SOCKET;
		goto server_wait;
	}

	connection_cleanup();

	// gdk_threads_enter();
	// gtk_widget_set_sensitive(GTK_WIDGET(g_settings.button), TRUE);
	// gdk_threads_leave();
	dbgprint("Video Thread End\n");
	return 0;
}

static void StopVideo(DCContext *context)
{
	context->running = 0;
	if (hVideoThread != nullptr) {
		dbgprint("Waiting for videothread..\n");
		g_thread_join(hVideoThread);
		dbgprint("videothread joined\n");
		hVideoThread = nullptr;
		//gtk_widget_set_sensitive(GTK_WIDGET(g_settings.button), TRUE);
	}
}

/* Messages */
/*
static gint button_press_event(GtkWidget *widget, GdkEvent *event){
	if (event->type == GDK_BUTTON_PRESS){
		GdkEventButton *bevent = (GdkEventButton *) event;
		//if (bevent->button == 3)
		gtk_menu_popup (GTK_MENU(menu), nullptr, nullptr, nullptr, nullptr,
						bevent->button, bevent->time);
		return TRUE;
	}
	return false;
}
*/
static bool
accel_callback(GtkAccelGroup *group,
               GObject *obj,
               guint keyval,
               GdkModifierType mod,
               gpointer user_data)
{
/* TODO this is incorrect, that is not passed here
	DCContext *context = ((CallbackContext *) user_data)->context;
	if (context->running == 1 && thread_cmd == CB_NONE) {
		thread_cmd = static_cast<Callback>(reinterpret_cast<size_t>(user_data));
	}
*/
	return (!(0));
}

static void doConnect(DCContext *context)
{
#if 1
	updateSettingsFromContext(context);
	Settings *settings = &(context->settings);
	char *ip = nullptr;
	SOCKET droidcam_socket = INVALID_SOCKET;
	saveSettings(settings);

	if (settings->connection == CB_RADIO_ADB) {
		if (CheckAdbDevices(settings->port) != 8) return;
		ip = "127.0.0.1";
	} else if (settings->connection == CB_RADIO_WIFI && wifi_srvr_mode == 0) {
		ip = settings->hostName;
	}

	ThreadArgs *args = new ThreadArgs{ 0, ip, context };

	if (ip != nullptr) // Not Bluetooth or "Server Mode", so connect first
	{
		if (strlen(ip) < 7 || settings->port < 1024) {
			MSG_ERROR("You must enter the correct IP address (and port) to connect to.");
			return;
		}
		gtk_button_set_label(GTK_BUTTON(context->button), "Please wait");
		if (settings->inputMode == IM_DROIDCAM) {
			droidcam_socket = connect_droidcam(ip, settings->port);

			if (droidcam_socket == INVALID_SOCKET) {
				dbgprint("failed\n");
				gtk_button_set_label(GTK_BUTTON(context->button), "Connect");
				return;
			}
		}
		args->socket = droidcam_socket;
	}

/*
	args->settings = settings;
	ar
*/
	if (settings->inputMode == IM_DROIDCAM) {
		hVideoThread = g_thread_new("droidcam-process", DroidcamVideoThreadProc, args);
	} else {
		hVideoThread = g_thread_new("ipcam-process", ipcamVideoThreadProc, args);
	}


	gtk_button_set_label(GTK_BUTTON(context->button), "Stop");
	//gtk_widget_set_sensitive(GTK_WIDGET(settings->button), false);

	gtk_widget_set_sensitive(GTK_WIDGET(context->ipEntry), false);
	gtk_widget_set_sensitive(GTK_WIDGET(context->portEntry), false);
#else
	context->decoder->showTestImage();
#endif
}

static void doDisconnect(DCContext *context)
{
	StopVideo(context);
}

static void the_callback(GtkWidget *widget, CallbackContext *callbackContext)
{
	Callback cb = callbackContext->cb;
	DCContext *context = callbackContext->context;
	bool ipEdit = true;
	bool portEdit = true;
	char *text = nullptr;

	_up:
	dbgprint("the_cb=%d\n", cb);
	switch (cb) {
		case CB_BUTTON:
			if (context->running) {
				doDisconnect(context);
				cb = context->settings.connection;
				goto _up;
			} else {// START
				doConnect(context);
			}
			break;
		case CB_WIFI_SRVR:
			wifi_srvr_mode = !wifi_srvr_mode;
			if (context->settings.connection != CB_RADIO_WIFI)
				break;
			// else : fall through
		case CB_RADIO_WIFI:
			context->settings.connection = CB_RADIO_WIFI;
			if (wifi_srvr_mode) {
				text = "Prepare";
				ipEdit = false;
			} else {
				text = "Connect";
			}
			break;
		case CB_RADIO_BTH:
			context->settings.connection = CB_RADIO_BTH;
			text = "Prepare";
			ipEdit = false;
			portEdit = false;
			break;
		case CB_RADIO_ADB:
			context->settings.connection = CB_RADIO_ADB;
			text = "Connect";
			ipEdit = false;
			break;
		case CB_BTN_OTR:
			gtk_menu_popup(GTK_MENU(menu), nullptr, nullptr, nullptr, nullptr, 0, 0);
			break;
		case CB_CONTROL_ZIN  :
		case CB_CONTROL_ZOUT :
		case CB_CONTROL_AF   :
		case CB_CONTROL_LED  :
			if (context->running == 1 && thread_cmd == 0) {
				thread_cmd = cb;
			}
			break;
		case CB_AUDIO: {
			bool &a = context->settings.audio;
			a = !a;
			break;
		}
		case CB_MODE_DROIDCAM:
			if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) {
				context->settings.inputMode = IM_DROIDCAM;
			}
			break;
		case CB_MODE_IPCAM:
			if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) {
				context->settings.inputMode = IM_IPCAM;
			}
			break;
		case CB_QUIT:
			updateSettingsFromContext(context);
			saveSettings(&context->settings);
			gtk_main_quit();
			break;
	}

	if(context->settings.inputMode == IM_DROIDCAM) {
		gtk_widget_set_sensitive(context->radioServer, true);
	} else {
		if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(context->radioServer))) {
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(context->radioServer), false);
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(context->radioClient), true);
		}
		gtk_widget_set_sensitive(context->radioServer, false);
	}

	if (text != nullptr && context->running == 0) {
		gtk_button_set_label(GTK_BUTTON(context->button), text);
		gtk_widget_set_sensitive(GTK_WIDGET(context->ipEntry), ipEdit);
		gtk_widget_set_sensitive(GTK_WIDGET(context->portEntry), portEdit);
	}
}

static CallbackContext *cbContext(DCContext *context, Callback cb)
{
	CallbackContext *result = &context->callbackData[cb];
	*result = (CallbackContext) {context, cb};
	return result;
}

int main(int argc, char *argv[])
{
	Decoder decoder = {};
	DCContext context = {
		.settings = {},
		.button=nullptr,
		.running=false,
		.decoder = &decoder
	};


	if ((&decoder)->init()) {

		GtkWidget *window;
		GtkWidget *hbox, *hbox2, *hbox3;
		GtkWidget *vbox, *vbox2;
//		GtkWidget *widget; // generic stuff
//		GtkWidget *mode_ipcam, *mode_droidcam;

		// init threads
		g_thread_init(nullptr);
		gdk_threads_init();
		gtk_init(&argc, &argv);

		window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
		gtk_window_set_title(GTK_WINDOW(window), "DroidCam Client");
		gtk_container_set_border_width(GTK_CONTAINER(window), 1);
		gtk_window_set_resizable(GTK_WINDOW(window), false);
		gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_NONE);
		gtk_container_set_border_width(GTK_CONTAINER(window), 10);
//	gtk_widget_set_size_request(window, 250, 120);
		gtk_window_set_icon(GTK_WINDOW(window), gdk_pixbuf_new_from_inline(-1, icon_inline, false, nullptr));

		{
			GtkAccelGroup *gtk_accel = gtk_accel_group_new();
			GClosure *closure = g_cclosure_new(G_CALLBACK(accel_callback), (gpointer) (CB_CONTROL_AF - 10), nullptr);
			gtk_accel_group_connect(gtk_accel, gdk_keyval_from_name("a"), GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE, closure);

			closure = g_cclosure_new(G_CALLBACK(accel_callback), (gpointer) (CB_CONTROL_LED - 10), nullptr);
			gtk_accel_group_connect(gtk_accel, gdk_keyval_from_name("l"), GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE, closure);

			closure = g_cclosure_new(G_CALLBACK(accel_callback), (gpointer) (CB_CONTROL_ZOUT - 10), nullptr);
			gtk_accel_group_connect(gtk_accel, gdk_keyval_from_name("minus"), static_cast<GdkModifierType>(0), GTK_ACCEL_VISIBLE, closure);

			closure = g_cclosure_new(G_CALLBACK(accel_callback), (gpointer) (CB_CONTROL_ZIN - 10), nullptr);
			gtk_accel_group_connect(gtk_accel, gdk_keyval_from_name("equal"), static_cast<GdkModifierType>(0), GTK_ACCEL_VISIBLE, closure);

			gtk_window_add_accel_group(GTK_WINDOW(window), gtk_accel);
		}
		menu = gtk_menu_new();

		{
			GtkWidget *widget = gtk_menu_item_new_with_label("DroidCamX Commands:");
			gtk_menu_append (GTK_MENU(menu), widget);
			gtk_widget_show(widget);
			gtk_widget_set_sensitive(widget, 0);
		}

		{
			GtkWidget *widget = gtk_menu_item_new_with_label("Auto-Focus (Ctrl+A)");
			gtk_menu_append (GTK_MENU(menu), widget);
			gtk_widget_show(widget);
			g_signal_connect(widget, "activate", G_CALLBACK(the_callback), cbContext(&context, CB_CONTROL_AF));
		}

		{
			GtkWidget *widget = gtk_menu_item_new_with_label("Toggle LED Flash (Ctrl+L)");
			gtk_menu_append (GTK_MENU(menu), widget);
			gtk_widget_show(widget);
			g_signal_connect(widget, "activate", G_CALLBACK(the_callback), cbContext(&context, CB_CONTROL_LED));
		}

		{
			GtkWidget *widget = gtk_menu_item_new_with_label("Zoom In (+)");
			gtk_menu_append (GTK_MENU(menu), widget);
			gtk_widget_show(widget);
			g_signal_connect(widget, "activate", G_CALLBACK(the_callback), cbContext(&context, CB_CONTROL_ZIN));
		}

		{
			GtkWidget *widget = gtk_menu_item_new_with_label("Zoom Out (-)");
			gtk_menu_append (GTK_MENU(menu), widget);
			gtk_widget_show(widget);
			g_signal_connect(widget, "activate", G_CALLBACK(the_callback), cbContext(&context, CB_CONTROL_ZOUT));
		}

		vbox2 = gtk_vbox_new(false, 1);
		hbox = gtk_hbox_new(false, 50);
		gtk_container_add(GTK_CONTAINER(window), vbox2);

		// Toggle buttons
		vbox = gtk_vbox_new(false, 1);

		{
			hbox3 = gtk_hbox_new(true, 1);
			context.mode_ipcam = gtk_radio_button_new_with_label(nullptr, "IPCam mode");
			gtk_box_pack_start(GTK_BOX(hbox3), context.mode_ipcam, false, false, 8);

			context.mode_droidcam = gtk_radio_button_new_with_label(gtk_radio_button_get_group(GTK_RADIO_BUTTON(context.mode_ipcam)),
			                                                "Droidcam mode");
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(context.mode_droidcam), true);

			gtk_box_pack_start(GTK_BOX(hbox3), context.mode_droidcam, false, false, 8);


			g_signal_connect(context.mode_ipcam, "toggled", G_CALLBACK(the_callback), cbContext(&context, CB_MODE_IPCAM));
			g_signal_connect(context.mode_droidcam, "toggled", G_CALLBACK(the_callback), cbContext(&context, CB_MODE_DROIDCAM));

			gtk_box_pack_start(GTK_BOX(vbox2), hbox3, false, false, 8);
		}

		context.radioClient = gtk_radio_button_new_with_label(nullptr, "WiFi / LAN");
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(context.radioClient), (!(0)));
		g_signal_connect(context.radioClient, "toggled", G_CALLBACK(the_callback), cbContext(&context, CB_RADIO_WIFI));
		gtk_box_pack_start(GTK_BOX(vbox), context.radioClient, false, false, 0);

		context.radioServer = gtk_radio_button_new_with_label(gtk_radio_button_group(GTK_RADIO_BUTTON(context.radioClient)), "Wifi Server Mode");
		g_signal_connect(context.radioServer, "toggled", G_CALLBACK(the_callback), cbContext(&context, CB_WIFI_SRVR));
		gtk_box_pack_start(GTK_BOX(vbox), context.radioServer, false, false, 0);

		// widget = gtk_radio_button_new_with_label(gtk_radio_button_group(GTK_RADIO_BUTTON(widget)), "Bluetooth");
		// g_signal_connect(widget, "toggled", G_CALLBACK(the_callback), (gpointer)CB_RADIO_BTH);
//		gtk_box_pack_start(GTK_BOX(vbox), widget, false, false, 0);

		{
			GtkWidget *widget = gtk_radio_button_new_with_label(gtk_radio_button_group(GTK_RADIO_BUTTON(context.radioServer)), "USB (over adb)");
			g_signal_connect(widget, "toggled", G_CALLBACK(the_callback), cbContext(&context, CB_RADIO_ADB));
			gtk_box_pack_start(GTK_BOX(vbox), widget, false, false, 0);
		}

		/* TODO: Figure out audio
		widget = gtk_check_button_new_with_label("Enable Audio");
		g_signal_connect(widget, "toggled", G_CALLBACK(the_callback), (gpointer)CB_AUDIO);
		gtk_box_pack_start(GTK_BOX(vbox), widget, false, false, 5);
		*/

		hbox2 = gtk_hbox_new(false, 1);
		{
			GtkWidget *widget = gtk_button_new_with_label("...");
			gtk_widget_set_size_request(widget, 40, 28);
			g_signal_connect(widget, "clicked", G_CALLBACK(the_callback), cbContext(&context, CB_BTN_OTR));
			gtk_box_pack_start(GTK_BOX(hbox2), widget, false, false, 0);
		}
		gtk_box_pack_start(GTK_BOX(vbox), hbox2, false, false, 0);

		gtk_box_pack_start(GTK_BOX(hbox), vbox, false, false, 0);

		// IP/Port/Button

		vbox = gtk_vbox_new(false, 5);

		hbox2 = gtk_hbox_new(false, 1);
		gtk_box_pack_start(GTK_BOX(hbox2), gtk_label_new("Phone IP:"), false, false, 0);
		{
			context.ipEntry = gtk_entry_new_with_max_length(16);
			gtk_widget_set_size_request(context.ipEntry, 120, 30);
			gtk_box_pack_start(GTK_BOX(hbox2), context.ipEntry, false, false, 0);
		}

		{
			GtkWidget *widget = gtk_alignment_new(0, 0, 0, 0);
			gtk_container_add(GTK_CONTAINER(widget), hbox2);
			gtk_box_pack_start(GTK_BOX(vbox), widget, false, false, 0);
		}

		hbox2 = gtk_hbox_new(false, 1);
		gtk_box_pack_start(GTK_BOX(hbox2), gtk_label_new("DroidCam Port:"), false, false, 0);
		{
			context.portEntry = gtk_entry_new_with_max_length(5);
			gtk_widget_set_size_request(context.portEntry, 60, 30);
			gtk_box_pack_start(GTK_BOX(hbox2), context.portEntry, false, false, 0);
		}

		{
			GtkWidget *widget = gtk_alignment_new(0, 0, 0, 0);
			gtk_container_add(GTK_CONTAINER(widget), hbox2);
			gtk_box_pack_start(GTK_BOX(vbox), widget, false, false, 0);
		}

		hbox2 = gtk_hbox_new(false, 1);
		{
			GtkWidget *widget = gtk_button_new_with_label("Connect");
			gtk_widget_set_size_request(widget, 80, 30);
			g_signal_connect(widget, "clicked", G_CALLBACK(the_callback), cbContext(&context, CB_BUTTON));
			gtk_box_pack_start(GTK_BOX(hbox2), widget, false, false, 0);
			context.button = widget;
		}

		{
			GtkWidget *widget = gtk_alignment_new(1, 0, 0, 0);
			gtk_container_add(GTK_CONTAINER(widget), hbox2);
			gtk_box_pack_start(GTK_BOX(vbox), widget, false, false, 10);
		}

		gtk_box_pack_start(GTK_BOX(hbox), vbox, false, false, 0);

		gtk_box_pack_start(GTK_BOX(vbox2), hbox, false, false, 0);

		loadSettings(&context); // Load here after we have controls to put text into

		// Update UI from settings

		g_signal_connect(window, "destroy", G_CALLBACK(the_callback), cbContext(&context, CB_QUIT));
		gtk_widget_show_all(window);

		gdk_threads_enter();
		gtk_main();
		gdk_threads_leave();

		if (context.running == 1) StopVideo(&context);

		FREE_OBJECT(context.settings.hostName, free)

		connection_cleanup();
	}

	return 0;
}
