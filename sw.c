#include <windows.h>
#include <shlobj.h>   // for SHGetKnownFolderPath
#include <direct.h>   // for _mkdir
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cjson.h"

char LIST_PATH[512];
char INSTALLED_PATH[512];

void init_paths() {
    PWSTR wide_path = NULL;

    // Get home directory (C:\Users\YourName)
    HRESULT hr = SHGetKnownFolderPath(&FOLDERID_Profile, 0, NULL, &wide_path);
    if (!SUCCEEDED(hr)) {
        fprintf(stderr, "❌ Could not locate home directory.\n");
        exit(1);
    }

    // Convert wide string to UTF-8 string
    char home[MAX_PATH];
    wcstombs(home, wide_path, MAX_PATH);
    CoTaskMemFree(wide_path); // ✅ free the string after use

    // Build paths
    sprintf(LIST_PATH, "%s\\swissknife\\list.json", home);
    sprintf(INSTALLED_PATH, "%s\\swissknife\\package.json", home);

    // Create folder if it doesn't exist
    char dir_path[512];
    sprintf(dir_path, "%s\\swissknife", home);
    _mkdir(dir_path); // safe even if it already exists
}


void execute_command(const char* path, const char* args) {
    ShellExecute(NULL, "open", path, args, NULL, SW_SHOW);
}

cJSON* load_json_array(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return cJSON_CreateArray(); // If not exists, return empty array

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);

    char* data = (char*)malloc(len + 1);
    fread(data, 1, len, f);
    data[len] = '\0';
    fclose(f);

    cJSON* root = cJSON_Parse(data);
    free(data);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return cJSON_CreateArray();
    }
    return root;
}

void save_json_array(const char* path, cJSON* array) {
    char* data = cJSON_Print(array);
    FILE* f = fopen(path, "w");
    if (!f) {
        perror("Failed to write JSON");
        return;
    }
    fputs(data, f);
    fclose(f);
    free(data);
}

cJSON* find_package_by_id(cJSON* list, const char* id) {
    int size = cJSON_GetArraySize(list);
    for (int i = 0; i < size; i++) {
        cJSON* pkg = cJSON_GetArrayItem(list, i);
        const char* pid = cJSON_GetObjectItem(pkg, "id")->valuestring;
        if (strcmp(pid, id) == 0) return pkg;
    }
    return NULL;
}

void append_to_installed(const char* name, const char* id) {
    cJSON* installed = load_json_array(INSTALLED_PATH);
    int already = 0;

    for (int i = 0; i < cJSON_GetArraySize(installed); i++) {
        cJSON* item = cJSON_GetArrayItem(installed, i);
        const char* existing_id = cJSON_GetObjectItem(item, "id")->valuestring;
        if (strcmp(existing_id, id) == 0) {
            already = 1;
            break;
        }
    }

    if (!already) {
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", name);
        cJSON_AddStringToObject(obj, "id", id);
        cJSON_AddItemToArray(installed, obj);
        save_json_array(INSTALLED_PATH, installed);
    }

    cJSON_Delete(installed);
}

void remove_from_installed(const char* id) {
    cJSON* installed = load_json_array(INSTALLED_PATH);
    int size = cJSON_GetArraySize(installed);

    for (int i = 0; i < size; i++) {
        cJSON* item = cJSON_GetArrayItem(installed, i);
        if (strcmp(cJSON_GetObjectItem(item, "id")->valuestring, id) == 0) {
            cJSON_DeleteItemFromArray(installed, i);
            break;
        }
    }

    save_json_array(INSTALLED_PATH, installed);
    cJSON_Delete(installed);
}

void install_package(cJSON* pkg) {
    const char* name = cJSON_GetObjectItem(pkg, "name")->valuestring;
    const char* id = cJSON_GetObjectItem(pkg, "id")->valuestring;
    const char* path = cJSON_GetObjectItem(pkg, "installer_path")->valuestring;
    const char* type = cJSON_GetObjectItem(pkg, "type")->valuestring;
    const char* silent = cJSON_GetObjectItem(pkg, "silent")->valuestring;

    printf("Installing %s (%s)\n", name, id);
    if (strlen(type) > 0) {
        printf("Using type '%s' (ignoring silent)\n", type);
        execute_command(path, NULL); // Add type-specific logic later
    } else {
        execute_command(path, silent);
    }

    append_to_installed(name, id);
}

void uninstall_package(cJSON* pkg) {
    const char* name = cJSON_GetObjectItem(pkg, "name")->valuestring;
    const char* id = cJSON_GetObjectItem(pkg, "id")->valuestring;
    const char* uninstaller = cJSON_GetObjectItem(pkg, "uninstaller_path")->valuestring;

    printf("Uninstalling %s (%s)\n", name, id);
    execute_command(uninstaller, NULL);
    remove_from_installed(id);
}

void list_all(cJSON* list) {
    printf("Available Packages:\n");
    for (int i = 0; i < cJSON_GetArraySize(list); i++) {
        cJSON* pkg = cJSON_GetArrayItem(list, i);
        printf("- %s (%s)\n",
               cJSON_GetObjectItem(pkg, "name")->valuestring,
               cJSON_GetObjectItem(pkg, "id")->valuestring);
    }
}

void import_all(cJSON* list) {
    cJSON* installed = load_json_array(INSTALLED_PATH);
    for (int i = 0; i < cJSON_GetArraySize(installed); i++) {
        const char* id = cJSON_GetObjectItem(cJSON_GetArrayItem(installed, i), "id")->valuestring;
        cJSON* pkg = find_package_by_id(list, id);
        if (pkg) install_package(pkg);
        else printf("⚠ Package not found in list: %s\n", id);
    }
    cJSON_Delete(installed);
}

int main(int argc, char* argv[]) {
    init_paths();
    cJSON* list = load_json_array(LIST_PATH);
    if (!list) return 1;

    if (argc < 2) {
        printf("Usage:\n  sw list\n  sw install [id]\n  sw uninstall [id]\n  sw import\n");
        return 0;
    }
    if (strcmp(argv[1], "init") == 0) {
        // Only create if doesn't exist
        FILE* f1 = fopen(LIST_PATH, "r");
        FILE* f2 = fopen(INSTALLED_PATH, "r");

        if (!f1) {
            cJSON* template = cJSON_CreateArray();
            cJSON* pkg = cJSON_CreateObject();
            cJSON_AddStringToObject(pkg, "name", "App Name");
            cJSON_AddStringToObject(pkg, "id", "appid");
            cJSON_AddStringToObject(pkg, "installer_path", "C:/Path/To/Installer.exe");
            cJSON_AddStringToObject(pkg, "uninstaller_path", "C:/Path/To/Uninstaller.exe");
            cJSON_AddStringToObject(pkg, "type", "");
            cJSON_AddStringToObject(pkg, "silent", "/S");
            cJSON_AddItemToArray(template, pkg);
            save_json_array(LIST_PATH, template);
            cJSON_Delete(template);
            printf("Created template list.json \n");
        } else {
            fclose(f1);
            printf("list.json already exists \n");
        }

        if (!f2) {
            cJSON* empty = cJSON_CreateArray();
            save_json_array(INSTALLED_PATH, empty);
            cJSON_Delete(empty);
            printf("Created empty package.json \n");
        } else {
            fclose(f2);
            printf("package.json already exists \n");
        }

        cJSON_Delete(list); // Clean up since we loaded it earlier
    }
    else if (strcmp(argv[1], "list") == 0) {
        list_all(list);
    } else if (strcmp(argv[1], "install") == 0 && argc == 3) {
        cJSON* pkg = find_package_by_id(list, argv[2]);
        if (pkg) install_package(pkg);
        else printf("Package not found: %s\n", argv[2]);
    } else if (strcmp(argv[1], "uninstall") == 0 && argc == 3) {
        cJSON* pkg = find_package_by_id(list, argv[2]);
        if (pkg) uninstall_package(pkg);
        else printf("Package not found: %s\n", argv[2]);
    } else if (strcmp(argv[1], "import") == 0) {
        import_all(list);
    } else {
        printf("Invalid command\n");
    }

    cJSON_Delete(list);
    return 0;
}

