#include "Http.hpp"
#include "Client.hpp"
#include "Cache.hpp"
#include "Backend.hpp"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
#include <sstream>
#include <algorithm>
#include <fcntl.h>

std::unordered_map<std::string,Http *> Http::pathToHttp;
char Http::buffer[4096];

Http::Http(const int &cachefd, //0 if no old cache file found
           const std::string &cachePath) :
    cachePath(cachePath),
    tempCache(nullptr),
    finalCache(nullptr),
    act(0),
    parsedHeader(false),
    contentsize(-1),
    contentwritten(0),
    http_code(0),
    parsing(Parsing_None),
    requestSended(false),
    backend(nullptr)
{
    if(cachefd==0)
    {
        std::cerr << "Http::Http()cachefd==0 then tempCache(nullptr): " << this << std::endl;
        mtime=0;
    }
    else
    {
        std::cerr << "Http::Http() cachefd!=0: " << this << std::endl;
        finalCache=new Cache(cachefd);
        mtime=finalCache->modification_time();
    }
    /*
    //while receive write to cache
    //when finish
        //unset Http to all future listener
        //Close all listener
    */
}

Http::~Http()
{
    std::cerr << "Http::~Http(): " << this << std::endl;
    delete tempCache;
    tempCache=nullptr;
    disconnectFrontend();
    disconnectBackend();
    for(Client * client : clientsList)
        client->writeEnd();
    clientsList.clear();
}

bool Http::tryConnect(const sockaddr_in6 &s,const std::string &host,const std::string &uri)
{
    this->host=host;
    this->uri=uri;
    return tryConnectInternal(s);
}

bool Http::tryConnectInternal(const sockaddr_in6 &s)
{
    bool connectInternal=false;
    backend=Backend::tryConnectHttp(s,this,connectInternal);
    std::cerr << this << ": http->backend=" << backend << std::endl;
    return connectInternal;
}

const std::string &Http::getCachePath() const
{
    return cachePath;
}

const int64_t &Http::get_mtime() const
{
    return mtime;
}

void Http::sendRequest()
{
    std::cout << "Http::sendRequest(): " << this << std::endl;
    requestSended=true;
    std::string h(std::string("GET ")+uri+" HTTP/1.1\nHOST: "+host+"\n\n");
    socketWrite(h.data(),h.size());
    /*used for retry host.clear();
    uri.clear();*/
}

void Http::readyToRead()
{
/*    if(var=="content-length")
    if(var=="content-type")*/
    //::read(Http::buffer

    //load into buffer the previous content
    uint16_t offset=0;
    if(!headerBuff.empty())
    {
        offset=headerBuff.size();
        memcpy(buffer,headerBuff.data(),headerBuff.size());
    }

    const ssize_t size=socketRead(buffer+offset,sizeof(buffer)-offset);
    if(size>0)
    {
        //std::cout << std::string(buffer,size) << std::endl;
        if(parsing==Parsing_Content)
            write(buffer,size);
        else
        {
            uint16_t pos=0;
            if(http_code==0)
            {
                //HTTP/1.1 200 OK
                void *fh=nullptr;
                while(pos<size && buffer[pos]!='\n')
                {
                    char &c=buffer[pos];
                    if(http_code==0 && c==' ')
                    {
                        if(fh==nullptr)
                        {
                            pos++;
                            fh=buffer+pos;
                        }
                        else
                        {
                            c=0x00;
                            http_code=atoi((char *)fh);
                            if(!HttpReturnCode(http_code))
                                return;
                            pos++;
                        }
                    }
                    else
                        pos++;
                }
            }
            pos++;

            parsing=Parsing_HeaderVar;
            uint16_t pos2=pos;
            //content-length: 5000
            if(http_code!=0)
            {
                while(pos<size)
                {
                    char &c=buffer[pos];
                    if(c==':' && parsing==Parsing_HeaderVar)
                    {
                        if((pos-pos2)==14)
                        {
                            std::string var(buffer+pos2,pos-pos2);
                            std::transform(var.begin(), var.end(), var.begin(),[](unsigned char c){return std::tolower(c);});
                            if(var=="content-length")
                            {
                                parsing=Parsing_ContentLength;
                                pos++;
                                //std::cout << "content-length" << std::endl;
                            }
                            else
                            {
                                parsing=Parsing_HeaderVal;
                                //std::cout << "1a) " << std::string(buffer+pos2,pos-pos2) << " (" << pos-pos2 << ")" << std::endl;
                                pos++;
                            }
                        }
                        else if((pos-pos2)==12)
                        {
                            std::string var(buffer+pos2,pos-pos2);
                            std::transform(var.begin(), var.end(), var.begin(),[](unsigned char c){return std::tolower(c);});
                            if(var=="content-type")
                            {
                                parsing=Parsing_ContentType;
                                pos++;
                                //std::cout << "content-type" << std::endl;
                            }
                            else
                            {
                                parsing=Parsing_HeaderVal;
                                //std::cout << "1a) " << std::string(buffer+pos2,pos-pos2) << " (" << pos-pos2 << ")" << std::endl;
                                pos++;
                            }
                        }
                        else if((pos-pos2)==17)
                        {
                            std::string var(buffer+pos2,pos-pos2);
                            std::transform(var.begin(), var.end(), var.begin(),[](unsigned char c){return std::tolower(c);});
                            if(var=="transfer-encoding")
                            {
                                parsing=Parsing_TransferEncoding;
                                pos++;
                                //std::cout << "transfer-encoding" << std::endl;
                            }
                            else
                            {
                                parsing=Parsing_HeaderVal;
                                //std::cout << "1a) " << std::string(buffer+pos2,pos-pos2) << " (" << pos-pos2 << ")" << std::endl;
                                pos++;
                            }
                        }
                        else
                        {
                            parsing=Parsing_HeaderVal;
                            //std::cout << "1a) " << std::string(buffer+pos2,pos-pos2) << " (" << pos-pos2 << ")" << std::endl;
                            pos++;
                        }
                        if(c=='\r')
                        {
                            pos++;
                            char &c2=buffer[pos];
                            if(c2=='\n')
                                pos++;
                        }
                        else
                            pos++;
                        pos2=pos;
                    }
                    else if(c=='\n' || c=='\r')
                    {
                        if(pos==pos2 && parsing==Parsing_HeaderVar)
                        {
                            //end of header
                            std::cout << "end of header" << std::endl;
                            parsing=Parsing_Content;
                            if(c=='\r')
                            {
                                pos++;
                                char &c2=buffer[pos];
                                if(c2=='\n')
                                    pos++;
                            }
                            else
                                pos++;

                            long filetime=0;
                    /*        long http_code = 0;
                            Http_easy_getinfo (easy, HttpINFO_RESPONSE_CODE, &http_code);
                            if(http_code==304) //when have header 304 Not Modified
                            {
                                //set_last_modification_time_check() call before
                                for(Client * client : clientsList)
                                    client->startRead(cachePath,false);
                                return size;
                            }

                            Httpcode res = Http_easy_getinfo(easy, HttpINFO_FILETIME, &filetime);
                            if((HttpE_OK != res))
                                filetime=0;*/
                            if(tempCache!=nullptr)
                            {
                                tempCache=nullptr;
                                delete tempCache;
                            }
                            int cachefd=-1;
                            std::cerr << "open((cachePath+.tmp).c_str() " << (cachePath+".tmp") << std::endl;
                            if((cachefd = open((cachePath+".tmp").c_str(), O_RDWR | O_CREAT | O_TRUNC/* | O_NONBLOCK*/, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH))==-1)
                            {
                                if(errno==2)
                                {
                                    ::mkdir("cache/",S_IRWXU);
                                    if(Cache::hostsubfolder)
                                    {
                                        const std::string::size_type &n=cachePath.rfind("/");
                                        const std::string basePath=cachePath.substr(0,n);
                                        mkdir(basePath.c_str(),S_IRWXU);
                                    }
                                    if((cachefd = open((cachePath+".tmp").c_str(), O_RDWR | O_CREAT | O_TRUNC/* | O_NONBLOCK*/, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH))==-1)
                                    {
                                        std::cerr << "open((cachePath+.tmp).c_str() failed " << (cachePath+".tmp") << " errno " << errno << std::endl;
                                        //return internal error
                                        for(Client * client : clientsList)
                                        {
                                            #ifdef DEBUGFASTCGI
                                            std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
                                            #endif
                                            client->cacheError();
                                            client->disconnect();
                                        }
                                        disconnectFrontend();
                                        return;
                                    }
                                }
                                else
                                {
                                    std::cerr << "open((cachePath+.tmp).c_str() failed " << (cachePath+".tmp") << " errno " << errno << std::endl;
                                    //return internal error
                                    disconnectFrontend();
                                    return;
                                }
                            }

                            /* cachePath (content header, 64Bits aligned):
                             * 64Bits: access time
                             * 64Bits: last modification time check
                             * 64Bits: modification time */
                            const int64_t &currentTime=time(NULL);
                            ::write(cachefd,&currentTime,sizeof(currentTime));
                            ::write(cachefd,&currentTime,sizeof(currentTime));
                            ::write(cachefd,&http_code,sizeof(http_code));
                            ::write(cachefd,&filetime,sizeof(filetime));

                            std::string header;
                            switch(http_code)
                            {
                                case 200:
                                break;
                                case 404:
                                header="Status: 404 NOT FOUND\n";
                                break;
                                default:
                                header="Status: 500 Internal Server Error\n";
                                break;
                            }
                            if(contentsize>=0)
                                header+="Content-Length: "+std::to_string(contentsize)+"\n";
                            if(!contenttype.empty())
                                header+="Content-type: "+contenttype+"\n";
                            else
                                header+="Content-type: text/html\n";
                            if(http_code==200)
                            {
                                    header+=+"Last-Modified: "+timestampsToHttpDate(filetime)+"\n"
                                    "Date: "+timestampsToHttpDate(currentTime)+"\n"
                                    "Expires: "+timestampsToHttpDate(currentTime+Cache::timeToCache(http_code))+"\n"
                                    "Cache-Control: public\n"
                                    "Access-Control-Allow-Origin: *\n";
                            }
                            header+="\n";
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

                            break;
                        }
                        else
                        {
                            switch(parsing)
                            {
                                case Parsing_ContentLength:
                                {
                                    uint64_t value64;
                                    std::istringstream iss(std::string(buffer+pos2,pos-pos2));
                                    iss >> value64;
                                    contentsize=value64;
                                }
                                break;
                                case Parsing_ContentType:
                                    contenttype=std::string(buffer+pos2,pos-pos2);
                                break;
                                case Parsing_TransferEncoding:
                                    if(std::string(buffer+pos2,pos-pos2)=="chunked")
                                    {
                                        //do client reject
                                        std::cerr << "Transfer-Encoding: chunked not coded, do client reject" << std::endl;
                                    }
                                break;
                                default:
                                //std::cout << "1b) " << std::string(buffer+pos2,pos-pos2) << std::endl;
                                break;
                            }
                            parsing=Parsing_HeaderVar;
                        }
                        if(c=='\r')
                        {
                            pos++;
                            char &c2=buffer[pos];
                            if(c2=='\n')
                                pos++;
                        }
                        else
                            pos++;
                        pos2=pos;
                    }
                    else
                    {
                        //std::cout << c << std::endl;
                        if(c=='\r')
                        {
                            pos++;
                            char &c2=buffer[pos];
                            if(c2=='\n')
                                pos++;
                        }
                        else
                            pos++;
                    }
                }
            }
            if(parsing==Parsing_Content)
            {
                //std::cerr << "content: " << std::string(buffer+pos,size-pos) << std::endl;
                write(buffer+pos,size-pos);
            }
            else
            {
                switch(parsing)
                {
                    case Parsing_HeaderVar:
                    case Parsing_ContentType:
                    case Parsing_ContentLength:
                        if(headerBuff.empty() && pos2>0)
                            headerBuff=std::string(buffer+pos2,pos-pos2);
                        else
                        {
                            switch(parsing)
                            {
                                case Parsing_ContentLength:
                                case Parsing_ContentType:
                                    parsing=Parsing_HeaderVar;
                                    readyToRead();
                                break;
                                default:
                                std::cerr << "parsing var before abort over size: " << (int)parsing << std::endl;
                                abort();
                            }
                        }
                    break;
                    default:
                    std::cerr << "parsing var before abort over size: " << (int)parsing << std::endl;
                    abort();
                }
            }
        }
        /*const char *ptr = strchr(buffer,':',size);
        if(ptr==nullptr)
        {}
        else
        {
            if(header.empty())
            {
                if((ptr-buffer)==sizeof("content-length") && memcmp(buffer,"content-length",sizeof("content-length"))==0)
                {}
                //if(var=="content-type")
            }
        }*/
    }
    else
        std::cout << "socketRead(), errno " << errno << std::endl;
}

void Http::readyToWrite()
{
    if(!requestSended)
        sendRequest();
}

ssize_t Http::socketRead(void *buffer, size_t size)
{
    if(backend==nullptr)
        return -1;
    return backend->socketRead(buffer,size);
}

bool Http::socketWrite(const void *buffer, size_t size)
{
    if(backend==nullptr)
    {
        std::cerr << "Http::socketWrite error backend==nullptr" << std::endl;
        return false;
    }
    return backend->socketWrite(buffer,size);
}

void Http::disconnectFrontend()
{
    for(Client * client : clientsList)
        client->writeEnd();
    clientsList.clear();
    //disconnectSocket();

    pathToHttp.erase(cachePath);

    contenttype.clear();
    url.clear();
    headerBuff.clear();
    host.clear();
    uri.clear();
}

bool Http::HttpReturnCode(const int &errorCode)
{
    if(errorCode==200)
        return true;
    if(errorCode==304) //when have header 304 Not Modified
    {
        std::cout << http_code << " http code!, cache already good" << std::endl;
        finalCache->set_last_modification_time_check(time(NULL));
        //send file to listener
        for(Client * client : clientsList)
            client->startRead(cachePath,false);
        return true;
    }
    const std::string &errorString("Http "+std::to_string(errorCode));
    for(Client * client : clientsList)
        client->httpError(errorString);
    clientsList.clear();
    return false;
    //disconnectSocket();
}

void Http::parseNonHttpError(const Backend::NonHttpError &error)
{
    switch(error)
    {
        case Backend::NonHttpError_AlreadySend:
        {
            const std::string &errorString("Tcp request already send (internal error)");
            for(Client * client : clientsList)
                client->httpError(errorString);
        }
        break;
        default:
        {
            const std::string &errorString("Unknown non HTTP error");
            for(Client * client : clientsList)
                client->httpError(errorString);
        }
        break;
    }
}

void Http::disconnectBackend()
{
    std::cerr << "Http::disconnectBackend() " << this << std::endl;
    if(tempCache!=nullptr)
        tempCache->close();
    if(finalCache!=nullptr)
        finalCache->close();
    const char * const cstr=cachePath.c_str();
    //todo, optimise with renameat2(RENAME_EXCHANGE) if --flatcache + destination
    ::unlink(cstr);
    if(rename((cachePath+".tmp").c_str(),cstr)==-1)
        std::cerr << "unable to move " << cachePath << ".tmp to " << cachePath << ", errno: " << errno << std::endl;
    //disable to cache
    ::unlink(cstr);

    backend->downloadFinished();
    std::cerr << this << ": http->backend=null" << std::endl;
    backend=nullptr;
    cachePath.clear();
}

/* Assign information to a SockInfo structure */
/*void Http::setsock(Http_socket_t s, Http *e, int act)
{
    (void)e;
    struct epoll_event ev;
    memset(&ev,1,sizeof(ev));

    if(this->fd>0)
    {
        std::cerr << "EPOLL_CTL_DEL Http: " << fd << std::endl;
        if(epoll_ctl(epollfd, EPOLL_CTL_DEL, this->fd, NULL))
            std::cerr << "EPOLL_CTL_DEL failed for fd: " << this->fd << " : " << errno << std::endl;
    }

    this->fd=s;
    this->act=act;
    //this->e=e;

    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.ptr = this;
    std::cerr << "EPOLL_CTL_ADD: " << s << " on " << this << std::endl;
    if(epoll_ctl(epollfd, EPOLL_CTL_ADD, s, &ev))
        std::cerr << "EPOLL_CTL_ADD failed for fd: " << s << " : " << errno << std::endl;
}*/

const int &Http::getAction() const
{
    return act;
}

std::string Http::timestampsToHttpDate(const int64_t &time)
{
    char buffer[100];
    struct tm *my_tm = gmtime(&time);
    strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", my_tm);
    return buffer;
}

void Http::addClient(Client * client)
{
    clientsList.push_back(client);
    if(tempCache)
        client->startRead(cachePath+".tmp",true);
}

void Http::removeClient(Client * client)
{
    size_t i=0;
    while(i<clientsList.size())
    {
        if(clientsList.at(i)==client)
        {
            clientsList.erase(clientsList.cbegin()+i);
            break;
        }
        i++;
    }
    /*auto p=std::find(clientsList.cbegin(),clientsList.cend(),client);
    if(p!=clientsList.cend())
        clientsList.erase(p);*/
}

int Http::write(const void * const data,const size_t &size)
{
    if(tempCache==nullptr)
    {
        //std::cerr << "tempCache==nullptr internal error" << std::endl;
        return size;
    }
    const size_t &writedSize=tempCache->write((char *)data,size);
    for(Client * client : clientsList)
        client->tryResumeReadAfterEndOfFile();

    if(contentsize>=0)
        contentwritten+=size;
    if(contentsize<=contentwritten)
    {
        disconnectFrontend();
        disconnectBackend();
    }

    return writedSize;
    //(write partial cache)
    //open to write .tmp (mv at end)
    //std::cout << std::string((const char *)data,size) << std::endl;
}
