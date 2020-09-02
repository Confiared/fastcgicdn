#include "Http.hpp"
#include "Client.hpp"
#include "Cache.hpp"
#include "Backend.hpp"
#include "Common.hpp"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
#include <sstream>
#include <algorithm>
#include <fcntl.h>

//ETag -> If-None-Match
const char rChar[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
//Todo: limit max file size 9GB
//reuse cache stale for file <20KB

std::unordered_map<std::string,Http *> Http::pathToHttp;
int Http::fdRandom=-1;
char Http::buffer[4096];

Http::Http(const int &cachefd, //0 if no old cache file found
           const std::string &cachePath) :
    cachePath(cachePath),//to remove from Http::pathToHttp
    tempCache(nullptr),
    finalCache(nullptr),
    parsedHeader(false),
    contentsize(-1),
    contentwritten(0),
    http_code(0),
    parsing(Parsing_None),
    requestSended(false),
    backend(nullptr),
    contentLengthPos(-1),
    chunkLength(-1)
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
    if(cachePath.empty())
        abort();
    #endif
    if(cachefd==0)
        std::cerr << "Http::Http()cachefd==0 then tempCache(nullptr): " << this << std::endl;
    else
    {
        std::cerr << "Http::Http() cachefd!=0: " << this << std::endl;
        finalCache=new Cache(cachefd);
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

bool Http::tryConnect(const sockaddr_in6 &s,const std::string &host,const std::string &uri,const std::string &etag)
{
    this->host=host;
    this->uri=uri;
    this->etagBackend=etag;
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

void Http::resetRequestSended()
{
    if(http_code!=0)
        return;
    parsedHeader=false;
    contentsize=-1;
    contentwritten=0;
    parsing=Parsing_None;
    requestSended=false;
    contentLengthPos=-1;
    chunkLength=-1;
}

void Http::sendRequest()
{
    std::cout << "Http::sendRequest(): " << this << std::endl;
    requestSended=true;
    if(etagBackend.empty())
    {
        std::string h(std::string("GET ")+uri+" HTTP/1.1\nHOST: "+host+"\n\n");
        socketWrite(h.data(),h.size());
    }
    else
    {
        std::string h(std::string("GET ")+uri+" HTTP/1.1\nHOST: "+host+"\nIf-None-Match: "+etagBackend+"\n\n");
        socketWrite(h.data(),h.size());
    }
    /*used for retry host.clear();
    uri.clear();*/
}

char Http::randomETagChar(uint8_t r)
{
    const auto &l=sizeof(rChar);
    return rChar[r%l];
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
                            if(backend!=nullptr)
                                backend->wasTCPConnected=true;
                            if(!HttpReturnCode(http_code))
                            {
                                flushRead();
                                return;
                            }
                            pos++;
                        }
                    }
                    else
                        pos++;
                }
            }
            if(http_code!=200)
            {
                flushRead();
                return;
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
                        if((pos-pos2)==4)
                        {
                            std::string var(buffer+pos2,pos-pos2);
                            std::transform(var.begin(), var.end(), var.begin(),[](unsigned char c){return std::tolower(c);});
                            if(var=="etag")
                            {
                                parsing=Parsing_ETag;
                                pos++;
                                std::cout << "content-length" << std::endl;
                            }
                            else
                            {
                                parsing=Parsing_HeaderVal;
                                //std::cout << "1a) " << std::string(buffer+pos2,pos-pos2) << " (" << pos-pos2 << ")" << std::endl;
                                pos++;
                            }
                        }
                        else if((pos-pos2)==14)
                        {
                            std::string var(buffer+pos2,pos-pos2);
                            std::transform(var.begin(), var.end(), var.begin(),[](unsigned char c){return std::tolower(c);});
                            if(var=="content-length")
                            {
                                parsing=Parsing_ContentLength;
                                pos++;
                                std::cout << "content-length" << std::endl;
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
                                std::cout << "content-type" << std::endl;
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

                            //long filetime=0;
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

                            tempCache=new Cache(cachefd);
                            std::string r;
                            char randomIndex[6];
                            read(Http::fdRandom,randomIndex,sizeof(randomIndex));
                            r+=randomETagChar(randomIndex[r.size()]);
                            r+=randomETagChar(randomIndex[r.size()]);
                            r+=randomETagChar(randomIndex[r.size()]);
                            r+=randomETagChar(randomIndex[r.size()]);
                            r+=randomETagChar(randomIndex[r.size()]);
                            r+=randomETagChar(randomIndex[r.size()]);

                            const int64_t &currentTime=time(NULL);
                            tempCache->set_access_time(currentTime);
                            tempCache->set_last_modification_time_check(currentTime);
                            tempCache->set_http_code(http_code);
                            tempCache->set_ETagFrontend(r);//string of 6 char
                            tempCache->set_ETagBackend(etagBackend);//at end seek to content pos

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
                            /*else
                                header+="Transfer-Encoding: chunked\n";*/
                            if(!contenttype.empty())
                                header+="Content-Type: "+contenttype+"\n";
                            else
                                header+="Content-Type: text/html\n";
                            if(http_code==200)
                            {
                                    header+=
                                    "Date: "+timestampsToHttpDate(currentTime)+"\n"
                                    "Expires: "+timestampsToHttpDate(currentTime+Cache::timeToCache(http_code))+"\n"
                                    "Cache-Control: public\n"
                                    "ETag: \""+r+"\"\n"
                                    "Access-Control-Allow-Origin: *\n";
                            }
                            std::cout << "header: " << header << std::endl;
                            header+="\n";
                            tempCache->seekToContentPos();
                            if(tempCache->write(header.data(),header.size())!=(ssize_t)header.size())
                                abort();

                            epoll_event event;
                            memset(&event,0,sizeof(event));
                            event.data.ptr = tempCache;
                            event.events = EPOLLOUT;
                            //std::cerr << "EPOLL_CTL_ADD bis: " << cachefd << std::endl;

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
                                    std::cout << "content-length: " << value64 << std::endl;
                                }
                                break;
                                case Parsing_ContentType:
                                    contenttype=std::string(buffer+pos2,pos-pos2);
                                break;
                                case Parsing_ETag:
                                    etagBackend=std::string(buffer+pos2,pos-pos2);
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

    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
    if(cachePath.empty())
        abort();
    if(Http::pathToHttp.find(cachePath)==Http::pathToHttp.cend())
    {
        std::cerr << "Http::pathToHttp.find(" << cachePath << ")==Http::pathToHttp.cend()" << std::endl;
        abort();
    }
    #endif
    Http::pathToHttp.erase(cachePath);

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
        return false;
    }
    const std::string &errorString("Http "+std::to_string(errorCode));
    for(Client * client : clientsList)
        client->httpError(errorString);
    clientsList.clear();
    return false;
    //disconnectSocket();
}

bool Http::backendError(const std::string &errorString)
{
    for(Client * client : clientsList)
        client->httpError(errorString);
    clientsList.clear();
    return false;
    //disconnectSocket();
}

void Http::flushRead()
{
    disconnectBackend();
    disconnectFrontend();
    while(socketRead(Http::buffer,sizeof(Http::buffer))==sizeof(Http::buffer))
    {}
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

    if(finalCache!=nullptr)
        finalCache->close();
    const char * const cstr=cachePath.c_str();
    //todo, optimise with renameat2(RENAME_EXCHANGE) if --flatcache + destination
    if(tempCache!=nullptr)
    {
        tempCache->close();
        ::unlink(cstr);
        if(rename((cachePath+".tmp").c_str(),cstr)==-1)
            std::cerr << "unable to move " << cachePath << ".tmp to " << cachePath << ", errno: " << errno << std::endl;
        //disable to cache
        //::unlink(cstr);
    }

    backend->downloadFinished();
    std::cerr << this << ": http->backend=null" << std::endl;
    backend=nullptr;
}

void Http::addClient(Client * client)
{
    #ifdef DEBUGFASTCGI
    std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
    if(cachePath.empty())
        abort();
    if(Http::pathToHttp.find(cachePath)==Http::pathToHttp.cend())
    {
        std::cerr << "Http::pathToHttp.find(" << cachePath << ")==Http::pathToHttp.cend()" << std::endl;
        abort();
    }
    #endif
    clientsList.push_back(client);
    if(tempCache)
        client->startRead(cachePath+".tmp",true);
    if(backend==nullptr)
        abort();
    else
    {
        if(backend->http==nullptr)
            abort();
    }
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

int Http::write(const char * const data,const size_t &size)
{
    if(tempCache==nullptr)
    {
        //std::cerr << "tempCache==nullptr internal error" << std::endl;
        return size;
    }

    if(contentsize>=0)
    {
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
        #endif
        const size_t &writedSize=tempCache->write((char *)data,size);
        (void)writedSize;
        for(Client * client : clientsList)
            client->tryResumeReadAfterEndOfFile();
        contentwritten+=size;
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
        std::cerr << "contentsize: " << contentsize << ", contentwritten: " << contentwritten << std::endl;
        #endif
        if(contentsize<=contentwritten)
        {
            disconnectFrontend();
            disconnectBackend();
        }
    }
    else
    {
        #ifdef DEBUGFASTCGI
        std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
        #endif
        size_t pos=0;
        size_t pos2=0;
        //content-length: 5000
        if(http_code!=0)
        {
            #ifdef DEBUGFASTCGI
            std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
            #endif
            while(pos<size)
            {
                if(chunkLength>0)
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
                    #endif
                    if((size_t)chunkLength>(size-pos))
                    {
                        const size_t &writedSize=tempCache->write((char *)data+pos,size-pos);
                        (void)writedSize;
                        for(Client * client : clientsList)
                            client->tryResumeReadAfterEndOfFile();
                        contentwritten+=size;
                        pos+=size-pos;
                        pos2=pos;
                    }
                    else
                    {
                        const size_t &writedSize=tempCache->write((char *)data+pos,chunkLength);
                        (void)writedSize;
                        for(Client * client : clientsList)
                            client->tryResumeReadAfterEndOfFile();
                        contentwritten+=chunkLength;
                        pos+=chunkLength;
                        pos2=pos;
                    }
                    chunkLength=-1;
                }
                else
                {
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
                    #endif
                    while((size_t)pos<size)
                    {
                        char c=data[pos];
                        if(c=='\n' || c=='\r')
                        {
                            if(pos2==pos)
                            {
                                pos++;
                                pos2=pos;
                            }
                            else
                            {
                                #ifdef DEBUGFASTCGI
                                std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
                                #endif
                                if(chunkHeader.empty())
                                    chunkLength=Common::hexaTo64Bits(std::string(data+pos2,pos-pos2));
                                else
                                {
                                    chunkHeader+=std::string(data,pos-1);
                                    chunkLength=Common::hexaTo64Bits(chunkHeader);
                                }
                                #ifdef DEBUGFASTCGI
                                std::cerr << "chunkLength: " << chunkLength << std::endl;
                                #endif
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
                                break;
                            }
                        }
                        else
                            pos++;
                    }
                    #ifdef DEBUGFASTCGI
                    std::cerr << __FILE__ << ":" << __LINE__ << std::endl;
                    #endif
                    if(chunkLength==0)
                    {
                        disconnectFrontend();
                        disconnectBackend();
                    }
                    else if((size_t)pos>=size && chunkLength<0 && pos2<pos)
                        chunkHeader+=std::string(data+pos2,size-pos2);
                }
            }
        }
    }

    return size;
    //(write partial cache)
    //open to write .tmp (mv at end)
    //std::cout << std::string((const char *)data,size) << std::endl;
}

std::string Http::timestampsToHttpDate(const int64_t &time)
{
    char buffer[100];
    struct tm *my_tm = gmtime(&time);
    strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", my_tm);
    return buffer;
}
