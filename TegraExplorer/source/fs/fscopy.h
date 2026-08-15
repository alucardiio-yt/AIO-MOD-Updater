#pragma once
#include <utils/types.h>
#include "../err.h"

#define COPY_MODE_CANCEL BIT(0)
#define COPY_MODE_PRINT BIT(1)

typedef void (*FileCopyProgressCallback)(const char *source, u32 percent);

void FileCopySetProgressCallback(FileCopyProgressCallback callback);
void FileCopySetQuiet(bool quiet);

ErrCode_t FileCopy(const char *locin, const char *locout, u8 options);
ErrCode_t FolderDelete(const char *path);
ErrCode_t FolderCopy(const char *locin, const char *locout);