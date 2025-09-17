/* $OpenBSD$ */

/*
 * Copyright (c) 2011 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include "tmux.h"

char *
osdep_get_name(__unused int fd, __unused char *tty)
{
	return (NULL);
}

char *
osdep_get_cwd(__unused int fd)
{
	return (NULL);
}

char *
osdep_get_tmux_path(const char *argv0)
{
	static char	exe_path[PATH_MAX] = {0};
	ssize_t		len;

	if (exe_path[0])
		return (exe_path);
	/* n.b. This is not documented on IBM's proc(5) manpage, which means it
	 * probably isn't supported yet (IBM documents everything). However,
	 * in the past, they have added features for compatibility with
	 * Solaris (cf. the lwp subdir, which is from Solaris)
	 */
	len = readlink("/proc/self/execname", exe_path, sizeof(exe_path));
	if (len > 0) {
		len = min(len, sizeof(exe_path) - 1);
		exe_path[len] = '\0';
		return (exe_path);
	}
	len = readlink("/proc/self/paths/a.out", exe_path, sizeof(exe_path));
	if (len > 0) {
		len = min(len, sizeof(exe_path) - 1);
		exe_path[len] = '\0';
		return (exe_path);
	}
	/* This _is_ documented, but it's not guaranteed to be a symlink */
	len = readlink("/proc/self/objects/a.out", exe_path, sizeof(exe_path));
	if (len > 0) {
		len = min(len, sizeof(exe_path) - 1);
		exe_path[len] = '\0';
		return (exe_path);
	}
	if (argv0) {
		if (find_tmux(argv0, exe_path, sizeof(exe_path)) == 0)
			return (exe_path);
	}

	return (NULL);
}

struct event_base *
osdep_event_init(void)
{
	return (event_init());
}
