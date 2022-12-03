#ifndef _SDL_malivideo_h
#define _SDL_malivideo_h

#include "../../SDL_internal.h"
#include "../SDL_sysvideo.h"

#include "SDL_egl.h"

#include <EGL/egl.h>
#include <linux/vt.h>
#include <linux/fb.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#include "mali.h"
#include "ion.h"
#include "ge2d.h"
#include "ge2d_cmd.h"

typedef struct SDL_DisplayData
{
    struct fb_var_screeninfo vinfo;
    int cur_fb;
    struct fbdev_window_s native_display;
    NativePixmapType (*egl_create_pixmap_ID_mapping)(mali_pixmap *);
    NativePixmapType (*egl_destroy_pixmap_ID_mapping)(int id);

    int rotation;
    int ge2d_fd, ion_fd, fb_fd;
} SDL_DisplayData;

typedef struct MALI_EGL_Surface
{
    // A pixmap is backed by multiple ION allocated backbuffers, EGL fences, etc.
    EGLSyncKHR fence;
    EGLSurface egl_surface;
    NativePixmapType pixmap_handle;
    mali_pixmap pixmap;
    int needs_clear;
    int shared_fd;
    int handle;
} MALI_EGL_Surface;

typedef struct SDL_WindowData
{
    int swapInterval;
	int flip_page;
	int current_page;
	int new_page;
	SDL_mutex *triplebuf_mutex;
	SDL_cond *triplebuf_cond;
	SDL_Thread *triplebuf_thread;
	int triplebuf_thread_stop;
    int prev_w, prev_h;

    MALI_EGL_Surface surface[3];

    // The created EGL Surface is backed by a mali pixmap
} SDL_WindowData;

/****************************************************************************/
/* SDL_VideoDevice functions declaration                                    */
/****************************************************************************/

/* Display and window functions */
int MALI_VideoInit(_THIS);
void MALI_VideoQuit(_THIS);
void MALI_GetDisplayModes(_THIS, SDL_VideoDisplay * display);
int MALI_SetDisplayMode(_THIS, SDL_VideoDisplay * display, SDL_DisplayMode * mode);
int MALI_CreateWindow(_THIS, SDL_Window * window);
void MALI_SetWindowTitle(_THIS, SDL_Window * window);
void MALI_SetWindowPosition(_THIS, SDL_Window * window);
void MALI_SetWindowSize(_THIS, SDL_Window * window);
void MALI_SetWindowFullscreen(_THIS, SDL_Window * window, SDL_VideoDisplay * display, SDL_bool fullscreen);
void MALI_ShowWindow(_THIS, SDL_Window * window);
void MALI_HideWindow(_THIS, SDL_Window * window);
void MALI_DestroyWindow(_THIS, SDL_Window * window);
void MALI_TripleBufferInit(SDL_WindowData *);
void MALI_TripleBufferStop(SDL_VideoDevice *);
int MALI_TripleBufferingThread(void *data);

/* Window manager function */
SDL_bool MALI_GetWindowWMInfo(_THIS, SDL_Window * window,
                             struct SDL_SysWMinfo *info);

/* Event functions */
void MALI_PumpEvents(_THIS);

#endif /* _SDL_malivideo_h */

