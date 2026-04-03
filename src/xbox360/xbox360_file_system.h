#pragma once

#include "../file_system.h"

typedef struct {
    FileSystem base;
    char* basePath; // directory containing data.win, with trailing separator
} X360FileSystem;

// Creates an Xbox 360 file system from the path to the data.win file.
// The basePath is derived by stripping the filename from dataWinPath.
// On Xbox 360 with libxenon, files are on USB (uda:/) or HDD (hdd:/).
X360FileSystem* X360FileSystem_create(const char* dataWinPath);
void X360FileSystem_destroy(X360FileSystem* fs);
