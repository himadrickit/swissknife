/* inst.c - detect installer flavour / archive type from the file itself.
 * Plain stdio only, so it builds and tests on Linux as well as with MinGW. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "inst.h"
#include "config.h"

#define CHUNK (1 << 20)
#define MARKMAX 64
#define NMARK (sizeof inst_markers / sizeof inst_markers[0])

static int ext_is(const char *hint, const char *ext)
{
	size_t n, el = strlen(ext);
	if (!hint) return 0;
	n = strcspn(hint, "?#");
	if (n < el) return 0;
	for (size_t i = 0; i < el; i++)
		if (tolower((unsigned char)hint[n - el + i]) != ext[i]) return 0;
	return 1;
}

static long long find_mem(const unsigned char *h, size_t hl, const unsigned char *n, size_t nl)
{
	size_t i = 0;
	while (i + nl <= hl) {
		const unsigned char *q = memchr(h + i, n[0], hl - nl - i + 1);
		if (!q) return -1;
		i = (size_t)(q - h);
		if (memcmp(q, n, nl) == 0) return (long long)i;
		i++;
	}
	return -1;
}

/* PE file: look for installer markers over the whole file, earliest one wins */
static InstType scan_pe(FILE *f)
{
	long long first[NMARK], base = 0;
	unsigned char nd[NMARK][2 * MARKMAX];
	size_t nl[NMARK], keep = 0, have = 0, best = NMARK;
	unsigned char *buf;

	for (size_t i = 0; i < NMARK; i++) {
		size_t l = strlen(inst_markers[i].marker);
		first[i] = -1;
		nl[i] = 0;
		if (l == 0 || l >= MARKMAX) continue;      /* unusable entry, never matches */
		for (size_t k = 0; k < l; k++) {
			nd[i][nl[i]++] = (unsigned char)inst_markers[i].marker[k];
			if (inst_markers[i].wide) nd[i][nl[i]++] = 0;
		}
		if (nl[i] > keep) keep = nl[i];
	}
	keep = keep ? keep - 1 : 0;
	if (!(buf = malloc(CHUNK + keep))) return INST_EXE;

	rewind(f);
	for (;;) {
		size_t n = fread(buf + have, 1, CHUNK, f), total = have + n, k;
		if (n == 0) break;
		for (size_t i = 0; i < NMARK; i++) {
			long long off;
			if (first[i] >= 0 || nl[i] == 0) continue;
			off = find_mem(buf, total, nd[i], nl[i]);
			if (off >= 0) first[i] = base + off;
		}
		k = total < keep ? total : keep;
		memmove(buf, buf + total - k, k);
		base += (long long)(total - k);
		have = k;
	}
	free(buf);

	/* the earliest strong marker wins; weak ones (generic archive signatures) only if there is none */
	for (int pass = 0; pass < 2 && best == NMARK; pass++)
		for (size_t i = 0; i < NMARK; i++)
			if (first[i] >= 0 && inst_markers[i].weak == pass && (best == NMARK || first[i] < first[best])) best = i;
	return best == NMARK ? INST_EXE : inst_markers[best].type;
}

InstType inst_detect(const char *path, const char *hint)
{
	unsigned char h[16];
	size_t n;
	InstType t = INST_UNKNOWN;
	FILE *f = fopen(path, "rb");

	if (!f) return INST_UNKNOWN;
	n = fread(h, 1, sizeof h, f);

	if (n >= 8 && memcmp(h, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) == 0)
		t = INST_MSI;
	else if (n >= 4 && h[0] == 'P' && h[1] == 'K' &&
	         ((h[2] == 3 && h[3] == 4) || (h[2] == 5 && h[3] == 6) || (h[2] == 7 && h[3] == 8))) {
		if (ext_is(hint, ".appx") || ext_is(hint, ".msix") ||
		    ext_is(hint, ".appxbundle") || ext_is(hint, ".msixbundle")) t = INST_APPX;
		else if (ext_is(hint, ".nupkg")) t = INST_NUPKG;
		else t = INST_ZIP;
	}
	else if (n >= 6 && memcmp(h, "\x37\x7A\xBC\xAF\x27\x1C", 6) == 0) t = INST_7Z;
	else if (n >= 6 && memcmp(h, "\xFD\x37\x7A\x58\x5A\x00", 6) == 0) t = INST_XZ;
	else if (n >= 3 && memcmp(h, "BZh", 3) == 0)                      t = INST_BZIP2;
	else if (n >= 2 && h[0] == 0x1F && h[1] == 0x8B)                  t = INST_GZIP;
	else if (n >= 2 && h[0] == 'M' && h[1] == 'Z')                    t = scan_pe(f);
	else {
		unsigned char u[5];
		if (fseek(f, 257, SEEK_SET) == 0 && fread(u, 1, 5, f) == 5 && memcmp(u, "ustar", 5) == 0)
			t = INST_TAR;
		else if (ext_is(hint, ".msi")) t = INST_MSI;
	}
	fclose(f);
	return t;
}

static const struct { const char *name; InstType type; } names[] = {
	{ "msi", INST_MSI }, { "nsis", INST_NSIS }, { "inno", INST_INNO }, { "burn", INST_BURN },
	{ "installshield", INST_INSTALLSHIELD }, { "squirrel", INST_SQUIRREL }, { "7zip", INST_7ZIP }, { "exe", INST_EXE },
	{ "zip", INST_ZIP }, { "nupkg", INST_NUPKG }, { "appx", INST_APPX }, { "7z", INST_7Z },
	{ "tar", INST_TAR }, { "gzip", INST_GZIP }, { "bzip2", INST_BZIP2 }, { "xz", INST_XZ }, { "sfx", INST_SFX },
};

InstType inst_from_name(const char *s)
{
	if (!s || !*s) return INST_UNKNOWN;
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
		const char *a = s, *b = names[i].name;
		while (*a && *b && tolower((unsigned char)*a) == *b) a++, b++;
		if (!*a && !*b) return names[i].type;
	}
	return INST_UNKNOWN;
}

const char *inst_name(InstType t)
{
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
		if (names[i].type == t) return names[i].name;
	return "unknown";
}

const char *inst_quiet_args(InstType t, int uninstall)
{
	for (size_t i = 0; i < sizeof inst_args / sizeof inst_args[0]; i++)
		if (inst_args[i].type == t)
			return uninstall ? inst_args[i].uninstall : inst_args[i].install;
	return NULL;
}
