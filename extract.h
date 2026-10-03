/* extract.h - run programs without a shell, extract archives */
#ifndef EXTRACT_H
#define EXTRACT_H

#include <stddef.h>
#include "inst.h"

/* argv -> one Windows command line (MSVCRT quoting rules). 0 on overflow or empty argv. */
size_t ex_cmdline(char *out, size_t cap, const char *const argv[]);
/* run argv[0] with args, no shell, wait. exit code, or -1 if it could not start */
int    ex_run(const char *const argv[]);
/* unpack archive (zip, nupkg, tar, gz, bz2, xz, 7z) into dest, creating it. 0 on success */
int    ex_extract(InstType t, const char *archive, const char *dest);

#endif
