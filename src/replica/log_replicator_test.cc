//
// file_appender_test.cc
// Copyright (C) 2017 4paradigm.com
// Author vagrant
// Date 2017-04-21
//

#include "replica/log_replicator.h"
#include "replica/replicate_node.h"
#include <sched.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <gtest/gtest.h>
#include <boost/lexical_cast.hpp>
#include <boost/atomic.hpp>
#include <boost/bind.hpp>
#include <stdio.h>
#include "proto/tablet.pb.h"
#include "logging.h"
#include "thread_pool.h"
#include <sofa/pbrpc/pbrpc.h>
#include "storage/table.h"
#include "storage/segment.h"
#include "storage/ticket.h"
#include "timer.h"

using ::baidu::common::ThreadPool;
using ::rtidb::storage::Table;
using ::rtidb::storage::Ticket;
using ::rtidb::storage::DataBlock;
using ::google::protobuf::RpcController;
using ::google::protobuf::Closure;
using ::baidu::common::INFO;
using ::baidu::common::DEBUG;

namespace rtidb {
namespace replica {

const std::vector<std::string> g_endpoints;

class MockTabletImpl : public ::rtidb::api::TabletServer {

public:
    MockTabletImpl(const ReplicatorRole& role,
                   const std::string& path,
                   const std::vector<std::string>& endpoints,
                   Table* table): role_(role),
    path_(path), endpoints_(endpoints), 
    replicator_(path_, endpoints_, role_, table) {
    }

    ~MockTabletImpl() {
        replicator_.Stop();
    }
    bool Init() {
        //table_ = new Table("test", 1, 1, 8, 0, false, g_endpoints);
        //table_->Init();
        return replicator_.Init();
    }

    void Put(RpcController* controller,
             const ::rtidb::api::PutRequest* request,
             ::rtidb::api::PutResponse* response,
             Closure* done) {}

    void Scan(RpcController* controller,
              const ::rtidb::api::ScanRequest* request,
              ::rtidb::api::ScanResponse* response,
              Closure* done) {}

    void CreateTable(RpcController* controller,
            const ::rtidb::api::CreateTableRequest* request,
            ::rtidb::api::CreateTableResponse* response,
            Closure* done) {}

    void DropTable(RpcController* controller,
            const ::rtidb::api::DropTableRequest* request,
            ::rtidb::api::DropTableResponse* response,
            Closure* done) {}

    void AppendEntries(RpcController* controller,
            const ::rtidb::api::AppendEntriesRequest* request,
            ::rtidb::api::AppendEntriesResponse* response,
            Closure* done) {
        bool ok = replicator_.AppendEntries(request, response);
        if (ok) {
            LOG(INFO, "receive log entry from leader ok");
            response->set_code(0);
        }else {
            LOG(INFO, "receive log entry from leader error");
            response->set_code(1);
        }
        done->Run();
        replicator_.Notify();
    }

private:
    ReplicatorRole role_;
    std::string path_;
    std::vector<std::string> endpoints_;
    LogReplicator replicator_;
};

bool ReceiveEntry(const ::rtidb::api::LogEntry& entry) {
    return true;
}

class LogReplicatorTest : public ::testing::Test {

public:
    LogReplicatorTest() {}

    ~LogReplicatorTest() {}
};

inline std::string GenRand() {
    return boost::lexical_cast<std::string>(rand() % 10000000 + 1);
}

bool StartRpcServe(MockTabletImpl* tablet,
        const std::string& endpoint) {
    sofa::pbrpc::RpcServerOptions options;
    sofa::pbrpc::RpcServer rpc_server(options);
    if (!rpc_server.RegisterService(tablet)) {
        return false;
    }
    bool ok =rpc_server.Start(endpoint);
    if (ok) {
        LOG(INFO, "register service ok");
    }else {
        LOG(WARNING, "fail to start service");
    }
    return ok;
}

TEST_F(LogReplicatorTest, Init) {
    std::vector<std::string> endpoints;
    std::string folder = "/tmp/" + GenRand() + "/";
    Table* table = new Table("test", 1, 1, 8, 0, false, g_endpoints);
    table->Init();
    LogReplicator replicator(folder, endpoints, kLeaderNode, table);
    bool ok = replicator.Init();
    ASSERT_TRUE(ok);
    replicator.Stop();
}

TEST_F(LogReplicatorTest, BenchMark) {
    std::vector<std::string> endpoints;
    std::string folder = "/tmp/" + GenRand() + "/";
    Table* table = new Table("test", 1, 1, 8, 0, false, g_endpoints);
    table->Init();
    LogReplicator replicator(folder, endpoints, kLeaderNode, table);
    bool ok = replicator.Init();
    ::rtidb::api::LogEntry entry;
    entry.set_term(1);
    entry.set_pk("test");
    entry.set_value("test");
    entry.set_ts(9527);
    ok = replicator.AppendEntry(entry);
    ASSERT_TRUE(ok);
    replicator.Stop();
}


TEST_F(LogReplicatorTest, LeaderAndFollower) {
    sofa::pbrpc::RpcServerOptions options;
    sofa::pbrpc::RpcServer rpc_server0(options);
    sofa::pbrpc::RpcServer rpc_server1(options);
    Table* t7 = new Table("test", 1, 1, 8, 0, false, g_endpoints);
    t7->Init();
    {
        std::string follower_addr = "127.0.0.1:18527";
        std::string folder = "/tmp/" + GenRand() + "/";
        MockTabletImpl* follower = new MockTabletImpl(kFollowerNode, 
                folder, g_endpoints, t7);
        bool ok = follower->Init();
        ASSERT_TRUE(ok);
        if (!rpc_server1.RegisterService(follower)) {
            ASSERT_TRUE(false);
        }
        ok =rpc_server1.Start(follower_addr);
        ASSERT_TRUE(ok);
        LOG(INFO, "start follower");
    }

    std::vector<std::string> endpoints;
    endpoints.push_back("127.0.0.1:18527");
    std::string folder = "/tmp/" + GenRand() + "/";
    LogReplicator leader(folder, g_endpoints, kLeaderNode, t7);
    bool ok = leader.Init();
    ASSERT_TRUE(ok);
    ::rtidb::api::LogEntry entry;
    entry.set_pk("test_pk");
    entry.set_value("value1");
    entry.set_ts(9527);
    ok = leader.AppendEntry(entry);
    entry.set_value("value2");
    entry.set_ts(9526);
    ok = leader.AppendEntry(entry);
    entry.set_value("value3");
    entry.set_ts(9525);
    ok = leader.AppendEntry(entry);
    entry.set_value("value4");
    entry.set_ts(9524);
    ok = leader.AppendEntry(entry);
    ASSERT_TRUE(ok);
    leader.Notify();
    leader.AddReplicateNode("127.0.0.1:18528");
    sleep(2);
    Table* t8 = new Table("test", 1, 1, 8, 0, false, g_endpoints);;
    t8->Init();
    {
        std::string follower_addr = "127.0.0.1:18528";
        std::string folder = "/tmp/" + GenRand() + "/";
        MockTabletImpl* follower = new MockTabletImpl(kFollowerNode, 
                folder, g_endpoints, t8);
        bool ok = follower->Init();
        ASSERT_TRUE(ok);
        if (!rpc_server0.RegisterService(follower)) {
            ASSERT_TRUE(false);
        }
        ok =rpc_server0.Start(follower_addr);
        ASSERT_TRUE(ok);
        LOG(INFO, "start follower");
    }
    sleep(4);
    leader.Stop();
    {
        Ticket ticket;
        // check 18527
        Table::Iterator* it = t8->NewIterator("test_pk", ticket);
        it->Seek(9527);
        ASSERT_TRUE(it->Valid());
        DataBlock* value = it->GetValue();
        std::string value_str(value->data, value->size);
        ASSERT_EQ("value1", value_str);
        ASSERT_EQ(9527, it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str1(value->data, value->size);
        ASSERT_EQ("value2", value_str1);
        ASSERT_EQ(9526, it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str2(value->data, value->size);
        ASSERT_EQ("value3", value_str2);
        ASSERT_EQ(9525, it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str3(value->data, value->size);
        ASSERT_EQ("value4", value_str3);
        ASSERT_EQ(9524, it->GetKey());
    }
}

}
}

int main(int argc, char** argv) {
    srand (time(NULL));
    ::baidu::common::SetLogLevel(::baidu::common::DEBUG);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

