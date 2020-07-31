#include "Curl.hpp"
#include "Client.hpp"
#include "CurlMulti.hpp"
#include "Cache.hpp"
#include <unistd.h>
#include <errno.h>
#include <iostream>
#include <cstring>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <algorithm>
#include <sstream>

Curl::Curl(const int &cachefd,//0 if no old cache file found
           const char * const path) :
    tempCache(nullptr),
    easy(curl_easy_init()),
    act(0),
    parsedHeader(false),
    firstWrite(false),
    contentsize(0)
{
    this->kind=EpollObject::Kind::Kind_Curl;
    cachePath=std::string(path);
    if(cachefd==0)
        mtime=0;
    else
    {
        tempCache=new Cache(cachefd);
        mtime=tempCache->modification_time();
    }
    /*
    //while receive write to cache
    //when finish
        //unset curl to all future listener
        //Close all listener
    */
}

Curl::~Curl()
{
    delete tempCache;
    //when finish
        //unset curl to all future listener
        //Close all listener
}

const int64_t &Curl::get_mtime() const
{
    return mtime;
}

void Curl::parseEvent(const epoll_event &event)
{
    (void)event;
/*    if(!(event & EPOLLIN))
        readyToRead();*/
    CURLMcode rc;

    int action = ((event.events & EPOLLIN) ? CURL_CSELECT_IN : 0) |
                 ((event.events & EPOLLOUT) ? CURL_CSELECT_OUT : 0);

    rc = curl_multi_socket_action(CurlMulti::curlMulti->multi, fd, action, &CurlMulti::curlMulti->still_running);
    CurlMulti::curlMulti->mcode_or_die("event_cb: curl_multi_socket_action", rc);

    CurlMulti::curlMulti->check_multi_info();
}

void Curl::disconnect()
{
    for(Client * client : clientsList)
        client->writeEnd();
    clientsList.clear();
    //disconnectSocket();
}

void Curl::curlError(const CURLcode &errorCode)
{
    const std::string &errorString(curl_easy_strerror(errorCode));
    for(Client * client : clientsList)
        client->curlError(errorString);
    clientsList.clear();
    //disconnectSocket();
}

void Curl::disconnectSocket()
{
    if(fd!=-1)
    {
        epoll_ctl(epollfd,EPOLL_CTL_DEL, fd, NULL);
        //::close(fd);managed by curl multi
        if(tempCache!=nullptr)
            tempCache->close();
        rename((cachePath+".tmp").c_str(),cachePath.c_str());
        fd=-1;
    }
}

/* Assign information to a SockInfo structure */
void Curl::setsock(curl_socket_t s, CURL *e, int act)
{
    (void)e;
    struct epoll_event ev;
    memset(&ev,1,sizeof(ev));

    if(this->fd>0)
    {
        if(epoll_ctl(epollfd, EPOLL_CTL_DEL, this->fd, NULL))
            std::cerr << "EPOLL_CTL_DEL failed for fd: " << this->fd << " : " << errno << std::endl;
    }

    this->fd=s;
    this->act=act;
    //this->e=e;

    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.ptr = this;
    //std::cerr << "EPOLL_CTL_ADD: " << s << std::endl;
    if(epoll_ctl(epollfd, EPOLL_CTL_ADD, s, &ev))
        std::cerr << "EPOLL_CTL_ADD failed for fd: " << s << " : " << errno << std::endl;
}

const int &Curl::getAction() const
{
    return act;
}

CURL * Curl::getEasy() const
{
    return easy;
}

std::string Curl::timestampsToHttpDate(const int64_t &time)
{
    char buffer[100];
    struct tm *my_tm = gmtime(&time);
    strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", my_tm);
    return buffer;
}

void Curl::addClient(Client * client)
{
    clientsList.push_back(client);
    if(tempCache)
        client->startRead(cachePath+".tmp",true);
}

int Curl::write(const void * const data,const size_t &size)
{
    if(!firstWrite)
    {
        firstWrite=true;
        long http_code = 0;
        curl_easy_getinfo (easy, CURLINFO_RESPONSE_CODE, &http_code);
        if(http_code==304) //when have header 304 Not Modified
        {
            //set_last_modification_time_check() call before
            for(Client * client : clientsList)
                client->startRead(cachePath,false);
            return size;
        }
        long filetime=0;
        CURLcode res = curl_easy_getinfo(easy, CURLINFO_FILETIME, &filetime);
        if((CURLE_OK != res))
            filetime=0;
        if(tempCache!=nullptr)
            delete tempCache;
        int cachefd=-1;
        std::cerr << "open((cachePath+.tmp).c_str() " << (cachePath+".tmp") << std::endl;
        if((cachefd = open((cachePath+".tmp").c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH))==-1)
        {
            std::cerr << "open((cachePath+.tmp).c_str() failed " << (cachePath+".tmp") << " errno " << errno << std::endl;
            //return internal error
            return size;
        }
        /* cachePath (content header, 64Bits aligned):
         * 64Bits: access time
         * 64Bits: last modification time check
         * 64Bits: modification time */
        const int64_t &currentTime=time(NULL);
        ::write(cachefd,&currentTime,sizeof(currentTime));
        ::write(cachefd,&currentTime,sizeof(currentTime));
        ::write(cachefd,&filetime,sizeof(filetime));

        std::string header=
                "Content-Length: "+std::to_string(contentsize)+"\n"
                "Content-type: "+contenttype+"\n"
                "Last-Modified: "+timestampsToHttpDate(filetime)+"\n"
                "Date: "+timestampsToHttpDate(currentTime)+"\n"
                "Expires: "+timestampsToHttpDate(currentTime+24*3600)+"\n"
                "Cache-Control: public\n"
                "Access-Control-Allow-Origin: *\n"
                "\n"
                ;
        if(::write(cachefd,header.data(),header.size())!=(ssize_t)header.size())
            abort();

        epoll_event event;
        memset(&event,0,sizeof(event));
        event.data.ptr = tempCache;
        event.events = EPOLLOUT;
        //std::cerr << "EPOLL_CTL_ADD bis: " << cachefd << std::endl;

        tempCache=new Cache(cachefd);
        //tempCache->setAsync(); -> to hard for now

        for(Client * client : clientsList)
            client->startRead(cachePath+".tmp",true);
    }

    const size_t &writedSize=tempCache->write((char *)data,size);
    /*while(Client list)
        new cache data;*/
    return writedSize;
    //(write partial cache)
    //open to write .tmp (mv at end)
    //std::cout << std::string((const char *)data,size) << std::endl;
}

int Curl::header(const void * const data,const size_t &size)
{
    if(!parsedHeader)
    {
        parsedHeader=true;
        const CURLcode &curl_code = curl_easy_perform(easy);
        if(curl_code==CURLE_OK || true)
        {
            long http_code = 0;
            curl_easy_getinfo (easy, CURLINFO_RESPONSE_CODE, &http_code);
            if(http_code==200)
                 //Succeeded
            std::cout << "200 code!" << std::endl;
            else if(http_code==304) //when have header 304 Not Modified
            {
                std::cout << http_code << " http code!, cache already good" << std::endl;
                tempCache->set_last_modification_time_check(time(NULL));
                //send file to listener
                //break
            }
            else
                std::cout << http_code << " http code error!" << std::endl;
        }
        else
        {
            std::cout << "curl_code: " << curl_code << std::endl;
                 //Failed
        }
    }

    const char * const pos=(char *)memchr(data,':',size);
    if(pos==NULL)
        return size;
    std::string var((const char *)data,pos-(const char *)data);
    std::string value(pos+1,size-(pos-(const char *)data)-2);
    if(value.empty())
        return size;
    if(value[0]==' ')
        value.erase(value.cbegin());
    while(!value.empty())
        if(value.back()==0x0a || value.back()==0x0d)
            value.erase(value.cbegin()+value.size()-1);
        else
        {
            //std::cerr << "value.back()" << (int)value.back() << std::endl;
            break;
        }
    //std::cerr << value << value.size() << std::endl;
    std::transform(var.begin(), var.end(), var.begin(),[](unsigned char c){return std::tolower(c);});
    if(var=="content-length")
    {
        uint64_t value64;
        std::istringstream iss(value);
        iss >> value64;
        contentsize=value64;
        return size;
    }
    if(var=="content-type")
    {
        contenttype=value;
        return size;
    }
    std::cout << std::string((const char *)data,size) << std::endl;
    return size;
}
