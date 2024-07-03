#include <cinttypes>
#include <numeric>
#include <set>
#include <string>
#include <string.h>
#include <tuple>
#include <vector>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <ctime>
#include "RingBuffer.h"

#define USB_INFO_SIZE       256
#define RING_BUFF_SIZE      512
#define MAX_EVENT_NUMBER    10

#define MAX_EPOLL_EVENTS    5

struct usb_info_t {
    char name[USB_INFO_SIZE];
    unsigned short vendor_id;
    unsigned short product_id;
    unsigned long long insert_time;
    unsigned long long remove_time;
};


class user_usb_monitor
{
public:
    char *m_dev_name;
    int m_fd;// /proc/usb_monitor
    int m_epoll_fd;
    RingBuffer<usb_info_t> m_ring_buff;
public:
    int init();
    user_usb_monitor(char* dev_name):m_dev_name(dev_name){}
    ~user_usb_monitor();
};

int user_usb_monitor::init()
{
    m_fd = open(m_dev_name,O_RDWR);
    if (m_fd == -1) 
    {
        printf("open %s fail, Check!!!\n",m_dev_name);
        return errno;
    }
    m_epoll_fd = epoll_create(MAX_EPOLL_EVENTS);
    if (m_epoll_fd == -1) 
    {
        printf("epoll_create failed errno = %d ", errno);	
        return errno;
    }
    struct epoll_event ev;
    ev.data.fd = m_fd;
    ev.events = EPOLLIN;
    if(epoll_ctl(m_epoll_fd,EPOLL_CTL_ADD,m_fd,&ev) < 0)
    {
        printf("epoll_ctl failed, errno = %d \n", errno);
        return errno;
    }
    return 0;
}

user_usb_monitor::~user_usb_monitor()
{
    epoll_ctl(m_epoll_fd,EPOLL_CTL_DEL,m_fd,0);
    close(m_epoll_fd);
    close(m_fd);
}

static void sec2human_time(unsigned long long sec_time,char buff[64])
{
    if(sec_time == 0)
    {
        memset(buff,0,64);
        return;
    }
    std::time_t seconds = sec_time;
    std::tm* timeinfo = std::localtime(&seconds);

    snprintf(buff, 64, "%04ld-%02d-%02d %02d:%02d:%02d",
             timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
             timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
}

int main()
{
    int n_ready;
    size_t len;
    epoll_event events[MAX_EVENT_NUMBER];

    user_usb_monitor monitor("/proc/usb_monitor");
    if(0 != monitor.init())return -1;

    while (1)
    {
        n_ready = epoll_wait(monitor.m_epoll_fd,events,MAX_EVENT_NUMBER,-1);
        if(n_ready == -1 && errno != EINTR)
        {
            printf("epoll_wait failed; errno=%d\n", errno);
            break;
        }

        for(int i = 0;i < n_ready;i++)
        {
            if(events[i].events & EPOLLIN)
            {
                // 读入ring_buf
                usb_info_t  info;
                len = read(monitor.m_fd,&info,sizeof(usb_info_t));
                monitor.m_ring_buff.Append(info);
            }
        }
        // 显示
        printf("\033[2J\033[H");
        printf("%-20s %-15s %-15s %-24s %-20s\n", "Name", "Vendor ID", "Product ID", "Insert Time", "Remove Time");
        printf("---------------------------------------------------------------------------------------------------\n");
        // 打印每个 usb_info_t 的信息
        char insert_time_human[64] = {0};
        char remove_time_human[64] = {0};
        for (size_t i = 0; i < monitor.m_ring_buff.GetSize(); ++i) {
            usb_info_t infos = monitor.m_ring_buff.Get(i);
            sec2human_time(infos.insert_time,insert_time_human);
            sec2human_time(infos.remove_time,remove_time_human);
            printf("%-20s %-15hu %-15hu %-24s %-20s\n", 
                infos.name,
                infos.vendor_id, 
                infos.product_id, 
                insert_time_human, 
                remove_time_human);
        }
    }
    

    return 0;
}