//
// Created by pdvrieze on 01/04/2020.
//

#include "ipcam.h"
#include "connection.h"
#include "context.h"
#include "common.h"
#include "util.h"
#include "decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <gtk/gtk.h>
#include <curl/curl.h>
#include <string.h>
#include <jmorecfg.h>

typedef enum _State {
	ST_INIT, // nothing done
	ST_BOUNDARY, // Waiting to read the boundary
	ST_HEADERS, // Waiting to read/process sub headers
	ST_IMG, // Part read the image
	ST_ERROR, // Some error occured. Shut down the processing
} State;


typedef struct _ReadBuffer {
	unsigned char *buffer;
	size_t maxSize;
	size_t offset;
	size_t recvCount;
} ReadBuffer;

struct CurlContext {
	CURL *easy;
	CURLM *multi;
	curl_socket_t socket;
	DCContext *dcContext;
	char boundary[256];
	State state;
	ReadBuffer b;
	Buffer out;
};

static const int BUFSIZE = 32 * 1024;
static const int LINESIZE = 1 * 1024;
static const int FRAME_MAX = 4 * 1024;
static const char CR = 13;
static const char LF = 10;
static const char CRLF[] = {CR, LF, 0};

gboolean startsWith(const char *haystack, const char *needle)
{
	return haystack != NULL && strncmp(needle, haystack, strlen(needle)) == 0;
}

static int wait_on_recv_socket(curl_socket_t sockfd, long timeout_ms)
{
	fd_set inFds, errFds;
	FD_ZERO(&inFds);
	FD_ZERO(&errFds);
	FD_SET(sockfd, &inFds);
	FD_SET(sockfd, &errFds);

	struct timeval tv = {.tv_sec=timeout_ms / 1000, .tv_usec=(timeout_ms % 1000) * 1000};
	return select(sockfd + 1, &inFds, NULL, &errFds, &tv);
}

static size_t min(size_t a, size_t b)
{
	return a < b ? a : b;
}

static size_t max(size_t a, size_t b)
{
	return a > b ? a : b;
}

static gboolean readIntoBuffer(ReadBuffer *b, const char *data, size_t size)
{
	if (b->buffer != NULL) {
		b->recvCount -= b->offset;
		memmove(b->buffer, b->buffer + b->offset, b->recvCount);
		memset(b->buffer + b->recvCount, 0, b->maxSize - b->recvCount); // Set to zero
	}
	b->offset = 0;
	size_t needed_size = b->recvCount + size;
	if (needed_size > b->maxSize) {
		size_t s = max(b->maxSize, 1024);
		while (s < needed_size) { s <<= 1; }
		b->buffer = static_cast<unsigned char *>(realloc(b->buffer, s));
		b->maxSize = s;
	}
	memcpy(b->buffer + b->recvCount, data, size);
	b->recvCount += size;
	return true;
}

static gboolean readIntoBuffer_Old(ReadBuffer *b, CurlContext *curlContext, size_t minRecv)
{
	while (curlContext->dcContext->running && b->recvCount < minRecv) {
		// Read first buffer content
		size_t recvCount;
		CURLcode res = curl_easy_recv(curlContext->easy, b->buffer + b->recvCount, b->maxSize - b->recvCount,
		                              &recvCount);
		while (res == CURLE_AGAIN && curlContext->dcContext->running) {
			int retval = wait_on_recv_socket(curlContext->socket, 250);// quarter of a second

			if (retval == -1) {
				MSG_ERROR("select");
				return false;
			}

			if (retval && curlContext->dcContext->running) {
				res = curl_easy_recv(curlContext->easy, b->buffer + b->recvCount, b->maxSize - b->recvCount,
				                     &recvCount);
			}
		}
		if (res == CURLE_OK) {
			b->recvCount += recvCount;
		}
	}
	return true;
}

static gboolean consumeLine(CurlContext *curlContext, char *line, size_t maxLine)
{
	ReadBuffer *b = &(curlContext->b);
	unsigned char *last = b->buffer + b->recvCount - 1; // extra space for LF
	unsigned char *cur;
	for (cur = b->buffer + b->offset; cur < last && (cur[0] != CR || cur[1] != LF); ++cur) {}
	if (cur[0] == CR && cur[1] == LF) {
		size_t n = min(cur - b->buffer, maxLine - 1);
		strncpy(line, (char *) b->buffer + b->offset, n);
		line[n - b->offset] = 0; // ensure terminating 0
		b->offset = cur - b->buffer + 2; // add 2 to skip CR/LF
		return true; // skip CRLF
	} else {
		return false; // no move
	}
}

static gboolean consumeBoundary(CurlContext *curlContext)
{
	ReadBuffer *b = &curlContext->b;

	const char *boundary = curlContext->boundary;
	size_t maxOffset = 2 + strlen(boundary);
	if (b->recvCount-b->offset<maxOffset) { return false; } // Read more data

	const unsigned char *lastAvail = b->buffer + b->recvCount;

	const unsigned char *current = b->buffer + b->offset;

	size_t offset = 0;

	while (offset < maxOffset && current < lastAvail) {
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
	if (current == lastAvail) {
		return false; // We need to read more, not an error
	}
	if (offset >= maxOffset) {
		b->offset += offset;
		if (b->buffer[b->offset] == CR) ++(b->offset);
		if (b->buffer[b->offset] == LF) ++(b->offset);
		curlContext->state = ST_HEADERS;
		return true;
	} else {
		dbgprint("Could not consume boundary\n");
		curlContext->state = ST_ERROR;
		return false;
	}
}

static gboolean consumeSubHeader(CurlContext *curlContext)
{
	size_t contentLength = 0;
	char line[LINESIZE];
	while (consumeLine(curlContext, line, sizeof(line)) && line[0] != 0) {
		if (startsWith(line, "Content-Length:")) {
			char *endPtr;
			contentLength = strtol(&line[15], &endPtr, 10);
			if (endPtr == line) {
				dbgprint("Could not consume the per frame headers\n");
				curlContext->state = ST_ERROR;
				return false;
			}
		}
	}
	if (line[0] != 0) { // We didn't read the entire header yet
		return false;
	}
	if (contentLength==0) {
		// skip until next boundary
		curlContext->state = ST_BOUNDARY;
		return true;
	}
	Buffer *resultPtr = &curlContext->out;
	if (resultPtr->data == NULL) {
		resultPtr->data = static_cast<unsigned char *>(malloc(contentLength));
		resultPtr->buf_size = contentLength;
	} else if (contentLength > resultPtr->buf_size || (contentLength < (resultPtr->buf_size / 4))) {
		resultPtr->data = static_cast<unsigned char *>(realloc(resultPtr->data, contentLength));
		if (resultPtr->data == NULL) {
			dbgprint("Error reallocating memory from %lu to %lu bytes\n", resultPtr->buf_size, contentLength);
			curlContext->state = ST_ERROR;
			return false;
		}
		resultPtr->buf_size = contentLength;
	}
	resultPtr->data_length = contentLength;

	curlContext->state = ST_IMG;
	return true;
}

bool readBoundary(CurlContext *curlContext)
{
	char *contentType;
	CURLcode res = curl_easy_getinfo(curlContext->easy, CURLINFO_CONTENT_TYPE, &contentType);
	if (res != CURLE_OK) { return 0; }
	if (!startsWith(contentType, "multipart/x-mixed-replace;")) {
		MSG_ERROR("Unexpected content type in ipcam");
		return false;
	}

	char *start = strstr(contentType, "boundary");
	char *end = start + strlen(start);
	if (start == NULL) {
		MSG_ERROR("Missing boundary");
		return false;
	}
	for (start += 8; *start == ' ' && (start < end); ++start) {}
	if (*start != '=') {
		MSG_ERROR("No equals for boundary");
		return false;
	}
	do {
		++start;
	} while (start < end && (*start) == ' ');

	strncpy(curlContext->boundary, start, sizeof(curlContext->boundary));
	dbgprint("Found boundary: '%s'\n", curlContext->boundary);
	return true;
}

gboolean consumeFrame(CurlContext *curlContext)
{

	ReadBuffer *b = &curlContext->b;
	Buffer *resultPtr = &curlContext->out;
	size_t contentLength = resultPtr->data_length;

	g_assert(b->recvCount >= contentLength);
	memcpy(resultPtr->data, b->buffer + b->offset, contentLength);
	b->offset += contentLength;
	if (b->recvCount > b->offset) {
		memmove(b->buffer, b->buffer + b->offset, b->recvCount - b->offset);
		b->recvCount -= contentLength;
		b->offset = 0;
	}

	if (curlContext->dcContext->decoder->jpg_frames[0].data == nullptr) { // Not initialized
		if (curlContext->dcContext->decoder->prepareVideoFromFrame(resultPtr) == false) {
			dbgprint("Could not prepare video from frame\n");
			curlContext->state = ST_ERROR;
			return false;
		}
	}

	curlContext->state = ST_BOUNDARY; // now look for boundary again
	return true;
}

void sendFrame(CurlContext *curlContext)
{
	Decoder *decoder = curlContext->dcContext->decoder;
	if (decoder->outputMode() == OutputMode::OM_V4LLOOPBACK) {
		unsigned int srcWidth = decoder->srcWidth();
		unsigned int srcHeight = decoder->srcHeight();

/*
		if (srcWidth != decoder->loopbackWidth() || srcHeight != decoder->loopbackHeight()) {
			// If the size changed, reinitialize (not clear that this can happen)
			decoder->initLoopback(srcWidth, srcHeight);
		}
*/
	}
	Buffer *frame = decoder->getNextFrame();
	frame->data_length = curlContext->out.data_length;
	memcpy(frame->data, curlContext->out.data, frame->data_length); // Actually put the JPEG into the buffer
}

size_t processData(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	g_assert(size == 1);
	CurlContext *curlContext = static_cast<CurlContext *>(userdata);
	if (curlContext->boundary[0] == 0) {
		g_assert(curlContext->state == ST_INIT);
		readBoundary(curlContext);
		curlContext->state = ST_BOUNDARY;
	}
	ReadBuffer *b = &curlContext->b;

	readIntoBuffer(b, ptr, nmemb);

	gboolean continueLoop = true;
	while (continueLoop &&
	       b->offset < b->recvCount &&
	       curlContext->state != ST_ERROR &&
	       curlContext->dcContext->running) {

		switch (curlContext->state) {
			case ST_INIT: // we shouldn't be in this state
				return 0;
			case ST_HEADERS:
				continueLoop = consumeSubHeader(curlContext);
				break;
			case ST_BOUNDARY:
				continueLoop = consumeBoundary(curlContext);
				break;
			case ST_IMG:
				if (b->recvCount - b->offset >= curlContext->out.data_length) {
					continueLoop = consumeFrame(curlContext);
					sendFrame(curlContext);
					g_assert(curlContext->state == ST_BOUNDARY);
				} else {
					continueLoop = false; // no full image
				}
				break;
			default:
				return 0; // broken
		}

	}
	if (!curlContext->dcContext->running) {
		dbgprint("Stopping video reading because of input request\n");
		return 0;
	}
	if (curlContext->state == ST_ERROR) {
		dbgprint("Stopping video reading because of error\n");
		return 0;
	}
	return nmemb;
}

int progress(void *clientp,   curl_off_t dltotal,   curl_off_t dlnow,   curl_off_t ultotal,   curl_off_t ulnow) {
	auto curlContext = static_cast<CurlContext *>(clientp);
	if (! curlContext->dcContext->running) {
		return 1;
	} else {
		dbgprint(dlnow==0 ? "." : "*");
	}

	return 0;
}

void *ipcamVideoThreadProc(void *args)
{
	DCContext *context;
	Settings *settings;
	std::string hostName;
	{
		auto *threadargs = static_cast<ThreadArgs *>(args);
		context = threadargs->context;
		settings = &(context->settings);
		hostName = threadargs->hostName;
		delete threadargs;
		args = 0;
	}

	dbgprint("Video Thread Started\n");
	context->running = 1;


	char url[256];
	snprintf(url, 255, "http://%s:%d/video", hostName.data(), settings->port);
	dbgprint("Connecting to: %s\n", url);

	if (curl_global_init(CURL_GLOBAL_NOTHING)) { return 0; }


	CurlContext curlContext{};
	curlContext.easy = curl_easy_init();
	curlContext.boundary[0] = 0;
	curlContext.dcContext = context;


	curl_easy_setopt(curlContext.easy, CURLOPT_URL, url);
	curl_easy_setopt(curlContext.easy, CURLOPT_WRITEFUNCTION, processData);
	curl_easy_setopt(curlContext.easy, CURLOPT_WRITEDATA, &curlContext);
	curl_easy_setopt(curlContext.easy, CURLOPT_NOPROGRESS, 0);
	curl_easy_setopt(curlContext.easy, CURLOPT_XFERINFOFUNCTION, progress);
	curl_easy_setopt(curlContext.easy, CURLOPT_XFERINFODATA, &curlContext);
//	curl_easy_setopt(curlContext.easy, CURLOPT_VERBOSE, 1L);

//	curl_multi_add_handle(curlContext.multi, curlContext.easy);

	CURLcode res = curl_easy_perform(curlContext.easy);
	if (res!=CURLE_OK) {
		dbgprint("Curl error: %d\n", res);
	}

	early_out:
	dbgprint("disconnect\n");

	FREE_OBJECT(curlContext.b.buffer, free)
	FREE_OBJECT(curlContext.out.data, free)

	context->decoder->cleanupJpeg();
	bool result;
	result = context->decoder->init();

	curl_easy_cleanup(curlContext.easy);

	dbgprint("Video Thread End\n");
	return 0;
}
