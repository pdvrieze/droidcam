/* DroidCam & DroidCamX (C) 2010-
 * https://github.com/aramg
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * Use at your own risk. See README file for more details.
 */

#ifndef _COMMON_H_
#define _COMMON_H_

#include "config.h"

extern void ShowError(const char *title, const char *msg);

#define MSG_ERROR(str)     ShowError("Error",str)
#define MSG_LASTERROR(str) ShowError(str,strerror(errno))

#define VIDEO_REQ "CMD /v2/video?%dx%d"
#define OTHER_REQ "CMD /v1/ctl?%d"

#undef MAX_COMPONENTS
#define MAX_COMPONENTS  4

class Dimension {
public:
	Dimension() = default;

	Dimension(unsigned int w, unsigned int h) : width(w), height(h)
	{};

	Dimension(const Dimension &d) = default;;

	unsigned int width;
	unsigned int height;
};

inline unsigned int make_int(unsigned char b1, unsigned char b2)
{
	return (b1 & 0xFFu) << 8u | (b2 & 0xFFu);
}

inline unsigned int make_int4(unsigned char b0, unsigned char b1, unsigned char b2, unsigned char b3)
{
	return ((b3 & 0xFFu) << 24u) | ((b2 & 0xFFu) << 16u) | ((b1 & 0xFFu) << 8u) | (b0 & 0xFFu);
}

#define errprint(...) fprintf(stderr, __VA_ARGS__)
#define voidprint(...) /* */
#ifndef NDEBUG
#define dbgprint errprint
#else
#define dbgprint voidprint
#endif

#endif
