fastcgicdn: Cache.cpp Client.cpp Curl.cpp CurlMulti.cpp Dns.cpp EpollObject.cpp main.cpp Server.cpp Timer.cpp Timer/DNSCache.cpp Timer/DNSQuery.cpp
#	g++ -o fastcgicdn -DDEBUGDNS -DDEBUGFASTCGI -g Cache.cpp Client.cpp Curl.cpp CurlMulti.cpp Dns.cpp EpollObject.cpp main.cpp Server.cpp Timer.cpp Timer/DNSCache.cpp Timer/DNSQuery.cpp -lcurl -std=c++11 -I.
	g++ -o fastcgicdn -O3 Cache.cpp Client.cpp Curl.cpp CurlMulti.cpp Dns.cpp EpollObject.cpp main.cpp Server.cpp Timer.cpp Timer/DNSCache.cpp Timer/DNSQuery.cpp -lcurl -std=c++11 -I.
