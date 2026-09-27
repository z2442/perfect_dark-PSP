#include <stdio.h>
#include <unistd.h>
#include <time.h>
#ifdef __PSP__
#include <pspkernel.h>
#endif

#include "gfx_window_manager_api.h"
#include "gfx_screen_config.h"

static int32_t target_fps = 0;
static int swap_interval = 1;
static double last_frame_time = 0.0;
static double gfx_sdl_get_time(void);

static int32_t gfx_sdl_get_maximized_state(void) { return 0; }
static int32_t gfx_sdl_get_fullscreen_state(void) { return 0; }
static int32_t gfx_sdl_get_fullscreen_flag_mode(void) { return 0; }
static void gfx_sdl_set_fullscreen_flag(int32_t mode) { }
static void gfx_sdl_set_fullscreen_changed_callback(void (*on_fullscreen_changed)(bool is_now_fullscreen)) { }
static void gfx_sdl_set_fullscreen(bool enable) { }
static void gfx_sdl_set_fullscreen_exclusive(bool enable) { }
static void gfx_sdl_set_maximize_window(bool enable) { }
static void gfx_sdl_set_cursor_visibility(bool visible) { }
static void gfx_sdl_get_active_window_refresh_rate(uint32_t* refresh_rate) { *refresh_rate = 60; }

static void gfx_sdl_init(const struct GfxWindowInitSettings *set) {

}

static void gfx_sdl_close(void) { }

static void gfx_sdl_get_centered_positions(int32_t width, int32_t height, int32_t *posX, int32_t *posY) {
    *posX = (480 - width) / 2;
    *posY = (272 - height) / 2;
}

static void gfx_sdl_set_closest_resolution(int32_t width, int32_t height, bool should_center) { }
static void gfx_sdl_set_dimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) { }
static void gfx_sdl_get_dimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) {
    *width = 480;
    *height = 272;
    *posX = 0;
    *posY = 0;
}
static void gfx_sdl_handle_events(void) { }

static bool gfx_sdl_start_frame(void) {
    double now = gfx_sdl_get_time();
    if (target_fps > 0 && last_frame_time > 0.0) {
        const double deadline = last_frame_time + 1.0 / target_fps;
        while (now < deadline) {
            const unsigned int delay = (unsigned int)((deadline - now) * 1000000.0) + 1;
#ifdef __PSP__
            sceKernelDelayThread(delay);
#else
            usleep(delay);
#endif
            now = gfx_sdl_get_time();
        }
    }
    last_frame_time = now;
    return true;
}
static void gfx_sdl_swap_buffers_begin(void) { }
static void gfx_sdl_swap_buffers_end(void) {

}
static double gfx_sdl_get_time(void) {
#ifdef __PSP__
    return (double)sceKernelGetSystemTimeWide() / 1000000.0;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1000000000.0;
#endif
}
static int32_t gfx_sdl_get_target_fps(void) {
    return target_fps;
}
static void gfx_sdl_set_target_fps(int fps) {
    target_fps = fps > 0 ? fps : 0;
    last_frame_time = 0.0;
}
static bool gfx_sdl_can_disable_vsync(void) {
    return true;
}
static void *gfx_sdl_get_window_handle(void) {
    return NULL;
}
static void gfx_sdl_set_window_title(const char *title) { }
static int gfx_sdl_get_swap_interval(void) {
    return swap_interval;
}
static bool gfx_sdl_set_swap_interval(int interval) {
    // The PSP supports immediate presentation or one vblank per frame.
    swap_interval = interval == 0 ? 0 : 1;
    return true;
}

int gfx_sdl_get_display_mode(int modenum, int *out_w, int *out_h) {
    if (modenum == 0) {
        *out_w = 480;
        *out_h = 272;
        return 1;
    }
    return 0;
}
int gfx_sdl_get_current_display_mode(int *out_w, int *out_h) {
    *out_w = 480;
    *out_h = 272;
    return 1;
}
int gfx_sdl_get_num_display_modes(void) {
    return 1;
}

struct GfxWindowManagerAPI gfx_sdl = {
    gfx_sdl_init,
    gfx_sdl_close,
    gfx_sdl_get_display_mode,
    gfx_sdl_get_current_display_mode,
    gfx_sdl_get_num_display_modes,
    gfx_sdl_get_fullscreen_state,
    gfx_sdl_set_fullscreen_changed_callback,
    gfx_sdl_set_fullscreen,
    gfx_sdl_set_fullscreen_exclusive,
    gfx_sdl_set_fullscreen_flag,
    gfx_sdl_get_fullscreen_flag_mode,
    gfx_sdl_get_maximized_state,
    gfx_sdl_set_maximize_window,
    gfx_sdl_get_active_window_refresh_rate,
    gfx_sdl_set_cursor_visibility,
    gfx_sdl_set_closest_resolution,
    gfx_sdl_set_dimensions,
    gfx_sdl_get_dimensions,
    gfx_sdl_get_centered_positions,
    gfx_sdl_handle_events,
    gfx_sdl_start_frame,
    gfx_sdl_swap_buffers_begin,
    gfx_sdl_swap_buffers_end,
    gfx_sdl_get_time,
    gfx_sdl_get_target_fps,
    gfx_sdl_set_target_fps,
    gfx_sdl_can_disable_vsync,
    gfx_sdl_get_window_handle,
    gfx_sdl_set_window_title,
    gfx_sdl_get_swap_interval,
    gfx_sdl_set_swap_interval,
};