
#ifndef _TCPreqchannel_H_
#define _TCPreqchannel_H_

#include "common.h"
#include <sys/socket.h>
#include <netdb.h>

class TCPRequestChannel
{
private:
	int sockfd;
public:
    TCPRequestChannel(const string host, const string post);

    TCPRequestChannel(int );

	~TCPRequestChannel();

	int cread(void* msgbuf, int bufcapacity);
	
	int cwrite(void *msgbuf , int msglen);
	 
	string name();

	int getfd();
};

#endif
