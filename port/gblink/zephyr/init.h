/*
 * zephyr/init.h -- SYS_INIT hooks run from zshim_run_inits() in level and
 * priority order, rather than by the loader, for the same reason static
 * threads are started explicitly (see kernel.h).
 */
#ifndef GBAGATE_ZSHIM_INIT_H
#define GBAGATE_ZSHIM_INIT_H
#include <zephyr/kernel.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { ZSHIM_PRE_KERNEL_1 = 0, ZSHIM_PRE_KERNEL_2, ZSHIM_POST_KERNEL, ZSHIM_APPLICATION };
#define PRE_KERNEL_1 ZSHIM_PRE_KERNEL_1
#define PRE_KERNEL_2 ZSHIM_PRE_KERNEL_2
#define POST_KERNEL  ZSHIM_POST_KERNEL
#define APPLICATION  ZSHIM_APPLICATION
typedef struct zshim_init_def {
    int (*fn)(void);
    int level;
    int prio;
    struct zshim_init_def *next;
} zshim_init_def_t;
void zshim_register_init(zshim_init_def_t *def);
#define SYS_INIT(fn_, level_, prio_)                                          \
    static zshim_init_def_t zshim_init_##fn_ = { fn_, level_, prio_, NULL };  \
    ZSHIM_CTOR(zshim_init_reg_##fn_) { zshim_register_init(&zshim_init_##fn_); }
#ifdef __cplusplus
}
#endif
#endif
