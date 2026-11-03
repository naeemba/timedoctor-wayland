/*
 * td-inputbridge-start: part of td-waylandshot.so. Starts td-inputbridge for
 * every Time Doctor that loads the shim, so a Time Doctor that relaunches
 * itself (heartbeat restart, update, "Clear cache and restart") gets its own
 * bridge too: the relaunch runs the timedoctor binary directly, not the
 * launcher, but inherits LD_PRELOAD.
 *
 * The bridge is forked from this constructor, which runs on the main thread,
 * so its parent is Time Doctor and its death signal fires when Time Doctor
 * exits. It is passed getpid() as its owner, and a bridge started while the
 * previous Time Doctor's bridge is still alive waits for that one to exit.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void log_message(const char *format, ...);

__attribute__((constructor)) static void start_input_bridge(void)
{
	Dl_info info;
	char library[PATH_MAX], bridge[PATH_MAX], owner[16];
	const char *log_path = getenv("TD_SHOT_LOG");
	pid_t child;

	/* LD_PRELOAD reaches Time Doctor's helper processes too; only the app itself needs a bridge. */
	if (strcmp(program_invocation_short_name, "timedoctor"))
		return;
	if (!dladdr((void *)start_input_bridge, &info) || !info.dli_fname)
		return;
	snprintf(library, sizeof library, "%s", info.dli_fname);
	snprintf(bridge, sizeof bridge, "%s/td-inputbridge", dirname(library));
	snprintf(owner, sizeof owner, "%ld", (long)getpid());

	child = fork();
	if (child < 0)
		log_message("td-inputbridge: fork failed: %s", strerror(errno));
	if (child != 0)
		return;
	/* Only async-signal-safe calls between fork and exec. */
	if (log_path) {
		int log_fd = open(log_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);

		if (log_fd >= 0)
			dup2(log_fd, STDERR_FILENO);
	}
	execl(bridge, bridge, owner, (char *)NULL);
	_exit(127);
}
