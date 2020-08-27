#ifndef BACKEND_H
#define BACKEND_H

#include "EpollObject.hpp"
#include <netinet/in.h>
#include <unordered_map>
#include <vector>

class Http;

class Backend : public EpollObject
{
public:
    struct BackendList
    {
        std::vector<Backend *> busy;
        std::vector<Backend *> idle;
        std::vector<Http *> pending;//only when no idle and max busy reached
        sockaddr_in6 s;
    };
    enum NonHttpError : uint8_t
    {
        NonHttpError_AlreadySend
    };
public:
    Backend(BackendList * backendList);
    virtual ~Backend();
    void remoteSocketClosed();
    static Backend * tryConnectInternalList(const sockaddr_in6 &s, Http *http, std::unordered_map<std::string, BackendList *> &addressToList, bool &connectInternal);
    static Backend * tryConnectHttp(const sockaddr_in6 &s,Http *http, bool &connectInternal);
    static Backend * tryConnectHttps(const sockaddr_in6 &s,Http *http, bool &connectInternal);
    void downloadFinished();
    void parseEvent(const epoll_event &event) override;
    bool tryConnectInternal(const sockaddr_in6 &s);
    static std::unordered_map<std::string,BackendList *> addressToHttp;
    static std::unordered_map<std::string,BackendList *> addressToHttps;

    void readyToWrite();
    ssize_t socketRead(void *buffer, size_t size);
    bool socketWrite(const void *buffer, size_t size);
public:
    Http *http;
private:
    std::string bufferSocket;
    BackendList * backendList;
};

#endif // BACKEND_H
