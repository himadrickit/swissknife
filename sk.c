// sk.c - Swissknife with cJSON and multithreaded parallel BITS download, sequential install
// Updated to support ~/knives/knives.json and per-knife folders, installed list at ~/package.json

#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <io.h>
#include "cjson.h"
#include "net.h"
#include "inst.h"
#include "extract.h"
#include "config.h"

#define MAX_KNIFE_NAME 128

HANDLE installed_mutex;
static char KNIVES_FOLDER_PATH[MAX_PATH];
static char KNIVES_CONFIG_PATH[MAX_PATH];
static char INSTALLED_FILE_PATH[MAX_PATH];
static char CACHE_DIR[MAX_PATH];  // ~/.cache/swiss: downloaded files and logs

typedef struct Package {
    char name[128];
    char id[64];
    char version[128];
    char url[1024];
    int silent;       // 1 = run installers quietly (default), 0 = let them show their window
    char type[16];
    char binpath[256];
    char out_path[MAX_PATH];
    char untype[32]; // uninstaller type
    char uninstaller[MAX_PATH];
    int ok;       // download finished and file is at out_path
    int progress; // show a progress line while downloading
} Package;

char* read_file(const char* filepath) {
    FILE* f = fopen(filepath, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char* buffer = malloc((size_t)size + 1);
    if (!buffer) {
        fclose(f);
        return NULL;
    }
    fread(buffer, 1, size, f);
    buffer[size] = 0;
    fclose(f);
    return buffer;
}

int write_file(const char* filepath, const char* data) {
    FILE* f = fopen(filepath, "w");
    if (!f) return -1;
    fwrite(data, 1, strlen(data), f);
    fclose(f);
    return 0;
}

/* ---- knives.json helpers ----
   knives.json format:
   { "knives": [ { "name": "main", "url": "https://..." }, ... ] }
   */
void ensure_knives_folder_and_config() {
    // KNIVES_FOLDER_PATH and KNIVES_CONFIG_PATH are initialized in main()
    CreateDirectoryA(KNIVES_FOLDER_PATH, NULL);

    // ensure knives.json exists
    FILE* f = fopen(KNIVES_CONFIG_PATH, "r");
    if (!f) {
        f = fopen(KNIVES_CONFIG_PATH, "w");
        if (f) {
            fputs("{\"knives\":[]}", f);
            fclose(f);
        }
    } else {
        fclose(f);
    }
}

void init_knives() {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\knives", getenv("USERPROFILE"));
    CreateDirectory(path, NULL);

    snprintf(path, sizeof(path), "%s\\knives\\knives.json", getenv("USERPROFILE"));
    FILE *f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (f) {
            fputs("{\"repos\":[]}", f);
            fclose(f);
        }
    } else {
        fclose(f);
    }
}

// List knives (-Skl)
void list_knives() {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\knives\\knives.json", getenv("USERPROFILE"));

    FILE *f = fopen(path, "r");
    if (!f) {
        printf("No knives.json found.\n");
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    char *data = malloc(size + 1);
    fread(data, 1, size, f);
    data[size] = '\0';
    fclose(f);

    cJSON *json = cJSON_Parse(data);
    free(data);

    if (!json) {
        printf("Invalid knives.json\n");
        return;
    }

    cJSON *repos = cJSON_GetObjectItem(json, "repos");
    if (repos && cJSON_IsArray(repos)) {
        printf("Available knives:\n");
        cJSON *repo;
        cJSON_ArrayForEach(repo, repos) {
            cJSON *name = cJSON_GetObjectItem(repo, "name");
            cJSON *url  = cJSON_GetObjectItem(repo, "url");
            if (name && url)
                printf("- %s: %s\n", name->valuestring, url->valuestring);
        }
    }

    cJSON_Delete(json);
}

void save_knife(const char* name, const char* url) {
    char* data = read_file(KNIVES_CONFIG_PATH);
    cJSON* root = NULL;
    if (data) {
        root = cJSON_Parse(data);
        free(data);
    }
    if (!root) root = cJSON_CreateObject();

    cJSON* knives = cJSON_GetObjectItem(root, "knives");
    if (!knives) {
        knives = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "knives", knives);
    }

    cJSON* knife = NULL;
    cJSON_ArrayForEach(knife, knives) {
        cJSON* nm = cJSON_GetObjectItem(knife, "name");
        if (nm && nm->valuestring && strcmp(nm->valuestring, name) == 0) {
            // replace url
            cJSON_ReplaceItemInObject(knife, "url", cJSON_CreateString(url));
            char* out = cJSON_Print(root);
            write_file(KNIVES_CONFIG_PATH, out);
            free(out);
            cJSON_Delete(root);
            return;
        }
    }

    // add new
    cJSON* new_knife = cJSON_CreateObject();
    cJSON_AddStringToObject(new_knife, "name", name);
    cJSON_AddStringToObject(new_knife, "url", url);
    cJSON_AddItemToArray(knives, new_knife);

    char* out = cJSON_Print(root);
    write_file(KNIVES_CONFIG_PATH, out);
    free(out);
    cJSON_Delete(root);
}

char* get_knife_url(const char* name) {
    char* data = read_file(KNIVES_CONFIG_PATH);
    if (!data) return NULL;
    cJSON* root = cJSON_Parse(data);
    free(data);
    if (!root) return NULL;

    cJSON* knives = cJSON_GetObjectItem(root, "knives");
    if (!knives) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON* knife = NULL;
    char* url_dup = NULL;
    cJSON_ArrayForEach(knife, knives) {
        cJSON* nm = cJSON_GetObjectItem(knife, "name");
        cJSON* u = cJSON_GetObjectItem(knife, "url");
        if (nm && u && nm->valuestring && u->valuestring && strcmp(nm->valuestring, name) == 0) {
            url_dup = _strdup(u->valuestring);
            break;
        }
    }

    cJSON_Delete(root);
    return url_dup;
}

int sync_knife_git_to_folder(const char* knife_name) {
    if (strpbrk(knife_name, "/\\:") || strstr(knife_name, "..")) {
        printf("Invalid knife name: %s\n", knife_name);
        return -1;
    }
    char *url = get_knife_url(knife_name);
    if (!url) {
        printf("Knife '%s' not found in knives.json\n", knife_name);
        return -1;
    }

    char dest[MAX_PATH];
    snprintf(dest, MAX_PATH, "%s\\%s", KNIVES_FOLDER_PATH, knife_name);

    if (url[0] == '-') {
        printf("Refusing a knife URL that looks like an option: %s\n", url);
        free(url);
        return -1;
    }

    int r;
    if (_access(dest, 0) != 0) {
        const char* clone[] = { "git", "clone", "--", url, dest, NULL };
        r = ex_run(clone);
    } else {
        const char* pull[] = { "git", "-C", dest, "pull", "--ff-only", NULL };
        r = ex_run(pull);
    }
    if (r == -1) printf("Could not run git. Is it installed and on PATH?\n");
    free(url);
    return r;
}

/* -Sy with no name: sync every knife listed in knives.json; one failing knife doesn't stop the rest */
static int sync_all_knives(void) {
    char* content = read_file(KNIVES_CONFIG_PATH);
    cJSON* root = content ? cJSON_Parse(content) : NULL;
    free(content);
    const cJSON* list = root ? cJSON_GetObjectItem(root, "knives") : NULL;
    int total = 0, failed = 0;

    if (cJSON_IsArray(list)) {
        for (int i = 0, n = cJSON_GetArraySize(list); i < n; ++i) {
            const cJSON* name = cJSON_GetObjectItem(cJSON_GetArrayItem(list, i), "name");
            if (!cJSON_IsString(name) || !name->valuestring[0]) continue;
            total++;
            printf("Syncing knife '%s'...\n", name->valuestring);
            if (sync_knife_git_to_folder(name->valuestring) == 0)
                printf("Synced knife '%s'\n", name->valuestring);
            else {
                printf("Could not sync knife '%s'\n", name->valuestring);
                failed++;
            }
        }
    }
    cJSON_Delete(root);

    if (total == 0) {
        printf("No knives configured. Add one with: sk -Sr <name> <url>\n");
        return 1;
    }
    printf("%d of %d knives synced.\n", total - failed, total);
    return failed ? 1 : 0;
}

/* ---- parse package json from a given path ---- */
static void copy_str(const cJSON* root, const char* key, char* dst, size_t cap) {
    const cJSON* it = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(it) && it->valuestring) snprintf(dst, cap, "%s", it->valuestring);
}

/* "silent": true/false (or "true"/"false"); missing or anything else means quiet */
static int json_silent(const cJSON* it) {
    if (cJSON_IsFalse(it)) return 0;
    if (cJSON_IsNumber(it)) return it->valuedouble != 0;
    if (cJSON_IsString(it) && it->valuestring) {
        const char* v = it->valuestring;
        if (_stricmp(v, "false") == 0 || _stricmp(v, "no") == 0 || strcmp(v, "0") == 0) return 0;
    }
    return 1;
}

int parse_package_json(const char* filepath, Package* pkg) {
    char* content = read_file(filepath);
    if (!content) return -1;
    cJSON* root = cJSON_Parse(content);
    free(content);
    if (!root) return -2;

    memset(pkg, 0, sizeof(Package));
    copy_str(root, "name", pkg->name, sizeof(pkg->name));
    copy_str(root, "id", pkg->id, sizeof(pkg->id));
    copy_str(root, "version", pkg->version, sizeof(pkg->version));
    copy_str(root, "url", pkg->url, sizeof(pkg->url));
    pkg->silent = json_silent(cJSON_GetObjectItem(root, "silent"));
    copy_str(root, "type", pkg->type, sizeof(pkg->type));
    copy_str(root, "uninstaller", pkg->uninstaller, sizeof(pkg->uninstaller));
    copy_str(root, "untype", pkg->untype, sizeof(pkg->untype));
    copy_str(root, "binpath", pkg->binpath, sizeof(pkg->binpath));
    cJSON_Delete(root);

    if (!pkg->id[0]) {
        memset(pkg, 0, sizeof(Package));
        return -3;
    }

    // downloads live in the cache folder
    snprintf(pkg->out_path, MAX_PATH, "%s\\%s.%s", CACHE_DIR, pkg->id, pkg->type[0] ? pkg->type : "tmp");
    return 0;
}

/* resolve [latest], then download to pkg->out_path; sets pkg->ok on success */
DWORD WINAPI download_thread(LPVOID param) {
    Package* pkg = (Package*)param;
    NetResolved r;

    if (net_resolve(pkg->url, &r) != 0) {
        printf("Could not resolve the download URL for %s\n", pkg->name);
        return 0;
    }
    if (strcmp(r.version, "unknown") != 0)
        snprintf(pkg->version, sizeof(pkg->version), "%s", r.version);

    printf("Downloading %s %s...\n", pkg->name, pkg->version);
    if (net_download(r.url, pkg->out_path, pkg->progress) == 0)
        pkg->ok = 1;
    else
        printf("Download failed for %s\n", pkg->name);
    return 0;
}

/* download every loaded package (id set) in parallel, in batches the wait API allows */
static void download_all(Package* pk, int n) {
    HANDLE h[MAXIMUM_WAIT_OBJECTS];
    int i = 0;
    while (i < n) {
        int m = 0;
        for (; i < n && m < MAXIMUM_WAIT_OBJECTS; ++i) {
            if (!pk[i].id[0]) continue;
            pk[i].progress = (n == 1);
            h[m] = CreateThread(NULL, 0, download_thread, &pk[i], 0, NULL);
            if (h[m]) m++;
        }
        if (m > 0) {
            WaitForMultipleObjects(m, h, TRUE, INFINITE);
            for (int k = 0; k < m; ++k) CloseHandle(h[k]);
        }
    }
}

/* ---- installed packages helpers (~/package.json) ---- */
void add_or_update_installed_package(Package* pkg) {
    WaitForSingleObject(installed_mutex, INFINITE);

    char* content = read_file(INSTALLED_FILE_PATH);
    cJSON* root = NULL;
    if (content) {
        root = cJSON_Parse(content);
        free(content);
    }
    if (!root) root = cJSON_CreateArray();

    cJSON* updated = cJSON_CreateObject();
    cJSON_AddStringToObject(updated, "name", pkg->name);
    cJSON_AddStringToObject(updated, "id", pkg->id);
    cJSON_AddStringToObject(updated, "version", pkg->version);

    int len = cJSON_GetArraySize(root);
    for (int i = 0; i < len; ++i) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        cJSON* id = cJSON_GetObjectItem(item, "id");
        if (id && id->valuestring && strcmp(id->valuestring, pkg->id) == 0) {
            cJSON_DeleteItemFromArray(root, i);
            break;
        }
    }

    cJSON_AddItemToArray(root, updated);
    char* output = cJSON_Print(root);
    write_file(INSTALLED_FILE_PATH, output);
    free(output);
    cJSON_Delete(root);

    ReleaseMutex(installed_mutex);
}

void remove_installed_package(const char* id) {
    WaitForSingleObject(installed_mutex, INFINITE);
    char* content = read_file(INSTALLED_FILE_PATH);
    if (!content) {
        ReleaseMutex(installed_mutex);
        return;
    }

    cJSON* root = cJSON_Parse(content);
    free(content);
    if (!root || !cJSON_IsArray(root)) {
        ReleaseMutex(installed_mutex);
        return;
    }

    int len = cJSON_GetArraySize(root);
    for (int i = 0; i < len; ++i) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        cJSON* id_field = cJSON_GetObjectItem(item, "id");
        if (id_field && id_field->valuestring && strcmp(id_field->valuestring, id) == 0) {
            cJSON_DeleteItemFromArray(root, i);
            break;
        }
    }

    char* updated = cJSON_Print(root);
    write_file(INSTALLED_FILE_PATH, updated);
    free(updated);
    cJSON_Delete(root);
    ReleaseMutex(installed_mutex);
}

/* ---- start menu helper (unchanged) ---- */
void create_start_menu_shortcut(const char *appName, const char *targetPath, const char *workingDir, const char *iconPath) {
    char shortcutPath[MAX_PATH];
    char startMenuPath[MAX_PATH];

    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROGRAMS, NULL, 0, startMenuPath))) {
        snprintf(shortcutPath, MAX_PATH, "%s\\%s.lnk", startMenuPath, appName);

        IShellLinkA* psl;
        HRESULT hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                &IID_IShellLinkA, (LPVOID*)&psl);

        if (SUCCEEDED(hr)) {
            psl->lpVtbl->SetPath(psl, targetPath);
            if (workingDir) psl->lpVtbl->SetWorkingDirectory(psl, workingDir);
            if (iconPath)   psl->lpVtbl->SetIconLocation(psl, iconPath, 0);

            IPersistFile* ppf;
            hr = psl->lpVtbl->QueryInterface(psl, &IID_IPersistFile, (LPVOID*)&ppf);
            if (SUCCEEDED(hr)) {
                WCHAR wsz[MAX_PATH];
                MultiByteToWideChar(CP_ACP, 0, shortcutPath, -1, wsz, MAX_PATH);
                ppf->lpVtbl->Save(ppf, wsz, TRUE);
                ppf->lpVtbl->Release(ppf);
                printf("Shortcut created: %s\n", shortcutPath);
            }
            psl->lpVtbl->Release(psl);
        }
    }
}

static int file_exists(const char* p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

/* is dir already one of the ;-separated entries of list (case-insensitive, ignoring a trailing \) */
static int path_has(const char* list, const char* dir) {
    size_t dl = strlen(dir);
    while (dl > 1 && (dir[dl - 1] == '\\' || dir[dl - 1] == '/')) dl--;
    for (const char* p = list; *p;) {
        size_t l = strcspn(p, ";"), e = l;
        while (e > 1 && (p[e - 1] == '\\' || p[e - 1] == '/')) e--;
        if (e == dl && _strnicmp(p, dir, dl) == 0) return 1;
        p += l + (p[l] ? 1 : 0);
    }
    return 0;
}

/* append dir to the user PATH in the registry. Never touches PATH if it can't be read in full. */
static void path_add(const char* dir) {
    HKEY k;
    DWORD type = REG_EXPAND_SZ, sz = 0;
    size_t dl = strlen(dir);
    char* buf;
    LONG r;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Environment", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    r = RegQueryValueExA(k, "Path", NULL, &type, NULL, &sz);
    if (r == ERROR_SUCCESS && type != REG_SZ && type != REG_EXPAND_SZ) r = ERROR_INVALID_DATA;
    if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND) {
        printf("Could not read the user PATH; leaving it alone.\n");
        RegCloseKey(k);
        return;
    }
    if (r == ERROR_FILE_NOT_FOUND) { type = REG_EXPAND_SZ; sz = 0; }

    buf = (char*)malloc((size_t)sz + dl + 3);
    if (!buf) { RegCloseKey(k); return; }
    buf[0] = 0;
    if (r == ERROR_SUCCESS) {
        DWORD got = sz;
        if (RegQueryValueExA(k, "Path", NULL, NULL, (LPBYTE)buf, &got) != ERROR_SUCCESS) {
            printf("Could not read the user PATH; leaving it alone.\n");
            free(buf);
            RegCloseKey(k);
            return;
        }
        buf[sz] = 0;
    }

    if (!path_has(buf, dir)) {
        size_t n = strlen(buf);
        if (n + dl + 2 > 32766) {
            printf("PATH would exceed the Windows limit; not adding %s\n", dir);
        } else {
            if (n && buf[n - 1] != ';') buf[n++] = ';';
            memcpy(buf + n, dir, dl + 1);
            if (RegSetValueExA(k, "Path", 0, type, (const BYTE*)buf, (DWORD)strlen(buf) + 1) == ERROR_SUCCESS) {
                SendMessageTimeoutA(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)"Environment", SMTO_ABORTIFHUNG, 5000, NULL);
                printf("Added %s to PATH (new terminals pick it up).\n", dir);
            }
        }
    }
    free(buf);
    RegCloseKey(k);
}

/* ---- quiet installs: run, wait, judge by exit code ---- */
static int g_quiet = INST_QUIET;

static int exit_ok(DWORD code) {
    for (size_t i = 0; i < sizeof(inst_ok_codes) / sizeof(inst_ok_codes[0]); ++i)
        if (code == inst_ok_codes[i]) return 1;
    return 0;
}

/* start file with args, wait for it; 0 = ran (exit code in *code), -1 = could not start */
static int run_wait(const char* file, const char* args, int hidden, DWORD* code) {
    SHELLEXECUTEINFOA sei;
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = "open";
    sei.lpFile = file;
    sei.lpParameters = (args && *args) ? args : NULL;
    sei.nShow = hidden ? SW_HIDE : SW_SHOWNORMAL;
    if (!ShellExecuteExA(&sei) || !sei.hProcess) return -1;
    WaitForSingleObject(sei.hProcess, INFINITE);
    *code = 0;
    GetExitCodeProcess(sei.hProcess, code);
    CloseHandle(sei.hProcess);
    return 0;
}

/* %USERPROFILE%\swiss\logs\<id>-<tag>.log */
static void log_path(const char* id, const char* tag, char* out, size_t cap) {
    char dir[MAX_PATH], safe[128];
    size_t n = 0;
    for (; id[n] && n < sizeof(safe) - 1; ++n)
        safe[n] = (isalnum((unsigned char)id[n]) || id[n] == '-' || id[n] == '_') ? id[n] : '_';
    safe[n] = 0;
    snprintf(dir, sizeof(dir), "%s\\logs", CACHE_DIR);
    ex_mkdirs(dir);
    snprintf(out, cap, "%s\\%s-%s.log", dir, safe, tag);
}

/* install an MSI or an EXE installer of detected type t; 1 = success.
 * Quiet switches come from the table in config.h only; -V or "silent": false skips them. */
static int install_binary(Package* p, InstType t) {
    char args[2 * MAX_PATH + 256], log[MAX_PATH] = "";
    const char* quiet = (g_quiet && p->silent) ? inst_quiet_args(t, 0) : NULL;
    DWORD code = 0;
    int started;

    if (t == INST_MSI) {
        log_path(p->id, "msi", log, sizeof(log));
        snprintf(args, sizeof(args), "/i \"%s\" %s /L*v \"%s\"", p->out_path, quiet ? quiet : "", log);
        if (!g_quiet) printf("Running: msiexec.exe %s\n", args);
        started = run_wait("msiexec.exe", args, quiet != NULL, &code) == 0;
    } else {
        if (g_quiet && p->silent && !quiet)
            printf("Unknown installer type, so it can't run silently; its window will show.\n");
        if (!g_quiet) printf("Running: %s %s\n", p->out_path, quiet ? quiet : "");
        started = run_wait(p->out_path, quiet, quiet != NULL, &code) == 0;
    }

    if (!started) {
        printf("Could not start the installer for %s\n", p->name);
        return 0;
    }
    if (exit_ok(code)) {
        if (code == 3010 || code == 1641) printf("A reboot is needed to finish installing %s.\n", p->name);
        return 1;
    }
    printf("Installer exited with code %lu.\n", (unsigned long)code);
    if (code == 1925 || code == 740) printf("It needs administrator rights: run from an elevated shell.\n");
    if (log[0]) printf("MSI log: %s\n", log);
    return 0;
}

/* ---- install/unpack logic (mostly unchanged) ---- */
void wait_and_install_packages(int count, Package packages[]) {

    char userProfile[MAX_PATH];
    SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, userProfile);

    for (int i = 0; i < count; ++i) {
        if (!packages[i].ok) continue; // not loaded or download failed

        // Work out what the file really is, from its content
        InstType t = inst_detect(packages[i].out_path, packages[i].url);

        char extract_path[MAX_PATH];
        snprintf(extract_path, MAX_PATH, "%s\\%s\\%s", userProfile, DIR_EXTRACT, packages[i].name);

        if (t == INST_ZIP || t == INST_NUPKG || t >= INST_7Z) {
            printf("Extracting %s (%s)...\n", packages[i].name, inst_name(t));
            if (ex_extract(t, packages[i].out_path, extract_path) != 0) {
                printf("Extraction failed for %s\n", packages[i].name);
                continue;
            }
            add_or_update_installed_package(&packages[i]);
            printf("%s extracted to %s\n", packages[i].name, extract_path);

            if (t != INST_NUPKG) {  // a plain archive: expose its folder, add a Start Menu entry if <name>.exe exists
                char exePath[MAX_PATH];
                path_add(extract_path);
                snprintf(exePath, MAX_PATH, "%s\\%s.exe", extract_path, packages[i].name);
                if (file_exists(exePath))
                    create_start_menu_shortcut(packages[i].name, exePath, extract_path, exePath);
            }
        }

        else if (t == INST_APPX) {
            // paths and names travel in environment variables, so nothing is ever parsed as PowerShell code
            printf("Installing AppX package %s...\n", packages[i].name);
            SetEnvironmentVariableA("SK_PKG_PATH", packages[i].out_path);
            const char* add[] = { "powershell", "-NoProfile", "-WindowStyle", "Hidden", "-Command",
                "Add-AppxPackage -Path $env:SK_PKG_PATH -ForceApplicationShutdown -ErrorAction Stop", NULL };
            if (ex_run(add) != 0) {
                printf("AppX installation failed for %s\n", packages[i].name);
                continue;
            }
            add_or_update_installed_package(&packages[i]);
            printf("%s installed successfully as AppX.\n", packages[i].name);

            // package identity = name, executable = <name>.exe, as before
            SetEnvironmentVariableA("SK_PKG_NAME", packages[i].name);
            FILE* fp = _popen("powershell -NoProfile -Command \"$p = Get-AppxPackage -Name $env:SK_PKG_NAME; "
                              "if ($p) { $e = Join-Path $p.InstallLocation ($env:SK_PKG_NAME + '.exe'); "
                              "if (Test-Path $e) { Write-Output $e } }\"", "r");
            if (fp) {
                char exePath[MAX_PATH];
                if (fgets(exePath, MAX_PATH, fp)) {
                    char exeDir[MAX_PATH];
                    char* slash;
                    exePath[strcspn(exePath, "\r\n")] = 0;
                    create_start_menu_shortcut(packages[i].name, exePath, NULL, exePath);
                    snprintf(exeDir, MAX_PATH, "%s", exePath);
                    if ((slash = strrchr(exeDir, '\\'))) { *slash = 0; path_add(exeDir); }
                }
                _pclose(fp);
            }
        }

        else if (t == INST_UNKNOWN) {
            printf("%s: the download is neither an installer nor a known archive, skipping.\n", packages[i].name);
            continue;
        }

        else {
            printf("Installing %s (%s)...\n", packages[i].name, inst_name(t));
            if (install_binary(&packages[i], t)) {
                add_or_update_installed_package(&packages[i]);
                printf("%s installed successfully.\n", packages[i].name);
            } else {
                printf("Installation failed for %s\n", packages[i].name);
                continue;
            }
        }

        // --- BINPATH SUPPORT ---
        if (packages[i].binpath[0]) {
            char fullPath[MAX_PATH];
            snprintf(fullPath, sizeof(fullPath), "%s\\%s", userProfile, packages[i].binpath);
            path_add(fullPath);
        }
    }
}



/* ---- helper: build package file path from "knife/id" or "id" (falls back to "main") ---- */
void build_package_filepath(const char* spec, char* out_path, size_t out_len) {
    // spec: either "knife/id" or "id"
    const char* slash = strchr(spec, '/');
    char knife[MAX_KNIFE_NAME] = {0};
    char id[256] = {0};
    if (slash) {
        size_t klen = (size_t)(slash - spec);
        if (klen >= sizeof(knife)) klen = sizeof(knife)-1;
        strncpy(knife, spec, klen);
        knife[klen] = '\0';
        strncpy(id, slash + 1, sizeof(id)-1);
    } else {
        // default knife 'main'
        strcpy(knife, "main");
        strncpy(id, spec, sizeof(id)-1);
    }

    snprintf(out_path, out_len, "%s\\%s\\%s.json", KNIVES_FOLDER_PATH, knife, id);
}

/* ---- find a package file: "knife/id" as given; bare "id" in main first, then any knife ---- */
static int locate_package(const char* spec, char* out, size_t cap) {
    WIN32_FIND_DATAA d;
    char pat[MAX_PATH];

    build_package_filepath(spec, out, cap);
    if (file_exists(out)) return 1;
    if (strchr(spec, '/')) return 0;

    // FindFirstFile only globs the last path component, so walk the knife folders ourselves
    snprintf(pat, sizeof(pat), "%s\\*", KNIVES_FOLDER_PATH);
    HANDLE h = FindFirstFileA(pat, &d);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || d.cFileName[0] == '.') continue;
        snprintf(out, cap, "%s\\%s\\%s.json", KNIVES_FOLDER_PATH, d.cFileName, spec);
        if (file_exists(out)) { FindClose(h); return 1; }
    } while (FindNextFileA(h, &d));
    FindClose(h);
    return 0;
}

/* load specs into a zeroed array; entries that fail stay zeroed (id empty) and are skipped */
static void load_packages(Package* pk, int n, char* const specs[]) {
    for (int i = 0; i < n; ++i) {
        char path[MAX_PATH];
        if (!locate_package(specs[i], path, sizeof(path)) ||
            parse_package_json(path, &pk[i]) != 0) {
            printf("Package not found: %s\n", specs[i]);
            memset(&pk[i], 0, sizeof(Package));
        }
    }
}

void download_and_install_packages(int count, char* package_names[]) {
    Package* packages = calloc((size_t)count, sizeof(Package));
    if (!packages) return;
    load_packages(packages, count, package_names);
    download_all(packages, count);
    wait_and_install_packages(count, packages);
    free(packages);
}

/* ---- list packages across knives ---- */
void list_packages() {
    WIN32_FIND_DATAA kd, fd;
    char pat[MAX_PATH];
    int n = 0;

    snprintf(pat, MAX_PATH, "%s\\*", KNIVES_FOLDER_PATH);
    HANDLE hk = FindFirstFileA(pat, &kd);
    if (hk == INVALID_HANDLE_VALUE) {
        printf("No packages found.\n");
        return;
    }
    do {
        if (!(kd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || kd.cFileName[0] == '.') continue;
        snprintf(pat, MAX_PATH, "%s\\%s\\*.json", KNIVES_FOLDER_PATH, kd.cFileName);
        HANDLE hf = FindFirstFileA(pat, &fd);
        if (hf == INVALID_HANDLE_VALUE) continue;
        do {
            char path[MAX_PATH];
            snprintf(path, MAX_PATH, "%s\\%s\\%s", KNIVES_FOLDER_PATH, kd.cFileName, fd.cFileName);
            char* content = read_file(path);
            if (!content) continue;
            cJSON* root = cJSON_Parse(content);
            free(content);
            if (!root) continue;
            const cJSON* name = cJSON_GetObjectItem(root, "name");
            const cJSON* id = cJSON_GetObjectItem(root, "id");
            if (cJSON_IsString(name) && cJSON_IsString(id)) {
                if (!n++) printf("Available packages:\n");
                printf(" - %s (%s) [%s]\n", name->valuestring, id->valuestring, kd.cFileName);
            }
            cJSON_Delete(root);
        } while (FindNextFileA(hf, &fd));
        FindClose(hf);
    } while (FindNextFileA(hk, &kd));
    FindClose(hk);
    if (!n) printf("No packages found.\n");
}

/* ---- installed list helpers (list & check updates) ---- */
void list_installed_packages() {
    char* content = read_file(INSTALLED_FILE_PATH);
    if (!content) {
        printf("No packages installed.\n");
        return;
    }

    cJSON* root = cJSON_Parse(content);
    free(content);
    if (!root || !cJSON_IsArray(root)) {
        printf("Invalid installed package format.\n");
        cJSON_Delete(root);
        return;
    }

    printf("Installed packages:\n");
    int len = cJSON_GetArraySize(root);
    for (int i = 0; i < len; ++i) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        if (!item) continue;
        const cJSON* name = cJSON_GetObjectItem(item, "name");
        const cJSON* id = cJSON_GetObjectItem(item, "id");
        if (name && id) {
            printf("  %s (%s)\n", name->valuestring, id->valuestring);
        }
    }

    cJSON_Delete(root);
}

void check_updates() {
    char* content = read_file(INSTALLED_FILE_PATH);
    if (!content) {
        printf("No installed packages recorded.\n");
        return;
    }
    cJSON* root = cJSON_Parse(content);
    free(content);
    if (!root || !cJSON_IsArray(root)) {
        printf("Invalid installed file format.\n");
        cJSON_Delete(root);
        return;
    }

    int found = 0, len = cJSON_GetArraySize(root);
    for (int i = 0; i < len; ++i) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        const cJSON* id = cJSON_GetObjectItem(item, "id");
        const cJSON* name = cJSON_GetObjectItem(item, "name");
        const cJSON* installed = cJSON_GetObjectItem(item, "version");
        if (!cJSON_IsString(id) || !cJSON_IsString(installed)) continue;

        char path[MAX_PATH];
        if (!locate_package(id->valuestring, path, sizeof(path))) continue;
        char* pkgdata = read_file(path);
        if (!pkgdata) continue;
        cJSON* pkg = cJSON_Parse(pkgdata);
        free(pkgdata);
        if (!pkg) continue;

        // a [latest] URL means the repo JSON never carries a version: ask upstream
        const cJSON* url = cJSON_GetObjectItem(pkg, "url");
        const cJSON* ver = cJSON_GetObjectItem(pkg, "version");
        const char* latest = NULL;
        NetResolved r;
        if (cJSON_IsString(url) && strstr(url->valuestring, NET_LATEST)) {
            if (net_resolve(url->valuestring, &r) == 0 && strcmp(r.version, "unknown") != 0)
                latest = r.version;
        } else if (cJSON_IsString(ver)) {
            latest = ver->valuestring;
        }

        if (latest && strcmp(installed->valuestring, latest) != 0) {
            printf("Update available: %s (%s -> %s)\n",
                   cJSON_IsString(name) ? name->valuestring : id->valuestring,
                   installed->valuestring, latest);
            found++;
        }
        cJSON_Delete(pkg);
    }
    if (!found) printf("Everything is up to date.\n");
    cJSON_Delete(root);
}

/* ---- install from installed file (reinstall all) ---- */
void install_from_package_json() {
    char* content = read_file(INSTALLED_FILE_PATH);
    if (!content) {
        printf("No package.json found.\n");
        return;
    }

    cJSON* root = cJSON_Parse(content);
    free(content);
    if (!root || !cJSON_IsArray(root)) {
        printf("Invalid package.json format.\n");
        cJSON_Delete(root);
        return;
    }

    int count = cJSON_GetArraySize(root);
    if (count == 0) {
        printf("No packages listed in package.json.\n");
        cJSON_Delete(root);
        return;
    }

    Package* packages = calloc((size_t)count, sizeof(Package));
    if (!packages) {
        cJSON_Delete(root);
        return;
    }
    for (int i = 0; i < count; ++i) {
        const cJSON* id = cJSON_GetObjectItem(cJSON_GetArrayItem(root, i), "id");
        if (!cJSON_IsString(id)) continue;
        char* spec = id->valuestring;
        load_packages(&packages[i], 1, &spec);
    }

    download_all(packages, count);
    wait_and_install_packages(count, packages);

    free(packages);
    cJSON_Delete(root);
}

/* ---- main() ---- */
int main(int argc, char* argv[]) {
    installed_mutex = CreateMutex(NULL, FALSE, NULL);
    if (!installed_mutex) return 1;

    CoInitialize(NULL);
    if (net_init() != 0) {
        fprintf(stderr, "Could not initialise libcurl.\n");
        return 1;
    }
    atexit(net_cleanup);

    // -V anywhere on the line = verbose: installers run with their own window, no silent switches
    for (int a = 1; a < argc;) {
        if (strcmp(argv[a], "-V") == 0) {
            g_quiet = 0;
            memmove(&argv[a], &argv[a + 1], (size_t)(argc - a) * sizeof(*argv));
            argc--;
        } else {
            a++;
        }
    }

    // Build paths based on %USERPROFILE%
    char userProfile[MAX_PATH];
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, userProfile))) {
        // fallback to env
        char* up = getenv("USERPROFILE");
        if (up) strncpy(userProfile, up, MAX_PATH-1);
        else strcpy(userProfile, ".");
    }

    snprintf(KNIVES_FOLDER_PATH, MAX_PATH, "%s\\%s", userProfile, DIR_KNIVES);
    snprintf(KNIVES_CONFIG_PATH, MAX_PATH, "%s\\knives.json", KNIVES_FOLDER_PATH);
    snprintf(INSTALLED_FILE_PATH, MAX_PATH, "%s\\package.json", userProfile);

    // downloads go to ~/.cache/swiss; if that can't be created, fall back to %TEMP%
    snprintf(CACHE_DIR, MAX_PATH, "%s\\%s", userProfile, DIR_CACHE);
    if (ex_mkdirs(CACHE_DIR) != 0) {
        const char* tmp = getenv("TEMP");
        printf("Could not create %s; downloading to %s instead.\n", CACHE_DIR, tmp ? tmp : ".");
        snprintf(CACHE_DIR, MAX_PATH, "%s", tmp ? tmp : ".");
    }

    ensure_knives_folder_and_config();

    if (argc >= 2 && strcmp(argv[1], "-Sr") == 0 && argc == 4) {  // Add/update knife
        save_knife(argv[2], argv[3]);  // name, url
        printf("Knife '%s' saved with URL: %s\n", argv[2], argv[3]);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "-Sy") == 0 && argc == 3) {  // Sync a knife
        int r = sync_knife_git_to_folder(argv[2]);
        if (r == 0) printf("Synced knife '%s'\n", argv[2]);
        return r == 0 ? 0 : 1;
    }

    if (argc < 2) {
        printf("Usage:\n");
        printf("  sk -Qi                 [List installed packages]\n");
        printf("  sk -Q --info <pkg>     [Show installed package info]\n");
        printf("  sk -Ql                 [List all packages in the knives folders]\n");
        printf("  sk -Ss <pkg>           [Search for package in knives]\n");
        printf("  sk -S <knife>/<pkg> [pkg2...] [Install packages]\n");
        printf("  sk -Sy [knife]         [Sync one knife, or every knife if none is named]\n");
        printf("  sk -Si                 [Install from %s]\n", INSTALLED_FILE_PATH);
        printf("  sk -Sr <name> <url>    [Add/update knife]\n");
        printf("  sk -R <pkg>            [Uninstall a package]\n");
        printf("  -V                     [Anywhere: verbose, installers show their own window]\n");
        return 0;
    }

    if (strcmp(argv[1], "-Sy") == 0 && argc == 2) {
        return sync_all_knives();
    }

    if (strcmp(argv[1], "-R") == 0 && argc >= 3) {
        // Uninstall by id (we need to locate package file first)
        char spec[256];
        strncpy(spec, argv[2], sizeof(spec)-1);
        char pkgpath[MAX_PATH];
        Package pkg;
        if (!locate_package(spec, pkgpath, sizeof(pkgpath)) ||
            parse_package_json(pkgpath, &pkg) != 0) {
            printf("Failed to load package info for %s\n", argv[2]);
            return 1;
        }

        if (strlen(pkg.uninstaller) == 0) {
            printf("No uninstaller path defined for %s\n", pkg.name);
            return 1;
        }

        InstType ut = inst_from_name(pkg.untype);
        const char* flags = (g_quiet && pkg.silent) ? inst_quiet_args(ut, 1) : NULL;
        DWORD code = 0;
        int started;

        printf("Uninstalling %s...\n", pkg.name);
        if (ut == INST_MSI) {
            char log[MAX_PATH], margs[2 * MAX_PATH + 64];
            log_path(pkg.id, "msi-uninstall", log, sizeof(log));
            snprintf(margs, sizeof(margs), "/x \"%s\" %s /L*v \"%s\"", pkg.uninstaller, flags ? flags : "", log);
            started = run_wait("msiexec.exe", margs, flags != NULL, &code) == 0;
        } else {
            started = run_wait(pkg.uninstaller, flags, flags != NULL, &code) == 0;
        }

        if (started && exit_ok(code)) {
            remove_installed_package(pkg.id);
            printf("%s uninstalled and removed from records.\n", pkg.name);
            return 0;
        }
        if (started)
            printf("The uninstaller for %s exited with code %lu; keeping its record.\n", pkg.name, (unsigned long)code);
        else
            printf("Failed to run the uninstaller for %s.\n", pkg.name);
        return 1;
    }

    if (strcmp(argv[1], "-Q") == 0) {
        list_installed_packages();
        return 0;
    }
    if (strcmp(argv[1], "-Su") == 0) {
        check_updates();
        return 0;
    }
    if (strcmp(argv[1], "-Si") == 0) {
        install_from_package_json();
        return 0;
    }
    if (strcmp(argv[1], "-Ql") == 0) {
        list_packages();
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "-Skl") == 0) {
        list_knives();
        return 0;
    }

    if (strcmp(argv[1], "-S") == 0 && argc >= 3) {
        // For each package arg, ensure the knife is synced first if it references a knife
        for (int i = 2; i < argc; ++i) {
            // if arg contains knife/name then sync that knife
            char* slash = strchr(argv[i], '/');
            if (slash) {
                char knife_name[MAX_KNIFE_NAME] = {0};
                size_t len = (size_t)(slash - argv[i]);
                if (len >= sizeof(knife_name)) len = sizeof(knife_name)-1;
                strncpy(knife_name, argv[i], len);
                knife_name[len] = '\0';
                sync_knife_git_to_folder(knife_name);
            } else {
                // ensure default main is present and synced
                // optional: we can auto sync main, but keep it optional:
                // sync_knife_git_to_folder("main");
            }
        }

        download_and_install_packages(argc - 2, &argv[2]);
        return 0;
    }

    CoUninitialize();
    printf("Unknown command. Run without arguments for help.\n");
    return 0;
}
