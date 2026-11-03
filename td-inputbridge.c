/*
 * td-inputbridge: make Time Doctor's activity detector see real input under
 * Wayland (Hyprland).
 *
 * Time Doctor watches XI2 raw events on the XWayland root window, which only
 * fire for input sent to X11 windows. This reads the real keyboard/mouse
 * devices and mirrors each real event into XWayland through XTest:
 *   - real key press        -> press+release of a keycode with no keysym
 *                              (types nothing in any X window)
 *   - real motion/wheel     -> X pointer warped to the real Hyprland cursor
 *                              position (read from Hyprland's IPC socket),
 *                              carried onto XWayland's root window through
 *                              the output of the same name
 *   - real click            -> the same warp, offset by 1px on every other
 *                              click so a click in place still moves it,
 *                              sent once the button is back up (never an X
 *                              button press)
 *   - real touchpad touch   -> the click warp when the finger lands
 * While any mouse button, pen barrel button, or pen tip or finger on a
 * touchscreen or pen tablet is down, no pointer motion is sent at all: a warp
 * between press and release reads as a drag in X apps, which then drop a
 * click that acts on release (a left click) or lose the drag itself.
 * Time Doctor compares pointer positions over time, so the X pointer has to
 * actually end up somewhere new; a nudge that undoes itself reads as idle.
 * Nothing is generated without real input, and key codes are never logged.
 *
 * Installed setgid "input" so only this binary can open /dev/input.
 * Exits when no "timedoctor" process has existed for EXIT_GRACE_SECONDS seconds.
 */
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xrandr.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEVICES 64
#define MOTION_MIN_MS 100
#define EXIT_GRACE_SECONDS 60
#define LONGS_FOR_BITS(x) ((((x) - 1) / (8 * sizeof(long))) + 1)
#define TEST_BIT(b, a) (((a)[(b) / (8 * sizeof(long))] >> ((b) % (8 * sizeof(long)))) & 1)

/* held[code - BTN_MISC] is set while that button holds the X pointer still. */
struct device { int fd, touch_is_click; unsigned char held[KEY_OK - BTN_MISC]; char name[256]; };
static struct device devices[MAX_DEVICES];
static int device_count;

static long now_ms(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec * 1000L + now.tv_nsec / 1000000L;
}

static int is_input_device(int fd)
{
	unsigned long events[LONGS_FOR_BITS(EV_MAX + 1)] = {0}, keys[LONGS_FOR_BITS(KEY_MAX + 1)] = {0};
	unsigned long relative[LONGS_FOR_BITS(REL_MAX + 1)] = {0}, absolute[LONGS_FOR_BITS(ABS_MAX + 1)] = {0};

	if (ioctl(fd, EVIOCGBIT(0, sizeof events), events) < 0)
		return 0;
	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
	ioctl(fd, EVIOCGBIT(EV_REL, sizeof relative), relative);
	ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absolute), absolute);
	if (TEST_BIT(EV_KEY, events) && TEST_BIT(KEY_A, keys) && TEST_BIT(KEY_Z, keys))
		return 1;	/* keyboard */
	if (TEST_BIT(EV_REL, events) && TEST_BIT(REL_X, relative))
		return 1;	/* mouse */
	if (TEST_BIT(EV_ABS, events) && TEST_BIT(ABS_X, absolute) &&
	    (TEST_BIT(BTN_TOUCH, keys) || TEST_BIT(BTN_LEFT, keys)))
		return 1;	/* touchpad */
	return 0;
}

/*
 * Touchscreens and pen tablets, where BTN_TOUCH is the click itself. Screenless
 * pen tablets (Intuos and most others) are not INPUT_PROP_DIRECT, so a pen is
 * recognised by BTN_TOOL_PEN, which a touchpad never has.
 */
static int is_touch_click_device(int fd)
{
	unsigned long properties[LONGS_FOR_BITS(INPUT_PROP_MAX + 1)] = {0}, keys[LONGS_FOR_BITS(KEY_MAX + 1)] = {0};

	if (ioctl(fd, EVIOCGPROP(sizeof properties), properties) >= 0 && TEST_BIT(INPUT_PROP_DIRECT, properties))
		return 1;
	return ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) >= 0 && TEST_BIT(BTN_TOOL_PEN, keys);
}

static void scan_devices(void)
{
	DIR *directory = opendir("/dev/input");
	struct dirent *entry;

	if (!directory)
		return;
	while ((entry = readdir(directory)) && device_count < MAX_DEVICES) {
		char path[300];
		int i, fd, known = 0;

		if (strncmp(entry->d_name, "event", 5))
			continue;
		for (i = 0; i < device_count; i++)
			if (!strcmp(devices[i].name, entry->d_name))
				known = 1;
		if (known)
			continue;
		snprintf(path, sizeof path, "/dev/input/%s", entry->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!is_input_device(fd)) {
			close(fd);
			continue;
		}
		memset(&devices[device_count], 0, sizeof devices[device_count]);
		devices[device_count].fd = fd;
		devices[device_count].touch_is_click = is_touch_click_device(fd);
		snprintf(devices[device_count].name, sizeof devices[device_count].name, "%s", entry->d_name);
		device_count++;
	}
	closedir(directory);
}

static void drop_device(int i)
{
	close(devices[i].fd);
	devices[i] = devices[--device_count];
}

/*
 * Whether a button code (BTN_MISC..KEY_OK) keeps the X pointer still while
 * down. Touchpad touch and tool codes (BTN_DIGI..BTN_WHEEL) do not, or a
 * resting finger would stop every warp; their press warps as a click. Pen
 * barrel buttons do, and so does BTN_TOUCH on a touchscreen or pen tablet,
 * where it is the click.
 */
static int holds_pointer(const struct device *device, int code)
{
	if (code == BTN_STYLUS || code == BTN_STYLUS2 || code == BTN_STYLUS3)
		return 1;
	if (code == BTN_TOUCH)
		return device->touch_is_click;
	return code < BTN_DIGI || code >= BTN_WHEEL;
}

/*
 * After SYN_DROPPED the kernel has discarded events, maybe a release, so the
 * held buttons are read back from the kernel's key state. Events still queued
 * replay on top harmlessly: a press or release only sets or clears its bit.
 */
static void held_from_key_state(struct device *device, const unsigned long *state)
{
	int code;

	for (code = BTN_MISC; code < KEY_OK; code++)
		device->held[code - BTN_MISC] = holds_pointer(device, code) && TEST_BIT(code, state);
}

static void resync_held(struct device *device)
{
	unsigned long state[LONGS_FOR_BITS(KEY_MAX + 1)] = {0};

	if (ioctl(device->fd, EVIOCGKEY(sizeof state), state) < 0)
		memset(state, 0, sizeof state);
	held_from_key_state(device, state);
}

/* The X pointer is left alone while any device has a button down. */
static int any_button_held(void)
{
	int i, code;

	for (i = 0; i < device_count; i++)
		for (code = 0; code < KEY_OK - BTN_MISC; code++)
			if (devices[i].held[code])
				return 1;
	return 0;
}

enum warp { WARP_NONE, WARP_MOTION, WARP_CLICK };

/*
 * Reads one batch of evdev events from one device: counts key presses into
 * *key_presses, tracks the device's held buttons, and says what the X pointer
 * should do, which is nothing while any device holds a button. Motion is
 * throttled by the caller.
 */
static enum warp read_batch(struct device *device, const struct input_event *events, int count, int *key_presses)
{
	const struct input_event *event;
	int motion = 0, click = 0, dropped = 0;

	*key_presses = 0;
	for (event = events; event < events + count; event++) {
		if (event->type == EV_SYN && event->code == SYN_DROPPED) {
			dropped = 1;
		} else if (event->type == EV_REL || event->type == EV_ABS) {
			motion = 1;
		} else if (event->type != EV_KEY || event->value == 2) {
			continue;	/* autorepeat is not new input */
		} else if (event->code < BTN_MISC || event->code >= KEY_OK) {
			*key_presses += event->value == 1;
		} else if (!holds_pointer(device, event->code)) {
			click |= event->value == 1;
		} else {
			device->held[event->code - BTN_MISC] = event->value == 1;
			click |= event->value == 0;	/* the button is up now, so the warp cannot split a click */
		}
	}
	if (dropped)
		resync_held(device);
	if (any_button_held())
		return WARP_NONE;
	return click ? WARP_CLICK : motion ? WARP_MOTION : WARP_NONE;
}

/* A keycode with no keysyms and no modifier role types nothing anywhere. */
static int pick_blank_keycode(Display *display)
{
	int minimum, maximum, keysyms_per_keycode, keycode, i;
	KeySym *map;
	XModifierKeymap *modifiers = XGetModifierMapping(display);

	XDisplayKeycodes(display, &minimum, &maximum);
	map = XGetKeyboardMapping(display, minimum, maximum - minimum + 1, &keysyms_per_keycode);
	for (keycode = maximum; keycode >= minimum; keycode--) {
		int blank = 1;

		for (i = 0; i < keysyms_per_keycode; i++)
			if (map[(keycode - minimum) * keysyms_per_keycode + i] != NoSymbol)
				blank = 0;
		for (i = 0; i < 8 * modifiers->max_keypermod; i++)
			if (modifiers->modifiermap[i] == keycode)
				blank = 0;
		if (blank)
			break;
	}
	XFree(map);
	XFreeModifiermap(modifiers);
	return keycode >= minimum ? keycode : 0;
}

/* Sends one command to Hyprland's IPC socket; the reply is NUL-terminated. */
static int hyprland_request(const char *command, char *reply, size_t size)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR"), *instance = getenv("HYPRLAND_INSTANCE_SIGNATURE");
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	struct timeval timeout = { 0, 50000 };	/* never stall the input loop on a busy Hyprland */
	size_t length = strlen(command), used = 0;
	ssize_t got;
	int fd;

	if (!runtime || !instance)
		return 0;
	snprintf(address.sun_path, sizeof address.sun_path, "%s/hypr/%s/.socket.sock", runtime, instance);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
	if (!connect(fd, (struct sockaddr *)&address, sizeof address) && write(fd, command, length) == (ssize_t)length)
		while (used < size - 1 && (got = read(fd, reply + used, size - 1 - used)) > 0)
			used += got;	/* Hyprland closes the socket after the reply */
	reply[used] = '\0';
	close(fd);
	return used > 0;
}

/* Real cursor position in Hyprland's layout coordinates. */
static int cursor_position(int *x, int *y)
{
	char reply[64];

	return hyprland_request("cursorpos", reply, sizeof reply) && sscanf(reply, "%d, %d", x, y) == 2;
}

/*
 * One output, in either Hyprland's layout or XWayland's root window. The two
 * place the same outputs differently (Hyprland can leave gaps and offsets
 * XWayland packs away), so a point is carried across by output name.
 */
#define MAX_MONITORS 16
struct monitor { char name[64]; int x, y, width, height; };
static struct monitor layout_monitors[MAX_MONITORS], root_monitors[MAX_MONITORS];
static int layout_monitor_count, root_monitor_count;

/*
 * Parses `hyprctl monitors` text. Each output's mode line comes before its
 * scale line, which comes before its transform line, so the pixel size is
 * scaled down, then swapped for a 90 or 270 degree turn, into the size the
 * output takes up in the layout.
 */
static int parse_hyprland_monitors(char *reply, struct monitor *monitors, int maximum)
{
	struct monitor *monitor = NULL;
	char *line, *saved;
	double scale;
	int count = 0, transform, swap;

	for (line = strtok_r(reply, "\n", &saved); line; line = strtok_r(NULL, "\n", &saved)) {
		if (!strncmp(line, "Monitor ", 8)) {
			monitor = count < maximum && sscanf(line + 8, "%63s", monitors[count].name) == 1 ? &monitors[count++] : NULL;
		} else if (!monitor) {
			continue;
		} else if (sscanf(line, " %dx%d@%*f at %dx%d", &monitor->width, &monitor->height, &monitor->x, &monitor->y) == 4) {
			/* the mode line */
		} else if (sscanf(line, " scale: %lf", &scale) == 1 && scale > 0) {
			monitor->width = (int)(monitor->width / scale + 0.5);
			monitor->height = (int)(monitor->height / scale + 0.5);
		} else if (sscanf(line, " transform: %d", &transform) == 1 && transform % 2) {
			swap = monitor->width, monitor->width = monitor->height, monitor->height = swap;
		}
	}
	return count;
}

/*
 * Carries a layout point onto XWayland's root window through the output of
 * the same name, scaling the offset inside it to the size XWayland gives that
 * output. Returns 0 when the point is on no output both sides list.
 */
static int layout_to_root(const struct monitor *layout, int layout_count, const struct monitor *root, int root_count,
			  int x, int y, int *root_x, int *root_y)
{
	int i, j;

	for (i = 0; i < layout_count; i++) {
		const struct monitor *from = &layout[i];

		if (x < from->x || x >= from->x + from->width || y < from->y || y >= from->y + from->height)
			continue;
		for (j = 0; j < root_count; j++)
			if (!strcmp(root[j].name, from->name)) {
				*root_x = root[j].x + (int)((long)(x - from->x) * root[j].width / from->width);
				*root_y = root[j].y + (int)((long)(y - from->y) * root[j].height / from->height);
				return 1;
			}
		return 0;
	}
	return 0;
}

/* Reads both output lists; 0 when either is empty and nothing can be mapped. */
static int load_monitors(Display *display)
{
	char reply[16384];
	XRRMonitorInfo *info;
	int count = 0, i;

	layout_monitor_count = hyprland_request("monitors", reply, sizeof reply) ?
		parse_hyprland_monitors(reply, layout_monitors, MAX_MONITORS) : 0;
	info = XRRGetMonitors(display, DefaultRootWindow(display), True, &count);
	root_monitor_count = 0;
	for (i = 0; info && i < count && root_monitor_count < MAX_MONITORS; i++) {
		struct monitor *monitor = &root_monitors[root_monitor_count];
		char *name = XGetAtomName(display, info[i].name);

		if (!name)
			continue;
		snprintf(monitor->name, sizeof monitor->name, "%s", name);
		monitor->x = info[i].x, monitor->y = info[i].y;
		monitor->width = info[i].width, monitor->height = info[i].height;
		root_monitor_count++;
		XFree(name);
	}
	if (info)
		XRRFreeMonitors(info);
	return layout_monitor_count && root_monitor_count;
}

static int timedoctor_running(void)
{
	DIR *directory = opendir("/proc");
	struct dirent *entry;
	int found = 0;

	if (!directory)
		return 1;
	while (!found && (entry = readdir(directory))) {
		char path[300], comm[64] = "";
		FILE *file;

		if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
			continue;
		snprintf(path, sizeof path, "/proc/%s/comm", entry->d_name);
		file = fopen(path, "r");
		if (!file)
			continue;
		if (fgets(comm, sizeof comm, file) && !strncmp(comm, "timedoctor", 10))
			found = 1;
		fclose(file);
	}
	closedir(directory);
	return found;
}

int main(void)
{
	char lock_path[256];
	const char *runtime_directory = getenv("XDG_RUNTIME_DIR");
	int lock_fd, inotify_fd, keycode, event_base, error_base, major, minor, direction = 1, click_offset = 0, i;
	long last_motion = 0, last_seen_timedoctor = now_ms();
	Display *display;

	snprintf(lock_path, sizeof lock_path, "%s/td-inputbridge.lock", runtime_directory ? runtime_directory : "/tmp");
	lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) < 0)
		return 0;	/* already running */

	display = XOpenDisplay(NULL);
	if (!display || !XTestQueryExtension(display, &event_base, &error_base, &major, &minor)) {
		fprintf(stderr, "td-inputbridge: no X display with XTest\n");
		return 1;
	}
	keycode = pick_blank_keycode(display);
	if (!keycode) {
		fprintf(stderr, "td-inputbridge: no unmapped keycode available\n");
		return 1;
	}

	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	inotify_add_watch(inotify_fd, "/dev/input", IN_CREATE | IN_ATTRIB);
	scan_devices();
	fprintf(stderr, "td-inputbridge: %d devices, blank keycode %d\n", device_count, keycode);

	for (;;) {
		struct pollfd poll_fds[MAX_DEVICES + 2];
		int poll_count = 0, polled_devices = device_count, x_fd = ConnectionNumber(display);

		for (i = 0; i < device_count; i++)
			poll_fds[poll_count++] = (struct pollfd){ devices[i].fd, POLLIN, 0 };
		poll_fds[poll_count++] = (struct pollfd){ inotify_fd, POLLIN, 0 };
		poll_fds[poll_count++] = (struct pollfd){ x_fd, POLLIN, 0 };

		if (poll(poll_fds, poll_count, 10000) < 0 && errno != EINTR)
			return 1;

		if (poll_fds[poll_count - 1].revents & (POLLHUP | POLLERR))
			return 0;	/* X server went away */
		if (poll_fds[poll_count - 1].revents & POLLIN) {
			while (XPending(display)) {
				XEvent x_event;
				XNextEvent(display, &x_event);
			}
		}
		if (poll_fds[poll_count - 2].revents & POLLIN) {
			char buffer[4096];
			while (read(inotify_fd, buffer, sizeof buffer) > 0)
				;
			usleep(200000);	/* let udev apply permissions */
			scan_devices();
		}

		/* Only the devices in poll_fds; ones the rescan just added wait for the next poll. */
		for (i = polled_devices - 1; i >= 0; i--) {
			struct input_event events[64];
			ssize_t bytes_read;
			int key_presses, x, y;
			enum warp warp;

			if (!(poll_fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			bytes_read = read(devices[i].fd, events, sizeof events);
			if (bytes_read < 0 && errno != EAGAIN) {
				drop_device(i);
				continue;
			}
			warp = read_batch(&devices[i], events, bytes_read / (ssize_t)sizeof events[0], &key_presses);
			while (key_presses--) {
				XTestFakeKeyEvent(display, keycode, True, CurrentTime);
				XTestFakeKeyEvent(display, keycode, False, CurrentTime);
			}
			if (warp == WARP_CLICK)
				click_offset = !click_offset;
			if (warp == WARP_CLICK || (warp == WARP_MOTION && now_ms() - last_motion >= MOTION_MIN_MS)) {
				/* Both layouts are read fresh for every warp (well under 1ms), so a hotplug never leaves them stale. */
				if (cursor_position(&x, &y) && load_monitors(display)) {
					int root_x, root_y;

					if (layout_to_root(layout_monitors, layout_monitor_count, root_monitors, root_monitor_count,
							   x, y, &root_x, &root_y)) {
						XTestFakeMotionEvent(display, -1, root_x + click_offset, root_y, CurrentTime);
					} else {
						/* A warp to untranslated coordinates lands on the wrong output. */
						static int warned;
						if (!warned++)
							fprintf(stderr, "td-inputbridge: cursor on no output XWayland lists, warp skipped\n");
					}
				} else {
					/* No Hyprland IPC or no output list: the old nudge, which only sometimes registers. */
					static int warned;
					if (!warned++)
						fprintf(stderr, "td-inputbridge: Hyprland cursorpos or output list unavailable, falling back to 1px nudge\n");
					XTestFakeRelativeMotionEvent(display, direction, 0, CurrentTime);
					direction = -direction;
				}
				last_motion = now_ms();
			}
		}
		XFlush(display);

		if (timedoctor_running())
			last_seen_timedoctor = now_ms();
		else if (now_ms() - last_seen_timedoctor > EXIT_GRACE_SECONDS * 1000L)
			return 0;
	}
}
