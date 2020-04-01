//
// Created by pdvrieze on 01/04/2020.
//

#include "ipcam.h"
#include "connection.h"
#include "context.h"
#include "common.h"
#include "decoder.h"
#include <stdlib.h>
#include <gtk/gtk.h>

void *ipcamVideoThreadProc(void *args)
{
	SOCKET videoSocket;
	DCContext *context;
	Settings *settings;
	{
		ThreadArgs *threadargs = (ThreadArgs *) args;
		context = threadargs->context;
		videoSocket = threadargs->socket;
		settings = context->settings;
		free(args);
		args=0;
	}
	char buf[32];
	int keep_waiting = 0;
	dbgprint("Video Thread Started s=%d\n", videoSocket);
	context->running = 1;

	server_wait:
	if (videoSocket == INVALID_SOCKET) {
		videoSocket = accept_connection(atoi(gtk_entry_get_text(settings->portEntry)), &(context->running));
		if (videoSocket == INVALID_SOCKET) { goto early_out; }
		keep_waiting = 1;
	}

	if (context->droidcam_output_mode == 2) {
		loopback_init(1280, 720);

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

	if (decoder_prepare_video(buf) == FALSE) {
		goto early_out;
	}

	while (context->running != 0) {
		if (thread_cmd != 0) {
			int len = sprintf(buf, OTHER_REQ, thread_cmd);
			sendToSocket(buf, len, videoSocket);
			thread_cmd = 0;
		}

		int frameLen;
		struct jpg_frame_s *f = decoder_get_next_frame();
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
	decoder_cleanup();

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
