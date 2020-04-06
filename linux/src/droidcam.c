/* DroidCam & DroidCamX (C) 2010-
 * https://github.com/aramg
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * Use at your own risk. See README file for more details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <gtk/gtk.h>
#include <curl/curl.h>
#include <errno.h>

#include "common.h"
#include "connection.h"
#include "decoder.h"
#include "icon.h"
#include "context.h"
#include "ipcam.h"


typedef struct _CallbackContext {
	DCContext *context;
	Callbacks cb;
} CallbackContext;

/* Globals */
GtkWidget *menu;
GThread *hVideoThread;
int thread_cmd = 0;
int wifi_srvr_mode = 0;

/* Helper Functions */
void ShowError(const char *title, const char *msg)
{
	if (hVideoThread != NULL)
		gdk_threads_enter();

	GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
	                                           "%s", msg);
	gtk_window_set_title(GTK_WINDOW(dialog), title);
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);

	if (hVideoThread != NULL)
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
		if (fgets(buf, sizeof(buf), pipe) == NULL) break;
		dbgprint("Got line: %s", buf);

		if (strstr(buf, "List of") != NULL) {
			haveDevice = 2;
			continue;
		}
		if (haveDevice == 2) {
			if (strstr(buf, "offline") != NULL) {
				haveDevice = 4;
				break;
			}
			if (strstr(buf, "device") != NULL && strstr(buf, "??") == NULL) {
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

	dbgprint("set defaults...");
	gtk_entry_set_text(context->ipEntry, "");
	gtk_entry_set_text(context->portEntry, "4747");


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

			free(context->settings.hostName);
			context->settings.hostName = strdup(buf);

		}

		if (fgets(buf, sizeof(buf), fp)) {
			buf[strlen(buf) - 1] = '\0';
		}

		if (version == 2 && fgets(buf, sizeof(buf), fp)) {
			buf[strlen(buf) - 1] = '\0';
			if (strcmp(buf, "ipcam") == 0) {
				context->settings.inputMode = IM_IPCAM;
			}
		}
	}

	gtk_entry_set_text(context->ipEntry, context->settings.hostName);
	char portText[8];
	memset(portText, 0, 8);
	snprintf(portText, 7, "%d", context->settings.port);
	gtk_entry_set_text(context->portEntry, portText);

	fclose(fp);
}

static void updateSettingsFromContext(DCContext *context) {
	Settings *settings = &(context->settings);
	char *end;
	const gchar *entryText = gtk_entry_get_text(context->portEntry);
	unsigned int portNo = strtol(entryText, &end, 10);
	if (end != entryText && *end == '\0' && errno != ERANGE) {
		settings->port = portNo;
	}

	const gchar *host = gtk_entry_get_text(context->ipEntry);

	if (settings->hostName == NULL ||
	    strcmp(host, settings->hostName)!=0) {
		if(settings->hostName!=NULL) { free(settings->hostName); }
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
		        gtk_entry_get_text(settings->hostName),
		        gtk_entry_get_text(portText),
		        settings->inputMode == IM_IPCAM ? "ipcam" : "droidcam");
	}
	fclose(fp);
}

/* Video Thread */
void *DroidcamVideoThreadProc(void *args)
{
	SOCKET videoSocket;
	DCContext *context;
	JpgCtx *jpgCtx;
	{
		ThreadArgs *threadArgs = (ThreadArgs *) args;
		videoSocket = threadArgs->socket;
		context = threadArgs->context;
		jpgCtx = context->jpgCtx;

		free(args);
		args = 0;
	}
	char buf[32];
	int keep_waiting = 0;
	dbgprint("Video Thread Started s=%d\n", videoSocket);
	context->running = 1;

	server_wait:
	if (videoSocket == INVALID_SOCKET) {
		videoSocket = accept_connection(context->settings.port, &(context->running));
		if (videoSocket == INVALID_SOCKET) { goto early_out; }
		keep_waiting = 1;
	}

	if (context->droidcam_output_mode == 2) {
		loopback_init(jpgCtx, 1280, 720);

	} else if (context->droidcam_output_mode != 1) {
		MSG_ERROR("Droidcam not in proper output mode");
		goto early_out;
	}

	int len = snprintf(buf, sizeof(buf), VIDEO_REQ, decoder_get_video_width(), decoder_get_video_height());
	if (sendToSocket(buf, len, videoSocket) <= 0) {
		MSG_ERROR("Error sending request, DroidCam might be busy with another client.");
		goto early_out;
	}


	memset(buf, 0, sizeof(buf));
	if (recvFromSocket(buf, 5, videoSocket) <= 0) {
		MSG_ERROR("Connection reset by app!\nDroidCam is probably busy with another client");
		goto early_out;
	}

	if (decoder_prepare_video(jpgCtx, buf) == FALSE) {
		goto early_out;
	}

	while (context->running) {
		if (thread_cmd != 0) {
			int len = sprintf(buf, OTHER_REQ, thread_cmd);
			sendToSocket(buf, len, videoSocket);
			thread_cmd = 0;
		}

		int frameLen;
		struct jpg_frame_s *f = decoder_get_next_frame(jpgCtx);
		if (recvFromSocket(buf, 4, videoSocket) == FALSE) break;
		make_int4(frameLen, buf[0], buf[1], buf[2], buf[3]);
		f->length = frameLen;
		char *p = (char *) f->data;
		while (frameLen > 4096) {
			if (recvFromSocket(p, 4096, videoSocket) == FALSE) goto early_out;
			frameLen -= 4096;
			p += 4096;
		}
		if (recvFromSocket(p, frameLen, videoSocket) == FALSE) break;
	}

	early_out:
	dbgprint("disconnect\n");
	disconnect(videoSocket);
	decoder_cleanup(jpgCtx);

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
	if (hVideoThread != NULL) {
		dbgprint("Waiting for videothread..\n");
		g_thread_join(hVideoThread);
		dbgprint("videothread joined\n");
		hVideoThread = NULL;
		//gtk_widget_set_sensitive(GTK_WIDGET(g_settings.button), TRUE);
	}
}

/* Messages */
/*
static gint button_press_event(GtkWidget *widget, GdkEvent *event){
	if (event->type == GDK_BUTTON_PRESS){
		GdkEventButton *bevent = (GdkEventButton *) event;
		//if (bevent->button == 3)
		gtk_menu_popup (GTK_MENU(menu), NULL, NULL, NULL, NULL,
						bevent->button, bevent->time);
		return TRUE;
	}
	return FALSE;
}
*/
static gboolean
accel_callback(GtkAccelGroup *group,
               GObject *obj,
               guint keyval,
               GdkModifierType mod,
               gpointer user_data)
{
	DCContext *context = ((CallbackContext *) user_data)->context;
	if (context->running == 1 && thread_cmd == 0) {
		thread_cmd = (int) user_data;
	}
	return TRUE;
}

static void doConnect(DCContext *context)
{
#if 1
	updateSettingsFromContext(context);
	Settings *settings = &(context->settings);
	char *ip = NULL;
	SOCKET droidcam_socket = INVALID_SOCKET;
	saveSettings(settings);

	if (settings->connection == CB_RADIO_ADB) {
		if (CheckAdbDevices(settings->port) != 8) return;
		ip = "127.0.0.1";
	} else if (settings->connection == CB_RADIO_WIFI && wifi_srvr_mode == 0) {
		ip = settings->hostName;
	}

	ThreadArgs *args = malloc(sizeof(ThreadArgs));
	(*args) = (ThreadArgs) {0, context};

	if (ip != NULL) // Not Bluetooth or "Server Mode", so connect first
	{
		if (strlen(ip) < 7 || settings->port < 1024) {
			MSG_ERROR("You must enter the correct IP address (and port) to connect to.");
			return;
		}
		gtk_button_set_label(context->button, "Please wait");
		if (settings->inputMode == IM_DROIDCAM) {
			droidcam_socket = connect_droidcam(ip, settings->port);

			if (droidcam_socket == INVALID_SOCKET) {
				dbgprint("failed");
				gtk_button_set_label(context->button, "Connect");
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


	gtk_button_set_label(context->button, "Stop");
	//gtk_widget_set_sensitive(GTK_WIDGET(settings->button), FALSE);

	gtk_widget_set_sensitive(GTK_WIDGET(context->ipEntry), FALSE);
	gtk_widget_set_sensitive(GTK_WIDGET(context->portEntry), FALSE);
#else
	decoder_show_test_image(context->jpgCtx, &context->droidcam_output_mode);
#endif
}

static void doDisconnect(DCContext *context)
{
	StopVideo(context);
}

static void the_callback(GtkWidget *widget, CallbackContext *callbackContext)
{
	int cb = (int) callbackContext->cb;
	DCContext *context = callbackContext->context;
	gboolean ipEdit = TRUE;
	gboolean portEdit = TRUE;
	char *text = NULL;

	_up:
	dbgprint("the_cb=%d\n", cb);
	switch (cb) {
		case CB_BUTTON:
			if (context->running) {
				doDisconnect(context);
				cb = (int) context->settings.connection;
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
				ipEdit = FALSE;
			} else {
				text = "Connect";
			}
			break;
		case CB_RADIO_BTH:
			context->settings.connection = CB_RADIO_BTH;
			text = "Prepare";
			ipEdit = FALSE;
			portEdit = FALSE;
			break;
		case CB_RADIO_ADB:
			context->settings.connection = CB_RADIO_ADB;
			text = "Connect";
			ipEdit = FALSE;
			break;
		case CB_BTN_OTR:
			gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, 0, 0);
			break;
		case CB_CONTROL_ZIN  :
		case CB_CONTROL_ZOUT :
		case CB_CONTROL_AF   :
		case CB_CONTROL_LED  :
			if (context->running == 1 && thread_cmd == 0) {
				thread_cmd = cb - 10;
			}
			break;
		case CB_AUDIO: {
			int *a = &(context->settings.audio);
			*a = !(*a);
			break;
		}
		case CB_MODE_DROIDCAM:
			context->settings.inputMode = IM_DROIDCAM;
			break;
		case CB_MODE_IPCAM:
			context->settings.inputMode = IM_IPCAM;
			break;
	}

	if(context->settings.inputMode == IM_DROIDCAM) {
		gtk_widget_set_sensitive(GTK_WIDGET(context->radioServer), TRUE);
	} else {
		if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(context->radioServer))) {
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(context->radioServer), FALSE);
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(context->radioClient), TRUE);
		}
		gtk_widget_set_sensitive(GTK_WIDGET(context->radioServer), FALSE);
	}

	if (text != NULL && context->running == 0) {
		gtk_button_set_label(context->button, text);
		gtk_widget_set_sensitive(GTK_WIDGET(context->ipEntry), ipEdit);
		gtk_widget_set_sensitive(GTK_WIDGET(context->portEntry), portEdit);
	}
}

static CallbackContext *cbContext(DCContext *context, Callbacks cb)
{
	CallbackContext *result = malloc(sizeof(CallbackContext));
	*result = (CallbackContext) {context, cb};
	return result;
}

int main(int argc, char *argv[])
{
	JpgCtx jpgCtx;
	DCContext context = {
		.settings = {},
		.button=NULL,
		.running=FALSE,
		.droidcam_output_mode=0,
		.jpgCtx = &jpgCtx
	};


	if (decoder_init(&jpgCtx, &(context.droidcam_output_mode))) {

		GtkWidget *window;
		GtkWidget *hbox, *hbox2, *hbox3;
		GtkWidget *vbox, *vbox2;
		GtkWidget *widget; // generic stuff
		GtkWidget *mode_ipcam, *mode_droidcam;

		// init threads
		g_thread_init(NULL);
		gdk_threads_init();
		gtk_init(&argc, &argv);

		window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
		gtk_window_set_title(GTK_WINDOW(window), "DroidCam Client");
		gtk_container_set_border_width(GTK_CONTAINER(window), 1);
		gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
		gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_NONE);
		gtk_container_set_border_width(GTK_CONTAINER(window), 10);
//	gtk_widget_set_size_request(window, 250, 120);
		gtk_window_set_icon(GTK_WINDOW(window), gdk_pixbuf_new_from_inline(-1, icon_inline, FALSE, NULL));

		{
			GtkAccelGroup *gtk_accel = gtk_accel_group_new();
			GClosure *closure = g_cclosure_new(G_CALLBACK(accel_callback), (gpointer) (CB_CONTROL_AF - 10), NULL);
			gtk_accel_group_connect(gtk_accel, gdk_keyval_from_name("a"), GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE, closure);

			closure = g_cclosure_new(G_CALLBACK(accel_callback), (gpointer) (CB_CONTROL_LED - 10), NULL);
			gtk_accel_group_connect(gtk_accel, gdk_keyval_from_name("l"), GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE, closure);

			closure = g_cclosure_new(G_CALLBACK(accel_callback), (gpointer) (CB_CONTROL_ZOUT - 10), NULL);
			gtk_accel_group_connect(gtk_accel, gdk_keyval_from_name("minus"), 0, GTK_ACCEL_VISIBLE, closure);

			closure = g_cclosure_new(G_CALLBACK(accel_callback), (gpointer) (CB_CONTROL_ZIN - 10), NULL);
			gtk_accel_group_connect(gtk_accel, gdk_keyval_from_name("equal"), 0, GTK_ACCEL_VISIBLE, closure);

			gtk_window_add_accel_group(GTK_WINDOW(window), gtk_accel);
		}
		menu = gtk_menu_new();

		widget = gtk_menu_item_new_with_label("DroidCamX Commands:");
		gtk_menu_append (GTK_MENU(menu), widget);
		gtk_widget_show(widget);
		gtk_widget_set_sensitive(widget, 0);

		widget = gtk_menu_item_new_with_label("Auto-Focus (Ctrl+A)");
		gtk_menu_append (GTK_MENU(menu), widget);
		gtk_widget_show(widget);
		g_signal_connect(widget, "activate", G_CALLBACK(the_callback), cbContext(&context, CB_CONTROL_AF));

		widget = gtk_menu_item_new_with_label("Toggle LED Flash (Ctrl+L)");
		gtk_menu_append (GTK_MENU(menu), widget);
		gtk_widget_show(widget);
		g_signal_connect(widget, "activate", G_CALLBACK(the_callback), cbContext(&context, CB_CONTROL_LED));

		widget = gtk_menu_item_new_with_label("Zoom In (+)");
		gtk_menu_append (GTK_MENU(menu), widget);
		gtk_widget_show(widget);
		g_signal_connect(widget, "activate", G_CALLBACK(the_callback), cbContext(&context, CB_CONTROL_ZIN));

		widget = gtk_menu_item_new_with_label("Zoom Out (-)");
		gtk_menu_append (GTK_MENU(menu), widget);
		gtk_widget_show(widget);
		g_signal_connect(widget, "activate", G_CALLBACK(the_callback), cbContext(&context, CB_CONTROL_ZOUT));

		vbox2 = gtk_vbox_new(FALSE, 1);
		hbox = gtk_hbox_new(FALSE, 50);
		gtk_container_add(GTK_CONTAINER(window), vbox2);

		// Toggle buttons
		vbox = gtk_vbox_new(FALSE, 1);

		hbox3 = gtk_hbox_new(TRUE, 1);
		mode_ipcam = gtk_radio_button_new_with_label(NULL, "IPCam mode");
		gtk_box_pack_start(GTK_BOX(hbox3), mode_ipcam, FALSE, FALSE, 8);
		g_signal_connect(mode_ipcam, "toggled", G_CALLBACK(the_callback), cbContext(&context, CB_MODE_IPCAM));

		mode_droidcam = gtk_radio_button_new_with_label(gtk_radio_button_get_group(GTK_RADIO_BUTTON(mode_ipcam)),
		                                                "Droidcam mode");
		gtk_box_pack_start(GTK_BOX(hbox3), mode_droidcam, FALSE, FALSE, 8);
		g_signal_connect(mode_ipcam, "toggled", G_CALLBACK(the_callback), cbContext(&context, CB_MODE_DROIDCAM));

		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mode_droidcam), TRUE);
		gtk_box_pack_start(GTK_BOX(vbox2), hbox3, FALSE, FALSE, 8);

		context.radioClient = gtk_radio_button_new_with_label(NULL, "WiFi / LAN");
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(context.radioClient), TRUE);
		g_signal_connect(context.radioClient, "toggled", G_CALLBACK(the_callback), cbContext(&context, CB_RADIO_WIFI));
		gtk_box_pack_start(GTK_BOX(vbox), context.radioClient, FALSE, FALSE, 0);

		context.radioServer = gtk_radio_button_new_with_label(gtk_radio_button_group(GTK_RADIO_BUTTON(widget)), "Wifi Server Mode");
		g_signal_connect(context.radioServer, "toggled", G_CALLBACK(the_callback), cbContext(&context, CB_WIFI_SRVR));
		gtk_box_pack_start(GTK_BOX(vbox), context.radioServer, FALSE, FALSE, 0);

		// widget = gtk_radio_button_new_with_label(gtk_radio_button_group(GTK_RADIO_BUTTON(widget)), "Bluetooth");
		// g_signal_connect(widget, "toggled", G_CALLBACK(the_callback), (gpointer)CB_RADIO_BTH);
//		gtk_box_pack_start(GTK_BOX(vbox), widget, FALSE, FALSE, 0);

		widget = gtk_radio_button_new_with_label(gtk_radio_button_group(GTK_RADIO_BUTTON(widget)), "USB (over adb)");
		g_signal_connect(widget, "toggled", G_CALLBACK(the_callback), cbContext(&context, CB_RADIO_ADB));
		gtk_box_pack_start(GTK_BOX(vbox), widget, FALSE, FALSE, 0);

		/* TODO: Figure out audio
		widget = gtk_check_button_new_with_label("Enable Audio");
		g_signal_connect(widget, "toggled", G_CALLBACK(the_callback), (gpointer)CB_AUDIO);
		gtk_box_pack_start(GTK_BOX(vbox), widget, FALSE, FALSE, 5);
		*/

		hbox2 = gtk_hbox_new(FALSE, 1);
		widget = gtk_button_new_with_label("...");
		gtk_widget_set_size_request(widget, 40, 28);
		g_signal_connect(widget, "clicked", G_CALLBACK(the_callback), cbContext(&context, CB_BTN_OTR));
		gtk_box_pack_start(GTK_BOX(hbox2), widget, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(vbox), hbox2, FALSE, FALSE, 0);

		gtk_box_pack_start(GTK_BOX(hbox), vbox, FALSE, FALSE, 0);

		// IP/Port/Button

		vbox = gtk_vbox_new(FALSE, 5);

		hbox2 = gtk_hbox_new(FALSE, 1);
		gtk_box_pack_start(GTK_BOX(hbox2), gtk_label_new("Phone IP:"), FALSE, FALSE, 0);
		widget = gtk_entry_new_with_max_length(16);
		gtk_widget_set_size_request(widget, 120, 30);
		context.ipEntry = (GtkEntry *) widget;
		gtk_box_pack_start(GTK_BOX(hbox2), widget, FALSE, FALSE, 0);

		widget = gtk_alignment_new(0, 0, 0, 0);
		gtk_container_add(GTK_CONTAINER(widget), hbox2);
		gtk_box_pack_start(GTK_BOX(vbox), widget, FALSE, FALSE, 0);

		hbox2 = gtk_hbox_new(FALSE, 1);
		gtk_box_pack_start(GTK_BOX(hbox2), gtk_label_new("DroidCam Port:"), FALSE, FALSE, 0);
		widget = gtk_entry_new_with_max_length(5);
		gtk_widget_set_size_request(widget, 60, 30);
		context.portEntry = (GtkEntry *) widget;
		gtk_box_pack_start(GTK_BOX(hbox2), widget, FALSE, FALSE, 0);

		widget = gtk_alignment_new(0, 0, 0, 0);
		gtk_container_add(GTK_CONTAINER(widget), hbox2);
		gtk_box_pack_start(GTK_BOX(vbox), widget, FALSE, FALSE, 0);

		hbox2 = gtk_hbox_new(FALSE, 1);
		widget = gtk_button_new_with_label("Connect");
		gtk_widget_set_size_request(widget, 80, 30);
		g_signal_connect(widget, "clicked", G_CALLBACK(the_callback), cbContext(&context, CB_BUTTON));
		gtk_box_pack_start(GTK_BOX(hbox2), widget, FALSE, FALSE, 0);
		context.button = (GtkButton *) widget;

		widget = gtk_alignment_new(1, 0, 0, 0);
		gtk_container_add(GTK_CONTAINER(widget), hbox2);
		gtk_box_pack_start(GTK_BOX(vbox), widget, FALSE, FALSE, 10);

		gtk_box_pack_start(GTK_BOX(hbox), vbox, FALSE, FALSE, 0);

		gtk_box_pack_start(GTK_BOX(vbox2), hbox, FALSE, FALSE, 0);

		loadSettings(&context); // Load here after we have controls to put text into

		g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
		gtk_widget_show_all(window);

		gdk_threads_enter();
		gtk_main();
		gdk_threads_leave();

		if (context.running == 1) StopVideo(&context);

		decoder_fini(&jpgCtx);
		connection_cleanup();
	}

	return 0;
}
