#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "../Application/boot/boot_main.c"
#include "flash_if.h"
#include "crc32.h"

static unsigned erases[12], programs, networks;
static int fail_sector = -1;
static uint32_t fail_program = UINT32_MAX;
static bool corrupt_program;
static uint8_t image[721];
struct netif gnetif;

static void *at(uint32_t address) { return (void *)(uintptr_t)address; }
static uint32_t sector_address(uint32_t sector)
{
    return 0x08000000u + (sector < 4u ? sector * 0x4000u :
                          sector == 4u ? 0x10000u : (sector - 4u) * 0x20000u);
}

HAL_StatusTypeDef HAL_FLASH_Unlock(void) { return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void) { return HAL_OK; }
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *erase, uint32_t *error)
{
    (void)error;
    for (uint32_t i = erase->Sector; i < erase->Sector + erase->NbSectors; i++) {
        assert(i >= 5u && i <= 9u);
        if ((int)i == fail_sector) return HAL_ERROR;
        memset(at(sector_address(i)), 0xFF, 0x20000u);
        erases[i]++;
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type, uint32_t address, uint64_t data)
{
    (void)type;
    assert(address >= META_FLASH_BASE && address < STAGING_FLASH_END && !(address & 3u));
    if (address == fail_program) return HAL_ERROR;
    uint32_t *dest = at(address);
    uint32_t word = (uint32_t)data;
    if ((word & ~*dest) != 0u) return HAL_ERROR;
    *dest &= word;
    programs++;
    if (corrupt_program && address >= STAGING_FLASH_BASE) *dest ^= 1u;
    return HAL_OK;
}

uint32_t HAL_GetTick(void) { static uint32_t tick; return tick++; }
static unsigned wdg_starts, wdg_refreshes;
void boot_wdg_start(void) { wdg_starts++; }
void boot_wdg_refresh(void) { wdg_refreshes++; }
void HAL_NVIC_SystemReset(void) { abort(); }
void boot_jump_to_app(uint32_t base) { (void)base; abort(); }
void MX_LWIP_Init(void) { networks++; }
void modbus_boot_server_init(metadata_t *meta, uint16_t port) { (void)port; fw_proto_init(meta); }
void modbus_boot_server_poll(void) {}
void ethernetif_input(struct netif *netif) { (void)netif; }
void sys_check_timeouts(void) {}
void discovery_init(void) {}
bool discovery_take_pending_reboot(void) { return false; }
void led_indication_init(void) {}
void led_indication_deinit(void) {}
void led_indication_set(led_pattern_t p) { (void)p; }
void led_indication_poll(uint32_t t) { (void)t; }
void net_id_get_mac(uint8_t mac[6]) { memcpy(mac, "\x02\x00\x12\x34\x56\x78", 6); }

static void map_memory(uint32_t base, size_t size)
{
#ifdef _WIN32
    assert(VirtualAlloc(at(base), size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) == at(base));
#else
    assert(mmap(at(base), size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == at(base));
#endif
}

static void reset_boot(void)
{
    unsigned before = networks;
    sm_start();
    sm_check_entry();
    if (s_state != BOOT_READY_TO_BOOT) assert(networks == before + 1u);
}

static void fixture(void)
{
    memset(at(0x08000000u), 0xFF, 0x100000u);
    memset(at(RAM_BASE), 0, RAM_SIZE);
    memset(erases, 0, sizeof(erases));
    programs = networks = 0;
    fail_sector = -1;
    fail_program = UINT32_MAX;
    corrupt_program = false;
    memset(image, 0x55, sizeof(image));
    uint32_t vectors[2] = {BOOT_REQUEST_FLAG_ADDR, APP_FLASH_BASE + 0x241u};
    memcpy(image, vectors, sizeof(vectors));
    fw_header_t header = {0};
    header.magic = FW_IMAGE_MAGIC;
    header.product_id = PRODUCT_ID_DEFAULT;
    header.hw_revision = HW_REVISION_DEFAULT_U16;
    header.fw_version = 0x0102;
    memcpy(image + FW_HEADER_OFFSET, &header, sizeof(header));
    reset_boot();
    assert(s_state == BOOT_WAIT_COMMAND);
}

static uint16_t status(void)
{
    uint16_t value;
    assert(fw_proto_read_input_regs(0x15, 1, &value) == NMBS_ERROR_NONE);
    return value;
}

static void command(uint16_t cmd)
{
    assert(fw_proto_write_holding_regs(0, 1, &cmd) == NMBS_ERROR_NONE);
}

static void begin(uint16_t size, uint32_t count)
{
    uint32_t crc = crc32_calc(image, sizeof(image));
    uint16_t params[12] = {0, sizeof(image), (uint16_t)(crc >> 16), (uint16_t)crc,
        0, 0, (uint16_t)(PRODUCT_ID_DEFAULT >> 16), (uint16_t)PRODUCT_ID_DEFAULT,
        HW_REVISION_DEFAULT_U16, size, (uint16_t)(count >> 16), (uint16_t)count};
    assert(fw_proto_write_holding_regs(0x10, 12, params) == NMBS_ERROR_NONE);
    command(CMD_BEGIN_UPDATE);
    sm_commands();
}

static void block(uint32_t index, uint16_t length, bool change)
{
    uint16_t regs[123] = {(uint16_t)(index >> 16), (uint16_t)index, length};
    for (uint32_t i = 0; i < length; i++) {
        uint32_t offset = index * 240u + i;
        uint8_t b = offset < sizeof(image) ? image[offset] : 0xFF;
        regs[3u + i / 2u] |= (uint16_t)b << ((i & 1u) ? 0u : 8u);
    }
    if (change) regs[3] ^= 0x100u;
    assert(fw_proto_write_holding_regs(0x100, 3u + (length + 1u) / 2u, regs) == NMBS_ERROR_NONE);
}

static void receive_image(void)
{
    begin(240, 4);
    assert(status() == CMD_STATUS_OK);
    block(2, 240, false);
    block(0, 240, false);
    block(3, 1, false);
    block(1, 240, false);
    assert(s_meta.received_block_count == 4);
    command(CMD_FINALIZE_UPDATE);
    assert(status() == CMD_STATUS_OK);
    sm_commands();
    assert(s_state == BOOT_VERIFY_STAGING && !s_meta.install_requested);
}

static void test_blocks(void)
{
    fixture();
    begin(239, 4);
    assert(status() == CMD_STATUS_ERROR && erases[8] == 0);
    begin(240, 3);
    assert(status() == CMD_STATUS_ERROR && erases[8] == 0);
    begin(240, 4);
    block(0, 239, false);
    assert(status() == CMD_STATUS_ERROR && !s_meta.received_block_count);
    block(4, 1, false);
    assert(status() == CMD_STATUS_ERROR);
    block(0, 240, false);
    unsigned before = programs;
    block(0, 240, false);
    assert(status() == CMD_STATUS_OK && programs == before && s_meta.received_block_count == 1);
    block(0, 240, true);
    assert(status() == CMD_STATUS_ERROR && programs == before);
    s_meta.received_block_count = 4;
    assert(!metadata_all_blocks_received(&s_meta));
    command(CMD_FINALIZE_UPDATE);
    assert(status() == CMD_STATUS_ERROR);
    reset_boot();
    assert(s_state == BOOT_WAIT_COMMAND && !s_meta.received_block_count && !erases[6]);
    begin(240, 4);
    corrupt_program = true;
    block(0, 240, false);
    assert(status() == CMD_STATUS_ERROR && !s_meta.received_block_count);
}

static void test_recovery(void)
{
    fixture();
    receive_image();
    reset_boot();
    assert(s_state == BOOT_VERIFY_STAGING && !erases[6]);
    s_meta.install_requested = 1;
    assert(metadata_save(&s_meta));
    reset_boot();
    assert(s_state == BOOT_VERIFY_STAGING && !erases[6]);
    command(CMD_INSTALL_UPDATE);
    sm_commands();
    assert(s_state == BOOT_INSTALL_FW && s_meta.install_in_progress && !s_meta.app_valid);
    const uint16_t commands[] = {CMD_BEGIN_UPDATE, CMD_ABORT_UPDATE, CMD_REBOOT};
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        assert(fw_proto_write_holding_regs(0, 1, &commands[i]) == NMBS_EXCEPTION_SERVER_DEVICE_BUSY);
    assert(status() == CMD_STATUS_BUSY);
    sm_install_fw();
    reset_boot();
    assert(s_state == BOOT_INSTALL_FW && fw_installer_get_state() == INST_ERASING);
    sm_install_fw();
    sm_install_fw();
    sm_install_fw();
    reset_boot();
    assert(s_state == BOOT_INSTALL_FW && fw_installer_get_state() == INST_ERASING);
    while (fw_installer_get_state() != INST_VERIFYING) sm_install_fw();
    reset_boot();
    assert(s_state == BOOT_INSTALL_FW && fw_installer_get_state() == INST_ERASING);
    unsigned guard = 100;
    while (s_state == BOOT_INSTALL_FW && guard--) sm_install_fw();
    assert(s_state == BOOT_VERIFY_APP);
    sm_verify_app();
    assert(s_state == BOOT_READY_TO_BOOT && status() == CMD_STATUS_OK);
    assert(memcmp(image, at(APP_FLASH_BASE), sizeof(image)) == 0);
    reset_boot();
    assert(s_state == BOOT_READY_TO_BOOT && s_meta.app_valid);
    uint16_t mac[3];
    assert(fw_proto_read_input_regs(0x19, 3, mac) == NMBS_ERROR_NONE);
    assert(mac[0] == 0x0200 && mac[1] == 0x1234 && mac[2] == 0x5678);
}

static void test_failures(void)
{
    fixture();
    receive_image();
    fail_sector = META_FLASH_SECTOR;
    command(CMD_INSTALL_UPDATE);
    sm_commands();
    assert(s_state == BOOT_ERROR && !erases[6]);
    fail_sector = -1;
    command(CMD_INSTALL_UPDATE);
    sm_commands();
    fail_sector = APP_FIRST_SECTOR;
    sm_install_fw();
    assert(s_state == BOOT_ERROR && !s_meta.app_valid && !s_meta.install_in_progress);
    fail_sector = -1;
    reset_boot();
    assert(s_state == BOOT_ERROR);
    command(CMD_INSTALL_UPDATE);
    sm_commands();
    while (fw_installer_get_state() == INST_ERASING) sm_install_fw();
    fail_program = APP_FLASH_BASE;
    sm_install_fw();
    assert(s_state == BOOT_ERROR && !s_meta.app_valid);
    fail_program = UINT32_MAX;
    command(CMD_INSTALL_UPDATE);
    sm_commands();
    while (fw_installer_get_state() != INST_VERIFYING) sm_install_fw();
    fail_sector = META_FLASH_SECTOR;
    sm_install_fw();
    assert(s_state == BOOT_ERROR && !s_meta.app_valid && status() == CMD_STATUS_ERROR);
    fail_sector = -1;
    fail_program = META_FLASH_BASE + 4u;
    command(CMD_ABORT_UPDATE);
    assert(status() == CMD_STATUS_ERROR);
    fail_program = UINT32_MAX;
    reset_boot();
    assert(s_state == BOOT_WAIT_COMMAND && !s_meta.app_valid);
}

static void test_vectors_and_flash(void)
{
    fixture();
    memcpy(at(STAGING_FLASH_BASE), image, sizeof(image));
    assert(app_validate_image(STAGING_FLASH_BASE, sizeof(image), crc32_calc(image, sizeof(image))));
    fw_header_t *header = at(STAGING_FLASH_BASE + FW_HEADER_OFFSET);
    header->hw_revision ^= 0x100u;
    assert(!app_validate_header(STAGING_FLASH_BASE, PRODUCT_ID_DEFAULT, HW_REVISION_DEFAULT_U16));
    header->hw_revision ^= 0x100u;
    header->hw_revision ^= 1u;
    assert(app_validate_header(STAGING_FLASH_BASE, PRODUCT_ID_DEFAULT, HW_REVISION_DEFAULT_U16));
    header->product_id ^= 1u;
    assert(!app_validate_header(STAGING_FLASH_BASE, PRODUCT_ID_DEFAULT, HW_REVISION_DEFAULT_U16));
    header->product_id ^= 1u;
    header->vector_table_offset = 0x200u;
    assert(!app_validate_header(STAGING_FLASH_BASE, PRODUCT_ID_DEFAULT, HW_REVISION_DEFAULT_U16));
    header->vector_table_offset = 0u;
    uint32_t *vectors = at(STAGING_FLASH_BASE);
    vectors[0] = RAM_BASE + RAM_SIZE;
    assert(!app_validate_vectors_at(STAGING_FLASH_BASE, sizeof(image)));
    vectors[0] = BOOT_REQUEST_FLAG_ADDR - 1u;
    assert(!app_validate_vectors_at(STAGING_FLASH_BASE, sizeof(image)));
    vectors[0] = BOOT_REQUEST_FLAG_ADDR;
    vectors[1] &= ~1u;
    assert(!app_validate_vectors_at(STAGING_FLASH_BASE, sizeof(image)));
    vectors[1] = APP_FLASH_BASE + 0x10001u;
    assert(!app_validate_vectors_at(STAGING_FLASH_BASE, sizeof(image)));
    assert(!flash_if_write(APP_FLASH_END - 4u, image, 5));
    assert(!flash_if_write(STAGING_FLASH_END, image, 4));
    assert(!flash_if_write(APP_FLASH_BASE + 1u, image, 4));
    assert(!flash_if_erase_sectors(10, 1));
    assert(!flash_if_erase_sectors(11, 1));
    assert(!flash_if_erase_sectors(5, 2));
    assert(!flash_if_erase_sectors(0, 1));
}

static void test_finalize_crc(void)
{
    fixture();
    begin(240, 4);
    block(0, 240, false);
    block(1, 240, false);
    block(2, 240, false);
    block(3, 1, false);
    ((uint8_t *)at(STAGING_FLASH_BASE))[700] ^= 1u;
    command(CMD_FINALIZE_UPDATE);
    assert(status() == CMD_STATUS_ERROR && s_meta.last_error == BOOT_ERR_IMAGE_CRC);
    assert(!s_meta.staging_valid && !erases[6]);
}

static void touch_session(void)
{
    uint16_t param = 0;
    assert(fw_proto_write_holding_regs(0x10, 1, &param) == NMBS_ERROR_NONE);
}

static void test_session_timeout(void)
{
    const uint32_t T = FW_SESSION_TIMEOUT_MS;

    /* Idle bootloader (no session) never expires. */
    fixture();
    assert(!fw_proto_session_timeout(1000));
    assert(!fw_proto_session_timeout(1000 + 10u * T));
    assert(s_meta.boot_state == BOOT_WAIT_COMMAND && s_meta.last_error == BOOT_ERR_NONE);

    /* Verified staging waiting for INSTALL, no valid application: the window is
     * restarted by client writes, expiry clears the session and stays in BL. */
    receive_image();
    assert(s_meta.staging_valid);
    assert(!fw_proto_session_timeout(1000));
    assert(!fw_proto_session_timeout(1000 + T - 1u));
    touch_session();
    assert(!fw_proto_session_timeout(1000 + T + 10u));          /* window restarts here */
    assert(!fw_proto_session_timeout(1000 + T + 10u + T - 1u));
    assert(!fw_proto_session_timeout(1000 + T + 10u + T));      /* expires, app invalid */
    assert(s_meta.boot_state == BOOT_WAIT_COMMAND && !s_meta.staging_valid);
    assert(s_meta.last_error == BOOT_ERR_UPDATE_TIMEOUT && !s_meta.received_block_count);
    reset_boot();
    assert(s_state == BOOT_WAIT_COMMAND);

    /* Block reception with a valid installed application: expiry asks the caller
     * to reset into it, and the next boot really starts the application. */
    fixture();
    memcpy(at(APP_FLASH_BASE), image, sizeof(image));
    s_meta.app_valid = 1u;
    s_meta.app_image_size = sizeof(image);
    s_meta.app_image_crc32 = crc32_calc(image, sizeof(image));
    assert(metadata_save(&s_meta));
    begin(240, 4);
    block(0, 240, false);
    assert(s_meta.boot_state == BOOT_RECEIVE_FW);
    assert(!fw_proto_session_timeout(5000));
    assert(fw_proto_session_timeout(5000 + T));
    assert(s_meta.boot_state == BOOT_WAIT_COMMAND && !s_meta.received_block_count);
    const unsigned starts = wdg_starts;
    reset_boot();
    assert(s_state == BOOT_READY_TO_BOOT && s_meta.app_valid);
    assert(wdg_starts == starts);   /* the plain handoff never arms the watchdog */
    fixture();
    assert(wdg_starts == starts + 1u && s_state == BOOT_WAIT_COMMAND);   /* staying arms it once */
}

int main(void)
{
    map_memory(0x08000000u, 0x100000u);
    map_memory(RAM_BASE, RAM_SIZE);
    test_blocks();
    test_recovery();
    test_failures();
    test_vectors_and_flash();
    test_finalize_crc();
    test_session_timeout();
    puts("Boot, Flash, blocks, metadata and power-loss tests passed");
    return 0;
}
