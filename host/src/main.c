#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/time.h>
#include <stdint.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>

#ifndef __WIN32__
#include <sys/socket.h>
#include <arpa/inet.h>
#else
#include <winsock2.h>
typedef int socklen_t;
typedef uint32_t in_addr_t;
#endif

#include <zlib.h>
#include <assert.h>

#define ZLIB_CHUNK (16 * 1024)

char cmdbuf[3072];
uint32_t cmdlen=0;

//---------------------------------------------------------------------------------
void shutdownSocket(int socket) {
//---------------------------------------------------------------------------------
#ifdef __WIN32__
	shutdown (socket, SD_SEND);
	closesocket (socket);
#else
	close(socket);
#endif
}

/*---------------------------------------------------------------------------------
	Subtract the `struct timeval' values Y from X,
	storing the result in RESULT.
	Return 1 if the difference is negative, otherwise 0.

	From http://www.gnu.org/software/libtool/manual/libc/Elapsed-Time.html
---------------------------------------------------------------------------------*/
int timeval_subtract (struct timeval *result, struct timeval *x, struct timeval *y) {
//---------------------------------------------------------------------------------
	struct timeval tmp;
	tmp.tv_sec = y->tv_sec;
	tmp.tv_usec = y->tv_usec;

	/* Perform the carry for the later subtraction by updating y. */
	if (x->tv_usec < tmp.tv_usec) {
		int nsec = (tmp.tv_usec - x->tv_usec) / 1000000 + 1;
		tmp.tv_usec -= 1000000 * nsec;
		tmp.tv_sec += nsec;
	}

	if (x->tv_usec - tmp.tv_usec > 1000000) {
		int nsec = (x->tv_usec - tmp.tv_usec) / 1000000;
		tmp.tv_usec += 1000000 * nsec;
		tmp.tv_sec -= nsec;
	}

	/*	Compute the time remaining to wait.
		tv_usec is certainly positive. */
	result->tv_sec = x->tv_sec - tmp.tv_sec;
	result->tv_usec = x->tv_usec - tmp.tv_usec;

	/* Return 1 if result is negative. */
	return x->tv_sec < tmp.tv_sec;
}

//---------------------------------------------------------------------------------
void timeval_add (struct timeval *result, struct timeval *x, struct timeval *y) {
//---------------------------------------------------------------------------------
	result->tv_sec = x->tv_sec + y->tv_sec;
	result->tv_usec = x->tv_usec + y->tv_usec;

	if ( result->tv_usec > 1000000) {
		result->tv_sec += result->tv_usec / 1000000;
		result->tv_usec = result->tv_usec % 1000000;
	}
}

//---------------------------------------------------------------------------------
static struct in_addr find3DS() {
//---------------------------------------------------------------------------------
    struct sockaddr_in s, remote, rs;
	char recvbuf[256];
	char mess[] = "3dsboot";

	int broadcastSock = socket(PF_INET, SOCK_DGRAM, 0);
	if(broadcastSock < 0) perror("create send socket");

	int optval = 1, len;
	setsockopt(broadcastSock, SOL_SOCKET, SO_BROADCAST, (char *)&optval, sizeof(optval));

	memset(&s, '\0', sizeof(struct sockaddr_in));
    s.sin_family = AF_INET;
    s.sin_port = htons(17491);
    s.sin_addr.s_addr = INADDR_BROADCAST;

	memset(&rs, '\0', sizeof(struct sockaddr_in));
    rs.sin_family = AF_INET;
    rs.sin_port = htons(17491);
    rs.sin_addr.s_addr = INADDR_ANY;

	int recvSock = socket(PF_INET, SOCK_DGRAM, 0);

	if (recvSock < 0)  perror("create receive socket");

	if(bind(recvSock, (struct sockaddr*) &rs, sizeof(rs)) < 0) perror("bind");
#ifndef __WIN32__
	fcntl(recvSock, F_SETFL, O_NONBLOCK);
#else
	u_long opt = 1;
	ioctlsocket(recvSock, FIONBIO, &opt);
#endif
	struct timeval wanted, now, result;

	gettimeofday(&wanted, NULL);

	int timeout = 10;
	while(timeout) {
		gettimeofday(&now, NULL);
		if ( timeval_subtract(&result,&wanted,&now)) {
			if(sendto(broadcastSock, mess, strlen(mess), 0, (struct sockaddr *)&s, sizeof(s)) < 0) perror("sendto");
			result.tv_sec=0;
			result.tv_usec=150000;
			timeval_add(&wanted,&now,&result);
			timeout--;
		}
		socklen_t socklen = sizeof(remote);
		len = recvfrom(recvSock,recvbuf,sizeof(recvbuf),0,(struct sockaddr *)&remote,&socklen);
		if ( len != -1) {
			if ( strncmp("boot3ds",recvbuf,strlen("boot3ds")) == 0) {
				break;
			}
		}
	}
	if (timeout == 0) remote.sin_addr.s_addr =  INADDR_NONE;
	shutdownSocket(broadcastSock);
	shutdownSocket(recvSock);
	return remote.sin_addr;
}

//---------------------------------------------------------------------------------
int sendData(int sock, int sendsize, char *buffer) {
//---------------------------------------------------------------------------------
	while(sendsize) {
		int len = send(sock, buffer, sendsize, 0);
		if (len == 0) break;
		if (len != -1) {
			sendsize -= len;
			buffer += len;
		} else {
#ifdef _WIN32
			int errcode = WSAGetLastError();
			if (errcode != WSAEWOULDBLOCK) {
				printf("error %d\n",errcode);
				break;
			}
#else
			if ( errno != EWOULDBLOCK && errno != EAGAIN) {
				perror(NULL);
				break;
			}
#endif
		}
	}
	return sendsize != 0;
}

//---------------------------------------------------------------------------------
int recvData(int sock, char *buffer, int size, int flags) {
//---------------------------------------------------------------------------------
	int len, sizeleft = size;

	while (sizeleft) {
		len = recv(sock,buffer,sizeleft,flags);
		if (len == 0) {
			size = 0;
			break;
		}
		if (len != -1) {
			sizeleft -=len;
			buffer +=len;
		} else {
#ifdef _WIN32
			int errcode = WSAGetLastError();
			if (errcode != WSAEWOULDBLOCK) {
				printf("error %d\n",errcode);
				break;
			}
#else
			if ( errno != EWOULDBLOCK && errno != EAGAIN) {
				perror(NULL);
				break;
			}
#endif
		}
	}
	return size;
}


//---------------------------------------------------------------------------------
int sendInt32LE(int socket, uint32_t size) {
//---------------------------------------------------------------------------------
	char lenbuf[4];
	lenbuf[0] = size & 0xff;
	lenbuf[1] = (size >>  8) & 0xff;
	lenbuf[2] = (size >> 16) & 0xff;
	lenbuf[3] = (size >> 24) & 0xff;

	return sendData(socket,4,lenbuf);
}

//---------------------------------------------------------------------------------
int recvInt32LE(int socket, int32_t *data) {
//---------------------------------------------------------------------------------
	char intbuf[4];
	int len = recvData(socket,intbuf,4,0);

	if (len == 4) {
		*data = intbuf[0] & 0xff + (intbuf[1] <<  8) + (intbuf[2] <<  16) + (intbuf[3] <<  24);
		return 0;
	}

	return -1;

}

unsigned char in[ZLIB_CHUNK];
unsigned char out[ZLIB_CHUNK];

//---------------------------------------------------------------------------------
int send3DSXFile(in_addr_t dsaddr, char *name, size_t filesize, FILE *fh) {
//---------------------------------------------------------------------------------

	int retval = 0;

	int ret, flush;
	unsigned have;
	z_stream strm;

	/* allocate deflate state */
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	ret = deflateInit(&strm, Z_DEFAULT_COMPRESSION);
	if (ret != Z_OK) return ret;

	int sock = socket(AF_INET,SOCK_STREAM,0);
	if (sock < 0) {
		perror("create connection socket");
		return -1;
	}

	struct sockaddr_in s;
	memset(&s, '\0', sizeof(struct sockaddr_in));
    s.sin_family = AF_INET;
    s.sin_port = htons(17491);
    s.sin_addr.s_addr = dsaddr;

	if (connect(sock,(struct sockaddr *)&s,sizeof(s)) < 0 ) {
		struct in_addr address;
		address.s_addr = dsaddr;
		fprintf(stderr,"Connection to %s failed\n",inet_ntoa(address));
		return -1;
	}

	int namelen = strlen(name);

	if (sendInt32LE(sock,namelen)) {
		fprintf(stderr,"Failed sending filename length\n");
		retval = -1;
		goto error;
	}

	if (sendData(sock,namelen,name)) {
		fprintf(stderr,"Failed sending filename\n");
		retval = -1;
		goto error;
	}

	if (sendInt32LE(sock,filesize)) {
		fprintf(stderr,"Failed sending file length\n");
		retval = -1;
		goto error;
	}

	int response;

	if(recvInt32LE(sock,&response)!=0) {
		fprintf(stderr,"Invalid response\n");
		retval = 1;
		goto error;
	}

	if(response!=0) {
		switch(response) {
			case -1:
				fprintf(stderr,"Failed to create file\n");
				break;
			case -2:
				fprintf(stderr,"Insufficient space\n");
				break;
			case -3:
				fprintf(stderr,"Insufficient memory\n");
				break;
		}
		retval = 1;
		goto error;
	}

	printf("Sending %s, %d bytes\n",name, filesize);

	size_t totalsent = 0, blocks = 0;


	do {
		strm.avail_in = fread(in, 1, ZLIB_CHUNK, fh);
		if (ferror(fh)) {
			(void)deflateEnd(&strm);
			return Z_ERRNO;
		}
		flush = feof(fh) ? Z_FINISH : Z_NO_FLUSH;
		strm.next_in = in;
		/* run deflate() on input until output buffer not full, finish
		   compression if all of source has been read in */
		do {
			strm.avail_out = ZLIB_CHUNK;
			strm.next_out = out;
			ret = deflate(&strm, flush);    /* no bad return value */
			assert(ret != Z_STREAM_ERROR);  /* state not clobbered */
			have = ZLIB_CHUNK - strm.avail_out;

			if (have != 0) {
				if (sendInt32LE(sock,have)) {
					fprintf(stderr,"Failed sending chunk size\n");
					retval = -1;
					goto error;
				}

				if(sendData(sock,have,out)) {
					fprintf(stderr,"Failed sending %s\n", name);
					retval = 1;
					(void)deflateEnd(&strm);
					goto error;
				}

				totalsent += have;
				blocks++;
			}
		} while (strm.avail_out == 0);
		assert(strm.avail_in == 0);     /* all input will be used */
		/* done when last data in file processed */
	} while (flush != Z_FINISH);
	assert(ret == Z_STREAM_END);        /* stream will be complete */
	(void)deflateEnd(&strm);

	printf("%u sent (%.2f%%), %d blocks\n",totalsent, (float)(totalsent * 100.0)/ filesize, blocks);

	if(recvInt32LE(sock,&response)!=0) {
		fprintf(stderr,"Failed sending %s\n",name);
		retval = 1;
		goto error;
	}


	if(sendData(sock,cmdlen+4,cmdbuf)) {

		fprintf(stderr,"Failed sending command line\n");
		retval = 1;
		goto error;

	}


error:
	shutdownSocket(sock);
	return retval;
}

//---------------------------------------------------------------------------------
void showHelp() {
//---------------------------------------------------------------------------------
	puts("Usage: 3dslink [options] 3dsxfile\n");
	puts("--help, -h      Display this information");
	puts("--address, -a   ipv4 address of 3DS");
	puts("--arg0   , -0   set argv[0]");
	puts("\n");
}

//---------------------------------------------------------------------------------
int main(int argc, char **argv) {
//---------------------------------------------------------------------------------
	char *address = NULL;
	char *argv0 = NULL;

	if (argc < 2) {
		showHelp();
		return 1;
	}

	while(1) {
		static struct option long_options[] = {
			{"address",	required_argument,	0,	'a'},
			{"arg0",	required_argument,	0,	'0'},
			{"help",	no_argument,		0,	'h'},
			{0, 0, 0, 0}
		};

		/* getopt_long stores the option index here. */
		int option_index = 0, c;

		c = getopt_long (argc, argv, "a:h0:", long_options, &option_index);

		/* Detect the end of the options. */
		if (c == -1)
		break;

		switch(c) {

		case 'a':
			address = optarg;
			break;
		case '0':
			argv0 = optarg;
			break;
		case 'h':
			showHelp();
			break;
		}

	}

	char *filename = argv[optind++];
	if (filename== NULL) {
		showHelp();
		return 1;
	}

	memset(cmdbuf, '\0', sizeof(cmdbuf));

	FILE *fh = fopen(filename,"rb");
	if (fh == NULL) {
		fprintf(stderr,"Failed to open %s\n",filename);
		return -1;
	}

	fseek(fh,0,SEEK_END);
	size_t filesize = ftell(fh);
	fseek(fh,0,SEEK_SET);

	char *basename = NULL;
	if((basename=strrchr(filename,'/'))!=NULL) {
		basename++;
	} else if ((basename=strrchr(filename,'\\'))!=NULL) {
		basename++;
	} else {
		basename = filename;
	}

	if (argv0 == NULL) {
		strcpy(&cmdbuf[4],"3dslink:/");
		strcat(&cmdbuf[4],basename);
	} else {
		strcpy(&cmdbuf[4],argv0);
	}

	cmdlen = strlen(&cmdbuf[4]) + 1;

	for (int index = optind; index < argc; index++) {
		int len=strlen(argv[index]);
		if ( (cmdlen + len + 5 ) >= (sizeof(cmdbuf) - 2) ) break;
		strcpy(&cmdbuf[cmdlen+4],argv[index]);
		cmdlen+= len + 1;
	}

	cmdbuf[0] = cmdlen & 0xff;
	cmdbuf[1] = (cmdlen>>8) & 0xff;
	cmdbuf[2] = (cmdlen>>16) & 0xff;
	cmdbuf[3] = (cmdlen>>24) & 0xff;

#ifdef __WIN32__
	WSADATA wsa_data;
	if (WSAStartup (MAKEWORD(2,2), &wsa_data)) {
		printf ("WSAStartup failed\n");
		return 1;
	}
#endif

	struct in_addr dsaddr;
	dsaddr.s_addr  =  INADDR_NONE;

	if (address == NULL) {
		dsaddr = find3DS();

		if (dsaddr.s_addr == INADDR_NONE) {
			printf("No response from 3DS!\n");
			return 1;
		}

	} else {
		dsaddr.s_addr = inet_addr(address);
	}

	if (dsaddr.s_addr == INADDR_NONE) {
		fprintf(stderr,"Invalid address\n");
		return 1;
	}

	int res = send3DSXFile(dsaddr.s_addr,basename,filesize,fh);

	fclose(fh);

#ifdef __WIN32__
	WSACleanup ();
#endif
	return res;
}

