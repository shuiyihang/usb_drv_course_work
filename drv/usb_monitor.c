#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/fb.h>
#include <linux/notifier.h>
#include <linux/proc_fs.h>
#include <linux/syscore_ops.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/ktime.h>
#include <linux/time.h>
#include <linux/mutex.h>


#define TAG "SYH_USB > "
#define LOG_INFO(fmt,...)   printk(TAG fmt,##__VA_ARGS__)   // 使用dmesg查看信息

#define USB_INFO_SIZE 256
#define MESSAGE_BUFFER_SIZE	5

// 消息格式
struct usb_info_t {
    char name[USB_INFO_SIZE];
    unsigned short vendor_id;
    unsigned short product_id;
    unsigned long long insert_time;
    unsigned long long remove_time;
};

struct usb_monitor_t
{
    struct usb_info_t   messages[MESSAGE_BUFFER_SIZE];// 缓存的消息
    int                 cache_msg_count;
    int                 msg_w_ofs;// 写指针
    int                 msg_r_ofs;// 读指针
    char                cmd_buff[10];
    int                 enable_monitor;// 使能usb监视器
    wait_queue_head_t   monitor_queue;
    struct mutex        monitor_mutex;// 对cache_msg_count互斥访问
};


static struct usb_monitor_t *monitor;

static ssize_t usb_monitor_read(struct file *filp, char __user *buf,
				size_t size, loff_t *ppos)
{
    size_t message_size = sizeof(struct usb_info_t);

    if(size < message_size)
    {
        LOG_INFO("read size is smaller than message size!\n");
        return -EINVAL;
    }
    // 没有缓存的消息则阻塞
    wait_event_interruptible(monitor->monitor_queue, monitor->cache_msg_count > 0);

    mutex_lock(&monitor->monitor_mutex);
    if(monitor->cache_msg_count > 0)
    {
        int index = monitor->msg_r_ofs;

        if(copy_to_user(buf, &monitor->messages[index], message_size))
        // char test[] = "this is a test output\n";
        // if(copy_to_user(buf, test, sizeof(test)))
        {
            LOG_INFO("%s:copy_from_user error!\n");
            mutex_unlock(&monitor->monitor_mutex);
			return -EFAULT;
        }
        monitor->msg_r_ofs++;
        if(monitor->msg_r_ofs >= MESSAGE_BUFFER_SIZE)
		{
            monitor->msg_r_ofs = 0;
        }
        monitor->cache_msg_count--;
    }
    mutex_unlock(&monitor->monitor_mutex);
    return message_size;
}

static unsigned int usb_monitor_poll(struct file *filp,
						struct poll_table_struct *wait)
{
	unsigned int mask = 0;

	poll_wait(filp, &monitor->monitor_queue, wait);

	mutex_lock(&monitor->monitor_mutex);

    // epoll可读
	if (monitor->cache_msg_count > 0)
		mask |= POLLIN | POLLRDNORM;

	mutex_unlock(&monitor->monitor_mutex);

	return mask;
}

static ssize_t usb_monitor_write(struct file *filp, const char __user *buf,
				size_t size, loff_t *ppos)
{
	char end_flag = 0x0a, cmd;

	/* only support size=2, such as "echo 0 > suspend_monitor" */
	if (size != 2) {
		LOG_INFO("invalid cmd size: size = %d\n",(int)size);
		return -EINVAL;
	}

	if (copy_from_user(monitor->cmd_buff, buf, size)) {
		LOG_INFO("copy_from_user error!\n");
		return -EFAULT;
	}

	if (monitor->cmd_buff[1] != end_flag) {
		LOG_INFO("invalid cmd: end_flag != 0x0a\n");
		return -EINVAL;
	}

	cmd = monitor->cmd_buff[0];

	mutex_lock(&monitor->monitor_mutex);

	switch (cmd)
    {
        case '0':
            monitor->enable_monitor = 0;
            LOG_INFO("disable suspend monitor\n");
            break;
        case '1':
            monitor->enable_monitor = 1;
            LOG_INFO("enable suspend monitor\n");
            break;
        default:
            LOG_INFO("invalid cmd: cmd = %d\n",cmd);
            mutex_unlock(&monitor->monitor_mutex);
            return -EINVAL;
	}
	mutex_unlock(&monitor->monitor_mutex);
	return size;
}

static const struct proc_ops usb_monitor_fops = {
	.proc_read = usb_monitor_read,
	.proc_write = usb_monitor_write,
	.proc_poll = usb_monitor_poll,
	// .unlocked_ioctl = usb_monitor_ioctl,// 与用户空间调用的ioctl有关
};



static int usb_notify(struct notifier_block *self, unsigned long action, void *dev)
{
    struct usb_device *usb_dev = (struct usb_device *)dev;
    int index = monitor->msg_w_ofs;

    struct usb_info_t *dev_info = monitor->messages + index;

    mutex_lock(&monitor->monitor_mutex);

    if(monitor->cache_msg_count >= MESSAGE_BUFFER_SIZE)
    {
        // 抛弃掉旧消息
        monitor->msg_r_ofs++;
        if(monitor->msg_r_ofs >= MESSAGE_BUFFER_SIZE)
		{
            monitor->msg_r_ofs = 0;
        }
        monitor->cache_msg_count--;
        LOG_INFO("usb cache msg overflow!\n");
    }

    strncpy(dev_info->name, usb_dev->product, USB_INFO_SIZE - 1);

    dev_info->vendor_id = usb_dev->descriptor.idVendor;
    dev_info->product_id = usb_dev->descriptor.idProduct;
    switch (action)
    {
        case USB_DEVICE_ADD:
            LOG_INFO("usb insert\n");
            dev_info->insert_time = ktime_get_real_seconds();
            dev_info->remove_time = 0;
            break;
        
        case USB_DEVICE_REMOVE:
            LOG_INFO("usb remove\n");
            dev_info->insert_time = 0;
            dev_info->remove_time = ktime_get_real_seconds();
            break;
    }
    monitor->msg_w_ofs++;
    if(monitor->msg_w_ofs >= MESSAGE_BUFFER_SIZE)
    {
        monitor->msg_w_ofs = 0;
    }

    if(monitor->cache_msg_count < MESSAGE_BUFFER_SIZE)
	{
        monitor->cache_msg_count++;
    }
    wake_up_interruptible(&monitor->monitor_queue);

    mutex_unlock(&monitor->monitor_mutex);

    return NOTIFY_OK;

}
static struct notifier_block usb_nb = {
    .notifier_call = usb_notify,
};

static int __init usb_monitor_init(void)
{
    monitor = kzalloc(sizeof(struct usb_monitor_t), GFP_KERNEL);// 在内核态下分配空间，允许睡眠
    if (!monitor)
    {
		return -ENOMEM;
	}
    monitor->cache_msg_count    =   0;
    monitor->enable_monitor     =   1;
    monitor->msg_r_ofs          =   0;
    monitor->msg_w_ofs          =   0;

    /*  
     *   创建usb_monitor文件与用户空间交互
     *   文件名字，文件权限，设置新目录，文件操作
     */
    proc_create("usb_monitor", 0644, NULL, &usb_monitor_fops);

    init_waitqueue_head(&monitor->monitor_queue);
    mutex_init(&monitor->monitor_mutex);

    usb_register_notify(&usb_nb);

    return 0;
}

static void __exit usb_monitor_exit(void)
{
    remove_proc_entry("usb_monitor", NULL);
    usb_unregister_notify(&usb_nb);
    kfree(monitor);
}

module_init(usb_monitor_init);
module_exit(usb_monitor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("shuiyihang");
MODULE_DESCRIPTION("USB Monitor");