/* net.c - libcurl helpers: GET/HEAD, downloads, [latest] URL resolution.
 * Only net_sleep() touches a Windows API, so this also builds on Linux for testing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "net.h"
#include "config.h"

typedef struct { char *p; size_t n; } Buf;

static void net_sleep(int ms)
{
#ifdef _WIN32
	Sleep(ms);
#else
	usleep((useconds_t)ms * 1000);
#endif
}

int net_init(void)     { return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -1; }
void net_cleanup(void) { curl_global_cleanup(); }

static CURL *net_easy(const char *url, int follow)
{
	CURL *c = curl_easy_init();
	if (!c) return NULL;
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_USERAGENT, NET_USER_AGENT);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, (long)NET_CONNECT_TIMEOUT);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)NET_TIMEOUT);
	if (NET_STALL_SECS > 0) {
		curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
		curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, (long)NET_STALL_SECS);
	}
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, follow ? 1L : 0L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, (long)NET_MAX_REDIRS);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_PROXY, NET_PROXY);
	curl_easy_setopt(c, CURLOPT_CAINFO, NET_CAINFO);
#ifdef CURLSSLOPT_NATIVE_CA
	if (NET_NATIVE_CA)
		curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
	return c;
}

/* local failures (disk, size cap) are not worth retrying */
static int net_retryable(CURLcode rc, long code)
{
	if (rc == CURLE_HTTP_RETURNED_ERROR)
		return code == 408 || code == 429 || code >= 500;
	return rc != CURLE_WRITE_ERROR && rc != CURLE_ABORTED_BY_CALLBACK;
}

/* ---- GET into memory ---- */

static size_t buf_write(char *d, size_t s, size_t m, void *u)
{
	Buf *b = u;
	size_t n = s * m;
	char *q;
	if (b->n + n > (size_t)NET_MAX_BODY) return 0;
	if (!(q = realloc(b->p, b->n + n + 1))) return 0;
	b->p = q;
	memcpy(b->p + b->n, d, n);
	b->n += n;
	b->p[b->n] = 0;
	return n;
}

int net_get(const char *url, char **out, size_t *len)
{
	for (int t = 0; t <= NET_RETRIES; t++) {
		Buf b = { NULL, 0 };
		long code = 0;
		CURL *c = net_easy(url, 1);
		if (!c) return -1;
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, buf_write);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
		curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
		CURLcode rc = curl_easy_perform(c);
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
		curl_easy_cleanup(c);
		if (rc == CURLE_OK) {
			if (!b.p && !(b.p = calloc(1, 1))) return -1;
			*out = b.p;
			if (len) *len = b.n;
			return 0;
		}
		free(b.p);
		fprintf(stderr, "net: GET %s: %s\n", url, curl_easy_strerror(rc));
		if (t == NET_RETRIES || !net_retryable(rc, code)) break;
		net_sleep(NET_RETRY_DELAY_MS);
	}
	return -1;
}

/* ---- existence check: ranged GET, abort on the first body byte ---- */

static size_t drop_write(char *d, size_t s, size_t m, void *u)
{
	(void)d; (void)s; (void)m; (void)u;
	return 0;
}

int net_exists(const char *url)
{
	long code = 0;
	CURL *c = net_easy(url, 1);
	if (!c) return 0;
	curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, drop_write);
	CURLcode rc = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	if (rc != CURLE_OK && rc != CURLE_WRITE_ERROR) return 0;
	return code >= 200 && code < 400;
}

/* ---- download to file ---- */

static size_t file_write(char *d, size_t s, size_t m, void *u)
{
	return fwrite(d, s, m, (FILE *)u);
}

static int show_progress(void *u, curl_off_t tot, curl_off_t now, curl_off_t ut, curl_off_t un)
{
	(void)u; (void)ut; (void)un;
	if (now == 0) return 0;
	if (tot > 0)
		fprintf(stderr, "\r  %3d%%  %.2f / %.2f MB", (int)(now * 100 / tot),
		        now / 1048576.0, tot / 1048576.0);
	else
		fprintf(stderr, "\r  %.2f MB", now / 1048576.0);
	return 0;
}

int net_download(const char *url, const char *dest, int progress)
{
	char part[4096];
	if (snprintf(part, sizeof part, "%s.part", dest) >= (int)sizeof part) return -1;

	for (int t = 0; t <= NET_RETRIES; t++) {
		FILE *f = fopen(part, "wb");
		if (!f) { fprintf(stderr, "net: cannot write %s\n", part); return -1; }
		CURL *c = net_easy(url, 1);
		if (!c) { fclose(f); remove(part); return -1; }
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, file_write);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
		curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
		if (progress) {
			curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, show_progress);
			curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		}
		curl_off_t got = 0;
		long code = 0;
		CURLcode rc = curl_easy_perform(c);
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
		curl_easy_getinfo(c, CURLINFO_SIZE_DOWNLOAD_T, &got);
		curl_easy_cleanup(c);
		if (fclose(f) != 0 && rc == CURLE_OK) rc = CURLE_WRITE_ERROR;
		if (progress) fputc('\n', stderr);

		if (rc == CURLE_OK && got > 0) {
			remove(dest);  /* rename() fails on Windows if dest exists */
			if (rename(part, dest) == 0) return 0;
			fprintf(stderr, "net: cannot rename %s to %s\n", part, dest);
			remove(part);
			return -1;
		}
		remove(part);
		if (rc == CURLE_OK)
			fprintf(stderr, "net: %s: empty response\n", url);
		else
			fprintf(stderr, "net: %s: %s\n", url, curl_easy_strerror(rc));
		if (t == NET_RETRIES || (rc != CURLE_OK && !net_retryable(rc, code))) break;
		net_sleep(NET_RETRY_DELAY_MS);
	}
	return -1;
}

/* ---- [latest] resolution ---- */

static int str_replace_all(const char *s, const char *from, const char *to, char *out, size_t cap)
{
	size_t fl = strlen(from), tl = strlen(to), n = 0;
	while (*s) {
		if (strncmp(s, from, fl) == 0) {
			if (n + tl >= cap) return -1;
			memcpy(out + n, to, tl);
			n += tl;
			s += fl;
		} else {
			if (n + 1 >= cap) return -1;
			out[n++] = *s++;
		}
	}
	out[n] = 0;
	return 0;
}

/* first [latest] -> tag, any later one -> version (for names like pwsh-7.5.0.msi) */
static int url_tag_then_version(const char *url, const char *tag, const char *ver, char *out, size_t cap)
{
	const char *p = strstr(url, NET_LATEST);
	size_t hl, tl;
	if (!p) return -1;
	hl = (size_t)(p - url);
	tl = strlen(tag);
	if (hl + tl + 1 > cap) return -1;
	memcpy(out, url, hl);
	memcpy(out + hl, tag, tl);
	return str_replace_all(p + strlen(NET_LATEST), NET_LATEST, ver, out + hl + tl, cap - hl - tl);
}

static int fill(NetResolved *r, const char *url, const char *tag, const char *ver)
{
	if (snprintf(r->url, sizeof r->url, "%s", url) >= (int)sizeof r->url) return -1;
	snprintf(r->tag, sizeof r->tag, "%s", tag);
	snprintf(r->version, sizeof r->version, "%s", ver);
	return 0;
}

static int gh_repo(const char *url, char *owner, char *repo, size_t cap)
{
	const char *p = strstr(url, "github.com/");
	const char *s1, *e;
	size_t ol, rl;
	if (!p) p = strstr(url, "githubusercontent.com/");
	if (!p) return -1;
	p = strchr(p, '/') + 1;
	if (!(s1 = strchr(p, '/'))) return -1;
	e = s1 + 1;
	while (*e && *e != '/' && *e != '?' && *e != '#') e++;
	ol = (size_t)(s1 - p);
	rl = (size_t)(e - (s1 + 1));
	if (rl > 4 && strncmp(s1 + 1 + rl - 4, ".git", 4) == 0) rl -= 4;
	if (!ol || !rl || ol >= cap || rl >= cap) return -1;
	memcpy(owner, p, ol);       owner[ol] = 0;
	memcpy(repo, s1 + 1, rl);   repo[rl] = 0;
	return 0;
}

/* /releases/latest answers with a redirect to /releases/tag/<tag>; read it, don't follow it */
static int gh_latest_tag(const char *owner, const char *repo, char *tag, size_t cap)
{
	char u[512];
	snprintf(u, sizeof u, "https://github.com/%s/%s/releases/latest", owner, repo);
	for (int t = 0; t <= NET_RETRIES; t++) {
		CURL *c = net_easy(u, 0);
		if (!c) return -1;
		curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
		CURLcode rc = curl_easy_perform(c);
		char *loc = NULL;
		if (rc == CURLE_OK) {
			curl_easy_getinfo(c, CURLINFO_REDIRECT_URL, &loc);
			const char *m = loc ? strstr(loc, "/releases/tag/") : NULL;
			if (m) {
				m += strlen("/releases/tag/");
				size_t n = strcspn(m, "/?#");
				if (n && n < cap) {
					memcpy(tag, m, n);
					tag[n] = 0;
					curl_easy_cleanup(c);
					return 0;
				}
			}
			curl_easy_cleanup(c);
			fprintf(stderr, "net: %s/%s has no releases\n", owner, repo);
			return -1;
		}
		curl_easy_cleanup(c);
		fprintf(stderr, "net: %s: %s\n", u, curl_easy_strerror(rc));
		if (t < NET_RETRIES) net_sleep(NET_RETRY_DELAY_MS);
	}
	return -1;
}

static int res_github(const char *url, NetResolved *r)
{
	char owner[128], repo[128], tag[128], c1[2048], c2[2048];
	const char *ver;

	if (gh_repo(url, owner, repo, sizeof owner) != 0) {
		fprintf(stderr, "net: not a GitHub repository URL: %s\n", url);
		return -1;
	}
	if (gh_latest_tag(owner, repo, tag, sizeof tag) != 0) return -1;
	ver = tag[0] == 'v' ? tag + 1 : tag;

	if (strstr(url, "raw.githubusercontent.com/")) {
		if (str_replace_all(url, NET_LATEST, tag, c1, sizeof c1) != 0) return -1;
		return fill(r, c1, tag, ver);
	}
	if (str_replace_all(url, NET_LATEST, tag, c1, sizeof c1) == 0 && net_exists(c1))
		return fill(r, c1, tag, ver);
	if (strcmp(tag, ver) != 0 &&
	    url_tag_then_version(url, tag, ver, c2, sizeof c2) == 0 && net_exists(c2))
		return fill(r, c2, tag, ver);
	fprintf(stderr, "net: no release asset found for tag %s: %s\n", tag, url);
	return -1;
}

static int res_choco(const char *url, NetResolved *r)
{
	const char *p = strstr(url, "/package/");
	char id[128], q[512], out[2048], *body = NULL, *v, *e;
	size_t n;

	if (!p) return -1;
	p += strlen("/package/");
	n = strcspn(p, "/");
	if (!n || n >= sizeof id) return -1;
	memcpy(id, p, n);
	id[n] = 0;
	if (snprintf(q, sizeof q, "%s%s%s", NET_CHOCO_PRE, id, NET_CHOCO_POST) >= (int)sizeof q) return -1;
	if (net_get(q, &body, NULL) != 0) return -1;
	v = strstr(body, ":Version>");   /* matches <d:Version>, not <d:NormalizedVersion> */
	if (!v || !(e = strchr(v += strlen(":Version>"), '<')) || e == v || e - v >= 128) {
		fprintf(stderr, "net: no Chocolatey version for %s\n", id);
		free(body);
		return -1;
	}
	*e = 0;
	if (str_replace_all(url, NET_LATEST, v, out, sizeof out) != 0) { free(body); return -1; }
	n = fill(r, out, v, v);
	free(body);
	return (int)n;
}

static int scrape_ok(const char *s, size_t n)
{
	if (!n || n >= 32) return 0;
	for (size_t i = 0; i < n; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'z') ||
		      (s[i] >= 'A' && s[i] <= 'Z') || s[i] == '.' || s[i] == '-' || s[i] == '_'))
			return 0;
	return 1;
}

static int res_scrape(const char *url, const NetResolver *rs, NetResolved *r)
{
	char *page = NULL, ver[32], out[2048];
	const char *p, *q, *e;
	size_t n;
	int ok = -1;

	if (!rs->page || !rs->prefix || !rs->suffix) return -1;
	if (net_get(rs->page, &page, NULL) != 0) return -1;
	for (p = page; (p = strstr(p, rs->prefix)); p++) {
		q = p + strlen(rs->prefix);
		if (!(e = strstr(q, rs->suffix))) break;
		n = (size_t)(e - q);
		if (!scrape_ok(q, n)) continue;
		memcpy(ver, q, n);
		ver[n] = 0;
		if (str_replace_all(url, NET_LATEST, ver, out, sizeof out) == 0)
			ok = fill(r, out, ver, ver);
		break;
	}
	if (ok != 0) fprintf(stderr, "net: version not found on %s\n", rs->page);
	free(page);
	return ok;
}

int net_resolve(const char *url, NetResolved *r)
{
	memset(r, 0, sizeof *r);
	if (!strstr(url, NET_LATEST))
		return fill(r, url, "unknown", "unknown");
	for (size_t i = 0; i < sizeof resolvers / sizeof resolvers[0]; i++) {
		if (!strstr(url, resolvers[i].match)) continue;
		switch (resolvers[i].kind) {
		case RES_GITHUB: return res_github(url, r);
		case RES_CHOCO:  return res_choco(url, r);
		case RES_SCRAPE: return res_scrape(url, &resolvers[i], r);
		}
	}
	fprintf(stderr, "net: no resolver for %s\n", url);
	return -1;
}
