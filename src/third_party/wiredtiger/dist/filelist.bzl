
# DO NOT EDIT: automatically built by dist/s_bazel.

# This file is only used by external projects building WiredTiger via Bazel.

WT_FILELIST = ['src/block/block_addr.c',
 'src/block/block_ckpt.c',
 'src/block/block_ckpt_scan.c',
 'src/block/block_compact.c',
 'src/block/block_ext.c',
 'src/block/block_open.c',
 'src/block/block_read.c',
 'src/block/block_session.c',
 'src/block/block_slvg.c',
 'src/block/block_vrfy.c',
 'src/block/block_write.c',
 'src/block_cache/block_cache.c',
 'src/block_cache/block_chunkcache.c',
 'src/block_cache/block_io.c',
 'src/block_cache/block_map.c',
 'src/block_cache/block_mgr.c',
 'src/block_cache/block_tier.c',
 'src/block_disagg/block_disagg_addr.c',
 'src/block_disagg/block_disagg_ckpt.c',
 'src/block_disagg/block_disagg_mgr.c',
 'src/block_disagg/block_disagg_open.c',
 'src/block_disagg/block_disagg_read.c',
 'src/block_disagg/block_disagg_unsup.c',
 'src/block_disagg/block_disagg_write.c',
 'src/bloom/bloom.c',
 'src/btree/bt_compact.c',
 'src/btree/bt_curnext.c',
 'src/btree/bt_curprev.c',
 'src/btree/bt_cursor.c',
 'src/btree/bt_debug.c',
 'src/btree/bt_delete.c',
 'src/btree/bt_discard.c',
 'src/btree/bt_handle.c',
 'src/btree/bt_import.c',
 'src/btree/bt_misc.c',
 'src/btree/bt_npos.c',
 'src/btree/bt_ovfl.c',
 'src/btree/bt_page.c',
 'src/btree/bt_prefetch.c',
 'src/btree/bt_random.c',
 'src/btree/bt_read.c',
 'src/btree/bt_ret.c',
 'src/btree/bt_slvg.c',
 'src/btree/bt_split.c',
 'src/btree/bt_stat.c',
 'src/btree/bt_sync.c',
 'src/btree/bt_sync_obsolete.c',
 'src/btree/bt_vrfy.c',
 'src/btree/bt_vrfy_dsk.c',
 'src/btree/bt_walk.c',
 'src/btree/col_modify.c',
 'src/btree/col_srch.c',
 'src/btree/row_key.c',
 'src/btree/row_modify.c',
 'src/btree/row_srch.c',
 'src/cache/cache.c',
 'src/cache/cache_pool.c',
 'src/call_log/call_log.c',
 'src/checkpoint/checkpoint_ckptlist.c',
 'src/checkpoint/checkpoint_conn.c',
 'src/checkpoint/checkpoint_stats.c',
 'src/checkpoint/checkpoint_txn.c',
 'src/checksum/software/checksum.c',
 'src/conf/conf_bind.c',
 'src/conf/conf_compile.c',
 'src/conf/conf_get.c',
 'src/config/config.c',
 'src/config/config_api.c',
 'src/config/config_check.c',
 'src/config/config_collapse.c',
 'src/config/config_def.c',
 'src/config/config_ext.c',
 'src/config/test_config.c',
 'src/conn/api_calc_modify.c',
 'src/conn/api_strerror.c',
 'src/conn/api_version.c',
 'src/conn/conn_api.c',
 'src/conn/conn_capacity.c',
 'src/conn/conn_chunkcache.c',
 'src/conn/conn_compact.c',
 'src/conn/conn_dhandle.c',
 'src/conn/conn_handle.c',
 'src/conn/conn_layered.c',
 'src/conn/conn_open.c',
 'src/conn/conn_page_history.c',
 'src/conn/conn_prefetch.c',
 'src/conn/conn_reconfig.c',
 'src/conn/conn_stat.c',
 'src/conn/conn_sweep.c',
 'src/conn/conn_tiered.c',
 'src/cursor/cur_backup.c',
 'src/cursor/cur_backup_incr.c',
 'src/cursor/cur_bulk.c',
 'src/cursor/cur_config.c',
 'src/cursor/cur_ds.c',
 'src/cursor/cur_dump.c',
 'src/cursor/cur_file.c',
 'src/cursor/cur_hs.c',
 'src/cursor/cur_index.c',
 'src/cursor/cur_layered.c',
 'src/cursor/cur_metadata.c',
 'src/cursor/cur_prepare_discovered.c',
 'src/cursor/cur_stat.c',
 'src/cursor/cur_std.c',
 'src/cursor/cur_table.c',
 'src/cursor/cur_version.c',
 'src/evict/evict_conn.c',
 'src/evict/evict_file.c',
 'src/evict/evict_lru.c',
 'src/evict/evict_page.c',
 'src/evict/evict_stat.c',
 'src/history/hs_conn.c',
 'src/history/hs_cursor.c',
 'src/history/hs_verify.c',
 'src/live_restore/live_restore_fs.c',
 'src/live_restore/live_restore_server.c',
 'src/live_restore/live_restore_state.c',
 'src/log/log.c',
 'src/log/log_auto.c',
 'src/log/log_cursor.c',
 'src/log/log_mgr.c',
 'src/log/log_slot.c',
 'src/log/log_sys.c',
 'src/meta/meta_apply.c',
 'src/meta/meta_ckpt.c',
 'src/meta/meta_ext.c',
 'src/meta/meta_table.c',
 'src/meta/meta_track.c',
 'src/meta/meta_turtle.c',
 'src/optrack/optrack.c',
 'src/os_common/filename.c',
 'src/os_common/os_abort.c',
 'src/os_common/os_alloc.c',
 'src/os_common/os_errno.c',
 'src/os_common/os_fhandle.c',
 'src/os_common/os_fs_inmemory.c',
 'src/os_common/os_fstream.c',
 'src/os_common/os_fstream_stdio.c',
 'src/os_common/os_getopt.c',
 'src/os_common/os_strtouq.c',
 'src/packing/pack_api.c',
 'src/packing/pack_impl.c',
 'src/packing/pack_stream.c',
 'src/prepared_discover/prepared_discover_txn.c',
 'src/prepared_discover/prepared_discover_walk.c',
 'src/reconcile/rec_child.c',
 'src/reconcile/rec_col.c',
 'src/reconcile/rec_dictionary.c',
 'src/reconcile/rec_row.c',
 'src/reconcile/rec_track.c',
 'src/reconcile/rec_visibility.c',
 'src/reconcile/rec_write.c',
 'src/reconcile/rec_hs.c',
 'src/schema/schema_alter.c',
 'src/schema/schema_create.c',
 'src/schema/schema_drop.c',
 'src/schema/schema_list.c',
 'src/schema/schema_open.c',
 'src/schema/schema_plan.c',
 'src/schema/schema_project.c',
 'src/schema/schema_stat.c',
 'src/schema/schema_truncate.c',
 'src/schema/schema_util.c',
 'src/schema/schema_worker.c',
 'src/session/session_api.c',
 'src/session/session_compact.c',
 'src/session/session_dhandle.c',
 'src/session/session_helper.c',
 'src/session/session_prefetch.c',
 'src/rollback_to_stable/rts_api.c',
 'src/rollback_to_stable/rts_btree.c',
 'src/rollback_to_stable/rts_btree_walk.c',
 'src/rollback_to_stable/rts.c',
 'src/rollback_to_stable/rts_history.c',
 'src/rollback_to_stable/rts_visibility.c',
 'src/support/cond_auto.c',
 'src/support/crypto.c',
 'src/support/err.c',
 'src/support/generation.c',
 'src/support/global.c',
 'src/support/hash_city.c',
 'src/support/hash_fnv.c',
 'src/support/hash_map.c',
 'src/support/hazard.c',
 'src/support/hex.c',
 'src/support/json.c',
 'src/support/lock_ext.c',
 'src/support/modify.c',
 'src/support/mtx_rw.c',
 'src/support/pow.c',
 'src/support/rand.c',
 'src/support/scratch.c',
 'src/support/stat.c',
 'src/support/thread_group.c',
 'src/support/timestamp.c',
 'src/support/update_vector.c',
 'src/tiered/tiered_config.c',
 'src/tiered/tiered_handle.c',
 'src/tiered/tiered_work.c',
 'src/txn/txn.c',
 'src/txn/txn_log.c',
 'src/txn/txn_recover.c',
 'src/txn/txn_timestamp.c']

WT_FILELIST_ARM64_HOST = ['src/checksum/arm64/crc32-arm64.c']

WT_FILELIST_LOONGARCH64_HOST = ['src/checksum/loongarch64/crc32-loongarch64.c']

WT_FILELIST_POWERPC_HOST = ['src/checksum/power8/crc32_wrapper.c', 'src/checksum/power8/vec_crc32.c']

WT_FILELIST_RISCV64_HOST = ['src/checksum/riscv64/crc32-riscv64.c']

WT_FILELIST_X86_HOST = ['src/checksum/x86/crc32-x86-alt.c', 'src/checksum/x86/crc32-x86.c']

WT_FILELIST_ZSERIES_HOST = ['src/checksum/zseries/crc32-s390x.c', 'src/checksum/zseries/crc32le-vx.S']

WT_FILELIST_DARWIN_HOST = ['src/os_darwin/os_futex.c']

WT_FILELIST_LINUX_HOST = ['src/os_linux/os_futex.c']

WT_FILELIST_WINDOWS_HOST = ['src/os_win/os_futex.c',
 'src/os_win/os_dir.c',
 'src/os_win/os_dlopen.c',
 'src/os_win/os_fs.c',
 'src/os_win/os_getenv.c',
 'src/os_win/os_map.c',
 'src/os_win/os_mtx_cond.c',
 'src/os_win/os_once.c',
 'src/os_win/os_pagesize.c',
 'src/os_win/os_path.c',
 'src/os_win/os_priv.c',
 'src/os_win/os_setvbuf.c',
 'src/os_win/os_sleep.c',
 'src/os_win/os_snprintf.c',
 'src/os_win/os_thread.c',
 'src/os_win/os_time.c',
 'src/os_win/os_utf8.c',
 'src/os_win/os_winerr.c',
 'src/os_win/os_yield.c']

WT_FILELIST_POSIX_HOST = ['src/os_posix/os_dir.c',
 'src/os_posix/os_dlopen.c',
 'src/os_posix/os_fallocate.c',
 'src/os_posix/os_fs.c',
 'src/os_posix/os_getenv.c',
 'src/os_posix/os_map.c',
 'src/os_posix/os_mtx_cond.c',
 'src/os_posix/os_once.c',
 'src/os_posix/os_pagesize.c',
 'src/os_posix/os_path.c',
 'src/os_posix/os_priv.c',
 'src/os_posix/os_setvbuf.c',
 'src/os_posix/os_sleep.c',
 'src/os_posix/os_snprintf.c',
 'src/os_posix/os_thread.c',
 'src/os_posix/os_time.c',
 'src/os_posix/os_yield.c']
