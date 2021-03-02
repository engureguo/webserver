#include <stdio.h>   
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <time.h>
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


struct timeval t1,t2;

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
  case ERROR: (void)sprintf(logbuffer,"[%s] ERROR: %s:%s Errno=%d exiting pid=%d", timebuf, s1, s2, errno,getpid());
    break;
  case FORBIDDEN:
    (void)write(socket_fd, "HTTP/1.1 403 Forbidden\nContent-Length: 185\nConnection: close\nContent-Type: text/html\n\n<html><head>\n<title>403 Forbidden</title>\n</head><body>\n<h1>Forbidden</h1>\nThe requested URL, file type or operation is not allowed on this simple static file webserver.\n</body></html>\n",271);
    (void)sprintf(logbuffer,"[%s] FORBIDDEN: %s:%s", timebuf, s1, s2);
    break;
  case NOTFOUND:
    (void)write(socket_fd, "HTTP/1.1 404 Not Found\nContent-Length: 136\nConnection: close\nContent-Type: text/html\n\n<html><head>\n<title>404 Not Found</title>\n</head><body>\n<h1>Not Found</h1>\nThe requested URL was not found on this server.\n</body></html>\n",224);
    (void)sprintf(logbuffer,"[%s] NOT FOUND: %s:%s", timebuf, s1, s2);
    break;

  case LOG: (void)sprintf(logbuffer,"[%s] INFO: %s:%s:%d", timebuf, s1, s2,socket_fd); break;
  }
  // 将logbuffer缓存中的消息存入 log文件
  if((fd = open("nweb.log", O_CREAT| O_WRONLY | O_APPEND,0644)) >= 0) {
    (void)write(fd,logbuffer,strlen(logbuffer));
    (void)write(fd,"\n",1);
    (void)close(fd);
  }


}
long long duration2() {
	return ( (t2.tv_sec-t1.tv_sec)*1000000+t2.tv_usec-t1.tv_usec  );
}

/*
	完成 webserver的主要功能。
	首先解析客户端发送的消息，然后从中获取客户端请求的文件名，
	然后根据文件名从本地读入缓存，并生成相应的 HTTP 响应消息；
	最有通过服务器与客户端的 socket 通道向客户端返回 HTTP响应消息。

*/

void web(int fd, int hit)
{
  int j, file_fd, buflen;
  long i, ret, len;
  char * fstr;
  static char buffer[BUFSIZE+1]; /* 静态缓冲区 */

  ret =read(fd,buffer,BUFSIZE);   /* 从客户端读取请求消息 */

  //2.接收请求 ---> 读取请求
  //2.--->开始计时
  gettimeofday(&t2, NULL);
  printf("\t接收请求->读取请求用时:%lld us\n", duration2());
  gettimeofday(&t1, NULL);

  if(ret == 0 || ret == -1) {  /* 读消息失败 */
    logger(FORBIDDEN,"failed to read browser request","",fd);
  }
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
  len = (long)lseek(file_fd, (off_t)0, SEEK_END); 
	// 将指针移动到文件首位置
  (void)lseek(file_fd, (off_t)0, SEEK_SET); /* lseek back to the file start ready for reading */
    // 发送响应消息
  (void)sprintf(buffer,"HTTP/1.1 200 OK\nServer: nweb/%d.0\nContent-Length: %ld\nConnection: close\nContent-Type: %s\n\n", VERSION, len, fstr);
  // 写入log
  logger(LOG,"Header",buffer,hit);
  (void)write(fd,buffer,strlen(buffer));

  //3.解析完请求信息 --> 发送响应信息的时间
  //3.----->开始发送文件
  gettimeofday(&t2, NULL);
  printf("\t读取完请求->发送完响应消息用时:%lld us\n", duration2());
  gettimeofday(&t1, NULL);


  /* send file in 8KB block - last block may be smaller */
  while (  (ret = read(file_fd, buffer, BUFSIZE)) > 0 ) {
    (void)write(fd,buffer,ret);
  }

  sleep(1); /* 保证消息全部发出 */
  close(fd);

  //4.开始发送--->结束发送
  //4.停止
	gettimeofday(&t2, NULL);
	printf("\t发送完响应消息->发送完文件用时:%lld us\n", duration2());

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

  printf("天哪！- 跳转%s成功！\n",argv[2] );
	
  logger(LOG,"nweb starting", argv[1], getpid());

	// 建立服务端侦听socket
  if((listenfd = socket(AF_INET, SOCK_STREAM,0)) <0)
    logger(ERROR, "system call","socket",0);
  port = atoi(argv[1]);
  if(port < 0 || port >60000)
    logger(ERROR,"Invalid port number (try 1->60000)",argv[1],0);
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  serv_addr.sin_port = htons(port);
  if(bind(listenfd, (struct sockaddr *)&serv_addr,sizeof(serv_addr)) <0)
    logger(ERROR,"system call","bind",0);
  if( listen(listenfd,64) <0)
    logger(ERROR,"system call","listen",0);

  printf("天哪！- 侦听%s成功！\n", argv[1]);

// hit!!
  for(hit=1; ;hit++) {
    length = sizeof(cli_addr);
    if((socketfd = accept(listenfd, (struct sockaddr *)&cli_addr, &length)) < 0)
      logger(ERROR,"system call","accept",0);
		printf("天哪！- 接收到了一个请求！\n");
	
//1.-->开始计时
		gettimeofday(&t1, NULL);
		web(socketfd,hit); 

    }
  
}


