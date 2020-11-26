
#include "kfifo.h"
#include <string.h>

#define min(a, b)  (((a) < (b)) ? (a) : (b))

static bool is_power_of_2(uint32_t x);
static uint32_t roundup_pow_of_two(uint32_t x);

/**
  * @brief  Allocates a new FIFO and its internal buffer.
  * @param  [in] size: The size of the internal buffer to be allocated.
  * @note   The size will be rounded-up to a power of 2.
  * @return RingBuffer pointer.
  */
kfifo *kfifo_malloc(uint32_t size)
{
  kfifo *fifo = KFIFO_MALLOC(sizeof(kfifo));

  if(fifo != NULL)
  {
    if(is_power_of_2(size) != true)
    {
      if(size > 0x80000000UL)
      {
        KFIFO_FREE(fifo);
        return NULL;
      }

      size = roundup_pow_of_two(size);
    }

    fifo->buffer = KFIFO_MALLOC(size);

    if(fifo->buffer == NULL)
    {
      KFIFO_FREE(fifo);
      return NULL;
    }

    fifo->size = size;
    fifo->in = fifo->out = 0;

    fifo->lock = rt_mutex_create("kfifo", RT_IPC_FLAG_FIFO);
  }

  return fifo;
}

/**
  * @brief  Frees the FIFO.
  * @param  [in] fifo: The fifo to be freed.
  * @return None.
  */
void kfifo_free(kfifo *fifo)
{
  KFIFO_FREE(fifo->buffer);
  KFIFO_FREE(fifo);
}

/**
  * @brief  Puts some data into the FIFO.
  * @param  [in] fifo: The fifo to be used.
  * @param  [in] in:   The data to be added.
  * @param  [in] len:  The length of the data to be added.
  * @return The number of bytes copied.
  * @note   This function copies at most @len bytes from the @in into
  *         the FIFO depending on the free space, and returns the number
  *         of bytes copied.
  */
uint32_t kfifo_in(kfifo *fifo, const void *in, uint32_t len)
{
    rt_mutex_take(fifo->lock, RT_WAITING_FOREVER);
    len = min(len, kfifo_is_avail(fifo));

    /* First put the data starting from fifo->in to buffer end. */
    uint32_t l = min(len, fifo->size - (fifo->in & (fifo->size - 1)));
    memcpy(fifo->buffer + (fifo->in & (fifo->size - 1)), in, l);

    /* Then put the rest (if any) at the beginning of the buffer. */
    memcpy(fifo->buffer, (uint8_t *)in + l, len - l);

    fifo->in += len;
    rt_mutex_release(fifo->lock);

    return len;
}

/**
  * @brief  Gets some data from the FIFO.
  * @param  [in] fifo: The fifo to be used.
  * @param  [in] out:  Where the data must be copied.
  * @param  [in] len:  The size of the destination buffer.
  * @return The number of copied bytes.
  * @note   This function copies at most @len bytes from the FIFO into
  *         the @out and returns the number of copied bytes.
  */
uint32_t kfifo_out(kfifo *fifo, void *out, uint32_t len)
{
    rt_mutex_take(fifo->lock, RT_WAITING_FOREVER);
    len = min(len, kfifo_len(fifo));

    /* First get the data from fifo->out until the end of the buffer. */
    uint32_t l = min(len, fifo->size - (fifo->out & (fifo->size - 1)));
    memcpy(out, fifo->buffer + (fifo->out & (fifo->size - 1)), l);

    /* Then get the rest (if any) from the beginning of the buffer. */
    memcpy((uint8_t *)out + l, fifo->buffer, len - l);

    fifo->out += len;
    rt_mutex_release(fifo->lock);

    return len;
}

/**
  * @brief  Determine whether some value is a power of two.
  * @param  [in] x: The number to be confirmed.
  * @retval true:   Yes.
  * @retval false:  No.
  * @note   Where zero is not considered a power of two.
  */
static bool is_power_of_2(uint32_t x)
{
  return (x != 0) && ((x & (x - 1)) == 0);
}

/**
  * @brief  Round the given value up to nearest power of two.
  * @param  [in] x: The number to be converted.
  * @return The power of two.
  */
static uint32_t roundup_pow_of_two(uint32_t x)
{
  uint32_t b = 0;

  for(int i = 0; i < 32; i++)
  {
    b = 1UL << i;

    if(x <= b)
    {
      break;
    }
  }

  return b;
}

#include <rtthread.h>
kfifo * fifo;
rt_thread_t thread1;
rt_thread_t thread2;

void thread1_entry(void *arg)
{
    uint8_t in = 1;
    size_t len;

    while(1)
    {
        if(kfifo_is_full(fifo))
        {
            rt_thread_mdelay(10);
            continue;
        }
        len = kfifo_in(fifo, &in, 1);
        rt_kprintf("in len %d\n", len);
        in++;
        rt_thread_mdelay(1000);
    }
}
void thread2_entry(void *arg)
{
    uint8_t out;
    size_t len;

    while(1)
    {
        if (kfifo_is_empty(fifo))
        {
            rt_thread_mdelay(10);
            continue;
        }

        len = kfifo_out(fifo, &out, 1);
        rt_kprintf("out len %d, data %d\n", len, out);

        rt_thread_mdelay(4000);
    }
}

void kfifo_test(void)
{
    fifo = kfifo_malloc(64);

    thread1 = rt_thread_create("thread1", thread1_entry, RT_NULL, 1024, 10, 10);
    rt_thread_startup(thread1);

    thread2 = rt_thread_create("thread2", thread2_entry, RT_NULL, 1024, 10, 10);
    rt_thread_startup(thread2);
}
MSH_CMD_EXPORT(kfifo_test, ...);
