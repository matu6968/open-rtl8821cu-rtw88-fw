/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

int bt_read_efuse_bdaddr(int (*read)(uint16_t, uint8_t *), uint16_t limit, uint16_t address[3]);
unsigned int bt_efuse_rx_mode(uint8_t field);

static const uint8_t *raw;
static size_t raw_size;

static int read_factory(uint16_t offset, uint8_t *value)
{
    if (offset >= raw_size)
        return 0;
    *value = raw[offset];
    return 1;
}

int main(void)
{
    static const uint8_t address_records[] = {
        0x1e, 0xff, 0xff,            /* Another section. */
        0x83, 0x01, 0x02, 0x03, 0x04,
        0x2f, 0x1e, 0xb0, 0x06,     /* Extended section 9. */
        0xff,
    };
    uint16_t address[] = {0xaaaa, 0xbbbb, 0xcccc};
    raw = address_records;
    raw_size = sizeof(address_records);
    assert(!bt_read_efuse_bdaddr(read_factory, 4, address));
    assert(address[0] == 0xaaaa && address[1] == 0xbbbb && address[2] == 0xcccc);
    assert(bt_read_efuse_bdaddr(read_factory, 6, address));
    assert(address[0] == 0x0201 && address[1] == 0x4c23 && address[2] == 0x00e0);
    assert(bt_read_efuse_bdaddr(read_factory, raw_size, address));
    assert(address[0] == 0x0201 && address[1] == 0x0403 && address[2] == 0x06b0);

    raw_size = 7;
    assert(!bt_read_efuse_bdaddr(read_factory, sizeof(address_records), address));
    assert(address[0] == 0x0201 && address[1] == 0x0403 && address[2] == 0x06b0);

    assert(bt_efuse_rx_mode(0x36) == 1);
    assert(bt_efuse_rx_mode(0x35) == 2);
    assert(!bt_efuse_rx_mode(0x0d) && !bt_efuse_rx_mode(0x0e) && !bt_efuse_rx_mode(0xff));
    return 0;
}
