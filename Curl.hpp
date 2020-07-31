#ifndef CURL_H
#define CURL_H

#include "EpollObject.hpp"

#include <curl/curl.h>
#include <string>
#include <vector>

class Cache;
class Client;

class Curl : public EpollObject
{
public:
    Curl(const int &cachefd,//0 if no old cache file found
         const char * const path);
    ~Curl();
    void parseEvent(const epoll_event &event) override;
    void disconnect();
    void disconnectSocket();
    void setsock(curl_socket_t s, CURL *e, int act);
    const int &getAction() const;
    CURL * getEasy() const;
    int write(const void * const data, const size_t &size);
    int header(const void * const data, const size_t &size);
    const int64_t &get_mtime() const;
    static std::string timestampsToHttpDate(const int64_t &time);
    void addClient(Client * client);
    void curlError(const CURLcode &errorCode);
public:
    char error[CURL_ERROR_SIZE];
private:
    std::vector<Client *> clientsList;
    std::string cachePath;
    Cache *tempCache;
    CURL *easy;
    int act;
    bool parsedHeader;
    bool firstWrite;
    //if 0 then the cache is tmp file
    int64_t mtime;

    std::string contenttype;
    uint64_t contentsize;
};

#endif // CURL_H
