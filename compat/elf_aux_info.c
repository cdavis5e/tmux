/*
 * Copyright (c) 2025 Chip Davis <cdavis5x+dev@gmail.com>
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

#include <errno.h>

#ifdef HAVE_ELF_H
#include <elf.h>
#elif defined(HAVE_SYS_ELF_H)
#include <sys/elf.h>
#elif defined(HAVE_SYS_EXEC_ELF_H)
#include <sys/exec_elf.h>
#endif

#include "compat.h"

#ifndef AT_EXECPATH
#ifdef AT_EXECFN
#define AT_EXECPATH AT_EXECFN
#elif defined(AT_SUN_EXECNAME)
#define AT_EXECPATH AT_SUN_EXECNAME
#endif
#endif

#ifndef HAVE_AUXINFO
#ifdef HAVE_ELF_AUXINFO
typedef Elf_Auxinfo	AuxInfo;
#elif defined(HAVE_AUXV_T)
typedef auxv_t		AuxInfo;
#elif defined(ElfW)
typedef ElfW(auxv_t)	AuxInfo;
#endif
#endif

int
elf_aux_info(int type, void *buf, size_t size)
{
#ifdef __ELF__
	unsigned long	v;
#ifndef HAVE_GETAUXVAL
	extern char	**environ;
	char		**var;
	AuxInfo		*auxv;
#endif

	if (type >= AT_COUNT)
		return (errno = EINVAL);
#ifdef HAVE_GETAUXVAL
	v = getauxval(type);
	if (v == 0 && errno == ENOENT)
		return (errno);
#elif !defined(__linux__) || !defined(__powerpc__)
	/* n.b. Doesn't work on Linux/ppc: auxv is passed differently there */
	for (var = environ; *var != NULL; var++)
		;
	for (auxv = (AuxInfo *)var;
	     auxv->a_type != AT_NULL && auxv->a_type != type;
	     auxv++)
		;
	if (auxv->a_type == AT_NULL)
		return (errno = ENOENT);
	v = auxv->a_un.a_val;
#else
	/* TODO: Implement */
	return (errno = ENOENT);
#endif

	/* TODO: Some others are data blocks, but this is all we need for now */
#ifdef AT_EXECPATH
	if (type == AT_EXECPATH) {
		if (strlcpy(buf, size, (const char *)v) > size)
			return (errno = EINVAL);
	} else
#endif
	{
		if (size != sizeof(unsigned long))
			return (errno = EINVAL);
		*(unsigned long *)buf = v;
	}
	return (0);
#else
	(void)type;
	(void)buf;
	(void)size;
	return (errno = ENOSYS);
#endif
}
