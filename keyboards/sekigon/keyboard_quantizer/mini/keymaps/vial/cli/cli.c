// Copyright 2023 sekigon-gonnoc
// SPDX-License-Identifier: GPL-2.0-or-later

#define EMBEDDED_CLI_IMPL
#include "embedded_cli.h"

#include "debug.h"
#include "bootloader.h"
#include "quantum.h"
#include "pio_usb_ll.h"

extern void tusb_print_debug_buffer(void);

// Hot-plug diagnostics counters, defined in mini/matrix.c
extern volatile uint32_t kq_dbg_dev_mount_count;
extern volatile uint32_t kq_dbg_hid_mount_count;
extern volatile uint32_t kq_dbg_hid_umount_count;
extern volatile uint32_t kq_dbg_would_stage1_count;
extern volatile uint32_t kq_dbg_would_stage2_count;
extern volatile uint32_t kq_dbg_longest_unmounted_ms;
extern volatile uint32_t kq_dbg_last_stage1_ms;
extern volatile uint32_t kq_dbg_last_stage2_ms;
extern volatile uint32_t kq_dbg_mounted_mask;
extern volatile uint32_t kq_dbg_unplug_count;
extern volatile uint32_t kq_dbg_unplug_with_mount_count;
extern volatile uint32_t kq_dbg_unplug_with_keys_count;
extern volatile uint32_t kq_dbg_unplug_grv_count;
extern volatile uint32_t kq_dbg_stuck_after_unplug_count;
extern volatile uint32_t kq_dbg_last_unplug_ms;
extern volatile uint32_t kq_dbg_last_stuck_ms;
extern volatile uint32_t kq_dbg_hid_report_count;
extern volatile uint32_t kq_dbg_grv_press_count;
extern volatile uint32_t kq_dbg_grv_release_count;
extern volatile uint32_t kq_dbg_last_grv_change_ms;
uint32_t                 kq_dbg_connected_now(void);
uint32_t                 kq_dbg_line_state(void);
uint32_t                 kq_dbg_suspended(void);
extern volatile uint32_t kq_dbg_partial_mount_count;
extern volatile uint32_t kq_dbg_last_partial_ms;
extern volatile uint32_t kq_dbg_session_max_mask;
uint32_t                 kq_dbg_last_report_nonzero(void);
uint32_t                 kq_dbg_last_report_grv(void);
uint32_t                 kq_dbg_matrix_nonzero_now(void);
uint32_t                 kq_dbg_matrix_grv_now(void);

#define CLI_BUFFER_SIZE 1024
static CLI_UINT           cliBuffer[BYTES_TO_CLI_UINTS(CLI_BUFFER_SIZE)];
static EmbeddedCli       *cli             = NULL;
static bool               cli_initialized = false;
static EmbeddedCliConfig *config;
static volatile bool      isLoadMode = false;

void virtser_recv(uint8_t c) {
    embeddedCliReceiveChar(cli, c);
}

static void writeChar(EmbeddedCli *_, char c) { printf("%c", c); }

static void onVersion(EmbeddedCli *cli, char *args, void *context) {
    printf("firmware version: %s\n", STR(GIT_DESCRIBE));
}

static void onDfu(EmbeddedCli *cli, char *args, void *context) {
    bootloader_jump();
}

static void onReset(EmbeddedCli *cli, char *args, void *context) {
    // Manual soft reset from the PC side, e.g. when the downstream
    // keyboard is no longer recognized. Only runs when typed here;
    // nothing is reset automatically in this diagnostic build.
    soft_reset_keyboard();
}

static void onHostStat(EmbeddedCli *cli, char *args, void *context) {
    printf("connected now: %u (uptime %u ms)\n", (unsigned)kq_dbg_connected_now(), (unsigned)timer_read32());
    printf("root port: line state %u (0=SE0 1=FS idle 2=LS idle 3=SE1), suspended %u\n", (unsigned)kq_dbg_line_state(), (unsigned)kq_dbg_suspended());
    printf("partial mount episodes (interface missing >2s): %u (last at %u ms); session max mask: 0x%x\n", (unsigned)kq_dbg_partial_mount_count, (unsigned)kq_dbg_last_partial_ms, (unsigned)kq_dbg_session_max_mask);
    printf("mounted mask: 0x%x\n", (unsigned)kq_dbg_mounted_mask);
    printf("dev mounts: %u, hid mounts: %u, hid umounts: %u\n", (unsigned)kq_dbg_dev_mount_count, (unsigned)kq_dbg_hid_mount_count, (unsigned)kq_dbg_hid_umount_count);
    printf("longest connected-but-unmounted gap: %u ms\n", (unsigned)kq_dbg_longest_unmounted_ms);
    printf("would-be stage1 (port reset @3s): %u (last at %u ms)\n", (unsigned)kq_dbg_would_stage1_count, (unsigned)kq_dbg_last_stage1_ms);
    printf("would-be stage2 (soft reset @6s): %u (last at %u ms)\n", (unsigned)kq_dbg_would_stage2_count, (unsigned)kq_dbg_last_stage2_ms);
    printf("unplugs: %u (last at %u ms), still mounted at unplug: %u, matrix held keys at unplug: %u, matrix held GRV (hankaku/zenkaku) at unplug: %u\n", (unsigned)kq_dbg_unplug_count, (unsigned)kq_dbg_last_unplug_ms, (unsigned)kq_dbg_unplug_with_mount_count, (unsigned)kq_dbg_unplug_with_keys_count, (unsigned)kq_dbg_unplug_grv_count);
    printf("stuck after unplug (matrix still nonzero 1s later): %u (last at %u ms)\n", (unsigned)kq_dbg_stuck_after_unplug_count, (unsigned)kq_dbg_last_stuck_ms);
    printf("matrix now: nonzero %u, GRV %u; last kbd report: nonzero %u, GRV %u\n", (unsigned)kq_dbg_matrix_nonzero_now(), (unsigned)kq_dbg_matrix_grv_now(), (unsigned)kq_dbg_last_report_nonzero(), (unsigned)kq_dbg_last_report_grv());
    printf("hid reports received: %u; GRV press/release in reports: %u/%u (last GRV change at %u ms)\n", (unsigned)kq_dbg_hid_report_count, (unsigned)kq_dbg_grv_press_count, (unsigned)kq_dbg_grv_release_count, (unsigned)kq_dbg_last_grv_change_ms);
}

static void onDebug(EmbeddedCli *cli, char *args, void *context) {
    debug_enable = !debug_enable;
}

static void onPioUsbStatus(EmbeddedCli *cli, char *args, void *context) {
    pio_port_t const *pp = PIO_USB_PIO_PORT(0);
    printf("error rate: %ld / %ld = %f%%\n", pp->total_error_count,
           pp->total_transaction_count,
           (float)pp->total_error_count / pp->total_transaction_count * 100);
    printf(
        "fatal error rate: %ld / %ld = %f%%\n", pp->total_fatal_error_count,
        pp->total_transaction_count,
        (float)pp->total_fatal_error_count / pp->total_transaction_count * 100);
}

void cli_init(void) {
    config                     = embeddedCliDefaultConfig();
    config->cliBuffer          = cliBuffer;
    config->cliBufferSize      = CLI_BUFFER_SIZE;
    config->enableAutoComplete = false;
    config->historyBufferSize  = 64;
    config->maxBindingCount    = 16;
    cli                        = embeddedCliNew(config);
    cli->writeChar             = writeChar;
    embeddedCliAddBinding(
        cli, (CliCommandBinding){"version", "Show version", false, NULL,
                                 onVersion});
    embeddedCliAddBinding(
        cli, (CliCommandBinding){"piousb", "Pio usb status", false, NULL,
                                 onPioUsbStatus});
    embeddedCliAddBinding(cli, (CliCommandBinding){"dfu", "jump to bootloader",
                                                   false, NULL, onDfu});
    embeddedCliAddBinding(cli, (CliCommandBinding){"reset", "soft reset",
                                                   false, NULL, onReset});
    embeddedCliAddBinding(
        cli, (CliCommandBinding){"hoststat", "show downstream host diagnostics",
                                 false, NULL, onHostStat});
    embeddedCliAddBinding(
        cli, (CliCommandBinding){"debug", "toggle debug option", false, NULL,
                                 onDebug});
    cli_initialized = true;
}

void cli_exec(void) {
    if (cli_initialized) embeddedCliProcess(cli);
    
    tusb_print_debug_buffer();
}