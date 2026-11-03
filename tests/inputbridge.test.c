/*
 * Self-check for td-inputbridge's pointer decisions; no X server or input
 * devices needed.
 *   make test
 */
#define main inputbridge_main
#include "../td-inputbridge.c"
#undef main
#include <assert.h>
#include <sys/wait.h>

/*
 * A launcher process starts a bridge-like child tied to it, then exits; the
 * child must die with it. The child holds the write end of `alive`, so the
 * read sees end-of-file once it is gone. A child tied to a pid that is not
 * its parent gives up at once.
 */
static void check_owner_tie(void)
{
	int alive[2], ready[2];
	char byte;
	pid_t launcher;
	int status;
	struct pollfd watch;

	assert(pipe(alive) == 0 && pipe(ready) == 0);
	launcher = fork();
	assert(launcher >= 0);
	if (launcher == 0) {
		if (fork() == 0) {
			close(alive[0]), close(ready[0]);
			if (!tie_to_owner(getppid()))
				_exit(1);
			if (write(ready[1], "x", 1) != 1)
				_exit(1);
			alarm(10);	/* never outlive a failed check for long */
			for (;;)
				pause();
		}
		close(ready[1]);
		_exit(read(ready[0], &byte, 1) == 1 ? 0 : 1);	/* Time Doctor quits once the bridge is set up */
	}
	close(alive[1]), close(ready[1]);
	assert(waitpid(launcher, &status, 0) == launcher && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	watch = (struct pollfd){ alive[0], POLLIN, 0 };
	assert(poll(&watch, 1, 2000) == 1 && read(alive[0], &byte, 1) == 0);
	close(alive[0]), close(ready[0]);

	launcher = fork();
	assert(launcher >= 0);
	if (launcher == 0)
		_exit(tie_to_owner(getppid() + 1));
	assert(waitpid(launcher, &status, 0) == launcher && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* A second bridge waits while the lock is held and takes it once it is free. */
static void check_lock_takeover(void)
{
	char lock_path[] = "/tmp/inputbridge-test-lock-XXXXXX";
	int holder = mkstemp(lock_path), status;
	pid_t waiter;

	assert(holder >= 0 && flock(holder, LOCK_EX) == 0);
	waiter = fork();
	assert(waiter >= 0);
	if (waiter == 0) {
		close(holder);	/* a forked copy would hold the lock too */
		alarm(10);
		_exit(take_lock(lock_path) ? 0 : 1);
	}
	usleep(200000);
	assert(waitpid(waiter, &status, WNOHANG) == 0);	/* still waiting */
	close(holder);	/* the old bridge exits */
	assert(waitpid(waiter, &status, 0) == waiter && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	unlink(lock_path);
}

#define EVENT(event_type, event_code, event_value) { .type = (event_type), .code = (event_code), .value = (event_value) }
#define TEST_BIT_SET(b, a) ((a)[(b) / (8 * sizeof(long))] |= 1UL << ((b) % (8 * sizeof(long))))
#define LENGTH(array) ((int)(sizeof (array) / sizeof (array)[0]))
#define BATCH(device, array) read_batch(&devices[device], (array), LENGTH(array), &key_presses)

int main(void)
{
	int key_presses;
	struct input_event press[] = { EVENT(EV_KEY, BTN_LEFT, 1) };
	struct input_event move[] = { EVENT(EV_REL, REL_X, 3), EVENT(EV_REL, REL_Y, -1) };
	struct input_event release[] = { EVENT(EV_KEY, BTN_LEFT, 0) };
	struct input_event right_press[] = { EVENT(EV_KEY, BTN_RIGHT, 1) };
	struct input_event right_release[] = { EVENT(EV_KEY, BTN_RIGHT, 0) };
	struct input_event click_in_one_batch[] = { EVENT(EV_KEY, BTN_LEFT, 1), EVENT(EV_KEY, BTN_LEFT, 0) };
	struct input_event typing[] = { EVENT(EV_KEY, KEY_A, 1), EVENT(EV_KEY, KEY_A, 2), EVENT(EV_KEY, KEY_A, 0), EVENT(EV_KEY, KEY_B, 1) };
	struct input_event touch_down[] = { EVENT(EV_KEY, BTN_TOUCH, 1), EVENT(EV_ABS, ABS_X, 10) };
	struct input_event touch_up[] = { EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1), EVENT(EV_KEY, BTN_TOUCH, 0) };
	struct input_event dropped[] = { EVENT(EV_SYN, SYN_DROPPED, 0), EVENT(EV_REL, REL_X, 3), EVENT(EV_SYN, SYN_REPORT, 0) };
	struct input_event stylus_press[] = { EVENT(EV_KEY, BTN_STYLUS, 1) };
	struct input_event stylus_release[] = { EVENT(EV_KEY, BTN_STYLUS, 0) };

	unsigned long key_state[LONGS_FOR_BITS(KEY_MAX + 1)] = {0};

	device_count = 4;	/* a mouse, a touchpad, a display pen tablet and a screenless one */
	devices[0].fd = devices[1].fd = devices[2].fd = devices[3].fd = -1;
	devices[2].touch_is_click = 1;	/* INPUT_PROP_DIRECT */
	devices[3].touch_is_click = 1;	/* BTN_TOOL_PEN, not direct */

	/* A left click: nothing moves the X pointer until the button is up. */
	assert(BATCH(0, press) == WARP_NONE);
	assert(BATCH(0, move) == WARP_NONE);
	assert(BATCH(0, release) == WARP_CLICK);
	assert(BATCH(0, move) == WARP_MOTION);

	/* Press and release read together still warp once, after the release. */
	assert(BATCH(0, click_in_one_batch) == WARP_CLICK);

	/* Two buttons held: releasing one is not enough. */
	BATCH(0, press);
	BATCH(0, right_press);
	assert(BATCH(0, right_release) == WARP_NONE);
	assert(BATCH(0, release) == WARP_CLICK);

	/* A release with nothing counted (held when the bridge started) never goes negative. */
	assert(BATCH(0, release) == WARP_CLICK);
	assert(BATCH(0, move) == WARP_MOTION);

	/* Keys count presses only, never autorepeat or release, and never touch the pointer. */
	assert(BATCH(0, typing) == WARP_NONE && key_presses == 2);

	/* A resting finger is not a held button: motion keeps flowing. */
	assert(BATCH(1, touch_down) == WARP_CLICK);
	assert(BATCH(1, move) == WARP_MOTION);
	assert(BATCH(1, touch_up) == WARP_MOTION);

	/* On a pen tablet the tip is the click: held like a mouse button. */
	assert(BATCH(2, touch_down) == WARP_NONE);
	assert(BATCH(2, move) == WARP_NONE);
	assert(BATCH(2, touch_up) == WARP_CLICK);
	assert(BATCH(3, touch_down) == WARP_NONE);
	assert(BATCH(3, move) == WARP_NONE);
	assert(BATCH(3, touch_up) == WARP_CLICK);

	/* A pen barrel button is held on any device. */
	assert(BATCH(1, stylus_press) == WARP_NONE);
	assert(BATCH(1, move) == WARP_NONE);
	assert(BATCH(1, stylus_release) == WARP_CLICK);

	/* Events lost to an overflow, the release among them: the held state is read back. */
	BATCH(0, press);
	assert(BATCH(0, dropped) == WARP_MOTION);
	assert(BATCH(0, move) == WARP_MOTION);

	/* The read-back keeps a button the kernel still has down... */
	TEST_BIT_SET(BTN_LEFT, key_state);
	held_from_key_state(&devices[0], key_state);
	assert(BATCH(0, move) == WARP_NONE);
	memset(key_state, 0, sizeof key_state);
	held_from_key_state(&devices[0], key_state);
	assert(BATCH(0, move) == WARP_MOTION);

	/* ...but never restores a resting touchpad finger as held. */
	TEST_BIT_SET(BTN_TOUCH, key_state);
	held_from_key_state(&devices[1], key_state);
	assert(BATCH(1, move) == WARP_MOTION);

	/* A button held on the mouse stops warps from the touchpad too. */
	BATCH(0, press);
	assert(BATCH(1, move) == WARP_NONE);

	/* Losing the mouse mid-click frees the pointer; losing another device does not. */
	devices[2] = devices[1];
	drop_device(2);
	assert(BATCH(1, move) == WARP_NONE);
	drop_device(0);
	assert(BATCH(0, move) == WARP_MOTION);

	/* Hyprland's layout and XWayland's root order the same outputs differently. */
	char hyprland_reply[] =
		"Monitor eDP-1 (ID 0):\n\t1920x1080@144.02299 at 4480x360\n\tdescription: BOE 0x0AE3\n"
		"\tmake: BOE\n\tmodel: 0x0AE3\n\tphysical size (mm): 340x190\n\tserial: \n"
		"\tactive workspace: 10 (10)\n\tspecial workspace: 0 ()\n\treserved: 0 42 0 42\n"
		"\tscale: 1\n\ttransform: 0\n\tfocused: no\n\tdisabled: false\n\tmirrorOf: none\n"
		"\tavailableModes: 1920x1080@144.02Hz 2560x1440@120.00Hz \n\n"
		"Monitor DP-3 (ID 1):\n\t2560x1440@59.95100 at 1920x0\n\tscale: 1.00\n\ttransform: 0\n\n"
		"Monitor HDMI-A-1 (ID 2):\n\t1920x1080@60.00000 at 0x360\n\tscale: 1.00\n\ttransform: 0\n";
	struct monitor root[] = {
		{ "eDP-1", 0, 0, 1920, 1080 }, { "DP-3", 1920, 0, 2560, 1440 }, { "HDMI-A-1", 4480, 0, 1920, 1080 },
	};
	struct monitor layout[MAX_MONITORS];
	int layout_count = parse_hyprland_monitors(hyprland_reply, layout, MAX_MONITORS), root_x, root_y;

	assert(layout_count == 3);
	assert(!strcmp(layout[2].name, "HDMI-A-1") && layout[2].x == 0 && layout[2].y == 360);
	assert(layout[2].width == 1920 && layout[2].height == 1080);
	/* The LG lines up in both, so its points pass through unchanged. */
	assert(layout_to_root(layout, layout_count, root, LENGTH(root), 2000, 100, &root_x, &root_y));
	assert(root_x == 2000 && root_y == 100);
	/* The Samsung is leftmost in Hyprland, rightmost and 360px higher in XWayland. */
	assert(layout_to_root(layout, layout_count, root, LENGTH(root), 10, 370, &root_x, &root_y));
	assert(root_x == 4490 && root_y == 10);
	/* The laptop is the other way round. */
	assert(layout_to_root(layout, layout_count, root, LENGTH(root), 6399, 1439, &root_x, &root_y));
	assert(root_x == 1919 && root_y == 1079);
	/* Above the Samsung, in the gap the LG's height leaves: no output, no warp. */
	assert(!layout_to_root(layout, layout_count, root, LENGTH(root), 10, 100, &root_x, &root_y));
	/* An output XWayland does not list yet (just plugged in): no warp. */
	assert(!layout_to_root(layout, layout_count, root, 2, 10, 370, &root_x, &root_y));

	/*
	 * Scale 1.25, rotated 90 and 270 degrees: the layout size is the pixel
	 * size scaled down, then swapped. A 180 degree turn keeps it. A mode-like
	 * line before the first Monitor belongs to no output.
	 */
	char scaled_reply[] = "\t1280x720@60.0 at 9x9\n"
		"Monitor DP-1 (ID 0):\n\t2560x1440@60.0 at 0x0\n\tscale: 1.25\n\ttransform: 1\n"
		"Monitor DP-2 (ID 1):\n\t2560x1440@60.0 at 1152x0\n\tscale: 1.25\n\ttransform: 3\n"
		"Monitor DP-4 (ID 2):\n\t1920x1080@60.0 at 2304x0\n\tscale: 1.00\n\ttransform: 2\n";
	char overflow_reply[sizeof scaled_reply];
	struct monitor scaled_root[] = {
		{ "DP-1", 0, 0, 1440, 2560 }, { "DP-2", 1440, 0, 1440, 2560 }, { "DP-4", 2880, 0, 1920, 1080 },
	};
	memcpy(overflow_reply, scaled_reply, sizeof scaled_reply);
	assert(parse_hyprland_monitors(scaled_reply, layout, MAX_MONITORS) == 3);
	assert(layout[0].x == 0 && layout[0].y == 0);
	assert(layout[0].width == 1152 && layout[0].height == 2048);
	assert(layout[1].width == 1152 && layout[1].height == 2048);
	assert(layout[2].width == 1920 && layout[2].height == 1080);
	/* XWayland sees the output at its pixel size, so the offset scales up with it. */
	assert(layout_to_root(layout, 3, scaled_root, 3, 576, 1024, &root_x, &root_y));
	assert(root_x == 720 && root_y == 1280);
	/* The lower part of the 270 degree output is on it, not past its unrotated height. */
	assert(layout_to_root(layout, 3, scaled_root, 3, 1728, 2000, &root_x, &root_y));
	assert(root_x == 2160 && root_y == 2500);
	/* More outputs than fit: the extra one is dropped and its lines change nothing. */
	assert(parse_hyprland_monitors(overflow_reply, layout, 2) == 2);
	assert(!strcmp(layout[1].name, "DP-2") && layout[1].x == 1152 && layout[1].width == 1152);

	check_owner_tie();
	check_lock_takeover();

	puts("inputbridge: ok");
	return 0;
}
