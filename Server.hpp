#ifndef SERVER_H
#define SERVER_H

#include "EpollObject.hpp"

class Server : public EpollObject
{
public:
    Server(const char * const path);
    void parseEvent(const epoll_event &) override;
};

#endif // SERVER_H
