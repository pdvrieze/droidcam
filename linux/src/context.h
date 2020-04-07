//
// Created by pdvrieze on 01/04/2020.
//

#ifndef DROIDCAM_CONTEXT_H
#define DROIDCAM_CONTEXT_H

#include <glib.h>

typedef enum _InputMode {
	IM_DROIDCAM = 0,
	IM_IPCAM
} InputMode;

typedef enum _OutputMode {
	OM_DROIDCAM=1,
	OM_V4LLOOPBACK=2
} OutputMode;

typedef enum _Callbacks {
	CB_BUTTON = 0,
	CB_RADIO_WIFI,
	CB_RADIO_BTH,
	CB_RADIO_ADB,
	CB_WIFI_SRVR,
	CB_AUDIO,
	CB_BTN_OTR,
	CB_MODE_IPCAM,
	CB_MODE_DROIDCAM,
	CB_QUIT,
	CB_CONTROL_ZIN = 16,  // 6
	CB_CONTROL_ZOUT, // 7
	CB_CONTROL_AF,  // 8
	CB_CONTROL_LED, // 9

} Callbacks;


typedef struct _GtkEntry GtkEntry;
typedef struct _GtkButton GtkButton;
typedef struct _GtkWidget GtkWidget;

typedef struct _Settings {
	char *hostName;
	unsigned int port;
	Callbacks connection; // Connection type
	gboolean audio;
	InputMode inputMode;
} Settings;


typedef struct _JpgDecContext JpgDecContext;


typedef struct _Buffer {
	unsigned char *data;
	unsigned length;
} Buffer;

#define JPG_BACKBUF_MAX 10

typedef struct _JpegDecoder {
	Buffer jpg_frames[JPG_BACKBUF_MAX];
	JpgDecContext *jpg_decoder;
} JpgCtx;

typedef struct _DCContext {
	Settings settings;
	GtkWidget *button;
	GtkWidget *ipEntry;
	GtkWidget *portEntry;
	GtkWidget *radioServer;
	GtkWidget *radioClient;
	GtkWidget *mode_ipcam;
	GtkWidget *mode_droidcam;
	gboolean running;
	OutputMode droidcam_output_mode;
	JpgCtx * jpgCtx;
} DCContext;

typedef struct _ThreadArgs {
	int socket;
	DCContext *context;
//	Settings *settings;
//	int droidcam_output_mode;
} ThreadArgs;

Callbacks thread_cmd;

#endif //DROIDCAM_CONTEXT_H
