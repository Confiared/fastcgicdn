#include "CurlMulti.hpp"
#include "Curl.hpp"
#include <stdlib.h>
#include <iostream>
#include <sys/timerfd.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

CurlMulti *CurlMulti::curlMulti=nullptr;

CurlMulti::CurlMulti()
{
    //new_conn(s, g); /* if we read a URL, go get it! */
    still_running=0;
    multi = curl_multi_init();

  /* setup the generic multi interface options we want */
  curl_multi_setopt(multi, CURLMOPT_SOCKETFUNCTION, &CurlMulti::sock_cb);
  //curl_multi_setopt(multi, CURLMOPT_SOCKETDATA, &g);
  curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION, &CurlMulti::multi_timer_cb);
  curl_multi_setopt(multi, CURLMOPT_TIMERDATA, this);

  fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if(fd == -1) {
    perror("timerfd_create failed");
    exit(1);
  }

  memset(&its, 0, sizeof(struct itimerspec));
  its.it_interval.tv_sec = 0;
  its.it_value.tv_sec = 1;
  timerfd_settime(fd, 0, &its, NULL);

  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.ptr = this;
  //std::cerr << "EPOLL_CTL_ADD multi: " << fd << std::endl;
  epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
}

CurlMulti::~CurlMulti()
{
}

void CurlMulti::parseEvent(const epoll_event &event)
{
    (void)event;
    /* Called by main loop when our timeout expires */
    CURLMcode rc;
    uint64_t count = 0;
    ssize_t err = 0;

    err = read(fd, &count, sizeof(uint64_t));
    if(err == -1) {
      /* Note that we may call the timer callback even if the timerfd isn't
       * readable. It's possible that there are multiple events stored in the
       * epoll buffer (i.e. the timer may have fired multiple times). The
       * event count is cleared after the first call so future events in the
       * epoll buffer will fail to read from the timer. */
      if(errno == EAGAIN) {
        std::cout << "EAGAIN on tfd " << fd << std::endl;
        return;
      }
    }
    if(err != sizeof(uint64_t)) {
      std::cerr << "read(fd) == " << err << std::endl;
      abort();
    }

    rc = curl_multi_socket_action(multi,CURL_SOCKET_TIMEOUT, 0, &still_running);
    mcode_or_die("timer_cb: curl_multi_socket_action", rc);
    check_multi_info();
}

/* Check for completed transfers, and remove their easy handles */
void CurlMulti::check_multi_info()
{
  char *eff_url;
  CURLMsg *msg;
  Curl *curl;
  int msgs_left;
  CURL *easy;
  CURLcode res;

  //std::cout << "REMAINING: " << still_running << std::endl;
  while((msg = curl_multi_info_read(multi, &msgs_left))) {
    if(msg->msg == CURLMSG_DONE) {
      easy = msg->easy_handle;
      res = msg->data.result;
      curl_easy_getinfo(easy, CURLINFO_PRIVATE, &curl);
      curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &eff_url);
      std::cout << "DONE: " << eff_url << " => (" << res << ") " << curl->error << std::endl;
      curl->curlError(res);
      curl->disconnect();
      curl_multi_remove_handle(multi, easy);
      curl_easy_cleanup(easy);
      delete curl;
    }
  }

  if(CurlMulti::curlMulti->still_running <= 0) {
    std::cout << "last transfer done, kill timeout" << std::endl;
    memset(&its, 0, sizeof(struct itimerspec));
    timerfd_settime(CurlMulti::curlMulti->fd, 0, &its, NULL);
  }
}

/* Called by libevent when we get action on a multi socket filedescriptor*/
int CurlMulti::multi_timer_cb(CURLM *multi, long timeout_ms)
{
    (void)multi;
    //std::cout << "multi_timer_cb: Setting timeout to " << timeout_ms << " ms" << std::endl;

    if(timeout_ms > 0) {
        CurlMulti::curlMulti->its.it_interval.tv_sec = 0;
        CurlMulti::curlMulti->its.it_interval.tv_nsec = 0;
        CurlMulti::curlMulti->its.it_value.tv_sec = timeout_ms / 1000;
        CurlMulti::curlMulti->its.it_value.tv_nsec = (timeout_ms % 1000) * 1000 * 1000;
    }
    else if(timeout_ms == 0) {
        /* libcurl wants us to timeout now, however setting both fields of
         * new_value.it_value to zero disarms the timer. The closest we can
         * do is to schedule the timer to fire in 1 ns. */
        CurlMulti::curlMulti->its.it_interval.tv_sec = 0;
        CurlMulti::curlMulti->its.it_interval.tv_nsec = 0;
        CurlMulti::curlMulti->its.it_value.tv_sec = 0;
        CurlMulti::curlMulti->its.it_value.tv_nsec = 1;
    }
    else
        memset(&CurlMulti::curlMulti->its, 0, sizeof(struct itimerspec));

    timerfd_settime(CurlMulti::curlMulti->fd, /*flags=*/0, &CurlMulti::curlMulti->its, NULL);
    return 0;
}

/* CURLMOPT_SOCKETFUNCTION */
int CurlMulti::sock_cb(CURL *e, curl_socket_t s, int what, void *cbp, void *sockp)
{
    (void)cbp;
    //GlobalInfo *g = (GlobalInfo*) cbp;
    Curl *fdp = (Curl*) sockp;
    const char *whatstr[]={ "none", "IN", "OUT", "INOUT", "REMOVE" };

    std::cout << "socket callback: s=" << s << " e=" << e << " what=" << whatstr[what] << std::endl;
    if(what == CURL_POLL_REMOVE) {
        std::cout << "REMOVE" << std::endl;
        fdp->disconnectSocket();
        //delete fdp;
    }
    else {
        if(!fdp) {
            std::cout << "2) CURL pointer " << e << std::endl;
            std::cout << "Adding data: " << whatstr[what] << std::endl;
            if(curl_easy_getinfo(e, CURLINFO_PRIVATE, &fdp)!=CURLE_OK)
                abort();
            std::cout << "2) CURL resolved " << fdp << std::endl;
            fdp->setsock(s, e, what);
            curl_multi_assign(CurlMulti::curlMulti->multi, s, fdp);
        }
        else {
            if(curl_easy_getinfo(e, CURLINFO_PRIVATE, &fdp)!=CURLE_OK)
                abort();
            std::cout << "3) CURL resolved " << fdp << std::endl;
            std::cout << "Changing action from " << whatstr[fdp->getAction()] << " to " << whatstr[what] << std::endl;
            fdp->setsock(s, e, what);
        }
    }
    return 0;
}

/* CURLOPT_WRITEFUNCTION */
size_t CurlMulti::write_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
    Curl *curl=(Curl *)data;
    return curl->write(ptr,size * nmemb);
}

/* CURLOPT_HEADERFUNCTION */
size_t CurlMulti::header_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
    Curl *curl=(Curl *)data;
    return curl->header(ptr,size * nmemb);
}

#define mycase(code) \
  case code: s = __STRING(code)

/* Die if we get a bad CURLMcode somewhere */
void CurlMulti::mcode_or_die(const char *where, CURLMcode code)
{
  if(CURLM_OK != code) {
    const char *s;
    switch(code) {
      mycase(CURLM_BAD_HANDLE); break;
      mycase(CURLM_BAD_EASY_HANDLE); break;
      mycase(CURLM_OUT_OF_MEMORY); break;
      mycase(CURLM_INTERNAL_ERROR); break;
      mycase(CURLM_UNKNOWN_OPTION); break;
      mycase(CURLM_LAST); break;
      default: s = "CURLM_unknown"; break;
      mycase(CURLM_BAD_SOCKET);
      std::cout << "ERROR: " << where << " returns " << s << std::endl;
      /* ignore this error */
      return;
    }
    std::cout << "ERROR: " << where << " returns " << s << " (abort)" << std::endl;
    abort();
  }
}

/* Create a new easy handle, and add it to the global curl_multi */
Curl *CurlMulti::download(const char * const url, const char * const cachePath,const int &cachefd/*0 if no old cache file found*/)
{
    Curl *curl=new Curl(cachefd,cachePath);
    CURLMcode rc;
    curl_easy_setopt(curl->getEasy(), CURLOPT_URL, url);
    curl_easy_setopt(curl->getEasy(), CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl->getEasy(), CURLOPT_WRITEDATA, curl);
    curl_easy_setopt(curl->getEasy(), CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl->getEasy(), CURLOPT_HEADERDATA, curl);
    curl_easy_setopt(curl->getEasy(), CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(curl->getEasy(), CURLOPT_ERRORBUFFER, curl->error);
    if(curl_easy_setopt(curl->getEasy(), CURLOPT_PRIVATE, (void *)curl)!=CURLE_OK)
        abort();
    //curl_easy_setopt(curl->getEasy(), CURLOPT_PROGRESSDATA, curl);
    /*curl_easy_setopt(curl->getEasy(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl->getEasy(), CURLOPT_LOW_SPEED_TIME, 3L);
    curl_easy_setopt(curl->getEasy(), CURLOPT_LOW_SPEED_LIMIT, 10L);*/
    curl_easy_setopt(curl->getEasy(), CURLOPT_FILETIME, 1L);
    std::cout << "1) curl pointer " << curl << " to " << curl->getEasy() << std::endl;

    if(curl->get_mtime()>0)
    {
        /* January 1, 2020 is 1577833200 */
        curl_easy_setopt(curl, CURLOPT_TIMEVALUE, curl->get_mtime());
        /* If-Modified-Since the above time stamp */
        curl_easy_setopt(curl, CURLOPT_TIMECONDITION, CURL_TIMECOND_IFMODSINCE);
    }

    //optimise:
        curl_easy_setopt(curl->getEasy(), CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl->getEasy(), CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl->getEasy(), CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:78.0) Gecko/20100101 Firefox/78.0");
    //curl_easy_setopt(curl->getEasy(), CURLOPT_RETURNTRANSFER, 1L);
    curl_easy_setopt(curl->getEasy(), CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V6);

    std::cout << "Adding easy " << curl->getEasy() << " to multi " << multi << " (" << url << ")" << std::endl;
    rc = curl_multi_add_handle(multi, curl->getEasy());
    mcode_or_die("new_conn: curl_multi_add_handle", rc);

    /* note that the add_handle() will set a time-out to trigger very soon so
    that the necessary socket_action() call will be called by this app */
    return curl;
}
