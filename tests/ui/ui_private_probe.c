// SPDX-License-Identifier: Apache-2.0
/* Compile the unchanged production module in this translation unit solely so
 * tests can observe its actual static draft bytes. No behavior is substituted,
 * reset, patched or conditionalized. LVGL remains the real managed library. */
#include "../../firmware/main/app_ui.c"
#include "ui_private_probe.h"

static bool probe_contains(const void *memory, size_t size, const char *value)
{
    size_t length = strlen(value);
    const unsigned char *bytes = memory;
    if (!length || length > size) return false;
    for (size_t offset = 0; offset <= size - length; ++offset)
        if (memcmp(bytes + offset, value, length) == 0) return true;
    return false;
}

static bool probe_zero(const void *memory, size_t size)
{
    const unsigned char *bytes = memory;
    for (size_t i = 0; i < size; ++i) if (bytes[i]) return false;
    return true;
}

bool ui_private_password_contains(const char *value)
{
    return probe_contains(s_wifi_password, sizeof(s_wifi_password), value);
}

bool ui_private_buffers_cleared(void)
{
    return probe_zero(s_wifi_password, sizeof(s_wifi_password)) &&
        probe_zero(s_bluetooth_name, sizeof(s_bluetooth_name));
}

bool ui_private_password_tail_cleared(void)
{
    for (size_t i = 0; i < sizeof(s_wifi_password); ++i) {
        if (!s_wifi_password[i]) return probe_zero(s_wifi_password + i, sizeof(s_wifi_password) - i);
    }
    return false;
}

bool ui_private_public_state_contains(const char *value)
{
    return probe_contains(&s_view, sizeof(s_view), value) ||
        probe_contains(s_message_text, sizeof(s_message_text), value) ||
        probe_contains(s_target_text, sizeof(s_target_text), value);
}
