#include "Dns.hpp"
#include "Client.hpp"
#include <unistd.h>
#include <iostream>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>

struct __attribute__ ((__packed__)) dns_query {
    uint16_t id;
    uint16_t flags;
    uint16_t question_count;
    uint16_t answer_count;
    uint16_t authority_count;
    uint16_t add_count;
    uint8_t  payload[];
};

Dns *Dns::dns=nullptr;

Dns::Dns()
{
    clientInProgress=0;
    this->kind=EpollObject::Kind::Kind_Dns;

    memset(&targetDnsIPv6, 0, sizeof(targetDnsIPv6));
    targetDnsIPv6.sin6_port = htobe16(53);
    memset(&targetDnsIPv4, 0, sizeof(targetDnsIPv4));
    targetDnsIPv4.sin_port = htobe16(53);

    //read resolv.conf
    {
        FILE * fp;
        char * line = NULL;
        size_t len = 0;
        ssize_t read;

        fp = fopen("/etc/resolv.conf", "r");
        if (fp == NULL)
            exit(EXIT_FAILURE);

        while ((read = getline(&line, &len, fp)) != -1) {
            //create udp socket to dns server
            if(tryOpenSocket(line))
                break;
        }

        fclose(fp);
        if (line)
            free(line);
    }

    //add to event loop
    epoll_event event;
    event.data.ptr = this;
    event.events = EPOLLIN;
    //std::cerr << "EPOLL_CTL_ADD: " << fd << std::endl;
    if(epoll_ctl(epollfd,EPOLL_CTL_ADD, fd, &event) == -1)
    {
        printf("epoll_ctl failed to add server: %s", strerror(errno));
        abort();
    }

    increment=1;
}

Dns::~Dns()
{
}

bool Dns::tryOpenSocket(std::string line)
{
    std::string prefix=line.substr(0,11);
    if(prefix=="nameserver ")
    {
        line=line.substr(11);
        line.resize(line.size()-1);
        const std::string &host=line;

        memset(&targetDnsIPv6, 0, sizeof(targetDnsIPv6));
        targetDnsIPv6.sin6_port = htobe16(53);
        const char * const hostC=host.c_str();
        int convertResult=inet_pton(AF_INET6,hostC,&targetDnsIPv6.sin6_addr);
        if(convertResult!=1)
        {
            memset(&targetDnsIPv4, 0, sizeof(targetDnsIPv4));
            targetDnsIPv4.sin_port = htobe16(53);
            convertResult=inet_pton(AF_INET,hostC,&targetDnsIPv4.sin_addr);
            if(convertResult!=1)
            {
                std::cerr << "not IPv4 and IPv6 address, host: \"" << host << "\", portstring: \"53\", errno: " << std::to_string(errno) << std::endl;
                abort();
            }
            else
            {
                targetDnsIPv4.sin_family = AF_INET;

                fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (fd == -1)
                {
                    std::cerr << "unable to create UDP socket" << std::endl;
                    abort();
                }
                sockaddr_in si_me;
                memset((char *) &si_me, 0, sizeof(si_me));
                si_me.sin_family = AF_INET;
                si_me.sin_port = htons(50053);
                si_me.sin_addr.s_addr = htonl(INADDR_ANY);
                if(bind(fd,(struct sockaddr*)&si_me, sizeof(si_me))==-1)
                {
                    std::cerr << "unable to bind UDP socket" << std::endl;
                    abort();
                }

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

                mode=Mode_IPv4;
                return true;
            }
        }
        else
        {
            targetDnsIPv6.sin6_family = AF_INET6;

            fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
            if (fd == -1)
            {
                std::cerr << "unable to create UDP socket" << std::endl;
                abort();
            }
            sockaddr_in6 si_me;
            memset((char *) &si_me, 0, sizeof(si_me));
            si_me.sin6_family = AF_INET6;
            si_me.sin6_port = htons(50053);
            si_me.sin6_addr = IN6ADDR_ANY_INIT;
            if(bind(fd,(struct sockaddr*)&si_me, sizeof(si_me))==-1)
            {
                std::cerr << "unable to bind UDP socket" << std::endl;
                abort();
            }

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

            mode=Mode_IPv6;
            return true;
        }
    }
    return false;
}

void Dns::parseEvent(const epoll_event &event)
{
    if(event.events & EPOLLIN)
    {
        int size = 0;
        do
        {
            char buffer[4096];
            if(mode==Mode_IPv6)
            {
                sockaddr_in6 si_other;
                unsigned int slen = sizeof(si_other);
                memset(&si_other,0,sizeof(si_other));
                size = recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr *) &si_other, &slen);
                if(size<0)
                    break;
                if(memcmp(&targetDnsIPv6.sin6_addr,&si_other.sin6_addr,16)!=0)
                    return;
            }
            else //if(mode==Mode_IPv4)
            {
                sockaddr_in si_other;
                unsigned int slen = sizeof(si_other);
                memset(&si_other,0,sizeof(si_other));
                size = recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr *) &si_other, &slen);
                if(memcmp(&targetDnsIPv4.sin_addr,&si_other.sin_addr,4)!=0)
                    return;
            }
            clientInProgress--;

            int pos=0;
            if(!canAddToPos(2,size,pos))
                return;
            uint16_t flags=0;
            if(!read16Bits(flags,buffer,size,pos))
                return;
            uint16_t questions=0;
            if(!read16Bits(questions,buffer,size,pos))
                return;
            uint16_t answersIndex=0;
            uint16_t answers=0;
            if(!read16Bits(answers,buffer,size,pos))
                return;
            if(!canAddToPos(2,size,pos))
                return;
            if(!canAddToPos(2,size,pos))
                return;
            //load the query
            char host[4096]={0};
            uint8_t len,offs=0;
            while((offs<(size-pos)) && (len = buffer[pos+offs])) {
                strncat(host,buffer+pos+offs+1,len);
                strncat(host,".",2);
                offs += len+1;
            }
            host[offs-1]=0x00;
            pos+=offs+1;
            if (offs>253)//Sorry: query name length exceeds maximum.
                return;

            uint16_t type=0;
            if(!read16Bits(type,buffer,size,pos))
                return;
            if(type!=0x001c)
            {
                abort();
                return;
            }
            uint16_t classIn=0;
            if(!read16Bits(classIn,buffer,size,pos))
                return;
            if(classIn!=0x0001)
                return;

            for(unsigned int i=0;host[i]!=0 && i<sizeof(host);i++)
                host[i] = tolower(host[i]);
            std::string hostcpp(host);

            //answers list
            if(queryList.find(hostcpp)!=queryList.cend())
            {
                const std::vector<Client *> &clients=queryList.at(hostcpp).clients;
                if(!clients.empty())
                {
                    bool clientsFlushed=false;
                    while(answersIndex<answers)
                    {
                        uint16_t AName=0;
                        if(!read16Bits(AName,buffer,size,pos))
                            return;
                        /*if(AName!=0xc00c)
                            return;*/
                        if(!read16Bits(type,buffer,size,pos))
                            return;
                        switch(type)
                        {
                            //AAAA
                            case 0x001c:
                            {
                                if(!read16Bits(classIn,buffer,size,pos))
                                    return;
                                if(classIn!=0x0001)
                                    return;
                                uint32_t ttl=0;
                                if(!read32Bits(ttl,buffer,size,pos))
                                    return;
                                uint16_t datasize=0;
                                if(!read16Bits(datasize,buffer,size,pos))
                                    return;
                                if(datasize!=16)
                                    return;

                                //TODO saveToCache();
                                unsigned char check[]={0x28,0x03,0x19,0x20};
                                if(memcmp(buffer+pos,check,sizeof(check))!=0 || (flags & 0xFF0F)!=0x8100)
                                {
                                    if(!clientsFlushed)
                                    {
                                        clientsFlushed=true;
                                        for(Client * const c : clients)
                                            c->dnsWrong();
                                        queryList.erase(hostcpp);
                                    }
                                }
                                else
                                {
                                    if(!clientsFlushed)
                                    {
                                        clientsFlushed=true;
                                        for(Client * const c : clients)
                                            c->dnsRight();
                                        queryList.erase(hostcpp);
                                    }
                                }
                                answersIndex=answers;
                            }
                            break;
                            default:
                            {
                                canAddToPos(2+4,size,pos);
                                uint16_t datasize=0;
                                if(!read16Bits(datasize,buffer,size,pos))
                                    return;
                                canAddToPos(datasize,size,pos);
                            }
                            break;
                        }
                    }
                    if(!clientsFlushed)
                    {
                        clientsFlushed=true;
                        for(Client * const c : clients)
                            c->dnsError();
                        queryList.erase(hostcpp);
                    }
                }
            }
        } while(size>=0);
    }
}

bool Dns::canAddToPos(const int &i, const int &size, int &pos)
{
    if((pos+i)>size)
        return false;
    pos+=i;
    return true;
}

bool Dns::read8Bits(uint8_t &var, const char * const data, const int &size, int &pos)
{
    if((pos+(int)sizeof(var))>size)
        return false;
    var=data[pos];
    pos+=sizeof(var);
    return true;
}

bool Dns::read16Bits(uint16_t &var, const char * const data, const int &size, int &pos)
{
    if((pos+(int)sizeof(var))>size)
        return false;
    uint16_t t;
    memcpy(&t,data+pos,sizeof(var));
    var=be16toh(t);
    pos+=sizeof(var);
    return true;
}

bool Dns::read32Bits(uint32_t &var, const char * const data, const int &size, int &pos)
{
    if((pos+(int)sizeof(var))>size)
        return false;
    uint32_t t;
    memcpy(&t,data+pos,sizeof(var));
    var=be32toh(t);
    pos+=sizeof(var);
    return true;
}

bool Dns::get(Client * client,const std::string &host)
{
    if(queryList.find(host)!=queryList.cend())
    {
        queryList[host].clients.push_back(client);
        return true;
    }
    if(clientInProgress>1000)
        return false;
    clientInProgress++;
    /* TODO if(isInCache())
    {load from cache}*/
    //std::cout << "dns query count merged in progress>1000" << std::endl;
    uint8_t buffer[4096];
    struct dns_query* query = (struct dns_query*)buffer;
    query->id=increment++;
    if(increment>65534)
        increment=1;
    query->flags=htobe16(288);
    query->question_count=htobe16(1);
    query->answer_count=0;
    query->authority_count=0;
    query->add_count=0;
    int pos=2+2+2+2+2+2;

    //hostname encoded
    int hostprevpos=0;
    size_t hostpos=host.find(".",hostprevpos);
    while(hostpos!=std::string::npos)
    {
        const std::string &part=host.substr(hostprevpos,hostpos-hostprevpos);
        //std::cout << part << std::endl;
        buffer[pos]=part.size();
        pos+=1;
        memcpy(buffer+pos,part.data(),part.size());
        pos+=part.size();
        hostprevpos=hostpos+1;
        hostpos=host.find(".",hostprevpos);
    }
    const std::string &part=host.substr(hostprevpos);
    //std::cout << part << std::endl;
    buffer[pos]=part.size();
    pos+=1;
    memcpy(buffer+pos,part.data(),part.size());
    pos+=part.size();

    buffer[pos]=0x00;
    pos+=1;

    //type AAAA
    buffer[pos]=0x00;
    pos+=1;
    buffer[pos]=0x1c;
    pos+=1;

    //class IN
    buffer[pos]=0x00;
    pos+=1;
    buffer[pos]=0x01;
    pos+=1;

    if(mode==Mode_IPv6)
        /*int result = */sendto(fd,&buffer,pos,0,(struct sockaddr*)&targetDnsIPv6,sizeof(targetDnsIPv6));
    else //if(mode==Mode_IPv4)
        /*int result = */sendto(fd,&buffer,pos,0,(struct sockaddr*)&targetDnsIPv4,sizeof(targetDnsIPv4));

    Query queryToPush;
    queryToPush.startTime=0;//todo
    queryToPush.clients.push_back(client);
    queryList[host]=queryToPush;
    return true;
}

void Dns::cancelClient(Client * client,const std::string &host)
{
    if(queryList.find(host)!=queryList.cend())
    {
        std::vector<Client *> &clients=queryList[host].clients;
        //optimize, less check
        if(clients.size()<=1)
        {
            queryList.erase(host);
            return;
        }
        unsigned int index=0;
        while(index<clients.size())
        {
            if(client==clients.at(index))
            {
                clients.erase(clients.cbegin()+index);
                break;
            }
            index++;
        }
    }
}

int Dns::requestCountMerged()
{
    return 0;
}
