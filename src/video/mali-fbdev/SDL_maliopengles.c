#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_MALI && SDL_VIDEO_OPENGL_EGL

#include "SDL_maliopengles.h"
#include "SDL_malivideo.h"

#include <sys/mman.h>

/* EGL implementation of SDL OpenGL support */
static void
MALI_GetAspectCorrected(struct rectangle_s *rect, int w, int h, int flip)
{
    float ratio_x, ratio_y;
    if (flip) {
        int tmp = h;
        h = w;
        w = tmp;
    }

    ratio_x = (float)rect->w / w;
    ratio_y = (float)rect->h / h;
    if (ratio_y < ratio_x) {
        int new_w = w * ratio_y;
        rect->x += (rect->w - new_w) / 2;
        rect->w = new_w;
    } else {
        int new_h = h * ratio_x;
        rect->y += (rect->h - new_h) / 2;
        rect->h = new_h;
    }
}

void
MALI_Rotate_Blit(_THIS, SDL_Window *window, MALI_EGL_Surface *target, int dst_index, int rotation, int aspect)
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
    blit_config.dst_para.top = displaydata->vinfo.yres * dst_index;
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

    blitRect.dst_rect.x = blit_config.dst_para.left;
    blitRect.dst_rect.y = blit_config.dst_para.top;
    blitRect.dst_rect.w = blit_config.dst_para.width;
    blitRect.dst_rect.h = blit_config.dst_para.height;

    if (aspect)
        MALI_GetAspectCorrected(&blitRect.dst_rect, window->w, window->h, rotation & 1);

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
        if (_this->egl_data->eglClientWaitSyncKHR(
            _this->egl_data->egl_display,
            current_surface->fence, 
            EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 
            (EGLTimeKHR)1e+8) == EGL_CONDITION_SATISFIED_KHR) {
            if (current_surface->needs_clear) {
                struct fb_fix_screeninfo finfo = {};
                void *fb;
                
                // Clear the frame once right before a new frame is in to avoid flickering
                if (ioctl(displaydata->fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
                    MALI_VideoQuit(_this);
                    return SDL_SetError("mali-fbdev: Could not clear framebuffer.");
                }

                fb = mmap(0, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, displaydata->fb_fd, 0);
                if (fb != MAP_FAILED) {
                    uintptr_t frame_len = displaydata->vinfo.yres * displaydata->vinfo.xres * 4;
                    uintptr_t off = (uintptr_t)fb + (frame_len * displaydata->cur_fb);
                    memset((void*)off, 0, frame_len);
                    munmap(fb, finfo.smem_len);
                } else {
                    MALI_VideoQuit(_this);
                    return SDL_SetError("mali-fbdev: Could not clear framebuffer.");      
                }

                current_surface->needs_clear = 0;
            }

            MALI_Rotate_Blit(data, _this->windows, current_surface, displaydata->cur_fb, displaydata->rotation, displaydata->aspect);

            displaydata->vinfo.yoffset = displaydata->vinfo.yres * displaydata->cur_fb;
            ioctl(displaydata->fb_fd, FBIOPUT_VSCREENINFO, &displaydata->vinfo);
#if 0
            if (windowdata->swapInterval)
                ioctl(displaydata->fb_fd, FBIO_WAITFORVSYNC, 0);
#endif
            displaydata->cur_fb = !displaydata->cur_fb;
        }
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
    EGLSurface surf;
    SDL_WindowData *windowdata;

    windowdata = (SDL_WindowData*)_this->windows->driverdata;

    // Create the fence to signal frame completion
    windowdata->surface[windowdata->flip_page].fence = _this->egl_data->eglCreateSyncKHR(_this->egl_data->egl_display, EGL_SYNC_FENCE_KHR, NULL);
    SDL_LockMutex(windowdata->triplebuf_mutex);

    page = windowdata->new_page;
    windowdata->new_page = windowdata->flip_page;
    windowdata->flip_page = page;

    surf = windowdata->surface[windowdata->flip_page].egl_surface;
    r = _this->egl_data->eglMakeCurrent(_this->egl_data->egl_display, surf, surf, _this->current_glctx);

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
        return SDL_EGL_MakeCurrent(_this, windowdata->surface[windowdata->flip_page].egl_surface, context);

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
