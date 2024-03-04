/**
 * Copyright (c) 2022 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef OCEANBASE_ROOTSERVER_OB_TENANT_TRANSFER_SERVICE_H
#define OCEANBASE_ROOTSERVER_OB_TENANT_TRANSFER_SERVICE_H

#include "rootserver/ob_tenant_thread_helper.h" // ObTenantThreadHelper
#include "share/transfer/ob_transfer_info.h" // ObTransferTask
#include "share/ob_balance_define.h" // share::ObBalanceTaskID, share::ObTransferTaskID
#include "share/ls/ob_ls_info.h" // MemberList

namespace oceanbase
{
namespace common
{
class ObTimeoutCtx;
}

namespace share
{
class ObLSID;
namespace schema
{
class ObSimpleTableSchemaV2;
class SchemaKey;
}
}

namespace rootserver
{
// ObTenantTransferService is used to manage the transfer tasks generated by the balance module.
// It will iteract with storage layer to do tablet transfer.
class ObTenantTransferService : public ObTenantThreadHelper,
                                public logservice::ObICheckpointSubHandler,
                                public logservice::ObIReplaySubHandler
{
public:
  static const int64_t THREAD_COUNT = 4;
  static const int64_t MINI_MODE_THREAD_COUNT = 1;

  ObTenantTransferService()
      : is_inited_(false), tenant_id_(OB_INVALID_TENANT_ID), sql_proxy_(NULL) {}
  virtual ~ObTenantTransferService() {}
  int init();
  void destroy();
  virtual void do_work() override;
  DEFINE_MTL_FUNC(ObTenantTransferService)

  /*
   * generate transfer task with INIT status (a task handles no more than 100 partitions)
   *
   * @param [in] trans:           transaction client
   * @param [in] src_ls:          source log stream
   * @param [in] dest_ls:         destination log stream
   * @param [in] part_list:       partition list for transfer
   * @param [in] balance_task_id: parenet balance task id
   * @param [out] task_id:        unique transfer task id
   * @return
   * - OB_SUCCESS:                generate task successfully
   * - other:                     generate task failed
   */
  int generate_transfer_task(
      ObMySQLTransaction &trans,
      const share::ObLSID &src_ls,
      const share::ObLSID &dest_ls,
      const share::ObTransferPartList &part_list,
      const share::ObBalanceTaskID balance_task_id,
      share::ObTransferTaskID &task_id);
  /*
   * try cancel and clear transfer task (only task in INIT status can be canceled)
   *
   * @param[in] task_id:   transfer task id
   * @return
   * - OB_SUCCESS:         cancel task successfully
   * - OB_OP_NOT_ALLOW:    task status is not INIT, can't be cancelled
   * - other:              cancel task failed
   */
  int try_cancel_transfer_task(const share::ObTransferTaskID task_id);

  /*
   * try clear finished transfer task and record history
   * if task is already cleared, return OB_SUCCESS and related info recorded in history
   *
   * @param[in] task_id:               transfer task id
   * @param[out] task:                 transfer task
   * @param[out] all_part_list:        all partitons of the transfer task
   * @param[out] finished_part_list:   successfully transferred partitions + needless transferred (not exist or not in src LS) partitions
   * @return
   * - OB_SUCCESS:         clear task successfully
   * - OB_NEED_RETRY:      task is not finished, can't be cleared
   * - OB_ENTRY_NOT_EXIST: task not found
   * - other:              clear task failed
   */
  int try_clear_transfer_task(
      const share::ObTransferTaskID task_id,
      share::ObTransferTask &transfer_task,
      share::ObTransferPartList &all_part_list,
      share::ObTransferPartList &finished_part_list);

public:
  // interfaces used to register with logservice
  virtual share::SCN get_rec_scn() override { return share::SCN::max_scn();}
  virtual int flush(share::SCN &) override { return OB_SUCCESS; }
  int replay(const void *buffer, const int64_t nbytes, const palf::LSN &lsn, const share::SCN &) override
  {
    UNUSED(buffer);
    UNUSED(nbytes);
    UNUSED(lsn);
    return OB_SUCCESS;
  }

private:
  int process_task_(const share::ObTransferTask::TaskStatus &task_stat);
  int process_init_task_(const share::ObTransferTaskID task_id);
  int check_ls_member_list_(
      common::ObISQLClient &sql_proxy,
      const share::ObLSID &src_ls,
      const share::ObLSID &dest_ls,
      share::ObTransferTaskComment &result_comment);
  int get_member_lists_by_inner_sql_(
      common::ObISQLClient &sql_proxy,
      const share::ObLSID &src_ls,
      const share::ObLSID &dest_ls,
      share::ObLSReplica::MemberList &src_ls_member_list,
      share::ObLSReplica::MemberList &dest_ls_member_list);
  int lock_table_and_part_(
      ObMySQLTransaction &trans,
      const share::ObLSID &src_ls,
      share::ObTransferPartList &part_list,
      share::ObTransferPartList &not_exist_part_list,
      share::ObTransferPartList &lock_failed_part_list,
      share::ObDisplayTabletList &table_lock_tablet_list,
      common::ObIArray<common::ObTabletID> &tablet_ids,
      transaction::tablelock::ObTableLockOwnerID &lock_owner_id);
  int unlock_table_and_part_(
      ObMySQLTransaction &trans,
      const share::ObTransferPartList &part_list,
      const transaction::tablelock::ObTableLockOwnerID &lock_owner_id);
  int get_related_table_schemas_(
      common::ObISQLClient &sql_proxy,
      share::schema::ObSimpleTableSchemaV2 &table_schema,
      ObArenaAllocator &allocator,
      ObArray<share::schema::ObSimpleTableSchemaV2 *> &related_table_schemas);
  int get_tablet_and_partition_idx_by_object_id_(
      share::schema::ObSimpleTableSchemaV2 &table_schema,
      const ObObjectID &part_object_id,
      common::ObTabletID &tablet_id,
      int64_t &part_idx);
  int get_tablet_and_partition_idx_by_object_id_(
      share::schema::ObSimpleTableSchemaV2 &table_schema,
      const ObObjectID &part_object_id,
      common::ObTabletID &tablet_id,
      int64_t &part_idx,
      int64_t &subpart_idx);
  int get_tablet_by_partition_idx_(
      share::schema::ObSimpleTableSchemaV2 &table_schema,
      const int64_t part_idx,
      const int64_t subpart_idx,
      common::ObTabletID &tablet_id);
  int check_tenant_schema_is_ready_(bool &is_ready);
  int unlock_and_clear_task_(
      const share::ObTransferTaskID task_id,
      share::ObTransferTask &task);
  int notify_storage_transfer_service_(
      const share::ObTransferTaskID task_id,
      const share::ObLSID &src_ls);
  int add_in_trans_lock_and_refresh_schema_(
      ObMySQLTransaction &trans,
      const share::ObLSID &src_ls,
      const share::ObTransferPartInfo &part_info,
      common::ObIAllocator &allocator,
      share::schema::ObSimpleTableSchemaV2 *&table_schema,
      common::ObTabletID &tablet_id,
      int64_t &part_idx,
      int64_t &subpart_idx);
  int add_table_lock_(
      ObMySQLTransaction &trans,
      share::schema::ObSimpleTableSchemaV2 &table_schema,
      const share::ObTransferPartInfo &part_info,
      const bool is_out_trans,
      const transaction::tablelock::ObTableLockOwnerID &lock_owner_id);
  int add_out_trans_lock_(
      ObMySQLTransaction &trans,
      const transaction::tablelock::ObTableLockOwnerID &lock_owner_id,
      share::schema::ObSimpleTableSchemaV2 &table_schema,
      const share::ObTransferPartInfo &part_info,
      const common::ObTabletID &tablet_id);
  int generate_related_tablet_ids_(
      share::schema::ObSimpleTableSchemaV2 &table_schema,
      const int64_t part_idx,
      const int64_t subpart_idx,
      common::ObIArray<common::ObTabletID> &tablet_ids);
  int generate_tablet_list_(
      const common::ObIArray<common::ObTabletID> &tablet_ids,
      share::ObTransferTabletList &tablet_list);
  int unlock_table_lock_(
      ObMySQLTransaction &trans,
      share::schema::ObSimpleTableSchemaV2 &table_schema,
      const share::ObTransferPartInfo &part_info,
      const transaction::tablelock::ObTableLockOwnerID &lock_owner_id,
      const int64_t timeout_us);
  int fill_finished_task_info_(
      const share::ObTransferTask &task,
      share::ObTransferPartList &finished_part_list,
      share::ObTransferPartList &all_part_list);
  int batch_get_latest_table_schemas_(
      common::ObIAllocator &allocator,
      const common::ObIArray<ObObjectID> &table_ids,
      common::ObIArray<share::schema::ObSimpleTableSchemaV2 *> &table_schemas);
  int get_latest_table_schema_(
      common::ObIAllocator &allocator,
      const ObObjectID &table_id,
      share::schema::ObSimpleTableSchemaV2 *&table_schema);
  int record_need_move_table_lock_tablet_(
      share::schema::ObSimpleTableSchemaV2 &table_schema,
      const common::ObTabletID &tablet_id,
      share::ObDisplayTabletList &table_lock_tablet_list);
  int set_transaction_timeout_(common::ObTimeoutCtx &ctx);
  int update_comment_for_expected_errors_(
      const int err,
      const share::ObTransferTaskID &task_id,
      const share::ObTransferTaskComment &result_comment);
  int64_t get_tablet_count_threshold_() const;
  int construct_ls_member_list_(
      common::sqlclient::ObMySQLResult &res,
      share::ObLSReplica::MemberList &ls_member_list);
  int check_if_need_wait_due_to_last_failure_(
      common::ObISQLClient &sql_proxy,
      const share::ObTransferTask &task,
      bool &need_wait);
private:
  static const int64_t IDLE_TIME_US = 10 * 1000 * 1000L; // 10s
  static const int64_t BUSY_IDLE_TIME_US = 100 * 1000L; // 100ms
  static const int64_t PART_COUNT_IN_A_TRANSFER = 100;

  bool is_inited_;
  uint64_t tenant_id_;
  common::ObMySQLProxy *sql_proxy_;
};

} // end namespace rootserver
} // end namespace oceanbase
#endif
