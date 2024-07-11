#ifdef USE_ESP_IDF

#include "streamer.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

#include "esp_dsp.h"
namespace esphome {
namespace nabu {

// Major TODOs:
//  - Rename/split up file, it contains more than one class
//  - Ring buffers are potentially unsafe when used outside of a component

static const size_t HTTP_BUFFER_SIZE = 8192;
static const size_t BUFFER_SIZE = 2048;

// static const size_t QUEUE_COUNT = 10;

DecodeStreamer::DecodeStreamer() {
  this->input_ring_buffer_ = RingBuffer::create(BUFFER_SIZE * sizeof(int16_t));
  this->output_ring_buffer_ = RingBuffer::create(BUFFER_SIZE * sizeof(int16_t));

  this->event_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(TaskEvent));
  this->command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(CommandEvent));

  // TODO: Handle if this fails to allocate
  if ((this->input_ring_buffer_) || (this->output_ring_buffer_ == nullptr)) {
    return;
  }
}

void DecodeStreamer::start(UBaseType_t priority) {
  if (this->task_handle_ == nullptr) {
    xTaskCreate(DecodeStreamer::decode_task_, "decode_task", 8096, (void *) this, priority, &this->task_handle_);
  }
}

void OutputStreamer::stop() {
  vTaskDelete(this->task_handle_);
  this->task_handle_ = nullptr;

  xQueueReset(this->event_queue_);
  xQueueReset(this->command_queue_);
}

size_t DecodeStreamer::write(uint8_t *buffer, size_t length) {
  size_t free_bytes = this->input_ring_buffer_->free();
  size_t bytes_to_write = std::min(length, free_bytes);
  if (bytes_to_write > 0) {
    return this->input_ring_buffer_->write((void *) buffer, bytes_to_write);
  }
  return 0;
}

void DecodeStreamer::decode_task_(void *params) {
  DecodeStreamer *this_streamer = (DecodeStreamer *) params;

  TaskEvent event;
  CommandEvent command_event;

  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  uint8_t *buffer = allocator.allocate(BUFFER_SIZE * sizeof(int16_t));

  if (buffer == nullptr) {
    event.type = EventType::WARNING;
    event.err = ESP_ERR_NO_MEM;
    xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

    event.type = EventType::STOPPED;
    event.err = ESP_OK;
    xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

    while (true) {
      delay(10);
    }

    return;
  }

  event.type = EventType::STARTED;
  xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

  bool stopping = false;
  while (true) {
    if (xQueueReceive(this_streamer->command_queue_, &command_event, (10 / portTICK_PERIOD_MS)) == pdTRUE) {
      if (command_event.command == CommandEventType::START) {
        this_streamer->reset_ring_buffers();
      } else if (command_event.command == CommandEventType::STOP) {
        break;
      } else if (command_event.command == CommandEventType::STOP_GRACEFULLY) {
        stopping = true;
      }
    }

    size_t bytes_available = this_streamer->input_ring_buffer_->available();
    // we will need to know how much we can fit in the output buffer as well depending the file type
    size_t bytes_free = this_streamer->output_ring_buffer_->free();

    size_t bytes_to_read = std::min(bytes_free, bytes_available);
    size_t bytes_read = 0;
    if (bytes_to_read > 0) {
      bytes_read = this_streamer->input_ring_buffer_->read((void *) buffer, bytes_to_read);
    }

    size_t bytes_written = 0;
    if (bytes_read > 0) {
      bytes_written = this_streamer->output_ring_buffer_->write((void *) buffer, bytes_read);
    }

    if (this_streamer->input_ring_buffer_->available() || this_streamer->output_ring_buffer_->available()) {
      event.type = EventType::RUNNING;
      xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
    } else {
      event.type = EventType::IDLE;
      xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
    }

    if (stopping && (this_streamer->input_ring_buffer_->available() == 0) &&
        (this_streamer->output_ring_buffer_->available() == 0)) {
      break;
    }
  }
  event.type = EventType::STOPPING;
  xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

  this_streamer->reset_ring_buffers();
  allocator.deallocate(buffer, BUFFER_SIZE);

  event.type = EventType::STOPPED;
  xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

  while (true) {
    delay(10);
  }
}

void DecodeStreamer::reset_ring_buffers() {
  this->input_ring_buffer_->reset();
  this->output_ring_buffer_->reset();
}

HTTPStreamer::HTTPStreamer() {
  this->output_ring_buffer_ = RingBuffer::create(HTTP_BUFFER_SIZE * sizeof(int16_t));
  // TODO: Handle if this fails to allocate
  if (this->output_ring_buffer_ == nullptr) {
    return;
  }

  this->event_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(TaskEvent));
  this->command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(CommandEvent));
}

void HTTPStreamer::establish_connection_(esp_http_client_handle_t *client) {
  this->cleanup_connection_(client);

  if (this->current_uri_.empty()) {
    return;
  }

  esp_http_client_config_t config = {
      .url = this->current_uri_.c_str(),
      .cert_pem = nullptr,
      .disable_auto_redirect = false,
      .max_redirection_count = 10,
  };
  *client = esp_http_client_init(&config);

  if (client == nullptr) {
    // ESP_LOGE(TAG, "Failed to initialize HTTP connection");
    return;
  }

  esp_err_t err;
  if ((err = esp_http_client_open(*client, 0)) != ESP_OK) {
    // ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
    this->cleanup_connection_(client);
    return;
  }

  int content_length = esp_http_client_fetch_headers(*client);

  if (content_length <= 0) {
    // ESP_LOGE(TAG, "Failed to get content length: %s", esp_err_to_name(err));
    this->cleanup_connection_(client);
    return;
  }

  return;
} 

void HTTPStreamer::start(UBaseType_t priority) {
  if (this->task_handle_ == nullptr) {
    xTaskCreate(HTTPStreamer::read_task_, "read_task", 8096, (void *) this, priority, &this->task_handle_);
  }

  CommandEvent command_event;
  command_event.command = CommandEventType::START;
  this->send_command(&command_event);
}

void HTTPStreamer::start(const std::string &uri, UBaseType_t priority) {
  this->current_uri_ = uri;
  this->start(priority);
}

void HTTPStreamer::cleanup_connection_(esp_http_client_handle_t *client) {
  if (*client != nullptr) {
    esp_http_client_close(*client);
    esp_http_client_cleanup(*client);
    *client = nullptr;
  }
}

void HTTPStreamer::read_task_(void *params) {
  HTTPStreamer *this_streamer = (HTTPStreamer *) params;

  TaskEvent event;
  CommandEvent command_event;

  esp_http_client_handle_t client = nullptr;

  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  uint8_t *buffer = allocator.allocate(HTTP_BUFFER_SIZE * sizeof(int16_t));

  if (buffer == nullptr) {
    event.type = EventType::WARNING;
    event.err = ESP_ERR_NO_MEM;
    xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

    event.type = EventType::STOPPED;
    event.err = ESP_OK;
    xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

    while (true) {
      delay(10);
    }

    return;
  }

  event.type = EventType::STARTED;
  xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

  while (true) {
    if (xQueueReceive(this_streamer->command_queue_, &command_event, (10 / portTICK_PERIOD_MS)) == pdTRUE) {
      if (command_event.command == CommandEventType::START) {
        this_streamer->reset_ring_buffers();
        this_streamer->establish_connection_(&client);
      } else if (command_event.command == CommandEventType::STOP) {
        this_streamer->cleanup_connection_(&client);
        break;
      } else if (command_event.command == CommandEventType::STOP_GRACEFULLY) {
        // Waits until output ring buffer is empty before stopping the loop
        this_streamer->cleanup_connection_(&client);
      }
    }

    if (client != nullptr) {
      size_t read_bytes = this_streamer->output_ring_buffer_->free();
      int received_len = 0;
      if (read_bytes > 0) {
        received_len = esp_http_client_read(client, (char *) buffer, read_bytes);
      }

      if (received_len > 0) {
        size_t bytes_written = this_streamer->output_ring_buffer_->write((void *) buffer, received_len);
      } else if (received_len < 0) {
        // Error situation
      }

      if (esp_http_client_is_complete_data_received(client)) {
        // this_streamer->current_uri_ = std::string();
        this_streamer->cleanup_connection_(&client);
      }

      event.type = EventType::RUNNING;
      xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
    } else if (this_streamer->output_ring_buffer_->available() > 0) {
      // the connection is closed but there is still data in the ring buffer
      event.type = EventType::IDLE;
      xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
    } else {
      // there is no active connection and the ring buffer is empty, so move to end task
      break;
    }
  }
  event.type = EventType::STOPPING;
  xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

  this_streamer->reset_ring_buffers();
  allocator.deallocate(buffer, HTTP_BUFFER_SIZE);

  event.type = EventType::STOPPED;
  xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

  while (true) {
    delay(10);
  }
}

CombineStreamer::CombineStreamer() {
  this->output_ring_buffer_ = RingBuffer::create(BUFFER_SIZE * sizeof(int16_t));
  this->media_ring_buffer_ = RingBuffer::create(BUFFER_SIZE * sizeof(int16_t));
  this->announcement_ring_buffer_ = RingBuffer::create(BUFFER_SIZE * sizeof(int16_t));

  // TODO: Handle not being able to allocate these...
  if ((this->output_ring_buffer_ == nullptr) || (this->media_ring_buffer_ == nullptr) ||
      ((this->output_ring_buffer_ == nullptr))) {
    return;
  }

  this->event_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(TaskEvent));
  this->command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(CommandEvent));
}

size_t CombineStreamer::write_media(uint8_t *buffer, size_t length) {
  size_t free_bytes = this->media_free();
  size_t bytes_to_write = std::min(length, free_bytes);
  if (bytes_to_write > 0) {
    return this->media_ring_buffer_->write((void *) buffer, bytes_to_write);
  }
  return 0;
}

size_t CombineStreamer::write_announcement(uint8_t *buffer, size_t length) {
  size_t free_bytes = this->announcement_free();
  size_t bytes_to_write = std::min(length, free_bytes);

  if (bytes_to_write > 0) {
    return this->announcement_ring_buffer_->write((void *) buffer, bytes_to_write);
  }
  return 0;
}

void CombineStreamer::start(UBaseType_t priority) {
  if (this->task_handle_ == nullptr) {
    xTaskCreate(CombineStreamer::combine_task_, "combine_task", 8096, (void *) this, priority, &this->task_handle_);
  }
}

void CombineStreamer::reset_ring_buffers() {
  this->output_ring_buffer_->reset();
  this->media_ring_buffer_->reset();
  this->announcement_ring_buffer_->reset();
}

void CombineStreamer::combine_task_(void *params) {
  CombineStreamer *this_combiner = (CombineStreamer *) params;

  TaskEvent event;
  CommandEvent command_event;

  //  ?big? assumption here is that incoming stream is 16 bits per sample... TODO: Check and verify this
  // TODO: doesn't handle different sample rates
  // TODO: doesn't handle different amount of channels
  ExternalRAMAllocator<int16_t> allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);
  int16_t *media_buffer = allocator.allocate(BUFFER_SIZE);
  int16_t *announcement_buffer = allocator.allocate(BUFFER_SIZE);
  int16_t *combination_buffer = allocator.allocate(BUFFER_SIZE);

  if ((media_buffer == nullptr) || (announcement_buffer == nullptr)) {
    event.type = EventType::WARNING;
    event.err = ESP_ERR_NO_MEM;
    xQueueSend(this_combiner->event_queue_, &event, portMAX_DELAY);

    event.type = EventType::STOPPED;
    event.err = ESP_OK;
    xQueueSend(this_combiner->event_queue_, &event, portMAX_DELAY);

    while (true) {
      delay(10);
    }

    return;
  }

  event.type = EventType::STARTED;
  xQueueSend(this_combiner->event_queue_, &event, portMAX_DELAY);

  int16_t q15_ducking_ratio = (int16_t) (1 * std::pow(2, 15));  // esp-dsp using q15 fixed point numbers
  bool transfer_media = true;

  while (true) {
    if (xQueueReceive(this_combiner->command_queue_, &command_event, (10 / portTICK_PERIOD_MS)) == pdTRUE) {
      if (command_event.command == CommandEventType::STOP) {
        break;
      } else if (command_event.command == CommandEventType::DUCK) {
        float ducking_ratio = command_event.ducking_ratio;
        q15_ducking_ratio = (int16_t) (ducking_ratio * std::pow(2, 15));  // convert float to q15 fixed point
      } else if (command_event.command == CommandEventType::PAUSE_MEDIA) {
        transfer_media = false;
      } else if (command_event.command == CommandEventType::RESUME_MEDIA) {
        transfer_media = true;
      }
    }

    size_t media_available = this_combiner->media_ring_buffer_->available();
    size_t announcement_available = this_combiner->announcement_ring_buffer_->available();
    size_t output_free = this_combiner->output_ring_buffer_->free();

    if ((output_free > 0) && (media_available * transfer_media + announcement_available > 0)) {
      size_t bytes_to_read = output_free;

      if (media_available > 0) {
        bytes_to_read = std::min(bytes_to_read, media_available);
      }

      if (announcement_available > 0) {
        bytes_to_read = std::min(bytes_to_read, announcement_available);
      }

      size_t media_bytes_read = 0;
      if (transfer_media && (media_available > 0)) {
        media_bytes_read = this_combiner->media_ring_buffer_->read((void *) media_buffer, bytes_to_read, 0);
        if (media_bytes_read > 0) {
          if (q15_ducking_ratio < (1 * std::pow(2, 15))) {
            dsps_mulc_s16_ae32(media_buffer, combination_buffer, media_bytes_read, q15_ducking_ratio, 1, 1);
            std::memcpy((void *) media_buffer, (void *) combination_buffer, media_bytes_read);
          }
        }
      }

      size_t announcement_bytes_read = 0;
      if (announcement_available > 0) {
        announcement_bytes_read =
            this_combiner->announcement_ring_buffer_->read((void *) announcement_buffer, bytes_to_read, 0);
      }

      size_t bytes_written = 0;
      if ((media_bytes_read > 0) && (announcement_bytes_read > 0)) {
        // This adds the two signals together and then shifts it by 1 bit to avoid clipping
        // TODO: Don't shift by 1 as the announcement stream will be quieter than desired (need to clamp?)
        dsps_add_s16_aes3(media_buffer, announcement_buffer, combination_buffer, bytes_to_read, 1, 1, 1, 1);
        bytes_written = this_combiner->output_ring_buffer_->write((void *) combination_buffer, bytes_to_read);
      } else if (media_bytes_read > 0) {
        bytes_written = this_combiner->output_ring_buffer_->write((void *) media_buffer, media_bytes_read);
        // size_t total_mono_samples = media_bytes_read/2;
        // for (int i = 0; i < total_mono_samples; ++i) {
        //   combination_buffer[2*i] = media_buffer[i];
        //   combination_buffer[2*i+1] = media_buffer[i];
        // }

        // size_t stereo_bytes_used = total_mono_samples*2*sizeof(int16_t);
        // ESP_LOGD("mixer", "free bytes in ring bufer %d; bytes wanting to write %d",
        // this_combiner->output_ring_buffer_->free(), stereo_bytes_used); bytes_written =
        // this_combiner->output_ring_buffer_->write((void *) media_buffer, stereo_bytes_used);
      } else if (announcement_bytes_read > 0) {
        bytes_written =
            this_combiner->output_ring_buffer_->write((void *) announcement_buffer, announcement_bytes_read);
      }

      if (bytes_written) {
        event.type = EventType::RUNNING;
        xQueueSend(this_combiner->event_queue_, &event, portMAX_DELAY);
      } else if (this_combiner->output_ring_buffer_->available() == 0) {
        event.type = EventType::IDLE;
        xQueueSend(this_combiner->event_queue_, &event, portMAX_DELAY);
      }
    }
  }

  event.type = EventType::STOPPING;
  xQueueSend(this_combiner->event_queue_, &event, portMAX_DELAY);

  this_combiner->reset_ring_buffers();
  allocator.deallocate(media_buffer, BUFFER_SIZE);
  allocator.deallocate(announcement_buffer, BUFFER_SIZE);
  allocator.deallocate(combination_buffer, BUFFER_SIZE);

  event.type = EventType::STOPPED;
  xQueueSend(this_combiner->event_queue_, &event, portMAX_DELAY);

  while (true) {
    delay(10);
  }
}

void Pipeline::transfer_task_(void *params) {
  Pipeline *this_pipeline = (Pipeline *) params;

  TaskEvent event;
  CommandEvent command_event;

  event.type = EventType::STARTING;
  event.err = ESP_OK;
  xQueueSend(this_pipeline->event_queue_, &event, portMAX_DELAY);

  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  uint8_t *transfer_buffer = allocator.allocate(BUFFER_SIZE * sizeof(int16_t));
  if (transfer_buffer == nullptr) {
    event.type = EventType::WARNING;
    event.err = ESP_ERR_NO_MEM;
    xQueueSend(this_pipeline->event_queue_, &event, portMAX_DELAY);

    event.type = EventType::STOPPED;
    event.err = ESP_OK;
    xQueueSend(this_pipeline->event_queue_, &event, portMAX_DELAY);

    while (true) {
      delay(10);
    }

    return;
  }

  event.type = EventType::STARTED;
  xQueueSend(this_pipeline->event_queue_, &event, portMAX_DELAY);

  bool stopping = false;

  this_pipeline->reading_ = true;
  this_pipeline->decoding_ = true;

  while (true) {
    if (xQueueReceive(this_pipeline->command_queue_, &command_event, (10 / portTICK_PERIOD_MS)) == pdTRUE) {
      if (command_event.command == CommandEventType::STOP) {
        this_pipeline->reader_->send_command(&command_event);
        this_pipeline->decoder_->send_command(&command_event);
        break;
      }
      if (command_event.command == CommandEventType::STOP_GRACEFULLY) {
        this_pipeline->reader_->send_command(&command_event);
        stopping = true;
      }
    }

    size_t bytes_to_read = 0;
    size_t bytes_read = 0;
    size_t bytes_written = 0;

    // Move data from decoder to the mixer
    if (this_pipeline->pipeline_type_ == PipelineType::MEDIA) {
      bytes_to_read = this_pipeline->mixer_->media_free();
      bytes_read = this_pipeline->decoder_->read(transfer_buffer, bytes_to_read);
      bytes_written += this_pipeline->mixer_->write_media(transfer_buffer, bytes_read);
    } else if (this_pipeline->pipeline_type_ == PipelineType::ANNOUNCEMENT) {
      bytes_to_read = this_pipeline->mixer_->announcement_free();
      bytes_read = this_pipeline->decoder_->read(transfer_buffer, bytes_to_read);
      bytes_written += this_pipeline->mixer_->write_announcement(transfer_buffer, bytes_read);
    }

    // Move data from http reader to decoder
    bytes_to_read = this_pipeline->decoder_->input_free();
    bytes_read = this_pipeline->reader_->read(transfer_buffer, bytes_to_read);
    bytes_written = this_pipeline->decoder_->write(transfer_buffer, bytes_read);

    this_pipeline->watch_();

    // This isn't being called once teh reader and decoder have finished... so the pipeline never closes!
    if (!this_pipeline->reading_ && !this_pipeline->decoding_) {
      break;
    }
  }

  event.type = EventType::STOPPING;
  xQueueSend(this_pipeline->event_queue_, &event, portMAX_DELAY);

  allocator.deallocate(transfer_buffer, BUFFER_SIZE);

  event.type = EventType::STOPPED;
  xQueueSend(this_pipeline->event_queue_, &event, portMAX_DELAY);

  while (true) {
    delay(10);
  }
}

}  // namespace nabu
}  // namespace esphome

#endif