#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_MALI && SDL_VIDEO_OPENGL_EGL

#include "SDL_maliopengles.h"
#include "SDL_malivideo.h"

/* EGL implementation of SDL OpenGL support */
void MALI_Rotate_Blit(_THIS, SDL_Window *window, MALI_EGL_Surface *target, int rotation)
{
    int io;
    static struct ge2d_para_s blitRect = {};
    static struct config_para_ex_ion_s blit_config = {};
    SDL_DisplayData *displaydata;

    displaydata = SDL_GetDisplayDriverData(0);

    blit_config.alu_const_color = (uint32_t)~0x0;

    // Definitions for the destionation buffer
    blit_config.dst_para.mem_type = CANVAS_OSD0;
    blit_config.dst_para.format = GE2D_FORMAT_S32_ARGB;

    blit_config.dst_para.left = 0;
    blit_config.dst_para.top = displaydata->vinfo.yres * displaydata->cur_fb;
    blit_config.dst_para.width = displaydata->vinfo.xres;
    blit_config.dst_para.height = displaydata->vinfo.yres;
    blit_config.dst_para.x_rev = 0;
    blit_config.dst_para.y_rev = 0;

    switch (rotation)
    {
        // OpenGL is flipped...
        case Rotation_0:
            blit_config.dst_para.y_rev = 1;
            break;

        case Rotation_90:
            blit_config.dst_xy_swap = 1;
            blit_config.dst_para.y_rev = 1;
            blit_config.dst_para.x_rev = 1;
            break;

        case Rotation_180:
            blit_config.dst_para.x_rev = 1;
            break;

        case Rotation_270:
            blit_config.dst_xy_swap = 1;
            break;
            
        default:
            break;
    }

    // Definitions for the source buffers
    blit_config.src_para.mem_type = CANVAS_ALLOC;
    blit_config.src_para.format = GE2D_FORMAT_S32_ARGB;

    blit_config.src_para.left = 0;
    blit_config.src_para.top = 0;
    blit_config.src_para.width = window->w;
    blit_config.src_para.height = window->h;

    blit_config.src_planes[0].shared_fd = target->shared_fd;
    blit_config.src_planes[0].w = target->pixmap.planes[0].stride / 4;
    blit_config.src_planes[0].h = target->pixmap.height;

    io = ioctl(displaydata->ge2d_fd, GE2D_CONFIG_EX_ION, &blit_config);
    if (io < 0)
    {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "GE2D_CONFIG failed.");
        abort();
    }

    blitRect.src1_rect.x = 0;
    blitRect.src1_rect.y = 0;
    blitRect.src1_rect.w = target->pixmap.width;
    blitRect.src1_rect.h = target->pixmap.height;

    blitRect.dst_rect.x = 0;
    blitRect.dst_rect.y = displaydata->vinfo.yres * displaydata->cur_fb;
    blitRect.dst_rect.w = blit_config.dst_para.width;
    blitRect.dst_rect.h = blit_config.dst_para.height;

    io = ioctl(displaydata->ge2d_fd, GE2D_STRETCHBLIT_NOALPHA, &blitRect);
    if (io < 0)
    {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "GE2D Blit failed.");
        abort();
    }
}

int MALI_TripleBufferingThread(void *data)
{
    unsigned int page;
    MALI_EGL_Surface *current_surface;
	SDL_WindowData *windowdata;
    SDL_DisplayData *displaydata;
    SDL_VideoDevice* _this;
    
    _this = (SDL_VideoDevice*)data;
    windowdata = (SDL_WindowData *)_this->windows->driverdata;
    displaydata = SDL_GetDisplayDriverData(0);

	SDL_LockMutex(windowdata->triplebuf_mutex);
	SDL_CondSignal(windowdata->triplebuf_cond);

	for (;;) {
        SDL_CondWait(windowdata->triplebuf_cond, windowdata->triplebuf_mutex);
		if (windowdata->triplebuf_thread_stop)
			break;

		/* Flip the most recent back buffer with the front buffer */
		page = windowdata->current_page;
		windowdata->current_page = windowdata->new_page;
		windowdata->new_page = page;

        /* select surface to wait and blit */
        current_surface = &windowdata->surface[windowdata->current_page];

		/* wait for fence and flip display */
        _this->egl_data->eglClientWaitSyncKHR(
            _this->egl_data->egl_display,
            current_surface->fence, 
            EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 
            (EGLTimeKHR)1e+8);
        
        /* blit, flip and wait for vsync if needed */
        MALI_Rotate_Blit(data, _this->windows, current_surface, Rotation_0);

		displaydata->vinfo.yoffset = displaydata->vinfo.yres * displaydata->cur_fb;
		ioctl(displaydata->fb_fd, FBIOPUT_VSCREENINFO, &displaydata->vinfo);
#if 0
        if (windowdata->swapInterval)
            ioctl(displaydata->fb_fd, FBIO_WAITFORVSYNC, 0);
#endif
        displaydata->cur_fb = !displaydata->cur_fb;
	}

	SDL_UnlockMutex(windowdata->triplebuf_mutex);
	return 0;
}

void MALI_TripleBufferInit(SDL_WindowData *windowdata)
{
	windowdata->triplebuf_mutex = SDL_CreateMutex();
	windowdata->triplebuf_cond = SDL_CreateCond();
	windowdata->triplebuf_thread = NULL;
}

void MALI_TripleBufferStop(_THIS)
{
    SDL_WindowData *windowdata = (SDL_WindowData*)_this->windows->driverdata;
    if (windowdata) {
        SDL_LockMutex(windowdata->triplebuf_mutex);
        windowdata->triplebuf_thread_stop = 1;
        SDL_CondSignal(windowdata->triplebuf_cond);
        SDL_UnlockMutex(windowdata->triplebuf_mutex);

        SDL_WaitThread(windowdata->triplebuf_thread, NULL);
        windowdata->triplebuf_thread = NULL;
    }
}

void MALI_TripleBufferQuit(_THIS)
{
    SDL_WindowData *windowdata = (SDL_WindowData*)_this->windows->driverdata;

	if (windowdata->triplebuf_thread)
		MALI_TripleBufferStop(_this);
	SDL_DestroyMutex(windowdata->triplebuf_mutex);
	SDL_DestroyCond(windowdata->triplebuf_cond);
}

int MALI_GLES_LoadLibrary(_THIS, const char *path)
{
    return SDL_EGL_LoadLibrary(_this, path, EGL_DEFAULT_DISPLAY, 0);
}

int MALI_GLES_SwapWindow(_THIS, SDL_Window * window)
{
    int r;
    unsigned int page;
    SDL_WindowData *windowdata;

    windowdata = (SDL_WindowData*)_this->windows->driverdata;

    SDL_LockMutex(windowdata->triplebuf_mutex);

    page = windowdata->new_page;
    windowdata->new_page = windowdata->flip_page;
    windowdata->flip_page = page;

    r = SDL_EGL_MakeCurrent(_this, windowdata->surface[windowdata->flip_page].egl_surface, _this->current_glctx);
    windowdata->surface[windowdata->new_page].fence = _this->egl_data->eglCreateSyncKHR(_this->egl_data->egl_display, EGL_SYNC_FENCE_KHR, NULL);

    SDL_CondSignal(windowdata->triplebuf_cond);
    SDL_UnlockMutex(windowdata->triplebuf_mutex);

    return r;
}

int
MALI_GLES_MakeCurrent(_THIS, SDL_Window * window, SDL_GLContext context)
{
    SDL_WindowData *windowdata;
    if (window) {
        windowdata = window->driverdata;
        return SDL_EGL_MakeCurrent(_this, windowdata->surface[windowdata->new_page].egl_surface, context);

    } else {
        return SDL_EGL_MakeCurrent(_this, EGL_NO_SURFACE, context);
    }
}

SDL_GLContext
MALI_GLES_CreateContext(_THIS, SDL_Window * window)
{
    SDL_WindowData *windowdata = (SDL_WindowData *)window->driverdata;
    return SDL_EGL_CreateContext(_this, windowdata->surface[windowdata->new_page].egl_surface);
}

#endif /* SDL_VIDEO_DRIVER_MALI && SDL_VIDEO_OPENGL_EGL */
