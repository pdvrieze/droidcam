/* DroidCam & DroidCamX (C) 2010-
 * https://github.com/aramg
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * Use at your own risk. See README file for more details.
 */

#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include "common.h"
#include "connection.h"

SOCKET wifiServerSocket = INVALID_SOCKET;

SOCKET connect_droidcam(char * ip, unsigned int port)
{
    struct sockaddr_in sin;
    SOCKET sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);

    printf("connecting to %s:%d\n", ip, port);
    if(sock == INVALID_SOCKET) {
        MSG_LASTERROR("Error");
    } else {
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = inet_addr(ip);
        sin.sin_port = htons(port);

        if (connect(sock, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
            printf("connect failed %d '%s'\n", errno, strerror(errno));
            MSG_ERROR("Connect failed, please try again.\nCheck IP and Port.\nCheck network connection.");
            close(sock);
            sock = INVALID_SOCKET;
        }
    }

    dbgprint(" - return fd: %d\n", sock);
    return sock;
}

static ssize_t sendRecvImpl(int doSend, char * buffer, int bytes, SOCKET s)
{
    int retCode;
    char * ptr = buffer;

    while (bytes > 0) {
        retCode = (doSend) ? send(s, ptr, bytes, 0) : recv(s, ptr, bytes, 0);
        if (retCode <= 0 ){ // closed or error
            return retCode;
        }
        ptr += retCode;
        bytes -= retCode;
    }

    return 1;
}

ssize_t sendToSocket(char * buffer, size_t count, SOCKET s) {
	return sendRecvImpl(1, buffer, count, s);
}
ssize_t recvFromSocket(char * buffer, size_t count, SOCKET s)
{
	return sendRecvImpl(0, buffer, count, s);
}

static int StartInetServer(int port)
{
    int flags = 0;
    struct sockaddr_in sin;

    sin.sin_family    = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port        = htons(port);

    wifiServerSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(wifiServerSocket == INVALID_SOCKET)
    {
        MSG_LASTERROR("Could not create socket");
        goto _error_out;
    }

    if(bind(wifiServerSocket, (struct sockaddr*)&sin, sizeof(sin)) < 0)
    {
        MSG_LASTERROR("Error: bind");
        goto _error_out;
    }
    if(listen(wifiServerSocket, 1) < 0)
    {
        MSG_LASTERROR("Error: listen");
        goto _error_out;
    }

    flags = fcntl(wifiServerSocket, F_GETFL, nullptr);
    if(flags < 0)
    {
        MSG_LASTERROR("Error: fcntl");
        goto _error_out;
    }
    flags |= O_NONBLOCK;
    fcntl(wifiServerSocket, F_SETFL, flags);

    return 1;

_error_out:
    if (wifiServerSocket != INVALID_SOCKET){
        close(wifiServerSocket);
        wifiServerSocket = INVALID_SOCKET;
    }

    return 0;
}

void connection_cleanup() {
    if (wifiServerSocket != INVALID_SOCKET) {
        close(wifiServerSocket);
        wifiServerSocket = INVALID_SOCKET;
    }
}

void disconnect(SOCKET s) {
    close(s);
}

SOCKET accept_connection(unsigned int port, bool *running)
{
    int flags;
    SOCKET client =  INVALID_SOCKET;

    dbgprint("serverSocket=%d\n", wifiServerSocket);
    if (wifiServerSocket == INVALID_SOCKET && !StartInetServer(port))
        goto _error_out;

    errprint("waiting on port %d..", port);
    while(*running && (client = accept(wifiServerSocket, nullptr, nullptr)) == INVALID_SOCKET)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR){
            usleep(50000);
            continue;
        }
        MSG_LASTERROR("Accept Failed");
        break;
    }
    errprint("got socket %d\n", client);

    if (client != INVALID_SOCKET) {
        // Blocking..
        flags = fcntl(wifiServerSocket, F_GETFL, nullptr);
        flags |= O_NONBLOCK;
        fcntl(wifiServerSocket, F_SETFL, flags);
    }

_error_out:
    return client;
}
