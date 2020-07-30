#ifndef Cache_H
#define Cache_H

#include "EpollObject.hpp"

#include <curl/curl.h>

class Cache;

class Cache : public EpollObject
{
public:
    Cache(const int &fd);
    ~Cache();
    void parseEvent(const epoll_event &event) override;
    uint64_t access_time();
    uint64_t last_modification_time_check();
    uint64_t modification_time();
    void set_access_time(const uint64_t &time);
    void set_last_modification_time_check(const uint64_t &time);
    void set_modification_time(const uint64_t &time);
    void close();
    void setAsync();
    bool setContentPos();
    ssize_t write(const char * const data,const size_t &size);
    ssize_t read(char * data,const size_t &size);
};

#endif // Cache_H
