#include <rthw.h>
#include <rtthread.h>
#include "netbuffer.h"

#define DBG_TAG           "NET_BUF"
#define DBG_LVL            DBG_LOG
#include <rtdbg.h>

/* net buffer worker status */
#define NETBUF_STAT_STOPPED     0
#define NETBUF_STAT_BUFFERING   1
#define NETBUF_STAT_SUSPEND     2
#define NETBUF_STAT_STOPPING    3

/* net buffer module */
struct net_buffer
{
    /* read index and save index in the buffer */
	rt_size_t read_index, save_index;

    /* buffer data and size of buffer */
	rt_uint8_t* buffer_data;
	rt_size_t data_length;
	rt_size_t size;

	/* buffer ready water mater */
	rt_uint32_t ready_wm, resume_wm;
	rt_bool_t is_wait_ready;
    rt_sem_t wait_ready, wait_resume;

	/* buffer worker status */
	rt_uint8_t stat;
};

struct net_buffer_job
{
	rt_size_t (*fetch)(rt_uint8_t* ptr, rt_size_t len, void* parameter);
	void (*close)(void* parameter);
	void* parameter;
};

static struct net_buffer _netbuf;
static rt_mq_t _netbuf_mq = RT_NULL;

/* net buffer worker public API */
rt_size_t net_buf_read(rt_uint8_t* buffer, rt_size_t length)
{
    rt_size_t data_length, read_index;
    rt_uint32_t level;

    data_length = _netbuf.data_length;

    if ((data_length == 0) && (_netbuf.stat == NETBUF_STAT_BUFFERING || _netbuf.stat == NETBUF_STAT_SUSPEND))
    {
    	rt_err_t result;

        /* buffer is not ready. */
        _netbuf.is_wait_ready = RT_TRUE;
        LOG_D("wait ready, data length: %d, status %d", data_length, _netbuf.stat);

        /* take semaphore wait_ready */
        result = rt_sem_take(_netbuf.wait_ready, RT_WAITING_FOREVER);

        /* take semaphore failed, net buffer worker is stopped */
        if (result != RT_EOK) return 0;
    }

    /* get read and save index */
    read_index = _netbuf.read_index;
    /* re-get data length */
    data_length = _netbuf.data_length;

    /* set the length */
    if (length > data_length) length = data_length;

    LOG_D("data length: %d, read index %d", data_length, read_index);
    if (data_length > 0)
    {
        /* copy buffer */
        if (_netbuf.size - read_index > length)
        {
            rt_memcpy(buffer, &_netbuf.buffer_data[read_index],
                length);
            _netbuf.read_index += length;
        }
        else
        {
            rt_memcpy(buffer, &_netbuf.buffer_data[read_index],
                _netbuf.size - read_index);
            rt_memcpy(&buffer[_netbuf.size - read_index],
                &_netbuf.buffer_data[0],
                length - (_netbuf.size - read_index));
			_netbuf.read_index = length - (_netbuf.size - read_index);
        }

		/* update length of data in buffer */
		level = rt_hw_interrupt_disable();
		_netbuf.data_length -= length;
		data_length = _netbuf.data_length;

        if ((_netbuf.stat == NETBUF_STAT_SUSPEND) && data_length < _netbuf.resume_wm)
        {
        	_netbuf.stat = NETBUF_STAT_BUFFERING;
			rt_hw_interrupt_enable(level);

			/* resume net buffer worker */
			LOG_D("status[suspend] -> [buffering]");
			rt_sem_release(_netbuf.wait_resume);
        }
		else
		{
			rt_hw_interrupt_enable(level);
		}
    }
	
    return length;
}

int net_buf_start_job(rt_size_t (*fetch)(rt_uint8_t* ptr, rt_size_t len, void* parameter),
	void (*close)(void* parameter),
	void* parameter)
{
	struct net_buffer_job job;
	rt_uint32_t level;

	/* job message */
	job.fetch = fetch;
	job.close = close;
	job.parameter = parameter;

	level = rt_hw_interrupt_disable();
	/* check net buffer worker is stopped */
	if (_netbuf.stat == NETBUF_STAT_STOPPED)
	{
		/* change status to buffering if netbuf stopped */
		_netbuf.stat = NETBUF_STAT_BUFFERING;
		rt_hw_interrupt_enable(level);

		LOG_D("status[stopped] -> [buffering]");

		rt_mq_send(_netbuf_mq, (void*)&job, sizeof(struct net_buffer_job));
		return 0;
	}
	rt_hw_interrupt_enable(level);

	return -1;
}

void net_buf_stop_job()
{
	rt_uint32_t level;

	level = rt_hw_interrupt_disable();
	if (_netbuf.stat == NETBUF_STAT_SUSPEND)
	{
		/* resume the net buffer worker */
		rt_sem_release(_netbuf.wait_resume);
		_netbuf.stat = NETBUF_STAT_STOPPING;
		LOG_D("status[suspend] -> [stopping]");
		
	}
	else if (_netbuf.stat == NETBUF_STAT_BUFFERING)
	{
		/* net buffer worker is working, set stat to stopping */
		_netbuf.stat = NETBUF_STAT_STOPPING;
		LOG_D("status[buffering] -> [stopping]");
	}
	rt_hw_interrupt_enable(level);
}

/* get buffer usage percent */
int net_buf_get_usage(void)
{
	return _netbuf.data_length;
}

static void net_buf_do_stop(struct net_buffer_job* job)
{
	/* source closed */
	job->close(job->parameter);

	/* set status to stopped */
	_netbuf.stat = NETBUF_STAT_STOPPED;
	LOG_D("status -> stopped");
	if (_netbuf.is_wait_ready == RT_TRUE)
	{
		/* resume the wait for buffer task */
		_netbuf.is_wait_ready = RT_FALSE;
		rt_sem_release(_netbuf.wait_ready);
	}
	/* reset buffer status */
	_netbuf.data_length = 0;
	_netbuf.read_index = 0 ;
	_netbuf.save_index = 0;

	LOG_D("job done");
}

#define NETBUF_BLOCK_SIZE  1024

static void net_buf_do_job(struct net_buffer_job* job)
{
	rt_uint32_t level;
	rt_size_t read_length, data_length;
	rt_uint8_t *ptr;

	ptr = rt_malloc(NETBUF_BLOCK_SIZE);

    while (1)
    {
    	if (_netbuf.stat == NETBUF_STAT_STOPPING)
    	{
    		net_buf_do_stop(job);
            break;
    	}

    	/* fetch data buffer */
		read_length = job->fetch(ptr, NETBUF_BLOCK_SIZE, job->parameter);
		if (read_length <= 0)
		{
		    LOG_E("read_length < 0");
			net_buf_do_stop(job);
            break;
		}
		else
		{
		    LOG_D("fetch data %d bytes", read_length);
			/* got data length in the buffer */
			data_length = _netbuf.data_length;

			/* check available buffer to save */
			if ((_netbuf.size - data_length) < read_length)
			{
				rt_err_t result, level;

				/* no free space yet, suspend itself */
				LOG_D("status[buffering] -> [suspend], available room %d", data_length);
				level = rt_hw_interrupt_disable();
				_netbuf.stat = NETBUF_STAT_SUSPEND;
				rt_hw_interrupt_enable(level);
				result = rt_sem_take(_netbuf.wait_resume, RT_WAITING_FOREVER);
				if (result != RT_EOK)
				{
					/* stop net buffer worker */
				    LOG_E("wait resume failed");
					net_buf_do_stop(job);
					break;
				}
			}

			/* there are enough free space to fetch data */
	        if ((_netbuf.size - _netbuf.save_index) < read_length)
	        {
	        	rt_memcpy(&_netbuf.buffer_data[_netbuf.save_index],
						ptr, _netbuf.size - _netbuf.save_index);
				rt_memcpy(&_netbuf.buffer_data[0],
						ptr + (_netbuf.size - _netbuf.save_index),
						read_length - (_netbuf.size - _netbuf.save_index));

						/* move save index */
				_netbuf.save_index = read_length - (_netbuf.size - _netbuf.save_index);
	        }
	        else
	        {
	        	rt_memcpy(&_netbuf.buffer_data[_netbuf.save_index],
						ptr, read_length);

				/* move save index */
				_netbuf.save_index += read_length;
				if (_netbuf.save_index >= _netbuf.size) _netbuf.save_index = 0;
	        }

			level = rt_hw_interrupt_disable();
			_netbuf.data_length += read_length;
			data_length = _netbuf.data_length;
			rt_hw_interrupt_enable(level);
		}

        if ((_netbuf.stat == NETBUF_STAT_BUFFERING) 
			&& (data_length >= _netbuf.ready_wm) 
			&& _netbuf.is_wait_ready == RT_TRUE)
        {
            /* notify the thread for waiting buffer ready */
            LOG_D("resume wait buffer");

            _netbuf.is_wait_ready = RT_FALSE;
            /* set buffer status to playing */
            rt_sem_release(_netbuf.wait_ready);
        }
    }

	/* release fetch buffer */
	rt_free(ptr);
}

static void net_buf_thread_entry(void* parameter)
{
	rt_err_t result;
	struct net_buffer_job job;

	LOG_D("enter net buffer thread entry");
    while (1)
    {
    	/* get a job */
		result = rt_mq_recv(_netbuf_mq, (void*)&job, sizeof(struct net_buffer_job), RT_WAITING_FOREVER);
		if (result == RT_EOK)
		{
			/* set status to buffering */
			if (_netbuf.stat == NETBUF_STAT_BUFFERING)
			{
				/* reset data length and read/save index */
				_netbuf.data_length = 0;
				_netbuf.read_index = _netbuf.save_index = 0;

				/* perform the job */
				net_buf_do_job(&job);
			}
		}
    }
}

void net_buf_init(rt_size_t size)
{
    rt_thread_t tid;

    /* init net buffer structure */
    _netbuf.read_index = _netbuf.save_index = 0;
    /* set net buffer size */
    _netbuf.size = size;

    /* allocate buffer */
    _netbuf.buffer_data = rt_malloc(_netbuf.size);
	_netbuf.data_length = 0;

	/* set ready and resume water mater */
	_netbuf.ready_wm = _netbuf.size * 90/100;
	_netbuf.resume_wm = _netbuf.size * 80/100;

	/* set init status */
	_netbuf.stat = NETBUF_STAT_STOPPED;

	_netbuf.wait_ready  = rt_sem_create("n_ready", 0, RT_IPC_FLAG_FIFO);
	_netbuf.wait_resume = rt_sem_create("n_resum", 0, RT_IPC_FLAG_FIFO);
	_netbuf.is_wait_ready = RT_FALSE;

	/* crate message queue */
	_netbuf_mq = rt_mq_create("n_job", sizeof(struct net_buffer_job), 4, RT_IPC_FLAG_FIFO);

    /* create net buffer thread */
    tid = rt_thread_create("n_buf", net_buf_thread_entry, RT_NULL, 4096, 22, 5);
    if (tid != RT_NULL)
        rt_thread_startup(tid);
}

/* test net buffer */
#include <dfs_posix.h>

static rt_size_t fd_fetch(rt_uint8_t* ptr, rt_size_t len, void* parameter)
{
    int size;
    size = read((int)parameter, ptr, len);
    return size;
}

void fd_close(void* parameter)
{
    close((int)parameter);
}

void test_buf(void)
{
    int fd;
    net_buf_init(4096);

    fd = open("/test.txt", O_RDONLY);
    net_buf_start_job(fd_fetch, fd_close, (void *)fd);
}
MSH_CMD_EXPORT(test_buf, test the net buf);

rt_uint8_t *buf;
rt_thread_t read_thread;

void read_buf_thread_entry(void * p)
{
    int r_size;
    buf = rt_malloc(512);

    while(1)
    {
        r_size = net_buf_read(buf, 512);
        if(r_size > 0)
        {
            LOG_D("read from buffer %d bytes", r_size);
        }
        else
        {
            break;
        }

        rt_thread_mdelay(3000);

    }

    rt_free(buf);
}

void read_buf(void)
{
    read_thread = rt_thread_create("r_buf", read_buf_thread_entry, RT_NULL, 4096, 20, 10);

    if(read_thread == RT_NULL)
    {
        LOG_E("create read buffer thread failed");
    }
    rt_thread_startup(read_thread);
}
MSH_CMD_EXPORT(read_buf, read the net buf);
