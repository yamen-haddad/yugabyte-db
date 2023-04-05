// Copyright (c) YugaByte, Inc.
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

#pragma once

#include "yb/common/hybrid_time.h"
#include "yb/common/snapshot.h"

#include "yb/docdb/docdb_fwd.h"

#include "yb/master/master_backup.pb.h"
#include "yb/master/state_with_tablets.h"

#include "yb/tablet/tablet_fwd.h"

#include "yb/tserver/tserver_fwd.h"

#include "yb/util/async_task_util.h"

DECLARE_int64(max_concurrent_snapshot_rpcs);
DECLARE_int64(max_concurrent_snapshot_rpcs_per_tserver);

namespace yb {
namespace master {

struct TabletSnapshotOperation {
  TabletId tablet_id;
  SnapshotScheduleId schedule_id;
  TxnSnapshotId snapshot_id;
  SysSnapshotEntryPB::State state;
  HybridTime snapshot_hybrid_time;

  std::string ToString() const {
    return YB_STRUCT_TO_STRING(tablet_id, snapshot_id, state, snapshot_hybrid_time);
  }
};

using TabletSnapshotOperations = std::vector<TabletSnapshotOperation>;

class SnapshotState : public StateWithTablets {
 public:
  SnapshotState(
      SnapshotCoordinatorContext* context, const TxnSnapshotId& id,
      const tserver::TabletSnapshotOpRequestPB& request,
      uint64_t throttle_limit = std::numeric_limits<int>::max());

  SnapshotState(
      SnapshotCoordinatorContext* context, const TxnSnapshotId& id,
      const SysSnapshotEntryPB& entry);

  const TxnSnapshotId& id() const {
    return id_;
  }

  HybridTime snapshot_hybrid_time() const {
    return snapshot_hybrid_time_;
  }

  HybridTime previous_snapshot_hybrid_time() const {
    return previous_snapshot_hybrid_time_;
  }

  const SnapshotScheduleId& schedule_id() const {
    return schedule_id_;
  }

  int64_t version() const {
    return version_;
  }

  AsyncTaskTracker& CleanupTracker() {
    return cleanup_tracker_;
  }

  AsyncTaskThrottler& Throttler() {
    return throttler_;
  }

  Result<tablet::CreateSnapshotData> SysCatalogSnapshotData(
      const tablet::SnapshotOperation& operation) const;

  std::string ToString() const;
  // The `options` argument for `ToPB` and `ToEntryPB` controls which entry types are serialized.
  // Pass `nullopt` to serialize all entry types.
  Status ToPB(
      SnapshotInfoPB* out, ListSnapshotsDetailOptionsPB options);
  Status ToEntryPB(
      SysSnapshotEntryPB* out, ForClient for_client,
      ListSnapshotsDetailOptionsPB options);
  Status StoreToWriteBatch(docdb::KeyValueWriteBatchPB* out);
  Status TryStartDelete();
  void PrepareOperations(TabletSnapshotOperations* out);
  void SetVersion(int value);
  bool NeedCleanup() const;
  bool ShouldUpdate(const SnapshotState& other) const;
  void DeleteAborted(const Status& status);

 private:
  std::optional<SysSnapshotEntryPB::State> GetTerminalStateForStatus(const Status& status) override;
  Status CheckDoneStatus(const Status& status) override;

  TxnSnapshotId id_;
  HybridTime snapshot_hybrid_time_;
  HybridTime previous_snapshot_hybrid_time_;
  SysRowEntries entries_;
  // When snapshot is taken as a part of snapshot schedule schedule_id_ will contain this
  // schedule id. Otherwise it will be nil.
  SnapshotScheduleId schedule_id_;
  int64_t version_;
  bool delete_started_ = false;
  AsyncTaskTracker cleanup_tracker_;
  AsyncTaskThrottler throttler_;
};

Result<docdb::KeyBytes> EncodedSnapshotKey(
    const TxnSnapshotId& id, SnapshotCoordinatorContext* context);

} // namespace master
} // namespace yb
