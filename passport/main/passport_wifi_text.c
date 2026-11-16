#include "passport_wifi.h"
#include <stdio.h>

const char *passport_wifi_security_name(passport_wifi_security_t value)
{
    return value == PW_OPEN ? "Open" : value == PW_WPA2 ? "WPA2" : "Unsupported";
}
const char *passport_wifi_phase_name(passport_wifi_phase_t value)
{
    return (const char *const[]){"Offline", "Switching network", "Connecting", "Associated / no IP", "LAN IP assigned"}[value];
}
void passport_wifi_ssid_text(const passport_ssid_t *ssid, char *out, size_t capacity)
{
    bool printable = ssid->length != 0;
    for (unsigned i = 0; i < ssid->length; ++i) printable &= ssid->bytes[i] >= 32 && ssid->bytes[i] <= 126;
    if (!ssid->length) { snprintf(out, capacity, "Hidden SSID"); return; }
    if (printable) { snprintf(out, capacity, "%.*s", ssid->length, ssid->bytes); return; }
    size_t used = snprintf(out, capacity, "hex:");
    for (unsigned i = 0; i < ssid->length && used + 2 < capacity; ++i) used += snprintf(out + used, capacity - used, "%02X", ssid->bytes[i]);
}
