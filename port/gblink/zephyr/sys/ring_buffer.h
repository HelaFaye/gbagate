/*
 * zephyr/sys/ring_buffer.h -- byte-mode ring buffer, the subset in use.
 *
 * Like Zephyr's, it is safe for exactly one producer and one consumer without
 * a lock, and no safer: the vendored code relies on that and nothing more.
 * Indices are free-running 32-bit counters, so full and empty are told apart
 * without a wasted byte.
 */
#ifndef GBAGATE_ZSHIM_RING_BUFFER_H
#define GBAGATE_ZSHIM_RING_BUFFER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct ring_buf {
    uint8_t *buffer;
    uint32_t size;
    volatile uint32_t head;   /* total bytes ever put  */
    volatile uint32_t tail;   /* total bytes ever got  */
    uint32_t claim;           /* bytes handed out by get_claim, not finished */
};
#define RING_BUF_DECLARE(name, size8)                                          \
    static uint8_t zshim_rb_mem_##name[size8];                                 \
    struct ring_buf name = { zshim_rb_mem_##name, (size8), 0, 0, 0 }
void     ring_buf_init(struct ring_buf *rb, uint32_t size, uint8_t *data);
void     ring_buf_reset(struct ring_buf *rb);
uint32_t ring_buf_space_get(struct ring_buf *rb);
uint32_t ring_buf_size_get(struct ring_buf *rb);
uint32_t ring_buf_capacity_get(struct ring_buf *rb);
uint32_t ring_buf_put(struct ring_buf *rb, const uint8_t *data, uint32_t size);
uint32_t ring_buf_get(struct ring_buf *rb, uint8_t *data, uint32_t size);
uint32_t ring_buf_peek(struct ring_buf *rb, uint8_t *data, uint32_t size);
uint32_t ring_buf_get_claim(struct ring_buf *rb, uint8_t **data, uint32_t size);
int      ring_buf_get_finish(struct ring_buf *rb, uint32_t size);
static inline int ring_buf_is_empty(struct ring_buf *rb) { return rb->head == rb->tail; }
#ifdef __cplusplus
}
#endif
#endif
