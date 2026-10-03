// swissknife_master.c - Full Swissknife installer with detailed GUI and CLI support

#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <stdbool.h>
#include <process.h>
#include "cjson.h"

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")

#define REPO_FOLDER "C:/farm/wheats/Swissknife/knives"
#define CONFIG_FILE "C:/farm/wheats/Swissknife/.pkgconfig"
#define INSTALLED_FILE "C:/farm/wheats/Swissknife/package.json"
#define LOG_FILE "C:/farm/wheats/Swissknife/install.log"
#define MAX_PACKAGES 64
#define WM_UPDATE_GUI (WM_USER + 1)

typedef struct Package {
    char name[128];
    char id[64];
    char version[64];
    char url[1024];
    char silent[128];
    char type[16];
    char installer[64];
    char out_path[MAX_PATH];
} Package;

// Globals for GUI and threads
static HWND hwndMain = NULL;
static HWND hwndList = NULL;
static HWND hwndOverallProgress = NULL;
static HWND hwndCancelBtn = NULL;
static HWND hwndStatusText = NULL;
static HWND hwndQueueList = NULL;
static HANDLE cancel_event = NULL;
static HANDLE download_threads[MAX_PACKAGES];
static Package packages[MAX_PACKAGES];
static int package_count = 0;
static HANDLE installed_mutex = NULL;

typedef struct {
    int index;
    HANDLE cancel_event;
} ThreadParam;

void append_log(const char* text);
void update_status_list(int index, const char* status);
void cancel_install(void);
int parse_package_json(const char* filepath, Package* pkg);
int download_file_powershell(const char* url, const char* out_path);
DWORD WINAPI download_thread(LPVOID param);
void add_or_update_installed_package(Package* pkg);
void wait_and_install_packages(int count, Package packages[]);
void update_gui_status(int current, HWND hwnd);
void populate_queue_list(int current);

// File read/write helpers
char* read_file(const char* filepath) {
    FILE* f = fopen(filepath, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char* buffer = malloc(size + 1);
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

void save_repo_url(const char* url) {
    FILE* f = fopen(CONFIG_FILE, "w");
    if (f) {
        fprintf(f, "%s\n", url);
        fclose(f);
    }
}

void read_repo_url(char* buffer, size_t size) {
    FILE* f = fopen(CONFIG_FILE, "r");
    if (f) {
        fgets(buffer, (int)size, f);
        buffer[strcspn(buffer, "\n")] = 0;
        fclose(f);
    } else {
        printf("Enter your Git repository URL: ");
        fgets(buffer, (int)size, stdin);
        buffer[strcspn(buffer, "\n")] = 0;
        save_repo_url(buffer);
    }
}

void git_sync(const char* repo_url) {
    if (_access(REPO_FOLDER, 0) != 0) {
        char cmd[1024];
        sprintf(cmd, "git clone %s %s", repo_url, REPO_FOLDER);
        system(cmd);
    } else {
        char cmd[1024];
        sprintf(cmd, "git -C %s pull", REPO_FOLDER);
        system(cmd);
    }
}

// Parsing package JSON to Package struct
int parse_package_json(const char* filepath, Package* pkg) {
    char* content = read_file(filepath);
    if (!content) return -1;
    cJSON* root = cJSON_Parse(content);
    free(content);
    if (!root) return -2;

    memset(pkg, 0, sizeof(Package));
    cJSON* item = NULL;
    if ((item = cJSON_GetObjectItem(root, "name"))) strncpy(pkg->name, item->valuestring, sizeof(pkg->name) - 1);
    if ((item = cJSON_GetObjectItem(root, "id"))) strncpy(pkg->id, item->valuestring, sizeof(pkg->id) - 1);
    if ((item = cJSON_GetObjectItem(root, "version"))) strncpy(pkg->version, item->valuestring, sizeof(pkg->version) - 1);
    if ((item = cJSON_GetObjectItem(root, "url"))) strncpy(pkg->url, item->valuestring, sizeof(pkg->url) - 1);
    if ((item = cJSON_GetObjectItem(root, "silent"))) strncpy(pkg->silent, item->valuestring, sizeof(pkg->silent) - 1);
    if ((item = cJSON_GetObjectItem(root, "type"))) strncpy(pkg->type, item->valuestring, sizeof(pkg->type) - 1);
    if ((item = cJSON_GetObjectItem(root, "installer"))) strncpy(pkg->installer, item->valuestring, sizeof(pkg->installer) - 1);

    snprintf(pkg->out_path, MAX_PATH, "%s\\%s.%s", getenv("TEMP"), pkg->id, pkg->type);

    cJSON_Delete(root);
    return 0;
}

// Powershell-based download function, blocking
int download_file_powershell(const char* url, const char* out_path) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "powershell -Command \"Start-BitsTransfer -Source '%s' -Destination '%s'\"",
             url, out_path);
    return system(cmd);
}

// Thread function for download
DWORD WINAPI download_thread(LPVOID param) {
    ThreadParam* p = (ThreadParam*)param;
    int idx = p->index;
    if (WaitForSingleObject(p->cancel_event, 0) == WAIT_OBJECT_0) {
        free(p);
        return 0;
    }
    update_status_list(idx, "Downloading...");
    int res = download_file_powershell(packages[idx].url, packages[idx].out_path);
    if (res != 0) {
        update_status_list(idx, "Download Failed");
    } else {
        update_status_list(idx, "Downloaded");
    }
    free(p);
    return 0;
}

// Update status text in the listbox for a package index
void update_status_list(int index, const char* status) {
    if (!hwndList) return;
    TCHAR buf[512] = {0};
    SendMessage(hwndList, LB_GETTEXT, (WPARAM)index, (LPARAM)buf);
    char* orig = strstr(buf, " [");
    if (orig) *orig = 0;

    char newentry[512];
#ifdef UNICODE
    char name[512];
    wcstombs(name, buf, sizeof(name));
    snprintf(newentry, sizeof(newentry), "%s [%s]", name, status);
    // Convert back to wchar_t
    wchar_t wnewentry[512];
    mbstowcs(wnewentry, newentry, sizeof(wnewentry));
    SendMessage(hwndList, LB_DELETESTRING, (WPARAM)index, 0);
    SendMessage(hwndList, LB_INSERTSTRING, (WPARAM)index, (LPARAM)wnewentry);
#else
    snprintf(newentry, sizeof(newentry), "%s [%s]", buf, status);
    SendMessage(hwndList, LB_DELETESTRING, (WPARAM)index, 0);
    SendMessage(hwndList, LB_INSERTSTRING, (WPARAM)index, (LPARAM)newentry);
#endif
}

// Add or update installed package JSON
void add_or_update_installed_package(Package* pkg) {
    WaitForSingleObject(installed_mutex, INFINITE);
    char* content = read_file(INSTALLED_FILE);
    cJSON* root = NULL;
    if (content) {
        root = cJSON_Parse(content);
        free(content);
    }
    if (!root) root = cJSON_CreateArray();

    // Remove existing package with same id
    int len = cJSON_GetArraySize(root);
    for (int i = 0; i < len; ++i) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        cJSON* id = cJSON_GetObjectItem(item, "id");
        if (id && strcmp(id->valuestring, pkg->id) == 0) {
            cJSON_DeleteItemFromArray(root, i);
            break;
        }
    }

    cJSON* new_pkg = cJSON_CreateObject();
    cJSON_AddStringToObject(new_pkg, "name", pkg->name);
    cJSON_AddStringToObject(new_pkg, "id", pkg->id);
    cJSON_AddStringToObject(new_pkg, "version", pkg->version);

    cJSON_AddItemToArray(root, new_pkg);
    char* output = cJSON_Print(root);
    write_file(INSTALLED_FILE, output);
    free(output);
    cJSON_Delete(root);
    ReleaseMutex(installed_mutex);
}

// Append a line to install log
void append_log(const char* text) {
    FILE* f = fopen(LOG_FILE, "a");
    if (f) {
        fprintf(f, "%s\n", text);
        fclose(f);
    }
}

// Cancel installation by signaling event
void cancel_install(void) {
    if (cancel_event) SetEvent(cancel_event);
}

// Wait and install packages sequentially with GUI updates
void wait_and_install_packages(int count, Package packages[]) {
    for (int i = 0; i < count; ++i) {
        if (WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0) {
            update_status_list(i, "Cancelled");
            return;
        }

        update_status_list(i, "Installing...");

        // Set default silent flags if empty
        if (packages[i].silent[0] == 0 && packages[i].installer[0] != 0) {
            if (strcmp(packages[i].installer, "nsis") == 0) strcpy(packages[i].silent, "/S");
            else if (strcmp(packages[i].installer, "inno") == 0) strcpy(packages[i].silent, "/VERYSILENT /SUPPRESSMSGBOXES");
            else if (strcmp(packages[i].installer, "msi") == 0) strcpy(packages[i].silent, "/quiet /norestart");
            else if (strcmp(packages[i].installer, "installshield") == 0) strcpy(packages[i].silent, "/s /v\"/qn\"");
            else if (strcmp(packages[i].installer, "squirrel") == 0) strcpy(packages[i].silent, "--silent");
        }

        SHELLEXECUTEINFOA sei = { sizeof(sei) };
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpFile = packages[i].out_path;
        sei.lpParameters = (packages[i].silent[0] != 0) ? packages[i].silent : NULL;
        sei.nShow = (packages[i].silent[0] != 0) ? SW_HIDE : SW_SHOWNORMAL;
        sei.lpVerb = "open";

        if (ShellExecuteExA(&sei)) {
            WaitForSingleObject(sei.hProcess, INFINITE);
            CloseHandle(sei.hProcess);
            update_status_list(i, "Installed");
            append_log(packages[i].name);
            add_or_update_installed_package(&packages[i]);
        } else {
            update_status_list(i, "Install Failed");
        }
    }
}

// GUI: Update status text and queue list in the window
void update_gui_status(int current, HWND hwnd) {
    if (!hwndStatusText || !hwndQueueList) return;

    char status_msg[512];
    if (current >= 0 && current < package_count) {
        snprintf(status_msg, sizeof(status_msg),
                 "Downloading: %s\nPlease wait until the downloading is completed.",
                 packages[current].name);
    } else {
        strcpy(status_msg, "Download complete.");
    }
    SetWindowText(hwndStatusText, status_msg);

    // Update queue list (next packages)
    SendMessage(hwndQueueList, LB_RESETCONTENT, 0, 0);
    for (int i = current + 1; i < package_count; i++) {
        SendMessage(hwndQueueList, LB_ADDSTRING, 0, (LPARAM)packages[i].name);
    }
}

// Window procedure
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = CreateFont(
            -18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        CreateWindow("BUTTON", "Package Installation Status", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                     10, 10, 480, 270, hwnd, NULL, GetModuleHandle(NULL), NULL);

        hwndList = CreateWindow("LISTBOX", NULL,
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                                20, 40, 460, 160, hwnd, (HMENU)1,
                                GetModuleHandle(NULL), NULL);
        SendMessage(hwndList, WM_SETFONT, (WPARAM)hFont, TRUE);

        hwndOverallProgress = CreateWindowEx(0, PROGRESS_CLASS, NULL,
                                             WS_CHILD | WS_VISIBLE,
                                             20, 210, 460, 20,
                                             hwnd, NULL, GetModuleHandle(NULL), NULL);
        SendMessage(hwndOverallProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessage(hwndOverallProgress, PBM_SETPOS, 0, 0);

        hwndCancelBtn = CreateWindow("BUTTON", "Cancel",
                                     WS_CHILD | WS_VISIBLE,
                                     385, 240, 95, 30,
                                     hwnd, (HMENU)2, GetModuleHandle(NULL), NULL);
        SendMessage(hwndCancelBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

        hwndStatusText = CreateWindow("STATIC", "Starting...",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                     20, 280, 460, 40,
                                     hwnd, NULL, GetModuleHandle(NULL), NULL);
        SendMessage(hwndStatusText, WM_SETFONT, (WPARAM)hFont, TRUE);

        CreateWindow("STATIC", "Queue:", WS_CHILD | WS_VISIBLE | SS_LEFT,
                     20, 325, 460, 20,
                     hwnd, NULL, GetModuleHandle(NULL), NULL);

        hwndQueueList = CreateWindow("LISTBOX", NULL,
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                                     20, 345, 460, 150,
                                     hwnd, (HMENU)3, GetModuleHandle(NULL), NULL);
        SendMessage(hwndQueueList, WM_SETFONT, (WPARAM)hFont, TRUE);

        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == 2) { // Cancel button clicked
            cancel_install();
            MessageBox(hwnd, "Installation cancelled.", "Swissknife", MB_OK | MB_ICONINFORMATION);
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_UPDATE_GUI:
        // lParam contains current downloading index
        update_gui_status((int)lParam, hwnd);
        // Update overall progress bar accordingly
        if (hwndOverallProgress) {
            int pos = (int)(((double)(lParam + 1) / package_count) * 100);
            SendMessage(hwndOverallProgress, PBM_SETPOS, pos, 0);
        }
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// GUI thread procedure
DWORD WINAPI gui_thread(LPVOID param) {
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "SwissknifeWindowClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClass(&wc);

    hwndMain = CreateWindow(wc.lpszClassName, "Swissknife Installer",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, 520, 550,
                            NULL, NULL, wc.hInstance, NULL);

    ShowWindow(hwndMain, SW_SHOW);
    UpdateWindow(hwndMain);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

// Runs the installer GUI, downloads and installs packages one-by-one
int run_installation_gui(int count, Package pkgs[]) {
    cancel_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!cancel_event) {
        printf("Failed to create cancel event.\n");
        return -1;
    }

    package_count = count;
    memcpy(packages, pkgs, sizeof(Package) * count);

    HANDLE gui = CreateThread(NULL, 0, gui_thread, NULL, 0, NULL);
    if (!gui) {
        printf("Failed to create GUI thread.\n");
        CloseHandle(cancel_event);
        return -1;
    }

    // Wait for GUI to initialize
    Sleep(500);

    // Populate package names in listbox
    SendMessage(hwndList, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < package_count; i++) {
        SendMessage(hwndList, LB_ADDSTRING, 0, (LPARAM)packages[i].name);
    }

    // Start downloads one by one
    for (int i = 0; i < package_count; i++) {
        if (WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0) break;

        // Update GUI with current downloading package and queue
        PostMessage(hwndMain, WM_UPDATE_GUI, 0, (LPARAM)i);

        update_status_list(i, "Downloading...");
        int res = download_file_powershell(packages[i].url, packages[i].out_path);
        if (res != 0) {
            update_status_list(i, "Download Failed");
            break;
        } else {
            update_status_list(i, "Downloaded");
        }
    }

    // Install packages sequentially if not cancelled
    if (WaitForSingleObject(cancel_event, 0) != WAIT_OBJECT_0) {
        wait_and_install_packages(package_count, packages);
    } else {
        MessageBox(hwndMain, "Installation cancelled.", "Swissknife", MB_OK | MB_ICONINFORMATION);
    }

    // Cleanup and close GUI
    PostMessage(hwndMain, WM_CLOSE, 0, 0);
    WaitForSingleObject(gui, INFINITE);
    CloseHandle(gui);
    CloseHandle(cancel_event);
    cancel_event = NULL;

    return 0;
}

// Load all packages from repo folder knives/*.json
int load_packages_from_repo(const char* repo_path, Package* pkgs, int max_count) {
    WIN32_FIND_DATAA fd;
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*.json", repo_path);
    HANDLE hFind = FindFirstFileA(search_path, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    int count = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            char full_path[MAX_PATH];
            snprintf(full_path, sizeof(full_path), "%s\\%s", repo_path, fd.cFileName);
            if (count < max_count) {
                if (parse_package_json(full_path, &pkgs[count]) == 0) {
                    count++;
                }
            }
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    return count;
}

void check_updates() {
    FILE* installed = fopen(INSTALLED_FILE, "r");
    if (!installed) {
        printf("No installed packages recorded.\n");
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), installed)) {
        char name[128] = {0}, id[64] = {0}, version_installed[64] = {0};
        sscanf(line, " { \"name\": \"%[^\"]\", \"id\": \"%[^\"]\", \"version\": \"%[^\"]\" }", name, id, version_installed);
        char filepath[512];
        sprintf(filepath, "%s/%s.json", REPO_FOLDER, id);
        FILE* pkg = fopen(filepath, "r");
        if (!pkg) continue;
        char version_repo[64] = {0}, line2[512];
        while (fgets(line2, sizeof(line2), pkg)) {
            if (strstr(line2, "\"version\"")) {
                sscanf(line2, " \"version\" : \"%[^\"]\"", version_repo);
                break;
            }
        }
        fclose(pkg);
        if (strcmp(version_installed, version_repo) != 0) {
            printf("Update available: %s (%s → %s)\n", name, version_installed, version_repo);
        }
    }
    fclose(installed);
}
void list_installed_packages() {
    char* content = read_file(INSTALLED_FILE);
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
void install_from_package_json() {
    char* content = read_file(INSTALLED_FILE);
    if (!content) {
        printf("No package.json found.\n");
        return;
    }

    cJSON* root = cJSON_Parse(content);
    free(content);
    if (!root) {
        printf("Invalid package.json format.\n");
        return;
    }

    int count = cJSON_GetArraySize(root);
    if (count == 0) {
        printf("No packages listed in package.json.\n");
        cJSON_Delete(root);
        return;
    }

    Package* packages = malloc(sizeof(Package) * count);
    HANDLE* threads = malloc(sizeof(HANDLE) * count);
    int actual_count = 0;

    for (int i = 0; i < count; ++i) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        cJSON* id = cJSON_GetObjectItem(item, "id");
        if (!id || !cJSON_IsString(id)) continue;

        char filepath[512];
        sprintf(filepath, "%s/%s.json", REPO_FOLDER, id->valuestring);
        if (parse_package_json(filepath, &packages[actual_count]) != 0) {
            printf("Failed to load %s.json\n", id->valuestring);
            continue;
        }

        threads[actual_count] = CreateThread(NULL, 0, download_thread, &packages[actual_count], 0, NULL);
        actual_count++;
    }

    WaitForMultipleObjects(actual_count, threads, TRUE, INFINITE);
    for (int i = 0; i < actual_count; ++i) {
        if (threads[i]) CloseHandle(threads[i]);
    }

    wait_and_install_packages(actual_count, packages);
    free(threads);
    free(packages);
    cJSON_Delete(root);
}
int main(int argc, char* argv[]) {
    installed_mutex = CreateMutex(NULL, FALSE, NULL);

    if (!installed_mutex) return 1;
    char repo_url[1024] = {0};

    if (argc == 3 && strcmp(argv[1], "-Sr") == 0) {
        save_repo_url(argv[2]);
        printf("Repository URL saved.\n");
        return 0;
    }
    if (argc < 2) {
        printf("Swissknife Usage:\n");
        printf("  -S [pkg1 pkg2 ...] : Install packages with GUI\n");
        printf("  -Ql                 : List available packages\n");
        printf("  -Qi                 : List installed packages\n");
        CloseHandle(installed_mutex);
        return 0;
    }

    if (strcmp(argv[1], "-S") == 0) {
        if (argc < 3) {
            printf("Specify package names to install after -S.\n");
            CloseHandle(installed_mutex);
            return 1;
        }

        Package pkgs[MAX_PACKAGES];
        int count = 0;
        for (int i = 2; i < argc && count < MAX_PACKAGES; i++) {
            char filepath[MAX_PATH];
            snprintf(filepath, sizeof(filepath), "%s/%s.json", REPO_FOLDER, argv[i]);
            if (parse_package_json(filepath, &pkgs[count]) == 0) {
                count++;
            } else {
                printf("Failed to load package: %s\n", argv[i]);
            }
        }

        if (count > 0) {
            int res = run_installation_gui(count, pkgs);
            CloseHandle(installed_mutex);
            return res;
        } else {
            printf("No valid packages to install.\n");
            CloseHandle(installed_mutex);
            return 1;
        }
    } else if (strcmp(argv[1], "-Ql") == 0) {
        Package pkgs[MAX_PACKAGES];
        int count = load_packages_from_repo(REPO_FOLDER, pkgs, MAX_PACKAGES);
        if (count == 0) {
            printf("No packages found in repo.\n");
        } else {
            printf("Available packages:\n");
            for (int i = 0; i < count; i++) {
                printf("- %s (%s)\n", pkgs[i].name, pkgs[i].id);
            }
        }
        CloseHandle(installed_mutex);
        return 0;
    } else if (strcmp(argv[1], "-Qi") == 0) {
        char* content = read_file(INSTALLED_FILE);
        if (!content) {
            printf("No installed packages found.\n");
            CloseHandle(installed_mutex);
            return 0;
        }
        cJSON* root = cJSON_Parse(content);
        free(content);
        if (!root) {
            printf("Installed packages data corrupted.\n");
            CloseHandle(installed_mutex);
            return 1;
        }
        int count = cJSON_GetArraySize(root);
        printf("Installed packages:\n");
        for (int i = 0; i < count; i++) {
            cJSON* item = cJSON_GetArrayItem(root, i);
            cJSON* name = cJSON_GetObjectItem(item, "name");
            cJSON* version = cJSON_GetObjectItem(item, "version");
            printf("- %s %s\n", name ? name->valuestring : "Unknown", version ? version->valuestring : "");
        }
        cJSON_Delete(root);
        CloseHandle(installed_mutex);
        return 0;
    } else {
        printf("Unknown option: %s\n", argv[1]);
        CloseHandle(installed_mutex);
        return 1;
    }
    if (strcmp(argv[1], "-Sy") == 0) {
        git_sync(repo_url);
        return 0;
    }

    if (strcmp(argv[1], "-Q") == 0) {
        list_installed_packages();
        return 0;
    }
    if (strcmp(argv[1], "-Su") == 0) {
        check_updates();
    }
    if (strcmp(argv[1], "-Si") == 0) {
        install_from_package_json();
        return 0;
    }
}
