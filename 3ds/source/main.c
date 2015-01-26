#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <inttypes.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <zlib.h>

#define ZLIB_CHUNK (16 * 1024)

#include <3ds.h>

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

extern void (*__system_retAddr)(void);
static Handle hbHandle;
void (*callBootloader)(Handle hb, Handle file);
void (*setArgs)(u32* src, u32 length);

static void launchFile(void)
{
	//jump to bootloader
	callBootloader(0x00000000, hbHandle);
}


static int set_socket_nonblocking(int fd) {
  int flags;

  flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  return 0;
}

//---------------------------------------------------------------------------------
int recvall(int sock, void *buffer, int size, int flags) {
//---------------------------------------------------------------------------------
	int len, sizeleft = size;

	while (sizeleft) {
		len = recv(sock,buffer,sizeleft,flags);
		if (len == 0) {
			size = 0;
			break;
		};
		if (len == -1) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        printf("\nrecv -1, errno = %d\n", errno);
        perror(NULL);
        break;
      }
		} else {
      sizeleft -=len;
      buffer +=len;
		}
	}
	return size;
}

char *executablePath = NULL;
int cmdlen = 0;
char *commandline = NULL;

unsigned char in[ZLIB_CHUNK];
unsigned char out[ZLIB_CHUNK];

//---------------------------------------------------------------------------------
int decompress(int sock, FILE *fh, size_t filesize) {
//---------------------------------------------------------------------------------
  int ret;
  unsigned have;
  z_stream strm;
  size_t chunksize;

    /* allocate inflate state */
  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;
  strm.avail_in = 0;
  strm.next_in = Z_NULL;
  ret = inflateInit(&strm);
  if (ret != Z_OK) {
    printf("inflateInit failed.\n");
    return ret;
  }

  size_t total = 0;
  /* decompress until deflate stream ends or end of file */
  do {

    int len = recvall(sock, &chunksize, 4, 0);

    if (len != 4) {
      (void)inflateEnd(&strm);
      printf("\nError getting chunk size\n");
      return Z_DATA_ERROR;
    }

    strm.avail_in = recvall(sock,in,chunksize,0);

    if (strm.avail_in == 0) {
      (void)inflateEnd(&strm);
      printf("\nremote closed socket.\n");
      return Z_DATA_ERROR;
    }

    strm.next_in = in;

    /* run inflate() on input until output buffer not full */
    do {
      strm.avail_out = ZLIB_CHUNK;
      strm.next_out = out;
      ret = inflate(&strm, Z_NO_FLUSH);

      switch (ret) {
        case Z_NEED_DICT:
          ret = Z_DATA_ERROR;     /* and fall through */

        case Z_DATA_ERROR:
        case Z_MEM_ERROR:
        case Z_STREAM_ERROR:
          (void)inflateEnd(&strm);
          printf("inflate error: %d\n",ret);
          return ret;
      }

      have = ZLIB_CHUNK - strm.avail_out;

      if (fwrite(out, 1, have, fh) != have || ferror(fh)) {
        (void)inflateEnd(&strm);
        printf("\nfile write error\n");
        return Z_ERRNO;
      }

      total += have;
      printf("\r%zu (%d%%)",total, (100 * total) / filesize);
      gfxFlushBuffers();
    } while (strm.avail_out == 0);
    /* done when inflate() says it's done */
  } while (ret != Z_STREAM_END);

   /* clean up and return */
  (void)inflateEnd(&strm);
  return ret == Z_STREAM_END ? Z_OK : Z_DATA_ERROR;
}


//---------------------------------------------------------------------------------
int load3DSX(int sock, u32 remote) {
//---------------------------------------------------------------------------------
	int len, namelen, filelen;

  char filename[256];

  len = recvall(sock, &namelen, 4, 0);
  if (len != 4) {
    printf("Error getting name length\n");
    return -1;
  }

  len = recvall(sock, filename, namelen, 0);
  if (len != namelen) {
    printf("Error getting filename\n");
    return -1;
  }

  filename[namelen] = 0;

  len = recvall(sock, &filelen, 4, 0);
  if (len != 4) {
    printf("Error getting file length\n");
    return -1;
  }

  int response = 0;

  int fd = open(filename,O_CREAT|O_WRONLY,ACCESSPERMS);

  if (fd < 0) {
    response = -1;
  } else {
    if (ftruncate(fd,filelen) == -1) {
      response = -2;
      perror(NULL);
      printf("%x\n",errno);
    }
   }

  send(sock,(int *)&response,sizeof(response),0);

  close(fd);

  FILE *file = fopen(filename,"wb");
  char *writebuffer=malloc(65536);
  setvbuf(file,writebuffer,_IOFBF, 65536);

  if (response == 0) {
    printf("transferring %s\n%d bytes.\n", filename, filelen);

    if (decompress(sock,file,filelen)==Z_OK) {
      send(sock,(int *)&response,sizeof(response),0);
      printf("\ntransferring command line\n");
      len = recvall(sock,(char*)&cmdlen,4,0);
      if (cmdlen) {
        commandline = malloc(cmdlen);
        len = recvall(sock, commandline, cmdlen,0);
      }
    } else {
      response = 1;
    }
  }

  free(writebuffer);
  fclose(file);

  if (response == 0) {
    executablePath=getcwd(NULL,0);
    strcat(executablePath,filename);

    executablePath = strchr(executablePath,'/');
    printf("%s\n", executablePath);

  }
  return response;
}

static u32 *SOC_buffer = NULL;
static FS_archive sdmcArchive;

int main(int argc, char **argv) {
  hbInit();
  gfxInitDefault();
  consoleInit(GFX_TOP,NULL);

  SOC_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
  SOC_Initialize(SOC_buffer, SOC_BUFFERSIZE);

  struct in_addr host;
  host.s_addr = gethostid();

  printf("3dslink: %s:%d\n",inet_ntoa(host),17491);

  int sock_udp = socket(AF_INET, SOCK_DGRAM, 0);

  struct sockaddr_in sa_udp, sa_udp_remote;

  sa_udp.sin_family = AF_INET;
  sa_udp.sin_addr.s_addr = INADDR_ANY;
  sa_udp.sin_port = htons(17491);

  if(bind(sock_udp, (struct sockaddr*) &sa_udp, sizeof(sa_udp)) < 0) {
    printf(" UDP socket error\n");
    perror(NULL);
  }

  set_socket_nonblocking(sock_udp);

	struct sockaddr_in sa_tcp;
	sa_tcp.sin_addr.s_addr=INADDR_ANY;
	sa_tcp.sin_family=AF_INET;
	sa_tcp.sin_port=htons(17491);
	int sock_tcp=socket(AF_INET,SOCK_STREAM,0);
	bind(sock_tcp,(struct sockaddr *)&sa_tcp,sizeof(sa_tcp));

  set_socket_nonblocking(sock_tcp);

  listen(sock_tcp,2);

  socklen_t fromlen = sizeof(sa_udp_remote);

  char recvbuf[256];

  while (aptMainLoop()) {

    int len = recvfrom(sock_udp, recvbuf, sizeof(recvbuf), 0, (struct sockaddr*) &sa_udp_remote, &fromlen);

    if (len!=-1) {
      if (strncmp(recvbuf,"3dsboot",strlen("3dsboot")) == 0) {
				sa_udp_remote.sin_family=AF_INET;
				sa_udp_remote.sin_port=htons(17491);
        sendto(sock_udp, "boot3ds", strlen("boot3ds"), 0, (struct sockaddr*) &sa_udp_remote,sizeof(sa_udp_remote));
      }
    }

		int sock_tcp_remote = accept(sock_tcp,(struct sockaddr *)&sa_tcp,&fromlen);

		if (sock_tcp_remote != -1) {
			int result = load3DSX(sock_tcp_remote,sa_tcp.sin_addr.s_addr);
			close(sock_tcp_remote);
			if (result==0) {
        close(sock_tcp);
        close(sock_udp);
        break;
			}
		}

    gspWaitForVBlank();
    hidScanInput();


    u32 kDown = hidKeysDown();

    if (kDown & KEY_START)
      break; // break in order to return to hbmenu

    // Flush and swap framebuffers
    gfxFlushBuffers();
    gfxSwapBuffers();
  }

  Result ret = SOC_Shutdown();

  if(ret != 0) printf("SOC_Shutdown: 0x%08X\n", (unsigned int)ret);

  free(SOC_buffer);


  if (executablePath != NULL) {
    HB_GetBootloaderAddresses((void**)&callBootloader, (void**)&setArgs);
    hbExit();
    sdmcExit();
    fsExit();

    fsInit();
    sdmcArchive=(FS_archive){0x00000009, (FS_path){PATH_EMPTY, 1, (u8*)""}};
    FSUSER_OpenFileDirectly(NULL, &hbHandle, sdmcArchive, FS_makePath(PATH_CHAR, executablePath), FS_OPEN_READ, FS_ATTRIBUTE_NONE);

    fsExit();

    //set argv/argc
    static u32 argbuffer[0x200];

    argbuffer[0]=0;
    char *ptr = commandline;
    char *dst = (char*)&argbuffer[1];
    while (ptr < commandline + cmdlen) {
      char *arg = ptr;
      strcpy(dst,ptr);
      ptr += strlen(arg) + 1;
      dst += strlen(arg) + 1;
      argbuffer[0]++;
    }

    setArgs(argbuffer, 0x200*4);

    __system_retAddr = launchFile;

  }

  gfxExit();

  return 0;
}
