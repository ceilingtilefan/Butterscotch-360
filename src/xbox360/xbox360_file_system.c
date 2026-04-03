#include "xbox360_file_system.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ===[ Helpers ]===

static char* buildFullPath(X360FileSystem* fs, const char* relativePath) {
    size_t baseLen = strlen(fs->basePath);
    size_t relLen = strlen(relativePath);
    char* fullPath = safeMalloc(baseLen + relLen + 1);
    memcpy(fullPath, fs->basePath, baseLen);
    memcpy(fullPath + baseLen, relativePath, relLen);
    fullPath[baseLen + relLen] = '\0';
    return fullPath;
}

// ===[ Vtable Implementations ]===

static char* x360ResolvePath(FileSystem* fs, const char* relativePath) {
    return buildFullPath((X360FileSystem*) fs, relativePath);
}

static bool x360FileExists(FileSystem* fs, const char* relativePath) {
    char* fullPath = buildFullPath((X360FileSystem*) fs, relativePath);
    struct stat st;
    bool exists = (stat(fullPath, &st) == 0);
    free(fullPath);
    return exists;
}

static char* x360ReadFileText(FileSystem* fs, const char* relativePath) {
    char* fullPath = buildFullPath((X360FileSystem*) fs, relativePath);
    FILE* f = fopen(fullPath, "rb");
    free(fullPath);
    if (f == nullptr)
        return nullptr;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* content = safeMalloc((size_t) size + 1);
    size_t bytesRead = fread(content, 1, (size_t) size, f);
    content[bytesRead] = '\0';
    fclose(f);
    return content;
}

static bool x360WriteFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    char* fullPath = buildFullPath((X360FileSystem*) fs, relativePath);
    FILE* f = fopen(fullPath, "wb");
    free(fullPath);
    if (f == nullptr)
        return false;

    size_t len = strlen(contents);
    size_t written = fwrite(contents, 1, len, f);
    fclose(f);
    return written == len;
}

static bool x360DeleteFile(FileSystem* fs, const char* relativePath) {
    char* fullPath = buildFullPath((X360FileSystem*) fs, relativePath);
    int result = remove(fullPath);
    free(fullPath);
    return result == 0;
}

// ===[ Vtable ]===

static FileSystemVtable x360FileSystemVtable = {
    .resolvePath = x360ResolvePath,
    .fileExists = x360FileExists,
    .readFileText = x360ReadFileText,
    .writeFileText = x360WriteFileText,
    .deleteFile = x360DeleteFile,
};

// ===[ Lifecycle ]===

X360FileSystem* X360FileSystem_create(const char* dataWinPath) {
    X360FileSystem* fs = safeCalloc(1, sizeof(X360FileSystem));
    fs->base.vtable = &x360FileSystemVtable;

    // Derive basePath by stripping the filename from dataWinPath
    const char* lastSlash = strrchr(dataWinPath, '/');
    if (lastSlash != nullptr) {
        size_t dirLen = (size_t) (lastSlash - dataWinPath + 1);
        fs->basePath = safeMalloc(dirLen + 1);
        memcpy(fs->basePath, dataWinPath, dirLen);
        fs->basePath[dirLen] = '\0';
    } else {
        fs->basePath = safeStrdup("uda:/");
    }

    return fs;
}

void X360FileSystem_destroy(X360FileSystem* fs) {
    if (fs == nullptr) return;
    free(fs->basePath);
    free(fs);
}
