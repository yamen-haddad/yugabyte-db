// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include <chrono>
#include <cmath>
#include <memory>

#include <google/protobuf/util/message_differencer.h>

#include "yb/client/client.h"
#include "yb/client/schema.h"
#include "yb/client/table_creator.h"
#include "yb/client/table_info.h"
#include "yb/client/transaction_manager.h"
#include "yb/client/yb_table_name.h"

#include "yb/common/wire_protocol.h"

#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/postgres-minicluster.h"
#include "yb/integration-tests/yb_mini_cluster_test_base.h"

#include "yb/master/catalog_manager.h"
#include "yb/master/leader_epoch.h"
#include "yb/master/master_backup.proxy.h"
#include "yb/master/mini_master.h"

#include "yb/rpc/messenger.h"
#include "yb/rpc/proxy.h"
#include "yb/rpc/rpc_context.h"

#include "yb/tools/admin-test-base.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"

#include "yb/util/backoff_waiter.h"

#include "yb/yql/pgwrapper/libpq_utils.h"
#include "yb/yql/pgwrapper/pg_wrapper.h"

DECLARE_bool(master_auto_run_initdb);
DECLARE_int32(pgsql_proxy_webserver_port);

namespace yb {
namespace master {

constexpr auto kInterval = 6s;
constexpr auto kRetention = RegularBuildVsDebugVsSanitizers(10min, 18min, 10min);

YB_DEFINE_ENUM(YsqlColocationConfig, (kNotColocated)(kDBColocated));

using namespace std::chrono_literals;

Result<TxnSnapshotRestorationId> RestoreSnapshotSchedule(
    MasterBackupProxy* proxy, const SnapshotScheduleId& schedule_id, const HybridTime& ht,
    MonoDelta timeout) {
  rpc::RpcController controller;
  controller.set_timeout(timeout);
  master::RestoreSnapshotScheduleRequestPB req;
  master::RestoreSnapshotScheduleResponsePB resp;
  req.set_snapshot_schedule_id(schedule_id.data(), schedule_id.size());
  req.set_restore_ht(ht.ToUint64());
  RETURN_NOT_OK(proxy->RestoreSnapshotSchedule(req, &resp, &controller));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }
  return FullyDecodeTxnSnapshotRestorationId(resp.restoration_id());
}

Result<google::protobuf::RepeatedPtrField<RestorationInfoPB>> ListSnapshotRestorations(
    MasterBackupProxy* proxy, const TxnSnapshotRestorationId& restoration_id, MonoDelta timeout) {
  rpc::RpcController controller;
  controller.set_timeout(timeout);
  master::ListSnapshotRestorationsRequestPB req;
  master::ListSnapshotRestorationsResponsePB resp;
  if (restoration_id) {
    req.set_restoration_id(restoration_id.data(), restoration_id.size());
  }
  RETURN_NOT_OK(proxy->ListSnapshotRestorations(req, &resp, &controller));
  if (resp.has_status()) {
    return StatusFromPB(resp.status());
  }
  return resp.restorations();
}

Result<SnapshotScheduleId> CreateSnapshotSchedule(
    MasterBackupProxy* proxy,
    const client::YBTableName& table,
    MonoDelta interval,
    MonoDelta retention_duration,
    MonoDelta timeout) {
  rpc::RpcController controller;
  master::CreateSnapshotScheduleRequestPB req;
  master::CreateSnapshotScheduleResponsePB resp;
  controller.set_timeout(MonoDelta::FromSeconds(10));
  client::YBTableName keyspace;
  master::NamespaceIdentifierPB namespace_id;
  namespace_id.set_database_type(table.namespace_type());
  namespace_id.set_name(table.namespace_name());
  keyspace.GetFromNamespaceIdentifierPB(namespace_id);
  auto* options = req.mutable_options();
  auto* filter_tables = options->mutable_filter()->mutable_tables()->mutable_tables();
  keyspace.SetIntoTableIdentifierPB(filter_tables->Add());
  options->set_interval_sec(std::llround(interval.ToSeconds()));
  options->set_retention_duration_sec(std::llround(retention_duration.ToSeconds()));
  RETURN_NOT_OK(proxy->CreateSnapshotSchedule(req, &resp, &controller));
  return FullyDecodeSnapshotScheduleId(resp.snapshot_schedule_id());
}

Result<SnapshotScheduleInfoPB> GetSnapshotSchedule(
    MasterBackupProxy* proxy, const SnapshotScheduleId& id) {
  rpc::RpcController controller;
  ListSnapshotSchedulesRequestPB req;
  ListSnapshotSchedulesResponsePB resp;
  req.set_snapshot_schedule_id(id.data(), id.size());
  controller.set_timeout(10s);
  RETURN_NOT_OK(proxy->ListSnapshotSchedules(req, &resp, &controller));
  SCHECK_EQ(resp.schedules_size(), 1, NotFound, "Wrong number of schedules");
  return resp.schedules().Get(0);
}

Result<TxnSnapshotId> WaitNewSnapshot(MasterBackupProxy* proxy, const SnapshotScheduleId& id) {
  LOG(INFO) << "WaitNewSnapshot, schedule id: " << id;
  std::string last_snapshot_id;
  std::string new_snapshot_id;
  RETURN_NOT_OK(WaitFor(
      [&proxy, &id, &last_snapshot_id, &new_snapshot_id]() -> Result<bool> {
        // If there's a master leader failover then we should wait for the next cycle.
        auto schedule_info = VERIFY_RESULT(GetSnapshotSchedule(proxy, id));
        auto& snapshots = schedule_info.snapshots();
        if (snapshots.empty()) {
          return false;
        }
        auto snapshot_id = snapshots[snapshots.size() - 1].id();
        LOG(INFO) << "WaitNewSnapshot, last snapshot id: " << snapshot_id;
        if (last_snapshot_id.empty()) {
          last_snapshot_id = snapshot_id;
          return false;
        }
        if (last_snapshot_id != snapshot_id) {
          new_snapshot_id = snapshot_id;
          return true;
        } else {
          return false;
        }
      },
      kInterval * 5, "Wait new schedule snapshot"));
  return FullyDecodeTxnSnapshotId(new_snapshot_id);
}

Status WaitForRestoration(
    MasterBackupProxy* proxy, const TxnSnapshotRestorationId& restoration_id, MonoDelta timeout) {
  auto condition = [proxy, &restoration_id, timeout]() -> Result<bool> {
    auto restorations_status = ListSnapshotRestorations(proxy, restoration_id, timeout);
    RETURN_NOT_OK_RET(ResultToStatus(restorations_status), false);
    google::protobuf::RepeatedPtrField<RestorationInfoPB> restorations = *restorations_status;
    for (const auto& restoration : restorations) {
      if (!(VERIFY_RESULT(FullyDecodeTxnSnapshotRestorationId(restoration.id())) ==
            restoration_id)) {
        continue;
      }
      return restoration.entry().state() == SysSnapshotEntryPB::RESTORED;
    }
    return false;
  };
  return WaitFor(condition, timeout, "Waiting for restoration to complete");
}

Status WaitForSnapshotComplete(
    MasterBackupProxy* proxy, const TxnSnapshotId& snapshot_id, bool check_deleted = false) {
  return WaitFor(
      [&]() -> Result<bool> {
        master::ListSnapshotsRequestPB req;
        master::ListSnapshotsResponsePB resp;
        rpc::RpcController rpc;
        rpc.set_timeout(30s * kTimeMultiplier);
        req.set_snapshot_id(snapshot_id.data(), snapshot_id.size());
        Status s = proxy->ListSnapshots(req, &resp, &rpc);
        // If snapshot is cleaned up and we are waiting for a delete
        // then succeed this call.
        if (check_deleted && !s.ok() && s.IsNotFound()) {
          return true;
        }
        if (resp.has_error()) {
          Status s = StatusFromPB(resp.error().status());
          if (check_deleted && s.IsNotFound()) {
            return true;
          }
          return s;
        }
        if (resp.snapshots_size() != 1) {
          return STATUS(
              IllegalState, Format("There should be exactly one snapshot of id $0", snapshot_id));
        }
        if (check_deleted) {
          return resp.snapshots(0).entry().state() == master::SysSnapshotEntryPB::DELETED;
        }
        return resp.snapshots(0).entry().state() == master::SysSnapshotEntryPB::COMPLETE;
      },
      30s * kTimeMultiplier, "Waiting for snapshot to complete");
}

Result<SnapshotInfoPB> WaitScheduleSnapshot(
    MasterBackupProxy* proxy, const SnapshotScheduleId& id, MonoDelta duration,
    uint32_t num_snapshots = 1) {
  SnapshotInfoPB snapshot;
  RETURN_NOT_OK(WaitFor(
      [proxy, id, num_snapshots, &snapshot]() -> Result<bool> {
        // If there's a master leader failover then we should wait for the next cycle.
        auto schedule = VERIFY_RESULT(GetSnapshotSchedule(proxy, id));
        if ((uint32_t)schedule.snapshots_size() < num_snapshots) {
          return false;
        }
        snapshot = schedule.snapshots()[schedule.snapshots_size() - 1];
        return true;
      },
      duration, Format("Wait for schedule to have $0 snapshots", num_snapshots)));

  // Wait for the present time to become at-least the time chosen by the snapshot.
  auto snapshot_time_string = snapshot.entry().snapshot_hybrid_time();
  HybridTime snapshot_ht = HybridTime::FromPB(snapshot_time_string);

  RETURN_NOT_OK(WaitFor(
      [&snapshot_ht]() -> Result<bool> {
        Timestamp current_time(VERIFY_RESULT(WallClock()->Now()).time_point);
        HybridTime current_ht = HybridTime::FromMicros(current_time.ToInt64());
        return snapshot_ht <= current_ht;
      },
      duration, "Wait Snapshot Time Elapses"));
  return snapshot;
}

Result<master::SnapshotInfoPB> ExportSnapshot(
    MasterBackupProxy* proxy, const TxnSnapshotId& snapshot_id, bool prepare_for_backup = true) {
  master::ListSnapshotsRequestPB req;
  master::ListSnapshotsResponsePB resp;
  rpc::RpcController rpc;
  rpc.set_timeout(30s * kTimeMultiplier);
  req.set_snapshot_id(snapshot_id.data(), snapshot_id.size());
  req.set_prepare_for_backup(prepare_for_backup);
  Status s = proxy->ListSnapshots(req, &resp, &rpc);
  LOG(INFO) << Format("ExportSnapshot response is: $0", resp.ShortDebugString());
  if (!s.ok()) {
    return s;
  }
  if (resp.snapshots_size() != 1) {
    return STATUS(
        IllegalState, Format("There should be exactly one snapshot of id $0", snapshot_id));
  }
  return resp.snapshots(0);
}

class MasterSnapshotTest : public YBMiniClusterTestBase<MiniCluster> {
  void SetUp() override {
    YBMiniClusterTestBase::SetUp();
    MiniClusterOptions opts;
    opts.num_tablet_servers = 1;
    cluster_ = std::make_unique<MiniCluster>(opts);
    ASSERT_OK(cluster_->Start());
    client_ =
        ASSERT_RESULT(client::YBClientBuilder()
                          .add_master_server_addr(cluster_->mini_master()->bound_rpc_addr_str())
                          .Build());
  }

 protected:
  std::unique_ptr<client::YBClient> client_;
};

TEST_F(MasterSnapshotTest, FailSysCatalogWriteWithStaleTable) {
  auto messenger = ASSERT_RESULT(rpc::MessengerBuilder("test-msgr").set_num_reactors(1).Build());
  auto proxy_cache = rpc::ProxyCache(messenger.get());
  auto proxy = MasterBackupProxy(&proxy_cache, cluster_->mini_master()->bound_rpc_addr());

  auto first_epoch = LeaderEpoch(
      cluster_->mini_master()->catalog_manager().leader_ready_term(),
      cluster_->mini_master()->sys_catalog().pitr_count());
  const auto timeout = MonoDelta::FromSeconds(20);
  client::YBTableName table_name(YQL_DATABASE_CQL, "my_keyspace", "test_table");
  ASSERT_OK(client_->CreateNamespaceIfNotExists(
      table_name.namespace_name(), table_name.namespace_type()));
  SnapshotScheduleId schedule_id = ASSERT_RESULT(CreateSnapshotSchedule(
      &proxy, table_name, MonoDelta::FromSeconds(60), MonoDelta::FromSeconds(600), timeout));

  auto table_creator = client_->NewTableCreator();
  client::YBSchemaBuilder b;
  b.AddColumn("key")->Type(DataType::INT32)->NotNull()->HashPrimaryKey();
  b.AddColumn("v1")->Type(DataType::INT64)->NotNull();
  b.AddColumn("v2")->Type(DataType::STRING)->NotNull();
  client::YBSchema schema;
  ASSERT_OK(b.Build(&schema));
  ASSERT_OK(
      table_creator->table_name(table_name).schema(&schema).num_tablets(1).wait(true).Create());

  auto yb_table_info = ASSERT_RESULT(client_->GetYBTableInfo(table_name));
  LOG(INFO) << "Getting table info,";
  auto table_info =
      cluster_->mini_master()->catalog_manager_impl().GetTableInfo(yb_table_info.table_id);
  ASSERT_TRUE(table_info != nullptr);
  Timestamp time(ASSERT_RESULT(WallClock()->Now()).time_point);
  HybridTime ht = ASSERT_RESULT(HybridTime::ParseHybridTime(time.ToString()));
  LOG(INFO) << "Performing restoration.";
  auto restoration_id = ASSERT_RESULT(RestoreSnapshotSchedule(&proxy, schedule_id, ht, timeout));
  LOG(INFO) << "Waiting for restoration.";
  ASSERT_OK(WaitForRestoration(&proxy, restoration_id, timeout));

  LOG(INFO) << "Restoration finished.";
  {
    auto table_lock = table_info->LockForWrite();
    table_lock.mutable_data()->pb.set_parent_table_id("fnord");
    LOG(INFO) << Format(
        "Writing with stale epoch: $0, $1",
        first_epoch.leader_term,
        first_epoch.pitr_count);
    ASSERT_NOK(cluster_->mini_master()->sys_catalog().Upsert(first_epoch, table_info));
    auto post_restore_epoch = LeaderEpoch(
        cluster_->mini_master()->catalog_manager().leader_ready_term(),
        cluster_->mini_master()->sys_catalog().pitr_count());
    LOG(INFO) << Format(
        "Writing with fresh epoch: $0, $1", post_restore_epoch.leader_term,
        post_restore_epoch.pitr_count);
    ASSERT_OK(cluster_->mini_master()->sys_catalog().Upsert(post_restore_epoch, table_info));
  }
  messenger->Shutdown();
}

// Execute the provided sql query and return the result parsed into string
Result<std::string> ExecuteSQLQueryAsstring(
    pgwrapper::PGConn& conn, std::string query, int num_columns) {
  auto results = VERIFY_RESULT(conn.Fetch(query));
  std::stringstream result_string;
  for (int i = 0; i < PQntuples(results.get()); ++i) {
    if (i) {
      result_string << "\n";
    }
    for (int col_num = 0; col_num < num_columns; col_num++) {
      if (col_num != 0) {
        result_string << ", ";
      }
      result_string << VERIFY_RESULT(pgwrapper::ToString(results.get(), i, col_num));
    }
  }
  return result_string.str();
}

class PostgresMiniClusterTest : public YBTest,
                                public ::testing::WithParamInterface<YsqlColocationConfig> {
 public:
  void SetUp() override {
    master::SetDefaultInitialSysCatalogSnapshotFlags();
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_enable_ysql) = true;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_master_auto_run_initdb) = true;
    YBTest::SetUp();
    MiniClusterOptions opts;
    opts.num_tablet_servers = 3;

    test_cluster_.mini_cluster_ = std::make_unique<MiniCluster>(opts);

    ASSERT_OK(mini_cluster()->StartSync());
    ASSERT_OK(mini_cluster()->WaitForTabletServerCount(3));
    ASSERT_OK(WaitForInitDb(mini_cluster()));
    test_cluster_.client_ = ASSERT_RESULT(mini_cluster()->CreateClient());
    ASSERT_OK(test_cluster_.InitPostgres());

    LOG(INFO) << "Cluster created successfully";
  }

  void TearDown() override {
    YBTest::TearDown();

    LOG(INFO) << "Destroying cluster";

    if (test_cluster_.pg_supervisor_) {
      test_cluster_.pg_supervisor_->Stop();
    }
    if (test_cluster_.mini_cluster_) {
      test_cluster_.mini_cluster_->Shutdown();
      test_cluster_.mini_cluster_.reset();
    }
    test_cluster_.client_.reset();
  }

  MiniCluster* mini_cluster() { return test_cluster_.mini_cluster_.get(); }

  client::YBClient* client() { return test_cluster_.client_.get(); }
  Status CreateDatabase(
      PostgresMiniCluster* cluster, const std::string& namespace_name,
      YsqlColocationConfig colocated = YsqlColocationConfig::kNotColocated) {
    auto conn = VERIFY_RESULT(cluster->Connect());
    RETURN_NOT_OK(conn.ExecuteFormat(
        "CREATE DATABASE $0$1", namespace_name,
        colocated == YsqlColocationConfig::kDBColocated ? " with colocation = true" : ""));
    return Status::OK();
  }

  Result<Timestamp> GetCurrentTime() {
    // IMPORTANT NOTE: THE SLEEP IS TEMPORARY AND
    // SHOULD BE REMOVED ONCE GH#12796 IS FIXED.
    SleepFor(MonoDelta::FromSeconds(4 * kTimeMultiplier));
    auto time = Timestamp(VERIFY_RESULT(WallClock()->Now()).time_point);
    LOG(INFO) << "Time to restore: " << time.ToHumanReadableTime();
    return time;
  }

 protected:
  PostgresMiniCluster test_cluster_;
};

class MasterExportSnapshotTest : public PostgresMiniClusterTest {};

INSTANTIATE_TEST_CASE_P(
    Colocation, MasterExportSnapshotTest,
    ::testing::Values(YsqlColocationConfig::kNotColocated, YsqlColocationConfig::kDBColocated));

// Test that export_snapshot_from_schedule as of time generates correct SnapshotInfoPB.
// 1. Create some tables.
// 2. Mark time t and wait for a new snapshot to be created as part of the snapshot schedule.
// 3. export_snapshot to generate the SnapshotInfoPB as of current time. It is the traditional
// export_snapshot command (not the new command) to serve as ground truth.
// 4. Create more tables.
// 5. Generate snapshotInfo from schedule using the time t.
// 6. Assert the output of 5 and 3 are the same.
TEST_P(MasterExportSnapshotTest, ExportSnapshotAsOfTime) {
  auto namespace_name = "testdb";
  ASSERT_OK(CreateDatabase(&test_cluster_, namespace_name, GetParam()));
  LOG(INFO) << "Database created.";
  auto messenger = ASSERT_RESULT(rpc::MessengerBuilder("test-msgr").set_num_reactors(1).Build());
  auto proxy_cache = rpc::ProxyCache(messenger.get());
  auto proxy = MasterBackupProxy(&proxy_cache, mini_cluster()->mini_master()->bound_rpc_addr());

  client::YBTableName table_name(YQL_DATABASE_PGSQL, "testdb", "test_table");
  const auto timeout = MonoDelta::FromSeconds(30);
  SnapshotScheduleId schedule_id =
      ASSERT_RESULT(CreateSnapshotSchedule(&proxy, table_name, kInterval, kRetention, timeout));
  ASSERT_OK(WaitScheduleSnapshot(&proxy, schedule_id, 30s));
  auto conn = ASSERT_RESULT(test_cluster_.ConnectToDB(namespace_name));
  // 1.
  LOG(INFO) << Format("Create tables t1,t2");
  ASSERT_OK(conn.Execute("CREATE TABLE t1 (key INT PRIMARY KEY, value INT)"));
  ASSERT_OK(conn.Execute("CREATE TABLE t2 (key INT PRIMARY KEY, c1 TEXT, c2 TEXT)"));
  // 2.
  Timestamp time = ASSERT_RESULT(GetCurrentTime());
  LOG(INFO) << Format("current timestamp is: {$0}", time);

  // 3.
  auto decoded_snapshot_id = ASSERT_RESULT(WaitNewSnapshot(&proxy, schedule_id));
  ASSERT_OK(WaitForSnapshotComplete(&proxy, decoded_snapshot_id));
  master::SnapshotInfoPB ground_truth = ASSERT_RESULT(ExportSnapshot(&proxy, decoded_snapshot_id));
  // 4.
  ASSERT_OK(conn.Execute("CREATE TABLE t3 (key INT PRIMARY KEY, c1 INT, c2 TEXT, c3 TEXT)"));
  ASSERT_OK(conn.Execute("ALTER TABLE t2 ADD COLUMN new_col TEXT"));
  // 5.
  LOG(INFO) << Format(
      "Exporting snapshot from snapshot schedule: $0, Hybrid time = $1", schedule_id, time);
  auto deadline = CoarseMonoClock::Now() + timeout;
  master::SnapshotInfoPB snapshot_info_as_of_time = ASSERT_RESULT(
      mini_cluster()->mini_master()->catalog_manager_impl().GenerateSnapshotInfoFromSchedule(
          schedule_id, HybridTime::FromMicros(static_cast<uint64>(time.ToInt64())), deadline));
  // 6.
  LOG(INFO) << Format("SnapshotInfoPB ground_truth: $0", ground_truth.ShortDebugString());
  LOG(INFO) << Format(
      "SnapshotInfoPB as of time=$0 :$1", time, snapshot_info_as_of_time.ShortDebugString());
  ASSERT_TRUE(pb_util::ArePBsEqual(
      std::move(ground_truth), std::move(snapshot_info_as_of_time), /* diff_str */ nullptr));
  messenger->Shutdown();
}

class YsqlCloneTest : public PostgresMiniClusterTest {};

// Test cloning PG schema objects from source database to clone database.
// 1. Create database "db1" and create some tables (t1,t2).
// 2. Mark time t.
// 3. Create one more table t3 and add column to t1.
// 4. Clone the PG schema objects of db1 into a clone database db2 as of time t.
// 5. Assert the schema of db2 is same as schema of DB1 at time t.
TEST_F(YsqlCloneTest, ClonePgSchemaObjects) {
  // 1.
  auto source_namespace_name = "db1";
  auto clone_namespace_name = "db2";
  ASSERT_OK(CreateDatabase(&test_cluster_, source_namespace_name));
  LOG(INFO) << Format("Database $0 has been created.", source_namespace_name);
  auto conn = ASSERT_RESULT(test_cluster_.ConnectToDB(source_namespace_name));
  LOG(INFO) << Format("Creating tables t1,t2");
  ASSERT_OK(conn.Execute("CREATE TABLE t1 (key INT PRIMARY KEY, value INT)"));
  ASSERT_OK(conn.Execute("CREATE TABLE t2 (key INT PRIMARY KEY, c1 TEXT, c2 TEXT)"));
  // Get the column names and types of t1 and t2 to compare with the clone output.
  std::string table_columns_query_holder =
      "SELECT column_name, data_type"
      " FROM information_schema.columns"
      " WHERE table_schema = 'public' AND table_name = '$0'";
  std::string num_user_tables_query_holder =
      "SELECT count(*) FROM information_schema.tables WHERE table_schema = 'public' AND "
      "table_catalog = '$0'";
  std::string t1_columns =
      ASSERT_RESULT(ExecuteSQLQueryAsstring(conn, Format(table_columns_query_holder, "t1"), 2));
  std::string t2_columns =
      ASSERT_RESULT(ExecuteSQLQueryAsstring(conn, Format(table_columns_query_holder, "t2"), 2));
  auto db1_num_tables = ASSERT_RESULT(
      conn.FetchRow<int64_t>(Format(num_user_tables_query_holder, source_namespace_name)));
  LOG(INFO) << "db1 schema details at clone time:";
  LOG(INFO) << Format("table t1 colmns' names and type: $0", t1_columns);
  LOG(INFO) << Format("table t2 colmns' names and type: $0", t2_columns);
  LOG(INFO) << Format("num of tables of db1 is: $0", db1_num_tables);
  // 2.
  Timestamp time = ASSERT_RESULT(GetCurrentTime());
  LOG(INFO) << Format("current timestamp is: {$0}", time);
  // 3.
  ASSERT_OK(conn.Execute("CREATE TABLE t3 (key INT PRIMARY KEY, c1 INT, c2 TEXT, c3 TEXT)"));
  ASSERT_OK(conn.Execute("ALTER TABLE t1 ADD COLUMN new_col TEXT"));
  // 4.
  LOG(INFO) << Format(
      "Cloning PG schema objects of database: $0 into database: $1", source_namespace_name,
      clone_namespace_name);
  ASSERT_OK(mini_cluster()->mini_master()->catalog_manager_impl().ClonePgSchemaObjects(
      source_namespace_name, clone_namespace_name,
      HybridTime::FromMicros(static_cast<uint64>(time.ToInt64())), test_cluster_.pg_host_port_));
  // 5. verify that the two database has the same number of tables and that the two tables have the
  // same columns' names and types
  auto clone_db_conn = ASSERT_RESULT(test_cluster_.ConnectToDB(clone_namespace_name));
  ASSERT_EQ(
      db1_num_tables, ASSERT_RESULT(clone_db_conn.FetchRow<int64_t>(
                          Format(num_user_tables_query_holder, clone_namespace_name))));
  ASSERT_EQ(
      t1_columns, ASSERT_RESULT(ExecuteSQLQueryAsstring(
                      clone_db_conn, Format(table_columns_query_holder, "t1"), 2)));
  ASSERT_EQ(
      t2_columns, ASSERT_RESULT(ExecuteSQLQueryAsstring(
                      clone_db_conn, Format(table_columns_query_holder, "t2"), 2)));
}

}  // namespace master
}  // namespace yb
