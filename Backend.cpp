#include "Backend.hpp"
#include "Http.hpp"
#include <iostream>
#include <fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>

std::unordered_map<std::string,Backend::BackendList *> Backend::addressToHttp;
std::unordered_map<std::string,Backend::BackendList *> Backend::addressToHttps;

Backend::Backend(BackendList * backendList) :
    wasTCPConnected(false),
    backendList(backendList)
{
    this->kind=EpollObject::Kind::Kind_Backend;
}

Backend::~Backend()
{
    if(fd!=-1)
    {
        std::cerr << "EPOLL_CTL_DEL Http: " << fd << std::endl;
        if(epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL)==-1)
            std::cerr << "EPOLL_CTL_DEL Http: " << fd << ", errno: " << errno << std::endl;
    }
    if(http!=nullptr)
    {
        std::cerr << http << ": http->backend=nullptr;" << std::endl;
        http->backend=nullptr;
        http=nullptr;
    }
    size_t index=0;
    while(index<backendList->busy.size())
    {
        if(backendList->busy.at(index)==this)
        {
            backendList->busy.erase(backendList->busy.cbegin()+index);
            break;
        }
        index++;
    }
    index=0;
    while(index<backendList->idle.size())
    {
        if(backendList->idle.at(index)==this)
        {
            backendList->idle.erase(backendList->idle.cbegin()+index);
            break;
        }
        index++;
    }
}

void Backend::remoteSocketClosed()
{
    if(fd!=-1)
    {
        std::cerr << "EPOLL_CTL_DEL remoteSocketClosed Http: " << fd << std::endl;
        if(epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL)==-1)
            std::cerr << "EPOLL_CTL_DEL remoteSocketClosed Http: " << fd << ", errno: " << errno << std::endl;
        ::close(fd);
        fd=-1;
    }
    if(http!=nullptr)
        http->resetRequestSended();
    if(backendList!=nullptr)
    {
        if(!wasTCPConnected)
        {
            size_t index=0;
            while(index<backendList->busy.size())
            {
                if(backendList->busy.at(index)==this)
                {
                    backendList->busy.erase(backendList->busy.cbegin()+index);
                    break;
                }
                index++;
            }
            if(!backendList->pending.empty() && backendList->busy.empty())
            {
                const std::string error("Tcp connect problem");
                size_t index=0;
                while(index<backendList->pending.size())
                {
                    Http *http=backendList->pending.at(index);
                    http->backendError(error);
                    index++;
                }
            }
            if(http!=nullptr)
                http->backend=nullptr;
            return;
        }
        else
        {
            size_t index=0;
            while(index<backendList->busy.size())
            {
                if(backendList->busy.at(index)==this)
                {
                    backendList->busy.erase(backendList->busy.cbegin()+index);
                    if(http!=nullptr)
                    {
                        /*if(http->requestSended)
                        {
                            std::cerr << "reassign but request already send" << std::endl;
                            http->parseNonHttpError(Backend::NonHttpError_AlreadySend);
                            return;
                        }*/
                        http->requestSended=false;
                        //reassign to idle backend
                        if(!backendList->idle.empty())
                        {
                            //assign to idle backend and become busy
                            Backend *backend=backendList->idle.back();
                            backendList->idle.pop_back();
                            backendList->busy.push_back(backend);
                            backend->http=http;
                            std::cerr << http << ": http->backend=" << backend << std::endl;
                            http->backend=backend;
                            http->readyToWrite();
                        }
                        //reassign to new backend
                        else
                        {
                            Backend *newBackend=new Backend(backendList);
                            if(!newBackend->tryConnectInternal(backendList->s))
                                //todo abort client
                                return;
                            newBackend->http=http;
                            std::cerr << http << ": http->backend=" << newBackend << std::endl;
                            http->backend=newBackend;

                            backendList->busy.push_back(newBackend);
                        }
                        http=nullptr;
                        return;
                    }
                    if(backendList->busy.empty() && backendList->idle.empty() && backendList->pending.empty())
                    {
                        std::string addr((char *)&backendList->s.sin6_addr,16);
                        if(backendList->s.sin6_port == htobe16(80))
                            addressToHttp.erase(addr);
                        else
                            addressToHttps.erase(addr);
                    }
                    backendList=nullptr;
                    break;
                }
                index++;
            }
            index=0;
            while(index<backendList->idle.size())
            {
                if(backendList->idle.at(index)==this)
                {
                    backendList->idle.erase(backendList->idle.cbegin()+index);
                    break;
                }
                index++;
            }
        }
    }
}

void Backend::downloadFinished()
{
    if(backendList==nullptr)
        return;
    if(!wasTCPConnected)
    {
        size_t index=0;
        while(index<backendList->busy.size())
        {
            if(backendList->busy.at(index)==this)
            {
                backendList->busy.erase(backendList->busy.cbegin()+index);
                break;
            }
            index++;
        }
        if(!backendList->pending.empty() && backendList->busy.empty())
        {
            const std::string error("Tcp connect problem");
            size_t index=0;
            while(index<backendList->pending.size())
            {
                Http *http=backendList->pending.at(index);
                http->backendError(error);
                index++;
            }
        }
        http->backend=nullptr;
        return;
    }
    if(backendList->pending.empty())
    {
        size_t index=0;
        while(index<backendList->busy.size())
        {
            if(backendList->busy.at(index)==this)
            {
                backendList->busy.erase(backendList->busy.cbegin()+index);
                break;
            }
            index++;
        }
        backendList->idle.push_back(this);
        std::cerr << http << ": http->backend=null" << std::endl;
        http->backend=nullptr;
    }
    else
    {
        std::cerr << http << ": http->backend=null" << std::endl;
        http->backend=nullptr;
        Http * httpToGet=backendList->pending.front();
        backendList->pending.erase(backendList->pending.cbegin());
        httpToGet->backend=this;
        httpToGet->readyToWrite();
    }
}

Backend * Backend::tryConnectInternalList(const sockaddr_in6 &s,Http *http,std::unordered_map<std::string,BackendList *> &addressToList,bool &connectInternal)
{
    connectInternal=true;
    std::string addr((char *)&s.sin6_addr,16);
    if(addressToList.find(addr)!=addressToList.cend())
    {
        BackendList *list=addressToList[addr];
        if(!list->idle.empty())
        {
            //assign to idle backend and become busy
            Backend *backend=list->idle.back();
            list->idle.pop_back();
            list->busy.push_back(backend);
            backend->http=http;
            std::cerr << http << ": http->backend=" << backend << std::endl;
            http->backend=backend;
            http->readyToWrite();
            return backend;
        }
        else
        {
            if(list->busy.size()<3)
            {
                Backend *newBackend=new Backend(list);
                if(!newBackend->tryConnectInternal(s))
                {
                    connectInternal=false;
                    return nullptr;
                }
                newBackend->http=http;
                std::cerr << http << ": http->backend=" << newBackend << std::endl;
                http->backend=newBackend;

                list->busy.push_back(newBackend);
                return newBackend;
            }
            else
            {
                list->pending.push_back(http);
                return nullptr;
            }
        }
    }
    else
    {
        BackendList *list=new BackendList();
        memcpy(&list->s,&s,sizeof(sockaddr_in6));

        Backend *newBackend=new Backend(list);
        if(!newBackend->tryConnectInternal(s))
        {
            connectInternal=false;
            return nullptr;
        }
        newBackend->http=http;
        std::cerr << http << ": http->backend=" << newBackend << std::endl;
        http->backend=newBackend;

        list->busy.push_back(newBackend);
        addressToList[addr]=list;
        return newBackend;
    }
    return nullptr;
}

Backend * Backend::tryConnectHttp(const sockaddr_in6 &s,Http *http, bool &connectInternal)
{
    return tryConnectInternalList(s,http,addressToHttp,connectInternal);
}

Backend * Backend::tryConnectHttps(const sockaddr_in6 &s,Http *http, bool &connectInternal)
{
    return tryConnectInternalList(s,http,addressToHttps,connectInternal);
}

bool Backend::tryConnectInternal(const sockaddr_in6 &s)
{
    /* --------------------------------------------- */
    /* Create a normal socket and connect to server. */

    fd = socket(AF_INET6, SOCK_STREAM, 0);
    if(fd==-1)
    {
        std::cerr << "Unable to create socket" << std::endl;
        return false;
    }

    char astring[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &(s.sin6_addr), astring, INET6_ADDRSTRLEN);
    #ifdef DEBUGFASTCGI
    if(std::string(astring)=="::")
    {
        std::cerr << "Internal error, try connect on ::" << std::endl;
        abort();
    }
    #endif
    printf("Try connect on %s %i\n", astring, be16toh(s.sin6_port));
    std::cerr << std::endl;
    std::cout << std::endl;

    // non-blocking client socket
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
      exit(12);
    }
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    // ---------------------

    struct epoll_event event;
    event.data.ptr = this;
    event.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLERR | EPOLLHUP | EPOLLRDHUP;

    int t = epoll_ctl(EpollObject::epollfd, EPOLL_CTL_ADD, fd, &event);
    if (t == -1) {
        perror("epoll_ctl");
        exit(2);
    }

    /*sockaddr_in6 targetDnsIPv6;
    targetDnsIPv6.sin6_port = htobe16(53);
    const char * const hostC=host.c_str();
    int convertResult=inet_pton(AF_INET6,hostC,&targetDnsIPv6.sin6_addr);*/
    int err = connect(fd, (struct sockaddr*) &s, sizeof(s));
    if (err < 0 && errno != EINPROGRESS)
    {
        std::cerr << "connect != EINPROGRESS" << std::endl;
        return false;
    }
    return true;
}

void Backend::parseEvent(const epoll_event &event)
{
    if(event.events & EPOLLIN)
    {
        std::cout << "EPOLLIN" << std::endl;
        if(http!=nullptr)
            http->readyToRead();
    }
    if(event.events & EPOLLOUT)
    {
        std::cout << "EPOLLOUT" << std::endl;
        if(http!=nullptr)
            http->readyToWrite();
    }

    if(event.events & EPOLLHUP)
    {
        std::cout << "EPOLLHUP" << std::endl;
        remoteSocketClosed();
        //do client reject
    }
    if(event.events & EPOLLRDHUP)
    {
        std::cout << "EPOLLRDHUP" << std::endl;
        remoteSocketClosed();
    }
    if(event.events & EPOLLET)
    {
        std::cout << "EPOLLET" << std::endl;
        remoteSocketClosed();
    }
    if(event.events & EPOLLERR)
    {
        std::cout << "EPOLLERR" << std::endl;
        remoteSocketClosed();
    }
}

void Backend::readyToWrite()
{
    if(bufferSocket.empty())
        return;
    const ssize_t &sizeW=::write(fd,bufferSocket.data(),bufferSocket.size());
    if(sizeW>=0)
    {
        if((size_t)sizeW<bufferSocket.size())
            this->bufferSocket.erase(0,bufferSocket.size()-sizeW);
        else
            this->bufferSocket.clear();
    }
}

ssize_t Backend::socketRead(void *buffer, size_t size)
{
    if(fd<0)
        return -1;
    return ::read(fd,buffer,size);
}

bool Backend::socketWrite(const void *buffer, size_t size)
{
    if(fd<0)
        return false;
    if(!this->bufferSocket.empty())
    {
        this->bufferSocket+=std::string((char *)buffer,size);
        return true;
    }
    const ssize_t &sizeW=::write(fd,buffer,size);
    if(sizeW>=0)
    {
        if((size_t)sizeW<size)
            this->bufferSocket+=std::string((char *)buffer+sizeW,size-sizeW);
        return true;
    }
    else
    {
        std::cerr << "Http socket errno:" << errno << std::endl;
        return false;
    }
}
