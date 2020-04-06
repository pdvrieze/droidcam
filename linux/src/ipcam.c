//
// Created by pdvrieze on 01/04/2020.
//

#include "ipcam.h"
#include "connection.h"
#include "context.h"
#include "common.h"
#include "decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <gtk/gtk.h>
#include <curl/curl.h>
#include <string.h>

typedef struct _CurlContext {
	CURL *curl;
	curl_socket_t socket;
	gboolean *running;
} CurlContext;

typedef struct _FlexBuffer {
	unsigned char *buffer;
	size_t size;
} FlexBuffer;

typedef struct {
	unsigned char *buffer;
	size_t maxSize;
	size_t offset;
	size_t recvCount;
} ReadBuffer;

static const int BUFSIZE = 4 * 1024;
static const int FRAME_MAX = 4 * 1024;
static const char CR = 13;
static const char LF = 10;
static const char CRLF[] = {CR, LF};

gboolean startsWith(const char *haystack, const char *needle)
{
	return strncmp(needle, haystack, strlen(needle)) == 0;
}

static int wait_on_recv_socket(curl_socket_t sockfd, long timeout_ms)
{
	fd_set inFds, errFds;
	FD_ZERO(&inFds);
	FD_ZERO(&errFds);
	FD_SET(sockfd, &inFds);
	FD_SET(sockfd, &errFds);

	struct timeval tv = {.tv_sec=timeout_ms / 1000, .tv_usec=(timeout_ms % 1000) * 1000};
	return select(1, &inFds, NULL, &errFds, &tv);
}

static size_t min(size_t a, size_t b)
{
	return a < b ? a : b;
}

static gboolean readIntoBuffer(ReadBuffer *b, CurlContext *curlContext, const char *boundary, gboolean *running) {
	b->offset = 0;
	while (*running && b->recvCount<4+strlen(boundary))
	{
		// Read first buffer content
		int recvCount;
		CURLcode res = curl_easy_recv(curlContext->curl, b->buffer + b->recvCount, b->maxSize-b->recvCount, &recvCount);
		while (res == CURLE_AGAIN && *running) {
			int retval = wait_on_recv_socket(curlContext->socket, 250);// quarter of a second

			if (retval == -1) {
				MSG_ERROR("select");
				return FALSE;
			}

			if (retval && *running) {
				res = curl_easy_recv(curlContext->curl, b->buffer + b->recvCount, b->maxSize-b->recvCount, &recvCount);
			}
		}
		if (res==CURLE_OK) {
			b->recvCount+=recvCount;
		}
	}
	return TRUE;
}

static unsigned char *consumeLine(unsigned char *buffer, size_t max_buf, char *line, size_t maxLine)
{
	unsigned char *last = buffer + max_buf - 1; // extra space for LF
	unsigned char *cur;
	for (cur = buffer; cur < last && (cur[0] != CR || cur[1] != LF); ++cur) {}
	if (cur[0] == CR && cur[1] == LF) {
		size_t n = min(cur - buffer, maxLine - 1);
		strncpy(line, (char *) buffer, n);
		buffer[n] = 0; // ensure terminating 0
		return cur + 2; // skip CRLF
	} else {
		return buffer; // no move
	}
}

static gboolean consumeBoundary(ReadBuffer *b, const char *boundary)
{
	size_t maxOffset = 2 + strlen(boundary);
	const unsigned char *current = b->buffer + b->offset;
	size_t offset = 0;

	while (offset < maxOffset && current < (b->buffer + b->recvCount - maxOffset + offset)) {
		if (offset == 0 || offset == 1) {
			if (*current == '-') {
				++offset;
			} else {
				offset = 0;
			}
		} else {
			if (*current == boundary[offset - 2]) {
				++offset;
			} else {
				offset = 0;
			}
		}

		++current;
	}
	if (offset >= maxOffset) {
		b->offset += offset;
		return TRUE;
	} else {
		return FALSE;
	}
}

static gboolean consumeSubHeader(ReadBuffer *b, long *content_length)
{
	unsigned char *bufCur = b->buffer + b->offset;
	unsigned char *bufEnd = b->buffer + b->recvCount;
	unsigned char *bufNext;
	char line[BUFSIZE];
	for (
		bufNext = consumeLine(bufCur, bufEnd - bufCur, line, BUFSIZE);
		bufNext != bufCur && line[0] != 0;
		bufNext = consumeLine(bufCur, bufEnd - bufCur, line, BUFSIZE)) {

		if (startsWith(line, "Content-Length:")) {
			char *endPtr;
			*content_length = strtol(&line[15], &endPtr, 10);
		}

		bufCur = bufNext;
	}
	if (bufCur == bufNext) { // error, set invalid content length, don't move
		*content_length = -1;
		return FALSE;
	}
	b->offset = bufNext - b->buffer;
	return TRUE;
}


gboolean readFrame(ReadBuffer *b, CurlContext *curlContext, size_t contentLength, FlexBuffer *resultPtr)
{
	if (resultPtr->buffer == NULL) {
		resultPtr->buffer = malloc(contentLength);
		resultPtr->size = contentLength;
	} else if (contentLength > resultPtr->size || (contentLength < (resultPtr->size / 4))) {
		resultPtr->buffer = realloc(resultPtr->buffer, contentLength);
		resultPtr->size = b->buffer != NULL ? contentLength : 0;
	}

	// Short circuit if the entire image exists in the header buffer
	if (contentLength<b->recvCount-b->offset) {
		memcpy(resultPtr->buffer, b->buffer + b->offset, contentLength);
		memmove(b->buffer, b->buffer+b->offset, contentLength);
		b->recvCount-=contentLength;
		b->offset=0;
		return TRUE;
	}

	memcpy(resultPtr->buffer, b->buffer + b->offset, b->recvCount - b->offset);
	size_t offset = b->recvCount - b->offset;
	size_t recvCnt;

	while (*(curlContext->running) && offset < contentLength) {
		CURLcode res = curl_easy_recv(curlContext->curl, resultPtr->buffer + offset, contentLength - offset, &recvCnt);
		while (res == CURLE_AGAIN) {
			wait_on_recv_socket(curlContext->socket, 250);
			res = curl_easy_recv(curlContext->curl, resultPtr->buffer + offset, contentLength - offset, &recvCnt);
			if (!curlContext->running) return FALSE; // early out
		}

		offset += recvCnt;
	}

	b->offset=0;
	b->recvCount=0;

	if (offset == contentLength) { return TRUE; }

	return FALSE;
}


void *ipcamVideoThreadProc(void *args)
{
	DCContext *context;
	Settings *settings;
	JpgCtx *jpgCtx;
	{
		ThreadArgs *threadargs = (ThreadArgs *) args;
		context = threadargs->context;
		settings = &(context->settings);
		jpgCtx = context->jpgCtx;
		free(args);
		args = 0;
	}
	char url[256];
	snprintf(url, 255, "http://%s:%d/video", settings->hostName, settings->port);

	if (curl_global_init(CURL_GLOBAL_NOTHING)) { return 0; }


	CurlContext curlContext;
	curlContext.curl = curl_easy_init();
	curlContext.running = &(context->running);


	curl_easy_setopt(curlContext.curl, CURLOPT_URL, url);
//	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFunc);
//	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &curlContext);
	curl_easy_setopt(curlContext.curl, CURLOPT_CONNECT_ONLY, 1L);
	curl_easy_setopt(curlContext.curl, CURLOPT_VERBOSE, 1L);

	if (curl_easy_perform(curlContext.curl) == CURLE_OK) {
		char *contentType;
		{
			CURLcode res = curl_easy_getinfo(curlContext.curl, CURLINFO_CONTENT_TYPE, &contentType);
			if (res != CURLE_OK) { goto early_out; }
		}
		if (contentType != NULL) { printf("Content-Type: %s\n", contentType); }

		if (!startsWith(contentType, "multipart/x-mixed-replace;")) {
			MSG_ERROR("Unexpected content type in ipcam");
			goto early_out;
		}

		char *start = strstr(contentType, "boundary");
		char *end = start + strlen(start);
		if (start == NULL) {
			MSG_ERROR("Missing boundary");
			goto early_out;
		}
		for (start += 8; *start == ' ' && (start < end); ++start) {}
		if (*start != '=') {
			MSG_ERROR("No equals for boundary");
			goto early_out;
		}
		while (start < end && (*start) == ' ') { ++start; }

		char *boundary = start;
		printf("Found boundary: '%s'", boundary);

		{
			CURLcode res = curl_easy_getinfo(curlContext.curl, CURLINFO_ACTIVESOCKET, &curlContext.socket);
			if (res != CURLE_OK) {
				MSG_ERROR("Could not determine socket");
				goto early_out;
			}
		}

		unsigned char _buffer[BUFSIZE];
		ReadBuffer b = {.buffer=_buffer, .maxSize=sizeof(_buffer)};

		if (!readIntoBuffer(&b, &curlContext, boundary, &context->running)) goto early_out;

		if (!consumeBoundary(&b, boundary)) { MSG_ERROR("error consuming boundary"); goto early_out; }

		long contentLength;

		if (!consumeSubHeader(&b, &contentLength)) { MSG_ERROR("Could not consume any jpeg"); goto early_out; }

		FlexBuffer frame = {NULL, 0};
		if (!readFrame(&b, &curlContext, (unsigned long) contentLength,
		               &frame)) { goto early_out; };

		if (!decoder_prepare_video(jpgCtx, frame.buffer)) {
			MSG_ERROR("Could not decode jpeg");
			goto early_out;
		}

		if (context->droidcam_output_mode == OM_V4LLOOPBACK) {
			unsigned int width, height;
			decoder_source_dimensions(jpgCtx, &width, &height);
			loopback_init(jpgCtx, width, height); // init with actual image size in the non-droidcam loopback mode
		}


		decoder_get_next_frame(jpgCtx);

		while (context->running) {

			if(!consumeBoundary(&b, boundary)) { goto early_out; };
			if(!consumeSubHeader(&b, &contentLength)) { MSG_ERROR("Could not decode jpeg correctly"); goto early_out; };

			if (!readFrame(&b, &curlContext, contentLength, &frame)) { goto early_out; }

			if (context->droidcam_output_mode == OM_V4LLOOPBACK) {
				unsigned int width, height;
				decoder_source_dimensions(jpgCtx, &width, &height);
				if (width != decoder_get_video_width() || height != decoder_get_video_height()) {
					// If the size changed, reinitialize (not clear that this can happen)
					loopback_init(jpgCtx, width, height);
				}
			}
			decoder_get_next_frame(jpgCtx);
		}

	} else {
		MSG_ERROR("Could not connect with ipcam");
	}

	early_out:
	dbgprint("disconnect\n");
	decoder_cleanup(context->jpgCtx);

	connection_cleanup();

	curl_easy_cleanup(curlContext.curl);

	dbgprint("Video Thread End\n");
	return 0;
}
