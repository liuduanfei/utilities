
#ifndef __KFIFO_H
#define __KFIFO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <rtthread.h>

#define KFIFO_MALLOC(size)  rt_malloc(size)
#define KFIFO_FREE(block)   rt_free(block)

typedef struct
{
  uint8_t *buffer;
  uint32_t size;
  uint32_t in;
  uint32_t out;
  rt_mutex_t lock;
}kfifo;

/* Variable declarations -----------------------------------------------------*/
/* Variable definitions ------------------------------------------------------*/
/* Function declarations -----------------------------------------------------*/
kfifo *kfifo_malloc(uint32_t size);
void kfifo_free(kfifo *fifo);

uint32_t kfifo_in(kfifo *fifo, const void *in, uint32_t len);
uint32_t kfifo_out(kfifo *fifo, void *out, uint32_t len);

/* Function definitions ------------------------------------------------------*/

/**
  * @brief  Removes the entire FIFO contents.
  * @param  [in] fifo: The fifo to be emptied.
  * @return None.
  */
static inline void kfifo_reset(kfifo *fifo)
{
  fifo->in = fifo->out = 0;
}

/**
  * @brief  Returns the size of the FIFO in bytes.
  * @param  [in] fifo: The fifo to be used.
  * @return The size of the FIFO.
  */
static inline uint32_t kfifo_size(kfifo *fifo)
{
  return fifo->size;
}

/**
  * @brief  Returns the number of used bytes in the FIFO.
  * @param  [in] fifo: The fifo to be used.
  * @return The number of used bytes.
  */
static inline uint32_t kfifo_len(kfifo *fifo)
{
  return fifo->in - fifo->out;
}

/**
  * @brief  Returns the number of bytes available in the FIFO.
  * @param  [in] fifo: The fifo to be used.
  * @return The number of bytes available.
  */
static inline uint32_t kfifo_is_avail(kfifo *fifo)
{
  return kfifo_size(fifo) - kfifo_len(fifo);
}

/**
  * @brief  Is the FIFO empty?
  * @param  [in] fifo: The fifo to be used.
  * @retval true:      Yes.
  * @retval false:     No.
  */
static inline bool kfifo_is_empty(kfifo *fifo)
{
  return kfifo_len(fifo) == 0;
}

/**
  * @brief  Is the FIFO full?
  * @param  [in] fifo: The fifo to be used.
  * @retval true:      Yes.
  * @retval false:     No.
  */
static inline bool kfifo_is_full(kfifo *fifo)
{
  return kfifo_is_avail(fifo) == 0;
}

#ifdef __cplusplus
}
#endif

#endif /* __KFIFO_H */
