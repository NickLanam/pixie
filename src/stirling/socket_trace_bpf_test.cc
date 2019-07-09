#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sys/types.h>
#include <unistd.h>
#include <experimental/filesystem>

#include <cstdlib>
#include <string_view>
#include <thread>

#include "src/shared/types/column_wrapper.h"
#include "src/shared/types/types.h"
#include "src/stirling/bcc_bpf/socket_trace.h"
#include "src/stirling/socket_trace_connector.h"
#include "src/stirling/testing/tcp_socket.h"

namespace pl {
namespace stirling {

using ::pl::stirling::testing::TCPSocket;
using ::pl::types::ColumnWrapper;
using ::pl::types::ColumnWrapperRecordBatch;
using ::testing::Pair;
using ::testing::SizeIs;
using ::testing::UnorderedElementsAre;

class HTTPTraceBPFTest : public ::testing::Test {
 protected:
  void SetUp() override {
    source = SocketTraceConnector::Create("socket_trace_connector");
    ASSERT_OK(source->Init());
  }

  class ClientServerSystem {
   public:
    ClientServerSystem() { server.Bind(); }

    void RunWriterReader(const std::vector<std::string_view>& write_data) {
      SpawnReaderClient();
      SpawnWriterServer(write_data);
      JoinThreads();
    }

    void RunSenderReceiver(const std::vector<std::string_view>& write_data) {
      SpawnReceiverClient();
      SpawnSenderServer(write_data);
      JoinThreads();
    }

    void SpawnReaderClient() {
      client_thread = std::thread([this]() {
        client.Connect(server);
        std::string data;
        while (client.Read(&data)) {
        }
        client.Close();
      });
    }

    void SpawnReceiverClient() {
      client_thread = std::thread([this]() {
        client.Connect(server);
        std::string data;
        while (client.Recv(&data)) {
        }
        client.Close();
      });
    }

    void SpawnWriterServer(const std::vector<std::string_view>& write_data) {
      server_thread = std::thread([this, write_data]() {
        server.Accept();
        for (auto data : write_data) {
          ASSERT_EQ(data.length(), server.Write(data));
        }
        server.Close();
      });
    }

    void SpawnSenderServer(const std::vector<std::string_view>& write_data) {
      server_thread = std::thread([this, write_data]() {
        server.Accept();
        for (auto data : write_data) {
          ASSERT_EQ(data.length(), server.Send(data));
        }
        server.Close();
      });
    }

    void JoinThreads() {
      server_thread.join();
      client_thread.join();
    }

    TCPSocket& Server() { return server; }
    TCPSocket& Client() { return client; }

   private:
    TCPSocket client;
    TCPSocket server;

    std::thread client_thread;
    std::thread server_thread;
  };

  void ConfigureCapture(uint32_t protocol, uint64_t mask) {
    auto* socket_trace_connector = dynamic_cast<SocketTraceConnector*>(source.get());
    ASSERT_OK(socket_trace_connector->Configure(protocol, mask));
  }

  static constexpr std::string_view kHTTPReqMsg1 = R"(GET /endpoint1 HTTP/1.1
User-Agent: Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:67.0) Gecko/20100101 Firefox/67.0

)";

  static constexpr std::string_view kHTTPReqMsg2 = R"(GET /endpoint2 HTTP/1.1
User-Agent: Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:67.0) Gecko/20100101 Firefox/67.0

)";

  static constexpr std::string_view kHTTPRespMsg1 = R"(HTTP/1.1 200 OK
Content-Type: application/json; msg1
Content-Length: 0

)";

  static constexpr std::string_view kHTTPRespMsg2 = R"(HTTP/1.1 200 OK
Content-Type: application/json; msg2
Content-Length: 0

)";

  static constexpr std::string_view kNoProtocolMsg = R"(This is not an HTTP message)";

  static constexpr std::string_view kMySQLMsg = "\x16SELECT column FROM table";

  static constexpr int kHTTPTableNum = SocketTraceConnector::kHTTPTableNum;
  static constexpr DataTableSchema kHTTPTable = SocketTraceConnector::kHTTPTable;
  static constexpr uint32_t kHTTPMajorVersionIdx = kHTTPTable.ColIndex("http_major_version");
  static constexpr uint32_t kHTTPContentTypeIdx = kHTTPTable.ColIndex("http_content_type");
  static constexpr uint32_t kHTTPHeaderIdx = kHTTPTable.ColIndex("http_headers");
  static constexpr uint32_t kHTTPPIDIdx = kHTTPTable.ColIndex("pid");
  static constexpr uint32_t kHTTPRemoteAddrIdx = kHTTPTable.ColIndex("remote_addr");
  static constexpr uint32_t kHTTPFdIdx = kHTTPTable.ColIndex("fd");
  static constexpr uint32_t kHTTPStartTimeIdx = kHTTPTable.ColIndex("pid_start_time");

  static constexpr int kMySQLTableNum = SocketTraceConnector::kMySQLTableNum;
  static constexpr DataTableSchema kMySQLTable = SocketTraceConnector::kMySQLTable;
  static constexpr uint32_t kMySQLBodyIdx = kMySQLTable.ColIndex("body");

  std::unique_ptr<SourceConnector> source;
};

TEST_F(HTTPTraceBPFTest, TestWriteRespCapture) {
  ConfigureCapture(kProtocolHTTP, kSocketTraceSendResp);

  ClientServerSystem system;
  system.RunWriterReader({kHTTPRespMsg1, kHTTPRespMsg2});

  {
    types::ColumnWrapperRecordBatch record_batch;
    InitRecordBatch(kHTTPTable.elements(), /*target_capacity*/ 4, &record_batch);
    source->TransferData(kHTTPTableNum, &record_batch);

    for (const std::shared_ptr<ColumnWrapper>& col : record_batch) {
      ASSERT_EQ(2, col->Size());
    }

    // These getpid() EXPECTs require docker container with --pid=host so that the container's PID
    // and the host machine are identical. See
    // https://stackoverflow.com/questions/33328841/pid-mapping-between-docker-and-host

    EXPECT_EQ(getpid(), record_batch[kHTTPPIDIdx]->Get<types::Int64Value>(0).val);
    EXPECT_EQ(std::string_view("Content-Length: 0\nContent-Type: application/json; msg1"),
              record_batch[kHTTPHeaderIdx]->Get<types::StringValue>(0));
    EXPECT_EQ(system.Server().sockfd(), record_batch[kHTTPFdIdx]->Get<types::Int64Value>(0).val);
    EXPECT_EQ("127.0.0.1", record_batch[kHTTPRemoteAddrIdx]->Get<types::StringValue>(0));

    EXPECT_EQ(getpid(), record_batch[kHTTPPIDIdx]->Get<types::Int64Value>(1).val);
    EXPECT_EQ(std::string_view("Content-Length: 0\nContent-Type: application/json; msg2"),
              record_batch[kHTTPHeaderIdx]->Get<types::StringValue>(1));
    EXPECT_EQ(system.Server().sockfd(), record_batch[kHTTPFdIdx]->Get<types::Int64Value>(1).val);
    EXPECT_EQ("127.0.0.1", record_batch[kHTTPRemoteAddrIdx]->Get<types::StringValue>(1));

    // Additional verifications. These are common to all HTTP1.x tracing, so we decide to not
    // duplicate them on all relevant tests.
    EXPECT_EQ(1, record_batch[kHTTPMajorVersionIdx]->Get<types::Int64Value>(0).val);
    EXPECT_EQ(static_cast<uint64_t>(HTTPContentType::kJSON),
              record_batch[kHTTPContentTypeIdx]->Get<types::Int64Value>(0).val);
    EXPECT_EQ(1, record_batch[kHTTPMajorVersionIdx]->Get<types::Int64Value>(1).val);
    EXPECT_EQ(static_cast<uint64_t>(HTTPContentType::kJSON),
              record_batch[kHTTPContentTypeIdx]->Get<types::Int64Value>(1).val);
  }

  // Check that MySQL table did not capture any data.
  {
    types::ColumnWrapperRecordBatch record_batch;
    InitRecordBatch(kMySQLTable.elements(), /*target_capacity*/ 2, &record_batch);
    source->TransferData(kMySQLTableNum, &record_batch);

    for (const std::shared_ptr<ColumnWrapper>& col : record_batch) {
      ASSERT_EQ(0, col->Size());
    }
  }

  EXPECT_OK(source->Stop());
}

TEST_F(HTTPTraceBPFTest, TestSendRespCapture) {
  ConfigureCapture(kProtocolHTTP, kSocketTraceSendResp);

  ClientServerSystem system;
  system.RunSenderReceiver({kHTTPRespMsg1, kHTTPRespMsg2});

  {
    types::ColumnWrapperRecordBatch record_batch;
    InitRecordBatch(kHTTPTable.elements(), /*target_capacity*/ 2, &record_batch);
    source->TransferData(kHTTPTableNum, &record_batch);

    for (const std::shared_ptr<ColumnWrapper>& col : record_batch) {
      ASSERT_EQ(2, col->Size());
    }

    // These 2 EXPECTs require docker container with --pid=host so that the container's PID and the
    // host machine are identical.
    // See https://stackoverflow.com/questions/33328841/pid-mapping-between-docker-and-host

    EXPECT_EQ(getpid(), record_batch[kHTTPPIDIdx]->Get<types::Int64Value>(0).val);
    EXPECT_EQ(std::string_view("Content-Length: 0\nContent-Type: application/json; msg1"),
              record_batch[kHTTPHeaderIdx]->Get<types::StringValue>(0));
    EXPECT_EQ(system.Server().sockfd(), record_batch[kHTTPFdIdx]->Get<types::Int64Value>(0).val);

    EXPECT_EQ(getpid(), record_batch[kHTTPPIDIdx]->Get<types::Int64Value>(1).val);
    EXPECT_EQ(std::string_view("Content-Length: 0\nContent-Type: application/json; msg2"),
              record_batch[kHTTPHeaderIdx]->Get<types::StringValue>(1));
    EXPECT_EQ(system.Server().sockfd(), record_batch[kHTTPFdIdx]->Get<types::Int64Value>(1).val);
  }

  // Check that MySQL table did not capture any data.
  {
    types::ColumnWrapperRecordBatch record_batch;
    InitRecordBatch(kMySQLTable.elements(), /*target_capacity*/ 2, &record_batch);
    source->TransferData(kMySQLTableNum, &record_batch);

    for (const std::shared_ptr<ColumnWrapper>& col : record_batch) {
      ASSERT_EQ(0, col->Size());
    }
  }

  EXPECT_OK(source->Stop());
}

TEST_F(HTTPTraceBPFTest, TestReadRespCapture) {
  ConfigureCapture(kProtocolHTTP, kSocketTraceRecvResp);

  ClientServerSystem system;
  system.RunWriterReader({kHTTPRespMsg1, kHTTPRespMsg2});

  {
    types::ColumnWrapperRecordBatch record_batch;
    InitRecordBatch(kHTTPTable.elements(), /*target_capacity*/ 4, &record_batch);
    source->TransferData(kHTTPTableNum, &record_batch);

    for (const std::shared_ptr<ColumnWrapper>& col : record_batch) {
      ASSERT_EQ(2, col->Size());
    }

    // These 2 EXPECTs require docker container with --pid=host so that the container's PID and the
    // host machine are identical.
    // See https://stackoverflow.com/questions/33328841/pid-mapping-between-docker-and-host

    EXPECT_EQ(getpid(), record_batch[kHTTPPIDIdx]->Get<types::Int64Value>(0).val);
    EXPECT_EQ(std::string_view("Content-Length: 0\nContent-Type: application/json; msg1"),
              record_batch[kHTTPHeaderIdx]->Get<types::StringValue>(0));
    EXPECT_EQ(system.Client().sockfd(), record_batch[kHTTPFdIdx]->Get<types::Int64Value>(0).val);

    EXPECT_EQ(getpid(), record_batch[kHTTPPIDIdx]->Get<types::Int64Value>(1).val);
    EXPECT_EQ(std::string_view("Content-Length: 0\nContent-Type: application/json; msg2"),
              record_batch[kHTTPHeaderIdx]->Get<types::StringValue>(1));
    EXPECT_EQ(system.Client().sockfd(), record_batch[kHTTPFdIdx]->Get<types::Int64Value>(1).val);
  }

  // Check that MySQL table did not capture any data.
  {
    types::ColumnWrapperRecordBatch record_batch;
    InitRecordBatch(kMySQLTable.elements(), /*target_capacity*/ 2, &record_batch);
    source->TransferData(kMySQLTableNum, &record_batch);

    for (const std::shared_ptr<ColumnWrapper>& col : record_batch) {
      ASSERT_EQ(0, col->Size());
    }
  }

  EXPECT_OK(source->Stop());
}

TEST_F(HTTPTraceBPFTest, TestRecvRespCapture) {
  ConfigureCapture(kProtocolHTTP, kSocketTraceRecvResp);

  ClientServerSystem system;
  system.RunSenderReceiver({kHTTPRespMsg1, kHTTPRespMsg2});

  {
    types::ColumnWrapperRecordBatch record_batch;
    InitRecordBatch(kHTTPTable.elements(), /*target_capacity*/ 4, &record_batch);
    source->TransferData(kHTTPTableNum, &record_batch);

    for (const std::shared_ptr<ColumnWrapper>& col : record_batch) {
      ASSERT_EQ(2, col->Size());
    }

    // These 2 EXPECTs require docker container with --pid=host so that the container's PID and the
    // host machine are identical.
    // See https://stackoverflow.com/questions/33328841/pid-mapping-between-docker-and-host

    EXPECT_EQ(getpid(), record_batch[kHTTPPIDIdx]->Get<types::Int64Value>(0).val);
    EXPECT_EQ(std::string_view("Content-Length: 0\nContent-Type: application/json; msg1"),
              record_batch[kHTTPHeaderIdx]->Get<types::StringValue>(0));
    EXPECT_EQ(system.Client().sockfd(), record_batch[kHTTPFdIdx]->Get<types::Int64Value>(0).val);

    EXPECT_EQ(getpid(), record_batch[kHTTPPIDIdx]->Get<types::Int64Value>(1).val);
    EXPECT_EQ(std::string_view("Content-Length: 0\nContent-Type: application/json; msg2"),
              record_batch[kHTTPHeaderIdx]->Get<types::StringValue>(1));
    EXPECT_EQ(system.Client().sockfd(), record_batch[kHTTPFdIdx]->Get<types::Int64Value>(1).val);
  }

  // Check that MySQL table did not capture any data.
  {
    types::ColumnWrapperRecordBatch record_batch;
    InitRecordBatch(kMySQLTable.elements(), /*target_capacity*/ 2, &record_batch);
    source->TransferData(kMySQLTableNum, &record_batch);

    for (const std::shared_ptr<ColumnWrapper>& col : record_batch) {
      ASSERT_EQ(0, col->Size());
    }
  }

  EXPECT_OK(source->Stop());
}

TEST_F(HTTPTraceBPFTest, TestMySQLWriteCapture) {
  ClientServerSystem system;
  system.RunSenderReceiver({kMySQLMsg, kMySQLMsg});

  // Check that HTTP table did not capture any data.
  {
    types::ColumnWrapperRecordBatch record_batch;
    InitRecordBatch(kHTTPTable.elements(), /*target_capacity*/ 2, &record_batch);
    source->TransferData(kHTTPTableNum, &record_batch);

    for (const std::shared_ptr<ColumnWrapper>& col : record_batch) {
      ASSERT_EQ(0, col->Size());
    }
  }

  // Check that MySQL table did capture the appropriate data.
  {
    types::ColumnWrapperRecordBatch record_batch;
    InitRecordBatch(kMySQLTable.elements(), /*target_capacity*/ 2, &record_batch);
    source->TransferData(kMySQLTableNum, &record_batch);

    for (const std::shared_ptr<ColumnWrapper>& col : record_batch) {
      ASSERT_EQ(2, col->Size());
    }

    EXPECT_EQ(std::string_view("\x16SELECT column FROM table"),
              record_batch[kMySQLBodyIdx]->Get<types::StringValue>(0));
    EXPECT_EQ(std::string_view("\x16SELECT column FROM table"),
              record_batch[kMySQLBodyIdx]->Get<types::StringValue>(1));
  }

  EXPECT_OK(source->Stop());
}

TEST_F(HTTPTraceBPFTest, TestNoProtocolWritesNotCaptured) {
  ConfigureCapture(kProtocolHTTP, kSocketTraceSendReq | kSocketTraceRecvReq);
  ConfigureCapture(kProtocolHTTP, kSocketTraceRecvResp | kSocketTraceSendResp);
  ConfigureCapture(kProtocolMySQL, kSocketTraceSendReq | kSocketTraceRecvResp);

  ClientServerSystem system;
  system.RunWriterReader({kNoProtocolMsg, "", kNoProtocolMsg, ""});

  // Check that HTTP table did not capture any data.
  {
    types::ColumnWrapperRecordBatch record_batch;
    InitRecordBatch(kHTTPTable.elements(), /*target_capacity*/ 2, &record_batch);
    source->TransferData(kHTTPTableNum, &record_batch);

    // Should not have captured anything.
    for (const std::shared_ptr<ColumnWrapper>& col : record_batch) {
      ASSERT_EQ(0, col->Size());
    }
  }

  // Check that MySQL table did not capture any data.
  {
    types::ColumnWrapperRecordBatch record_batch;
    InitRecordBatch(kMySQLTable.elements(), /*target_capacity*/ 2, &record_batch);
    source->TransferData(kMySQLTableNum, &record_batch);

    // Should not have captured anything.
    for (const std::shared_ptr<ColumnWrapper>& col : record_batch) {
      ASSERT_EQ(0, col->Size());
    }
  }

  EXPECT_OK(source->Stop());
}

TEST_F(HTTPTraceBPFTest, TestMultipleConnections) {
  ConfigureCapture(kProtocolHTTP, kSocketTraceRecvResp);

  // Two separate connections.
  ClientServerSystem system1;
  system1.RunWriterReader({kHTTPRespMsg1});

  ClientServerSystem system2;
  system2.RunWriterReader({kHTTPRespMsg2});

  {
    types::ColumnWrapperRecordBatch record_batch;
    InitRecordBatch(kHTTPTable.elements(), /*target_capacity*/ 4, &record_batch);
    source->TransferData(kHTTPTableNum, &record_batch);

    for (const std::shared_ptr<ColumnWrapper>& col : record_batch) {
      ASSERT_EQ(2, col->Size());
    }

    std::vector<std::tuple<int64_t, std::string, int64_t>> results;
    for (int i = 0; i < 2; ++i) {
      results.emplace_back(
          std::make_tuple(record_batch[kHTTPPIDIdx]->Get<types::Int64Value>(i).val,
                          record_batch[kHTTPHeaderIdx]->Get<types::StringValue>(i),
                          record_batch[kHTTPFdIdx]->Get<types::Int64Value>(i).val));
    }

    EXPECT_THAT(
        results,
        UnorderedElementsAre(
            std::make_tuple(getpid(), "Content-Length: 0\nContent-Type: application/json; msg1",
                            system1.Client().sockfd()),
            std::make_tuple(getpid(), "Content-Length: 0\nContent-Type: application/json; msg2",
                            system2.Client().sockfd())));
  }
}

TEST_F(HTTPTraceBPFTest, TestStartTime) {
  ConfigureCapture(kProtocolHTTP, kSocketTraceRecvResp);

  ClientServerSystem system;
  system.RunSenderReceiver({kHTTPRespMsg1, kHTTPRespMsg2});

  // Kernel uses monotonic clock as start_time, so we must do the same.
  auto now = std::chrono::steady_clock::now();

  // Use a time window to make sure the recorded PID start_time is right.
  // Being super generous with the window, just in case test runs slow.
  auto time_window_start = now - std::chrono::minutes(30);
  auto time_window_end = now + std::chrono::minutes(5);

  types::ColumnWrapperRecordBatch record_batch;
  InitRecordBatch(kHTTPTable.elements(), /*target_capacity*/ 4, &record_batch);
  source->TransferData(kHTTPTableNum, &record_batch);

  ASSERT_EQ(2, record_batch[0]->Size());

  EXPECT_EQ(getpid(), record_batch[kHTTPPIDIdx]->Get<types::Int64Value>(0).val);
  EXPECT_LT(time_window_start.time_since_epoch().count(),
            record_batch[kHTTPStartTimeIdx]->Get<types::Int64Value>(0).val);
  EXPECT_GT(time_window_end.time_since_epoch().count(),
            record_batch[kHTTPStartTimeIdx]->Get<types::Int64Value>(0).val);

  EXPECT_EQ(getpid(), record_batch[kHTTPPIDIdx]->Get<types::Int64Value>(1).val);
  EXPECT_LT(time_window_start.time_since_epoch().count(),
            record_batch[kHTTPStartTimeIdx]->Get<types::Int64Value>(1).val);
  EXPECT_GT(time_window_end.time_since_epoch().count(),
            record_batch[kHTTPStartTimeIdx]->Get<types::Int64Value>(1).val);

  EXPECT_OK(source->Stop());
}

}  // namespace stirling
}  // namespace pl
