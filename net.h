/* net.h - libcurl helpers: GET/HEAD, downloads, [latest] URL resolution */
#ifndef NET_H
#define NET_H

#include <stddef.h>

#define NET_LATEST "[latest]"

enum { RES_GITHUB, RES_CHOCO, RES_SCRAPE };

typedef struct {
	const char *match;   /* substring of the package URL that selects this resolver */
	int         kind;    /* RES_* */
	const char *page;    /* RES_SCRAPE: page to fetch */
	const char *prefix;  /* RES_SCRAPE: text right before the version */
	const char *suffix;  /* RES_SCRAPE: text right after the version */
} NetResolver;

typedef struct {
	char url[2048];      /* URL with [latest] resolved */
	char tag[128];       /* upstream tag, e.g. v3.9.1 */
	char version[128];   /* tag without leading 'v', e.g. 3.9.1 */
} NetResolved;

int  net_init(void);
void net_cleanup(void);

/* body of url into a malloc'd, NUL-terminated buffer (free() it) */
int  net_get(const char *url, char **out, size_t *len);
/* 1 if url answers 2xx/3xx (ranged GET, no body transferred), else 0 */
int  net_exists(const char *url);
/* url -> dest via dest.part, retries, size check. progress: 1 prints a line */
int  net_download(const char *url, const char *dest, int progress);
/* resolve [latest] in url using the resolvers table in config.h */
int  net_resolve(const char *url, NetResolved *out);

#endif
