#include "reboot_payload.h"

#include "ams_bpc.h"

#define IRAM_PAYLOAD_MAX_SIZE 0x2F000
#define IRAM_PAYLOAD_BASE 0x40010000

static alignas(0x1000) u8 g_reboot_payload[IRAM_PAYLOAD_MAX_SIZE];
static alignas(0x1000) u8 g_ff_page[0x1000];
static alignas(0x1000) u8 g_work_page[0x1000];

void do_iram_dram_copy(void* buf, uintptr_t iram_addr, size_t size, int option)
{
    memcpy(g_work_page, buf, size);

    SecmonArgs args = {0};
    args.X[0] = 0xF0000201;             /* smcAmsIramCopy */
    args.X[1] = (uintptr_t)g_work_page; /* DRAM Address */
    args.X[2] = iram_addr;              /* IRAM Address */
    args.X[3] = size;                   /* Copy size */
    args.X[4] = option;                 /* 0 = Read, 1 = Write */
    svcCallSecureMonitor(&args);

    memcpy(buf, g_work_page, size);
}

void copy_to_iram(uintptr_t iram_addr, void* buf, size_t size)
{
    do_iram_dram_copy(buf, iram_addr, size, 1);
}

void copy_from_iram(void* buf, uintptr_t iram_addr, size_t size)
{
    do_iram_dram_copy(buf, iram_addr, size, 0);
}

static void clear_iram(void)
{
    memset(g_ff_page, 0xFF, sizeof(g_ff_page));
    for (size_t i = 0; i < IRAM_PAYLOAD_MAX_SIZE; i += sizeof(g_ff_page)) {
        copy_to_iram(IRAM_PAYLOAD_BASE + i, g_ff_page, sizeof(g_ff_page));
    }
}

static int inject_payload(void)
{
    Result rc = spsmInitialize();
    if (R_FAILED(rc))
        return (int)rc;
    smExit();
    rc = amsBpcInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = amsBpcSetRebootPayload(g_reboot_payload, 0x24000);
        amsBpcExit();
    }
    if (R_SUCCEEDED(rc))
        rc = spsmShutdown(true);
    spsmExit();
    if (R_FAILED(rc))
        smInitialize();
    return (int)rc;
}

static int inject_payload_legacy(void)
{
    clear_iram();
    for (size_t i = 0; i < IRAM_PAYLOAD_MAX_SIZE; i += 0x1000) {
        copy_to_iram(IRAM_PAYLOAD_BASE + i, &g_reboot_payload[i], 0x1000);
    }
    return (int)splSetConfig((SplConfigItem)65001, 2);
}

int reboot_to_payload(const char* path, bool legacy)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -2;
    }
    long size = ftell(f);
    if (size <= 0 || size > (legacy ? IRAM_PAYLOAD_MAX_SIZE : 0x24000) ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -2;
    }
    memset(g_reboot_payload, 0, sizeof(g_reboot_payload));
    const size_t read = fread(g_reboot_payload, 1, (size_t)size, f);
    const int close_result = fclose(f);
    if (read != (size_t)size || close_result != 0)
        return -3;

    const int result = legacy ? inject_payload_legacy() : inject_payload();
    if (result == 0 && legacy)
        splExit();
    return result;
}
