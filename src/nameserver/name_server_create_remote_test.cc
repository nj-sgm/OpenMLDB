//
// name_server_create_remote_test.cc
// Copyright (C) 2020 4paradigm.com
// Author wangbao
// Date 2020-01-13
//

#include <brpc/server.h>
#include <gflags/gflags.h>
#include <sched.h>
#include <unistd.h>
#include "base/file_util.h"
#include "client/ns_client.h"
#include "gtest/gtest.h"
#include "logging.h" // NOLINT
#include "nameserver/name_server_impl.h"
#include "proto/name_server.pb.h"
#include "proto/tablet.pb.h"
#include "rpc/rpc_client.h"
#include "tablet/tablet_impl.h"
#include "timer.h" // NOLINT

DECLARE_string(endpoint);
DECLARE_string(db_root_path);
DECLARE_string(zk_cluster);
DECLARE_string(zk_root_path);
DECLARE_int32(zk_session_timeout);
DECLARE_int32(request_timeout_ms);
DECLARE_int32(zk_keep_alive_check_interval);
DECLARE_int32(make_snapshot_threshold_offset);
DECLARE_uint32(name_server_task_max_concurrency);
DECLARE_bool(auto_failover);

using ::rtidb::zk::ZkClient;

namespace rtidb {
namespace nameserver {

inline std::string GenRand() { return std::to_string(rand() % 10000000 + 1); } // NOLINT

class MockClosure : public ::google::protobuf::Closure {
 public:
    MockClosure() {}
    ~MockClosure() {}
    void Run() {}
};
class NameServerImplRemoteTest : public ::testing::Test {
 public:
    NameServerImplRemoteTest() {}
    ~NameServerImplRemoteTest() {}
    void Start(NameServerImpl* nameserver) { nameserver->running_ = true; }
    std::vector<std::list<std::shared_ptr<OPData>>>& GetTaskVec(
        NameServerImpl* nameserver) {
        return nameserver->task_vec_;
    }
    std::map<std::string, std::shared_ptr<::rtidb::nameserver::TableInfo>>&
    GetTableInfo(NameServerImpl* nameserver) {
        return nameserver->table_info_;
    }
    ZoneInfo& GetZoneInfo(NameServerImpl* nameserver) {
        return nameserver->zone_info_;
    }
};

void StartNameServer(brpc::Server& server, NameServerImpl* nameserver) { // NOLINT
    bool ok = nameserver->Init();
    ASSERT_TRUE(ok);
    sleep(4);
    brpc::ServerOptions options;
    if (server.AddService(nameserver, brpc::SERVER_OWNS_SERVICE) != 0) {
        PDLOG(WARNING, "Fail to add service");
        exit(1);
    }
    if (server.Start(FLAGS_endpoint.c_str(), &options) != 0) {
        PDLOG(WARNING, "Fail to start server");
        exit(1);
    }
}

void StartNameServer(brpc::Server& server) { // NOLINT
    NameServerImpl* nameserver = new NameServerImpl();
    bool ok = nameserver->Init();
    ASSERT_TRUE(ok);
    sleep(4);
    brpc::ServerOptions options;
    if (server.AddService(nameserver, brpc::SERVER_OWNS_SERVICE) != 0) {
        PDLOG(WARNING, "Fail to add service");
        exit(1);
    }
    if (server.Start(FLAGS_endpoint.c_str(), &options) != 0) {
        PDLOG(WARNING, "Fail to start server");
        exit(1);
    }
}

void StartTablet(brpc::Server& server) { // NOLINT
    ::rtidb::tablet::TabletImpl* tablet = new ::rtidb::tablet::TabletImpl();
    bool ok = tablet->Init();
    ASSERT_TRUE(ok);
    sleep(2);
    brpc::ServerOptions options1;
    if (server.AddService(tablet, brpc::SERVER_OWNS_SERVICE) != 0) {
        PDLOG(WARNING, "Fail to add service");
        exit(1);
    }
    if (server.Start(FLAGS_endpoint.c_str(), &options1) != 0) {
        PDLOG(WARNING, "Fail to start server");
        exit(1);
    }
    ok = tablet->RegisterZK();
    ASSERT_TRUE(ok);
    sleep(2);
}

TEST_F(NameServerImplRemoteTest, CreateTableRemoteBeforeAddRepCluster) {
    // local ns and tablet
    // ns
    FLAGS_zk_cluster = "127.0.0.1:6181";
    FLAGS_zk_root_path = "/rtidb3" + GenRand();
    FLAGS_endpoint = "127.0.0.1:9631";
    FLAGS_db_root_path = "/tmp/" + ::rtidb::nameserver::GenRand();

    NameServerImpl* nameserver_1 = new NameServerImpl();
    brpc::Server server;
    StartNameServer(server, nameserver_1);
    ::rtidb::RpcClient<::rtidb::nameserver::NameServer_Stub>
        name_server_client_1(FLAGS_endpoint);
    name_server_client_1.Init();

    // tablet
    FLAGS_endpoint = "127.0.0.1:9931";
    brpc::Server server1;
    StartTablet(server1);

    // remote ns and tablet
    // ns
    FLAGS_zk_cluster = "127.0.0.1:6181";
    FLAGS_zk_root_path = "/rtidb3" + GenRand();
    FLAGS_endpoint = "127.0.0.1:9632";
    FLAGS_db_root_path = "/tmp/" + ::rtidb::nameserver::GenRand();

    NameServerImpl* nameserver_2 = new NameServerImpl();
    brpc::Server server2;
    StartNameServer(server2, nameserver_2);
    ::rtidb::RpcClient<::rtidb::nameserver::NameServer_Stub>
        name_server_client_2(FLAGS_endpoint);
    name_server_client_2.Init();

    // tablet
    FLAGS_endpoint = "127.0.0.1:9932";
    brpc::Server server3;
    StartTablet(server3);
    bool ok = false;
    std::string name = "test" + GenRand();
    {
        CreateTableRequest request;
        GeneralResponse response;
        TableInfo* table_info = request.mutable_table_info();
        table_info->set_name(name);
        TablePartition* partion = table_info->add_table_partition();
        partion->set_pid(1);
        PartitionMeta* meta = partion->add_partition_meta();
        meta->set_endpoint("127.0.0.1:9931");
        meta->set_is_leader(true);
        TablePartition* partion1 = table_info->add_table_partition();
        partion1->set_pid(2);
        PartitionMeta* meta1 = partion1->add_partition_meta();
        meta1->set_endpoint("127.0.0.1:9931");
        meta1->set_is_leader(true);
        ok = name_server_client_1.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::CreateTable, &request,
            &response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(307, response.code());

        TablePartition* partion2 = table_info->add_table_partition();
        partion2->set_pid(0);
        PartitionMeta* meta2 = partion2->add_partition_meta();
        meta2->set_endpoint("127.0.0.1:9931");
        meta2->set_is_leader(true);
        ok = name_server_client_1.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::CreateTable, &request,
            &response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        sleep(3);
    }
    {
        ::rtidb::nameserver::ShowTableRequest request;
        ::rtidb::nameserver::ShowTableResponse response;
        ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::ShowTable, &request,
            &response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(0, response.table_info_size());
    }
    {
        ::rtidb::nameserver::SwitchModeRequest request;
        ::rtidb::nameserver::GeneralResponse response;
        request.set_sm(kLEADER);
        ok = name_server_client_1.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::SwitchMode, &request,
            &response, FLAGS_request_timeout_ms, 1);
    }
    {
        std::string alias = "remote";
        std::string msg;
        ::rtidb::nameserver::ClusterAddress add_request;
        ::rtidb::nameserver::GeneralResponse add_response;
        add_request.set_alias(alias);
        add_request.set_zk_path(FLAGS_zk_root_path);
        add_request.set_zk_endpoints(FLAGS_zk_cluster);
        ok = name_server_client_1.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::AddReplicaCluster,
            &add_request, &add_response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, add_response.code());
        sleep(20);
    }
    {
        ::rtidb::nameserver::ShowTableRequest request;
        ::rtidb::nameserver::ShowTableResponse response;
        ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::ShowTable, &request,
            &response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(1, response.table_info_size());
        ASSERT_EQ(name, response.table_info(0).name());
        ASSERT_EQ(3, response.table_info(0).table_partition_size());
    }
    std::map<std::string, std::shared_ptr<::rtidb::nameserver::TableInfo>>&
        table_info_map_r = GetTableInfo(nameserver_2);
    uint32_t rtid = 0;
    for (const auto& table_info : table_info_map_r) {
        if (table_info.second->name() == name) {
            rtid = table_info.second->tid();
            for (const auto& table_partition :
                 table_info.second->table_partition()) {
                if (table_partition.pid() == 1) {
                    ASSERT_EQ(0, table_partition.remote_partition_meta_size());
                }
            }
            break;
        }
    }
    std::map<std::string, std::shared_ptr<::rtidb::nameserver::TableInfo>>&
        table_info_map = GetTableInfo(nameserver_1);
    for (const auto& table_info : table_info_map) {
        if (table_info.second->name() == name) {
            for (const auto& table_partition :
                 table_info.second->table_partition()) {
                if (table_partition.pid() == 1) {
                    for (const auto& meta :
                         table_partition.remote_partition_meta()) {
                        ASSERT_EQ(rtid, meta.remote_tid());
                        ASSERT_EQ("remote", meta.alias());
                    }
                    break;
                }
            }
            break;
        }
    }
    {
        ::rtidb::nameserver::DropTableRequest request;
        request.set_name(name);
        ::rtidb::nameserver::GeneralResponse response;
        bool ok = name_server_client_1.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::DropTable, &request,
            &response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        sleep(5);
    }
    {
        ::rtidb::nameserver::ShowTableRequest request;
        ::rtidb::nameserver::ShowTableResponse response;
        ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::ShowTable, &request,
            &response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(0, response.table_info_size());
    }
}

TEST_F(NameServerImplRemoteTest, CreateAndDropTableRemote) {
    // local ns and tablet
    // ns
    FLAGS_zk_cluster = "127.0.0.1:6181";
    FLAGS_zk_root_path = "/rtidb3" + GenRand();
    FLAGS_endpoint = "127.0.0.1:9631";
    FLAGS_db_root_path = "/tmp/" + ::rtidb::nameserver::GenRand();

    NameServerImpl* nameserver_1 = new NameServerImpl();
    brpc::Server server;
    StartNameServer(server, nameserver_1);
    ::rtidb::RpcClient<::rtidb::nameserver::NameServer_Stub>
        name_server_client_1(FLAGS_endpoint);
    name_server_client_1.Init();

    // tablet
    FLAGS_endpoint = "127.0.0.1:9931";
    brpc::Server server1;
    StartTablet(server1);

    // remote ns and tablet
    // ns
    FLAGS_zk_cluster = "127.0.0.1:6181";
    FLAGS_zk_root_path = "/rtidb3" + GenRand();
    FLAGS_endpoint = "127.0.0.1:9632";
    FLAGS_db_root_path = "/tmp/" + ::rtidb::nameserver::GenRand();

    NameServerImpl* nameserver_2 = new NameServerImpl();
    brpc::Server server2;
    StartNameServer(server2, nameserver_2);
    ::rtidb::RpcClient<::rtidb::nameserver::NameServer_Stub>
        name_server_client_2(FLAGS_endpoint);
    name_server_client_2.Init();

    // tablet
    FLAGS_endpoint = "127.0.0.1:9932";
    brpc::Server server3;
    StartTablet(server3);
    bool ok = false;
    {
        ::rtidb::nameserver::SwitchModeRequest request;
        ::rtidb::nameserver::GeneralResponse response;
        request.set_sm(kLEADER);
        ok = name_server_client_1.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::SwitchMode, &request,
            &response, FLAGS_request_timeout_ms, 1);
    }
    {
        std::string alias = "remote";
        std::string msg;
        ::rtidb::nameserver::ClusterAddress add_request;
        ::rtidb::nameserver::GeneralResponse add_response;
        add_request.set_alias(alias);
        add_request.set_zk_path(FLAGS_zk_root_path);
        add_request.set_zk_endpoints(FLAGS_zk_cluster);
        ok = name_server_client_1.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::AddReplicaCluster,
            &add_request, &add_response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, add_response.code());
        sleep(2);
    }
    std::string name = "test" + GenRand();
    {
        CreateTableRequest request;
        GeneralResponse response;
        TableInfo* table_info = request.mutable_table_info();
        table_info->set_name(name);
        TablePartition* partion = table_info->add_table_partition();
        partion->set_pid(1);
        PartitionMeta* meta = partion->add_partition_meta();
        meta->set_endpoint("127.0.0.1:9931");
        meta->set_is_leader(true);
        TablePartition* partion1 = table_info->add_table_partition();
        partion1->set_pid(2);
        PartitionMeta* meta1 = partion1->add_partition_meta();
        meta1->set_endpoint("127.0.0.1:9931");
        meta1->set_is_leader(true);
        ok = name_server_client_1.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::CreateTable, &request,
            &response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(307, response.code());

        TablePartition* partion2 = table_info->add_table_partition();
        partion2->set_pid(0);
        PartitionMeta* meta2 = partion2->add_partition_meta();
        meta2->set_endpoint("127.0.0.1:9931");
        meta2->set_is_leader(true);
        ok = name_server_client_1.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::CreateTable, &request,
            &response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        sleep(5);
    }
    {
        ::rtidb::nameserver::ShowTableRequest request;
        ::rtidb::nameserver::ShowTableResponse response;
        ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::ShowTable, &request,
            &response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(1, response.table_info_size());
        ASSERT_EQ(name, response.table_info(0).name());
        ASSERT_EQ(3, response.table_info(0).table_partition_size());
    }
    std::map<std::string, std::shared_ptr<::rtidb::nameserver::TableInfo>>&
        table_info_map_r = GetTableInfo(nameserver_2);
    uint32_t rtid = 0;
    for (const auto& table_info : table_info_map_r) {
        if (table_info.second->name() == name) {
            rtid = table_info.second->tid();
            for (const auto& table_partition :
                 table_info.second->table_partition()) {
                if (table_partition.pid() == 1) {
                    ASSERT_EQ(0, table_partition.remote_partition_meta_size());
                }
            }
            break;
        }
    }
    std::map<std::string, std::shared_ptr<::rtidb::nameserver::TableInfo>>&
        table_info_map = GetTableInfo(nameserver_1);
    for (const auto& table_info : table_info_map) {
        if (table_info.second->name() == name) {
            for (const auto& table_partition :
                 table_info.second->table_partition()) {
                if (table_partition.pid() == 1) {
                    for (const auto& meta :
                         table_partition.remote_partition_meta()) {
                        ASSERT_EQ(rtid, meta.remote_tid());
                        ASSERT_EQ("remote", meta.alias());
                    }
                    break;
                }
            }
            break;
        }
    }
    {
        ::rtidb::nameserver::DropTableRequest request;
        request.set_name(name);
        ::rtidb::nameserver::GeneralResponse response;
        bool ok = name_server_client_1.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::DropTable, &request,
            &response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        sleep(5);
    }
    {
        ::rtidb::nameserver::ShowTableRequest request;
        ::rtidb::nameserver::ShowTableResponse response;
        ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::ShowTable, &request,
            &response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(0, response.table_info_size());
    }
}

TEST_F(NameServerImplRemoteTest, CreateTableInfo) {
    // local ns and tablet
    // ns
    FLAGS_zk_cluster = "127.0.0.1:6181";
    FLAGS_zk_root_path = "/rtidb3" + GenRand();
    FLAGS_endpoint = "127.0.0.1:9631";

    brpc::Server server;
    NameServerImpl* nameserver_1 = new NameServerImpl();
    StartNameServer(server);
    ::rtidb::RpcClient<::rtidb::nameserver::NameServer_Stub>
        name_server_client_1(FLAGS_endpoint);
    name_server_client_1.Init();

    // tablet
    FLAGS_endpoint = "127.0.0.1:9931";
    FLAGS_db_root_path = "/tmp/" + ::rtidb::nameserver::GenRand();
    brpc::Server server1;
    StartTablet(server1);

    FLAGS_endpoint = "127.0.0.1:9941";
    FLAGS_db_root_path = "/tmp/" + ::rtidb::nameserver::GenRand();
    brpc::Server server2;
    StartTablet(server2);

    FLAGS_endpoint = "127.0.0.1:9951";
    FLAGS_db_root_path = "/tmp/" + ::rtidb::nameserver::GenRand();
    brpc::Server server3;
    StartTablet(server3);

    // remote ns and tablet
    // ns
    FLAGS_zk_cluster = "127.0.0.1:6181";
    FLAGS_zk_root_path = "/rtidb3" + GenRand();
    FLAGS_endpoint = "127.0.0.1:9632";

    brpc::Server server4;
    StartNameServer(server4);
    ::rtidb::RpcClient<::rtidb::nameserver::NameServer_Stub>
        name_server_client_2(FLAGS_endpoint);
    name_server_client_2.Init();

    // tablet
    FLAGS_endpoint = "127.0.0.1:9932";
    FLAGS_db_root_path = "/tmp/" + ::rtidb::nameserver::GenRand();
    brpc::Server server5;
    StartTablet(server5);

    FLAGS_endpoint = "127.0.0.1:9942";
    FLAGS_db_root_path = "/tmp/" + ::rtidb::nameserver::GenRand();
    brpc::Server server6;
    StartTablet(server6);

    bool ok = false;
    {
        ::rtidb::nameserver::SwitchModeRequest request;
        ::rtidb::nameserver::GeneralResponse response;
        request.set_sm(kLEADER);
        ok = name_server_client_1.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::SwitchMode, &request,
            &response, FLAGS_request_timeout_ms, 1);
    }
    {
        std::string alias = "remote";
        std::string msg;
        ::rtidb::nameserver::ClusterAddress add_request;
        ::rtidb::nameserver::GeneralResponse add_response;
        add_request.set_alias(alias);
        add_request.set_zk_path(FLAGS_zk_root_path);
        add_request.set_zk_endpoints(FLAGS_zk_cluster);
        ok = name_server_client_1.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::AddReplicaCluster,
            &add_request, &add_response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, add_response.code());
        sleep(2);
    }

    ZoneInfo zone_info = GetZoneInfo(nameserver_1);
    std::string name = "test" + GenRand();
    {
        ::rtidb::nameserver::CreateTableInfoRequest request;
        ::rtidb::nameserver::CreateTableInfoResponse response;
        ::rtidb::nameserver::ZoneInfo* zone_info_p =
            request.mutable_zone_info();
        zone_info_p->CopyFrom(zone_info);
        TableInfo* table_info = request.mutable_table_info();
        table_info->set_name(name);

        TablePartition* partion = table_info->add_table_partition();
        partion->set_pid(1);
        PartitionMeta* meta_11 = partion->add_partition_meta();
        meta_11->set_endpoint("127.0.0.1:9931");
        meta_11->set_is_leader(true);
        PartitionMeta* meta_12 = partion->add_partition_meta();
        meta_12->set_endpoint("127.0.0.1:9941");
        meta_12->set_is_leader(false);
        PartitionMeta* meta_13 = partion->add_partition_meta();
        meta_13->set_endpoint("127.0.0.1:9951");
        meta_13->set_is_leader(false);

        TablePartition* partion1 = table_info->add_table_partition();
        partion1->set_pid(2);
        PartitionMeta* meta_21 = partion1->add_partition_meta();
        meta_21->set_endpoint("127.0.0.1:9931");
        meta_21->set_is_leader(true);
        PartitionMeta* meta_22 = partion1->add_partition_meta();
        meta_22->set_endpoint("127.0.0.1:9941");
        meta_22->set_is_leader(false);
        PartitionMeta* meta_23 = partion1->add_partition_meta();
        meta_23->set_endpoint("127.0.0.1:9951");
        meta_23->set_is_leader(false);

        TablePartition* partion2 = table_info->add_table_partition();
        partion2->set_pid(0);
        PartitionMeta* meta_31 = partion2->add_partition_meta();
        meta_31->set_endpoint("127.0.0.1:9931");
        meta_31->set_is_leader(true);
        PartitionMeta* meta_32 = partion2->add_partition_meta();
        meta_32->set_endpoint("127.0.0.1:9941");
        meta_32->set_is_leader(false);
        PartitionMeta* meta_33 = partion2->add_partition_meta();
        meta_33->set_endpoint("127.0.0.1:9951");
        meta_33->set_is_leader(false);

        bool ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::CreateTableInfo, &request,
            &response, FLAGS_request_timeout_ms, 3);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(name, response.table_info().name());
        ASSERT_EQ(3, response.table_info().table_partition_size());
        ASSERT_EQ(2, response.table_info().replica_num());
    }
    {
        ::rtidb::nameserver::ShowTableRequest request;
        ::rtidb::nameserver::ShowTableResponse response;
        request.set_name(name);
        ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::ShowTable, &request,
            &response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(1, response.table_info_size());
        ASSERT_EQ(name, response.table_info(0).name());
        ASSERT_EQ(3, response.table_info(0).table_partition_size());
        ASSERT_EQ(
            1, response.table_info(0).table_partition(0).partition_meta_size());
        ASSERT_EQ(
            1, response.table_info(0).table_partition(1).partition_meta_size());
        ASSERT_EQ(
            1, response.table_info(0).table_partition(2).partition_meta_size());
    }

    name = "test" + GenRand();
    {
        ::rtidb::nameserver::CreateTableInfoRequest request;
        ::rtidb::nameserver::CreateTableInfoResponse response;
        ::rtidb::nameserver::ZoneInfo* zone_info_p =
            request.mutable_zone_info();
        zone_info_p->CopyFrom(zone_info);
        TableInfo* table_info = request.mutable_table_info();
        table_info->set_name(name);

        TablePartition* partion = table_info->add_table_partition();
        partion->set_pid(1);
        PartitionMeta* meta_11 = partion->add_partition_meta();
        meta_11->set_endpoint("127.0.0.1:9931");
        meta_11->set_is_leader(true);

        TablePartition* partion1 = table_info->add_table_partition();
        partion1->set_pid(2);
        PartitionMeta* meta_21 = partion1->add_partition_meta();
        meta_21->set_endpoint("127.0.0.1:9941");
        meta_21->set_is_leader(true);

        TablePartition* partion2 = table_info->add_table_partition();
        partion2->set_pid(0);
        PartitionMeta* meta_31 = partion2->add_partition_meta();
        meta_31->set_endpoint("127.0.0.1:9951");
        meta_31->set_is_leader(true);

        bool ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::CreateTableInfo, &request,
            &response, FLAGS_request_timeout_ms, 3);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(name, response.table_info().name());
        ASSERT_EQ(3, response.table_info().table_partition_size());
        ASSERT_EQ(1, response.table_info().replica_num());
    }
    {
        ::rtidb::nameserver::ShowTableRequest request;
        ::rtidb::nameserver::ShowTableResponse response;
        request.set_name(name);
        ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::ShowTable, &request,
            &response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(1, response.table_info_size());
        ASSERT_EQ(name, response.table_info(0).name());
        ASSERT_EQ(3, response.table_info(0).table_partition_size());
        ASSERT_EQ(
            1, response.table_info(0).table_partition(0).partition_meta_size());
        ASSERT_EQ(
            1, response.table_info(0).table_partition(1).partition_meta_size());
        ASSERT_EQ(
            1, response.table_info(0).table_partition(2).partition_meta_size());
    }

    name = "test" + GenRand();
    {
        ::rtidb::nameserver::CreateTableInfoRequest request;
        ::rtidb::nameserver::CreateTableInfoResponse response;
        ::rtidb::nameserver::ZoneInfo* zone_info_p =
            request.mutable_zone_info();
        zone_info_p->CopyFrom(zone_info);
        TableInfo* table_info = request.mutable_table_info();
        table_info->set_name(name);

        TablePartition* partion = table_info->add_table_partition();
        partion->set_pid(1);
        PartitionMeta* meta_11 = partion->add_partition_meta();
        meta_11->set_endpoint("127.0.0.1:9931");
        meta_11->set_is_leader(true);
        PartitionMeta* meta_12 = partion->add_partition_meta();
        meta_12->set_endpoint("127.0.0.1:9941");
        meta_12->set_is_leader(false);

        TablePartition* partion1 = table_info->add_table_partition();
        partion1->set_pid(2);
        PartitionMeta* meta_21 = partion1->add_partition_meta();
        meta_21->set_endpoint("127.0.0.1:9941");
        meta_21->set_is_leader(true);
        PartitionMeta* meta_22 = partion1->add_partition_meta();
        meta_22->set_endpoint("127.0.0.1:9951");
        meta_22->set_is_leader(false);

        TablePartition* partion2 = table_info->add_table_partition();
        partion2->set_pid(0);
        PartitionMeta* meta_31 = partion2->add_partition_meta();
        meta_31->set_endpoint("127.0.0.1:9951");
        meta_31->set_is_leader(true);

        bool ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::CreateTableInfo, &request,
            &response, FLAGS_request_timeout_ms, 3);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(name, response.table_info().name());
        ASSERT_EQ(3, response.table_info().table_partition_size());
        ASSERT_EQ(2, response.table_info().replica_num());
    }
    {
        ::rtidb::nameserver::ShowTableRequest request;
        ::rtidb::nameserver::ShowTableResponse response;
        request.set_name(name);
        ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::ShowTable, &request,
            &response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(1, response.table_info_size());
        ASSERT_EQ(name, response.table_info(0).name());
        ASSERT_EQ(3, response.table_info(0).table_partition_size());
        ASSERT_EQ(
            1, response.table_info(0).table_partition(0).partition_meta_size());
        ASSERT_EQ(
            1, response.table_info(0).table_partition(1).partition_meta_size());
        ASSERT_EQ(
            1, response.table_info(0).table_partition(2).partition_meta_size());
    }

    FLAGS_endpoint = "127.0.0.1:9952";
    FLAGS_db_root_path = "/tmp/" + ::rtidb::nameserver::GenRand();
    brpc::Server server7;
    StartTablet(server7);

    name = "test" + GenRand();
    {
        ::rtidb::nameserver::CreateTableInfoRequest request;
        ::rtidb::nameserver::CreateTableInfoResponse response;
        ::rtidb::nameserver::ZoneInfo* zone_info_p =
            request.mutable_zone_info();
        zone_info_p->CopyFrom(zone_info);
        TableInfo* table_info = request.mutable_table_info();
        table_info->set_name(name);

        TablePartition* partion = table_info->add_table_partition();
        partion->set_pid(1);
        PartitionMeta* meta_11 = partion->add_partition_meta();
        meta_11->set_endpoint("127.0.0.1:9931");
        meta_11->set_is_leader(true);
        PartitionMeta* meta_12 = partion->add_partition_meta();
        meta_12->set_endpoint("127.0.0.1:9941");
        meta_12->set_is_leader(false);
        PartitionMeta* meta_13 = partion->add_partition_meta();
        meta_13->set_endpoint("127.0.0.1:9951");
        meta_13->set_is_leader(false);

        TablePartition* partion1 = table_info->add_table_partition();
        partion1->set_pid(2);
        PartitionMeta* meta_21 = partion1->add_partition_meta();
        meta_21->set_endpoint("127.0.0.1:9931");
        meta_21->set_is_leader(true);
        PartitionMeta* meta_22 = partion1->add_partition_meta();
        meta_22->set_endpoint("127.0.0.1:9941");
        meta_22->set_is_leader(false);
        PartitionMeta* meta_23 = partion1->add_partition_meta();
        meta_23->set_endpoint("127.0.0.1:9951");
        meta_23->set_is_leader(false);

        TablePartition* partion2 = table_info->add_table_partition();
        partion2->set_pid(0);
        PartitionMeta* meta_31 = partion2->add_partition_meta();
        meta_31->set_endpoint("127.0.0.1:9931");
        meta_31->set_is_leader(true);
        PartitionMeta* meta_32 = partion2->add_partition_meta();
        meta_32->set_endpoint("127.0.0.1:9941");
        meta_32->set_is_leader(false);
        PartitionMeta* meta_33 = partion2->add_partition_meta();
        meta_33->set_endpoint("127.0.0.1:9951");
        meta_33->set_is_leader(false);

        bool ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::CreateTableInfo, &request,
            &response, FLAGS_request_timeout_ms, 3);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(name, response.table_info().name());
        ASSERT_EQ(3, response.table_info().table_partition_size());
        ASSERT_EQ(3, response.table_info().replica_num());
    }
    {
        ::rtidb::nameserver::ShowTableRequest request;
        ::rtidb::nameserver::ShowTableResponse response;
        request.set_name(name);
        ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::ShowTable, &request,
            &response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(1, response.table_info_size());
        ASSERT_EQ(name, response.table_info(0).name());
        ASSERT_EQ(3, response.table_info(0).table_partition_size());
        ASSERT_EQ(
            1, response.table_info(0).table_partition(0).partition_meta_size());
        ASSERT_EQ(
            1, response.table_info(0).table_partition(1).partition_meta_size());
        ASSERT_EQ(
            1, response.table_info(0).table_partition(2).partition_meta_size());
    }

    name = "test" + GenRand();
    {
        ::rtidb::nameserver::CreateTableInfoRequest request;
        ::rtidb::nameserver::CreateTableInfoResponse response;
        ::rtidb::nameserver::ZoneInfo* zone_info_p =
            request.mutable_zone_info();
        zone_info_p->CopyFrom(zone_info);
        TableInfo* table_info = request.mutable_table_info();
        table_info->set_name(name);

        TablePartition* partion = table_info->add_table_partition();
        partion->set_pid(1);
        PartitionMeta* meta_11 = partion->add_partition_meta();
        meta_11->set_endpoint("127.0.0.1:9931");
        meta_11->set_is_leader(true);
        PartitionMeta* meta_12 = partion->add_partition_meta();
        meta_12->set_endpoint("127.0.0.1:9941");
        meta_12->set_is_leader(false);

        TablePartition* partion1 = table_info->add_table_partition();
        partion1->set_pid(2);
        PartitionMeta* meta_21 = partion1->add_partition_meta();
        meta_21->set_endpoint("127.0.0.1:9931");
        meta_21->set_is_leader(true);
        PartitionMeta* meta_23 = partion1->add_partition_meta();
        meta_23->set_endpoint("127.0.0.1:9951");
        meta_23->set_is_leader(false);

        TablePartition* partion2 = table_info->add_table_partition();
        partion2->set_pid(0);
        PartitionMeta* meta_31 = partion2->add_partition_meta();
        meta_31->set_endpoint("127.0.0.1:9931");
        meta_31->set_is_leader(true);
        PartitionMeta* meta_33 = partion2->add_partition_meta();
        meta_33->set_endpoint("127.0.0.1:9951");
        meta_33->set_is_leader(false);

        bool ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::CreateTableInfo, &request,
            &response, FLAGS_request_timeout_ms, 3);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(name, response.table_info().name());
        ASSERT_EQ(3, response.table_info().table_partition_size());
        ASSERT_EQ(2, response.table_info().replica_num());
    }
    {
        ::rtidb::nameserver::ShowTableRequest request;
        ::rtidb::nameserver::ShowTableResponse response;
        request.set_name(name);
        ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::ShowTable, &request,
            &response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(1, response.table_info_size());
        ASSERT_EQ(name, response.table_info(0).name());
        ASSERT_EQ(3, response.table_info(0).table_partition_size());
        ASSERT_EQ(
            1, response.table_info(0).table_partition(0).partition_meta_size());
        ASSERT_EQ(
            1, response.table_info(0).table_partition(1).partition_meta_size());
        ASSERT_EQ(
            1, response.table_info(0).table_partition(2).partition_meta_size());
    }

    name = "test" + GenRand();
    {
        ::rtidb::nameserver::CreateTableInfoRequest request;
        ::rtidb::nameserver::CreateTableInfoResponse response;
        ::rtidb::nameserver::ZoneInfo* zone_info_p =
            request.mutable_zone_info();
        zone_info_p->CopyFrom(zone_info);
        TableInfo* table_info = request.mutable_table_info();
        table_info->set_name(name);

        TablePartition* partion = table_info->add_table_partition();
        partion->set_pid(1);
        PartitionMeta* meta_11 = partion->add_partition_meta();
        meta_11->set_endpoint("127.0.0.1:9931");
        meta_11->set_is_leader(true);
        PartitionMeta* meta_12 = partion->add_partition_meta();
        meta_12->set_endpoint("127.0.0.1:9941");
        meta_12->set_is_leader(false);
        PartitionMeta* meta_13 = partion->add_partition_meta();
        meta_13->set_endpoint("127.0.0.1:9951");
        meta_13->set_is_leader(false);

        bool ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::CreateTableInfo, &request,
            &response, FLAGS_request_timeout_ms, 3);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(name, response.table_info().name());
        ASSERT_EQ(1, response.table_info().table_partition_size());
        ASSERT_EQ(3, response.table_info().replica_num());
    }
    {
        ::rtidb::nameserver::ShowTableRequest request;
        ::rtidb::nameserver::ShowTableResponse response;
        request.set_name(name);
        ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::ShowTable, &request,
            &response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(1, response.table_info_size());
        ASSERT_EQ(name, response.table_info(0).name());
        ASSERT_EQ(1, response.table_info(0).table_partition_size());
        ASSERT_EQ(
            1, response.table_info(0).table_partition(0).partition_meta_size());
    }
}

TEST_F(NameServerImplRemoteTest, CreateTableInfoSimply) {
    // local ns and tablet
    // ns
    FLAGS_zk_cluster = "127.0.0.1:6181";
    FLAGS_zk_root_path = "/rtidb3" + GenRand();
    FLAGS_endpoint = "127.0.0.1:9631";

    NameServerImpl* nameserver_1 = new NameServerImpl();
    brpc::Server server;
    StartNameServer(server, nameserver_1);
    ::rtidb::RpcClient<::rtidb::nameserver::NameServer_Stub>
        name_server_client_1(FLAGS_endpoint);
    name_server_client_1.Init();

    // tablet
    FLAGS_endpoint = "127.0.0.1:9931";
    FLAGS_db_root_path = "/tmp/" + ::rtidb::nameserver::GenRand();
    brpc::Server server1;
    StartTablet(server1);

    FLAGS_endpoint = "127.0.0.1:9941";
    FLAGS_db_root_path = "/tmp/" + ::rtidb::nameserver::GenRand();
    brpc::Server server2;
    StartTablet(server2);

    FLAGS_endpoint = "127.0.0.1:9951";
    FLAGS_db_root_path = "/tmp/" + ::rtidb::nameserver::GenRand();
    brpc::Server server3;
    StartTablet(server3);

    // remote ns and tablet
    // ns
    FLAGS_zk_cluster = "127.0.0.1:6181";
    FLAGS_zk_root_path = "/rtidb3" + GenRand();
    FLAGS_endpoint = "127.0.0.1:9632";

    brpc::Server server4;
    StartNameServer(server4);
    ::rtidb::RpcClient<::rtidb::nameserver::NameServer_Stub>
        name_server_client_2(FLAGS_endpoint);
    name_server_client_2.Init();

    // tablet
    FLAGS_endpoint = "127.0.0.1:9932";
    FLAGS_db_root_path = "/tmp/" + ::rtidb::nameserver::GenRand();
    brpc::Server server5;
    StartTablet(server5);

    FLAGS_endpoint = "127.0.0.1:9942";
    FLAGS_db_root_path = "/tmp/" + ::rtidb::nameserver::GenRand();
    brpc::Server server6;
    StartTablet(server6);

    bool ok = false;
    {
        ::rtidb::nameserver::SwitchModeRequest request;
        ::rtidb::nameserver::GeneralResponse response;
        request.set_sm(kLEADER);
        ok = name_server_client_1.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::SwitchMode, &request,
            &response, FLAGS_request_timeout_ms, 1);
    }
    {
        std::string alias = "remote";
        std::string msg;
        ::rtidb::nameserver::ClusterAddress add_request;
        ::rtidb::nameserver::GeneralResponse add_response;
        add_request.set_alias(alias);
        add_request.set_zk_path(FLAGS_zk_root_path);
        add_request.set_zk_endpoints(FLAGS_zk_cluster);
        ok = name_server_client_1.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::AddReplicaCluster,
            &add_request, &add_response, FLAGS_request_timeout_ms, 1);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, add_response.code());
        sleep(2);
    }

    ZoneInfo zone_info = GetZoneInfo(nameserver_1);
    std::string name = "test" + GenRand();
    {
        ::rtidb::nameserver::CreateTableInfoRequest request;
        ::rtidb::nameserver::CreateTableInfoResponse response;
        ::rtidb::nameserver::ZoneInfo* zone_info_p =
            request.mutable_zone_info();
        zone_info_p->CopyFrom(zone_info);
        TableInfo* table_info = request.mutable_table_info();
        table_info->set_name(name);

        TablePartition* partion = table_info->add_table_partition();
        partion->set_pid(1);
        PartitionMeta* meta_11 = partion->add_partition_meta();
        meta_11->set_endpoint("127.0.0.1:9931");
        meta_11->set_is_leader(true);
        PartitionMeta* meta_12 = partion->add_partition_meta();
        meta_12->set_endpoint("127.0.0.1:9941");
        meta_12->set_is_leader(false);
        PartitionMeta* meta_13 = partion->add_partition_meta();
        meta_13->set_endpoint("127.0.0.1:9951");
        meta_13->set_is_leader(false);

        TablePartition* partion1 = table_info->add_table_partition();
        partion1->set_pid(2);
        PartitionMeta* meta_21 = partion1->add_partition_meta();
        meta_21->set_endpoint("127.0.0.1:9931");
        meta_21->set_is_leader(true);
        PartitionMeta* meta_22 = partion1->add_partition_meta();
        meta_22->set_endpoint("127.0.0.1:9941");
        meta_22->set_is_leader(false);
        PartitionMeta* meta_23 = partion1->add_partition_meta();
        meta_23->set_endpoint("127.0.0.1:9951");
        meta_23->set_is_leader(false);

        TablePartition* partion2 = table_info->add_table_partition();
        partion2->set_pid(0);
        PartitionMeta* meta_31 = partion2->add_partition_meta();
        meta_31->set_endpoint("127.0.0.1:9931");
        meta_31->set_is_leader(true);
        PartitionMeta* meta_32 = partion2->add_partition_meta();
        meta_32->set_endpoint("127.0.0.1:9941");
        meta_32->set_is_leader(false);
        PartitionMeta* meta_33 = partion2->add_partition_meta();
        meta_33->set_endpoint("127.0.0.1:9951");
        meta_33->set_is_leader(false);

        bool ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::CreateTableInfoSimply,
            &request, &response, FLAGS_request_timeout_ms, 3);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(name, response.table_info().name());
        ASSERT_EQ(3, response.table_info().table_partition_size());
        ASSERT_EQ(2, response.table_info().replica_num());
    }

    name = "test" + GenRand();
    {
        ::rtidb::nameserver::CreateTableInfoRequest request;
        ::rtidb::nameserver::CreateTableInfoResponse response;
        ::rtidb::nameserver::ZoneInfo* zone_info_p =
            request.mutable_zone_info();
        zone_info_p->CopyFrom(zone_info);
        TableInfo* table_info = request.mutable_table_info();
        table_info->set_name(name);

        TablePartition* partion = table_info->add_table_partition();
        partion->set_pid(1);
        PartitionMeta* meta_11 = partion->add_partition_meta();
        meta_11->set_endpoint("127.0.0.1:9931");
        meta_11->set_is_leader(true);

        TablePartition* partion1 = table_info->add_table_partition();
        partion1->set_pid(2);
        PartitionMeta* meta_21 = partion1->add_partition_meta();
        meta_21->set_endpoint("127.0.0.1:9941");
        meta_21->set_is_leader(true);

        TablePartition* partion2 = table_info->add_table_partition();
        partion2->set_pid(0);
        PartitionMeta* meta_31 = partion2->add_partition_meta();
        meta_31->set_endpoint("127.0.0.1:9951");
        meta_31->set_is_leader(true);

        bool ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::CreateTableInfoSimply,
            &request, &response, FLAGS_request_timeout_ms, 3);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(name, response.table_info().name());
        ASSERT_EQ(3, response.table_info().table_partition_size());
        ASSERT_EQ(1, response.table_info().replica_num());
    }

    name = "test" + GenRand();
    {
        ::rtidb::nameserver::CreateTableInfoRequest request;
        ::rtidb::nameserver::CreateTableInfoResponse response;
        ::rtidb::nameserver::ZoneInfo* zone_info_p =
            request.mutable_zone_info();
        zone_info_p->CopyFrom(zone_info);
        TableInfo* table_info = request.mutable_table_info();
        table_info->set_name(name);

        TablePartition* partion = table_info->add_table_partition();
        partion->set_pid(1);
        PartitionMeta* meta_11 = partion->add_partition_meta();
        meta_11->set_endpoint("127.0.0.1:9931");
        meta_11->set_is_leader(true);
        PartitionMeta* meta_12 = partion->add_partition_meta();
        meta_12->set_endpoint("127.0.0.1:9941");
        meta_12->set_is_leader(false);

        TablePartition* partion1 = table_info->add_table_partition();
        partion1->set_pid(2);
        PartitionMeta* meta_21 = partion1->add_partition_meta();
        meta_21->set_endpoint("127.0.0.1:9941");
        meta_21->set_is_leader(true);
        PartitionMeta* meta_22 = partion1->add_partition_meta();
        meta_22->set_endpoint("127.0.0.1:9951");
        meta_22->set_is_leader(false);

        TablePartition* partion2 = table_info->add_table_partition();
        partion2->set_pid(0);
        PartitionMeta* meta_31 = partion2->add_partition_meta();
        meta_31->set_endpoint("127.0.0.1:9951");
        meta_31->set_is_leader(true);

        bool ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::CreateTableInfoSimply,
            &request, &response, FLAGS_request_timeout_ms, 3);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(name, response.table_info().name());
        ASSERT_EQ(3, response.table_info().table_partition_size());
        ASSERT_EQ(2, response.table_info().replica_num());
    }

    FLAGS_endpoint = "127.0.0.1:9952";
    FLAGS_db_root_path = "/tmp/" + ::rtidb::nameserver::GenRand();
    brpc::Server server7;
    StartTablet(server7);

    name = "test" + GenRand();
    {
        ::rtidb::nameserver::CreateTableInfoRequest request;
        ::rtidb::nameserver::CreateTableInfoResponse response;
        ::rtidb::nameserver::ZoneInfo* zone_info_p =
            request.mutable_zone_info();
        zone_info_p->CopyFrom(zone_info);
        TableInfo* table_info = request.mutable_table_info();
        table_info->set_name(name);

        TablePartition* partion = table_info->add_table_partition();
        partion->set_pid(1);
        PartitionMeta* meta_11 = partion->add_partition_meta();
        meta_11->set_endpoint("127.0.0.1:9931");
        meta_11->set_is_leader(true);
        PartitionMeta* meta_12 = partion->add_partition_meta();
        meta_12->set_endpoint("127.0.0.1:9941");
        meta_12->set_is_leader(false);
        PartitionMeta* meta_13 = partion->add_partition_meta();
        meta_13->set_endpoint("127.0.0.1:9951");
        meta_13->set_is_leader(false);

        TablePartition* partion1 = table_info->add_table_partition();
        partion1->set_pid(2);
        PartitionMeta* meta_21 = partion1->add_partition_meta();
        meta_21->set_endpoint("127.0.0.1:9931");
        meta_21->set_is_leader(true);
        PartitionMeta* meta_22 = partion1->add_partition_meta();
        meta_22->set_endpoint("127.0.0.1:9941");
        meta_22->set_is_leader(false);
        PartitionMeta* meta_23 = partion1->add_partition_meta();
        meta_23->set_endpoint("127.0.0.1:9951");
        meta_23->set_is_leader(false);

        TablePartition* partion2 = table_info->add_table_partition();
        partion2->set_pid(0);
        PartitionMeta* meta_31 = partion2->add_partition_meta();
        meta_31->set_endpoint("127.0.0.1:9931");
        meta_31->set_is_leader(true);
        PartitionMeta* meta_32 = partion2->add_partition_meta();
        meta_32->set_endpoint("127.0.0.1:9941");
        meta_32->set_is_leader(false);
        PartitionMeta* meta_33 = partion2->add_partition_meta();
        meta_33->set_endpoint("127.0.0.1:9951");
        meta_33->set_is_leader(false);

        bool ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::CreateTableInfoSimply,
            &request, &response, FLAGS_request_timeout_ms, 3);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(name, response.table_info().name());
        ASSERT_EQ(3, response.table_info().table_partition_size());
        ASSERT_EQ(3, response.table_info().replica_num());
    }

    name = "test" + GenRand();
    {
        ::rtidb::nameserver::CreateTableInfoRequest request;
        ::rtidb::nameserver::CreateTableInfoResponse response;
        ::rtidb::nameserver::ZoneInfo* zone_info_p =
            request.mutable_zone_info();
        zone_info_p->CopyFrom(zone_info);
        TableInfo* table_info = request.mutable_table_info();
        table_info->set_name(name);

        TablePartition* partion = table_info->add_table_partition();
        partion->set_pid(1);
        PartitionMeta* meta_11 = partion->add_partition_meta();
        meta_11->set_endpoint("127.0.0.1:9931");
        meta_11->set_is_leader(true);
        PartitionMeta* meta_12 = partion->add_partition_meta();
        meta_12->set_endpoint("127.0.0.1:9941");
        meta_12->set_is_leader(false);

        TablePartition* partion1 = table_info->add_table_partition();
        partion1->set_pid(2);
        PartitionMeta* meta_21 = partion1->add_partition_meta();
        meta_21->set_endpoint("127.0.0.1:9931");
        meta_21->set_is_leader(true);
        PartitionMeta* meta_23 = partion1->add_partition_meta();
        meta_23->set_endpoint("127.0.0.1:9951");
        meta_23->set_is_leader(false);

        TablePartition* partion2 = table_info->add_table_partition();
        partion2->set_pid(0);
        PartitionMeta* meta_31 = partion2->add_partition_meta();
        meta_31->set_endpoint("127.0.0.1:9931");
        meta_31->set_is_leader(true);
        PartitionMeta* meta_33 = partion2->add_partition_meta();
        meta_33->set_endpoint("127.0.0.1:9951");
        meta_33->set_is_leader(false);

        bool ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::CreateTableInfoSimply,
            &request, &response, FLAGS_request_timeout_ms, 3);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(name, response.table_info().name());
        ASSERT_EQ(3, response.table_info().table_partition_size());
        ASSERT_EQ(2, response.table_info().replica_num());
    }

    name = "test" + GenRand();
    {
        ::rtidb::nameserver::CreateTableInfoRequest request;
        ::rtidb::nameserver::CreateTableInfoResponse response;
        ::rtidb::nameserver::ZoneInfo* zone_info_p =
            request.mutable_zone_info();
        zone_info_p->CopyFrom(zone_info);
        TableInfo* table_info = request.mutable_table_info();
        table_info->set_name(name);

        TablePartition* partion = table_info->add_table_partition();
        partion->set_pid(1);
        PartitionMeta* meta_11 = partion->add_partition_meta();
        meta_11->set_endpoint("127.0.0.1:9931");
        meta_11->set_is_leader(true);
        PartitionMeta* meta_12 = partion->add_partition_meta();
        meta_12->set_endpoint("127.0.0.1:9941");
        meta_12->set_is_leader(false);
        PartitionMeta* meta_13 = partion->add_partition_meta();
        meta_13->set_endpoint("127.0.0.1:9951");
        meta_13->set_is_leader(false);

        bool ok = name_server_client_2.SendRequest(
            &::rtidb::nameserver::NameServer_Stub::CreateTableInfoSimply,
            &request, &response, FLAGS_request_timeout_ms, 3);
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, response.code());
        ASSERT_EQ(name, response.table_info().name());
        ASSERT_EQ(1, response.table_info().table_partition_size());
        ASSERT_EQ(3, response.table_info().replica_num());
    }
}

}  // namespace nameserver
}  // namespace rtidb

int main(int argc, char** argv) {
    FLAGS_zk_session_timeout = 100000;
    ::testing::InitGoogleTest(&argc, argv);
    srand(time(NULL));
    ::baidu::common::SetLogLevel(::baidu::common::INFO);
    ::google::ParseCommandLineFlags(&argc, &argv, true);
    // FLAGS_db_root_path = "/tmp/" + ::rtidb::nameserver::GenRand();
    return RUN_ALL_TESTS();
}