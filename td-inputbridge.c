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
 *                              position (read from Hyprland's IPC socket)
 *   - real click/tap        -> the same warp, offset by 1px on every other
 *                              click so a click in place still moves it
 *                              (never an X button press)
 * Time Doctor compares pointer positions over time, so the X pointer has to
 * actually end up somewhere new; a nudge that undoes itself reads as idle.
 * Nothing is generated without real input, and key codes are never logged.
 *
 * Installed setgid "input" so only this binary can open /dev/input.
 * Exits when no "timedoctor" process has existed for EXIT_GRACE_SECONDS seconds.
 */
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
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

struct device { int fd; char name[32]; };
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
		devices[device_count].fd = fd;
		snprintf(devices[device_count].name, sizeof devices[device_count].name, "%.31s", entry->d_name);
		device_count++;
	}
	closedir(directory);
}

static void drop_device(int i)
{
	close(devices[i].fd);
	devices[i] = devices[--device_count];
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

/* Real cursor position in layout coordinates, which match XWayland's at scale 1. */
static int cursor_position(int *x, int *y)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR"), *instance = getenv("HYPRLAND_INSTANCE_SIGNATURE");
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	struct timeval timeout = { 0, 50000 };	/* never stall the input loop on a busy Hyprland */
	char reply[64] = "";
	int fd, ok = 0;

	if (!runtime || !instance)
		return 0;
	snprintf(address.sun_path, sizeof address.sun_path, "%s/hypr/%s/.socket.sock", runtime, instance);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
	if (!connect(fd, (struct sockaddr *)&address, sizeof address) &&
	    write(fd, "cursorpos", sizeof "cursorpos" - 1) == sizeof "cursorpos" - 1 && read(fd, reply, sizeof reply - 1) > 0)
		ok = sscanf(reply, "%d, %d", x, y) == 2;
	close(fd);
	return ok;
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
			int key = 0, motion = 0, click = 0, x, y, j;

			if (!(poll_fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			bytes_read = read(devices[i].fd, events, sizeof events);
			if (bytes_read < 0 && errno != EAGAIN) {
				drop_device(i);
				continue;
			}
			for (j = 0; j < bytes_read / (ssize_t)sizeof events[0]; j++) {
				if (events[j].type == EV_KEY && events[j].value == 1) {
					if (events[j].code < BTN_MISC || events[j].code >= KEY_OK)
						key++;
					else
						click = 1;	/* mouse button / touch */
				} else if (events[j].type == EV_REL || events[j].type == EV_ABS) {
					motion = 1;
				}
			}
			while (key--) {
				XTestFakeKeyEvent(display, keycode, True, CurrentTime);
				XTestFakeKeyEvent(display, keycode, False, CurrentTime);
			}
			if (click)
				click_offset = !click_offset;
			if (click || (motion && now_ms() - last_motion >= MOTION_MIN_MS)) {
				if (cursor_position(&x, &y)) {
					XTestFakeMotionEvent(display, -1, x + click_offset, y, CurrentTime);
				} else {
					/* No Hyprland IPC: the old nudge, which only sometimes registers. */
					static int warned;
					if (!warned++)
						fprintf(stderr, "td-inputbridge: Hyprland cursorpos unavailable, falling back to 1px nudge\n");
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
