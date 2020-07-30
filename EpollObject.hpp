#ifndef EPOLLOBJECT_H
#define EPOLLOBJECT_H

#include <stdint.h>
#include <sys/epoll.h>

class EpollObject
{
public:
    EpollObject();
    virtual ~EpollObject();
    bool isValid() const;
    enum Kind : uint8_t
    {
        Kind_Server=0x00,
        Kind_Client=0x01,
        Kind_Curl=0x02,
        Kind_CurlMulti=0x03,
        Kind_Dns=0x04,
        Kind_Timer=0x05,
        Kind_Cache=0x06,
    };
    virtual void parseEvent(const epoll_event &event) = 0;
    const Kind &getKind() const;
protected:
    int fd;
    Kind kind;
public:
    static int epollfd;
};

#endif // EPOLLOBJECT_H
