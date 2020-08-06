#include "Client.hpp"
#include "Curl.hpp"
#include "Dns.hpp"
#include "Cache.hpp"
#include "CurlMulti.hpp"
#include <unistd.h>
#include <iostream>
#include <string.h>
//#include <xxhash.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <xxh3.h>
#include <sstream>

#define DEBUGDNS
static const char* const lut = "0123456789ABCDEF";

Client::Client(int cfd) :
    fastcgiid(-1),
    readCache(nullptr),
    curl(nullptr),
    fullyParsed(false),
    endTriggered(false),
    status(Status_Idle),
    https(false),
    partial(false),
    partialEndOfFileTrigged(false),
    outputWrited(false)
{
    this->kind=EpollObject::Kind::Kind_Client;
    this->fd=cfd;
}

Client::~Client()
{
    if(readCache!=nullptr)
    {
        readCache->close();
        delete readCache;
        readCache=nullptr;
    }
    if(curl)
        curl->removeClient(this);
}

void Client::parseEvent(const epoll_event &event)
{
    if(event.events & EPOLLIN)
        readyToRead();
    if(event.events & EPOLLOUT)
        readyToWrite();
}

void Client::disconnect()
{
    if(fd!=-1)
    {
        //std::cerr << fd << " disconnect()" << std::endl;
        //std::cout << "disconnect()" << std::endl;
        epoll_ctl(epollfd,EPOLL_CTL_DEL, fd, NULL);
        ::close(fd);
        fd=-1;
    }
    if(curl)
        curl->removeClient(this);
    dataToWrite.clear();
    if(status==Status_WaitDns)
        Dns::dns->cancelClient(this,host);
    fastcgiid=-1;
}

uint8_t Client::hexToDecUnit(const char& c, bool &ok)
{
    if(c<48)
    {
        ok=false;
        return 0;
    }
    if(c<=57)
    {
        ok=true;
        return c-48;
    }
    if(c<65)
    {
        ok=false;
        return 0;
    }
    if(c<=70)
    {
        ok=true;
        return c-65+10;
    }
    if(c<97)
    {
        ok=false;
        return 0;
    }
    if(c<=102)
    {
        ok=true;
        return c-(uint8_t)97+10;
    }
    ok=false;
    return 0;
}

std::string Client::hexaToBinary(const std::string &hexa)
{
    if(hexa.size()%2!=0)
        return std::string();
    std::string r;
    r.resize(hexa.size()/2);
    unsigned int index=0;
    while(index<r.size())
    {
        bool ok=true;
        const uint8_t c1=hexToDecUnit(hexa.at(index*2),ok);
        if(!ok)
            return std::string();
        const uint8_t c2=hexToDecUnit(hexa.at(index*2+1),ok);
        if(!ok)
            return std::string();
        r[index]=c1*16+c2;
        index++;
    }
    return r;
}

void Client::readyToRead()
{
    if(fullyParsed)
        return;

    char buff[4096];
    const int size=read(fd,buff,sizeof(buff));
    if(size<=0)
        return;

    https=false;
    uri.clear();
    host.clear();
    //all is big endian
    int pos=0;
    uint8_t var8=0;
    uint16_t var16=0;

    /*{
        std::cerr << fd << ") ";
        const char* const lut = "0123456789ABCDEF";
        for(int i=0;i<size;++i)
        {
            const unsigned char c = buff[i];
            std::cerr << lut[c >> 4];
            std::cerr << lut[c & 15];
        }
        std::cerr << std::endl;
    }*/

    do
    {
        if(!read8Bits(var8,buff,size,pos))
            return;
        if(var8!=1)
        {
            disconnect();
            return;
        }
        if(!read8Bits(var8,buff,size,pos))
            return;
        if(fastcgiid==-1)
        {
            if(var8!=1)
            {
                disconnect();
                return;
            }
            if(!read16Bits(var16,buff,size,pos))
                return;
            fastcgiid=var16;
        }
        else
        {
            if(var8!=4 && var8!=5)
            {
                disconnect();
                return;
            }
            if(!read16Bits(var16,buff,size,pos))
                return;
            if(fastcgiid!=var16)
            {
                disconnect();
                return;
            }
        }
        uint16_t contentLenght=0;
        uint8_t paddingLength=0;
        if(!read16Bits(contentLenght,buff,size,pos))
            return;
        if(!read8Bits(paddingLength,buff,size,pos))
            return;
        if(!canAddToPos(1,size,pos))
            return;
        switch (var8) {
        //FCGI_BEGIN_REQUEST
        case 1:
            //skip the content length + padding length
            if(!canAddToPos(contentLenght+paddingLength,size,pos))
                return;
        break;
        //FCGI_PARAMS
        case 4:
        {
            int contentLenghtAbs=contentLenght+pos;
            while(pos<contentLenghtAbs)
            {
                uint8_t varSize=0;
                if(!read8Bits(varSize,buff,size,pos))
                    return;
                uint8_t valSize=0;
                if(!read8Bits(valSize,buff,size,pos))
                    return;
                switch(varSize)
                {
                    case 9:
                    if(memcmp(buff+pos,"HTTP_HOST",9)==0)
                        host=std::string(buff+pos+varSize,valSize);
                    break;
                    case 11:
                    if(memcmp(buff+pos,"REQUEST_URI",11)==0)
                        uri=std::string(buff+pos+varSize,valSize);
                    /*else if(memcmp(buff+pos,"SERVER_PORT",11)==0 && valSize==3)
                        if(memcmp(buff+pos+varSize,"443",3)==0)
                            https=true;*/
                    break;
                    case 14:
                    if(memcmp(buff+pos,"REQUEST_SCHEME",14)==0 && valSize==5)
                        if(memcmp(buff+pos+varSize,"https",5)==0)
                            https=true;
                    break;
                    default:
                    break;
                }
                //std::cout << std::string(buff+pos,varSize) << ": " << std::string(buff+pos+varSize,valSize) << std::endl;
                if(!canAddToPos(varSize+valSize,size,pos))
                    return;
            }
            if(!canAddToPos(paddingLength,size,pos))
                return;
        }
        break;
        //FCGI_STDIN
        case 5:
            //skip the content length + padding length
            if(!canAddToPos(contentLenght+paddingLength,size,pos))
                return;
            fullyParsed=true;
        break;
        default:
            break;
        }
    } while(pos<size);

    if(!fullyParsed)
        return;

    //check if robots.txt
    if(uri=="/robots.txt")
    {
        char text[]="X-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nUser-agent: *\r\nDisallow: /";
        writeOutput(text,sizeof(text)-1);
        writeEnd();
        return;
    }
    //check if robots.txt
    if(uri=="/favicon.ico")
    {
        char text[]="X-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nDropped for now";
        writeOutput(text,sizeof(text)-1);
        writeEnd();
        return;
    }

    //resolv the host or from subdomain or from uri
    {
        //resolv final url (hex, https, ...)
        const size_t &pos=host.rfind(".confiared.com");
        const size_t &mark=(host.size()-14);
        #ifdef DEBUGDNS
        const size_t &posdebug=host.rfind(".bolivia-online.com");
        const size_t &markdebug=(host.size()-19);
        if(pos==mark || posdebug==markdebug)
        {
            std::string hostb;
            if(pos==mark)
                hostb=host.substr(0,mark);
            else
                hostb=host.substr(0,markdebug);
        #else
        if(pos==mark)
        {
            std::string hostb=host.substr(0,mark);
        #endif

            size_t posb=hostb.rfind("cdn");
            size_t markb=(hostb.size()-3);
            if(posb==markb)
            {
                if(markb>1)
                    host=hexaToBinary(hostb.substr(0,markb-1));
                else if(markb==0)
                {
                    const size_t poss=uri.find("/",1);
                    if(poss!=std::string::npos)
                    {
                        if(poss>2)
                        {
                            host=uri.substr(1,poss-1);
                            uri=uri.substr(poss);
                        }
                    }
                    else
                    {
                        //std::cerr << "uri '/' not found " << uri << ", host: " << host << std::endl;
                        char text[]="X-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nCDN bad usage: contact@confiared.com";
                        writeOutput(text,sizeof(text)-1);
                        writeEnd();
                        return;
                    }
                }
            }
            else
            {
                markb=(hostb.size()-4);
                posb=hostb.rfind("cdn1");
                if(posb!=markb)
                    posb=hostb.rfind("cdn2");
                if(posb!=markb)
                    posb=hostb.rfind("cdn3");
                if(posb==markb)
                {
                    if(markb>1)
                        host=hexaToBinary(hostb.substr(0,markb-1));
                    else if(markb==0)
                    {
                        const size_t poss=uri.find("/",1);
                        if(poss!=std::string::npos)
                        {
                            if(poss>2)
                            {
                                host=uri.substr(1,poss-1);
                                uri=uri.substr(poss);
                            }
                        }
                        else
                        {
                            //std::cerr << "uri '/' not found " << uri << ", host: " << host << std::endl;
                            char text[]="X-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nCDN bad usage: contact@confiared.com";
                            writeOutput(text,sizeof(text)-1);
                            writeEnd();
                            return;
                        }
                    }
                }
            }
        }
    }

    //if have request
/*    if(https)
        std::cout << "downloading: https://" << host << uri << std::endl;
    else
        std::cout << "downloading: http://" << host << uri << std::endl;*/
    (void)https;

    if(host.empty())
    {
        char text[]="X-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nCDN bad usage: contact@confiared.com";
        writeOutput(text,sizeof(text)-1);
        writeEnd();
        return;
    }

    //get AAAA entry for host
    if(!Dns::dns->get(this,host))
    {
        char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nOverloaded CDN Dns";
        writeOutput(text,sizeof(text)-1);
        writeEnd();
        return;
    }
    status=Status_WaitDns;
}

bool Client::canAddToPos(const int &i, const int &size, int &pos)
{
    if((pos+i)>size)
    {
        disconnect();
        return false;
    }
    pos+=i;
    return true;
}

bool Client::read8Bits(uint8_t &var, const char * const data, const int &size, int &pos)
{
    if((pos+(int)sizeof(var))>size)
    {
        disconnect();
        return false;
    }
    var=data[pos];
    pos+=sizeof(var);
    return true;
}

bool Client::read16Bits(uint16_t &var, const char * const data, const int &size, int &pos)
{
    if((pos+(int)sizeof(var))>size)
    {
        disconnect();
        return false;
    }
    uint16_t t;
    memcpy(&t,data+pos,sizeof(var));
    var=be16toh(t);
    pos+=sizeof(var);
    return true;
}

std::string Client::binarytoHexa(const char * const data, const uint32_t &size)
{
    std::string output;
    //output.reserve(2*size);
    for(size_t i=0;i<size;++i)
    {
        const unsigned char c = data[i];
        output.push_back(lut[c >> 4]);
        output.push_back(lut[c & 15]);
    }
    return output;
}

void Client::dnsError()
{
    dnsRight();return;
    status=Status_Idle;
    char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nDns Error";
    writeOutput(text,sizeof(text)-1);
    writeEnd();
}

void Client::dnsWrong()
{
    dnsRight();return;
    status=Status_Idle;
    char text[]="Status: 403 Forbidden\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nThis site DNS (AAAA entry) is not into Confiared IPv6 range";
    writeOutput(text,sizeof(text)-1);
    writeEnd();
}

void Client::dnsRight()
{
    /* Each Hours clean cache (to defined better)
     *
     * When site have poor cache hit:
     * What file remove?
     * - Used one time + oldest than 1h
     *
     * When site have too many file:
     * While files count > 10000
     * What file remove?
     * - smallest (modification - access time)
     *
     * When site have too many disk space usage:
     * While site size > 1% partition size
     * What file remove?
     * - biggest: sort(File size/(time()-accesstime))
     * atime (access time) - The last time the file was accessed/opened by some command or application such as cat , vim or grep .
     *
     * Try: 2 list: oldest usage/size
     *
     * Try: Machine Learning Based Cache Algorithm
     * */

    status=Status_WaitTheContent;
    partial=false;
    std::string hostwithprotocol=host;
    if(https)
        hostwithprotocol+="s";

    std::string path("cache/");
    std::string folder;
    if(Cache::hostsubfolder)
    {
        const uint32_t &hashhost=static_cast<uint32_t>(XXH3_64bits(hostwithprotocol.data(),hostwithprotocol.size()));
        const XXH64_hash_t &hashuri=XXH3_64bits(uri.data(),uri.size());

        //do the hash for host to define cache subfolder, hash for uri to file

        //std::string folder;
        folder = binarytoHexa(reinterpret_cast<const char *>(&hashhost),sizeof(hashhost));
        path+=folder+"/";

        const std::string urifolder = binarytoHexa(reinterpret_cast<const char *>(&hashuri),sizeof(hashuri));
        path+=urifolder;
    }
    else
    {
        XXH3_state_t state;
        XXH3_64bits_reset(&state);
        XXH3_64bits_update(&state, hostwithprotocol.data(),hostwithprotocol.size());
        XXH3_64bits_update(&state, uri.data(),uri.size());
        const XXH64_hash_t &hashuri=XXH3_64bits_digest(&state);

        const std::string urifolder = binarytoHexa(reinterpret_cast<const char *>(&hashuri),sizeof(hashuri));
        path+=urifolder;
    }

    if(CurlMulti::curlMulti->pathToCurl.find(path)!=CurlMulti::curlMulti->pathToCurl.cend())
    {
        Curl *curl=CurlMulti::curlMulti->pathToCurl.at(path);
        curl->addClient(this);//into this call, start open cache and stream if partial have started
    }
    else
    {
        std::string url;
        if(https)
            url="https://";
        else
            url="http://";
        url+=host;
        url+=uri;
        //try open cache
        //std::cerr << "open((path).c_str() " << path << std::endl;
        int cachefd = open(path.c_str(), O_RDWR | O_NOCTTY/* | O_NONBLOCK*/, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        //if failed open cache
        if(cachefd==-1)
        {
            std::cerr << "can't open cache file " << path << " due to errno: " << errno << std::endl;
            if(Cache::hostsubfolder)
                ::mkdir(("cache/"+folder).c_str(),S_IRWXU);
            curl=CurlMulti::curlMulti->download(url,path,0);
            curl->addClient(this);//into this call, start open cache and stream if partial have started
        }
        else
        {
            /* cachePath (content header, 64Bits aligned):
             * 64Bits: access time
             * 64Bits: last modification time check
             * 64Bits: http code
             * 64Bits: modification time
             */

            uint64_t lastModificationTimeCheck=0;
            const off_t &s=lseek(cachefd,1*sizeof(uint64_t),SEEK_SET);
            if(s!=-1)
            {
                if(::read(cachefd,&lastModificationTimeCheck,sizeof(lastModificationTimeCheck))!=sizeof(lastModificationTimeCheck))
                    lastModificationTimeCheck=0;
            }
            uint64_t http_code=500;
            const off_t &s2=lseek(cachefd,2*sizeof(uint64_t),SEEK_SET);
            if(s2!=-1)
            {
                if(::read(cachefd,&http_code,sizeof(http_code))!=sizeof(http_code))
                    http_code=500;
            }
            //last modification time check <24h or in future to prevent time drift
            const uint64_t &currentTime=time(NULL);
            if(lastModificationTimeCheck>(currentTime-Cache::timeToCache(http_code)))
            {
                const off_t &s=lseek(cachefd,1*sizeof(uint64_t),SEEK_SET);
                if(s!=-1)
                {
                    if(readCache!=nullptr)
                    {
                        delete readCache;
                        readCache=nullptr;
                    }
                    readCache=new Cache(cachefd);
                    readCache->set_access_time(currentTime);
                    startRead();
                    return;
                }
            }
            if(Cache::hostsubfolder)
                ::mkdir(("cache/"+folder).c_str(),S_IRWXU);
            curl=CurlMulti::curlMulti->download(url,path,cachefd);
            curl->addClient(this);//into this call, start open cache and stream if partial have started
        }
    }
}

void Client::startRead()
{
    if(!readCache->setContentPos())
    {
        status=Status_Idle;
        char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nUnable to read cache (1)";
        writeOutput(text,sizeof(text)-1);
        writeEnd();
        return;
    }
    readCache->setAsync();
    continueRead();
}

void Client::startRead(const std::string &path, const bool &partial)
{
    this->partial=partial;
    int cachefd = open(path.c_str(), O_RDWR | O_NOCTTY, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    //if failed open cache
    if(cachefd==-1)
    {
        status=Status_Idle;
        char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nUnable to read cache (2)";
        writeOutput(text,sizeof(text)-1);
        writeEnd();
        return;
    }
    const off_t &s=lseek(cachefd,1*sizeof(uint64_t),SEEK_SET);
    if(s!=-1)
    {
        const uint64_t &currentTime=time(NULL);
        if(readCache!=nullptr)
        {
            delete readCache;
            readCache=nullptr;
        }
        readCache=new Cache(cachefd);
        readCache->set_access_time(currentTime);
    }
    startRead();
}

void Client::tryResumeReadAfterEndOfFile()
{
    if(partialEndOfFileTrigged)
        continueRead();
}

void Client::continueRead()
{
    if(readCache==nullptr)
        return;
    if(!dataToWrite.empty())
        return;
    char buffer[65536-1000];
    do {
        const ssize_t &s=readCache->read(buffer,sizeof(buffer));
        if(s<1)
        {
            if(!partial)
                writeEnd();
            else
            {
                partialEndOfFileTrigged=true;
                //std::cout << "End of file, wait more" << std::endl;
            }
            return;
        }
        partialEndOfFileTrigged=false;
        writeOutput(buffer,s);
        //if can't write all
        if(!dataToWrite.empty())
            return;
        //if can write all, try again
    } while(1);
}

void Client::cacheError()
{
    status=Status_Idle;
    char text[]="Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nCache file rror";
    writeOutput(text,sizeof(text)-1);
    writeEnd();
}

void Client::readyToWrite()
{
    if(!dataToWrite.empty())
    {
        const ssize_t writedSize=::write(fd,dataToWrite.data(),dataToWrite.size());
        if(errno!=0 && errno!=EAGAIN)
        {
            disconnect();
            return;
        }
        if(writedSize>=0)
            if((size_t)writedSize==dataToWrite.size())
                //event to continue to read file
                return;
        dataToWrite.erase(0,writedSize);

        if(endTriggered==true)
        {
            endTriggered=false;
            disconnect();
        }

        return;
    }
    continueRead();
}

void Client::writeOutput(const char * const data,const int &size)
{
    outputWrited=true;
    uint16_t padding=0;//size-size%16;
    uint16_t paddingbe=htobe16(padding);

    char header[1+1+2+2+2];
    header[0]=1;
    //FCGI_STDOUT
    header[1]=6;
    uint16_t idbe=htobe16(fastcgiid);
    memcpy(header+1+1,&idbe,2);
    uint16_t sizebe=htobe16(size);
    memcpy(header+1+1+2,&sizebe,2);
    memcpy(header+1+1+2+2,&paddingbe,2);
    write(header,1+1+2+2+2);
    write(data,size);

    if(padding>0)
    {
        char t[padding];
        write(t,padding);
    }
}

void Client::write(const char * const data,const int &size)
{
    if(fd==-1)
        return;
    if(!dataToWrite.empty())
    {
        dataToWrite+=std::string(data,size);
        return;
    }
    /*{
        std::cerr << fd << " write) ";
        if(size>255)
            std::cerr << "size: " << size;
        else
        {
            const char* const lut = "0123456789ABCDEF";
            for(int i=0;i<size;++i)
            {
                const unsigned char c = data[i];
                std::cerr << lut[c >> 4];
                std::cerr << lut[c & 15];
            }
        }
        std::cerr << std::endl;
    }*/

    errno=0;
    const int writedSize=::write(fd,data,size);
    if(writedSize==size)
        return;
    else if(errno!=0 && errno!=EAGAIN)
    {
        std::cerr << fd << ") error to write: " << errno << std::endl;
        disconnect();
        return;
    }
    if(errno==EAGAIN)
    {
        dataToWrite+=std::string(data+writedSize,size-writedSize);
        return;
    }
    else
    {
        disconnect();
        return;
    }
}

void Client::curlError(const std::string &errorString)
{
    const std::string &fullContent=
            "Status: 500 Internal Server Error\r\nX-Robots-Tag: noindex, nofollow\r\nContent-type: text/plain\r\n\r\nError: "+
            errorString;
    writeOutput(fullContent.data(),fullContent.size());
    writeEnd();
}

void Client::writeEnd()
{
    if(!outputWrited)
        return;
    if(partial)
        continueRead();
    char header[1+1+2+2+2+4+4];
    header[0]=1;
    //FCGI_END_REQUEST
    header[1]=3;
    uint16_t idbe=htobe16(fastcgiid);
    memcpy(header+1+1,&idbe,2);
    uint16_t sizebe=htobe16(8);
    memcpy(header+1+1+2,&sizebe,2);
    uint16_t padding=0;
    memcpy(header+1+1+2+2,&padding,2);
    uint32_t applicationStatus=0;
    memcpy(header+1+1+2+2+2,&applicationStatus,4);
    uint32_t protocolStatus=0;
    memcpy(header+1+1+2+2+2+4,&protocolStatus,4);

    if(!dataToWrite.empty())
    {
       dataToWrite+=std::string(header,sizeof(header));
       endTriggered=true;
       return;
    }
    if(readCache!=nullptr)
    {
        readCache->close();
        delete readCache;
        readCache=nullptr;
    }

    write(header,sizeof(header));

    fastcgiid=-1;
    if(dataToWrite.empty())
        disconnect();
    else
        endTriggered=true;
}
