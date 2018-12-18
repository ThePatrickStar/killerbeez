#ifndef _WIN32
//Headers necessary for the forkserver
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#endif

#include "instrumentation.h"
#include <utils.h>


#ifndef _WIN32
//The forkserver is not supported on Windows

#include "forkserver_internal.h"

#define STRINGIFY_INTERNAL(x) #x
#define STRINGIFY(x) STRINGIFY_INTERNAL(x)
#define MSAN_ERROR 86

//The amount of time to wait before considering the fork server initialization failed
#define FORK_SERVER_STARTUP_TIME 10

//Save a fd to the /dev/null, so we don't have to keep opening/closing it
static int dev_null_fd =  -1;

//TODO implement memory limiting
static int mem_limit = 0;

//TODO asan detection
static int uses_asan = 0;

/**
 * This function locates the fork server library
 * @param buffer - A buffer to return the path to the fork server library
 * @param buffer_len - The length of the buffer parameter
 */
static void find_fork_server_library(char * buffer, size_t buffer_len)
{
#ifdef __APPLE__
  char * library_name = "libforkserver.dylib";
#else
  char * library_name = "libforkserver.so";
#endif

  char * directory = filename_relative_to_binary_dir(".");
  snprintf(buffer, buffer_len, "%s/%s", directory, library_name);
  if (!file_exists(buffer))
    FATAL_MSG("Failed to find the %s in %s.", library_name, directory);
}

//////////////////////////////////////////////////////////////
// Fork Server Initialization ////////////////////////////////
//////////////////////////////////////////////////////////////
/**
 *
 * @param needs_stdin_fd - whether we should open a library for the stdin of
 *                         the newly created process
 * @param target_path - The path to the program to start
 * @param argv - Arguments to pass to the program
 * @param fs - A forkserver_t structure to hold the fork server state, or NULL
 *             if not using a fork server
 * @param use_forkserver_library - Whether or not to use
 *                                 LD_PRELOAD/DYLD_INSERT_LIBRARIES to inject
 *                                 the fork server
 * @param st_pipe - pointer to an array of two status pipes for the fork server
 * @param ctl_pipe - pointer to an array of two control pipes for the fork server
 * @param persistence_max_cnt - if fork server is in use, and perssistent mode
 *                              is in use, this is the number of inputs which
 *                              will be handled by each execution of the target
 * @return the process ID of spawned process
 */
static pid_t run_target(int needs_stdin_fd, char *target_path, char **argv,
                forkserver_t * fs, int use_forkserver_library, int *st_pipe,
                int *ctl_pipe, int persistence_max_cnt) {
/*
  This function is based on the AFL run_target function present in afl-fuzz.c,
  available at this URL:
  https://github.com/mirrorer/afl/blob/master/afl-fuzz.c#L1968.
  AFL's license is as shown below:

  american fuzzy lop - fuzzer code
  --------------------------------
  Written and maintained by Michal Zalewski <lcamtuf@google.com>

  Forkserver design by Jann Horn <jannhorn@googlemail.com>

  Copyright 2013, 2014, 2015, 2016, 2017 Google Inc. All rights reserved.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at:

    http://www.apache.org/licenses/LICENSE-2.0
 */
  int child_pid;

  DEBUG_MSG("Forking child process for target executable...");
  child_pid = fork();
  if(child_pid < 0) FATAL_MSG("fork() failed");

  if(!child_pid) {
    struct rlimit r;

    // Umpf. On OpenBSD, the default fd limit for root users is set to
    // soft 128. Let's try to fix that...
    if (!getrlimit(RLIMIT_NOFILE, &r) && r.rlim_cur < MAX_FORKSRV_FD) {
      r.rlim_cur = MAX_FORKSRV_FD;
      setrlimit(RLIMIT_NOFILE, &r); // Ignore errors
    }

    if (mem_limit) {
      r.rlim_max = r.rlim_cur = ((rlim_t)mem_limit) << 20;
#ifdef RLIMIT_AS
      setrlimit(RLIMIT_AS, &r); // Ignore errors
#else
      // This takes care of OpenBSD, which doesn't have RLIMIT_AS, but
      // according to reliable sources, RLIMIT_DATA covers anonymous
      // maps - so we should be getting good protection against OOM bugs.
      setrlimit(RLIMIT_DATA, &r); // Ignore errors
#endif // ^RLIMIT_AS
    }

    // Dumping cores is slow and can lead to anomalies if SIGKILL is delivered
    // before the dump is complete.

    r.rlim_max = r.rlim_cur = 0;

    setrlimit(RLIMIT_CORE, &r); // Ignore errors

    /* Isolate the process and configure standard descriptors. If out_file is
         specified, stdin is /dev/null; otherwise, out_fd is cloned instead. */
    setsid();

    if(dev_null_fd < 0)
      dev_null_fd = open("/dev/null", O_RDWR);
    if(needs_stdin_fd) {
      dup2(fs->target_stdin, 0);
      close(fs->target_stdin);
    } else {
      dup2(dev_null_fd, 0);
    }
    if(dup2(dev_null_fd, 1) < 0)
      WARNING_MSG("Sending stdout to /dev/null failed! errno=%d", errno);
    if(dup2(dev_null_fd, 2) < 0)
      WARNING_MSG("Sending stderr to /dev/null failed! errno=%d", errno);

    // The forkserver requires setting up some control pipes for interaction
    // between the fuzzer and forkserver (which lives in the target process)
    if(fs) {
      // Set up control and status pipes, close the unneeded original fds.
      if(dup2(ctl_pipe[0], FUZZER_TO_FORKSRV) < 0)
        FATAL_MSG("dup2() failed");
      if(dup2(st_pipe[1], FORKSRV_TO_FUZZER) < 0)
        FATAL_MSG("dup2() failed");

      close(ctl_pipe[0]);
      close(ctl_pipe[1]);
      close(st_pipe[0]);
      close(st_pipe[1]);
    }

    /* On Linux, would be faster to use O_CLOEXEC. Maybe TODO. */
    close(dev_null_fd);

    // If we are using a forksrv, we might need to inject it dynamically if it
    // is not already in the executable.  We also want to make sure we set the
    // environment variable which is used for persistence mode, and finally we
    // add the optimization to load all the libraries once so this is only
    // done on the execv, as opposed to each time the target process calls fork
    if(fs) {
      // Preload the forkserver library
      if(use_forkserver_library) {
        char fork_server_library_path[MAX_PATH];
        find_fork_server_library(fork_server_library_path, sizeof(fork_server_library_path));
  #ifdef __APPLE__
        setenv("DYLD_INSERT_LIBRARIES", fork_server_library_path, 1);
  #else
        setenv("LD_PRELOAD", fork_server_library_path, 1);
  #endif
      }

      if(persistence_max_cnt) {
        char buffer[16];
        snprintf(buffer, sizeof(buffer),"%d",persistence_max_cnt);
        setenv(PERSIST_MAX_VAR, buffer, 1);
      }

      // This should improve performance a bit, since it stops the linker from
      // doing extra work post-fork().
      if (!getenv("LD_BIND_LAZY")) setenv("LD_BIND_NOW", "1", 0);
    }

    // Set sane defaults for ASAN if nothing else specified.
    setenv("ASAN_OPTIONS", "abort_on_error=1:"
                           "detect_leaks=0:"
                           "symbolize=0:"
                           "allocator_may_return_null=1", 0);

    // MSAN uses slightly different arguments when using the forkserver
    if(fs) {
      // MSAN is tricky, because it doesn't support abort_on_error=1 at this
      // point. So, we do this in a very hacky way.
      setenv("MSAN_OPTIONS", "exit_code=" STRINGIFY(MSAN_ERROR) ":"
                             "symbolize=0:"
                             "abort_on_error=1:"
                             "allocator_may_return_null=1:"
                             "msan_track_origins=0", 0);
    } else {
      setenv("MSAN_OPTIONS", "exit_code=" STRINGIFY(MSAN_ERROR) ":"
                             "symbolize=0:"
                             "msan_track_origins=0", 0);
    }

    execv(target_path, argv);
    // The only time execv() returns is if it failed
    FATAL_MSG("Target executable failed to execute (execv())");
    exit(1);
  }

  return child_pid;
}

/**
 * This function starts a program with the fork server embedded in it
 * @param fs - A forkserver_t structure to hold the fork server state
 * @param target_path - The path to the program to start
 * @param argv - Arguments to pass to the program
 * @param use_forkserver_library - Whether or not to use LD_PRELOAD/DYLD_INSERT_LIBRARIES to inject the fork server
 * library or not
 * @param persistence_max_cnt - the maximum number of fuzz iterations a persistence mode process should run
 * @param needs_stdin_fd - whether we should open a library for the stdin of the newly created process
 */
void fork_server_init(forkserver_t * fs, char * target_path, char ** argv, int use_forkserver_library,
  int persistence_max_cnt, int needs_stdin_fd)
{
  static struct itimerval it;
  int st_pipe[2], ctl_pipe[2];
  int err, status, forksrv_pid;
  int rlen = -1, timed_out = 1;
  char stdin_filename[100];
  time_t start_time;

  if(dev_null_fd < 0) {
    dev_null_fd = open("/dev/null", O_RDWR);
    if (dev_null_fd < 0)
      FATAL_MSG("Unable to open /dev/null");
  }

  fs->sent_get_status = 0;
  fs->last_status = -1;

  if(needs_stdin_fd) {
    strncpy(stdin_filename, "/tmp/fuzzfileXXXXXX", sizeof(stdin_filename));
    fs->target_stdin = mkstemp(stdin_filename);
    if(fs->target_stdin < 0)
      FATAL_MSG("Couldn't make temp file\n");
  }
  else
    fs->target_stdin = -1;

  DEBUG_MSG("Spinning up the fork server...");

/*
  The code in the rest of this function is based on the AFL startup fork server
  present in afl-fuzz.c, available at this URL:
  https://github.com/mirrorer/afl/blob/master/afl-fuzz.c#L1968.
  AFL's license is as shown below:

  american fuzzy lop - fuzzer code
  --------------------------------
  Written and maintained by Michal Zalewski <lcamtuf@google.com>

  Forkserver design by Jann Horn <jannhorn@googlemail.com>

  Copyright 2013, 2014, 2015, 2016, 2017 Google Inc. All rights reserved.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at:

    http://www.apache.org/licenses/LICENSE-2.0
 */
  if(pipe(st_pipe) || pipe(ctl_pipe))
    FATAL_MSG("pipe() failed");

  forksrv_pid = run_target(needs_stdin_fd, target_path, argv, fs, use_forkserver_library,
             st_pipe, ctl_pipe, persistence_max_cnt);

  // Close the unneeded endpoints.
  close(ctl_pipe[0]);
  close(st_pipe[1]);

  fs->fuzzer_to_forksrv = ctl_pipe[1];
  fs->forksrv_to_fuzzer = st_pipe[0];
  fs->pid = forksrv_pid;

  // Wait for the fork server to come up, but don't wait too long.
  // Note, we do this looping, rather than blocking on read and using
  // a SIGALRM to breakout on time out, because we want to avoid globals
  // so the code can be used without worrying about any existing signal handlers
  start_time = time(NULL);
  while(time(NULL) - start_time < FORK_SERVER_STARTUP_TIME) {
    err = ioctl(fs->forksrv_to_fuzzer, FIONREAD, &rlen);
    if(!err && rlen == sizeof(int)) {
      rlen = read(fs->forksrv_to_fuzzer, &status, sizeof(status));
      timed_out = 0;
      break;
    }
    usleep(5);
  }

  // If we have a four-byte "hello" message from the server, we're all set.
  // Otherwise, try to figure out what went wrong.
  if (rlen == 4) {
    DEBUG_MSG("All right - fork server (PID %d) is up.", forksrv_pid);
    return;
  }

  kill(forksrv_pid, SIGKILL);
  if(timed_out)
    FATAL_MSG("Timeout while initializing fork server\n");

  if (waitpid(forksrv_pid, &status, 0) <= 0)
    FATAL_MSG("waitpid() failed");

  if (WIFSIGNALED(status)) {

    if (mem_limit && mem_limit < 500 && uses_asan) {

      ERROR_MSG(
           "Whoops, the target binary crashed suddenly, before receiving any input\n"
           "    from the fuzzer! Since it seems to be built with ASAN and you have a\n"
           "    restrictive memory limit configured, this is expected");

    } else if (!mem_limit) {

      ERROR_MSG(
           "Whoops, the target binary crashed suddenly, before receiving any input\n"
           "    from the fuzzer! There are several probable explanations:\n\n"

           "    - The binary is just buggy and explodes entirely on its own. If so, you\n"
           "      need to fix the underlying problem or find a better replacement.\n\n"

#ifdef __APPLE__

           "    - On MacOS X, the semantics of fork() syscalls are non-standard and may\n"
           "      break afl-fuzz performance optimizations when running platform-specific\n"
           "      targets. To fix this, try running without the forkserver.\n\n"

#endif // __APPLE__

           "    - Less likely, there is a horrible bug in the fuzzer.");

    } else {

      ERROR_MSG(
           "Whoops, the target binary crashed suddenly, before receiving any input\n"
           "    from the fuzzer! There are several probable explanations:\n\n"

           "    - The current memory limit (%s) is too restrictive, causing the\n"
           "      target to hit an OOM condition in the dynamic linker. Try bumping up\n"
           "      the limit with the -m setting in the command line. A simple way confirm\n"
           "      this diagnosis would be:\n\n"

#ifdef RLIMIT_AS
           "      ( ulimit -Sv $[%llu << 10]; /path/to/fuzzed_app )\n\n"
#else
           "      ( ulimit -Sd $[%llu << 10]; /path/to/fuzzed_app )\n\n"
#endif // ^RLIMIT_AS

           "      Tip: you can use http://jwilk.net/software/recidivm to quickly\n"
           "      estimate the required amount of virtual memory for the binary.\n\n"

           "    - The binary is just buggy and explodes entirely on its own. If so, you\n"
           "      need to fix the underlying problem or find a better replacement.\n\n"

#ifdef __APPLE__

           "    - On MacOS X, the semantics of fork() syscalls are non-standard and may\n"
           "      break afl-fuzz performance optimizations when running platform-specific\n"
           "      targets. To fix this, try running without the forkserver.\n\n"

#endif // __APPLE__

           "    - Less likely, there is a horrible bug in the fuzzer.",
           mem_limit << 20, mem_limit - 1);

    }

    FATAL_MSG("Fork server crashed with signal %d", WTERMSIG(status));

  }

  if (mem_limit && mem_limit < 500 && uses_asan) {

    ERROR_MSG(
           "Hmm, looks like the target binary terminated before we could complete a\n"
           "    handshake with the injected code. Since it seems to be built with ASAN and\n"
           "    you have a restrictive memory limit configured, this is expected.");

  } else if (!mem_limit) {

    ERROR_MSG(
         "Hmm, looks like the target binary terminated before we could complete a\n"
         "    handshake with the injected code. Perhaps there is a horrible bug in the\n"
         "    fuzzer.");

  } else {

    ERROR_MSG(
         "Hmm, looks like the target binary terminated before we could complete a\n"
         "    handshake with the injected code. There are a few probable explanations:\n\n"

         "    - The current memory limit (%s) is too restrictive, causing an OOM\n"
         "      fault in the dynamic linker. This can be fixed with the -m option. A\n"
         "      simple way to confirm the diagnosis may be:\n\n"

#ifdef RLIMIT_AS
         "      ( ulimit -Sv $[%llu << 10]; /path/to/fuzzed_app )\n\n"
#else
         "      ( ulimit -Sd $[%llu << 10]; /path/to/fuzzed_app )\n\n"
#endif // ^RLIMIT_AS

         "      Tip: you can use http://jwilk.net/software/recidivm to quickly\n"
         "      estimate the required amount of virtual memory for the binary.\n\n"

         "    - Less likely, there is a horrible bug in the fuzzer. If other options\n"
         "      fail.",
         mem_limit << 20, mem_limit - 1);

  }

  FATAL_MSG("Fork server handshake failed");
}

//////////////////////////////////////////////////////////////
// Fork Server Communication Functions ///////////////////////
//////////////////////////////////////////////////////////////

/**
 * This function sends a command to the fork server
 * @param fs - A forkserver_t structure to hold the fork server state
 * @param command - the command to send
 * @return - 0 on success, FORKSERVER_ERROR on failure
 */
static int send_command(forkserver_t * fs, char command)
{
  if (write(fs->fuzzer_to_forksrv, &command, sizeof(command)) != sizeof(command))
    return FORKSERVER_ERROR;
  return 0;
}

/**
 * This function reads a response from the fork server
 * @param fs - A forkserver_t structure to hold the fork server state
 * @return - the response value on success, FORKSERVER_ERROR on failure
 */
static int read_response(forkserver_t * fs)
{
  int response;
  if (read(fs->forksrv_to_fuzzer, &response, sizeof(response)) != sizeof(response))
    return FORKSERVER_ERROR;
  return response;
}

/**
 * This function tells the forkserver to exit, and closes any open file descriptors to it
 * @param fs - A forkserver_t structure to hold the fork server state
 * @return - the 0 on success, FORKSERVER_ERROR on failure
 */
int fork_server_exit(forkserver_t * fs)
{
  int ret = send_command(fs, EXIT);
  if(!ret) {
    close(fs->fuzzer_to_forksrv);
    close(fs->forksrv_to_fuzzer);
    close(fs->target_stdin);
  }
  return ret;
}

/**
 * This function tells the forkserver to fork or fork and run, and returns the newly created process's pid
 * @param fs - A forkserver_t structure to hold the fork server state
 * @param command - Either the FORK or FORK_RUN command
 * @return - the newly created process's pid on success, FORKSERVER_ERROR on failure
 */
static int send_fork(forkserver_t * fs, char command)
{
  if(send_command(fs, command))
    return FORKSERVER_ERROR;
  fs->sent_get_status = 0;
  return read_response(fs); //Wait for the target pid
}

/**
 * This function tells the forkserver to fork, and returns the newly created process's pid
 * @param fs - A forkserver_t structure to hold the fork server state
 * @return - the newly created process's pid on success, FORKSERVER_ERROR on failure
 */
int fork_server_fork(forkserver_t * fs)
{
  return send_fork(fs, FORK);
}

/**
 * This function tells the forkserver to fork and run, and returns the newly created process's pid
 * @param fs - A forkserver_t structure to hold the fork server state
 * @return - the newly created process's pid on success, FORKSERVER_ERROR on failure
 */
int fork_server_fork_run(forkserver_t * fs)
{
  return send_fork(fs, FORK_RUN);
}

/**
 * This function tells the forkserver to run
 * @param fs - A forkserver_t structure to hold the fork server state
 * @return - 0 on success, FORKSERVER_ERROR on failure
 */
int fork_server_run(forkserver_t * fs)
{
  if(send_command(fs, RUN))
    return FORKSERVER_ERROR;
  if(read_response(fs) != 0)
    return FORKSERVER_ERROR;
  return 0;
}

/**
 * This function gets the response of prevously sent GET_STATUS command from the fork server (i.e. the process's exit status)
 * @param fs - A forkserver_t structure to hold the fork server state
 * @param wait - whether this function should block or not
 * @return - the finished process's exit status (see waitpid) on success, FORKSERVER_ERROR on failure, or
 * FORKSERVER_NO_RESULTS_READY when not blocking and the forkserver has not responded yet
 */
int fork_server_get_pending_status(forkserver_t * fs, int wait)
{
  unsigned long bytes_available = 0;
  int err;

  if(fs->sent_get_status && fs->last_status != -1)
    return fs->last_status;

  if(wait)
    return read_response(fs); //Wait for the target's exit status
  else {
    err = ioctl(fs->forksrv_to_fuzzer, FIONREAD, &bytes_available);
    if(!err && bytes_available == sizeof(int)) {
      fs->last_status = read_response(fs); //Wait for the target's exit status
      return fs->last_status;
    }
  }
  return FORKSERVER_NO_RESULTS_READY;
}

/**
 * This function sends a GET_STATUS command to the fork server and gets the response (i.e. the process's exit status)
 * @param fs - A forkserver_t structure to hold the fork server state
 * @param wait - whether this function should block or not
 * @return - the finished process's exit status (see waitpid) on success, FORKSERVER_ERROR on failure, or
 * FORKSERVER_NO_RESULTS_READY when not blocking and the forkserver has not responded yet
 */
int fork_server_get_status(forkserver_t * fs, int wait)
{
  if(!fs->sent_get_status) {
    if(send_command(fs, GET_STATUS))
      return FORKSERVER_ERROR;
    fs->sent_get_status = 1;
    fs->last_status = -1;
  }
  return fork_server_get_pending_status(fs, wait);
}

#endif //!_WIN32
