/* compat.h -- portability shims for MSVC and other non-POSIX platforms */
#ifndef TTYD_CLIENT_COMPAT_H
#define TTYD_CLIENT_COMPAT_H

#ifdef _MSC_VER
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define strcasecmp _stricmp
#define strncasecmp _strnicmp

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif

typedef SSIZE_T ssize_t;

#endif /* _MSC_VER */

/* `strsep` is missing on Windows/MSVC and on some BSD variants */
#if defined(_WIN32) && !defined(__MINGW32__)
char *strsep(char **sp, char *sep);
#endif

/* `strdup` is POSIX; MSVC only provides `_strdup` */
#ifdef _MSC_VER
#ifndef strdup
#define strdup _strdup
#endif
#endif

#endif /* TTYD_CLIENT_COMPAT_H */
