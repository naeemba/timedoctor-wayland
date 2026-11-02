/*
 * td-microphone: part of td-waylandshot.so. Gives Time Doctor 3.20.14 on
 * Linux the microphone signal its Windows/macOS builds have.
 *
 * Time Doctor's Meeting time is "the total time a user's microphone is
 * active", and on a call it does not stop the timer at the idle check; both
 * come from microphone detection that the Linux build lacks. This file:
 *
 *  - polls PipeWire (pactl) every POLL_SECONDS for apps recording from a real
 *    microphone (monitor sources excluded, paused streams ignored); only the
 *    fact that a stream is open is read, never audio;
 *  - emits Time Doctor's own SFProcCoreApplication::audioCaptureChanged(
 *    app, appName, active) signal when an app starts or stops recording,
 *    which TimeBlock records as audio capture (Meeting time);
 *  - detours the native "is the user on a call" check (offset ON_CALL_OFFSET
 *    in libtimedoctor.so 3.20.14) to also answer yes while the microphone is
 *    in use. The detour is installed only if the function's first bytes match
 *    3.20.14 exactly, so another version is left untouched.
 *
 * Qt is reached through dlsym with its Qt 6.3 ABI spelled out below, so the
 * shim needs no Qt headers and binds to the Qt that Time Doctor bundles.
 * Polling runs on its own thread; the object-tree search and the detour run
 * on Time Doctor's GUI thread, which owns the tree and so is not inside the
 * on-call check while it is patched.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void log_message(const char *format, ...);

#define POLL_SECONDS 5
#define MAX_APPS 16
#define ON_CALL_OFFSET 0x1f1b40
/* "Don't break user on a call" setting, read the same way the original does. */
#define ON_CALL_ENABLED_FLAG 0x363
/* endbr64; push r15; push r14; push r13; push r12; push rbp; push rbx; mov rbx,rdi */
static const unsigned char ON_CALL_PROLOGUE[] = {
	0xf3, 0x0f, 0x1e, 0xfa, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
	0x41, 0x54, 0x55, 0x53, 0x48, 0x89, 0xfb,
};

struct app { char binary[128], name[128]; };

static volatile int microphone_active;

/* --- microphone state from PipeWire ------------------------------------ */

#define MAX_MONITORS 32

/* Indexes of monitor sources (speaker loopbacks), which are not a microphone. */
static int monitor_sources(char indexes[][32])
{
	char line[512], name[400];
	FILE *sources = popen("LC_ALL=C pactl list short sources 2>/dev/null", "r");
	int count = 0;

	if (!sources)
		return 0;
	while (fgets(line, sizeof line, sources) && count < MAX_MONITORS)
		if (sscanf(line, "%31s %399s", indexes[count], name) == 2 &&
		    strlen(name) > 8 && !strcmp(name + strlen(name) - 8, ".monitor"))
			count++;
	pclose(sources);
	return count;
}

static int is_listed(char indexes[][32], int count, const char *index)
{
	for (int i = 0; i < count; i++)
		if (!strcmp(indexes[i], index))
			return 1;
	return 0;
}

static void quoted_value(const char *line, char *out, size_t size)
{
	const char *start = strchr(line, '"'), *end = start ? strrchr(line, '"') : NULL;

	if (start && end > start)
		snprintf(out, size, "%.*s", (int)(end - start - 1), start + 1);
}

/* Apps with an open, unpaused recording stream on a non-monitor source. */
static int recording_apps(struct app *apps)
{
	char line[1024], source[32] = "", monitors[MAX_MONITORS][32];
	struct app current = { "", "" };
	int count = 0, in_stream = 0, corked = 0, monitor_count = monitor_sources(monitors);
	FILE *outputs = popen("LC_ALL=C pactl list source-outputs 2>/dev/null", "r");

	if (!outputs)
		return 0;
	for (;;) {
		int more = fgets(line, sizeof line, outputs) != NULL;

		if (!more || !strncmp(line, "Source Output #", 15)) {
			if (in_stream && !corked && source[0] && !is_listed(monitors, monitor_count, source) &&
			    count < MAX_APPS)
				apps[count++] = current;
			if (!more)
				break;
			in_stream = 1, corked = 0, source[0] = 0;
			current = (struct app){ "", "" };
			continue;
		}
		sscanf(line, " Source: %31s", source);
		if (strstr(line, "Corked: yes"))
			corked = 1;
		else if (strstr(line, "application.process.binary = "))
			quoted_value(line, current.binary, sizeof current.binary);
		else if (strstr(line, "application.name = "))
			quoted_value(line, current.name, sizeof current.name);
	}
	pclose(outputs);
	return count;
}

static int contains(const struct app *apps, int count, const struct app *wanted)
{
	for (int i = 0; i < count; i++)
		if (!strcmp(apps[i].binary, wanted->binary) && !strcmp(apps[i].name, wanted->name))
			return 1;
	return 0;
}

/* --- Qt, through its Qt 6.3 ABI --------------------------------------- */

struct qstring { void *d; void *ptr; long size; };
struct qbytearrayview { long size; const char *data; };
struct qgenericargument { const void *data; const char *name; };

typedef struct qstring (*from_utf8_fn)(struct qbytearrayview);
typedef int (*invoke_method_fn)(void *, const char *, int, struct qgenericargument,
	struct qgenericargument, struct qgenericargument, struct qgenericargument,
	struct qgenericargument, struct qgenericargument, struct qgenericargument,
	struct qgenericargument, struct qgenericargument, struct qgenericargument,
	struct qgenericargument);

#define QT_QUEUED_CONNECTION 2
#define AUDIO_CAPTURE_SIGNAL "audioCaptureChanged(QString,QString,bool)"
#define MAX_OBJECTS_SEARCHED 5000
/* One search per poll while Time Doctor starts up: a minute before giving up. */
#define MAX_SEARCHES 12

/* Qt 6.3 layouts: QObject { vptr; QObjectData *d_ptr; },
 * QObjectData { vptr; QObject *q_ptr; QObject *parent; QList<QObject *> children; }. */
struct qobject { void **vtable; struct qobject_data *data; };
struct qobject_data { void *vtable; void *q; void *parent; void *children_d; struct qobject **children; long children_count; };

typedef const void *(*meta_object_fn)(const struct qobject *);
typedef int (*index_of_signal_fn)(const void *, const char *);
typedef const char *(*class_name_fn)(const void *);

/*
 * The signal lives on SFProcCoreApplication, which is not the QApplication
 * itself (that is WebApplication); breadth-first search of the object tree
 * under the application for the object whose class declares the signal.
 */
static struct qobject *find_signal_owner(struct qobject *root)
{
	index_of_signal_fn index_of_signal = (index_of_signal_fn)dlsym(RTLD_DEFAULT, "_ZNK11QMetaObject13indexOfSignalEPKc");
	class_name_fn class_name = (class_name_fn)dlsym(RTLD_DEFAULT, "_ZNK11QMetaObject9classNameEv");
	static struct qobject *queue[MAX_OBJECTS_SEARCHED];
	int head = 0, tail = 0;

	if (!index_of_signal || !class_name)
		return NULL;
	queue[tail++] = root;
	while (head < tail) {
		struct qobject *object = queue[head++];
		const void *meta = ((meta_object_fn)object->vtable[0])(object);

		if (index_of_signal(meta, AUDIO_CAPTURE_SIGNAL) >= 0) {
			log_message("microphone: %s found on %s after %d objects", AUDIO_CAPTURE_SIGNAL, class_name(meta), head);
			return object;
		}
		for (long i = 0; i < object->data->children_count && tail < MAX_OBJECTS_SEARCHED; i++)
			queue[tail++] = object->data->children[i];
	}
	log_message("microphone: %s not found under %s (%d objects)", AUDIO_CAPTURE_SIGNAL,
	       class_name(((meta_object_fn)root->vtable[0])(root)), head);
	return NULL;
}

/* QCoreApplication::self, or NULL until Time Doctor has loaded Qt and created its application. */
static struct qobject *qt_application(void)
{
	void **application = (void **)dlsym(RTLD_DEFAULT, "_ZN16QCoreApplication4selfE");

	return application ? *application : NULL;
}

/* Set on the GUI thread, read by the watcher. */
static struct qobject *volatile signal_owner;

/* Queued to the application's thread, as Time Doctor's own platform code would emit it.
 * Returns whether the event was sent. */
static int emit_audio_capture_changed(const struct app *app, int active)
{
	static from_utf8_fn from_utf8;
	static invoke_method_fn invoke_method;
	struct qobject *owner = signal_owner;
	struct qgenericargument none = { 0, 0 };
	struct qstring binary, name;
	_Bool state = active;

	if (!invoke_method) {
		from_utf8 = (from_utf8_fn)dlsym(RTLD_DEFAULT, "_ZN7QString8fromUtf8E14QByteArrayView");
		invoke_method = (invoke_method_fn)dlsym(RTLD_DEFAULT,
			"_ZN11QMetaObject12invokeMethodEP7QObjectPKcN2Qt14ConnectionTypeE22QGenericReturnArgument"
			"16QGenericArgumentS7_S7_S7_S7_S7_S7_S7_S7_S7_");
	}
	if (!from_utf8 || !invoke_method) {
		log_message("microphone: Qt symbols missing, audio capture event dropped");
		return 0;
	}
	if (!owner)
		return 0;
	/* ponytail: the two QStrings are never released; a few bytes per mic start/stop. */
	binary = from_utf8((struct qbytearrayview){ (long)strlen(app->binary), app->binary });
	name = from_utf8((struct qbytearrayview){ (long)strlen(app->name), app->name });
	if (!invoke_method(owner, "audioCaptureChanged", QT_QUEUED_CONNECTION, none,
			   (struct qgenericargument){ &binary, "QString" },
			   (struct qgenericargument){ &name, "QString" },
			   (struct qgenericargument){ &state, "bool" },
			   none, none, none, none, none, none, none)) {
		log_message("microphone: invoking audioCaptureChanged failed");
		return 0;
	}
	log_message("microphone: %s (%s) %s", app->name, app->binary, active ? "started" : "stopped");
	return 1;
}

/* --- "is the user on a call" detour ------------------------------------ */

typedef _Bool (*on_call_fn)(void *, _Bool);
static on_call_fn original_on_call;

static _Bool on_call(void *self, _Bool force)
{
	if (microphone_active && (force || ((unsigned char *)self)[ON_CALL_ENABLED_FLAG]))
		return 1;
	return original_on_call(self, force);
}

static int find_timedoctor_library(struct dl_phdr_info *info, size_t size, void *base)
{
	(void)size;
	if (info->dlpi_name && strstr(info->dlpi_name, "/libtimedoctor.so")) {
		*(uintptr_t *)base = info->dlpi_addr;
		return 1;
	}
	return 0;
}

static void write_absolute_jump(unsigned char *at, const void *target)
{
	at[0] = 0xff, at[1] = 0x25, memset(at + 2, 0, 4);	/* jmp [rip+0] */
	memcpy(at + 6, &target, 8);
}

static int install_on_call_detour(void)
{
	uintptr_t base = 0;
	unsigned char *function, *trampoline;
	long page = sysconf(_SC_PAGESIZE);
	uintptr_t page_start;

	dl_iterate_phdr(find_timedoctor_library, &base);
	if (!base)
		return 0;	/* not loaded yet */
	function = (unsigned char *)(base + ON_CALL_OFFSET);
	if (memcmp(function, ON_CALL_PROLOGUE, sizeof ON_CALL_PROLOGUE)) {
		log_message("microphone: on-call check not at the 3.20.14 offset; timer detour skipped");
		return -1;
	}
	trampoline = mmap(NULL, page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (trampoline == MAP_FAILED)
		return -1;
	memcpy(trampoline, function, sizeof ON_CALL_PROLOGUE);
	write_absolute_jump(trampoline + sizeof ON_CALL_PROLOGUE, function + sizeof ON_CALL_PROLOGUE);
	mprotect(trampoline, page, PROT_READ | PROT_EXEC);
	original_on_call = (on_call_fn)trampoline;

	page_start = (uintptr_t)function & ~(uintptr_t)(page - 1);
	if (mprotect((void *)page_start, 2 * page, PROT_READ | PROT_WRITE | PROT_EXEC)) {
		log_message("microphone: mprotect failed (%s); timer detour skipped", strerror(errno));
		return -1;
	}
	write_absolute_jump(function, (const void *)on_call);
	memset(function + 14, 0x90, sizeof ON_CALL_PROLOGUE - 14);	/* nop the rest of the moved prologue */
	mprotect((void *)page_start, 2 * page, PROT_READ | PROT_EXEC);
	log_message("microphone: on-call check detoured");
	return 1;
}

/* --- work on the GUI thread --------------------------------------------- */

/* Qt 6.3 QtPrivate::QSlotObjectBase: { QAtomicInt ref; ImplFn impl; }, no vtable.
 * A queued invokeMethodImpl calls impl(CALL) on the receiver's thread, then
 * impl(DESTROY) once the last reference is gone. */
struct qslot_object;
typedef void (*slot_implementation_fn)(int, struct qslot_object *, void *, void **, _Bool *);
struct qslot_object { int reference_count; slot_implementation_fn implementation; };
typedef _Bool (*invoke_method_impl_fn)(void *, struct qslot_object *, int, void *);
#define QT_SLOT_DESTROY 0
#define QT_SLOT_CALL 1

/* Detour state: 0 not tried yet (libtimedoctor.so not loaded), 1 installed, -1 skipped. */
static volatile int detour;
static volatile int searches_left = MAX_SEARCHES;
static volatile int gui_work_queued;

static void gui_work(int which, struct qslot_object *slot, void *application, void **arguments, _Bool *result)
{
	(void)arguments, (void)result;
	if (which == QT_SLOT_DESTROY) {
		free(slot);
		return;
	}
	if (which != QT_SLOT_CALL)
		return;
	/* The GUI thread is running this, so it is not inside the on-call check being patched. */
	if (!detour)
		detour = install_on_call_detour();
	if (!signal_owner && searches_left > 0) {
		searches_left--;
		signal_owner = find_signal_owner(application);
	}
	gui_work_queued = 0;
}

static void queue_gui_work(struct qobject *application)
{
	static invoke_method_impl_fn invoke_method_impl;
	struct qslot_object *slot;

	if (gui_work_queued || (detour && (signal_owner || searches_left <= 0)))
		return;
	if (!invoke_method_impl)
		invoke_method_impl = (invoke_method_impl_fn)dlsym(RTLD_DEFAULT,
			"_ZN11QMetaObject16invokeMethodImplEP7QObjectPN9QtPrivate15QSlotObjectBaseEN2Qt14ConnectionTypeEPv");
	if (!invoke_method_impl || !(slot = malloc(sizeof *slot)))
		return;
	*slot = (struct qslot_object){ 1, gui_work };
	gui_work_queued = 1;
	if (!invoke_method_impl(application, slot, QT_QUEUED_CONNECTION, NULL))
		gui_work_queued = 0;
}

/* --- watcher thread ------------------------------------------------------ */

static void *watch_microphone(void *unused)
{
	struct app previous[MAX_APPS], current[MAX_APPS];
	int previous_count = 0;

	(void)unused;
	for (;;) {
		struct qobject *application;
		int count, announced = 0;

		sleep(POLL_SECONDS);
		application = qt_application();
		if (!application)
			continue;	/* an event emitted now would be lost */
		queue_gui_work(application);
		count = recording_apps(current);
		for (int i = 0; i < previous_count; i++)
			if (!contains(current, count, &previous[i]))
				emit_audio_capture_changed(&previous[i], 0);
		/* An app whose start was not sent stays out of previous, so the next poll retries it. */
		for (int i = 0; i < count; i++)
			if (contains(previous, previous_count, &current[i]) || emit_audio_capture_changed(&current[i], 1))
				current[announced++] = current[i];
		microphone_active = count > 0;
		memcpy(previous, current, announced * sizeof current[0]);
		previous_count = announced;
	}
	return NULL;
}

__attribute__((constructor)) static void start_microphone_watch(void)
{
	pthread_t thread;

	/* LD_PRELOAD reaches Time Doctor's helper processes too; only the app itself needs this. */
	if (strcmp(program_invocation_short_name, "timedoctor"))
		return;
	if (!pthread_create(&thread, NULL, watch_microphone, NULL))
		pthread_detach(thread);
}
