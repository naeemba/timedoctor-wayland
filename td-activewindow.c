/*
 * td-activewindow: make Time Doctor's app/window tracking see native Wayland
 * windows under Hyprland.
 *
 * libtimedoctor.so reads _NET_ACTIVE_WINDOW from the XWayland root window,
 * then _NET_WM_NAME and _NET_WM_PID of that window, then /proc/<pid>/cmdline.
 * XWayland only knows X11 windows, so a focused Wayland app is invisible and
 * Time Doctor records itself. When the focused Hyprland window is native
 * Wayland, these hooks answer _NET_ACTIVE_WINDOW with a placeholder window id
 * and answer that id's _NET_WM_NAME / _NET_WM_PID from Hyprland's
 * `activewindow`. Focused XWayland windows and every other property request
 * (including all of Qt's) pass through untouched.
 *
 * Built into td-waylandshot.so by the Makefile.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <xcb/xcb.h>

/* Outside the 29-bit X resource id range, so it never names a real window. */
#define PLACEHOLDER_WINDOW 0x3ffffff0u

typedef xcb_get_property_cookie_t (*get_property_fn)(xcb_connection_t *, uint8_t, xcb_window_t,
    xcb_atom_t, xcb_atom_t, uint32_t, uint32_t);
typedef xcb_get_property_reply_t *(*get_property_reply_fn)(xcb_connection_t *,
    xcb_get_property_cookie_t, xcb_generic_error_t **);

enum answer { ANSWER_NONE, ANSWER_ACTIVE, ANSWER_NAME, ANSWER_PID };

static struct {
    xcb_connection_t *conn;
    xcb_atom_t active, name, pid, utf8;
} atoms;

static struct { char title[512]; uint32_t pid; int wayland; } focused;

static struct { unsigned seq; xcb_connection_t *conn; enum answer answer; } pend[8];

static int called_from_timedoctor(void *address)
{
    Dl_info info;
    return dladdr(address, &info) && info.dli_fname && strstr(info.dli_fname, "libtimedoctor");
}

static xcb_atom_t intern(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c,
        xcb_intern_atom(c, 0, strlen(name), name), NULL);
    xcb_atom_t atom = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return atom;
}

static void load_atoms(xcb_connection_t *c)
{
    if (atoms.conn == c) return;
    atoms.conn = c;
    atoms.active = intern(c, "_NET_ACTIVE_WINDOW");
    atoms.name = intern(c, "_NET_WM_NAME");
    atoms.pid = intern(c, "_NET_WM_PID");
    atoms.utf8 = intern(c, "UTF8_STRING");
}

static xcb_window_t root_of(xcb_connection_t *c)
{
    return xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
}

/* Ask Hyprland's IPC socket for the focused window; leaves `focused` zeroed on failure. */
static void read_focused(void)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR"), *signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    struct timeval timeout = { 0, 300000 };
    char buffer[8192];
    size_t used = 0;
    ssize_t got;
    int fd;

    memset(&focused, 0, sizeof focused);
    if (!runtime || !signature) return;
    snprintf(address.sun_path, sizeof address.sun_path, "%s/hypr/%s/.socket.sock", runtime, signature);
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    if (connect(fd, (struct sockaddr *)&address, sizeof address) < 0 ||
        write(fd, "activewindow", 12) != 12) {
        close(fd);
        return;
    }
    while (used < sizeof buffer - 1 && (got = read(fd, buffer + used, sizeof buffer - 1 - used)) > 0)
        used += got;
    close(fd);
    buffer[used] = '\0';
    if (strncmp(buffer, "Window ", 7)) return;

    int xwayland = 1;
    for (char *line = strtok(buffer, "\n"); line; line = strtok(NULL, "\n")) {
        while (*line == '\t') line++;
        if (!strncmp(line, "title: ", 7))
            snprintf(focused.title, sizeof focused.title, "%s", line + 7);
        else if (!strncmp(line, "pid: ", 5))
            focused.pid = strtoul(line + 5, NULL, 10);
        else if (!strncmp(line, "xwayland: ", 10))
            xwayland = atoi(line + 10);
    }
    focused.wayland = !xwayland && focused.pid;
}

static xcb_get_property_reply_t *make_reply(xcb_get_property_reply_t *real, xcb_atom_t type,
    uint8_t format, const void *value, uint32_t count)
{
    uint32_t bytes = count * (format / 8), padded = (bytes + 3) & ~3u;
    xcb_get_property_reply_t *r = calloc(1, sizeof *r + padded);
    if (!r) return real;
    r->response_type = 1;	/* X reply */
    r->sequence = real ? real->sequence : 0;
    r->format = format;
    r->type = type;
    r->value_len = count;
    r->length = padded / 4;
    memcpy(r + 1, value, bytes);
    free(real);
    return r;
}

xcb_get_property_cookie_t xcb_get_property(xcb_connection_t *c, uint8_t delete, xcb_window_t window,
    xcb_atom_t property, xcb_atom_t type, uint32_t long_offset, uint32_t long_length)
{
    static get_property_fn real;
    enum answer answer = ANSWER_NONE;

    if (!real) real = (get_property_fn)dlsym(RTLD_NEXT, "xcb_get_property");
    /* Cheap window test first: Qt issues many property requests on other windows. */
    if ((window == PLACEHOLDER_WINDOW || window == root_of(c)) &&
        (c == atoms.conn || called_from_timedoctor(__builtin_return_address(0)))) {
        load_atoms(c);
        if (property == atoms.active && window == root_of(c))
            answer = ANSWER_ACTIVE;
        else if (window == PLACEHOLDER_WINDOW) {
            /* Ask about the root instead so the server never sees the fake id. */
            window = root_of(c);
            answer = property == atoms.name ? ANSWER_NAME : property == atoms.pid ? ANSWER_PID : ANSWER_NONE;
            if (answer == ANSWER_NONE) property = XCB_ATOM_NONE;
        }
    }
    xcb_get_property_cookie_t cookie = real(c, delete, window, property, type, long_offset, long_length);
    if (answer != ANSWER_NONE)
        for (int i = 0; i < 8; i++) if (!pend[i].conn) {
            pend[i].seq = cookie.sequence; pend[i].conn = c; pend[i].answer = answer;
            break;
        }
    return cookie;
}

xcb_get_property_reply_t *xcb_get_property_reply(xcb_connection_t *c, xcb_get_property_cookie_t cookie,
    xcb_generic_error_t **e)
{
    static get_property_reply_fn real;
    enum answer answer = ANSWER_NONE;

    if (!real) real = (get_property_reply_fn)dlsym(RTLD_NEXT, "xcb_get_property_reply");
    xcb_get_property_reply_t *r = real(c, cookie, e);
    for (int i = 0; i < 8; i++) if (pend[i].conn == c && pend[i].seq == cookie.sequence) {
        answer = pend[i].answer;
        pend[i].conn = NULL;
        break;
    }
    switch (answer) {
    case ANSWER_ACTIVE: {
        read_focused();
        if (!focused.wayland) return r;
        xcb_window_t placeholder = PLACEHOLDER_WINDOW;
        return make_reply(r, XCB_ATOM_WINDOW, 32, &placeholder, 1);
    }
    case ANSWER_NAME:
        return make_reply(r, atoms.utf8, 8, focused.title, strlen(focused.title));
    case ANSWER_PID:
        return make_reply(r, XCB_ATOM_CARDINAL, 32, &focused.pid, 1);
    default:
        return r;
    }
}
