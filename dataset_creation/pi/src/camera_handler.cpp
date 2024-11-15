// © 2024 Alec Fessler
// MIT License
// See LICENSE file in the project root for full license information.

#include <cstring>
#include <iostream>
#include <semaphore.h>
#include <stdexcept>
#include <sys/mman.h>
#include "camera_handler.h"
#include "config_parser.h"
#include "logger.h"
#include "lock_free_queue.h"

camera_handler_t::camera_handler_t(
  config_parser& config,
  logger_t& logger,
  lock_free_queue_t& frame_queue,
  sem_t& queue_counter
) :
  logger(logger),
  frame_queue(frame_queue),
  queue_counter(queue_counter),
  next_req_idx_(0),
  frame_bytes_offset_(0) {

  unsigned int frame_width = config.get_int("FRAME_WIDTH");
  unsigned int frame_height = config.get_int("FRAME_HEIGHT");
  unsigned int frame_duration_min = config.get_int("FRAME_DURATION_MIN");
  unsigned int frame_duration_max = config.get_int("FRAME_DURATION_MAX");
  frame_buffers_ = config.get_int("FRAME_BUFFERS");
  dma_frame_buffers_ = config.get_int("DMA_BUFFERS");

  unsigned int y_plane_bytes = frame_width * frame_height;
  unsigned int u_plane_bytes = y_plane_bytes / 4;
  unsigned int v_plane_bytes = u_plane_bytes;
  frame_bytes_ = y_plane_bytes + u_plane_bytes + v_plane_bytes;

  cm_ = std::make_unique<libcamera::CameraManager>();
  if (cm_->start() < 0) {
    const char* err = "Failed to start camera manager";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  auto cameras = cm_->cameras();
  if (cameras.empty()) {
    const char* err = "No cameras available";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  camera_ = cm_->get(cameras[0]->id());
  if (!camera_) {
    const char* err = "Failed to retrieve camera";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }
  if (camera_->acquire() < 0) {
    const char* err = "Failed to acquire camera";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  config_ = camera_->generateConfiguration({ libcamera::StreamRole::VideoRecording });
  if (!config_) {
    const char* err = "Failed to generate camera configuration";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  libcamera::StreamConfiguration& cfg = config_->at(0);
  cfg.pixelFormat = libcamera::formats::YUV420;
  cfg.size = { frame_width, frame_height };
  cfg.bufferCount = dma_frame_buffers_;

  if (config_->validate() == libcamera::CameraConfiguration::Invalid) {
    const char* err = "Invalid camera configuration, unable to adjust";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  } else if (config_->validate() == libcamera::CameraConfiguration::Adjusted) {
    const char* err = "Invalid camera configuration, adjusted";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  if (camera_->configure(config_.get()) < 0) {
    const char* err = "Failed to configure camera";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  allocator_ = std::make_unique<libcamera::FrameBufferAllocator>(camera_);
  stream_ = cfg.stream();
  if (allocator_->allocate(stream_) < 0) {
    const char* err = "Failed to allocate buffers";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  uint64_t req_cookie = 0; // maps request to index in mmap_buffers_
  for (const std::unique_ptr<libcamera::FrameBuffer>& buffer : allocator_->buffers(stream_)) {
    std::unique_ptr<libcamera::Request> request = camera_->createRequest(req_cookie++);
    if (!request) {
      const char* err = "Failed to create request";
      logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
      throw std::runtime_error(err);
    }
    if (request->addBuffer(stream_, buffer.get()) < 0) {
      const char* err = "Failed to add buffer to request";
      logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
      throw std::runtime_error(err);
    }
    requests_.push_back(std::move(request));

    const libcamera::FrameBuffer::Plane& y_plane = buffer->planes()[0];
    const libcamera::FrameBuffer::Plane& u_plane = buffer->planes()[1];
    const libcamera::FrameBuffer::Plane& v_plane = buffer->planes()[2];

    if (y_plane.length != y_plane_bytes || u_plane.length != u_plane_bytes || v_plane.length != v_plane_bytes) {
      const char* err = "Plane size does not match expected size";
      logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
      throw std::runtime_error(err);
    }

    void* data = mmap(
      nullptr,
      frame_bytes_,
      PROT_READ | PROT_WRITE,
      MAP_SHARED,
      y_plane.fd.get(),
      y_plane.offset
    );

    if (data == MAP_FAILED) {
      std::string err = "Failed to mmap plane data: " + std::string(strerror(errno));
      logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err.c_str());
      throw std::runtime_error(err);
    }

    mmap_buffers_.push_back(data);
  }

  camera_->requestCompleted.connect(this, &camera_handler_t::request_complete);

  // Configure some settings for more deterministic capture times
  // May need to be adjusted based on lighting conditions
  // and on a per device basis, but for development purposes, this is acceptable
  controls_ = std::make_unique<libcamera::ControlList>();

  // Fix exposure time to half the time between frames
  // May be able to remove frame duration limit control since we are setting exposure
  controls_->set(libcamera::controls::FrameDurationLimits, libcamera::Span<const std::int64_t, 2>({ frame_duration_min, frame_duration_max }));
  controls_->set(libcamera::controls::AeEnable, false);
  controls_->set(libcamera::controls::ExposureTime, frame_duration_min);

  // Fix focus to ~12 inches
  // Focus value should be reciprocal of distance in meters
  controls_->set(libcamera::controls::AfMode, libcamera::controls::AfModeManual);
  controls_->set(libcamera::controls::LensPosition, 3.33);

  // Fix white balance, gain, and disable HDR
  controls_->set(libcamera::controls::AwbEnable, false);
  controls_->set(libcamera::controls::AnalogueGain, 1.0);
  controls_->set(libcamera::controls::HdrMode, libcamera::controls::HdrModeOff);

  controls_->set(libcamera::controls::rpi::StatsOutputEnable, false);

  if (camera_->start(controls_.get()) < 0) {
    const char* err = "Failed to start camera";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  frame_bytes_buffer_ = malloc(frame_bytes_ * frame_buffers_);
  if (!frame_bytes_buffer_) {
    const char* err = "Failed to allocate frame bytes buffer";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }
}

camera_handler_t::~camera_handler_t() {
  camera_->stop();
  for (void* data : mmap_buffers_)
    munmap(data, frame_bytes_);
  free(frame_bytes_buffer_);
  allocator_->free(stream_);
  allocator_.reset();
  camera_->release();
  camera_.reset();
  cm_->stop();
}

void camera_handler_t::queue_request() {
  /**
   * Queue the next request in the sequence.
   *
   * Before queuing the request, ensure that the number of enqueued
   * buffers is no more than frame_buffers_ - 2. This is because
   * the queue counter may fall behind by, but no more than, 1. This
   * occurs when the main loop calls sem_wait, decrementing the
   * semaphore, but before it dequeues the buffer. Thus, we check
   * for 2 less than max to ensure at least one is available even
   * if the counter is behind by 1.
   *
   * If requests are not returned at the same rate as they are queued,
   * this method will throw to signal that the camera is not keeping up,
   * and this should be handled by adjusting the configuration.
   * ie. framerate, exposure, gain, etc.
   *
   * Throws:
   *  - std::runtime_error: Buffer is not ready for requeuing
   *  - std::runtime_error: Failed to queue request
   */
  int enqueued_buffers = 0;
  sem_getvalue(&queue_counter, &enqueued_buffers);

  if (enqueued_buffers > frame_buffers_ - 2) {
    const char* err = "Buffer is not ready for requeuing";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
    return;
  }

  if (camera_->queueRequest(requests_[next_req_idx_].get()) < 0) {
    const char* err = "Failed to queue request";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
    return;
  }

  ++next_req_idx_;
  next_req_idx_ %= requests_.size();
}

void camera_handler_t::request_complete(libcamera::Request* request) {
  /**
   * Signal handler for when a request is completed.
   *
   * This method is called by the camera manager when a request is completed.
   * The request is then reused and the buffer is enqueued for transmission.
   * The queue counter is incremented to signal that a buffer is available.
   *
   * Parameters:
   *  - request: The completed request
   */
  if (request->status() == libcamera::Request::RequestCancelled)
    return;

  const char* info = "Request completed";
  logger.log(logger_t::level_t::INFO, __FILE__, __LINE__, info);

  void* data = mmap_buffers_[request->cookie()];
  void* frame_offset = (char*)frame_bytes_buffer_ + frame_bytes_ * frame_bytes_offset_;

  memcpy(frame_offset, data, frame_bytes_);
  frame_bytes_offset_ = (frame_bytes_offset_ + 1) % frame_buffers_;

  bool enqueued = false;
  do {
    enqueued = frame_queue.enqueue(frame_offset);
  } while(!enqueued);

  sem_post(&queue_counter);
  request->reuse(libcamera::Request::ReuseBuffers);
}
