#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/signalfd.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

int fd_mem_stdout;
int fd_mem_stderr;

int fd_signal;

#define CHECK_ASSIGN(assign_var, stmt) do { \
    assign_var = stmt;                          \
    if (assign_var == -1) {                     \
      perror("Failed on " #stmt "");             \
      return errno;                             \
    }                                           \
  } while(0)

#define CHECK_RET(stmt) do {                    \
    int ret = stmt;                             \
    if (ret == -1) {                           \
      perror("Failed on " #stmt "");            \
      return errno;                            \
    }                                          \
  } while (0)

#define CHECK(stmt) do {                         \
    int ret = stmt;                              \
    if (ret == -1) {                            \
      perror("Failed on " #stmt "");             \
      exit(1);                                  \
      return;                                   \
    }                                           \
  } while (0)


int memfd_create(const char *name, unsigned int flags) {
  return syscall(SYS_memfd_create,
                 name,
                 flags);
}

void dump_memfd(int dst, int src) {
  ssize_t amount_read;
  char buf[1 << 10];
  char *buff = buf;
  lseek(src, 0, SEEK_SET);
  while((amount_read = read(src, buff, 1 << 10)) > 0) {
    ssize_t ret;
    while((ret = write(dst, buff, amount_read)) > 0) {
      buff = buff + ret;
      amount_read = amount_read - ret;
    }
  }
  ftruncate(src, 0);
}

int
main(int argc, char **argv) {
  // we start attached
  bool attached = true;
  char sigbuf_name[100];
  sprintf(sigbuf_name, "sigbuffer.stdout.%d", getpid());
  fd_mem_stdout = memfd_create(sigbuf_name, 0);

  sprintf(sigbuf_name, "sigbuffer.stderr.%d", getpid());
  fd_mem_stderr = memfd_create(sigbuf_name, 0);

  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  sigaddset(&mask, SIGUSR2);
  sigaddset(&mask, SIGCHLD);

  CHECK_RET(sigprocmask(SIG_BLOCK, &mask, NULL));

  fd_signal = signalfd(-1, &mask, 0);

  int cerr[2];
  int cout[2];
  //  int cin[2];

  pipe(cerr);
  pipe(cout);
  //  pipe(cin);

  pid_t cpid = fork();
  if (cpid ==  0) {
    //    close(cin[1]);
    close(cout[0]);
    close(cerr[0]);

    // we are the child!
    dup2(cout[1], STDOUT_FILENO);
    dup2(cerr[1], STDERR_FILENO);

    execlp("/usr/bin/rc", "rc", "-l", 0);
    return 0;
  }

  // close out the sides we're not using
  close(cout[1]);
  close(cerr[1]);

  // needs to be 3 for signals eventually
  int nfds = 3;
  struct pollfd pollfds[nfds];

  pollfds[0].fd = cout[0];
  pollfds[0].events = POLLIN;
  pollfds[1].fd = cerr[0];
  pollfds[1].events = POLLIN;
  pollfds[2].fd = fd_signal;
  pollfds[2].events = POLLIN;

  for (;;) {
    int num_evs = poll(pollfds, nfds, -1);
    if (num_evs == 0) {
      int status;
      wait(&status);
      close(cout[0]);
      close(cerr[0]);
      return status;
    }
    for (int i = 0; i < nfds; i++) {
      if (pollfds[i].revents == 0) {
        continue;
      }
      if (!(pollfds[i].revents & POLLIN)) {
        continue;
      }

      if (pollfds[i].fd == fd_signal) {
        struct signalfd_siginfo si;
        int ret = read(fd_signal, &si, sizeof(si));
        if (ret != sizeof(si)) {
          printf("So bad, cannot read whole signal");
          return 1;
        }

        if (si.ssi_signo == SIGUSR1) {
          // force into attached mode
          attached = true;
          // dump memfd to stdout
          // ftruncate memfd
          dump_memfd(STDOUT_FILENO, fd_mem_stdout);
          dump_memfd(STDOUT_FILENO, fd_mem_stderr);
        }
        if (si.ssi_signo == SIGUSR2) {
          // force into detached mode
          attached = false;
        }
        if (si.ssi_signo == SIGCHLD) {
          printf("Received SIGCHLD");
          int status;
          wait(&status);
          close(cout[0]);
          close(cerr[0]);
          return status;
        }

        continue;
      }

      ssize_t amount_read;
      char buf[1 << 10];
      char *buff = buf;
      amount_read = read(pollfds[i].fd, buff, 1 << 10);
      if (amount_read > 0) {
        int towrite_fd;
        if (pollfds[i].fd == cout[0]) {
          towrite_fd = attached ? STDOUT_FILENO : fd_mem_stdout;
        }
        if (pollfds[i].fd == cerr[0]) {
          towrite_fd = attached ? STDERR_FILENO : fd_mem_stderr;
        }

        ssize_t ret;
        while((ret = write(towrite_fd, buff, amount_read)) > 0) {
          buff = buff + ret;
          amount_read = amount_read - ret;
        }
      }
    }
  }
}
