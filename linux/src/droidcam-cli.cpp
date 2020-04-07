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

#include <errno.h>
#include <string.h>

#include "common.h"
#include "connection.h"
#include "decoder.h"
#include "context.h"

char *g_ip;
int g_port;
int v_running;

void ShowError(const char * title, const char * msg) {
    errprint("%s: %s\n", title, msg);
}

void stream_video(DCContext *context)
{
    char buf[32];
    int keep_waiting = 0;
    SOCKET videoSocket = INVALID_SOCKET;

    if (g_ip != NULL) {
        videoSocket = connect_droidcam(g_ip, g_port);
        if (videoSocket == INVALID_SOCKET) {
            return;
        }
    }
    v_running  =1;

server_wait:
    if (videoSocket == INVALID_SOCKET) {
        videoSocket = accept_connection(g_port, context->running);
        if (videoSocket == INVALID_SOCKET) { goto early_out; }
        keep_waiting = 1;
    }

    {
        int len = snprintf(buf, sizeof(buf), VIDEO_REQ, decoder_get_video_width(), decoder_get_video_height());
        if (sendToSocket(buf, len, videoSocket) <= 0){
            MSG_ERROR("Error sending request, DroidCam might be busy with another client.");
            goto early_out;
        }
    }

    memset(buf, 0, sizeof(buf));
    if (recvFromSocket(buf, 5, videoSocket) <= 0 ){
        MSG_ERROR("Connection reset by app!\nDroidCam is probably busy with another client");
        goto early_out;
    }

    if (context->jpgCtx->prepareVideo(buf) == FALSE) {
        goto early_out;
    }

    while (1){
        Buffer *f = decoder_get_next_frame(context->jpgCtx);
        if (recvFromSocket(buf, 4, videoSocket) == FALSE) break;
        unsigned int frameLen = make_int4(buf[0], buf[1], buf[2], buf[3]);
        f->data_length = frameLen;
        char *p = (char*)f->data;
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
	decoder_cleanup(context->jpgCtx);

    if (keep_waiting){
        videoSocket = INVALID_SOCKET;
        goto server_wait;
    }

    v_running = 0;
    connection_cleanup();
}

void usage(int argc, char *argv[]) {
    fprintf(stderr, "Usage: \n"
    " %s -l <port>\n"
    "   Listen on 'port' for connections\n"
    "\n"
    " %s <ip> <port>\n"
    "   Connect to 'ip' on 'port'\n"
    ,
    argv[0], argv[0]);
}


int main(int argc, char *argv[]) {
    if (argc == 3 && argv[1][0] == '-' && argv[1][1] == 'l') {
        g_ip = NULL;
        g_port = atoi(argv[2]);
    }
    else if (argc == 3) {
        g_ip = argv[1];
        g_port = atoi(argv[2]);
    }
    else {
        usage(argc, argv);
        return 1;
    }

    Settings settings={};
    Decoder decoder;
    OutputMode outputMode =OutputMode::OM_DROIDCAM;

	if (!(&decoder)->init()) {
        return 2;
    }
    stream_video(&settings);
    return 0;
}