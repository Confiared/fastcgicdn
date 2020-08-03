#include <errno.h>
#include <sys/epoll.h>
#include "Server.hpp"
#include "Client.hpp"
#include "Curl.hpp"
#include "Dns.hpp"
#include "CurlMulti.hpp"
#include "Timer.hpp"
#include "Timer/DNSCache.hpp"
#include "Timer/DNSQuery.hpp"
#include <vector>
#include <cstring>
#include <cstdio>
#include <signal.h>
#include <iostream>
#include <sys/stat.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define MAX_EVENTS 1024

void signal_callback_handler(int signum) {
    printf("Caught signal SIGPIPE %d\n",signum);
}

int main(int argc, char *argv[])
{
    /* Catch Signal Handler SIGPIPE */
    if(signal(SIGPIPE, signal_callback_handler)==SIG_ERR)
    {
        std::cerr << "signal(SIGPIPE, signal_callback_handler)==SIG_ERR, errno: " << std::to_string(errno) << std::endl;
        abort();
    }
    mkdir("cache", S_IRWXU);

    (void)argc;
    (void)argv;
    /*std::unordered_map<std::string,curl *> hashToCurl;
    std::unordered_map<int,int> fileToSocket;
    std::unordered_map<curl *,int> curlToSocket;
    std::unordered_map<curl *,int> futureCurlToSocket;*/

    /*char folderApp[strlen(argv[0])+1];
    strcpy(folderApp,argv[0]);
    dirname(folderApp);
    strcat(folderApp, "/");
    printf("path=%s\n", folderApp);*/

    //the event loop
    struct epoll_event ev, events[MAX_EVENTS];
    memset(&ev,0,sizeof(ev));
    int nfds, epollfd;

    ev.events = EPOLLIN|EPOLLET;

    if ((epollfd = epoll_create1(0)) == -1) {
        printf("epoll_create1: %s", strerror(errno));
        return -1;
    }
    EpollObject::epollfd=epollfd;
    Dns::dns=new Dns();
    CurlMulti::curlMulti=new CurlMulti();
    DNSCache dnsCache;
    dnsCache.start(3600*1000);
    DNSQuery dnsQuery;
    dnsQuery.start(10);

    /* cachePath (content header, 64Bits aligned):
     * 64Bits: access time
     * 64Bits: last modification time check
     * 64Bits: modification time */

    /*Server *server=*///new Server("/run/fastcgicdn.sock");
    Server s("/home/user/fastcgicdn.sock");
    (void)s;
    for (;;) {
        if ((nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1)) == -1)
            printf("epoll_wait error %s", strerror(errno));
        std::vector<Client *> deleteClient;
        std::vector<Curl *> deleteCurl;
        for (int n = 0; n < nfds; ++n)
        {
            epoll_event &e=events[n];
            switch(static_cast<EpollObject *>(e.data.ptr)->getKind())
            {
                case EpollObject::Kind::Kind_Server:
                {
                    Server * server=static_cast<Server *>(e.data.ptr);
                    server->parseEvent(e);
                }
                break;
                case EpollObject::Kind::Kind_Client:
                {
                    Client * client=static_cast<Client *>(e.data.ptr);
                    if(!(e.events & EPOLLHUP))
                    {
                        /*deleteClient.push_back(client);
                        client->disconnect();*/
                    }
                    else
                        client->parseEvent(e);
                    if(!client->isValid())
                    {
                        //if(!deleteClient.empty() && deleteClient.back()!=client)
                        deleteClient.push_back(client);
                        client->disconnect();
                    }
                }
                break;
                case EpollObject::Kind::Kind_Curl:
                {
                    Curl * curl=static_cast<Curl *>(e.data.ptr);
                    curl->parseEvent(e);
                    /*delete from CurlMulti::sock_cb() if(!curl->isValid() && deleteCurl.back()!=curl)
                    {
                        deleteCurl.push_back(curl);
                        curl->disconnect();
                    }*/
                }
                break;
                case EpollObject::Kind::Kind_CurlMulti:
                {
                    CurlMulti * curlMulti=static_cast<CurlMulti *>(e.data.ptr);
                    curlMulti->parseEvent(e);
                }
                break;
                case EpollObject::Kind::Kind_Dns:
                {
                    Dns * dns=static_cast<Dns *>(e.data.ptr);
                    dns->parseEvent(e);
                }
                break;
                case EpollObject::Kind::Kind_Timer:
                {
                    static_cast<Timer *>(e.data.ptr)->exec();
                    static_cast<Timer *>(e.data.ptr)->validateTheTimer();
                }
                break;
                default:
                break;
            }
        }
        for(Client * client : deleteClient)
            delete client;
        for(Curl * curl : deleteCurl)
            delete curl;
    }

    return 0;
}
