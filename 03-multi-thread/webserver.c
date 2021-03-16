#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define VERSION 23
#define BUFSIZE 8096
#define ERROR      42
#define LOG        44
#define FORBIDDEN 403
#define NOTFOUND  404

#ifndef SIGCLD
#   define SIGCLD SIGCHLD
#endif

struct {
  char *ext;
  char *filetype;
} extensions [] = {
  {"gif", "image/gif" },
  {"jpg", "image/jpg" },
  {"jpeg","image/jpeg"},
  {"png", "image/png" },
  {"ico", "image/ico" },
  {"zip", "image/zip" },
  {"gz",  "image/gz"  },
  {"tar", "image/tar" },
  {"htm", "text/html" },
  {"html","text/html" },
  {0,0} };

typedef struct {
  int hit;
  int fd;
} webparam;

unsigned long get_file_size(const char * path) {
  unsigned long filesize = -1;
  struct stat statbuff;
  if(stat(path, &statbuff)<0) {
    return filesize;
  }else {
    filesize = statbuff.st_size;
  }
  return filesize;
}


void logger(int type, char *s1, char *s2, int socket_fd)
{
  int fd ;
  char logbuffer[BUFSIZE*2];

  // 获取时间信息
  struct tm * ptr;
  time_t lt;
  lt = time(NULL);
  ptr = localtime(&lt);
  char timebuf[48] = {0};
  strftime(timebuf, sizeof(timebuf), "%H:%M:%S %a %d/%m/%Y", ptr);


  /*将消息写入logger，或直接将消息通过socket通道返回给客户端*/

  switch (type) {
    case ERROR: (void)sprintf(logbuffer,"[%s] ERROR: %s:%s Errno=%d exiting pid=%d\n", timebuf, s1, s2, errno,getpid());
      break;
    case FORBIDDEN:
      (void)write(socket_fd, "HTTP/1.1 403 Forbidden\nContent-Length: 185\nConnection: close\nContent-Type: text/html\n\n<html><head>\n<title>403 Forbidden</title>\n</head><body>\n<h1>Forbidden</h1>\nThe requested URL, file type or operation is not allowed on this simple static file webserver.\n</body></html>\n",271);
      (void)sprintf(logbuffer,"[%s] FORBIDDEN: %s:%s:%d\n", timebuf, s1, s2, socket_fd);
      break;
    case NOTFOUND:
      (void)write(socket_fd, "HTTP/1.1 404 Not Found\nContent-Length: 136\nConnection: close\nContent-Type: text/html\n\n<html><head>\n<title>404 Not Found</title>\n</head><body>\n<h1>Not Found</h1>\nThe requested URL was not found on this server.\n</body></html>\n",224);
      (void)sprintf(logbuffer,"[%s] NOT FOUND: %s:%s:%d\n", timebuf, s1, s2, socket_fd);
      break;

    case LOG: (void)sprintf(logbuffer,"[%s] INFO: %s:%s:%d", timebuf, s1, s2,socket_fd); break;
  }
  /* No checks here, nothing can be done with a failure anyway */
  if((fd = open("nweb.log", O_CREAT| O_WRONLY | O_APPEND,0644)) >= 0) {
    (void)write(fd,logbuffer,strlen(logbuffer));
    (void)write(fd,"\n",1);
    (void)close(fd);
  }

  if(type == NOTFOUND || type == FORBIDDEN) pthread_exit((void*)3);
  if(type == ERROR) exit(3);
}

/* this is a child web server process, so we can exit on errors */
void * web(void * data)
{
  int fd;
  int hit;

  int j, file_fd, buflen;
  long i, ret, len;
  char * fstr;
  char buffer[BUFSIZE+1]; /* !!!!!!!! 不能是静态缓冲区 */
  webparam * param = (webparam*)data;
  fd = param->fd;
  hit = param->hit;

  ret =read(fd,buffer,BUFSIZE);   /* 从客户端读取请求消息 */

  if(ret == 0 || ret == -1) {  /* 读消息失败 */
    logger(FORBIDDEN,"failed to read browser request","",fd);
  }else {
    if(ret > 0 && ret < BUFSIZE)  /* 设置有效字符串 */
      buffer[ret]=0;
    else buffer[0]=0;

    for(i=0;i<ret;i++)  /* remove CF and LF characters */
      if(buffer[i] == '\r' || buffer[i] == '\n')
        buffer[i]='*';

    // 请求信息l写入log
    logger(LOG,"request",buffer,hit);

    // method
    if( strncmp(buffer,"GET ",4) && strncmp(buffer,"get ",4) ) {
      logger(FORBIDDEN,"Only simple GET operation supported",buffer,fd);
    }

    for(i=4;i<BUFSIZE;i++) { /* null terminate after the second space to ignore extra stuff */
      if(buffer[i] == ' ') { /* string is "GET URL " +lots of other stuff */
        buffer[i] = 0;
        break;
      }
    }

    // 不能使用 ..
    for(j=0;j<i-1;j++)   /* check for illegal parent directory use .. */
      if(buffer[j] == '.' && buffer[j+1] == '.') {
        logger(FORBIDDEN,"Parent directory (..) path names not supported",buffer,fd);
      }

    // 不包含有效文件名有效文件名，使用默认 /index.html
    if( !strncmp(&buffer[0],"GET /\0",6) || !strncmp(&buffer[0],"get /\0",6) )
      (void)strcpy(buffer,"GET /index.html");

    // 根据预定义在extensionsh中的文件类型，检查请求的文件类型是否本服务器支持
    buflen=strlen(buffer);
    fstr = (char *)0;
    for(i=0;extensions[i].ext != 0;i++) {
      len = strlen(extensions[i].ext);
      if( !strncmp(&buffer[buflen-len], extensions[i].ext, len)) {
        fstr =extensions[i].filetype;
        break;
      }
    }
    if(fstr == 0) logger(FORBIDDEN,"file extension type not supported",buffer,fd);
    
    // 打开文件
    if(( file_fd = open(&buffer[5],O_RDONLY)) == -1) {  /* open the file for reading */
      logger(NOTFOUND, "failed to open file",&buffer[5],fd);
    }
    
    // 发送信息写入log
    logger(LOG,"SEND",&buffer[5],hit);
    // 获取文件长度
    len = get_file_size(&buffer[5]);
	// 发送响应消息
    (void)sprintf(buffer,"HTTP/1.1 200 OK\nServer: nweb/%d.0\nContent-Length: %ld\nConnection: close\nContent-Type: %s\n\n", VERSION, len, fstr);
    // 写入log
    logger(LOG,"Header",buffer,hit);
    (void)write(fd,buffer,strlen(buffer));

    /* send file in 8KB block - last block may be smaller */
    while (  (ret = read(file_fd, buffer, BUFSIZE)) > 0 ) {
      (void)write(fd,buffer,ret);
    }
    usleep(10000); /* 保证消息全部发出 */
    close(file_fd);
  }//else
  close(fd);
  free(param);
}

int main(int argc, char **argv)
{
  int i, port, pid, listenfd, socketfd, hit;
  socklen_t length;
  static struct sockaddr_in cli_addr; /* static = initialised to zeros */
  static struct sockaddr_in serv_addr; /* static = initialised to zeros */

	// 解析b命令行参数
  if( argc < 3  || argc > 3 || !strcmp(argv[1], "-?") ) {
    (void)printf("hint: nweb Port-Number Top-Directory\t\tversion %d\n\n"
  "\tnweb is a small and very safe mini web server\n"
  "\tnweb only servers out file/web pages with extensions named below\n"
  "\t and only from the named directory or its sub-directories.\n"
  "\tThere is no fancy features = safe and secure.\n\n"
  "\tExample: nweb 8181 /home/nwebdir &\n\n"
  "\tOnly Supports:", VERSION);
    for(i=0;extensions[i].ext != 0;i++)
      (void)printf(" %s",extensions[i].ext);

    (void)printf("\n\tNot Supported: URLs including \"..\", Java, Javascript, CGI\n"
  "\tNot Supported: directories / /etc /bin /lib /tmp /usr /dev /sbin \n"
  "\tNo warranty given or implied\n\tNigel Griffiths nag@uk.ibm.com\n"  );
    exit(0);
  }
  if( !strncmp(argv[2],"/"   ,2 ) || !strncmp(argv[2],"/etc", 5 ) ||
      !strncmp(argv[2],"/bin",5 ) || !strncmp(argv[2],"/lib", 5 ) ||
      !strncmp(argv[2],"/tmp",5 ) || !strncmp(argv[2],"/usr", 5 ) ||
      !strncmp(argv[2],"/dev",5 ) || !strncmp(argv[2],"/sbin",6) ){
    (void)printf("ERROR: Bad top directory %s, see nweb -?\n",argv[2]);
    exit(3);
  }
  if(chdir(argv[2]) == -1){
    (void)printf("ERROR: Can't Change to directory %s\n",argv[2]);
    exit(4);
  }
  /* Become deamon + unstopable and no zombies children (= no wait()) */
  if(fork() != 0)
    return 0; // parent returns OK to shell 
  (void)signal(SIGCLD, SIG_IGN); // ignore child death 
  (void)signal(SIGHUP, SIG_IGN); // ignore terminal hangups 
  for(i=0;i<32;i++)
    (void)close(i);    // close open files 
  (void)setpgrp();    // break away from process group 
  logger(LOG,"nweb starting",argv[1],getpid());

	// 建立服务端侦听socket
  if((listenfd = socket(AF_INET, SOCK_STREAM,0)) <0)
    logger(ERROR, "system call","socket",0);
  port = atoi(argv[1]);
  if(port < 0 || port >60000)
    logger(ERROR,"Invalid port number (try 1->60000)",argv[1],0);

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t pth;
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  serv_addr.sin_port = htons(port);
  if(bind(listenfd, (struct sockaddr *)&serv_addr,sizeof(serv_addr)) <0)
    logger(ERROR,"system call","bind",0);
  if( listen(listenfd,64) <0)
    logger(ERROR,"system call","listen",0);

// hit!!
  for(hit=1; ;hit++) {
    length = sizeof(cli_addr);
    if((socketfd = accept(listenfd, (struct sockaddr *)&cli_addr, &length)) < 0)
      logger(ERROR,"system call","accept",0);
    
    webparam * param = malloc(sizeof(webparam));
    param->fd = socketfd;
    param->hit = hit;
    
    if(pthread_create(&pth, &attr, &web, (void*)param)<0) {
      logger(ERROR,"system call","pthread_create",0);
    }

  }

}//main
