#include <sys/prctl.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <stdbool.h>
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

//#define NUM_THREADS 40
#define NUM_REQ 10
#define NUM_READ 8
#define NUM_SEND 30

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


/* 队列状态和条件变量 */
typedef struct staconv {
	pthread_mutex_t mutex;		/* 辅助条件变量 cond */
	pthread_cond_t cond;		/* 等待和激发线程池中的线程 */
	int status;
}staconv;

/* 任务 */
typedef struct task {
	struct task * next;			/* 指向下一个任务 */
	void (*function)(void*arg);	/* 函数指针 */
	void * arg;					/* 函数参数指针 */
} task;

/* 任务队列 */
typedef struct taskqueue {
	pthread_mutex_t mutex;		/* 用于互斥读/写任务队列 */
	task * front;				/* 指向队首 */
	task * rear;				/* 指向队尾 */
	staconv * has_jobs;			/* 根据状态阻塞线程 */
	int len;					/* 任务队列中任务个数 */
} taskqueue;

/* 线程 */
typedef struct thread {
	int type;
	int id;						/* 线程 id */
	pthread_t pthread;			/* 封装的 POSIX 线程 */
	struct threadpool * pool;	/* 与线程池进行绑定 */
	void * mp;					/* 用来记录 运行和阻塞的时间，线程池中的线程往里写，记录线程取数据 */
} thread;

/* 线程池 */
typedef struct threadpool {
	thread ** threads;			/* 线程指针数组 */
	volatile int num_threads;	/* 线程池中线程数量 */	
	volatile int num_working;	/* 目前正在工作的线程个数 */
	pthread_mutex_t thcount_lock;/* 线程池锁，用于修改上面两个变量  */
	pthread_cond_t threads_all_idle;/* 用于销毁线程的条件变量 */
	taskqueue queue;			/* 任务队列 */
	volatile bool is_alive;		/* 线程池是否存活 */
}threadpool;

typedef struct application {
	threadpool * reqpool;
	threadpool * readpool;
	threadpool * sendpool;
} application;

/* 向定时记录性能线程传递的参数 */
typedef struct rd {
	application * app;	// threadpool -> thread -> mp -> 数据 -> 定时记录平均阻塞和运行时长
						// threadpool -> num_threads & num_working -> 记录并更新最大活跃数、最小活跃数，记录活跃数
						// threadpool -> queue -> 消息队列长度
						// threadpool -> is_alive -> 线程退出

	int max[3];			// 记录3个线程池最大活跃数
	int min[3];			// 记录3个线程池最小活跃数
	int ttl[3];			// 有效活跃数累加和，用于计算 平均活跃数
	int ttlcnt;			// 总的记录次数，用于做求平均活跃数的分母
} rd;

void * recorder(void * arg) {
	rd * r = (rd*)arg;
	int i0;
	
	r->ttlcnt=0;
	for(i0=0; i0<3; i0++) {
		r->max[i0]=0;
		r->min[i0]=999999;
		r->ttl[i0]=0;
	}

	while (r->app->reqpool->is_alive && r->app->readpool->is_alive && r->app->sendpool->is_alive) {
		//r->ttlcnt++;

		int fd;
		char buf[2048];
		if((fd = open("recorder.log", O_CREAT| O_WRONLY | O_APPEND,0644)) >= 0) {
			
			int n1 = r->app->reqpool->num_working;
			int n2 = r->app->readpool->num_working;
			int n3 = r->app->sendpool->num_working;
			if( n1 == 0 && n2 == 0 && n3 == 0) {
				close(fd);
			} else {

			// 获取时间信息
	  	    struct tm * ptr;
   		    time_t lt;
		    lt = time(NULL);
		    ptr = localtime(&lt);
		    char timebuf[48] = {0};
		    strftime(timebuf, sizeof(timebuf), "%H:%M:%S %a %d/%m/%Y", ptr);
			sprintf(buf, "\n[%s]\n", timebuf);
			write(fd, buf, strlen(buf));

			// 记录平均阻塞时间和运行时间
			long tr=0, tb=0;
			int i;
			for (i=0; i<NUM_REQ; i++) {
				int * p = (int*)r->app->reqpool->threads[i]->mp;	// 所有线程的阻塞时间之和
				tr += p[0];
				tb += p[1];
			}
			sprintf(buf, "reqpool  avg-runtime=%ld us, avg-blocktime=%ld us\n", tr/NUM_REQ, tb/NUM_REQ);
			write(fd, buf, strlen(buf));
			tr=0, tb=0;
			for (i=0; i<NUM_READ; i++){		// reqpool has same thread num with sendpool
				int * p = (int*)r->app->readpool->threads[i]->mp;
				tr += p[0];
				tb += p[1];
			}
			sprintf(buf, "readpool avg-runtime=%ld us, avg-blocktime=%ld us\n", tr/NUM_READ, tb/NUM_READ);
			write(fd, buf, strlen(buf));
			tr=0, tb=0;
			for (i=0; i<NUM_SEND; i++){		// reqpool has same thread num with sendpool
				int * p = (int*)r->app->sendpool->threads[i]->mp;
				tr += p[0];
				tb += p[1];
			}
			sprintf(buf, "sendpool avg-runtime=%ld us, avg-blocktime=%ld us\n", tr/NUM_SEND, tb/NUM_SEND);
			write(fd, buf, strlen(buf));
			// 记录最高活跃数、最低活跃数、平均活跃数；更新
			if (r->max[0] < n1) r->max[0] = n1;
			if (r->max[1] < n2) r->max[1] = n2;
			if (r->max[2] < n3) r->max[2] = n3;
			if (r->min[0] > n1) r->min[0] = n1;
			if (r->min[1] > n2) r->min[1] = n2;
			if (r->min[2] > n3) r->min[2] = n3;
			r->ttl[0] += n1;
			r->ttl[1] += n2;
			r->ttl[2] += n3;
			r->ttlcnt++;
			sprintf(buf, "reqpool  alive-thread-num : max=%d, min=%d, avg=%d\n", r->max[0], r->min[0], r->ttl[0]/r->ttlcnt);
			write(fd, buf, strlen(buf));
			sprintf(buf, "readpool alive-thread-num : max=%d, min=%d, avg=%d\n", r->max[1], r->min[1], r->ttl[1]/r->ttlcnt);
			write(fd, buf, strlen(buf));
			sprintf(buf, "sendpool alive-thread-num : max=%d, min=%d, avg=%d\n", r->max[2], r->min[2], r->ttl[2]/r->ttlcnt);
			write(fd, buf, strlen(buf));
			// 记录任务队列长度
			sprintf(buf, "reqpool  task-queue-length=%d\n", r->app->reqpool->queue.len);
			write(fd, buf, strlen(buf));
			sprintf(buf, "readpool task-queue-length=%d\n", r->app->readpool->queue.len);
			write(fd, buf, strlen(buf));
			sprintf(buf, "sendpool task-queue-length=%d\n", r->app->sendpool->queue.len);
			write(fd, buf, strlen(buf));

			(void)close(fd);
		  }
		}

		// sleep
		usleep(5);//5us
	}


}

/* web()参数: 网络请求info */
typedef struct webparam{
  int hit;					
  int fd;
  application * app;
} webparam;

typedef struct filename {
	int hit;
	char * fn;
	char * fstr;
	int fd;
	application * app;
} filename;

typedef struct msg {
	char * fc;//文件内容
	int fd;
	long len;
} msg;

unsigned long get_file_size(const char * path) {
  unsigned long filesize = 0;
  struct stat statbuff;
  if(stat(path, &statbuff)<0) {
    return filesize;
  }else {
    filesize = statbuff.st_size;
  }
  return filesize;
}

/* nweb.log */
void logger(int type, char *s1, char *s2, int socket_fd);

/* this is a child web server process, so we can exit on errors */
void web(void * data);

//向哪个队列放任务
void push_taskqueue(taskqueue * queue, task * curTask) {
	if(NULL == curTask) return;
	
	pthread_mutex_lock( &queue->mutex );//写taskqueue
	if(queue->front == NULL && queue->rear == NULL){
		curTask->next=NULL;
		queue->front = queue->rear = curTask;
	}else {
		curTask->next = NULL;
		queue->rear->next = curTask;
		queue->rear = curTask;
	}
	queue->len++;
	pthread_mutex_lock( &queue->has_jobs->mutex );//条件变量加锁
	queue->has_jobs->status=1;
	pthread_cond_broadcast( &queue->has_jobs->cond ); //激发阻塞的线程等待条件
	pthread_mutex_unlock( &queue->has_jobs->mutex );
	
	pthread_mutex_unlock( &queue->mutex );
}

void sendFile(void * msg2) {
	//将 filecontent 和 fd发送
	msg * m = (msg*)msg2;
	//logger(LOG,"file>>>>",m->fc,0);
	//一次读8K，分几步写入 socket
	/*int i=0,len;
	while (i < m->len) {
		if(m->len - i >= 8192) len= 8192;
		else len = m->len-i;
		write(m->fd, &m->fc[i], len);
		i += len;
	}*/
	write(m->fd, m->fc, m->len);
	/*char b[1024];
	sprintf(b, "sendFile: fc=%s, len=%ld", m->fc, m->len);
	logger(LOG, b, "", 666);*/

	//write(m->fd, m->fc, m->len);
	usleep(10000);
	free(m->fc);
	close(m->fd);
	free(m);
}

void readFile(void * file) {
	//1.读取文件
	//2.将 file 和 fd 打包成msg 添加到 msg-queue
	filename * f = (filename*)file;
	int file_fd;
    // 打开文件
    if((file_fd = open(f->fn, O_RDONLY)) == -1) {  /* open the file for reading */
      logger(NOTFOUND, "failed to open file",f->fn,f->fd);
	  free(f->fn);
	  free(f->fstr);
	  close(f->fd);
	  free(f);
	  return;
    }
    
    // 发送信息写入log
    logger(LOG,"SEND",f->fn,f->hit);
    // 获取文件长度
    long len = get_file_size(f->fn);
	// 发送响应消息
	char buffer[BUFSIZE+1];
    (void)sprintf(buffer,"HTTP/1.1 200 OK\nServer: nweb/%d.0\nContent-Length: %ld\nConnection: close\nContent-Type: %s\n\n", VERSION, len, f->fstr);
    // 写入log
	logger(LOG,"Header",buffer,f->hit);
    (void)write(f->fd,buffer,strlen(buffer));
	
	msg * m = malloc(sizeof(msg));
	m->fd=f->fd;
	m->fc=(char*)calloc(len+1, sizeof(char));
	m->len=len;
	
	int ret;
    /*
	while ((ret = read(file_fd, buffer, BUFSIZE)) > 0 ) {
      //(void)write(fd,buffer,ret);
	  buffer[ret]='\0';
	  //logger(LOG,buffer,"",-1);
	  strncat(m->fc, buffer, ret);
    }
	*/
	read(file_fd, m->fc, len);
    close(file_fd);

	/*char b[1024];
	sprintf(b, "readFile: fn=%s, fstr=%s, fc=%s", f->fn, f->fstr, m->fc);
	logger(LOG, b, "", 666);*/

	free(f->fn);
	free(f->fstr);
	free(f);
	
	task * t = malloc(sizeof(task));
	t->function = &sendFile;
	t->arg=(void*)m;
	push_taskqueue( &f->app->sendpool->queue, t);

}

void readReq(void * p) {//webparam(hit,app,fd,fstr)
	//1.读取文件信息
	//2.将 filename 和 fd 打包成filename 添加到 filename-queue
	int j, file_fd, buflen;
	long i, ret, len;
	char * fstr;
	char buffer[BUFSIZE+1]; /* !!!!!!!! 不能是静态缓冲区 */
	webparam * param = (webparam*)p;
	int fd = param->fd;
	int hit = param->hit;

	ret =read(fd,buffer,BUFSIZE);   /* 从客户端读取请求消息 */
	
	if(ret == 0 || ret == -1) {  /* 读消息失败 */
		logger(FORBIDDEN,"failed to read browser request","",fd);
		return;
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
			close(fd);
			return;
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
				close(fd);
				return;
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
		if(fstr == 0){
			logger(FORBIDDEN,"file extension type not supported",buffer,fd);
			close(fd);
			return;
		}
	
		filename * f = (filename*)malloc(sizeof(filename));
		f->hit = hit;
		f->fd = fd;
		int l = strlen(&buffer[5]);
		f->fn = (char*)malloc(sizeof(char) * (l+1) );
		strncpy(f->fn, &buffer[5], l);
		f->fn[l]='\0';
		f->app = param->app;
		int l2 = strlen(fstr);
		f->fstr = (char*)malloc(sizeof(char) * (l2+1));
		strncpy(f->fstr, fstr, l2);
		f->fstr[l2]='\0';

		/*char b[1024];
		sprintf(b, "readReq: fn=%s, fstr=%s", f->fn, f->fstr);
		logger(LOG, b, "", 666);*/

		task * t = (task*)malloc(sizeof(task));
		t->function = &readFile;
		t->arg = (void*)f;
		push_taskqueue(&param->app->readpool->queue, t);
		free(param);
	}
}


/* 初始化任务队列 */
void init_taskqueue(taskqueue * queue) {
	queue->front = NULL;
	queue->rear  = NULL;
	queue->len = 0;
	pthread_mutex_init(&queue->mutex,NULL);//互斥读写任务队列
	/* staconv * has_jobs;  */
	queue->has_jobs = (staconv*)malloc(sizeof(struct staconv));
	pthread_mutex_init(&queue->has_jobs->mutex,NULL);
	pthread_cond_init(&queue->has_jobs->cond,NULL);//阻塞和唤醒线程池中线程
	queue->has_jobs->status=0;//任务队列任务状态，0表示无任务
}

// 从哪个队列取任务
task * take_taskqueue(taskqueue * queue) {
	//从头部提取任务
	pthread_mutex_lock( &queue->mutex );
	task * t = NULL;
	if(queue->front == NULL && queue->rear == NULL) {
		pthread_mutex_unlock( &queue->mutex );
		return t;
	}else if (queue->front == queue->rear) {
		t = queue->front;
		queue->front = queue->rear = NULL;
	}else {
		t = queue->front;
		queue->front = queue->front->next;
	}
	queue->len--;
	if(queue->len == 0){
		pthread_mutex_lock( &queue->has_jobs->mutex );//条件变量加锁
		queue->has_jobs->status=0;
		pthread_mutex_unlock( &queue->has_jobs->mutex );
	}
	pthread_mutex_unlock( &queue->mutex );
	return t;
}

//销毁任务队列
void destory_taskqueue(taskqueue * queue) {
	pthread_mutex_lock( &queue->mutex );
	queue->len = 0;
	queue->has_jobs->status = 0;
	while(queue->front != queue->rear) {
		task * t = queue->front;
		queue->front = queue->front->next;
		free(t);
	}
	if(queue->front != NULL) free(queue->front);
	queue->front = queue->rear = NULL;
	free(queue->has_jobs);
	queue->has_jobs = NULL;
	pthread_mutex_unlock( &queue->mutex );
	free(queue);
}

void * thread_do(void * pth) {
	struct thread * pthread = (struct thread*)pth;
	char thread_name[128] = {0};
	sprintf(thread_name, "%d-thread-%d", pthread->type, pthread->id);
	
	prctl(PR_SET_NAME, thread_name);// 设置进程名

	/* 获得线程池 */
	threadpool * pool = pthread->pool;
	
	pthread_mutex_lock( &pool->thcount_lock );
	/* 在线程池初始化时，用于已经创建线程的计数 */
	pool->num_threads++;
	pthread_mutex_unlock( &pool->thcount_lock );
	struct timeval t1,t2;
	long t;
	int * p = (int*)pthread->mp;

	/* 线程一直往返运行，直到 pool->is_alive=false */
	while(pool->is_alive) {
		
		gettimeofday(&t1, NULL);
		/* 任务队列没有任务则阻塞 */
		// 使用 staconv * has_jobs 等待和激发
		pthread_mutex_lock( &pool->queue.has_jobs->mutex );
		while( pool->queue.has_jobs->status == 0 ) {
			pthread_cond_wait( &pool->queue.has_jobs->cond, &pool->queue.has_jobs->mutex  ); // 队列无任务，等待被激发
		}
		pthread_mutex_unlock( &pool->queue.has_jobs->mutex );
		gettimeofday(&t2, NULL);
		t = (t2.tv_sec - t1.tv_sec) * 1000000 + (t2.tv_usec - t1.tv_usec);
		p[1] += t;
		
		gettimeofday(&t1, NULL);
		if (pool->is_alive) {
			/* 对工作线程数量进行计数 */
			pthread_mutex_lock( &pool->thcount_lock );
			pool->num_working++;
			pthread_mutex_unlock( &pool->thcount_lock );

			void (*func)(void*);
			void * arg;
			task * curtask = take_taskqueue(&pool->queue);	/* take task from taskqueue */
			
			if(NULL != curtask) {
				logger(LOG,"task_taskqueue_successful",thread_name,0);
				func = curtask->function;
				arg  = curtask->arg;
				func(arg);//执行任务 web(void*)
				free(curtask);//释放任务
			}

			/* 线程已经将任务执行完成，需要更改工作线程数量 */
			pthread_mutex_lock( &pool->thcount_lock );
			pool->num_working--;
			pthread_mutex_lock(&pool->queue.mutex);
			if(0 == pool->num_working && pool->queue.len == 0) {	/* 任务全部完成，让阻塞在waitThreadPool上的线程继续运行 */
				pthread_cond_broadcast( &pool->threads_all_idle );		//条件激发，此时工作线程数=0，任务数=0 ==> 等待所有线程停止，进行线程池资源释放
			}
			pthread_mutex_unlock(&pool->queue.mutex);
			pthread_mutex_unlock( &pool->thcount_lock );

		}
		gettimeofday(&t2, NULL);
		t = (t2.tv_sec - t1.tv_sec) * 1000000 + (t2.tv_usec - t1.tv_usec);
		p[0] += t;

	}
	
	pthread_mutex_lock( &pool->thcount_lock );
	/* 线程退出，要修改线程池中的数量 */
	pool->num_threads--;
	pthread_mutex_unlock( &pool->thcount_lock );
	
	//销毁此线程与对外的共享空间
	char shmname[128];
	if (pthread->type == 1) sprintf(shmname, "seqpool-thread-%d", pthread->id);
	if (pthread->type == 2) sprintf(shmname, "readpool-thread-%d", pthread->id);
	if (pthread->type == 3) sprintf(shmname, "sendpool-thread-%d", pthread->id);
	if (munmap(pthread->mp, 2 * sizeof(long)) == -1) {
		logger(LOG, "munmap", "", -1);
	}
	if (shm_unlink(shmname) == -1) {
		logger(LOG, "shm_unlink", shmname, -1);
	}

	return NULL;
}

/* 向线程池加入任务 */
void addTask2ThreadPool(threadpool * pool, task * curtask) {	
	//将任务加入队列
	//logger(LOG,"addTask2ThreadPool","",0);	
	push_taskqueue(&pool->queue, curtask);
}

/* 等待当前任务全部运行完 */
void waitThreadPool(threadpool * pool) {
	pthread_mutex_lock(&pool->thcount_lock);
	while(pool->queue.len || pool->num_working) {
		pthread_cond_wait(&pool->threads_all_idle, &pool->thcount_lock);
	}
	pthread_mutex_unlock(&pool->thcount_lock);

}

/* 销毁线程池 */
void destoryThreadPool(threadpool * pool,int n) {
	//1.如果当前任务队列中有任务，需等待任务队列为空，并且运行线程执行任务后
	waitThreadPool(pool);	//len=0 && num_working=0
	//2.销毁任务队列
	pthread_mutex_lock(&pool->thcount_lock);
	pool->is_alive = false;
	pthread_mutex_unlock(&pool->thcount_lock);

	destory_taskqueue(&pool->queue);
	//3.销毁线程指针数组，并释放所有为线程池分配的内存
	int i=0;
	for(;i<n;i++) free(pool->threads[i]);//free every thread
	free(pool->threads);//free array
	free(pool);//free pool
}

void destoryApp(application * app) {
	destoryThreadPool(app->reqpool,NUM_REQ);
	destoryThreadPool(app->readpool,NUM_READ);
	destoryThreadPool(app->sendpool,NUM_SEND);
}

/* 获得当前线程池中正在运行线程的数量 */
int getNumofThreadWorking(threadpool * pool) {
	return pool->num_working;
}

/* 创建线程 */
int create_thread(struct threadpool * pool, struct thread * pthread, int id, int type) {
	pthread->pool = pool;
	pthread->id = id;
	pthread->type=type;
	int shm_fd;
	char shmname[128];
	if (type == 1) sprintf(shmname, "seqpool-thread-%d", id);
	if (type == 2) sprintf(shmname, "readpool-thread-%d", id);
	if (type == 3) sprintf(shmname, "sendpool-thread-%d", id);
	
	if((shm_fd = shm_open(shmname, O_RDWR|O_CREAT, 0666)) < 0) {
		logger(LOG, "shm_open", "", -1);
		exit(-1);
	}
	ftruncate(shm_fd, 2 * sizeof(long));
	pthread->mp = mmap(NULL, 2 * sizeof(long), PROT_READ|PROT_WRITE, MAP_SHARED, shm_fd, 0);
	if (pthread->mp == MAP_FAILED) {
		logger(LOG, "mmap", "", -1);
		exit(-1);
	}
	int * p = (void*)pthread->mp;
	p[0] = 0;
	p[1] = 0;

	pthread_create( &pthread->pthread, NULL, (void*)thread_do, (void*)pthread );//例程 thread_do, 参数 thread*
	pthread_detach( pthread->pthread );
	return 0;
}

/* 线程池初始化函数 */
struct threadpool * initThreadPool(int threads_num, int type) {
	// 创建线程池空间
	threadpool * pool = (threadpool*)malloc(sizeof(struct threadpool));
	pool->num_threads=0;
	pool->num_working=0;
	pthread_mutex_init(&(pool->thcount_lock), NULL);//互斥量,用来修改 num_threads和num_working
	pthread_cond_init(&(pool->threads_all_idle), NULL);//条件变量,用来销毁线程
	init_taskqueue(&(pool->queue));
	pool->is_alive = true;
	/* 存放`thread*`的数组 */
	pool->threads=(struct thread **)malloc(threads_num * sizeof(struct thread));//总共创建NUM_THREADS个线程
	for(int i=0; i<threads_num; i++) {
		pool->threads[i] = (struct thread *)malloc(sizeof(struct thread));
		create_thread(pool, pool->threads[i], i, type);// i为线程id，循序执行，不必上锁
	}
	
	// 忙等待，直到所有线程创建完毕
	while(pool->num_threads != threads_num) {}
	
	//logger(LOG,"initThreadPool","",0);	
	return pool;
}

struct application * initApp() {
	struct application * app = (struct application *)malloc(sizeof(struct application));
	app->reqpool  = initThreadPool(NUM_REQ, 1);
	app->readpool = initThreadPool(NUM_READ, 2);
	app->sendpool = initThreadPool(NUM_SEND, 3);
	
	// 创建记录性能参数的线程
	rd * r = malloc(sizeof(struct rd));
	r->app = app;
	
	pthread_t pid;
	pthread_create(&pid, NULL, (void*)recorder, (void*)r);
	pthread_detach(pid);

	return app;
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
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  serv_addr.sin_port = htons(port);
  if(bind(listenfd, (struct sockaddr *)&serv_addr,sizeof(serv_addr)) <0)
    logger(ERROR,"system call","bind",0);
  if( listen(listenfd,64) <0)
    logger(ERROR,"system call","listen",0);

	//struct threadpool * pool = initThreadPool();//不传递参数，用宏常量
	struct application * app = initApp();

// hit!!
  for(hit=1; hit<5000;hit++) {
    length = sizeof(cli_addr);
    if((socketfd = accept(listenfd, (struct sockaddr *)&cli_addr, &length)) < 0) {
      logger(ERROR,"system call","accept",0);
	  continue;
	}
	
	webparam * param = malloc(sizeof(webparam));
	param->fd = socketfd;
	param->hit = hit;
	param->app = app;

	task * curTask = malloc(sizeof(struct task));
	curTask->function = &readReq;
	curTask->arg = (void*)param;

	addTask2ThreadPool(app->reqpool, curTask);

  }

  destoryApp(app);

}//main

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

  //if(type == NOTFOUND || type == FORBIDDEN) pthread_exit((void*)3);
  //if(type == ERROR) exit(3);
}

/* this is a child web server process, so we can exit on errors */
void web(void * data)
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
    close(file_fd);
    usleep(10000); /* 保证消息全部发出 */
  }//else
  close(fd);
  free(param);
}


