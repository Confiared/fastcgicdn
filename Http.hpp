#ifndef HTTP_H
#define HTTP_H

#include <string>
#include <vector>
#include <netinet/in.h>
#include <unordered_map>

#include "EpollObject.hpp"
#include "Backend.hpp"

class Client;
class Cache;

class Http
{
public:
    Http(const int &cachefd,//0 if no old cache file found
         const std::string &cachePath);
    virtual ~Http();
    bool tryConnect(const sockaddr_in6 &s, const std::string &host, const std::string &uri, const std::string &etagBackend=std::string());
    bool tryConnectInternal(const sockaddr_in6 &s);
    void parseEvent(const epoll_event &event);
    static char randomETagChar(uint8_t r);
    void sendRequest();
    void readyToRead();
    void readyToWrite();
    void flushRead();
    void disconnectFrontend();
    void disconnectBackend();
    const int &getAction() const;
    virtual int write(const char * const data, const size_t &size);
    static std::string timestampsToHttpDate(const int64_t &time);
    void addClient(Client * client);
    void removeClient(Client * client);
    const std::string &getCachePath() const;
    void resetRequestSended();
    bool HttpReturnCode(const int &errorCode);
    bool backendError(const std::string &errorString);
    void parseNonHttpError(const Backend::NonHttpError &error);

    ssize_t socketRead(void *buffer, size_t size);
    bool socketWrite(const void *buffer, size_t size);
public:
    static std::unordered_map<std::string,Http *> pathToHttp;
    static int fdRandom;
private:
    static char buffer[4096];
private:
    std::vector<Client *> clientsList;
    std::string cachePath;
    Cache *tempCache;
    Cache *finalCache;
    bool parsedHeader;

    std::string contenttype;
    std::string url;
    int64_t contentsize;
    int64_t contentwritten;
    std::string headerBuff;
    uint16_t http_code;
    enum Parsing: uint8_t
    {
        Parsing_None,
        Parsing_HeaderVar,
        Parsing_HeaderVal,
        Parsing_ContentLength,
        Parsing_ContentType,
        Parsing_ETag,
        Parsing_Content
    };
    Parsing parsing;

    std::string host;
    std::string uri;
    std::string etagBackend;
public:
    bool requestSended;
    Backend *backend;
    int64_t contentLengthPos;
    int64_t chunkLength;
    std::string chunkHeader;
private:
    sockaddr_in6 m_socket;
};

#endif // HTTP_H
