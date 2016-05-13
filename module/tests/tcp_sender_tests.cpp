#include <atomic>
#include <thread>

#include <gtest/gtest.h>

#include "tcp_sender.hpp"
#include "test_tcp_socket.hpp"

namespace {
  metrics::TCPSender::buf_ptr_t build_buf(const std::string& str) {
    metrics::TCPSender::buf_ptr_t buf(new boost::asio::streambuf);
    {
      std::ostream ostream(buf.get());
      ostream << str;
    }
    return buf;
  }

  const std::string SESSION_HEADER("THIS_is_a_session_HEADER"),
    HELLO("hello"),
    HEY("hey"),
    HI("hi");

  const boost::asio::ip::address DEST_LOCAL_IP = boost::asio::ip::address::from_string("127.0.0.1");
  const boost::asio::ip::address DEST_BAD_IP = boost::asio::ip::address::from_string("127.0.0.2");

  class ServiceThread {
   public:
    ServiceThread()
      : svc_(new boost::asio::io_service),
        check_timer(*svc_),
        shutdown(false) {
      LOG(INFO) << "start thread";
      check_timer.expires_from_now(boost::posix_time::milliseconds(100));
      check_timer.async_wait(std::bind(&ServiceThread::check_exit_cb, this));
      svc_thread.reset(new std::thread(std::bind(&ServiceThread::run_svc, this)));
    }
    virtual ~ServiceThread() {
      EXPECT_FALSE((bool)svc_thread) << "ServiceThread.join() must be called before destructor";
    }

    std::shared_ptr<boost::asio::io_service> svc() {
      return svc_;
    }

    void join() {
      LOG(INFO) << "join thread";
      shutdown = true;
      svc_thread->join();
      svc_thread.reset();
    }

    void flush() {
      metrics::sync_util::dispatch_run("flush", *svc_, &ServiceThread::flush_service_queue_with_noop);
    }

   private:
    void run_svc() {
      LOG(INFO) << "run svc";
      svc_->run();
      LOG(INFO) << "run svc done";
    }

    void check_exit_cb() {
      if (shutdown) {
        LOG(INFO) << "joining, exit";
        svc_->stop();
      } else {
        check_timer.expires_from_now(boost::posix_time::milliseconds(100));
        check_timer.async_wait(std::bind(&ServiceThread::check_exit_cb, this));
      }
    }

    static void flush_service_queue_with_noop() {
      LOG(INFO) << "async queue flushed";
    }

    std::shared_ptr<boost::asio::io_service> svc_;
    boost::asio::deadline_timer check_timer;
    std::shared_ptr<std::thread> svc_thread;
    std::atomic_bool shutdown;
  };
}

TEST(TCPSenderTests, connect_fails_data_dropped) {
  ServiceThread thread;
  {
    metrics::TCPSender sender(thread.svc(), SESSION_HEADER, DEST_BAD_IP, 12345);
    sender.start();

    thread.flush();

    sender.send(build_buf(HELLO));
    thread.flush();

    sender.send(build_buf(HEY));
    thread.flush();

    sender.send(build_buf(HI));
    thread.flush();

    //sleep(10);
  }
  thread.join();
}

TEST(TCPSenderTests, connect_succeeds_data_sent) {
  TestTCPReadSession test_reader;
  size_t listen_port = test_reader.port();

  ServiceThread thread;
  {
    metrics::TCPSender sender(thread.svc(), SESSION_HEADER, DEST_LOCAL_IP, listen_port);
    sender.start();

    thread.flush();
    EXPECT_TRUE(test_reader.wait_for_available());
    EXPECT_EQ(SESSION_HEADER, *test_reader.read());

    sender.send(build_buf(HELLO));
    thread.flush();
    EXPECT_TRUE(test_reader.wait_for_available());
    EXPECT_EQ(HELLO, *test_reader.read());

    sender.send(build_buf(HEY));
    thread.flush();
    EXPECT_TRUE(test_reader.wait_for_available());
    EXPECT_EQ(HEY, *test_reader.read());

    sender.send(build_buf(HI));
    thread.flush();
    EXPECT_TRUE(test_reader.wait_for_available());
    EXPECT_EQ(HI, *test_reader.read());
  }
  thread.join();

  EXPECT_FALSE(test_reader.available());
}

TEST(TCPSenderTests, connect_fails_then_succeeds) {
  std::shared_ptr<TestTCPReadSession> test_reader(new TestTCPReadSession);
  size_t listen_port = test_reader->port();
  test_reader.reset();

  ServiceThread thread;
  {
    metrics::TCPSender sender(thread.svc(), SESSION_HEADER, DEST_LOCAL_IP, listen_port);
    sender.start();

    thread.flush();

    sender.send(build_buf(HELLO));
    thread.flush();

    // reopen reader socket at previous portnum
    test_reader.reset(new TestTCPReadSession(listen_port));

    // wait long enough for sender to reconnect
    LOG(INFO) << "TEST SLEEP FOR SENDER";
    sleep(2);

    sender.send(build_buf(HEY));
    thread.flush();
    EXPECT_TRUE(test_reader->wait_for_available());
    EXPECT_EQ(SESSION_HEADER, *test_reader->read());
    EXPECT_EQ(HEY, *test_reader->read());

    sender.send(build_buf(HI));
    thread.flush();
    EXPECT_TRUE(test_reader->wait_for_available());
    EXPECT_EQ(HI, *test_reader->read());
  }
  thread.join();

  EXPECT_FALSE(test_reader->available());
}

TEST(TCPSenderTests, connect_succeeds_then_fails) {
  std::shared_ptr<TestTCPReadSession> test_reader(new TestTCPReadSession);
  size_t listen_port = test_reader->port();

  ServiceThread thread;
  {
    metrics::TCPSender sender(thread.svc(), SESSION_HEADER, DEST_LOCAL_IP, listen_port);
    sender.start();

    thread.flush();
    EXPECT_TRUE(test_reader->wait_for_available());
    EXPECT_EQ(SESSION_HEADER, *test_reader->read());

    sender.send(build_buf(HELLO));
    thread.flush();
    EXPECT_TRUE(test_reader->wait_for_available());
    EXPECT_EQ(HELLO, *test_reader->read());

    // close reader socket
    test_reader.reset();

    sender.send(build_buf(HEY));
    thread.flush();

    sender.send(build_buf(HI));
    thread.flush();
  }
  thread.join();
}

TEST(TCPSenderTests, connect_succeeds_then_fails_then_succeeds) {
  std::shared_ptr<TestTCPReadSession> test_reader(new TestTCPReadSession);
  size_t listen_port = test_reader->port();

  ServiceThread thread;
  {
    metrics::TCPSender sender(thread.svc(), SESSION_HEADER, DEST_LOCAL_IP, listen_port);
    sender.start();

    thread.flush();
    EXPECT_TRUE(test_reader->wait_for_available());
    EXPECT_EQ(SESSION_HEADER, *test_reader->read());

    sender.send(build_buf(HELLO));
    thread.flush();
    EXPECT_TRUE(test_reader->wait_for_available());
    EXPECT_EQ(HELLO, *test_reader->read());

    sender.send(build_buf(HI));
    thread.flush();
    EXPECT_TRUE(test_reader->wait_for_available());
    EXPECT_EQ(HI, *test_reader->read());

    // close reader socket
    test_reader.reset();

    sender.send(build_buf(HEY));
    thread.flush();

    sender.send(build_buf(HI));
    thread.flush();

    // reopen reader socket at previous portnum
    test_reader.reset(new TestTCPReadSession(listen_port));

    // wait long enough for sender to reconnect
    LOG(INFO) << "TEST SLEEP FOR SENDER";
    sleep(2);

    sender.send(build_buf(HELLO));
    thread.flush();
    EXPECT_TRUE(test_reader->wait_for_available());
    EXPECT_EQ(SESSION_HEADER, *test_reader->read());
    EXPECT_EQ(HELLO, *test_reader->read());

    sender.send(build_buf(HI));
    thread.flush();
    EXPECT_TRUE(test_reader->wait_for_available());
    EXPECT_EQ(HI, *test_reader->read());
  }
  thread.join();
}

int main(int argc, char **argv) {
  ::google::InitGoogleLogging(argv[0]);
  // avoid non-threadsafe logging code for these tests
  FLAGS_logtostderr = 1;
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}