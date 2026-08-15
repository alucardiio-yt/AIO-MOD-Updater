#include <switch.h>
#include <string.h>

#define AIO_EXIT_SENTINEL "aio-forwarder: no next target"

static char s_argv[2048];
static char s_next_argv[2048];
static char s_next_path[FS_MAX_PATH];
static char s_home_argv[2048];
static char s_home_path[FS_MAX_PATH];
static const char s_loader_tag[] = "aio-forwarder " VERSION;

static void* s_heap;
static size_t s_heap_size;
static Handle s_process;
static u8 s_tls_backup[0x100];
static u128 s_user_id;
static NroHeader s_header;
u64 s_mapped_addr;
Result s_previous_result;
static u64 s_mapped_size;

static enum {
    AioCodeMemory_None = 0,
    AioCodeMemory_Foreign = BIT(0),
    AioCodeMemory_Same = BIT(0) | BIT(1),
} s_code_memory;

void NX_NORETURN aioNroTrampoline(const ConfigEntry* entries, u64 handle, u64 entrypoint);
void NX_NORETURN aioLoadTarget(void);

static void abort_loader(Result rc)
{
    diagAbortWithResult(rc);
}

static void strip_sdmc_prefix(char* path)
{
    if (!strncmp(path, "sdmc:/", 6))
        memmove(path, path + 5, strlen(path + 5) + 1);
}

static void NX_NORETURN request_application_exit(void)
{
    Result rc = smInitialize();
    const bool sm_ready = R_SUCCEEDED(rc);
    Service applet = {0}, proxy = {0}, self = {0};

    if (sm_ready)
        rc = smGetService(&applet, "appletOE");
    if (R_SUCCEEDED(rc)) {
        const u64 reserved = 0;
        rc = serviceDispatchIn(&applet, 0, reserved,
            .in_send_pid = true,
            .in_num_handles = 1,
            .in_handles = { s_process },
            .out_num_objects = 1,
            .out_objects = &proxy);
    }
    if (R_SUCCEEDED(rc))
        rc = serviceDispatch(&proxy, 1, .out_num_objects = 1, .out_objects = &self);
    if (R_SUCCEEDED(rc))
        rc = serviceDispatch(&self, 0);

    serviceClose(&self);
    serviceClose(&proxy);
    serviceClose(&applet);
    if (sm_ready)
        smExit();

    if (R_FAILED(rc))
        abort_loader(rc);

    for (;;)
        svcSleepThread(86400000000000ULL);
}

static size_t choose_heap_size(void)
{
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);

    u64 result = 0;
    if (total > used + 0x200000)
        result = (total - used - 0x200000) & ~0x1FFFFFULL;
    if (!result)
        result = 0x2000000ULL * 16;
    if (result > 0x6000000)
        result -= 0x6000000;
    return (size_t)result;
}

static void prepare_heap(void)
{
    void* address = NULL;
    const size_t size = choose_heap_size();
    const Result rc = svcSetHeapSize(&address, size);
    if (R_FAILED(rc) || !address)
        abort_loader(MAKERESULT(Module_HomebrewLoader, 9));
    s_heap = address;
    s_heap_size = size;
}

static void process_handle_server(void* arg)
{
    Handle session = (Handle)(uintptr_t)arg;
    void* tls = armGetTls();
    hipcMakeRequestInline(tls);

    s32 index = 0;
    Result rc = svcReplyAndReceive(&index, &session, 1, INVALID_HANDLE, UINT64_MAX);
    if (R_FAILED(rc))
        abort_loader(MAKERESULT(Module_HomebrewLoader, 15));

    HipcParsedRequest req = hipcParseRequest(tls);
    if (req.meta.num_copy_handles != 1)
        abort_loader(MAKERESULT(Module_HomebrewLoader, 17));

    s_process = req.data.copy_handles[0];
    svcCloseHandle(session);
}

static void acquire_own_process_handle(void)
{
    Handle server = INVALID_HANDLE, client = INVALID_HANDLE;
    Result rc = svcCreateSession(&server, &client, 0, 0);
    if (R_FAILED(rc))
        abort_loader(MAKERESULT(Module_HomebrewLoader, 12));

    Thread thread;
    rc = threadCreate(&thread, process_handle_server, (void*)(uintptr_t)server,
                      s_heap, 0x1000, 0x20, 0);
    if (R_FAILED(rc))
        abort_loader(MAKERESULT(Module_HomebrewLoader, 10));
    if (R_FAILED(threadStart(&thread)))
        abort_loader(MAKERESULT(Module_HomebrewLoader, 13));

    hipcMakeRequestInline(armGetTls(), .num_copy_handles = 1).copy_handles[0] = CUR_PROCESS_HANDLE;
    svcSendSyncRequest(client);
    svcCloseHandle(client);
    threadWaitForExit(&thread);
    threadClose(&thread);
}

static bool kernel_has_5x_info(void)
{
    u64 ignored = 0;
    return R_VALUE(svcGetInfo(&ignored, InfoType_UserExceptionContextAddress, INVALID_HANDLE, 0)) !=
           KERNELRESULT(InvalidEnumValue);
}

static bool kernel_has_4x_info(void)
{
    u64 ignored = 0;
    return R_VALUE(svcGetInfo(&ignored, InfoType_InitialProcessIdRange, INVALID_HANDLE, 0)) !=
           KERNELRESULT(InvalidEnumValue);
}

static void detect_code_memory(void)
{
    if (detectMesosphere()) {
        s_code_memory = AioCodeMemory_Same;
        return;
    }

    if (kernel_has_5x_info()) {
        Handle code = INVALID_HANDLE;
        if (R_SUCCEEDED(svcCreateCodeMemory(&code, s_heap, 0x1000))) {
            const Result probe = svcControlCodeMemory(code, (CodeMapOperation)-1, 0, 0x1000, 0);
            svcCloseHandle(code);
            s_code_memory = R_VALUE(probe) == KERNELRESULT(InvalidEnumValue)
                ? AioCodeMemory_Same : AioCodeMemory_Foreign;
        }
    } else if (kernel_has_4x_info()) {
        s_code_memory = AioCodeMemory_Same;
    } else {
        s_code_memory = AioCodeMemory_None;
    }
}

static void unmap_previous_target(void)
{
    if (!s_mapped_size)
        return;

    const NroHeader* h = &s_header;
    size_t rw = (h->segments[2].size + h->bss_size + 0xFFF) & ~0xFFF;

    if (R_FAILED(svcBreak(BreakReason_NotificationOnlyFlag | BreakReason_PreUnloadDll,
                          s_mapped_addr, s_mapped_size)))
        abort_loader(MAKERESULT(Module_HomebrewLoader, 24));

    Result rc = svcUnmapProcessCodeMemory(s_process,
        s_mapped_addr + h->segments[0].file_off,
        (u64)s_heap + h->segments[0].file_off,
        h->segments[0].size);
    if (R_FAILED(rc)) abort_loader(MAKERESULT(Module_HomebrewLoader, 24));

    rc = svcUnmapProcessCodeMemory(s_process,
        s_mapped_addr + h->segments[1].file_off,
        (u64)s_heap + h->segments[1].file_off,
        h->segments[1].size);
    if (R_FAILED(rc)) abort_loader(MAKERESULT(Module_HomebrewLoader, 25));

    rc = svcUnmapProcessCodeMemory(s_process,
        s_mapped_addr + h->segments[2].file_off,
        (u64)s_heap + h->segments[2].file_off,
        rw);
    if (R_FAILED(rc)) abort_loader(MAKERESULT(Module_HomebrewLoader, 26));

    svcBreak(BreakReason_NotificationOnlyFlag | BreakReason_PostUnloadDll,
             s_mapped_addr, s_mapped_size);
    s_mapped_addr = s_mapped_size = 0;
}

static void read_initial_launch_spec(void)
{
    FsStorage storage;
    romfs_header header;
    Result rc = fsOpenDataStorageByCurrentProcess(&storage);
    if (R_FAILED(rc)) abort_loader(rc);
    rc = fsStorageRead(&storage, 0, &header, sizeof(header));
    if (R_FAILED(rc)) abort_loader(rc);

    u8 dirs[2048];
    u8 files[4096];
    if (header.dirTableSize > sizeof(dirs) || header.fileTableSize > sizeof(files))
        abort_loader(MAKERESULT(Module_HomebrewLoader, LibnxError_OutOfMemory));

    rc = fsStorageRead(&storage, header.dirTableOff, dirs, header.dirTableSize);
    if (R_FAILED(rc)) abort_loader(rc);
    rc = fsStorageRead(&storage, header.fileTableOff, files, header.fileTableSize);
    if (R_FAILED(rc)) abort_loader(rc);

    const romfs_dir* root = (const romfs_dir*)dirs;
    const romfs_file* first = (const romfs_file*)(files + root->childFile);
    const romfs_file* second = (const romfs_file*)(files + first->sibling);

    if (first->dataSize >= sizeof(s_next_argv) || second->dataSize >= sizeof(s_next_path))
        abort_loader(MAKERESULT(Module_HomebrewLoader, LibnxError_OutOfMemory));

    rc = fsStorageRead(&storage, header.fileDataOff + first->dataOff, s_next_argv, first->dataSize);
    if (R_FAILED(rc)) abort_loader(rc);
    rc = fsStorageRead(&storage, header.fileDataOff + second->dataOff, s_next_path, second->dataSize);
    if (R_FAILED(rc)) abort_loader(rc);
    fsStorageClose(&storage);

    s_next_argv[first->dataSize] = '\0';
    s_next_path[second->dataSize] = '\0';
    strcpy(s_home_argv, s_next_argv);
    strcpy(s_home_path, s_next_path);
}

static void choose_next_launch(void)
{
    if (!s_mapped_size) {
        read_initial_launch_spec();
        return;
    }

    if (!strcmp(s_next_argv, AIO_EXIT_SENTINEL)) {
        if (!strcmp(s_next_path, s_home_path))
            request_application_exit();
        strcpy(s_next_path, s_home_path);
        strcpy(s_next_argv, s_home_argv);
    }
}

static NroHeader* load_nro_into_heap(void)
{
    char path[FS_MAX_PATH];
    strncpy(path, s_next_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    strip_sdmc_prefix(path);
    memcpy(s_argv, s_next_argv, sizeof(s_argv));

    svcBreak(BreakReason_NotificationOnlyFlag | BreakReason_PreLoadDll,
             (uintptr_t)s_argv, sizeof(s_argv));

    NroStart* start = (NroStart*)s_heap;
    NroHeader* header = (NroHeader*)((u8*)s_heap + sizeof(NroStart));
    FsFileSystem sd;
    FsFile file;
    Result rc = fsOpenSdCardFileSystem(&sd);
    if (R_FAILED(rc)) abort_loader(rc);
    rc = fsFsOpenFile(&sd, path, FsOpenMode_Read, &file);
    if (R_FAILED(rc)) abort_loader(rc);

    u64 bytes = 0;
    rc = fsFileRead(&file, 0, start, s_heap_size, FsReadOption_None, &bytes);
    fsFileClose(&file);
    fsFsClose(&sd);
    if (R_FAILED(rc) || header->magic != NROHEADER_MAGIC ||
        bytes < sizeof(*start) + sizeof(*header) + header->size)
        abort_loader(R_FAILED(rc) ? rc : MAKERESULT(Module_HomebrewLoader, 6));

    return header;
}

static void validate_segments(const NroHeader* h)
{
    for (int i = 0; i < 3; ++i) {
        const u64 off = h->segments[i].file_off;
        const u64 len = h->segments[i].size;
        if (off >= h->size || len > h->size || off + len > h->size)
            abort_loader(MAKERESULT(Module_HomebrewLoader, 6));
    }
}

static u64 map_target(const NroHeader* source, u64* heap_start, u64* heap_size)
{
    memcpy(&s_header, source, sizeof(s_header));
    const NroHeader* h = &s_header;
    const size_t rw = (h->segments[2].size + h->bss_size + 0xFFF) & ~0xFFF;
    const size_t total = (h->size + h->bss_size + 0xFFF) & ~0xFFF;

    virtmemLock();
    void* target = virtmemFindCodeMemory(total, 0);
    Result rc = svcMapProcessCodeMemory(s_process, (u64)target, (u64)s_heap, total);
    virtmemUnlock();
    if (R_FAILED(rc)) abort_loader(MAKERESULT(Module_HomebrewLoader, 18));

    rc = svcSetProcessMemoryPermission(s_process, (u64)target + h->segments[0].file_off,
                                       h->segments[0].size, Perm_R | Perm_X);
    if (R_FAILED(rc)) abort_loader(rc);
    rc = svcSetProcessMemoryPermission(s_process, (u64)target + h->segments[1].file_off,
                                       h->segments[1].size, Perm_R);
    if (R_FAILED(rc)) abort_loader(rc);
    rc = svcSetProcessMemoryPermission(s_process, (u64)target + h->segments[2].file_off,
                                       rw, Perm_Rw);
    if (R_FAILED(rc)) abort_loader(rc);

    s_mapped_addr = (u64)target;
    s_mapped_size = h->segments[2].file_off + rw;

    *heap_start = (u64)s_heap + s_mapped_size;
    *heap_size = s_heap_size + (u64)s_heap - *heap_start;
    return s_mapped_addr;
}

static void launch_mapped_target(u64 entrypoint, u64 target_heap, u64 target_heap_size)
{
#define M EntryFlag_IsMandatory
    static ConfigEntry cfg[] = {
        { EntryType_MainThreadHandle,      0, {0, 0} },
        { EntryType_ProcessHandle,         0, {0, 0} },
        { EntryType_AppletType,            0, {AppletType_SystemApplication, EnvAppletFlags_ApplicationOverride} },
        { EntryType_OverrideHeap,          M, {0, 0} },
        { EntryType_Argv,                  0, {0, 0} },
        { EntryType_NextLoadPath,          0, {0, 0} },
        { EntryType_LastLoadResult,        0, {0, 0} },
        { EntryType_SyscallAvailableHint,  0, {UINT64_MAX, UINT64_MAX} },
        { EntryType_SyscallAvailableHint2, 0, {UINT64_MAX, 0} },
        { EntryType_RandomSeed,            0, {0, 0} },
        { EntryType_UserIdStorage,         0, {(u64)(uintptr_t)&s_user_id, 0} },
        { EntryType_HosVersion,            0, {0, 0} },
        { EntryType_EndOfList,             0, {(u64)(uintptr_t)s_loader_tag, sizeof(s_loader_tag)} },
    };
#undef M

    if (!(s_code_memory & BIT(0)))
        cfg[7].Value[0x4B / 64] &= ~(1UL << (0x4B % 64));
    if (!(s_code_memory & BIT(1)))
        cfg[7].Value[0x4C / 64] &= ~(1UL << (0x4C % 64));

    cfg[0].Value[0] = envGetMainThreadHandle();
    cfg[1].Value[0] = s_process;
    cfg[3].Value[0] = target_heap;
    cfg[3].Value[1] = target_heap_size;
    cfg[4].Value[1] = (u64)(uintptr_t)s_argv;
    cfg[5].Value[0] = (u64)(uintptr_t)s_next_path;
    cfg[5].Value[1] = (u64)(uintptr_t)s_next_argv;
    cfg[6].Value[0] = s_previous_result;
    cfg[9].Value[0] = randomGet64();
    cfg[9].Value[1] = randomGet64();
    cfg[11].Value[0] = hosversionGet();
    cfg[11].Value[1] = hosversionIsAtmosphere() ? 0x41544D4F53504852ULL : 0;

    svcBreak(BreakReason_NotificationOnlyFlag | BreakReason_PostLoadDll,
             s_mapped_addr, s_mapped_size);
    strcpy(s_next_argv, AIO_EXIT_SENTINEL);
    aioNroTrampoline(cfg, (u64)-1, entrypoint);
}

void NX_NORETURN aioLoadTarget(void)
{
    memcpy((u8*)armGetTls() + 0x100, s_tls_backup, sizeof(s_tls_backup));
    choose_next_launch();
    unmap_previous_target();

    NroHeader* header = load_nro_into_heap();
    validate_segments(header);

    u64 target_heap = 0, target_heap_size = 0;
    const u64 entrypoint = map_target(header, &target_heap, &target_heap_size);
    launch_mapped_target(entrypoint, target_heap, target_heap_size);
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    memcpy(s_tls_backup, (u8*)armGetTls() + 0x100, sizeof(s_tls_backup));
    prepare_heap();
    acquire_own_process_handle();
    detect_code_memory();
    aioLoadTarget();
}

u32 __nx_applet_type = AppletType_Application;
u32 __nx_fs_num_sessions = 1;
u32 __nx_fsdev_direntry_cache_size = 1;
bool __nx_fsdev_support_cwd = false;

void __libnx_initheap(void)
{
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    fake_heap_start = NULL;
    fake_heap_end = NULL;
}

void __appInit(void)
{
    Handle ams = INVALID_HANDLE;
    Result rc = svcConnectToNamedPort(&ams, "ams");
    const u32 ams_bit = R_SUCCEEDED(rc) ? BIT(31) : 0;
    if (R_SUCCEEDED(rc))
        svcCloseHandle(ams);

    rc = smInitialize();
    if (R_FAILED(rc))
        abort_loader(MAKERESULT(Module_HomebrewLoader, LibnxError_InitFail_SM));

    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion firmware;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&firmware)))
            hosversionSet(ams_bit | MAKEHOSVERSION(firmware.major, firmware.minor, firmware.micro));
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc))
        abort_loader(MAKERESULT(Module_HomebrewLoader, LibnxError_InitFail_FS));
    smExit();
}

void __appExit(void) {}

void __wrap_exit(void)
{
    abort_loader(MAKERESULT(Module_HomebrewLoader, 39));
}

void* __libnx_alloc(size_t size)
{
    (void)size;
    abort_loader(MAKERESULT(Module_HomebrewLoader, 40));
}

void* __libnx_aligned_alloc(size_t alignment, size_t size)
{
    (void)alignment;
    (void)size;
    abort_loader(MAKERESULT(Module_HomebrewLoader, 41));
}

void __libnx_free(void* ptr)
{
    (void)ptr;
    abort_loader(MAKERESULT(Module_HomebrewLoader, 43));
}
