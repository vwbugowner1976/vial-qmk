// Copyright 2023 sekigon-gonnoc
// SPDX-License-Identifier: GPL-2.0-or-later

#include QMK_KEYBOARD_H

#include "print.h"

#include "tusb.h"
#include "pio_usb_ll.h"
#include "report_descriptor_parser.h"
#include "report_parser.h"

matrix_row_t*           matrix_dest;
bool                    mouse_send_flag = false;
static uint8_t          kbd_addr;
static uint8_t          kbd_instance;
static int32_t          led_count = -1;
static uint8_t          hid_report_buffer[64];
static volatile uint8_t hid_report_size;
static uint8_t          hid_instance;
static bool             hid_disconnect_flag;
static uint8_t          pre_keyreport[8];

// --- Hot-plug diagnostics (log-only; never resets anything) ---
// The auto-recovery watchdog in cf4fbde misfired on real hardware: the
// late-plug recovery never engaged and normal typing lost keystrokes
// (e.g. "nn" for Japanese ん). These counters record the host-side state
// transitions that watchdog acted on, so the actual behaviour can be
// observed first. Read them out with the CLI "hoststat" command.
// Per-instance state is kept as a bitmask because a single bool cannot
// represent composite devices with several HID interfaces.
volatile uint32_t kq_dbg_dev_mount_count;
volatile uint32_t kq_dbg_hid_mount_count;
volatile uint32_t kq_dbg_hid_umount_count;
volatile uint32_t kq_dbg_would_stage1_count;
volatile uint32_t kq_dbg_would_stage2_count;
volatile uint32_t kq_dbg_longest_unmounted_ms;
volatile uint32_t kq_dbg_last_stage1_ms;
volatile uint32_t kq_dbg_last_stage2_ms;
volatile uint32_t kq_dbg_mounted_mask;  // bit ((dev_addr * 16 + instance) & 31)

// Stuck-key-after-unplug diagnostics (2026-10-25): on real hardware, a
// key (the half/full-width key, KC_GRV = 0x35) can stay "pressed" on the
// PC side after the downstream keyboard is unplugged. That happens if
// the unplug is never turned into a matrix clear, i.e. the umount
// callback does not run. The counters below record, at the moment the
// electrical connection drops, whether anything was still mounted and
// whether the matrix still held keys, and whether keys are STILL in
// the matrix 1 s after the unplug (no device is left to release them).
volatile uint32_t kq_dbg_unplug_count;
volatile uint32_t kq_dbg_unplug_with_mount_count;
volatile uint32_t kq_dbg_unplug_with_keys_count;
volatile uint32_t kq_dbg_unplug_grv_count;
volatile uint32_t kq_dbg_stuck_after_unplug_count;
volatile uint32_t kq_dbg_last_unplug_ms;
volatile uint32_t kq_dbg_last_stuck_ms;
// The user did NOT unplug while holding a key (confirmed 2026-10-25),
// so a stuck GRV cannot be explained by a release that the device
// never got to send. These counters track GRV transitions as seen in
// the reports that actually arrived: if press and release counts
// differ while the device was still connected, a release report was
// lost inside the host/pipeline before the unplug. If they match and
// the matrix is empty at unplug time, the "stuck" impression is not
// coming from this matrix at all (e.g. PC/IME-side state).
volatile uint32_t kq_dbg_hid_report_count;
volatile uint32_t kq_dbg_grv_press_count;
volatile uint32_t kq_dbg_grv_release_count;
volatile uint32_t kq_dbg_last_grv_change_ms;
volatile uint8_t  kq_dbg_last_kbd_bits[32];  // last keyboard report (HID usage bitmap)
static volatile matrix_row_t kq_dbg_matrix_snapshot[MATRIX_ROWS];

uint32_t kq_dbg_connected_now(void) {
    return PIO_USB_ROOT_PORT(0)->connected ? 1 : 0;
}

uint32_t kq_dbg_line_state(void) {
    // Raw D+/D- state of the root port right now
    // (PORT_PIN_SE0=0, FS_IDLE=1, LS_IDLE=2, SE1=3; pio_usb_ll.h).
    // In a phantom-connected state with nothing plugged in, this
    // should read SE0 while ->connected stays true.
    return (uint32_t)pio_usb_bus_get_line_state(PIO_USB_ROOT_PORT(0));
}

uint32_t kq_dbg_suspended(void) {
    // While suspended is true, the PIO-USB frame loop skips its
    // disconnect check entirely (pio_usb_host.c connection_check is
    // the last operand of the SOF condition), so a stale connected
    // flag can persist.
    return PIO_USB_ROOT_PORT(0)->suspended ? 1 : 0;
}

uint32_t kq_dbg_last_report_nonzero(void) {
    for (int i = 0; i < (int)sizeof(kq_dbg_last_kbd_bits); i++) {
        if (kq_dbg_last_kbd_bits[i] != 0) return 1;
    }
    return 0;
}

uint32_t kq_dbg_last_report_grv(void) {
    // KC_GRV (0x35): bitmap byte 0x35>>3, bit 0x35&7
    return (kq_dbg_last_kbd_bits[0x35 >> 3] >> (0x35 & 7)) & 1;
}

uint32_t kq_dbg_matrix_nonzero_now(void) {
    for (int i = 0; i < MATRIX_ROWS; i++) {
        if (kq_dbg_matrix_snapshot[i] != 0) return 1;
    }
    return 0;
}

uint32_t kq_dbg_matrix_grv_now(void) {
    // keyboard_report_hook maps report byte idx to matrix row idx+1,
    // so KC_GRV lands on row (0x35>>3)+1, col 0x35&7.
    return (kq_dbg_matrix_snapshot[(0x35 >> 3) + 1] >> (0x35 & 7)) & 1;
}

#define LED_BLINK_TIME_MS 50
#define KQ_PIN_LED 7
#define MATRIX_MSBTN_ROW 22

extern void busy_wait_us(uint64_t delay_us);
static bool send_led_report(uint8_t* leds);

void matrix_init_custom(void) {
    // Configure LED pin
    setPinOutput(KQ_PIN_LED);
    writePinHigh(KQ_PIN_LED);
}

bool matrix_scan_custom(matrix_row_t current_matrix[]) {
    bool matrix_has_changed = false;

    // If keyboard is disconnected, clear matrix
    if (hid_disconnect_flag) {
        for (uint8_t rowIdx = 0; rowIdx < MATRIX_ROWS; rowIdx++) {
            if (current_matrix[rowIdx] != 0) {
                matrix_has_changed     = true;
                current_matrix[rowIdx] = 0;
            }
            kq_dbg_matrix_snapshot[rowIdx] = current_matrix[rowIdx];
        }

        hid_disconnect_flag = false;

        return matrix_has_changed;
    }

    // If keyboard report is received, apply it to matrix
    if (hid_report_size > 0) {
        matrix_dest        = current_matrix;
        matrix_has_changed = parse_report(hid_instance, hid_report_buffer, hid_report_size);
        hid_report_size    = 0;
        return matrix_has_changed;
    } else {
        return false;
    }

    return matrix_has_changed;
}

void housekeeping_task_kb(void) {
    // Control keyboard indicator LED
    static uint8_t keyboard_led;
    if (keyboard_led != host_keyboard_leds()) {
        uint8_t led_backup = keyboard_led;
        keyboard_led       = host_keyboard_leds();
        if (!send_led_report(&keyboard_led)) {
            keyboard_led = led_backup;
        }
    }

    // Blink LED when USB reports are received
    if (led_count >= 0) {
        if (timer_elapsed(led_count) < LED_BLINK_TIME_MS) {
            writePinLow(KQ_PIN_LED);
        } else if (timer_elapsed(led_count) < 2 * LED_BLINK_TIME_MS) {
            writePinHigh(KQ_PIN_LED);
        } else {
            led_count = -1;
        }
    }

    // Hot-plug diagnostics: observe only, never intervene. Tracks how
    // long a device stays electrically connected with no HID interface
    // mounted, and counts the moments the cf4fbde watchdog WOULD have
    // fired its stage 1 (root port reset, 3 s) and stage 2 (soft reset,
    // 6 s). An episode re-arms when something mounts or the device is
    // unplugged, mirroring the watchdog's logic.
    {
        static uint32_t unmounted_since = 0;
        static uint8_t  episode_stage   = 0;
        static bool     prev_connected  = false;
        static uint32_t unplug_time     = 0;
        static bool     stuck_armed     = false;

        bool connected   = PIO_USB_ROOT_PORT(0)->connected;
        bool any_mounted = (kq_dbg_mounted_mask != 0);

        if (connected != prev_connected) {
            dprintf("KQDBG connected -> %u (t=%u)\n", connected ? 1 : 0, (unsigned)timer_read32());
            if (!connected && prev_connected) {
                // Electrical unplug detected. Record what was still
                // alive at this moment: if an interface is still marked
                // mounted, the umount callback has not run (yet), and
                // if the matrix still holds keys, the PC keeps seeing
                // them as pressed until something clears the matrix.
                kq_dbg_unplug_count++;
                kq_dbg_last_unplug_ms = timer_read32();
                if (any_mounted) kq_dbg_unplug_with_mount_count++;
                if (kq_dbg_matrix_nonzero_now()) kq_dbg_unplug_with_keys_count++;
                if (kq_dbg_matrix_grv_now()) kq_dbg_unplug_grv_count++;
                dprintf("KQDBG unplug: mask=0x%x matrix_nonzero=%u matrix_grv=%u last_report_nonzero=%u (t=%u)\n", (unsigned)kq_dbg_mounted_mask, (unsigned)kq_dbg_matrix_nonzero_now(), (unsigned)kq_dbg_matrix_grv_now(), (unsigned)kq_dbg_last_report_nonzero(), (unsigned)kq_dbg_last_unplug_ms);
                unplug_time = kq_dbg_last_unplug_ms;
                stuck_armed = true;
            }
            if (connected) {
                stuck_armed = false;
            }
            prev_connected = connected;
        }

        // 1 s after an unplug, a device that is gone can no longer send
        // a release report, so any key still in the matrix at that point
        // is stuck on the PC side unless an umount clear ran meanwhile.
        if (stuck_armed && !connected && timer_elapsed32(unplug_time) >= 1000) {
            if (kq_dbg_matrix_nonzero_now()) {
                kq_dbg_stuck_after_unplug_count++;
                kq_dbg_last_stuck_ms = timer_read32();
                dprintf("KQDBG stuck after unplug: matrix still nonzero 1s after unplug, grv=%u (t=%u)\n", (unsigned)kq_dbg_matrix_grv_now(), (unsigned)kq_dbg_last_stuck_ms);
            }
            stuck_armed = false;
        }

        if (connected && !any_mounted) {
            if (unmounted_since == 0) {
                unmounted_since = timer_read32();
                if (unmounted_since == 0) unmounted_since = 1;
                episode_stage = 0;
            } else {
                uint32_t gap = timer_elapsed32(unmounted_since);
                if (gap > kq_dbg_longest_unmounted_ms) {
                    kq_dbg_longest_unmounted_ms = gap;
                }
                if (episode_stage == 0 && gap >= 3000) {
                    kq_dbg_would_stage1_count++;
                    kq_dbg_last_stage1_ms = timer_read32();
                    episode_stage         = 1;
                    dprintf("KQDBG would-be stage1 (port reset) at gap=%u ms\n", (unsigned)gap);
                } else if (episode_stage == 1 && gap >= 6000) {
                    kq_dbg_would_stage2_count++;
                    kq_dbg_last_stage2_ms = timer_read32();
                    episode_stage         = 2;
                    dprintf("KQDBG would-be stage2 (soft reset) at gap=%u ms\n", (unsigned)gap);
                }
            }
        } else {
            unmounted_since = 0;
            episode_stage   = 0;
        }
    }

    housekeeping_task_user();
}

static bool send_led_report(uint8_t* leds) {
    if (kbd_addr != 0) {
        return tuh_hid_set_report(kbd_addr, kbd_instance, 0, HID_REPORT_TYPE_OUTPUT, leds, sizeof(*leds));
    }

    return false;
}

static volatile bool set_protocol_complete = false;
void                 tuh_hid_set_protocol_complete_cb(uint8_t dev_addr, uint8_t instance, uint8_t protocol) {
    set_protocol_complete = true;
}

void tuh_mount_cb(uint8_t dev_addr) {
    kq_dbg_dev_mount_count++;
    dprintf("USB device is mounted:%d\n", dev_addr);

    if (led_count < 0) {
        led_count = timer_read();
    }

    for (int instance = 0; instance < tuh_hid_instance_count(dev_addr); instance++) {
        uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
        if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
            kbd_addr     = dev_addr;
            kbd_instance = instance;
        }
    }
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len) {
    kq_dbg_hid_mount_count++;
    kq_dbg_mounted_mask |= (1u << (((dev_addr * 16) + instance) & 31));
    dprintf("HID is mounted:%d:%d\n", dev_addr, instance);
    parse_report_descriptor((dev_addr * 16) + instance, desc_report, desc_len);
    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    kq_dbg_hid_umount_count++;
    kq_dbg_mounted_mask &= ~(1u << (((dev_addr * 16) + instance) & 31));
    dprintf("HID unmounted:%d:%d\n", dev_addr, instance);
    memset(pre_keyreport, 0, sizeof(pre_keyreport));
    hid_disconnect_flag = true;
    kbd_addr            = 0;
    kbd_instance        = 0;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len) {
    if (len > 0) kq_dbg_hid_report_count++;
    dprintf("Report received\n");
    if (led_count < 0) {
        led_count = timer_read();
    }

    if (len > 0) {
        int cnt = 0;
        while (hid_report_size > 0 && cnt++ < 50000) {
            busy_wait_us(1);
            continue;
        }

        hid_instance = (dev_addr * 16) + instance;
        memcpy(hid_report_buffer, report, len);
        __compiler_memory_barrier();
        // hid_report_size is used as trigger of report parser
        hid_report_size = len;
    }

    tuh_hid_receive_report(dev_addr, instance);
}

__attribute__((weak)) void keyboard_report_hook(keyboard_parse_result_t const* report) {
    // Diagnostics: keep the latest keyboard report and the matrix state
    // it produced, so hoststat can show what was held at unplug time.
    memcpy((void*)kq_dbg_last_kbd_bits, report->bits, sizeof(kq_dbg_last_kbd_bits));

    {
        static bool prev_grv = false;
        bool        grv      = ((report->bits[0x35 >> 3] >> (0x35 & 7)) & 1) != 0;
        if (grv != prev_grv) {
            if (grv) {
                kq_dbg_grv_press_count++;
            } else {
                kq_dbg_grv_release_count++;
            }
            kq_dbg_last_grv_change_ms = timer_read32();
            dprintf("KQDBG GRV -> %u (t=%u, press=%u release=%u)\n", grv ? 1 : 0, (unsigned)kq_dbg_last_grv_change_ms, (unsigned)kq_dbg_grv_press_count, (unsigned)kq_dbg_grv_release_count);
            prev_grv = grv;
        }
    }

    if (debug_enable) {
        uprintf("Keyboard report\n");
        for (int idx = 0; idx < sizeof(report->bits); idx++) {
            uprintf("%02X ", report->bits[idx]);
        }
        uprintf("\n");
    }

    for (uint8_t rowIdx = 0; rowIdx < MATRIX_ROWS - 1; rowIdx++) {
        matrix_dest[rowIdx + 1] = report->bits[rowIdx];
    }

    // copy modifier bits
    matrix_dest[0] = report->bits[28];
    matrix_dest[29] = 0;

    for (uint8_t rowIdx = 0; rowIdx < MATRIX_ROWS; rowIdx++) {
        kq_dbg_matrix_snapshot[rowIdx] = matrix_dest[rowIdx];
    }
}

__attribute__((weak)) void mouse_report_hook(mouse_parse_result_t const* report) {
    if (debug_enable) {
        uprintf("Mouse report\n");
        uprintf("b:%d ", report->button);
        uprintf("x:%d ", report->x);
        uprintf("y:%d ", report->y);
        uprintf("v:%d ", report->v);
        uprintf("h:%d ", report->h);
        uprintf("undef:%u\n", report->undefined);
    }

    mouse_send_flag = true;

    report_mouse_t mouse = pointing_device_get_report();

    mouse.buttons = report->button;

    mouse.x += report->x;
    mouse.y += report->y;
    mouse.v += report->v;
    mouse.h += report->h;

    pointing_device_set_report(mouse);
}

bool pointing_device_task(void) {
    if (mouse_send_flag) {
        bool send_report = pointing_device_send();
        mouse_send_flag = false;
        return send_report;
    }

    return false;
}

void vendor_report_parser(uint16_t usage_id, hid_report_member_t const* member, uint8_t const* data, uint8_t len) {
    // For Lenovo thinkpad keyboard(17ef:6047)
    // TODO: restriction by VID:PID
    if (usage_id == 0xFFA1) {
        mouse_parse_result_t mouse = {0};
        mouse.h                    = (data[0] & 0x80 ? 0xFF00 : 0) | data[0];
        mouse_report_hook(&mouse);
    }
}

__attribute__((weak)) void system_report_hook(uint16_t report) {
    host_system_send(report);
    wait_ms(TAP_CODE_DELAY);
    host_system_send(0);
}

__attribute__((weak)) void consumer_report_hook(uint16_t report) {
    host_consumer_send(report);
    wait_ms(TAP_CODE_DELAY);
    host_consumer_send(0);
}
