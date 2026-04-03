#include <xtl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Core headers — compiled as C++ alongside the .c files (via /TP flag)
#include "utils.h"
#include "xdk_file_system.h"

// ===[ Vtable Implementations ]===

static char* xdkResolvePath(FileSystem* fs, const char* relativePath) {
    XdkFileSystem* xfs = (XdkFileSystem*)fs;
    size_t baseLen = strlen(xfs->basePath);
    size_t relLen = strlen(relativePath);
    char* result = (char*)malloc(baseLen + relLen + 1);
    if (!result) return NULL;
    memcpy(result, xfs->basePath, baseLen);
    memcpy(result + baseLen, relativePath, relLen + 1);

    // Convert forward slashes to backslashes for Xbox 360
    for (char* p = result + baseLen; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    return result;
}

static bool xdkFileExists(FileSystem* fs, const char* relativePath) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;

    HANDLE hFile = CreateFileA(fullPath, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(fullPath);

    if (hFile == INVALID_HANDLE_VALUE) return false;
    CloseHandle(hFile);
    return true;
}

static char* xdkReadFileText(FileSystem* fs, const char* relativePath) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return NULL;

    HANDLE hFile = CreateFileA(fullPath, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(fullPath);

    if (hFile == INVALID_HANDLE_VALUE) return NULL;

    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == INVALID_FILE_SIZE) {
        CloseHandle(hFile);
        return NULL;
    }

    char* buffer = (char*)malloc(fileSize + 1);
    if (!buffer) {
        CloseHandle(hFile);
        return NULL;
    }

    DWORD bytesRead;
    ReadFile(hFile, buffer, fileSize, &bytesRead, NULL);
    CloseHandle(hFile);

    buffer[bytesRead] = '\0';
    return buffer;
}

static bool xdkWriteFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;

    HANDLE hFile = CreateFileA(fullPath, GENERIC_WRITE, 0,
                               NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(fullPath);

    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD len = (DWORD)strlen(contents);
    DWORD written;
    BOOL ok = WriteFile(hFile, contents, len, &written, NULL);
    CloseHandle(hFile);
    return ok && written == len;
}

static bool xdkDeleteFile(FileSystem* fs, const char* relativePath) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (!fullPath) return false;

    BOOL ok = DeleteFileA(fullPath);
    free(fullPath);
    return ok != FALSE;
}

// ===[ Vtable ]===

static FileSystemVtable xdkFileSystemVtable = {
    xdkResolvePath,
    xdkFileExists,
    xdkReadFileText,
    xdkWriteFileText,
    xdkDeleteFile,
};

// ===[ Public API ]===

XdkFileSystem* XdkFileSystem_create(const char* dataWinPath) {
    XdkFileSystem* xfs = (XdkFileSystem*)calloc(1, sizeof(XdkFileSystem));
    xfs->base.vtable = &xdkFileSystemVtable;

    // Extract directory from data.win path
    const char* lastSlash = strrchr(dataWinPath, '\\');
    if (!lastSlash) lastSlash = strrchr(dataWinPath, '/');

    if (lastSlash) {
        size_t dirLen = (size_t)(lastSlash - dataWinPath + 1);
        if (dirLen >= sizeof(xfs->basePath)) dirLen = sizeof(xfs->basePath) - 1;
        memcpy(xfs->basePath, dataWinPath, dirLen);
        xfs->basePath[dirLen] = '\0';
    } else {
        strcpy(xfs->basePath, "game:\\");
    }

    return xfs;
}
