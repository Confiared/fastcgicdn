#include "Cache.hpp"
#include <unistd.h>
#include <iostream>
#include <fcntl.h>

bool Cache::hostsubfolder=true;
/*uint32_t Cache::maxiumSizeToBeSmallFile=4096;
uint64_t Cache::maxiumSmallFileCacheSize=0;//diable by default (to be safe if on ram disk)
uint64_t Cache::smallFileCacheSize=0;*/

Cache::Cache(const int &fd)
{
    this->kind=EpollObject::Kind::Kind_Cache;
    this->fd=fd;
    /*
    //while receive write to cache
    //when finish
        //unset curl to all future listener
        //Close all listener
    */
}

Cache::~Cache()
{
    close();
}

void Cache::parseEvent(const epoll_event &event)
{
    (void)event;
/*    if(!(event & EPOLLIN))
        readyToRead();*/
}

void Cache::close()
{
    if(fd!=-1)
    {
        ::close(fd);
        fd=-1;
    }
}

uint64_t Cache::access_time()
{
    const off_t &s=lseek(fd,0*sizeof(uint64_t),SEEK_SET);
    if(s!=-1)
    {
        uint64_t time=0;
        if(::read(fd,&time,sizeof(time))==sizeof(time))
            return time;

    }
    return 0;
}

uint64_t Cache::last_modification_time_check()
{
    const off_t &s=lseek(fd,1*sizeof(uint64_t),SEEK_SET);
    if(s!=-1)
    {
        uint64_t time=0;
        if(::read(fd,&time,sizeof(time))==sizeof(time))
            return time;

    }
    return 0;
}

uint64_t Cache::modification_time()
{
    const off_t &s=lseek(fd,3*sizeof(uint64_t),SEEK_SET);
    if(s!=-1)
    {
        uint64_t time=0;
        if(::read(fd,&time,sizeof(time))==sizeof(time))
            return time;

    }
    return 0;
}

uint64_t Cache::http_code()
{
    const off_t &s=lseek(fd,2*sizeof(uint64_t),SEEK_SET);
    if(s!=-1)
    {
        uint64_t time=0;
        if(::read(fd,&time,sizeof(time))==sizeof(time))
            return time;

    }
    return 500;
}

void Cache::set_access_time(const uint64_t &time)
{
    const off_t &s=lseek(fd,0*sizeof(time),SEEK_SET);
    if(s!=-1)
    {
        if(::write(fd,&time,sizeof(time))!=sizeof(time))
        {
            std::cerr << "Unable to write last_modification_time_check" << std::endl;
            return;
        }
    }
    else
    {
        std::cerr << "Unable to seek last_modification_time_check" << std::endl;
        return;
    }
}

void Cache::set_last_modification_time_check(const uint64_t &time)
{
    const off_t &s=lseek(fd,1*sizeof(uint64_t),SEEK_SET);
    if(s!=-1)
    {
        if(::write(fd,&time,sizeof(time))!=sizeof(time))
        {
            std::cerr << "Unable to write last_modification_time_check" << std::endl;
            return;
        }
    }
    else
    {
        std::cerr << "Unable to seek last_modification_time_check" << std::endl;
        return;
    }
}

void Cache::set_modification_time(const uint64_t &time)
{
    const off_t &s=lseek(fd,3*sizeof(uint64_t),SEEK_SET);
    if(s!=-1)
    {
        if(::write(fd,&time,sizeof(time))!=sizeof(time))
        {
            std::cerr << "Unable to write last_modification_time_check" << std::endl;
            return;
        }
    }
    else
    {
        std::cerr << "Unable to seek last_modification_time_check" << std::endl;
        return;
    }
}

void Cache::set_http_code(const uint64_t &http_code)
{
    const off_t &s=lseek(fd,2*sizeof(uint64_t),SEEK_SET);
    if(s!=-1)
    {
        if(::write(fd,&http_code,sizeof(http_code))!=sizeof(http_code))
        {
            std::cerr << "Unable to write last_modification_time_check" << std::endl;
            return;
        }
    }
    else
    {
        std::cerr << "Unable to seek last_modification_time_check" << std::endl;
        return;
    }
}

void Cache::setAsync()
{
    int flags, s;
    flags = fcntl(fd, F_GETFL, 0);
    if(flags == -1)
        std::cerr << "fcntl get flags error" << std::endl;
    else
    {
        flags |= O_NONBLOCK;
        s = fcntl(fd, F_SETFL, flags);
        if(s == -1)
            std::cerr << "fcntl set flags error" << std::endl;
    }
}

bool Cache::setContentPos()
{
    const off_t &s=lseek(fd,4*sizeof(uint64_t),SEEK_SET);
    if(s==-1)
    {
        std::cerr << "Unable to seek setContentPos" << std::endl;
        return false;
    }
    return true;
}

ssize_t Cache::write(const char * const data,const size_t &size)
{
    return ::write(fd,data,size);
}

ssize_t Cache::read(char * data,const size_t &size)
{
    return ::read(fd,data,size);
}

uint32_t Cache::timeToCache(uint16_t http_code)
{
    switch(http_code)
    {
        case 200:
            return 24*3600;
        break;
        default:
            return 60;
        break;
    }
}
