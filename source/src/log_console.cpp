/*-------------------------------------------------------------
 
Copyright (C) 2008 dhewg
 
This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any
damages arising from the use of this software.
 
Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:
 
1. The origin of this software must not be misrepresented; you
must not claim that you wrote the original software. If you use
this software in a product, an acknowledgment in the product
documentation would be appreciated but is not required.
 
2. Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.
 
3. This notice may not be removed or altered from any source
distribution.
 
-------------------------------------------------------------*/
 
#include <sys/iosupport.h>
#include <reent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
 
#include "log_console.h"
 
static bool gecko = false;
static const devoptab_t *dot_video = NULL;
static VIRetraceCallback rcb = NULL;
static char **log = NULL;
static u16 log_size = 0;
static u16 log_next = 0;
static bool log_active = true;
static bool video_active = true;
 
static int __out_write(struct _reent *r, void* fd, const char *ptr, size_t len) {
	 
	if (!ptr || len <= 0)
		return -1;
	 
	if (video_active) {
		dot_video->write_r(r, fd, ptr, len);
	} else {
		if (log_active) {
			u16 l = (log_next + 1) % log_size;
			if (log[l])
			free(log[l]);
			log[l] = strndup(ptr, len);
			 
			log_next = l;
		}
	}
 
	if (gecko)
		usb_sendbuffer(1, ptr, len);
	 
	return len;
}
 
const devoptab_t dot_out = {
	"stdout",    // device name
	0,           // size of file structure
	NULL,        // device open
	NULL,        // device close
	__out_write, // device write
	NULL,        // device read
	NULL,        // device seek
	NULL,        // device fstat
	NULL,        // device stat
	NULL,        // device link
	NULL,        // device unlink
	NULL,        // device chdir
	NULL,        // device rename
	NULL,        // device mkdir
	0,           // dirStateSize
	NULL,        // device diropen_r
	NULL,        // device dirreset_r
	NULL,        // device dirnext_r
	NULL,        // device dirclose_r
	NULL         // device statvfs_r
};
 
void log_console_init(GXRModeObj *vmode, u16 logsize, u16 x, u16 y, u16 w, u16 h)
{
	 
	CON_InitEx(vmode, x, y, w, h);
	rcb = VIDEO_SetPostRetraceCallback(NULL);
	VIDEO_SetPostRetraceCallback(rcb);
	 
	gecko = usb_isgeckoalive(1);
	 
	if (log_size && log) {
		int i = log_size - 1;
		do{
			if (log[i]){
				free(log[i]);
			}
			--i;
		}while(i >= 0);
		 
		free(log);
	}
	 
	log_size = logsize;
	log_next = 0;
	 
	if (log_size) {
		log = (char **) malloc(log_size * sizeof(char *));
		
		int i = log_size - 1;
		do{
			log[i] = NULL;	
			--i;
		}while(i >= 0);
	}
	 
	log_active = log_size > 0;
	 
	dot_video = devoptab_list[STD_OUT];
	video_active = true;
	 
	devoptab_list[STD_OUT] = &dot_out;
	devoptab_list[STD_ERR] = &dot_out;
}
 
void log_console_deinit(void) {
	 
	if (log_size && log) {

		int i = log_size - 1;
		do{
			if (log[i]){
				free(log[i]);
			}
			--i;
		}while(i >= 0);
		 
		free(log);
		log = NULL;
	}
	 
	log_size = 0;
	log_next = 0;
	 
	devoptab_list[STD_OUT] = dot_video;
	devoptab_list[STD_ERR] = dot_video;
	 
	// VIDEO_SetPostRetraceCallback(rcb);
	 
	dot_video = NULL;
}
 
void log_console_enable_log(bool enable) {
	if (!log_size)
		return;
	 
	log_active = enable;
}
 
void log_console_enable_video(bool enable) {
	if (video_active == enable)
		return;

	video_active = enable;
	 
	if (enable)
		VIDEO_SetPostRetraceCallback(rcb);
	else
		VIDEO_SetPostRetraceCallback(NULL);
	 
	if (!enable || !log_size)
		return;

	struct _reent *r = _REENT;

	for (u16 i = 0; i < log_size; ++i) {
		u16 l = (log_next + 1 + i) % log_size;
		if (log[l]) {
			dot_video->write_r(r, 0, log[l], strlen(log[l]));
			free(log[l]);
			log[l] = NULL;
		}
	}
	 
	fflush(stdout);
}
 
 
