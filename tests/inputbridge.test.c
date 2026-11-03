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

	puts("inputbridge: ok");
	return 0;
}
