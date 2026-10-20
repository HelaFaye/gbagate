/*
 * global.h -- what pret's decompiled librfu (src/librfu_*.c, include/librfu.h)
 * expects from its game's headers, for building it on a host against the GBA
 * hardware model in ../sim_rfu.c. Register names and bit values are the GBA's
 * (GBATEK); the registers themselves are memory in gba_io[], which the model
 * watches and updates the way the hardware would.
 */
#ifndef SIM_GBA_GLOBAL_H
#define SIM_GBA_GLOBAL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
/* librfu is built as C++ so that its volatile types can be register proxies:
 * a read or write that lands in gba_io[] goes through the hardware model at
 * once, as on the GBA, where writing TMxCNT_H restarts a timer and writing IF
 * acknowledges an interrupt -- effects a model that only watched the memory
 * would miss when two writes come back to back. Anywhere else (librfu's own
 * volatile fields) the proxy is plain memory. */
#ifdef __cplusplus
extern "C" uint32_t sim_gba_io_read(uintptr_t addr, unsigned size, uint32_t stored);
extern "C" void     sim_gba_io_write(uintptr_t addr, unsigned size, uint32_t value);

template <typename T>
struct GbaReg {
    T v;
    static bool io(const volatile void *p);
    T get() const volatile {
        return io(this) ? (T)sim_gba_io_read((uintptr_t)this, sizeof(T), (uint32_t)v) : v;
    }
    void set(T x) volatile {
        if (io(this)) sim_gba_io_write((uintptr_t)this, sizeof(T), (uint32_t)x);
        else v = x;
    }
    operator T() const volatile { return get(); }
    void operator=(T x) volatile { set(x); }
    void operator=(const volatile GbaReg &o) volatile { set(o.get()); }
    volatile GbaReg &operator|=(T x) volatile { set((T)(get() | x)); return *this; }
    volatile GbaReg &operator&=(T x) volatile { set((T)(get() & x)); return *this; }
    volatile GbaReg &operator^=(T x) volatile { set((T)(get() ^ x)); return *this; }
    volatile GbaReg &operator+=(T x) volatile { set((T)(get() + x)); return *this; }
    volatile GbaReg &operator-=(T x) volatile { set((T)(get() - x)); return *this; }
    T operator++() volatile { T x = (T)(get() + 1); set(x); return x; }
    T operator++(int) volatile { T x = get(); set((T)(x + 1)); return x; }
    T operator--() volatile { T x = (T)(get() - 1); set(x); return x; }
    T operator--(int) volatile { T x = get(); set((T)(x - 1)); return x; }
};
extern "C" uint8_t gba_io[0x400];
template <typename T>
bool GbaReg<T>::io(const volatile void *p) {
    const uintptr_t a = (uintptr_t)p, b = (uintptr_t)gba_io;
    return a >= b && a < b + 0x400;
}
typedef volatile GbaReg<uint8_t>  vu8;
typedef volatile GbaReg<uint16_t> vu16;
typedef volatile GbaReg<uint32_t> vu32;
typedef volatile GbaReg<int32_t>  vs32;
#else
typedef volatile uint8_t  vu8;
typedef volatile uint16_t vu16;
typedef volatile uint32_t vu32;
typedef volatile int32_t  vs32;
#endif
typedef uint8_t  bool8;

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#define COMMON_DATA
#define NAKED
#define REVISION 0

/* ---- I/O registers ------------------------------------------------------- */

#ifndef __cplusplus
extern uint8_t gba_io[0x400];
#endif

#define REG_BASE          ((uintptr_t)gba_io)
#define REG_ADDR_TMCNT_L  (REG_BASE + 0x100)
#define REG_ADDR_TMCNT_H  (REG_BASE + 0x102)
#define REG_TMCNT_L(n)    (*(vu16 *)(REG_ADDR_TMCNT_L + (n) * 4))
#define REG_TMCNT_H(n)    (*(vu16 *)(REG_ADDR_TMCNT_H + (n) * 4))
#define REG_TM0CNT_L      REG_TMCNT_L(0)
#define REG_TM0CNT_H      REG_TMCNT_H(0)
#define REG_SIODATA32     (*(vu32 *)(REG_BASE + 0x120))
#define REG_SIOCNT        (*(vu16 *)(REG_BASE + 0x128))
#define REG_RCNT          (*(vu16 *)(REG_BASE + 0x134))
#define REG_IE            (*(vu16 *)(REG_BASE + 0x200))
#define REG_IF            (*(vu16 *)(REG_BASE + 0x202))
#define REG_IME           (*(vu16 *)(REG_BASE + 0x208))

#define SIO_9600_BPS       0x0000
#define SIO_38400_BPS      0x0001  /* Normal mode: internal clock, 256 kHz */
#define SIO_57600_BPS      0x0002  /* Normal mode: external clock          */
#define SIO_115200_BPS     0x0003  /* Normal mode: internal clock, 2 MHz   */
#define SIO_MULTI_SI       0x0004  /* Normal mode: SI line (read)          */
#define SIO_MULTI_SI_SHIFT 2
#define SIO_MULTI_SD       0x0008  /* Normal mode: SO while idle           */
#define SIO_ENABLE         0x0080  /* start / busy                         */
#define SIO_MULTI_BUSY     0x0080
#define SIO_32BIT_MODE     0x1000
#define SIO_INTR_ENABLE    0x4000

#define TIMER_1CLK         0x00
#define TIMER_64CLK        0x01
#define TIMER_256CLK       0x02
#define TIMER_1024CLK      0x03
#define TIMER_INTR_ENABLE  0x40
#define TIMER_ENABLE       0x80

#define INTR_FLAG_TIMER0   (1 << 3)
#define INTR_FLAG_SERIAL   (1 << 7)

#define IntrEnable(flags)                                                      \
    {                                                                          \
        u16 imeTemp_ = REG_IME;                                                \
        REG_IME = 0;                                                           \
        REG_IE |= (flags);                                                     \
        REG_IME = imeTemp_;                                                    \
    }

#ifdef __cplusplus
extern "C"
#endif
void sim_gba_cpufill32(u32 value, void *dst, size_t size);
#define CpuFill32(value, dst, size) sim_gba_cpufill32((value), (dst), (size))
#define DmaCopy16(ch, src, dst, size) memcpy((dst), (src), (size))
#define CpuCopy32(src, dst, size) memcpy((dst), (src), (size))

#endif
