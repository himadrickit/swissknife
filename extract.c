/* extract.c - run programs without a shell and unpack archives with tar / 7-Zip.
 * Builds with MinGW; the POSIX branches exist so it can be tested on Linux. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#endif
#include "extract.h"
#include "config.h"

/* ---- command line quoting (what CommandLineToArgvW / the MSVCRT parser undo) ---- */

static int put(char *o, size_t cap, size_t *n, char c)
{
	if (*n + 1 >= cap) return -1;
	o[(*n)++] = c;
	return 0;
}

static int quote_arg(char *o, size_t cap, size_t *n, const char *a)
{
	if (*a && !strpbrk(a, " \t\n\v\"")) {
		for (; *a; a++) if (put(o, cap, n, *a)) return -1;
		return 0;
	}
	if (put(o, cap, n, '"')) return -1;
	for (;; a++) {
		size_t bs = 0;
		while (*a == '\\') { bs++; a++; }
		if (*a == 0) {                       /* backslashes before the closing quote */
			for (size_t i = 0; i < bs * 2; i++) if (put(o, cap, n, '\\')) return -1;
			break;
		}
		if (*a == '"') {                     /* backslashes before a quote, then the quote itself */
			for (size_t i = 0; i < bs * 2 + 1; i++) if (put(o, cap, n, '\\')) return -1;
			if (put(o, cap, n, '"')) return -1;
		} else {
			for (size_t i = 0; i < bs; i++) if (put(o, cap, n, '\\')) return -1;
			if (put(o, cap, n, *a)) return -1;
		}
	}
	return put(o, cap, n, '"');
}

size_t ex_cmdline(char *o, size_t cap, const char *const argv[])
{
	size_t n = 0;
	if (!argv || !argv[0]) return 0;
	for (size_t i = 0; argv[i]; i++) {
		if (i && put(o, cap, &n, ' ')) return 0;
		if (quote_arg(o, cap, &n, argv[i])) return 0;
	}
	o[n] = 0;
	return n;
}

/* ---- run without a shell ---- */

int ex_run(const char *const argv[])
{
#ifdef _WIN32
	static char cmd[32768];
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	DWORD code = 1;

	if (!ex_cmdline(cmd, sizeof cmd, argv)) return -1;
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return -1;
	WaitForSingleObject(pi.hProcess, INFINITE);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return (int)code;
#else
	int st;
	pid_t p = fork();
	if (p < 0) return -1;
	if (p == 0) { execvp(argv[0], (char *const *)argv); _exit(127); }
	if (waitpid(p, &st, 0) < 0) return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}

/* ---- helpers ---- */

static int is_dir(const char *p)
{
#ifdef _WIN32
	DWORD a = GetFileAttributesA(p);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

int ex_mkdirs(const char *path)
{
	char buf[4096];
	size_t n = strlen(path);
	if (n == 0 || n >= sizeof buf) return -1;
	memcpy(buf, path, n + 1);
	for (size_t i = 1; i <= n; i++) {
		char c = buf[i];
		if (c != '/' && c != '\\' && c != 0) continue;
		buf[i] = 0;
#ifdef _WIN32
		CreateDirectoryA(buf, NULL);          /* intermediate errors (drive roots, existing) don't matter */
#else
		mkdir(buf, 0777);
#endif
		buf[i] = c;
	}
	return is_dir(path) ? 0 : -1;
}

static int find_7z(char *out, size_t cap)
{
	static const char *const names[] = { EX_7Z_NAMES };
#ifdef _WIN32
	char base[MAX_PATH];
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
		DWORD r = SearchPathA(NULL, names[i], NULL, (DWORD)cap, out, NULL);
		if (r > 0 && r < cap) return 1;
	}
	for (size_t d = 0; d < sizeof ex_7z_dirs / sizeof ex_7z_dirs[0]; d++) {
		DWORD r = ExpandEnvironmentStringsA(ex_7z_dirs[d], base, sizeof base);
		if (r == 0 || r > sizeof base) continue;
		for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
			snprintf(out, cap, "%s\\%s", base, names[i]);
			if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return 1;
		}
	}
	return 0;
#else
	const char *path = getenv("PATH");
	char dir[1024];
	if (!path) return 0;
	for (const char *p = path; *p;) {
		size_t l = strcspn(p, ":");
		if (l && l < sizeof dir) {
			memcpy(dir, p, l);
			dir[l] = 0;
			for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
				snprintf(out, cap, "%s/%s", dir, names[i]);
				if (access(out, X_OK) == 0) return 1;
			}
		}
		p += l + (p[l] ? 1 : 0);
	}
	return 0;
#endif
	(void)ex_7z_dirs;
}

/* ---- extraction ---- */

int ex_extract(InstType t, const char *archive, const char *dest)
{
	char exe7[4096], o7[4200];
	int rc, use7z;

	if (ex_mkdirs(dest) != 0) {
		fprintf(stderr, "extract: cannot create %s\n", dest);
		return -1;
	}
	switch (t) {
	case INST_ZIP: case INST_NUPKG: case INST_TAR:
	case INST_GZIP: case INST_BZIP2: case INST_XZ: case INST_7Z: case INST_SFX:
		break;
	default:
		return -1;
	}

	use7z = (t == INST_7Z || t == INST_SFX) && find_7z(exe7, sizeof exe7);
	if (t == INST_SFX && !use7z) {
		fprintf(stderr, "extract: self-extracting archives need 7-Zip, which was not found\n");
		return -1;
	}
	if (t == INST_7Z && !use7z)
		fprintf(stderr, "extract: 7-Zip not found, trying %s\n", EX_TAR);

	if (use7z) {
		const char *a[] = { exe7, "x", archive, o7, EX_7Z_FLAGS, NULL };
		snprintf(o7, sizeof o7, "-o%s", dest);
		rc = ex_run(a);
		if (rc != 0) fprintf(stderr, "extract: 7-Zip failed (%d)\n", rc);
	} else {
		const char *a[] = { EX_TAR, "-xf", archive, "-C", dest, NULL };
		rc = ex_run(a);
		if (rc != 0) fprintf(stderr, "extract: %s failed (%d)\n", EX_TAR, rc);
	}
	return rc == 0 ? 0 : -1;
}
