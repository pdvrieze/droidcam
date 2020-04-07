//
// Created by pdvrieze on 01/04/2020.
//

#ifndef DROIDCAM_CONTEXT_H
#define DROIDCAM_CONTEXT_H

#include <glib.h>
#include <memory>

typedef enum _InputMode {
	IM_DROIDCAM = 0,
	IM_IPCAM
} InputMode;

enum class OutputMode {
	OM_MISSING=0,
	OM_DROIDCAM=1,
	OM_V4LLOOPBACK=2

};

typedef enum _Callbacks {
	CB_NONE=0,
	CB_BUTTON,
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

} Callback;


typedef struct _GtkEntry GtkEntry;
typedef struct _GtkButton GtkButton;
typedef struct _GtkWidget GtkWidget;

typedef struct _Settings {
	char *hostName;
	unsigned int port;
	Callback connection; // Connection type
	gboolean audio;
	InputMode inputMode;
} Settings;


class JpgDecContext;


typedef struct _Buffer {
	unsigned char *data;
	size_t buf_size;
	size_t data_length;
} Buffer;

#define JPG_BACKBUF_MAX 10

typedef struct _DCContext DCContext;

typedef struct _CallbackContext {
	DCContext *context;
	Callback cb;
} CallbackContext;

class Decoder;

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
	Decoder * decoder;
	CallbackContext callbackData[20];
} DCContext;

typedef struct _ThreadArgs {
	int socket;
	DCContext *context;
//	Settings *settings;
//	int droidcam_output_mode;
} ThreadArgs;

extern Callback thread_cmd;

#endif //DROIDCAM_CONTEXT_H
