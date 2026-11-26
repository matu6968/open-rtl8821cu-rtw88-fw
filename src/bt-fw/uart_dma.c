/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>

#define REG32(address) (*(volatile uint32_t *)(uintptr_t)(address))
#define REG8(address) (*(volatile uint8_t *)(uintptr_t)(address))

#define DMA_IRQ_STATUS 0xb4000050u
#define DMA_DESCRIPTOR_TYPE4 0x80121244u
#define DMA_DESCRIPTOR_TYPE2 0x80121320u
#define DMA_DESCRIPTOR_TYPE3 0x801213fcu
#define DMA_DESCRIPTOR_OWNED 0x80u
#define DMA_PACKET_TYPE4 0x80121274u
#define DMA_PACKET_TYPE2 0x80121298u
#define DMA_PACKET_TYPE3 0x801212dcu
#define DMA_SCRATCH_POINTER 0x80127bbcu
#define DMA_SCRATCH 0x801088a8u
#define DMA_SCRATCH_BYTES 0xccu

typedef void (*dma_dispatch_t)(uint32_t, uint16_t, uint8_t);
#define ROM_DMA_DISPATCH (*(dma_dispatch_t volatile *)(uintptr_t)0x80121330u)

/* Stock CS 0x8010f42c. The three receive paths share the type 2 descriptor. */
void bt_cs_reset_dma_descriptors(void)
{
    REG32(DMA_SCRATCH_POINTER) = DMA_SCRATCH;
    for (unsigned int i = 0; i < DMA_SCRATCH_BYTES; ++i)
        REG8(DMA_SCRATCH + i) = 0;

    uint32_t descriptor = REG32(DMA_DESCRIPTOR_TYPE2);
    REG32(DMA_DESCRIPTOR_TYPE3) = descriptor;
    REG32(DMA_DESCRIPTOR_TYPE4) = descriptor;
    REG32(0xb4000004u) = descriptor & 0xffffu;
    REG32(0xb400000cu) = descriptor & 0xffffu;
    REG32(0xb4000014u) = descriptor & 0xffffu;
    REG32(descriptor + 8u) = 0;
}

static int dma_descriptor_ready(uint32_t descriptor_slot)
{
    uint32_t descriptor = REG32(descriptor_slot);
    return !(REG8(descriptor + 3u) & DMA_DESCRIPTOR_OWNED);
}

/* Stock CS 0x8010cb8c checks ownership in this order. */
int bt_cs_dma_descriptors_ready(void)
{
    return dma_descriptor_ready(DMA_DESCRIPTOR_TYPE2) &&
           dma_descriptor_ready(DMA_DESCRIPTOR_TYPE4) &&
           dma_descriptor_ready(DMA_DESCRIPTOR_TYPE3);
}

/* Stock CS 0x8010cf80. */
void bt_cs_dma_type4_irq(void)
{
    REG32(DMA_IRQ_STATUS) = 8u;
    if (dma_descriptor_ready(DMA_DESCRIPTOR_TYPE4))
        ROM_DMA_DISPATCH(REG32(DMA_PACKET_TYPE4), 0, 4);
}

/* Stock CS 0x8010cfc0. Type 5 shares the type 2 receive path. */
void bt_cs_dma_type2_irq(void)
{
    REG32(DMA_IRQ_STATUS) = 16u;
    if (dma_descriptor_ready(DMA_DESCRIPTOR_TYPE2)) {
        uint32_t packet = REG32(DMA_PACKET_TYPE2);
        uint8_t type = 2;
        if ((packet & 0x0f000000u) == 0x06000000u) {
            packet &= ~0x06000000u;
            type = 5;
        }
        ROM_DMA_DISPATCH(packet, 0, type);
    }
}

/* Stock CS 0x8010d01c. Clear the packet slot before forwarding it. */
void bt_cs_dma_type3_irq(void)
{
    REG32(DMA_IRQ_STATUS) = 32u;
    if (dma_descriptor_ready(DMA_DESCRIPTOR_TYPE3)) {
        uint32_t packet = REG32(DMA_PACKET_TYPE3);
        REG32(DMA_PACKET_TYPE3) = 0;
        ROM_DMA_DISPATCH(packet, 0, 3);
    }
}
