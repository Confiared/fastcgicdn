#ifndef Dns_H
#define Dns_H

#include "EpollObject.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>

class Client;

class Dns : public EpollObject
{
public:
    Dns();
    ~Dns();
    void parseEvent(const epoll_event &event) override;
    inline bool canAddToPos(const int &i, const int &size, int &pos);
    inline bool read8Bits(uint8_t &var, const char * const data, const int &size, int &pos);
    inline bool read16Bits(uint16_t &var, const char * const data, const int &size, int &pos);
    inline bool read32Bits(uint32_t &var, const char * const data, const int &size, int &pos);
    bool tryOpenSocket(std::string line);
    bool get(Client * client,const std::string &host);
    void cancelClient(Client * client,const std::string &host);
    int requestCountMerged();
    static Dns *dns;
private:
    sockaddr_in6 targetDnsIPv6;
    sockaddr_in targetDnsIPv4;
    enum Mode : uint8_t
    {
        Mode_IPv6=0x00,
        Mode_IPv4=0x01,
    };
    Mode mode;
    uint16_t increment;

    struct Query {
        int64_t startTime;
        std::vector<Client *> clients;
    };
    int clientInProgress;
    std::unordered_map<std::string,Query> queryList;
};

#endif // Dns_H
