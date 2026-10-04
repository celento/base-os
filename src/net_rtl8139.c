/* Small polled RTL8139 (legacy 8139 mode) driver, written for BaseOS.
 * Hardware behavior checked against QEMU hw/net/rtl8139.c (MIT); see
 * docs/NETWORK.md. No interrupts, C+ descriptors, or physical-hardware claim. */
#include "net_driver.h"
#include "platform.h"
static uint16_t io;
static unsigned rx_offset, tx_next;
static uint8_t *rx_buffer, *tx_buffer;
static inline void out8(uint16_t p,uint8_t v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p):"memory");}
static inline void out16(uint16_t p,uint16_t v){__asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p):"memory");}
static inline void out32(uint16_t p,uint32_t v){__asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p):"memory");}
static inline uint8_t in8(uint16_t p){uint8_t v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p):"memory");return v;}
static inline uint16_t in16(uint16_t p){uint16_t v;__asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p):"memory");return v;}
static inline uint32_t in32(uint16_t p){uint32_t v;__asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p):"memory");return v;}
static uint32_t pci_read(unsigned bus,unsigned device,unsigned function,unsigned reg){
    out32(0xcf8,0x80000000u|(bus<<16)|(device<<11)|(function<<8)|(reg&0xfc));return in32(0xcfc);
}
static void pci_command(unsigned bus,unsigned device,unsigned function,uint16_t value){
    out32(0xcf8,0x80000004u|(bus<<16)|(device<<11)|(function<<8));out16(0xcfc,value);
}
static void receive_reset(void){
    out8(io+0x37,4);rx_offset=0;
    out32(io+0x30,(uint32_t)(uintptr_t)rx_buffer);out16(io+0x38,0xfff0);
    out8(io+0x37,12);
    /* 8KB ring; unlimited DMA burst, no WRAP bit so frames wrap in-ring.
     * Accept broadcasts and our MAC only. No malformed/runt acceptance. */
    out32(io+0x44,(7u<<8)|0x0a);out16(io+0x3e,0xffff);
}
int net_driver_init(uint8_t *rx,uint8_t *tx,uint8_t mac[6]){
    io=0;
    for(unsigned bus=0;bus<256&&!io;bus++)for(unsigned dev=0;dev<32&&!io;dev++){
        uint32_t first=pci_read(bus,dev,0,0);if(first==0xffffffffu)continue;
        unsigned functions=(pci_read(bus,dev,0,12)&0x800000)?8:1;
        for(unsigned fun=0;fun<functions&&!io;fun++){
            if(pci_read(bus,dev,fun,0)!=0x813910ecu)continue;
            uint32_t bar=pci_read(bus,dev,fun,16);if(!(bar&1)||(bar&~3u)>0xff00u||!(bar&~3u))continue;
            io=(uint16_t)(bar&~3u);pci_command(bus,dev,fun,(uint16_t)pci_read(bus,dev,fun,4)|5);
        }
    }
    if(!io)return 0;
    rx_buffer=rx;tx_buffer=tx;rx_offset=tx_next=0;
    for(unsigned i=0;i<NET_RX_MEMORY;i++)rx[i]=0;
    for(unsigned i=0;i<NET_TX_MEMORY;i++)tx[i]=0;
    out8(io+0x52,0);out8(io+0x37,0x10);
    unsigned guard=1000000;while((in8(io+0x37)&0x10)&&--guard){}
    if(!guard){io=0;return 0;}
    for(unsigned i=0;i<6;i++)mac[i]=in8(io+i);
    out16(io+0x3c,0);out16(io+0x3e,0xffff);
    for(unsigned i=0;i<4;i++)out32(io+0x20+i*4,(uint32_t)(uintptr_t)(tx+i*1536));
    out32(io+0x40,(3u<<24)|(7u<<8));receive_reset();
    return 1;
}
int net_driver_link(void){return io&&!(in8(io+0x58)&4);}
int net_driver_send(const uint8_t *frame,unsigned length){
    if(!io||length>NET_FRAME_MAX||length<14)return -1;
    for(unsigned count=0;count<4;count++){
        unsigned slot=(tx_next+count)&3;uint32_t status=in32(io+0x10+slot*4);
        if(!(status&0x2000))continue;
        uint8_t *target=tx_buffer+slot*1536;
        unsigned padded=length<60?60:length;
        for(unsigned i=0;i<padded;i++)target[i]=i<length?frame[i]:0;
        __asm__ volatile("":::"memory");
        out32(io+0x10+slot*4,(8u<<16)|padded);tx_next=(slot+1)&3;return 1;
    }
    return 0;
}
static uint8_t ring_byte(unsigned n){return ((volatile uint8_t *)rx_buffer)[n&(NET_RX_RING-1)];}
int net_driver_recv(uint8_t *frame,unsigned capacity){
    if(!io)return 0;
    uint16_t interrupts=in16(io+0x3e);
    if(interrupts&(0x10|0x40)){receive_reset();return -1;}
    if(in8(io+0x37)&1){if(interrupts)out16(io+0x3e,interrupts);return 0;}
    unsigned status=ring_byte(rx_offset)|(unsigned)ring_byte(rx_offset+1)<<8;
    unsigned length=ring_byte(rx_offset+2)|(unsigned)ring_byte(rx_offset+3)<<8;
    if(length==0xfff0)return 0; /* DMA has not completed yet. */
    if(!(status&1)||(status&0x3e)||length<18||length>NET_FRAME_MAX+4){receive_reset();return -1;}
    unsigned count=length-4; /* Strip NIC-supplied FCS. */
    if(count<=capacity)for(unsigned i=0;i<count;i++)frame[i]=ring_byte(rx_offset+4+i);
    rx_offset=(rx_offset+length+4+3)&~3u;rx_offset&=NET_RX_RING-1;
    out16(io+0x38,(uint16_t)(rx_offset-16));
    if(interrupts)out16(io+0x3e,interrupts);
    return count<=capacity?(int)count:-1;
}
