#include "buffer.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace zlog;

class BufferTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Code here will be called immediately after the constructor (right
    // before each test).
  }

  void TearDown() override {
    // Code here will be called immediately after each test (right
    // before the destructor).
  }

  Buffer buf;
};

TEST_F(BufferTest, InitialState) {
  EXPECT_EQ(buf.readAbleSize(), 0);
  EXPECT_EQ(buf.writeAbleSize(), DEFAULT_BUFFER_SIZE);
  EXPECT_TRUE(buf.empty());
}

TEST_F(BufferTest, PushAndConsume) {
  std::string data = "hello world";
  buf.push(data.c_str(), data.length());

  EXPECT_EQ(buf.readAbleSize(), data.length());
  EXPECT_FALSE(buf.empty());

  std::string output(buf.begin(), buf.readAbleSize());
  EXPECT_EQ(output, data);

  buf.moveReader(data.length());
  EXPECT_EQ(buf.readAbleSize(), 0);
  EXPECT_TRUE(buf.empty());
}

TEST_F(BufferTest, Reset) {
  buf.push("abc", 3);
  buf.reset();
  EXPECT_EQ(buf.readAbleSize(), 0);
  EXPECT_EQ(buf.writeAbleSize(), DEFAULT_BUFFER_SIZE);
  EXPECT_TRUE(buf.empty());
}

TEST_F(BufferTest, AutoResize) {
  std::vector<char> largeData(1024 * 1024 * 3, 'a'); // 3MB > 2MB default
  EXPECT_NO_THROW(buf.push(largeData.data(), largeData.size()));
  EXPECT_EQ(buf.readAbleSize(), largeData.size());
}

TEST_F(BufferTest, Swap) {
  Buffer b2;
  buf.push("hello", 5);
  b2.push("world", 5);

  buf.swap(b2);

  std::string s1(buf.begin(), buf.readAbleSize());
  std::string s2(b2.begin(), b2.readAbleSize());

  EXPECT_EQ(s1, "world");
  EXPECT_EQ(s2, "hello");
}
