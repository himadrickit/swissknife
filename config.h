/* config.h - compile-time settings; edit and recompile (suckless style) */

#include "net.h"
#include "inst.h"

/* ---- network ---- */
#define NET_USER_AGENT       "swissknife/2"
#define NET_CONNECT_TIMEOUT  15      /* seconds */
#define NET_TIMEOUT          0       /* whole-transfer limit in seconds, 0 = none */
#define NET_STALL_SECS       30      /* abort if < 1 byte/s for this long, 0 = off */
#define NET_RETRIES          3       /* extra attempts after the first */
#define NET_RETRY_DELAY_MS   2000
#define NET_MAX_REDIRS       10
#define NET_MAX_BODY         (8L * 1024 * 1024)  /* cap for in-memory pages */
#define NET_PROXY            NULL    /* e.g. "http://127.0.0.1:8080" */
#define NET_CAINFO           NULL    /* path to a CA bundle, NULL = libcurl default */
#define NET_NATIVE_CA        1       /* use the Windows cert store when libcurl supports it */

/* Chocolatey: query = PRE + package id + POST, answer must hold <d:Version> */
#define NET_CHOCO_PRE  "https://community.chocolatey.org/api/v2/Packages()?$filter=Id%20eq%20'"
#define NET_CHOCO_POST "'%20and%20IsLatestVersion&$top=1"

/* ---- [latest] resolvers: first entry whose .match is found in the URL wins ----
 * RES_GITHUB  tag from github.com/<owner>/<repo>/releases/latest
 * RES_CHOCO   latest version from the Chocolatey feed
 * RES_SCRAPE  version found on .page between .prefix and .suffix            */
static const NetResolver resolvers[] = {
	/* match                                      kind        page                                   prefix   suffix     */
	{ "community.chocolatey.org/api/v2/package/", RES_CHOCO,  NULL,                                  NULL,    NULL       },
	{ "www.7-zip.org/",                           RES_SCRAPE, "https://www.7-zip.org/download.html", "a/7z",  "-x64.exe" },
	{ "raw.githubusercontent.com/",               RES_GITHUB, NULL,                                  NULL,    NULL       },
	{ "github.com/",                              RES_GITHUB, NULL,                                  NULL,    NULL       },
};

/* ---- installers ---- */
#define INST_QUIET 1   /* run installers quietly by default; --nosilent turns it off per run */

/* exit codes that count as success (3010 / 1641: installed, reboot needed) */
static const unsigned long inst_ok_codes[] = { 0, 3010, 1641 };

/* The whole EXE is searched for these; the marker found nearest the start of the file decides
 * the type (the outer installer comes first, anything it carries comes later). */
static const InstMarker inst_markers[] = {
	/* type              marker                     wide (UTF-16) */
	{ INST_BURN,         ".wixburn",                0 },
	{ INST_NSIS,         "NullsoftInst",            0 },
	{ INST_NSIS,         "Nullsoft Install System", 0 },
	{ INST_INNO,         "Inno Setup Setup Data",   0 },
	{ INST_INNO,         "InnoSetupLdrWindow",      0 },
	{ INST_INSTALLSHIELD,"InstallShield",           0 },
	{ INST_SQUIRREL,     "Squirrel",                1 },
	{ INST_7ZIP,         "7-Zip Installer",         0 },
};

/* quiet switches per type. MSI goes through msiexec; its line is appended after /i "file" */
static const InstArgs inst_args[] = {
	/* type              install                                                uninstall                                       */
	{ INST_MSI,          "/qn /norestart",                                      "/qn /norestart"                                },
	{ INST_NSIS,         "/S",                                                  "/S"                                            },
	{ INST_INNO,         "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-",       "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART"      },
	{ INST_BURN,         "/quiet /norestart",                                   "/uninstall /quiet /norestart"                  },
	{ INST_INSTALLSHIELD,"/s /v\"/qn /norestart\"",                             "/s /x /v\"/qn /norestart\""                    },
	{ INST_SQUIRREL,     "--silent",                                            "--silent"                                      },
	{ INST_7ZIP,         "/S",                                                  "/S"                                            },
	{ INST_EXE,          NULL,                                                  "/S"                                            },
};

/* ---- archives ---- */
#ifndef EX_TAR
#define EX_TAR "tar.exe"      /* bsdtar, ships with Windows 10+: zip, tar, gz, bz2, xz */
#endif
#ifndef EX_7Z_NAMES
#define EX_7Z_NAMES "7z.exe", "7zz.exe"   /* looked up on PATH, then in ex_7z_dirs */
#endif
#ifndef EX_7Z_FLAGS
#define EX_7Z_FLAGS "-y", "-bso0", "-bsp0"   /* yes to all, no banner, no progress */
#endif
static const char *const ex_7z_dirs[] = { "%ProgramFiles%\\7-Zip", "%ProgramFiles(x86)%\\7-Zip" };
