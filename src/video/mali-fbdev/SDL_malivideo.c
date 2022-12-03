#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_MALI

/* SDL internals */
#include "../SDL_sysvideo.h"
#include "SDL_version.h"
#include "SDL_syswm.h"
#include "SDL_loadso.h"
#include "SDL_events.h"
#include "../../events/SDL_events_c.h"

#ifdef SDL_INPUT_LINUXEV
#include "../../core/linux/SDL_evdev.h"
#endif

#include "SDL_malivideo.h"
#include "SDL_maliopengles.h"

void
MALI_GLES_DefaultProfileConfig(_THIS, int *mask, int *major, int *minor)
{
    if (!SDL_getenv("SDL_DEFAULT_CONTEXT_PROFILE"))
    {
        *mask = SDL_GL_CONTEXT_PROFILE_ES;
        *major = 2;
        *minor = 0;
    }
}

static void
MALI_Destroy(SDL_VideoDevice * device)
{
    if (device->driverdata != NULL) {
        SDL_free(device->driverdata);
        device->driverdata = NULL;
    }
}

static int
MALI_GLES_SetSwapInterval(_THIS, int interval)
{
    SDL_WindowData *windowdata;
    if (!_this->windows)
        return 0;

    windowdata = (SDL_WindowData *)_this->windows->driverdata;
    windowdata->swapInterval = interval != 0;
    return windowdata->swapInterval;
}

static int
MALI_GLES_GetSwapInterval(_THIS)
{
    SDL_WindowData *windowdata;
    if (!_this->windows)
        return 0;

    windowdata = (SDL_WindowData *)_this->windows->driverdata;
    return windowdata->swapInterval;
}


static SDL_VideoDevice *
MALI_Create()
{
    SDL_VideoDevice *device;

    /* Initialize SDL_VideoDevice structure */
    device = (SDL_VideoDevice *) SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (device == NULL) {
        SDL_OutOfMemory();
        return NULL;
    }

    device->driverdata = NULL;

    /* Setup amount of available displays and current display */
    device->num_displays = 0;

    /* Set device free function */
    device->free = MALI_Destroy;

    /* Setup all functions which we can handle */
    device->VideoInit = MALI_VideoInit;
    device->VideoQuit = MALI_VideoQuit;
    device->GetDisplayModes = MALI_GetDisplayModes;
    device->SetDisplayMode = MALI_SetDisplayMode;
    device->CreateSDLWindow = MALI_CreateWindow;
    device->SetWindowTitle = MALI_SetWindowTitle;
    device->SetWindowPosition = MALI_SetWindowPosition;
    device->SetWindowSize = MALI_SetWindowSize;
    device->SetWindowFullscreen = MALI_SetWindowFullscreen;
    device->ShowWindow = MALI_ShowWindow;
    device->HideWindow = MALI_HideWindow;
    device->DestroyWindow = MALI_DestroyWindow;
    device->GetWindowWMInfo = MALI_GetWindowWMInfo;

    device->GL_LoadLibrary = MALI_GLES_LoadLibrary;
    device->GL_GetProcAddress = MALI_GLES_GetProcAddress;
    device->GL_UnloadLibrary = MALI_GLES_UnloadLibrary;
    device->GL_CreateContext = MALI_GLES_CreateContext;
    device->GL_MakeCurrent = MALI_GLES_MakeCurrent;
    device->GL_SetSwapInterval = MALI_GLES_SetSwapInterval;
    device->GL_GetSwapInterval = MALI_GLES_GetSwapInterval;
    device->GL_SwapWindow = MALI_GLES_SwapWindow;
    device->GL_DeleteContext = MALI_GLES_DeleteContext;

    device->GL_DefaultProfileConfig = MALI_GLES_DefaultProfileConfig;

    device->PumpEvents = MALI_PumpEvents;

    return device;
}

VideoBootStrap MALI_bootstrap = {
    "mali",
    "Mali EGL Video Driver",
    MALI_Create
};

/*****************************************************************************/
/* SDL Video and Display initialization/handling functions                   */
/*****************************************************************************/

void
MALI_Reset_Orientation_Rotation(_THIS, SDL_VideoDisplay *display, SDL_DisplayData *data)
{
    const char *orientation, *rotation, *aspect;

    /* 
     * Orientation is the default native display orientation, on ODROID Go Ultra
     * that would be SDL_MALI_ORIENTATION = 1, rotation is the desired rotation on
     * top of that, e.g. SDL_MALI_ROTATION = 1 or SDL_MALI_ROTATION = 3 for TATE modes.
     */
    data->rotation = 0;
    data->aspect = 1;
    orientation = SDL_getenv("SDL_MALI_ORIENTATION");
    rotation = SDL_getenv("SDL_MALI_ROTATION");
    aspect = SDL_getenv("SDL_MALI_FULLSCREEN");
    if (orientation)
        data->rotation = (data->rotation + SDL_atoi(orientation)) % 4;
    if (rotation)
        data->rotation = (data->rotation + SDL_atoi(rotation)) % 4;
    if (aspect)
        data->aspect = SDL_atoi(aspect) == 0;

    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "Setup: rotation: %d aspect: %d", data->rotation, data->aspect);

    if ((data->rotation & 1) == 0) {
        display->current_mode.w = data->vinfo.xres;
        display->current_mode.h = data->vinfo.yres;
    } else {
        display->current_mode.w = data->vinfo.yres;
        display->current_mode.h = data->vinfo.xres;
    }

    display->desktop_mode = display->current_mode;
}

int
MALI_VideoInit(_THIS)
{
    SDL_VideoDisplay display;
    SDL_DisplayMode current_mode;
    SDL_DisplayData *data;

    data = (SDL_DisplayData *) SDL_calloc(1, sizeof(SDL_DisplayData));
    if (data == NULL) {
        return SDL_OutOfMemory();
    }

    data->fb_fd = open("/dev/fb0", O_RDWR, 0);
    if (data->fb_fd < 0) {
        return SDL_SetError("mali-fbdev: Could not open framebuffer device");
    }

    data->ion_fd = open("/dev/ion", O_RDWR, 0);
    if (data->ion_fd < 0) {
        return SDL_SetError("mali-fbdev: Could not open ion device");
    }

    data->ge2d_fd = open("/dev/ge2d", O_RDWR, 0);
    if (data->ge2d_fd < 0) {
        close(data->ion_fd);
        return SDL_SetError("mali-fbdev: Could not open ge2d device");
    }

    if (ioctl(data->fb_fd, FBIOGET_VSCREENINFO, &data->vinfo) < 0) {
        MALI_VideoQuit(_this);
        return SDL_SetError("mali-fbdev: Could not get framebuffer information");
    }
#if 0
    /*
     * This is where we'd usually setup the double buffer props and everything,
     * but we're going to delay that as much as we can to avoid killing any
     * custom splash screen employed by the OS.
     */
    data->vinfo.yres_virtual = data->vinfo.yres * 2;
    if (ioctl(data->fb_fd, FBIOPUT_VSCREENINFO, &data->vinfo) == -1) {
        MALI_VideoQuit(_this);
        return SDL_SetError("mali-fbdev: Unable to setup framebuffer.");
    }
#endif
    system("setterm -cursor off");

    data->native_display.width = data->vinfo.xres;
    data->native_display.height = data->vinfo.yres;

    SDL_zero(current_mode);
    /* FIXME: Is there a way to tell the actual refresh rate? */
    current_mode.refresh_rate = 60;
    /* 32 bpp for default */
    //current_mode.format = SDL_PIXELFORMAT_ABGR8888;
    current_mode.format = SDL_PIXELFORMAT_RGBX8888;

    current_mode.driverdata = NULL;

    SDL_zero(display);
    display.desktop_mode = current_mode;
    display.current_mode = current_mode;
    display.driverdata = data;

    MALI_Reset_Orientation_Rotation(_this, &display, data);
    SDL_AddVideoDisplay(&display, SDL_FALSE);

#ifdef SDL_INPUT_LINUXEV
    if (SDL_EVDEV_Init() < 0) {
        return -1;
    }
#endif

    return 0;
}

void
MALI_VideoQuit(_THIS)
{
    SDL_DisplayData *displaydata = (SDL_DisplayData*)SDL_GetDisplayDriverData(0);
    int fd = open("/dev/tty", O_RDWR);

    /* Cleanup after ion and ge2d */
    if (_this->windows) {
        MALI_DestroyWindow(_this, _this->windows);
    }

    close(displaydata->fb_fd);
    close(displaydata->ion_fd);
    close(displaydata->ge2d_fd);
    //TODO:: Destroy the other buffers...

    /* Clear the framebuffer and ser cursor on again */
    ioctl(fd, VT_ACTIVATE, 5);
    ioctl(fd, VT_ACTIVATE, 1);
    close(fd);
    system("setterm -cursor on");

#ifdef SDL_INPUT_LINUXEV
    SDL_EVDEV_Quit();
#endif

}

void MALI_MaybeRecreate(_THIS, SDL_Window *window, int w, int h)
{
    SDL_WindowData *windowdata;
    if (!window || (windowdata = window->driverdata) == NULL)
        return;

    if (windowdata->prev_w == w && windowdata->prev_h == h)
        return;

    SDL_SendWindowEvent(window, SDL_WINDOWEVENT_RESIZED, w, h);
    window->w = w;
    window->h = h;
    MALI_DestroyWindow(_this, window);
    MALI_CreateWindow(_this, window);
}

void
MALI_GetDisplayModes(_THIS, SDL_VideoDisplay * display)
{
    /* Only one display mode available, the current one */
    SDL_AddDisplayMode(display, &display->current_mode);
}

int
MALI_SetDisplayMode(_THIS, SDL_VideoDisplay * display, SDL_DisplayMode * mode)
{
    SDL_Window *window;
    window = display->fullscreen_window;
    if (!window)
        window = display->device->windows;
    if (!window)
        return 0;

    display->current_mode = *mode;
    MALI_MaybeRecreate(_this, window, mode->w, mode->h);
    return 0;
}


static EGLSurface *MALI_EGL_CreatePixmapSurface(_THIS, int width, int height, SDL_WindowData *windowdata, SDL_DisplayData *displaydata) 
{
    struct ion_fd_data ion_data;
    struct ion_allocation_data allocation_data;
    int i, io, stride;

    _this->egl_data->egl_surfacetype = EGL_PIXMAP_BIT;
    if (SDL_EGL_ChooseConfig(_this) != 0) {
        SDL_SetError("Unable to find a suitable EGL config");
        return EGL_NO_SURFACE;
    }

    if (_this->gl_config.framebuffer_srgb_capable) {
        {
            SDL_SetError("EGL implementation does not support sRGB system framebuffers");
            return EGL_NO_SURFACE;
        }
    }

    // Populate pixmap definitions
    stride = MALI_ALIGN(width * 4, 64);
    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "Creating pixmap (%dx%d stride %d stride/4 %d).", width, height, stride, stride/4);

    for (i = 0; i < 3; i++)
    {
        MALI_EGL_Surface *surf = &windowdata->surface[i];
        surf->pixmap = (mali_pixmap) {
            .width = width,
            .height = height,
            .planes[0] = (mali_plane) {
                .stride = stride,
                .size = stride * height,
                .offset = 0
            },
            .planes[1] = (mali_plane) {},
            .planes[2] = (mali_plane) {},
            .format = MALI_FORMAT_ARGB8888,
            .handles = {-1, -1, -1},
        };

        allocation_data = (struct ion_allocation_data){
            .len = surf->pixmap.planes[0].size,
            .heap_id_mask = (1 << ION_HEAP_TYPE_DMA),
            .flags = 1 << ION_FLAG_CACHED
        };

        io = ioctl(displaydata->ion_fd, ION_IOC_ALLOC, &allocation_data);
        if (io != 0)
        {
            SDL_SetError("Unable to create backing ION buffers");
            return EGL_NO_SURFACE;
        }

        ion_data = (struct ion_fd_data){
            .handle = allocation_data.handle
        };

        io = ioctl(displaydata->ion_fd, ION_IOC_SHARE, &ion_data);
        if (io != 0)
        {
            SDL_SetError("Unable to create backing ION buffers");
            return EGL_NO_SURFACE;
        }

        surf->needs_clear = 1;
        surf->handle = allocation_data.handle;
        surf->shared_fd = ion_data.fd;
        surf->pixmap.handles[0] = ion_data.fd;

        surf->pixmap_handle = displaydata->egl_create_pixmap_ID_mapping(&surf->pixmap);
        SDL_Log("Created pixmap handle %ld\n", surf->pixmap_handle);
        
        surf->egl_surface = _this->egl_data->eglCreatePixmapSurface(
                _this->egl_data->egl_display,
                _this->egl_data->egl_config,
                surf->pixmap_handle, NULL);
        if (surf->egl_surface == EGL_NO_SURFACE) {
            SDL_EGL_SetError("Unable to create EGL window surface", "eglCreatePixmapSurface");
            return EGL_NO_SURFACE;
        }
    }

    return windowdata->surface[1].egl_surface;
}

int
MALI_CreateWindow(_THIS, SDL_Window * window)
{
    EGLSurface egl_surface;
    SDL_WindowData *windowdata;
    SDL_DisplayData *displaydata;
    SDL_VideoDisplay *display;

    display = SDL_GetDisplayForWindow(window);
    displaydata = SDL_GetDisplayDriverData(0);

    /* Allocate window internal data */
    windowdata = (SDL_WindowData *) SDL_calloc(1, sizeof(SDL_WindowData));
    if (windowdata == NULL) {
        return SDL_OutOfMemory();
    }

    /* OpenGL ES is the law here */
    window->flags |= SDL_WINDOW_OPENGL;

    if (!_this->egl_data) {
        if (SDL_GL_LoadLibrary(NULL) < 0) {
            return -1;
        }
    }

    /* Acquire handle to internal pixmap routines */
    if (!displaydata->egl_create_pixmap_ID_mapping) {
        displaydata->egl_create_pixmap_ID_mapping = SDL_EGL_GetProcAddress(_this, "egl_create_pixmap_ID_mapping");
        displaydata->egl_destroy_pixmap_ID_mapping = SDL_EGL_GetProcAddress(_this, "egl_destroy_pixmap_ID_mapping");
        if (!displaydata->egl_create_pixmap_ID_mapping || 
            !displaydata->egl_destroy_pixmap_ID_mapping) {
            MALI_VideoQuit(_this);
            return SDL_SetError("mali-fbdev: One or more required libmali internal not exposed.");
        }
    }

    /* Setup driver data for this window */
    window->driverdata = windowdata;

    /*
     * Find right side up and set correct resolution, if called from
     * MALI_SetWindowFullscreen we are going to use the current mode instead,
     * otherwise FNA fails, but we want to be able to set arbitrary resolutions
     * when you actually define a video mode.
     */
    MALI_Reset_Orientation_Rotation(_this, display, displaydata);
    if ((window->flags & SDL_WINDOW_FULLSCREEN) != 0)
    {
        window->w = display->current_mode.w;
        window->h = display->current_mode.h;
    }

    windowdata->prev_w = window->w;
    windowdata->prev_h = window->h;

    /* Populate triplebuffering data and threads */
    MALI_TripleBufferInit(windowdata);
    windowdata->current_page = 0;
    windowdata->flip_page = 1;
    windowdata->new_page = 2;

    SDL_LockMutex(windowdata->triplebuf_mutex);
    windowdata->triplebuf_thread = SDL_CreateThread(MALI_TripleBufferingThread, "MALI_TripleBufferingThread", _this);

    egl_surface = MALI_EGL_CreatePixmapSurface(_this, window->w, window->h, windowdata, displaydata);

    /* Wait until the triplebuf thread is ready */
    SDL_CondWait(windowdata->triplebuf_cond, windowdata->triplebuf_mutex);
    SDL_UnlockMutex(windowdata->triplebuf_mutex);
    
    if (egl_surface == EGL_NO_SURFACE) {
        MALI_VideoQuit(_this);
        return SDL_SetError("mali-fbdev: Can't create EGL window surface");
    } else {
        MALI_GLES_MakeCurrent(_this, window, _this->current_glctx);
    }

    /* One window, it always has focus */
    SDL_SetMouseFocus(window);
    SDL_SetKeyboardFocus(window);

    /* Window has been successfully created */
    return 0;
}

void
MALI_DestroyWindow(_THIS, SDL_Window * window)
{
    int i, io;
    SDL_WindowData *windowdata;
    SDL_DisplayData *displaydata;
    struct ion_handle_data ionHandleData;

    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "Destroying MALI window.");

    MALI_TripleBufferStop(_this);

    windowdata = window->driverdata;
    displaydata = SDL_GetDisplayDriverData(0);

    // You _MUST_ unset the current surface, otherwise you can't destroy a surface, when this happens
    // and you try deleting a pixmap mapping, you're left with stale references and further breakage.
    SDL_EGL_MakeCurrent(_this, EGL_NO_CONTEXT, EGL_NO_SURFACE);

    if (windowdata) {
        for (i = 0; i < 3; i++) {
            MALI_EGL_Surface *surf = &windowdata->surface[i];
            if (surf->shared_fd >= 0)
                close(surf->shared_fd);

            ionHandleData = (struct ion_handle_data) {
                .handle = surf->handle
            };

            io = ioctl(displaydata->ion_fd, ION_IOC_FREE, &ionHandleData);
            if (io != 0) {
                SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "ION_IOC_FREE failed.");
            }

            if (surf->egl_surface != EGL_NO_SURFACE) {
                SDL_EGL_DestroySurface(_this, surf->egl_surface);
                surf->egl_surface = EGL_NO_SURFACE;
            }

            displaydata->egl_destroy_pixmap_ID_mapping((unsigned long)surf->pixmap_handle);
            surf->shared_fd = -1;
            surf->handle = 0;
        }

        SDL_free(windowdata);
    }

    window->driverdata = NULL;
}

void
MALI_SetWindowTitle(_THIS, SDL_Window * window)
{
}

void
MALI_SetWindowPosition(_THIS, SDL_Window * window)
{
}

void
MALI_SetWindowSize(_THIS, SDL_Window * window)
{
    MALI_MaybeRecreate(_this, window, window->w, window->h);
}

void
MALI_SetWindowFullscreen(_THIS, SDL_Window * window, SDL_VideoDisplay * display, SDL_bool fullscreen)
{
    window->fullscreen_mode = display->current_mode;
    MALI_MaybeRecreate(_this, window, display->current_mode.w, display->current_mode.h);
}

void
MALI_ShowWindow(_THIS, SDL_Window * window)
{
}

void
MALI_HideWindow(_THIS, SDL_Window * window)
{
}

/*****************************************************************************/
/* SDL Window Manager function                                               */
/*****************************************************************************/
SDL_bool
MALI_GetWindowWMInfo(_THIS, SDL_Window * window, struct SDL_SysWMinfo *info)
{
    if (info->version.major <= SDL_MAJOR_VERSION) {
        return SDL_TRUE;
    } else {
        SDL_SetError("application not compiled with SDL %d.%d\n",
            SDL_MAJOR_VERSION, SDL_MINOR_VERSION);
    }

    /* Failed to get window manager information */
    return SDL_FALSE;
}

/*****************************************************************************/
/* SDL event functions                                                       */
/*****************************************************************************/
void MALI_PumpEvents(_THIS)
{
#ifdef SDL_INPUT_LINUXEV
    SDL_EVDEV_Poll();
#endif
}

#endif /* SDL_VIDEO_DRIVER_MALI */

