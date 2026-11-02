/*
 * td-sessiontype: part of td-waylandshot.so. Keeps Time Doctor 3.20+ on its
 * X11 code paths under Hyprland.
 *
 * 3.20 decides between its X11 and Wayland backends by running
 * "/usr/bin/loginctl show-session <id> -p Type --value" through popen(). On
 * Wayland it then expects GNOME Shell's extension and td-inputd for input,
 * window tracking and screenshots, and leaves mouse movement uncounted. The
 * X11 paths work under XWayland with the rest of this shim (grim screenshots,
 * Hyprland active window) and td-inputbridge (real input mirrored into X).
 *
 * This answers "x11" to that one query and passes every other popen() through.
 * The launcher also hides WAYLAND_DISPLAY, the second thing 3.20 checks.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

FILE *popen(const char *command, const char *type)
{
	static FILE *(*real)(const char *, const char *);

	if (!real)
		real = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, "popen");
	if (command && strstr(command, "loginctl show-session") && strstr(command, " -p Type "))
		return real("echo x11", type);
	return real(command, type);
}
