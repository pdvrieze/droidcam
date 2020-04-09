/* DroidCam & DroidCamX (C) 2010-
 * https://github.com/aramg
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * Use at your own risk. See README file for more details.
 */
#ifndef __CONN_H__
#define __CONN_H__

#include <sys/types.h>

#define INVALID_SOCKET -1

typedef int SOCKET;

SOCKET connect_droidcam(char * ip, unsigned int port);
void connection_cleanup();
void disconnect(SOCKET s);

SOCKET accept_connection(unsigned int port, bool *running);

//int SendRecv(int doSend, char * buffer, int bytes, SOCKET s);

ssize_t sendToSocket(char * buffer, size_t count, SOCKET s);
ssize_t recvFromSocket(char * buffer, size_t count, SOCKET s);

#endif
