#ifndef Client_H
#define Client_H

#include "EpollObject.hpp"
#include <string>

class Cache;

class Client : public EpollObject
{
public:
    Client(int cfd);
    ~Client();
    void parseEvent(const epoll_event &event) override;
    void disconnect();

    void dnsRight();
    void dnsError();
    void dnsWrong();

    static inline uint8_t hexToDecUnit(const char& c, bool &ok);
    static std::string hexaToBinary(const std::string &hexa);
    void readyToRead();
    inline bool canAddToPos(const int &i,const int &size,int &pos);
    inline bool read8Bits(uint8_t &var,const char * const data,const int &size,int &pos);
    inline bool read16Bits(uint16_t &var,const char * const data,const int &size,int &pos);
    static std::string binarytoHexa(const char * const data, const uint32_t &size);

    void readyToWrite();
    void write(const char * const data,const int &size);
    void writeOutput(const char * const data,const int &size);
    void writeEnd();

    void startRead();
    void startRead(const std::string &path, const bool &partial);
    void continueRead();

    enum Status : uint8_t
    {
        Status_Idle=0x00,
        Status_WaitDns=0x01,
        Status_WaitTheContent=0x02,
    };
private:
    int fastcgiid;
    Cache *readCache;
    std::string dataToWrite;
    bool fullyParsed;
    bool endTriggered;
    Status status;
    bool https;
    bool partial;
    std::string uri;
    std::string host;
};

#endif // Client_H
