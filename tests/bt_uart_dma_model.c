/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define WORD(address) (*(volatile uint32_t *)(uintptr_t)(address))
#define BYTE(address) (*(volatile uint8_t *)(uintptr_t)(address))
#define DESCRIPTOR 0x80124000u

void bt_cs_reset_dma_descriptors(void);
int bt_cs_dma_descriptors_ready(void);
void bt_cs_dma_type4_irq(void);
void bt_cs_dma_type2_irq(void);
void bt_cs_dma_type3_irq(void);

typedef void (*dispatch_t)(uint32_t, uint16_t, uint8_t);
static uint32_t dispatch_packet, dispatch_type, calls;

static void receive(uint32_t packet, uint16_t length, uint8_t type)
{
    assert(length == 0);
    dispatch_packet = packet;
    dispatch_type = type;
    ++calls;
}

static void map_ram(uintptr_t addr, size_t size)
{
    void *area = mmap((void *)addr, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(area == (void *)addr);
}

int main(void)
{
    map_ram(0x80100000u, 0x30000);
    map_ram(0xb4000000u, 0x1000);
    WORD(0x80121320u) = DESCRIPTOR;
    WORD(DESCRIPTOR + 4u) = 0xa5a5a5a5u;
    WORD(DESCRIPTOR + 8u) = 0xffffffffu;
    memset((void *)(uintptr_t)0x801088a7u, 0x5a, 0xce);
    bt_cs_reset_dma_descriptors();
    assert(WORD(0x80127bbcu) == 0x801088a8u);
    assert(WORD(0x80121244u) == DESCRIPTOR && WORD(0x801213fcu) == DESCRIPTOR);
    assert(WORD(0xb4000004u) == 0x4000u && WORD(0xb400000cu) == 0x4000u);
    assert(WORD(0xb4000014u) == 0x4000u);
    assert(WORD(DESCRIPTOR + 4u) == 0xa5a5a5a5u && WORD(DESCRIPTOR + 8u) == 0u);
    assert(BYTE(0x801088a7u) == 0x5a && BYTE(0x80108974u) == 0x5a);
    for (unsigned int i = 0; i < 0xccu; ++i)
        assert(BYTE(0x801088a8u + i) == 0);

    *(dispatch_t volatile *)(uintptr_t)0x80121330u = receive;
    assert(bt_cs_dma_descriptors_ready());
    BYTE(DESCRIPTOR + 3u) = 0x80;
    assert(!bt_cs_dma_descriptors_ready());
    bt_cs_dma_type4_irq();
    assert(WORD(0xb4000050u) == 8 && calls == 0);
    bt_cs_dma_type2_irq();
    assert(WORD(0xb4000050u) == 16 && calls == 0);
    bt_cs_dma_type3_irq();
    assert(WORD(0xb4000050u) == 32 && calls == 0);
    BYTE(DESCRIPTOR + 3u) = 0;

    WORD(0x80121274u) = 0x80101110u;
    bt_cs_dma_type4_irq();
    assert(calls == 1 && dispatch_type == 4 && dispatch_packet == 0x80101110u);
    WORD(0x80121298u) = 0x80101220u;
    bt_cs_dma_type2_irq();
    assert(calls == 2 && dispatch_type == 2 && dispatch_packet == 0x80101220u);
    WORD(0x80121298u) = 0x86101220u;
    bt_cs_dma_type2_irq();
    assert(calls == 3 && dispatch_type == 5 && dispatch_packet == 0x80101220u);
    WORD(0x801212dcu) = 0x80101330u;
    bt_cs_dma_type3_irq();
    assert(calls == 4 && dispatch_type == 3 && dispatch_packet == 0x80101330u);
    assert(WORD(0x801212dcu) == 0);
    return 0;
}
