// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Compile the actual pinned SDK serial/device-descriptor implementation. */
#include "../../firmware/managed_components/espressif__esp_tinyusb/usb_descriptors.c"

static bool mac_failure;
esp_err_t esp_read_mac(uint8_t *address, esp_mac_type_t type)
{
    assert(type == ESP_MAC_BASE);
    const uint8_t identifier[] = {0x02, 0x13, 0x24, 0x35, 0x46, 0x57};
    memcpy(address, identifier, sizeof(identifier));
    return mac_failure ? ESP_FAIL : ESP_OK;
}

int main(void)
{
    tinyusb_desc_serial_number_init();
    assert(strcmp(descriptor_str_default[3], "021324354657") == 0);
    assert(descriptor_dev_default.idVendor == 0x303a && descriptor_dev_default.idProduct == 0x4005);
    assert(descriptor_dev_default.bDeviceClass == TUSB_CLASS_MISC);
    assert(descriptor_dev_default.bDeviceSubClass == MISC_SUBCLASS_COMMON);
    assert(descriptor_dev_default.bDeviceProtocol == MISC_PROTOCOL_IAD);
    assert(descriptor_dev_default.bMaxPacketSize0 == 64);
    assert(descriptor_qualifier_default.bDeviceClass == descriptor_dev_default.bDeviceClass);
    assert(descriptor_qualifier_default.bMaxPacketSize0 == 64);
    mac_failure = true;
    tinyusb_desc_serial_number_init();
    assert(descriptor_str_default[3][0] == '\0'); /* No fabricated fixed identity on failure. */
    puts("Pinned USB SDK: empty serial selects chip identity, composite PID/IAD and qualifier verified");
    return 0;
}
