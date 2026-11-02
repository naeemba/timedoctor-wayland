/*
 * td-waylandshot: LD_PRELOAD shim that makes X11 root-window screen grabs
 * (as done by Qt's QScreen::grabWindow on the xcb platform) return a real
 * capture of the Wayland desktop, taken with `grim`, instead of the black
 * XWayland root window.
 *
 * It hooks xcb_copy_area() to remember the root-window rectangle being
 * copied into a pixmap, then hooks xcb_image_get() and overwrites the
 * pixels of that pixmap with the grim capture of the same region.
 * X monitor names (from `xrandr --listmonitors`) are matched to Wayland
 * output names, so differing X/Wayland layouts are handled per monitor.
 *
 * Build: make (see the Makefile)
 * Use:   LD_PRELOAD=/path/td-waylandshot.so <app>
 * Env:   TD_SHOT_LOG=<file>   append a log line per substitution
 *        TD_SHOT_DEBUG=<dir>  also write each substituted image as PPM
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <stdarg.h>
#include <xcb/xcb.h>
#include <xcb/xcb_image.h>

typedef xcb_void_cookie_t (*copy_area_function)(xcb_connection_t *, xcb_drawable_t, xcb_drawable_t,
    xcb_gcontext_t, int16_t, int16_t, int16_t, int16_t, uint16_t, uint16_t);
typedef xcb_image_t *(*image_get_function)(xcb_connection_t *, xcb_drawable_t, int16_t, int16_t,
    uint16_t, uint16_t, uint32_t, xcb_image_format_t);

static struct {
    xcb_connection_t *connection;
    xcb_drawable_t destination;
    int16_t source_x, source_y;
    uint16_t width, height;
    int valid;
} last_copy;

struct monitor { char name[64]; int x, y, width, height; };
static struct monitor monitors[16];
static int monitor_count;
static time_t monitors_loaded_at;

void log_message(const char *format, ...)
{
    const char *path = getenv("TD_SHOT_LOG");
    if (!path) return;
    FILE *file = fopen(path, "a");
    if (!file) return;
    char timestamp[32]; time_t now = time(NULL);
    strftime(timestamp, sizeof timestamp, "%F %T", localtime(&now));
    fprintf(file, "%s ", timestamp);
    va_list arguments; va_start(arguments, format); vfprintf(file, format, arguments); va_end(arguments);
    fputc('\n', file);
    fclose(file);
}

static int is_root(xcb_connection_t *connection, xcb_drawable_t drawable)
{
    const xcb_setup_t *setup = xcb_get_setup(connection);
    if (!setup) return 0;
    for (xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup); screens.rem; xcb_screen_next(&screens))
        if (screens.data->root == drawable) return 1;
    return 0;
}

static void load_monitors(void)
{
    time_t now = time(NULL);
    if (monitor_count && now - monitors_loaded_at < 60) return;
    FILE *xrandr = popen("timeout -k 1 3 xrandr --listmonitors 2>/dev/null", "r");
    if (!xrandr) return;
    char line[512]; int count = 0;
    while (fgets(line, sizeof line, xrandr) && count < 16) {
        /*  0: +*eDP-1 1920/340x1080/190+0+0  eDP-1 */
        int width, height, x, y; char name[64];
        if (sscanf(line, " %*d: %*s %d/%*dx%d/%*d+%d+%d %63s", &width, &height, &x, &y, name) != 5) continue;
        snprintf(monitors[count].name, sizeof monitors[count].name, "%s", name);
        monitors[count].x = x; monitors[count].y = y;
        monitors[count].width = width; monitors[count].height = height;
        count++;
    }
    pclose(xrandr);
    if (count) { monitor_count = count; monitors_loaded_at = now; }
}

/* Capture one Wayland output with grim as P6 PPM; returns malloc'd RGB data. */
static uint8_t *grab_output(const char *name, int *width, int *height)
{
    char command[256];
    /* The launcher hides WAYLAND_DISPLAY from Time Doctor; grim still needs it. */
    if (snprintf(command, sizeof command, "WAYLAND_DISPLAY=\"${TD_WAYLAND_DISPLAY:-$WAYLAND_DISPLAY}\" "
                 "timeout -k 1 3 grim -s 1 -o '%s' -t ppm - 2>/dev/null", name) >= (int)sizeof command)
        return NULL;
    FILE *grim = popen(command, "r");
    if (!grim) return NULL;
    char magic[3] = {0}; int maximum_value = 0;
    if (fscanf(grim, "%2s %d %d %d", magic, width, height, &maximum_value) != 4 || strcmp(magic, "P6") || maximum_value != 255) {
        pclose(grim); return NULL;
    }
    fgetc(grim); /* single whitespace after the maximum value */
    size_t size = (size_t)*width * *height * 3;
    uint8_t *pixels = malloc(size);
    if (!pixels) { pclose(grim); return NULL; }
    size_t got = fread(pixels, 1, size, grim);
    pclose(grim);
    if (got != size) { free(pixels); return NULL; }
    return pixels;
}

static void dump_debug(xcb_image_t *image, int most_significant_first)
{
    const char *directory = getenv("TD_SHOT_DEBUG");
    if (!directory) return;
    char path[512]; time_t now = time(NULL);
    snprintf(path, sizeof path, "%s/shot-%ld-%ux%u.ppm", directory, (long)now, image->width, image->height);
    FILE *file = fopen(path, "wb");
    if (!file) return;
    fprintf(file, "P6\n%u %u\n255\n", image->width, image->height);
    for (unsigned y = 0; y < image->height; y++) {
        uint8_t *row = image->data + (size_t)y * image->stride;
        for (unsigned x = 0; x < image->width; x++) {
            uint8_t *pixel = row + x * 4;
            uint8_t rgb[3];
            if (most_significant_first) { rgb[0] = pixel[1]; rgb[1] = pixel[2]; rgb[2] = pixel[3]; }
            else                        { rgb[0] = pixel[2]; rgb[1] = pixel[1]; rgb[2] = pixel[0]; }
            fwrite(rgb, 1, 3, file);
        }
    }
    fclose(file);
}

static void fill_from_wayland(xcb_image_t *image, int root_x, int root_y)
{
    load_monitors();
    if (!monitor_count) { log_message("no monitors from xrandr"); return; }
    int most_significant_first = image->byte_order == XCB_IMAGE_ORDER_MSB_FIRST;
    int filled = 0;
    for (int i = 0; i < monitor_count; i++) {
        struct monitor *monitor = &monitors[i];
        /* intersection of the request rect and this monitor, in X coords */
        int left = root_x > monitor->x ? root_x : monitor->x;
        int top = root_y > monitor->y ? root_y : monitor->y;
        int right = (root_x + image->width) < (monitor->x + monitor->width) ? (root_x + image->width) : (monitor->x + monitor->width);
        int bottom = (root_y + image->height) < (monitor->y + monitor->height) ? (root_y + image->height) : (monitor->y + monitor->height);
        if (right <= left || bottom <= top) continue;
        int grabbed_width, grabbed_height;
        uint8_t *rgb = grab_output(monitor->name, &grabbed_width, &grabbed_height);
        if (!rgb) { log_message("grim failed for %s", monitor->name); continue; }
        for (int y = top; y < bottom; y++) {
            int monitor_y = y - monitor->y, image_y = y - root_y;
            if (monitor_y >= grabbed_height) break;
            uint8_t *destination = image->data + (size_t)image_y * image->stride + (size_t)(left - root_x) * 4;
            for (int x = left; x < right; x++, destination += 4) {
                int monitor_x = x - monitor->x;
                if (monitor_x >= grabbed_width) break;
                const uint8_t *source = rgb + ((size_t)monitor_y * grabbed_width + monitor_x) * 3;
                if (most_significant_first) { destination[0] = 0; destination[1] = source[0]; destination[2] = source[1]; destination[3] = source[2]; }
                else                        { destination[0] = source[2]; destination[1] = source[1]; destination[2] = source[0]; destination[3] = 0; }
            }
        }
        free(rgb);
        filled++;
    }
    log_message("substituted %ux%u at %d,%d from %d monitor(s)", image->width, image->height, root_x, root_y, filled);
    if (filled) dump_debug(image, most_significant_first);
}

xcb_void_cookie_t xcb_copy_area(xcb_connection_t *connection, xcb_drawable_t source, xcb_drawable_t destination,
    xcb_gcontext_t graphics_context, int16_t source_x, int16_t source_y, int16_t destination_x, int16_t destination_y,
    uint16_t width, uint16_t height)
{
    static copy_area_function real;
    if (!real) real = (copy_area_function)dlsym(RTLD_NEXT, "xcb_copy_area");
    if (destination_x == 0 && destination_y == 0 && is_root(connection, source)) {
        last_copy.connection = connection; last_copy.destination = destination;
        last_copy.source_x = source_x; last_copy.source_y = source_y;
        last_copy.width = width; last_copy.height = height; last_copy.valid = 1;
    }
    return real(connection, source, destination, graphics_context, source_x, source_y,
                destination_x, destination_y, width, height);
}

/* ---- low-level hooks: xcb_get_image(_unchecked) + xcb_get_image_reply (what Qt 6 uses) ---- */
typedef xcb_get_image_cookie_t (*get_image_function)(xcb_connection_t *, uint8_t, xcb_drawable_t,
    int16_t, int16_t, uint16_t, uint16_t, uint32_t);
typedef xcb_get_image_reply_t *(*get_image_reply_function)(xcb_connection_t *, xcb_get_image_cookie_t,
    xcb_generic_error_t **);

struct pending { unsigned sequence; xcb_connection_t *connection; int root_x, root_y; uint16_t width, height; uint8_t format; int used; };
static struct pending pending_requests[8];

static void remember_request(xcb_connection_t *connection, unsigned sequence, uint8_t format, xcb_drawable_t drawable,
    int16_t x, int16_t y, uint16_t width, uint16_t height)
{
    int root_x, root_y;
    if (is_root(connection, drawable)) { root_x = x; root_y = y; }
    else if (last_copy.valid && last_copy.connection == connection && drawable == last_copy.destination) {
        root_x = last_copy.source_x + x; root_y = last_copy.source_y + y;
    }
    else return;
    for (int i = 0; i < 8; i++) if (!pending_requests[i].used) {
        pending_requests[i] = (struct pending){ sequence, connection, root_x, root_y, width, height, format, 1 };
        return;
    }
}

static void fill_reply(xcb_connection_t *connection, xcb_get_image_reply_t *reply, struct pending *request)
{
    if (!reply || request->format != XCB_IMAGE_FORMAT_Z_PIXMAP || reply->depth != 24) {
        log_message("unsupported get_image reply: depth=%u format=%u", reply ? reply->depth : 0, request->format);
        return;
    }
    size_t need = (size_t)request->width * request->height * 4;
    if ((size_t)xcb_get_image_data_length(reply) < need) { log_message("short reply"); return; }
    xcb_image_t image = {0};
    image.width = request->width; image.height = request->height; image.bpp = 32; image.depth = 24;
    image.stride = request->width * 4; image.data = xcb_get_image_data(reply);
    image.byte_order = xcb_get_setup(connection)->image_byte_order;
    fill_from_wayland(&image, request->root_x, request->root_y);
}

xcb_get_image_cookie_t xcb_get_image(xcb_connection_t *connection, uint8_t format, xcb_drawable_t drawable,
    int16_t x, int16_t y, uint16_t width, uint16_t height, uint32_t plane_mask)
{
    static get_image_function real;
    if (!real) real = (get_image_function)dlsym(RTLD_NEXT, "xcb_get_image");
    xcb_get_image_cookie_t cookie = real(connection, format, drawable, x, y, width, height, plane_mask);
    remember_request(connection, cookie.sequence, format, drawable, x, y, width, height);
    return cookie;
}

xcb_get_image_cookie_t xcb_get_image_unchecked(xcb_connection_t *connection, uint8_t format, xcb_drawable_t drawable,
    int16_t x, int16_t y, uint16_t width, uint16_t height, uint32_t plane_mask)
{
    static get_image_function real;
    if (!real) real = (get_image_function)dlsym(RTLD_NEXT, "xcb_get_image_unchecked");
    xcb_get_image_cookie_t cookie = real(connection, format, drawable, x, y, width, height, plane_mask);
    remember_request(connection, cookie.sequence, format, drawable, x, y, width, height);
    return cookie;
}

xcb_get_image_reply_t *xcb_get_image_reply(xcb_connection_t *connection, xcb_get_image_cookie_t cookie,
    xcb_generic_error_t **error)
{
    static get_image_reply_function real;
    if (!real) real = (get_image_reply_function)dlsym(RTLD_NEXT, "xcb_get_image_reply");
    xcb_get_image_reply_t *reply = real(connection, cookie, error);
    for (int i = 0; i < 8; i++)
        if (pending_requests[i].used && pending_requests[i].connection == connection && pending_requests[i].sequence == cookie.sequence) {
            if (reply) fill_reply(connection, reply, &pending_requests[i]);
            pending_requests[i].used = 0;
            break;
        }
    return reply;
}

/* ---- higher-level hook (libxcb-image helper, used by Qt 5) ---- */
xcb_image_t *xcb_image_get(xcb_connection_t *connection, xcb_drawable_t drawable, int16_t x, int16_t y,
    uint16_t width, uint16_t height, uint32_t plane_mask, xcb_image_format_t format)
{
    static image_get_function real;
    if (!real) real = (image_get_function)dlsym(RTLD_NEXT, "xcb_image_get");
    /* libxcb-image issues xcb_get_image + xcb_get_image_reply internally, which the hooks above handle. */
    return real(connection, drawable, x, y, width, height, plane_mask, format);
}
