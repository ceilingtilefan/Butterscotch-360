#pragma once

#include "file_system.h"

typedef struct {
    FileSystem base;
    char basePath[512]; // directory containing data.win (e.g. "game:\\")
} XdkFileSystem;

XdkFileSystem* XdkFileSystem_create(const char* dataWinPath);
