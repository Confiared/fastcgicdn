fastcgicdn: Backend.cpp Common.cpp Http.cpp Cache.cpp Client.cpp Dns.cpp EpollObject.cpp main.cpp Server.cpp Timer.cpp Timer/DNSCache.cpp Timer/DNSQuery.cpp
#	g++ -flto -o fastcgicdn -DDEBUGDNS -DDEBUGFASTCGI -g Backend.cpp Common.cpp Http.cpp Cache.cpp Client.cpp Dns.cpp EpollObject.cpp main.cpp Server.cpp Timer.cpp Timer/DNSCache.cpp Timer/DNSQuery.cpp -lcurl -std=c++11 -I.
	g++ -flto -o fastcgicdn -O3 Backend.cpp Common.cpp Http.cpp Cache.cpp Client.cpp Dns.cpp EpollObject.cpp main.cpp Server.cpp Timer.cpp Timer/DNSCache.cpp Timer/DNSQuery.cpp -lcurl -std=c++11 -I.
