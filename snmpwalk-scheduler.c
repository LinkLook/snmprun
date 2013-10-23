/**
 * @file		
 * @brief  	snmpwalk-scheduler

 * @version	1.0
 * @author		weida
 * @date		2013.4.10

 * initialise version.
 */
#define _GNU_SOURCE

#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <assert.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <wait.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>

#include <pthread.h>
#include <semaphore.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <ucontext.h>
#include <sys/ucontext.h>
#include <execinfo.h>

#define EPOLL_SIZE     1024
#define MAX_IMAGE_SIZE 1024

#define LOG_ERR(fmt, args...)						\
  do{									\
     log_write("ERR", __func__, __FILE__, __LINE__, fmt, ##args);	\
     }while(0)

#define LOG_INFO(fmt, args...)						\
  do {									\
    log_write("INFO", __func__, __FILE__, __LINE__, fmt, ##args);	\
  }while(0)

#ifdef DEBUG
#define LOG_DEBUG(fmt, args...)						\
  do {									\
    log_write("DEBUG", __func__, __FILE__, __LINE__, fmt, ##args);	\
  }while(0)
#else
#define LOG_DEBUG(fmt, args...) do{}while(0)
#endif /* ifdef DEBUG */


    struct signo_handler
    {
      int	signo;
      void	*handler;
    };

typedef int (*handler_func_t)(int fd, int event, void *data);
struct event_info
{
  handler_func_t handler;
  void *data;
};
struct child_ctx
{
  ///	Child's process id.
  pid_t	pid;
  ///	Child's return value.
  int	statloc;
  int   conn_fd;
  time_t start_tm;
  ///	The executable child path and arguments.
  char 	image[MAX_IMAGE_SIZE];
};


static struct event_info global_handlers[EPOLL_SIZE];
static struct child_ctx global_children[EPOLL_SIZE];
static int global_efd = -1;


static int tcp_main_service(int fd, int event, void *);
static int tcp_main_accept(int fd, int event, void *);
static int tcp_open_server_socket(unsigned short port, int * real_port);

static int main_loop();

static int thread_nanosleep(unsigned int sec, unsigned int nsec);
static int fd_set_nonblock(int fd);

static int register_event(int fd, handler_func_t handler, void *data);
static int unregister_event(int fd);

static int check_deadchild();
static int child_pipefd_handler(int fd, int event, void *ptr);
static int start_image(const char *image, int conn_fd);
static int close_all_fds(int min, int max);
static int make_child_args(const char *image, char *args, int args_size, char **argv, int argv_size);
static int exec_image(const char *image, int infd, int outfd);

static void sig_handler(int sig);
static void sig_btdump();
static void sigaction_handler(int value, siginfo_t *si, ucontext_t *uap) ;
static int sig_setup(struct signo_handler * sig, int nsig);

static void do_write(int sock, const char *data, int size);
static void logger_write_v(const char * prog, const char * tm, 
			   const char *level, const char *func, 
			   const char *file, int line, 
			   const char *fmt, va_list argptr);
static void log_write(const char *level, const char *func, const char *file, int line, const char *fmt, ...);

static void datetime_now(char * res);
static void datetime_localtime_safe(time_t time, long timezone, struct tm *tm_time);
static void safely_close(int fd);


int main(int argc, char ** argv)
{
  unsigned short	port = 10086;
  int sts;
  int listen_fd=0;

  struct signo_handler	signals[] = 
    {
      {SIGCHLD, sig_handler},
      {SIGPIPE, sig_handler},
      {SIGINT, sig_handler},
    };
	
  //Setup signal handler.
  sig_setup(signals, sizeof(signals)/sizeof(signals[0]));

  //Create epoll.
  global_efd = epoll_create(EPOLL_SIZE); 
  if(global_efd < 0){
    LOG_ERR("epoll_create(%d) failed, errno=%d(%m)", EPOLL_SIZE, errno);
    return 1;
  }

  //Create TCP Server.
  listen_fd=tcp_open_server_socket(port, NULL);
  if(listen_fd < 0){
    LOG_ERR("tcp_open_server_socket(%d)", port);
    return 1;
  }

  // Set sock to be non-blocking
  sts=fd_set_nonblock(listen_fd);
  if(sts < 0){
    LOG_ERR("fd_set_nonblock(%d)", listen_fd);
    return 1;
  }

  sts = register_event(listen_fd, &tcp_main_accept, NULL);
  if(sts < 0){
    LOG_ERR("regster_event(%d)", listen_fd);
  }

  main_loop();

  return 0;
}
static int thread_nanosleep(unsigned int sec, unsigned int nsec)
{
  struct timespec         ts={sec, nsec};
  struct timespec         rem;

  do{
    if(nanosleep(&ts, &rem) == 0)
      break;
    if(errno == EINTR){
      ts.tv_sec = rem.tv_sec;
      ts.tv_nsec = rem.tv_nsec;
      rem.tv_sec=rem.tv_nsec=0;
      continue;
    }
    LOG_ERR("nanosleep(%d,%d,%d,%d) errno=%d(%m)", ts.tv_sec, ts.tv_nsec, rem.tv_sec, rem.tv_nsec, errno);
    return -errno;
  }while(1);
  return 0;
}

static int fd_set_nonblock(int fd) 
{
  int             flag, sts;

  flag = fcntl(fd, F_GETFL);
  if (flag < 0){ 
    LOG_ERR(" fcntl(%d, F_GETFL) failed, errno=%d(%m)", fd, errno);
    return flag;
  }
  flag |= O_NONBLOCK;
  sts = fcntl(fd, F_SETFL, flag);
  if (sts < 0){
    LOG_ERR(" fcntl(%d, F_SETFL, 0x%x) failed, errno=%d(%m)", fd, flag, errno);
  }
  return sts;
}
static int tcp_open_server_socket(unsigned short port, int * real_port)
{
  struct sockaddr_in	addr;

  int			sock = -1;
  // SO_REUSEADDR : int
  int			enable= 1;
  socklen_t	addrlen;

  bzero(&addr, sizeof(addr));
  addr.sin_family	= AF_INET ;
  addr.sin_port		= htons(port);
  addr.sin_addr.s_addr	= htonl(INADDR_ANY);

  do{
    if((sock = socket(AF_INET, SOCK_STREAM, 0))< 0){
      LOG_ERR("socket(AF_INET, SOCK_STREAM, 0), errno=%d(%m)", errno);
      break;
    }

    if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&enable, sizeof(enable))){ 
      LOG_ERR("setsockopt(SO_REUSEADDR), errno=%d(%m)", errno);
      break;
    }
    /*
      recv_buff_size	= trans_size_kb * 1024 ;
      if(setsockopt(sock, SOL_SOCKET, SO_RCVBUF, 
      &recv_buff_size, sizeof(recv_buff_size))){ 
      LOG_ERR("setsockopt(SO_RCVBUF, %u), errno=%d(%m)\n", recv_buff_size, errno);
      break;
      }
    */

    if(bind(sock, (struct sockaddr*)&addr, sizeof(addr))){
      LOG_ERR("bind()failed, errno=%d(%m)", errno);
      break;
    }

    if(listen(sock, SOMAXCONN)){
      LOG_ERR("listen() failed, errno=%d(%m)\n", errno);
      break;
    }
    addrlen = sizeof(addr);
    if(getsockname(sock, (struct sockaddr*)&addr, &addrlen)){
      LOG_ERR("getsockname() failed, errno=%d(%m)\n", errno);
      break;
    }
    if(real_port != NULL){
      *real_port = ntohs(addr.sin_port);
    }
    LOG_INFO("create server sock ok. [sock=%d, port=%d]", sock, port);
    return sock;
  }while(0);

  if(sock >= 0) {
    safely_close(sock);
  }
  return -1;
}

/**
   @brief	Signal handler
*/
static void sig_handler(int sig) 
{ 
  switch(sig) {
    case SIGCHLD:
    case SIGPIPE:
    case SIGINT:
    default:;
    }
  return;
}

static int unregister_event(int fd)
{
  int sts;

  LOG_DEBUG("unregister_event: fd=%d",fd);

  if(global_handlers[fd].handler == NULL){
    LOG_DEBUG("already unregiste.");
    return 0;
  }

  sts = epoll_ctl(global_efd, EPOLL_CTL_DEL, fd, NULL);
  if(sts < 0){
    LOG_ERR("epoll_ctl EPOLL_CTL_DEL fd=%d, errno=%d(%m)", fd, errno);
  }else{
    global_handlers[fd].handler=NULL;
    global_handlers[fd].data = NULL;
  }
  return sts;
}
static int register_event(int fd, handler_func_t handler, void *data)
{
  int sts;
  struct epoll_event	ev;

  assert(fd > 0 && fd < EPOLL_SIZE);
  assert(handler != NULL);
  assert(global_handlers[fd].handler == NULL);

  ev.events = EPOLLIN | EPOLLET;
  ev.data.fd = fd;
  sts = epoll_ctl(global_efd, EPOLL_CTL_ADD, fd, &ev);
  if(sts < 0){
    LOG_ERR("epoll_ctl EPOLL_CTL_ADD fd=%d, errno=%d(%m)", fd, errno);
  }else{
    global_handlers[fd].handler = handler;
    global_handlers[fd].data = data;
  }
  return sts;
}

static int tcp_main_accept(int fd, int event, void * data)
{
  struct sockaddr_in	from_addr;
  socklen_t		from_addr_len;
  //  char    strip[INET_ADDRSTRLEN]={'\0'};
  int		clientfd;
  int		sts;

  //  assert(fd == listen_fd);
  //  assert(data == selfd);
  if(!(event & EPOLLIN)){
    LOG_ERR("arguments error.!(event & EPOLLIN)");
    return -1;
  }
  do{
    from_addr_len = sizeof(from_addr);
    clientfd = accept(fd, (struct sockaddr *)&from_addr, &from_addr_len);
    if(clientfd < 0) {
      if(errno == EINTR) continue;
      if(errno == EAGAIN) break;
      LOG_ERR("accept error.[error:%d(%m)]", errno);
      break;
    }
    /*
      memset(strip, '\0', sizeof(strip));
      inet_ntop(AF_INET, &(from_addr.sin_addr), strip, sizeof(strip));
    */
		
    //set NONBLOCK
    sts = fd_set_nonblock(clientfd);
    if(sts < 0){ 
      LOG_ERR("fd_set_nonblock() failed. [clientfd:%d]", clientfd);
      safely_close(clientfd);
      continue;
    }
    sts = register_event(clientfd, &tcp_main_service, data);
    if(sts < 0) {
      LOG_ERR("register_event() failed. [clientfd:%d]", clientfd);
      safely_close(clientfd);
      continue;
    }
    //		LOG_DEBUG("accept()=%d [%s:%d]", clientfd, strip, ntohs(from_addr.sin_port));
  }while(1);

  return 0;
}

static int tcp_main_service(int fd, int event, void * data)
{
  int	sts;
  //  struct sockaddr_in	from_addr;
  //  socklen_t		from_addr_len;
  int  bytes;
  char buff[MAX_IMAGE_SIZE+8]={'\0'};

  //  assert(data == selfd);

  if(!(event & EPOLLIN)){
    LOG_ERR("arguments error.!(event & EPOLLIN)");
    return -1;
  }

  //Get client peer address.
  /*
    memset(&from_addr, '\0', sizeof(from_addr));
    from_addr_len = sizeof(from_addr);
    sts = getpeername(fd, (struct sockaddr *)&from_addr, &from_addr_len);
    if(sts < 0){
    LOG_ERR("getpeername(%d)failed, errno=%d(%m)", fd, errno);
    }
  */

  do{
    bytes = recv(fd, buff, sizeof(buff)-8, 0);
    if(bytes <= 0){
      if(bytes == 0){
	LOG_DEBUG("peer closed. fd=%d", fd);
	break;
      }
      //assert(bytes < 0);
      if(errno == EINTR){continue;}
      if(errno == EAGAIN){return 0;}

      LOG_ERR("read(%d) failed.[errno=%d(%m)]", fd, errno);
      break;
    }
    buff[bytes] = '\0';
    LOG_INFO(buff);
    sts = start_image(buff, fd);
    if(sts < 0){
      unregister_event(fd);
      shutdown(fd, SHUT_RDWR);
      safely_close(fd);
    }
    return 0;
  }while(0);

  //	while(recv(fd, buff, sizeof(buff), 0) > 0){ LOG_ERR("recv(%d)>0", fd); }

  unregister_event(fd);
  shutdown(fd, SHUT_RD);

  return 0;
}
static int main_loop()
{
  int count,i;
  int fd;
  int timeout = 1000*2;//2 second
  struct epoll_event	events[EPOLL_SIZE];    

  while(1){
    count = epoll_wait(global_efd, events, EPOLL_SIZE, timeout);
    //    LOG_DEBUG("epoll_wait() count=%d", count);    
    //    thread_nanosleep(1, 0);
    if(count <= 0) {

      if(count == 0){
	check_deadchild();
	continue;
      }
      if(errno == EINTR){
	check_deadchild();
	continue;
      }
      LOG_ERR("epoll_wait failed. efd=%d [error:%d(%m)]", global_efd, errno);
      continue;
    }
    for(i=0; i<count; ++i){
      fd = events[i].data.fd;
      //      LOG_DEBUG("event: fd=%d, events=%d", fd, events[i].events);
      global_handlers[fd].handler(fd, events[i].events, global_handlers[fd].data);
    }
  }
  return 0;
}
/** @brief		Collect zombied child to be finished totally.
 */
static int check_deadchild()
{
  int		count = 0;
  int		deadPid;
  int		statloc;
  int i;
  struct child_ctx *ctx;

  // Loop until we pick up all the child statuses
  while(1) {
    // Get status and info from terminating child
    deadPid = waitpid(-1, &statloc, WNOHANG);
    // If error or no more zombies
    if(deadPid <= 0){
      if(deadPid < 0 && errno != ECHILD) {
	LOG_ERR(" waitpid() failed, errno=%d(%m)", errno);
      }
      break;
    }

    LOG_INFO(" Zombied child(pid=%d) statloc=%d", deadPid, statloc);

    for(i=0; i<EPOLL_SIZE; i++){
      ctx = &global_children[i];
      if(ctx->pid == deadPid){
	unregister_event(ctx->conn_fd);

	//	shutdown(ctx->conn_fd, SHUT_RDWR);
	safely_close(ctx->conn_fd);

	unregister_event(i);
	//	shutdown(i ,SHUT_RDWR);
	safely_close(i);

	memset(ctx, '\0', sizeof(*ctx));
      }
    }

    count ++;
  }

  return count;
}
static int child_pipefd_handler(int fd, int event, void *ptr)
{
  struct child_ctx *	child = (struct child_ctx *)(ptr);

  char	buff[4096+8]={'\0'};
  int	bytes;
  int   len,sts;

  if(!(event & EPOLLIN)){
    LOG_ERR("arguments error.!(event & EPOLLIN)");
    return -1;
  }

  do{
    bytes = read(fd, buff, sizeof(buff)-8);
    if(bytes <= 0){
      if(bytes == 0){
	LOG_DEBUG("child pipe fd=%d closed.", fd);
	break;
      }
      //assert(bytes < 0);
      if(errno == EINTR){continue;}
      if(errno == EAGAIN){return 0;}

      LOG_ERR("read(%d) failed.[errno=%d(%m)]", fd, errno);
      break;
    }
    len = 0;
    do{
      sts = send(child->conn_fd, buff+len, bytes-len, 0);
      if(sts < 0){
	if(errno == EAGAIN || errno == EINTR){continue;}
	LOG_ERR("send(%d) failed, [errno=%d(%m)]", child->conn_fd);
	//Drop
	break;
      }
      len += sts;
    }while(len < bytes);
  }while(1);

  return 0;
}

static int start_image(const char *image, int conn_fd)
{
  int		sts, pid;
  int		pipefds[2] = {-1, -1};

  struct child_ctx * child = NULL;

  // Create the pipe to talk with child
  sts = socketpair(AF_UNIX, SOCK_STREAM, 0, pipefds);
  if(sts < 0) {
    LOG_ERR("socketpair() failed, errno=%d(%m)", errno);
    return -1;
  }

  // Set pipe fd to be non-blocking
  if(fd_set_nonblock(pipefds[1]) != 0){
    LOG_ERR("fd_set_nonblock(%d) failed", pipefds[1]);
    return -1;
  }

  child = &global_children[pipefds[1]];
  strncpy(child->image, image, sizeof(child->image)-1);

  pid = fork();
  if(pid < 0){
    //fork() failed.
    LOG_ERR("fork() failed, errno=%d(%m)", errno);
    safely_close(pipefds[0]);
    safely_close(pipefds[1]);
    return -1;
  }
  if(pid == 0){
    // Child routine
    thread_nanosleep(0, 1000*1000);
    safely_close(pipefds[1]);
    sts = exec_image(child->image, pipefds[0], pipefds[0]);
    //child should not return, except error.
    exit(sts);
  }
  // Parent routine
  safely_close(pipefds[0]);

  child->pid	= pid;
  child->conn_fd = conn_fd;
  child->start_tm =time(NULL);

  register_event(pipefds[1], &child_pipefd_handler, child);
	
  return 0;
}
/**
 * @brief	Close all socket from min to max.
 *
 * @param [IN]min	minimum socket.
 * @param [IN]max	maximum	socket.(-1 means no limit)
 */
static int close_all_fds(int min, int max)
{
  int		fd;
  int		sts;

  if(max <= min) {
    max = sysconf(_SC_OPEN_MAX);
    if(max < 0){      max = 64;    }
    if(max < min){      return 0;    }
  }

  for (fd = min; fd <= max; fd++) {
    sts = close(fd);
    if(sts != 0) {
      errno = 0;
      continue;
    }
  }

  return 0;
}
static int make_child_args(const char *image, char *args, int args_size, char **argv, int argv_size)
{
  int i, len, argc;

  argc=0;
  strncpy(args, image, args_size-1);
  len = strlen(args);

  for(i = 0; i < len; ++i)  {
    if(isspace(args[i])){ 
      args[i] = '\0';
      if(args[i+1] && isspace(args[i+1])) {
	continue;
      }
      if(++argc >= argv_size) {
	break;
      } else{continue;}
    }
    if(argv[argc] == NULL){
      argv[argc] = args+i;
    }
  }
  return argc;
}

/**
   This function is running in child process.
*/
static int exec_image(const char *image, int infd, int outfd) 
{
  int	sts;
  int	argc;
  char *argv[128] = {NULL};
  char 	args[MAX_IMAGE_SIZE] = {'\0'};

  argc = make_child_args(image, args, sizeof(args), argv, sizeof(argv)/sizeof(argv[0]));

  if(!argv[0] || argc <= 0){
    return 105;
  }
  // Redirect stdin/stdout/stderr to the fd given.
  if(infd != STDIN_FILENO){
    close(STDIN_FILENO);
    if(dup2(infd, STDIN_FILENO) != STDIN_FILENO){
      exit(106);
    }
  }
  
  if(outfd != STDOUT_FILENO){
    close(STDOUT_FILENO);
    if(dup2(outfd, STDOUT_FILENO) != STDOUT_FILENO) {
      exit(107);
    }
  }

  if(outfd != STDERR_FILENO){
    close(STDERR_FILENO);
    if(dup2(outfd, STDERR_FILENO) != STDERR_FILENO) {
      exit(108);
    }
  }

  // Close all non-std* file descriptors
  close_all_fds(STDERR_FILENO+1, -1);

  // Finally, execute the image
  execvp(argv[0], argv);

  //never return, unless error.
  sts = strlen("exec failed");
  if(write(STDERR_FILENO, "exec failed", sts) != sts){
    exit(109);
  }

  //LOG_ERR(": execv of '%s' failed, errno %d (%m)", image, errno);
  return 110;
}

static void sig_btdump()
{
  char	buff[1024]={'\0'};
  int	size, i;
  char	**strings=NULL;
  void	*buffer[64]={NULL};
  int		len;

  size = backtrace(buffer, (sizeof(buffer)/sizeof(buffer[0])));
  strings = (char**)backtrace_symbols(buffer, size);
  len = sprintf(buff, "\nBacktrace:%d\t%s\n", size, strings==NULL? "strings:NULL":"");
  write(STDERR_FILENO, buff, len);
  if(strings){
    for(i = 0; i < size; i++){
      len = snprintf(buff, sizeof(buff), "\t%d: %s\n", i, strings[i]);
      write(STDERR_FILENO, buff, len);
    }
    free(strings);
  }
  return;
}


/**
 * @brief	signal handler that dumps debugging information

 * @param [IN]value	signal number
 * @param [IN]si	siginfo block
 * @param [IN]uap	pointer to user context area
 * @return	nothing
 */

static void sigaction_handler(int value, siginfo_t *si, ucontext_t *uap) 
{
  char	buff[1024]={'\0'}, tmbuf[32]={'\0'};
  int		save_errno; 
  pthread_t	tid;
  mcontext_t	*mc;
  int		len;

  // Ignore SIGPIPEs if they get here
  //  if (value == SIGPIPE)return;

  // Save errno (as advised) and print out basic info about signal
  save_errno = errno;
  // Back to default handling
  //  if (value != SIGUSR2) signal(value, SIG_DFL);
  tid = pthread_self();
  datetime_now(tmbuf);

  len = sprintf(buff, "%s PID[%d] Thread[%p] received signal %d (%s)\n",
		tmbuf, getpid(), (void *)tid, value, strsignal(value));
  write(STDERR_FILENO, buff, len);

  if(si != NULL) {
    len = sprintf(buff, "  signal %d, errno %d, code %d, pid %d uid %d\n"
		  "  status %d, addr %p, sival_ptr %p, si_band %ld\n",
		  si->si_signo, si->si_errno, si->si_code, si->si_pid, si->si_uid,
		  si->si_status, si->si_addr, si->si_value.sival_ptr, si->si_band);
    write(STDERR_FILENO, buff, len);
  }

  // Dump user context, if present

  len = 0;
  mc = &uap->uc_mcontext;
  if (mc != NULL) {
#ifdef __x86_64__
    len += sprintf(buff+len, "\n");
    len += sprintf(buff+len, "Context:  R8  0x%016lx R9  0x%016lx R10 0x%016lx\n", 
		   mc->gregs[REG_R8], mc->gregs[REG_R9], mc->gregs[REG_R10]);
    len += sprintf(buff+len, "Context:  R11 0x%016lx R12 0x%016lx R13 0x%016lx\n", 
		   mc->gregs[REG_R11], mc->gregs[REG_R12], mc->gregs[REG_R13]);
    len += sprintf(buff+len, "Context:  R14 0x%016lx R15 0x%016lx RDI 0x%016lx\n", 
		   mc->gregs[REG_R14], mc->gregs[REG_R15], mc->gregs[REG_RDI]);
    len += sprintf(buff+len, "Context:  RSI 0x%016lx RBP 0x%016lx RBX 0x%016lx\n", 
		   mc->gregs[REG_RSI], mc->gregs[REG_RBP], mc->gregs[REG_RBX]);
    len += sprintf(buff+len, "Context:  RDX 0x%016lx RAX 0x%016lx RCX 0x%016lx\n", 
		   mc->gregs[REG_RDX], mc->gregs[REG_RAX], mc->gregs[REG_RCX]);
    len += sprintf(buff+len, "Context:  RSP 0x%016lx RIP 0x%016lx EFL 0x%016lx\n", 
		   mc->gregs[REG_RSP], mc->gregs[REG_RIP], mc->gregs[REG_EFL]);
    len += sprintf(buff+len, "Context:  ERR 0x%016lx CR2 0x%016lx\n", mc->gregs[REG_ERR], mc->gregs[REG_CR2]);
    len += sprintf(buff+len, "Context:  CSGSFS 0x%016lx TRAPNO 0x%016lx OLDMASK 0x%016lx\n", 
		   mc->gregs[REG_CSGSFS], mc->gregs[REG_TRAPNO], mc->gregs[REG_OLDMASK]);
    len += sprintf(buff+len, "\n");
    len += sprintf(buff+len, "ucontext: flags 0x%lx\n", uap->uc_flags);
    len += sprintf(buff+len, "   stack: base %p size 0x%lx flags 0x%x\n", 
		   uap->uc_stack.ss_sp, uap->uc_stack.ss_size, uap->uc_stack.ss_flags);
#else
    len += sprintf(buff+len, "\n");
    len += sprintf(buff+len, "Context:  gs  0x%08x fs  0x%08x es  0x%08x ds   0x%08x\n", 
		   mc->gregs[REG_GS], mc->gregs[REG_FS], mc->gregs[REG_ES], mc->gregs[REG_DS]);
    len += sprintf(buff+len, "          edi 0x%08x esi 0x%08x ebp 0x%08x uesp 0x%08x\n", 
		   mc->gregs[REG_EDI], mc->gregs[REG_ESI], mc->gregs[REG_EBP], mc->gregs[REG_UESP]);
    len += sprintf(buff+len, "          ebx 0x%08x edx 0x%08x ecx 0x%08x eax  0x%08x\n", 
		   mc->gregs[REG_EBX], mc->gregs[REG_EDX], mc->gregs[REG_ECX], mc->gregs[REG_EAX]);
    len += sprintf(buff+len, "          err 0x%08x eip 0x%08x cs  0x%08x esp  0x%08x\n", 
		   mc->gregs[REG_ERR], mc->gregs[REG_EIP], mc->gregs[REG_CS], mc->gregs[REG_ESP]);
    len += sprintf(buff+len, "          ss  0x%08x trapno 0x%08x eflags 0x%08x\n", 
		   mc->gregs[REG_SS], mc->gregs[REG_TRAPNO], mc->gregs[REG_EFL]);
    len += sprintf(buff+len, "\n");
    len += sprintf(buff+len, "ucontext: flags 0x%lx\n", uap->uc_flags);
    len += sprintf(buff+len, "   stack: base %p size 0x%x flags 0x%x\n", 
		   uap->uc_stack.ss_sp, uap->uc_stack.ss_size, uap->uc_stack.ss_flags);
#endif

    write(STDERR_FILENO, buff, len);
  }
  // Dump stack, then re-enable signal for default handling
  sig_btdump();

  /*
    LOG_ERR(": PID %d Thread %p received signal %d (%s)",
    getpid(), (void *)tid, value, strsignal(value));

    LOG_ERR("[Handler done]");
  */

  len = sprintf(buff, "%s Programe exit!\n", tmbuf);
  write(STDERR_FILENO, buff, len);

  errno = save_errno;

  //	pause();
  exit(1);
  return;
}
static int sig_setup(struct signo_handler * sig, int nsig) 
{
  int	i, sts;
  struct sigaction	sigAction;

  struct signo_handler	signals[] = 
    {
      // Default handlers
      {SIGHUP,   SIG_DFL},		{SIGALRM, SIG_DFL},
      {SIGQUIT,  SIG_DFL},		{SIGPROF, SIG_DFL},
      {SIGCONT,  SIG_DFL},		{SIGURG,  SIG_DFL},
      {SIGWINCH, SIG_DFL},		{SIGPWR,  SIG_DFL},
      {SIGTERM,  SIG_DFL},		{SIGRTMIN,SIG_DFL},

      // Ignored
      {SIGPIPE, SIG_IGN},
      {SIGCHLD, SIG_IGN},

      // Output information

      {SIGUSR1, sigaction_handler},		
      {SIGTRAP,   sigaction_handler},	{SIGABRT,   sigaction_handler},
      {SIGIOT,    sigaction_handler},	{SIGBUS,    sigaction_handler},
      {SIGFPE,    sigaction_handler},	{SIGSEGV,   sigaction_handler},
      {SIGUSR2,   sigaction_handler},	{SIGINT,    sigaction_handler},
      {SIGURG,    sigaction_handler},	{SIGSTKFLT, sigaction_handler},
      {SIGTSTP,   sigaction_handler},	{SIGTTIN,   sigaction_handler},
      {SIGTTOU,   sigaction_handler},	{SIGILL,    sigaction_handler},
      {SIGXCPU,   sigaction_handler},	{SIGXFSZ,   sigaction_handler},
      {SIGVTALRM, sigaction_handler},	{SIGIO,     sigaction_handler},
      {SIGSYS,    sigaction_handler},
    };

  while(--nsig >= 0){
    for(i = 0; i<(sizeof(signals) / sizeof(*sig)); i++) {
      if(signals[i].signo == sig[nsig].signo) {
	signals[i].handler = sig[nsig].handler;
	break;
      }
    }
    if((sizeof(signals) / sizeof(*sig)) == i){
      LOG_ERR(" Unknown signo %d", sig[nsig].signo);
    }
  }
  // Set up signals to block, and signals to be caught
  //	CONFIG_CHANGE_SIGNAL - configuration change notification
  //	SIGPIPE - blocked so that TCP connection drops don't signal
  //	SIGCHLD	- caught when child exits (sysctl fork)

  for (i = 0; i < (sizeof(signals) / sizeof(signals[0])); i++) 
    {
      if((signals[i].handler == SIG_IGN) || (signals[i].handler == SIG_DFL) )
	sigAction.sa_flags = 0;
      else
	sigAction.sa_flags = SA_SIGINFO;

      if(signals[i].signo != SIGALRM || signals[i].signo != SIGRTMIN) 
	sigAction.sa_flags |= SA_RESTART;

      sigAction.sa_handler = signals[i].handler;
      sigfillset(&sigAction.sa_mask);
      sts = sigaction(signals[i].signo, &sigAction, (struct sigaction *) 0);
      if(sts != 0){
	LOG_ERR("sigaction(%d[%s]) failed, errno=%d(%m)", signals[i].signo, strsignal(signals[i].signo), errno);
      }
    }

  return 0;
}

static void do_write(int sock, const char *data, int size)
{
  int sts;
  
  do{
    sts = write(sock, data, size);
    if(sts < 0){
      if(errno == EINTR){ continue;}
      thread_nanosleep(0, 1000);
      //      fprintf(stderr, "write(%d) failed, errno=%d(%m)\n", sock, errno);
      //      return ;
    }
    data += sts;
    size -= sts;
  }while(size > 0);
  return;
}


static void logger_write_v(const char * prog, const char * tm, 
			   const char *level, const char *func, 
			   const char *file, int line, 
			   const char *fmt, va_list argptr)
{
  char	buff[1024]={'\0'};
  int		len;
  int		l;

  len = sprintf(buff, "%s %s [func:%s()][%s:%d][pid:%d] ", tm, level, func, file, line, getpid());
  l = vsnprintf(buff+len, sizeof(buff)-len-1, fmt, argptr);
  if(l < sizeof(buff)-len-1){
    len += l;
  }else{
    len = sizeof(buff)-1;
  }
  buff[len++] = '\n';
  do_write(1, buff, len);
  return;
}

static void log_write(const char *level, const char *func, const char *file, int line, const char *fmt, ...)
{
  va_list argptr;
  char	tmbuf[32];

  datetime_now(tmbuf);

  va_start(argptr, fmt);
  logger_write_v("L", tmbuf, level, func, file, line, fmt, argptr);
  va_end(argptr);
  return;
}
/**
 * @brief	Get current datetime.
 *
 * @param [OUT]res		A buffer to store datetime.(>= 32 bytes)
 */
static void datetime_now(char * res)
{
  time_t	t;
  struct tm 	tm;

  if(!res) return;

  time(&t);

  datetime_localtime_safe(t, -28800L, &tm);

  sprintf(res, "%04d-%02d-%02dT%02d:%02d:%02d", 
	  tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
	  tm.tm_hour, tm.tm_min, tm.tm_sec);
}
static void datetime_localtime_safe(time_t time, long timezone, struct tm *tm_time)
{
  static const int Days[] = 
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  unsigned  int n32_Pass4year;
  int n32_hpery;

  time=time-timezone;
    
  if(time < 0) {time = 0; }

  tm_time->tm_sec=(int)(time % 60);
  time /= 60;

  tm_time->tm_min=(int)(time % 60);
  time /= 60;

  n32_Pass4year=((unsigned int)time / (1461L * 24L));

  tm_time->tm_year=(n32_Pass4year << 2)+70;

  time %= 1461L * 24L;

  for (;;)    {
    n32_hpery = 365 * 24;
    if ((tm_time->tm_year & 3) == 0){
      n32_hpery += 24;
    }
    if (time < n32_hpery){
      break;
    }
    tm_time->tm_year++;
    time -= n32_hpery;
  }

  tm_time->tm_hour=(int)(time % 24);

  time /= 24;

  time++;

  if ((tm_time->tm_year & 3) == 0)    {
    if (time > 60)	{
      time--;
    }      else	{
      if (time == 60)	    {
	tm_time->tm_mon = 1;
	tm_time->tm_mday = 29;
	return ;
      }
    }
  }

  for (tm_time->tm_mon = 0;Days[tm_time->tm_mon] < time;tm_time->tm_mon++)    {
    time -= Days[tm_time->tm_mon];
  }

  tm_time->tm_mday = (int)(time);

  return;
}
static void safely_close(int fd)
{
  LOG_DEBUG("safely_close: fd=%d", fd);
  if(close(fd) < 0){
    LOG_ERR("close(%d).[error:%d(%m)]", fd, errno);
  }
  return;
}
