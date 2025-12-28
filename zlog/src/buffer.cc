#include "buffer.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace zlog {

Buffer::Buffer()
    : data_(static_cast<char *>(std::malloc(DEFAULT_BUFFER_SIZE))),
      capacity_(DEFAULT_BUFFER_SIZE), writerIdx_(0), readerIdx_(0) {
  if (!data_) {
    throw std::bad_alloc();
  }
}

Buffer::~Buffer() {
  if (data_) {
    std::free(data_);
  }
}

void Buffer::push(const char *data, size_t len) {
  ensureEnoughSize(len);
  std::memcpy(data_ + writerIdx_, data, len);
  moveWriter(len);
}

const char *Buffer::begin() const { return data_ + readerIdx_; }

size_t Buffer::writeAbleSize() const { return (capacity_ - writerIdx_); }

size_t Buffer::readAbleSize() const { return writerIdx_ - readerIdx_; }

void Buffer::moveReader(size_t len) {
  assert(len <= readAbleSize());
  readerIdx_ += len;
}

void Buffer::reset() { readerIdx_ = writerIdx_ = 0; }

void Buffer::swap(Buffer &buffer) noexcept {
  std::swap(data_, buffer.data_);
  std::swap(capacity_, buffer.capacity_);
  std::swap(readerIdx_, buffer.readerIdx_);
  std::swap(writerIdx_, buffer.writerIdx_);
}

bool Buffer::empty() const { return readerIdx_ == writerIdx_; }

void Buffer::ensureEnoughSize(size_t len) {
  if (len <= writeAbleSize())
    return;
  size_t newSize = 0;
  if (capacity_ < THRESHOLD_BUFFER_SIZE) {
    newSize = capacity_ * 2 + len;
  } else {
    newSize = capacity_ + INCREMENT_BUFFER_SIZE + len;
  }

  if (newSize > MAX_BUFFER_SIZE) {
    throw std::length_error("Buffer size exceeded MAX_BUFFER_SIZE");
  }

  char *newData = static_cast<char *>(std::realloc(data_, newSize));
  if (!newData) {
    throw std::bad_alloc();
  }
  data_ = newData;
  capacity_ = newSize;
}

void Buffer::moveWriter(size_t len) {
  assert(len <= writeAbleSize());
  writerIdx_ += len;
}

} // namespace zlog
