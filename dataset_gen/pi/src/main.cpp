// © 2024 Alec Fessler
// MIT License
// See LICENSE file in the project root for full license information.

#include <atomic>
#include <arpa/inet.h>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <sstream>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "camera_handler.h"
#include "connection.h"
#include "logger.h"
#include "videnc.h"

struct sem_deleter {
  void operator()(sem_t* sem) const {
    if (sem) sem_destroy(sem);
  }
};

extern char** environ;
volatile static sig_atomic_t running = 0;
constexpr int64_t ns_per_s = 1'000'000'000;
static int64_t timestamp = 0;
static timer_t timerid;
static std::unique_ptr<sem_t, sem_deleter> loop_ctl_sem;
static std::unique_ptr<camera_handler_t> cam;
static std::unique_ptr<connection> conn;
std::unique_ptr<logger_t> logger;

void capture_signal_handler(int signo, siginfo_t* info, void* context);
void io_signal_handler(int signo, siginfo_t* info, void* context);
void exit_signal_handler(int signo, siginfo_t* info, void* context);

inline int init_realtime_scheduling(int recording_cpu);
inline int init_timer();
inline int init_signals();
inline int init_sigio(int fd);
inline void arm_timer();
inline int stream_pkt(connection& conn, const uint8_t* data, size_t size);

int main() {
  try {
    config config = parse_config("config.txt");
    logger = std::make_unique<logger_t>("logs.txt");

    sem_t sem;
    loop_ctl_sem = std::unique_ptr<sem_t, sem_deleter>(&sem);
    if (sem_init(loop_ctl_sem.get(), 0, 0) < 0) {
      logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, "Failed to initialize semaphore");
      return -errno;
    }

    int64_t frame_duration = ns_per_s / config.fps;
    lock_free_queue_t frame_queue(config.dma_buffers);
    cam = std::make_unique<camera_handler_t>(
      config,
      frame_queue,
      *loop_ctl_sem.get()
    );
    videnc encoder(config);
    conn = std::make_unique<connection>(
      config.server_ip,
      config.tcp_port,
      config.udp_port
    );

    int ret;
    if ((ret = init_realtime_scheduling(config.recording_cpu)) < 0) return ret;
    if ((ret = init_timer()) < 0) return ret;
    if ((ret = init_signals()) < 0) return ret;

    running = 1;
    while (running) {
      // (re)connect to udp socket
      // and bind fd for SIGIO on incoming data
      if (conn->udpfd < 0) {
        if ((ret = conn->bind_udp()) < 0) return ret;
        if ((ret = init_sigio(conn->udpfd)) < 0) return ret;
      }

      // if timestamp is nonzero, get next frame capture time
      if (timestamp) timestamp += frame_duration;
      // arm timer completes only if timestamp is nonzero
      arm_timer();
      // block until either:
      // 1. frame is ready to encode
      // 2. initial timestamp is received
      // 3. exit signal is received (not "STOP" message)
      sem_wait(loop_ctl_sem.get());

      void* frame = frame_queue.dequeue();
      // continue if no frame is available (cases 2 and 3 above)
      if (!frame) continue;
      encoder.encode_frame(
        (uint8_t*)frame,
        stream_pkt,
        *conn
      );
    }
  } catch (const std::exception& e) {
    if (logger)
      logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, e.what());
    else
      std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
  }

  return 0;
}

void capture_signal_handler(int signo, siginfo_t* info, void* context) {
  /**
   * Signal handler for queueing a capture request to the camera
   *
   * This is called when the timer (see init_timer(), arm_timer())
   * hits the next capture timestamp and emits SIGUSR1.
   * The next timestamp is timestamp + ns_per_s / fps
   *
   * When the capture request is completed, the camera will emit a signal
   * which is handled by enqueuing a ptr to the filled dma buffer, and
   * incrementing the loop_ctl_sem to unblock the main loop so it can
   * handle the captured frame (see camera_handler::request_complete(request)).
   */
  (void)info;
  (void)context;
  if (signo != SIGUSR1 || !running) return;

  cam->queue_request();
  logger->log(logger_t::level_t::INFO, __FILE__, __LINE__, "Capture request queued");
}

void io_signal_handler(int signo, siginfo_t* info, void* context) {
  /**
   * Signal handler for handling messages from udp socket
   *
   * There are two types of messages:
   * 1. 8 byte int - number of nanoseconds since unix epoch
   * 2. 4 byte string - "STOP" signal
   *
   * The first message type is our starting timestamp, and all subsequent
   * frame captures will be on multiples of the frame duration added to
   * this initial timestamp value. It is assigned to the static timestamp
   * variable, and then we increment the semaphore without a frame to enable
   * the main loop to arm the timer before waiting for a frame (see arm_timer()).
   * This drives recording and streaming, as the timer will be rearmed every
   * time the loop resets, until the timestamp is set to zero again.
   *
   * The second message type is our stop signal, and is handled by resetting the
   * timestamp to zero, preventing any more arm_timer() calls from completing,
   * but still allowing the main loop to handle any remaining frames since the
   * semaphore is incremented once per available frame.
   */
  (void)info;
  (void)context;
  if (signo != SIGIO) return;

  char buf[8];
  ssize_t bytes_recvd = recvfrom(
    conn->udpfd,
    buf,
    sizeof(buf),
    0,
    NULL,
    NULL
  );

  if (bytes_recvd == 4 && strncmp(buf, "STOP", 4) == 0) {
    logger->log(logger_t::level_t::INFO, __FILE__, __LINE__, "Received STOP signal, shutting down...");
    timestamp = 0;
    return;
  }

  if (bytes_recvd == 8) {
    memcpy(&timestamp, buf, sizeof(timestamp));
    logger->log(logger_t::level_t::INFO, __FILE__, __LINE__, "Received timestamp");
    sem_post(loop_ctl_sem.get());
    return;
  }

  logger->log(logger_t::level_t::DEBUG, __FILE__, __LINE__, "Received unexpected message size");
}

void exit_signal_handler(int signo, siginfo_t* info, void* context) {
  /**
   * Signal handler for allowing the process to gracefully exit
   *
   * Handled by setting running to 0, and then incrementing the
   * semaphore without a frame so that the main loop can unblock
   * and then exit since the loop condition is false.
   */
  (void)info;
  (void)context;
  if (signo != SIGINT && signo != SIGTERM) return;

  running = 0;
  sem_post(loop_ctl_sem.get());
}

inline int init_realtime_scheduling(int recording_cpu) {
  /**
   * Initializes realtime scheduling for the process
   *
   * Two properties are set:
   *
   * 1. Process is pinned to a specific cpu core to prevent overhead of
   *    the scheduler potentially moving us to a different core.
   *
   * 2. FIFO scheduling with max priority, so any process of equal
   *    priority must wait until this one is blocking on the semaphore
   *    before it can be scheduled on our core. Additionally, any process
   *    of lower priority than max will be preempted as soon as we have
   *    a signal to handle or the semaphore is unblocked.
   */
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(recording_cpu, &cpuset);
  if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0) {
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, "Failed to set CPU affinity");
    return -errno;
  }

  struct sched_param param;
  param.sched_priority = sched_get_priority_max(SCHED_FIFO);
  if (sched_setscheduler(0, SCHED_FIFO, &param) < 0) {
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, "Failed to set real-time scheduling policy");
    return -errno;
  }
  return 0;
}


inline int init_timer() {
  /**
   * Initializes a timer reading from the system clock (CLOCK_REALTIME)
   *
   * This is the timer which arm_timer() makes use of to emit SIGUSR1
   * to queue a capture request (see capture_signal_handler()). It is
   * using the system clock instead of the monotonic clock because frame
   * capture synchronization between multiple cameras in the overall system
   * is achieved through PTP clock sync across the network, and then each pi
   * capturing frames every time their clock reaches the next capture timestamp.
   */
  struct sigevent sev;
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_signo = SIGUSR1;
  sev.sigev_value.sival_ptr = &timerid;

  if (timer_create(CLOCK_REALTIME, &sev, &timerid) == -1) {
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, "Failed to create socket disconnect timer");
    return -errno;
  }
  return 0;
}

inline void arm_timer() {
  /**
   * Arms the timer which emits SIGUSR1 on timeout
   *
   * If the timestamp has not been set (see io_signal_handler())
   * it returns early to not begin the capture/stream loop.
   *
   * If the timestamp has been set, it simply sets the timer
   * to timeout when it reaches the timestamp value. The timestamp
   * is incremented (if it's set) by the frame duration in main()
   * just before calling this function, ensuring the next frame capture
   * request is enqueued right on time. Because the main loop increments
   * and rearms the timer on every iteration of the loop, this effectively
   * drives the capture/stream loop forward until the timestamp is reset to
   * zero. Combined with PTP clock sync across the network, and all devices
   * receiving the same initial timestamp, this enables tight frame capture
   * sync across all cameras in the network.
   */
  if (!timestamp) return;

  struct itimerspec its;
  its.it_value.tv_sec = timestamp / ns_per_s;
  its.it_value.tv_nsec = timestamp % ns_per_s;
  its.it_interval.tv_sec = 0;
  its.it_interval.tv_nsec = 0;

  timer_settime(timerid, 0, &its, NULL);
}

inline int init_sigio(int fd) {
  /**
   * Binds the udp socket fd to emit SIGIO upon incoming data
   *
   * This enables us to block on the semaphore in the main loop
   * before the initial timestamp comes in. When it does, the main
   * loop will be preempted, the timestamp set, and the semaphore
   * incremented so the main loop can unblock and arm the timer
   * (see io_signal_handler(), arm_timer()).
   *
   * This function also enables us to avoid polling for the "STOP"
   * message since the SIGIO will be emitted upon receiving any
   * data on the port assigned to the udp file descriptor.
   */
  if (fcntl(fd, F_SETFL, O_NONBLOCK | O_ASYNC) < 0) {
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, "Failed to set O_NONBLOCK | O_ASYNC on UDP socket");
    return -errno;
  }

  if (fcntl(fd, F_SETOWN, getpid()) < 0) {
    logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, "Failed to set owner process for SIGIO");
    return -errno;
  }

  logger->log(logger_t::level_t::INFO, __FILE__, __LINE__, "Signal-driven I/O enabled");
  return 0;
}

inline int init_signals() {
  /**
   * Sets the sa_mask for the process
   *
   * There are 4 signals handled:
   *
   * SIGUSR1 - emitted when the timer (see init_timer(), arm_timer())
   *           reaches the assigned timestamp, handled by enqueueing
   *           a capture request with the camera
   *
   * SIGIO   - emitted whenever data is received on the udp port
   *           (see connection::bind_udp(), io_signal_handler()).
   *           If the data is 8 bytes, it's our starting timestamp,
   *           if it's 4 bytes, it's our "STOP" message, otherwise
   *           it's unexpected and is a server side bug.
   *
   * SIGINT  - emitted by the os to signal for exit
   *
   * SIGTERM - emitted by the os to signal for exit
   *
   * SA_RESTART flag is set, which means any interrupted syscalls
   * will be retried.
   */
  struct sigaction action;
  action.sa_sigaction = capture_signal_handler;
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&action.sa_mask);

  struct sigaction io_action;
  io_action.sa_sigaction = io_signal_handler;
  io_action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&io_action.sa_mask);

  struct sigaction exit_action;
  io_action.sa_sigaction = exit_signal_handler;
  io_action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&io_action.sa_mask);

  if (sigaction(SIGUSR1, &action, NULL) < 0 ||
      sigaction(SIGIO, &io_action, NULL) < 0 ||
      sigaction(SIGINT, &exit_action, NULL) < 0 ||
      sigaction(SIGTERM, &exit_action, NULL) < 0) {
      logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, "Failed to set signal handlers");
      return -errno;
    }

  return 0;
}

inline int stream_pkt(connection& conn, const uint8_t* data, size_t size) {
  /**
   * Streams encoded video frames via tcp to the server
   *
   * This function is passed as a callback to the encoder
   * which calls it whenever it has a frame to output. This
   * is done because the encoder does not give an output
   * upon every input, but instead buffers frames for some
   * time before any are ready. Rather than having the encoder
   * communicate externally about the outcome of encoding the
   * video frame, we simply pass this function and main isn't
   * concerned with whether it's called or not.
   *
   * The tcp socket is checked before use, and if it's not
   * connected, we connect. This means the first connection to
   * the tcp socket does not occur until sometime after recording
   * begins, once the first encoded frame is ready for transmission.
   */
  size_t total_bytes_written = 0;
  while (total_bytes_written < size) {
    if (conn.tcpfd < 0) {
      int ret = conn.conn_tcp();
      if (ret < 0) return ret;
    }

    ssize_t result = write(
      conn.tcpfd,
      data + total_bytes_written,
      size - total_bytes_written
    );

    if (result < 0) {
      if (errno == EINTR) continue;
      logger->log(logger_t::level_t::ERROR, __FILE__, __LINE__, "Error transmitting frame");
      return -1;
    }

    total_bytes_written += result;
  }

  logger->log(logger_t::level_t::INFO, __FILE__, __LINE__, "Transmitted frame");
  return 0;
}
