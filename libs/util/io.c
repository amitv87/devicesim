#include "io.h"
#include "time.h"

#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <inttypes.h>
#include <libunwind.h>

#ifdef __linux__
#include <pty.h>
#else
#include <util.h>
#endif

#ifdef __APPLE__
#include <IOKit/serial/ioss.h>
#endif

#ifndef MIN_LOOP_INTERVAL
#define MIN_LOOP_INTERVAL 5
#endif

#ifndef DEFAULT_LOOP_INTERVAL
#define DEFAULT_LOOP_INTERVAL 1000
#endif

// #define USE_POLL

#ifdef USE_POLL
#include <poll.h>
#define POLL_ERR_EVENTS (POLLERR | POLLNVAL | POLLHUP)
#else
struct pollfd{
  int fd;
  uint8_t events;
};

typedef struct{
  fd_set set;
  uint8_t mask;
} fd_set_info_t;

static fd_set_info_t fd_sets[] = {
  {.mask = FD_READ},
  {.mask = FD_WRITE},
  {.mask = FD_EXCEPT},
};
#endif

#define MAX_FD (32)

typedef struct{
  int maxfd;
  size_t count;
  struct pollfd pollfds[MAX_FD];
  io_handle_t* handles[MAX_FD];
} poll_info_t;
static poll_info_t poll_info = {0};

uint8_t io_rx_buff[4096];
static struct timeval tv;
static bool should_run = false;
static io_timer_t* timers = NULL;
static uint32_t loop_interval_ms = DEFAULT_LOOP_INTERVAL;

static int signals[] = {
  SIGINT,
  SIGSTOP,
};

void io_reg_handle(io_handle_t* handle){
  if(handle->fd > MAX_FD || poll_info.handles[handle->fd]) return;
  #ifdef USE_POLL
  short events = 0;
  if(handle->fd_mode_mask & FD_READ) events |= POLLIN;
  if(handle->fd_mode_mask & FD_WRITE) events |= POLLOUT;
  if(handle->fd_mode_mask & FD_EXCEPT) events |= POLL_ERR_EVENTS;
  #else
  uint8_t events = handle->fd_mode_mask;
  #endif
  poll_info.pollfds[poll_info.count++] = (struct pollfd){.fd = handle->fd, .events = events};
  poll_info.handles[handle->fd] = handle;
  if(poll_info.maxfd < handle->fd) poll_info.maxfd = handle->fd;
}

void io_dereg_handle(io_handle_t* handle){
  for(size_t i = 0; i < poll_info.count; i++){
    struct pollfd* p = &poll_info.pollfds[i];
    if(p->fd != handle->fd) continue;
    for(size_t j = i; j < poll_info.count; j++) poll_info.pollfds[j] = poll_info.pollfds[j+1];
    poll_info.count -= 1;
    poll_info.handles[handle->fd] = 0;
    if(poll_info.maxfd == handle->fd) poll_info.maxfd -= 1;
    break;
  }
}

#include <dlfcn.h>
#include <execinfo.h>

#define SIGNALS() REG_SIG(SEGV) REG_SIG(BUS) REG_SIG(ABRT) REG_SIG(ILL) REG_SIG(TRAP) \
  // REG_SIG(HUP) REG_SIG(INT) REG_SIG(TERM) REG_SIG(QUIT)

#define FOR_SIGS() for(int i = 0; i < sizeof(kDefaulSignals) / sizeof(kDefaulSignals[0]); i++)

static const int kDefaulSignals[] = {
  #define REG_SIG(x) SIG##x,
  SIGNALS()
};

static const char* kDefaulSignalsStr[] = {
  #undef REG_SIG
  #define REG_SIG(x) #x,
  SIGNALS()
};

static void bt(int sig){
  LOG();

  const char *event = "UNK";

  FOR_SIGS(){
    if(kDefaulSignals[i] == sig){
      event = kDefaulSignalsStr[i];
      break;
    }
  }

  void *stack[50] = {0};
  char buffer[128] = {0}, addr[128] = {0};
  int count = backtrace(stack, sizeof(stack)), cur = 0, skip = 1;

  for(int i = skip; i < count; i++){
    Dl_info info;
    snprintf(addr, sizeof(addr), "%p", stack[i]);
    if(dladdr(stack[i], &info) && info.dli_sname){
      printf("%d: %s [%s] %s\r\n", i, event, addr, info.dli_sname);
    }
  }

  exit(sig);
}

static void sig_handler(int signal){
  should_run = false;
  LOG("signal: %d", signal);
}

void io_init_loop(){
  FOR_SIGS() signal(kDefaulSignals[i], bt);
  for(int i = 0; i < countof(signals); i++) signal(signals[i], sig_handler);
}

void io_run_loop(){
  should_run = true;
  while(should_run){
    uint64_t elapsed_ms = io_step_loop(loop_interval_ms);
    // LOG("elapsed_ms: %llu", elapsed_ms);
  }
}

void io_stop_loop(){
  should_run = false;
}

int io_write(int fd, uint8_t* data, size_t length){
  int n = 0, written = 0, prevWritten = -1;
  while(written < length){
    n = write(fd, data + written, length - written);
    // printf("write: %d\r\n", n);
    if(n < 0){
      if(errno == EAGAIN || errno == EINTR) goto retry;
      LOG("write() failed (%s)", strerror(errno));
      break;
    }
    else if(n > 0) written += n;
    else{
      retry:
      if(prevWritten == written){
        // LOG("dropping %zu bytes...", length - written);
        break;
      }
      if(prevWritten == -1) prevWritten = 0;
      // LOG("sleeping, written: %d", written - prevWritten);
      prevWritten = written;
      usleep(2000);
    }
  }
  return written;
}

uint64_t io_step_loop(uint32_t timeout_ms){
  uint64_t prev_ts = uptime(), delay_ms = timeout_ms, now = 0, diff = 0;
  int rc = 0;
  while(true){
    if(poll_info.count){
      #ifdef USE_POLL
      rc = poll(poll_info.pollfds, poll_info.count, delay_ms);
      // LOG("poll -> %d", rc);
      for(size_t i = 0; rc > 0 && i < poll_info.count; i++){
        struct pollfd* p = &poll_info.pollfds[i];
        if(!p->revents) continue;
        io_handle_t* handle = poll_info.handles[p->fd];
        if(!handle) goto do_next;
        uint8_t fd_mode_mask = 0;
        if(p->revents & POLLIN) fd_mode_mask |= FD_READ;
        if(p->revents & POLLOUT) fd_mode_mask |= FD_WRITE;
        if(p->revents & POLL_ERR_EVENTS) fd_mode_mask |= FD_EXCEPT;

        if(fd_mode_mask){
          if(fd_mode_mask & handle->fd_mode_mask) handle->cb(handle, fd_mode_mask);
          do_next:
          rc -= 1;
        }
      }
      #else
      for(int i = 0; i < countof(fd_sets); i++) FD_ZERO(&fd_sets[i].set);

      for(int i = 0; i < poll_info.count; i++){
        struct pollfd* p = &poll_info.pollfds[i];
        for(int i = 0; i < countof(fd_sets); i++) if(p->events & fd_sets[i].mask) FD_SET(p->fd, &fd_sets[i].set);
      }

      tv.tv_sec = delay_ms / 1000;
      tv.tv_usec = (delay_ms % 1000) * 1000;
      rc = select(poll_info.maxfd + 1, &fd_sets[0].set, &fd_sets[1].set, &fd_sets[2].set, &tv);
      // LOG("select -> %d", rc);

      for(size_t i = 0; rc > 0 && i < poll_info.count; i++){
        struct pollfd* p = &poll_info.pollfds[i];
        io_handle_t* handle = poll_info.handles[p->fd];
        uint8_t fd_mode_mask = 0;
        for(int i = 0; i < countof(fd_sets) && rc; i++) if(FD_ISSET(p->fd, &fd_sets[i].set)) fd_mode_mask |= fd_sets[i].mask, rc -= 1;
        if(fd_mode_mask) handle->cb(handle, fd_mode_mask);
      }
      #endif
    }
    else if(delay_ms > 0) usleep(delay_ms * 1000);

    now = uptime();
    diff = now - prev_ts;

    if(rc < 0){
      LOG("poll/select failed with rc: %d, errno: %d -> %s", rc, errno, strerror(errno));
      break;
    }

    if(diff >= timeout_ms) break;
    else delay_ms = timeout_ms - diff;
  }

  io_timer_t* next = timers;
  while(next){
    io_timer_t* timer = next;
    next = (io_timer_t*)timer->item.next;
    if(now >= timer->last_ms + timer->interval_ms){
      timer->last_ms = now;
      bool skip = timer->skip_next;
      timer->skip_next = false;
      if(!skip) timer->cb(timer);
      if(!timer->repeat) io_rem_timer(timer);
    }
  }

  return diff;
}

static void reset_loop_timeout(){
  uint32_t min_interval_ms = DEFAULT_LOOP_INTERVAL;
  io_timer_t* timer = timers;
  while(timer){
    uint32_t other_interval_ms = min_interval_ms;
    if(min_interval_ms > timer->interval_ms) min_interval_ms = timer->interval_ms;
    else other_interval_ms = timer->interval_ms;

    min_interval_ms = (min_interval_ms / MIN_LOOP_INTERVAL);
    other_interval_ms = (other_interval_ms / MIN_LOOP_INTERVAL);

    uint32_t HCF = 1;
    for(uint32_t i = 1; i <= min_interval_ms; i++){
      if(min_interval_ms%i==0 && other_interval_ms%i==0){
        HCF = i;
      }
    }
    min_interval_ms = HCF * MIN_LOOP_INTERVAL;
    timer = (io_timer_t*)timer->item.next;
  }
  LOG("min_interval_ms: %d", min_interval_ms);
  loop_interval_ms = min_interval_ms;
}

void io_add_timer(io_timer_t* timer){
  uint32_t last_ms = uptime();
  if(timer->repeat && timer->run_now){
    timer->last_ms = last_ms;
    timer->cb(timer);
    if(!timer->repeat) return;
  }
  if(LIST_ADD(&timers, timer)){
    timer->last_ms = last_ms;
    reset_loop_timeout();
  }
}

void io_rem_timer(io_timer_t* timer){
  if(LIST_REM(&timers, timer)) reset_loop_timeout();
}

static speed_t convert_baudrate(uint32_t baud){
  switch (baud) {
    case 50: return B50;
    case 75: return B75;
    case 110: return B110;
    case 134: return B134;
    case 150: return B150;
    case 200: return B200;
    case 300: return B300;
    case 600: return B600;
    case 1200: return B1200;
    case 1800: return B1800;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    #ifdef __linux__
    case 460800: return B460800;
    case 500000: return B500000;
    case 576000: return B576000;
    case 921600: return B921600;
    case 1000000: return B1000000;
    case 1152000: return B1152000;
    case 1500000: return B1500000;
    case 2000000: return B2000000;
    case 2500000: return B2500000;
    case 3000000: return B3000000;
    case 3500000: return B3500000;
    case 4000000: return B4000000;
    #endif
    default: return 0;
  }
}

int io_tty_open(char* path, uint32_t baud_rate, char** slave_path){
  int fd = -1;
  if(strcmp(path, "/dev/ptmx") == 0){
    int slave_fd;
    int ret;
    CHK_ERR(openpty, &fd, &slave_fd, NULL, NULL, NULL);
    *slave_path = ptsname(fd);
    grantpt(fd);
    unlockpt(fd);
    chmod(*slave_path, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH);
  }
  else fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if(fd < 0) goto end;

  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

  speed_t baud = convert_baudrate(baud_rate);

  struct termios options;
  tcgetattr(fd, &options);

  cfmakeraw(&options);
  if(baud){
    cfsetispeed(&options, baud);
    cfsetospeed(&options, baud);
  }

  options.c_cflag |= (CLOCAL | CREAD);
  options.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
  options.c_cflag |= CS8;

  options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

  options.c_oflag &= ~OPOST;
  options.c_iflag &= ~(IXON | IXOFF | IXANY);
  options.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

  options.c_cc[VMIN]  = 0;

  tcsetattr(fd, TCSANOW | TCSAFLUSH, &options);

  if(!baud && baud_rate){
    #ifdef __APPLE__
    baud = baud_rate;
    int rc = ioctl(fd, IOSSIOSPEED, &baud);
    LOG("ioctl IOSSIOSPEED: %d", rc);
    #endif
  }

  int status;
  ioctl(fd, TIOCMGET, &status);
  status |= TIOCM_DTR;
  status |= TIOCM_RTS;
  ioctl(fd, TIOCMSET, &status);

  end:
  return fd;
}
