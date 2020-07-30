#ifndef CURLMulti_H
#define CURLMulti_H

#include <curl/curl.h>
#include <unordered_map>
#include "EpollObject.hpp"

class Curl;

class CurlMulti : public EpollObject
{
public:
    CurlMulti();
    ~CurlMulti();
    static CurlMulti *curlMulti;
    void parseEvent(const epoll_event &event) override;
    Curl * download(const char * const url, const char * const cachePath,const int &cachefd/*0 if no old cache file found*/);

    void check_multi_info();
    static int multi_timer_cb(CURLM *multi, long timeout_ms);
    static int sock_cb(CURL *e, curl_socket_t s, int what, void *cbp, void *sockp);
    static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *data);
    static size_t header_cb(void *ptr, size_t size, size_t nmemb, void *data);
    /* Die if we get a bad CURLMcode somewhere */
    void mcode_or_die(const char *where, CURLMcode code);
public:
    CURLM *multi;
    int still_running;
    struct itimerspec its;
    std::unordered_map<std::string,Curl *> pathToCurl;
};

#endif // CURL_H
