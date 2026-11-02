/*
 * Self-check for td-inputbridge's pointer decisions; no X server or input
 * devices needed.
 *   make test
 */
#define main inputbridge_main
#include "../td-inputbridge.c"
#undef main
#include <assert.h>

#define EVENT(event_type, event_code, event_value) { .type = (event_type), .code = (event_code), .value = (event_value) }
#define LENGTH(array) ((int)(sizeof (array) / sizeof (array)[0]))
#define BATCH(device, array) read_batch(&devices[device], (array), LENGTH(array), &keys)

int main(void)
{
	int keys;
	struct input_event press[] = { EVENT(EV_KEY, BTN_LEFT, 1) };
	struct input_event move[] = { EVENT(EV_REL, REL_X, 3), EVENT(EV_REL, REL_Y, -1) };
	struct input_event release[] = { EVENT(EV_KEY, BTN_LEFT, 0) };
	struct input_event right_press[] = { EVENT(EV_KEY, BTN_RIGHT, 1) };
	struct input_event right_release[] = { EVENT(EV_KEY, BTN_RIGHT, 0) };
	struct input_event click_in_one_batch[] = { EVENT(EV_KEY, BTN_LEFT, 1), EVENT(EV_KEY, BTN_LEFT, 0) };
	struct input_event typing[] = { EVENT(EV_KEY, KEY_A, 1), EVENT(EV_KEY, KEY_A, 2), EVENT(EV_KEY, KEY_A, 0), EVENT(EV_KEY, KEY_B, 1) };
	struct input_event touch_down[] = { EVENT(EV_KEY, BTN_TOUCH, 1), EVENT(EV_ABS, ABS_X, 10) };
	struct input_event touch_up[] = { EVENT(EV_KEY, BTN_TOUCH, 0) };

	device_count = 2;	/* a mouse and a touchpad */
	devices[0].fd = devices[1].fd = -1;

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
	assert(BATCH(0, typing) == WARP_NONE && keys == 2);

	/* A resting finger is not a held button: motion keeps flowing. */
	assert(BATCH(1, touch_down) == WARP_CLICK);
	assert(BATCH(1, move) == WARP_MOTION);
	assert(BATCH(1, touch_up) == WARP_NONE);

	/* A button held on the mouse stops warps from the touchpad too. */
	BATCH(0, press);
	assert(BATCH(1, move) == WARP_NONE);

	/* Losing the mouse mid-click frees the pointer; losing another device does not. */
	devices[2] = devices[1];
	device_count = 3;
	drop_device(2);
	assert(BATCH(1, move) == WARP_NONE);
	drop_device(0);
	assert(BATCH(0, move) == WARP_MOTION);

	puts("inputbridge: ok");
	return 0;
}
