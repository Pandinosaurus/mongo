/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"
#include "reconcile_private.h"
#include "reconcile_inline.h"

/*
 * __rec_update_save --
 *     Save a WT_UPDATE list for later restoration.
 */
static WT_INLINE int
__rec_update_save(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_INSERT *ins, WT_ROW *rip,
  WT_UPDATE *onpage_upd, WT_UPDATE *tombstone, WT_TIME_WINDOW *tw, bool supd_restore,
  size_t upd_memsize)
{
    WT_SAVE_UPD *supd;

    WT_ASSERT_ALWAYS(session, onpage_upd != NULL || tombstone != NULL || supd_restore,
      "If nothing is committed, the update chain must be restored");
    WT_ASSERT_ALWAYS(session,
      onpage_upd == NULL || onpage_upd->type == WT_UPDATE_STANDARD ||
        onpage_upd->type == WT_UPDATE_MODIFY,
      "Only a standard update or a modify can be written to the data store");
    /* For columns, ins is never null, so rip == NULL implies ins != NULL. */
    WT_ASSERT(session, rip != NULL || ins != NULL);

    WT_RET(__wt_realloc_def(session, &r->supd_allocated, r->supd_next + 1, &r->supd));
    supd = &r->supd[r->supd_next];
    supd->ins = ins;
    supd->rip = rip;
    supd->onpage_upd = onpage_upd;
    supd->onpage_tombstone = tombstone;
    supd->tw = *tw;
    supd->restore = supd_restore;
    ++r->supd_next;
    r->supd_memsize += upd_memsize;
    return (0);
}

/*
 * __rec_delete_hs_upd_save --
 *     Save an update into a WTI_DELETE_HS_UPD list to delete it from the history store later.
 */
static WT_INLINE int
__rec_delete_hs_upd_save(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_INSERT *ins, WT_ROW *rip,
  WT_UPDATE *upd, WT_UPDATE *tombstone)
{
    WTI_DELETE_HS_UPD *delete_hs_upd;

    WT_RET(__wt_realloc_def(
      session, &r->delete_hs_upd_allocated, r->delete_hs_upd_next + 1, &r->delete_hs_upd));
    delete_hs_upd = &r->delete_hs_upd[r->delete_hs_upd_next];
    delete_hs_upd->ins = ins;
    delete_hs_upd->rip = rip;
    delete_hs_upd->upd = upd;
    delete_hs_upd->tombstone = tombstone;
    ++r->delete_hs_upd_next;
    return (0);
}

/*
 * __rec_append_orig_value --
 *     Append the key's original value to its update list. It assumes that we have an onpage value,
 *     the onpage value is not a prepared update, and we don't overwrite transaction id to
 *     WT_TXN_NONE and timestamps to WT_TS_NONE in time window for in-memory databases.
 */
static int
__rec_append_orig_value(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_UPDATE *upd, WT_CELL_UNPACK_KV *unpack)
{
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_UPDATE *append, *oldest_upd, *tombstone;
    size_t size, total_size;
    bool tombstone_globally_visible;

    btree = S2BT(session);
    conn = S2C(session);

    WT_ASSERT_ALWAYS(session,
      upd != NULL && unpack != NULL && unpack->type != WT_CELL_DEL &&
        !WT_TIME_WINDOW_HAS_PREPARE(&(unpack->tw)),
      "__rec_append_orig_value requires an onpage, non-prepared update");

    append = oldest_upd = tombstone = NULL;
    size = total_size = 0;
    tombstone_globally_visible = false;

    /* Review the current update list, checking conditions that mean no work is needed. */
    for (;; upd = upd->next) {
        if (upd->txnid == WT_TXN_ABORTED) {
            if (upd->next == NULL)
                break;
            else
                continue;
        }

        /* Done if the update was restored from the history store or delta. */
        if (F_ISSET(upd, WT_UPDATE_RESTORED_FROM_HS | WT_UPDATE_RESTORED_FROM_DELTA))
            return (0);

        /* Done if the update is a full update restored from the data store. */
        if (F_ISSET(upd, WT_UPDATE_RESTORED_FROM_DS) && upd->type == WT_UPDATE_STANDARD)
            return (0);

        /*
         * Done if the on page value already appears on the update list. We can't do the same check
         * for stop time point because we may still need to append the onpage value if only the
         * tombstone is on the update chain. We only need to check it in the in memory case as in
         * other cases, the update must have been restored from the data store and we may overwrite
         * its transaction id to WT_TXN_NONE and its timestamps to WT_TS_NONE when we write the
         * update to the time window.
         */
        if ((F_ISSET(conn, WT_CONN_IN_MEMORY) || F_ISSET(btree, WT_BTREE_IN_MEMORY)) &&
          unpack->tw.start_ts == upd->upd_start_ts && unpack->tw.start_txn == upd->txnid &&
          upd->type != WT_UPDATE_TOMBSTONE)
            return (0);

        /*
         * Done if at least one self-contained update is globally visible. It's tempting to pull
         * this test out of the loop and only test the oldest self-contained update for global
         * visibility (as visibility tests are expensive). However, when running at lower isolation
         * levels, or when an application intentionally commits in out of timestamp order, it's
         * possible for an update on the chain to be globally visible and followed by an (earlier)
         * update that is not yet globally visible.
         */
        if (WT_UPDATE_DATA_VALUE(upd) && __wt_txn_upd_visible_all(session, upd))
            return (0);

        oldest_upd = upd;

        /* Leave reference pointing to the last item in the update list. */
        if (upd->next == NULL)
            break;
    }

    /*
     * We end up in this function because we have selected a newer value to write to disk. If we
     * select the newest committed update, we should see a valid update here. We can only write
     * uncommitted prepared updates in eviction and if the update chain only has uncommitted
     * prepared updates, we cannot abort them concurrently when we are still evicting the page
     * because we have to do a search for the prepared updates, which can not proceed until eviction
     * finishes.
     */
    WT_ASSERT_ALWAYS(session, oldest_upd != NULL, "No older updates found on update chain");

    bool delta_enabled = WT_DELTA_LEAF_ENABLED(session);
    /*
     * Additionally, we need to append a tombstone before the onpage value we're about to append to
     * the list, if the onpage value has a valid stop time point. Imagine a case where we insert and
     * delete a value respectively at timestamp 0 and 10, and later insert it again at 20. We need
     * the tombstone to tell us there is no value between 10 and 20.
     */
    if (WT_TIME_WINDOW_HAS_STOP(&unpack->tw)) {
        tombstone_globally_visible = __wt_txn_tw_stop_visible_all(session, &unpack->tw);

        /* No need to append the tombstone if it is already in the update chain. */
        if (oldest_upd->type != WT_UPDATE_TOMBSTONE) {
            /*
             * We still need to append the globally visible tombstone if its timestamp is WT_TS_NONE
             * as we may need it to clear the history store content of the key. We don't append a
             * timestamped globally visible tombstone because even if its timestamp is smaller than
             * the entries in the history store, we can't change the history store entries. This is
             * not correct but we hope we can get away with it.
             */
            if (unpack->tw.durable_stop_ts != WT_TS_NONE && tombstone_globally_visible)
                return (0);

            WT_ERR(__wt_upd_alloc_tombstone(session, &tombstone, &size));
            total_size += size;
            /*
             * When reconciling during recovery, we need to clear the transaction id as we haven't
             * done so when we read the page into memory to avoid using the transaction id from the
             * previous run.
             */
            if (F_ISSET(conn, WT_CONN_RECOVERING))
                tombstone->txnid = WT_TXN_NONE;
            else
                tombstone->txnid = unpack->tw.stop_txn;
            tombstone->upd_start_ts = unpack->tw.stop_ts;
            tombstone->upd_durable_ts = unpack->tw.durable_stop_ts;
            F_SET(tombstone, WT_UPDATE_RESTORED_FROM_DS);
            if (delta_enabled)
                F_SET(tombstone, WT_UPDATE_DURABLE);
        } else {
            /*
             * We may have overwritten its transaction id to WT_TXN_NONE and its timestamps to
             * WT_TS_NONE in the time window. In RTS in recovery, we may have cleared the
             * transaction id of the tombstone but we haven't cleared the transaction ids on the
             * disk-image if we are still in recovery.
             */
            WT_ASSERT(session,
              (unpack->tw.stop_ts == oldest_upd->upd_start_ts ||
                unpack->tw.stop_ts == WT_TS_NONE) &&
                (unpack->tw.stop_txn == oldest_upd->txnid || unpack->tw.stop_txn == WT_TXN_NONE ||
                  (oldest_upd->txnid == WT_TXN_NONE && F_ISSET(conn, WT_CONN_RECOVERING) &&
                    F_ISSET(oldest_upd, WT_UPDATE_RESTORED_FROM_DS))));

            if (tombstone_globally_visible)
                return (0);
        }
    }

    /* We need the original on-page value for some reader: get a copy. */
    if (!tombstone_globally_visible) {
        WT_ERR(__wt_scr_alloc(session, 0, &tmp));
        WT_ERR(__wt_page_cell_data_ref_kv(session, page, unpack, tmp));
        /*
         * We should never see an overflow removed value because we haven't freed the overflow
         * blocks.
         */
        WT_ASSERT(session,
          unpack->cell == NULL || __wt_cell_type_raw(unpack->cell) != WT_CELL_VALUE_OVFL_RM);
        WT_ERR(__wt_upd_alloc(session, tmp, WT_UPDATE_STANDARD, &append, &size));
        total_size += size;
        /*
         * When reconciling during recovery, we need to clear the transaction id as we haven't done
         * so when we read the page into memory to avoid using the transaction id from the previous
         * run.
         */
        if (F_ISSET(conn, WT_CONN_RECOVERING))
            append->txnid = WT_TXN_NONE;
        else
            append->txnid = unpack->tw.start_txn;
        append->upd_start_ts = unpack->tw.start_ts;
        append->upd_durable_ts = unpack->tw.durable_start_ts;
        F_SET(append, WT_UPDATE_RESTORED_FROM_DS);
        if (delta_enabled)
            F_SET(append, WT_UPDATE_DURABLE);
    }

    if (tombstone != NULL) {
        tombstone->next = append;
        append = tombstone;
    }

    /* Append the new entry into the update list. */
    WT_RELEASE_WRITE_WITH_BARRIER(upd->next, append);

    __wt_cache_page_inmem_incr(session, page, total_size, false);

    if (0) {
err:
        __wt_free(session, append);
        __wt_free(session, tombstone);
    }
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __rec_find_and_save_delete_hs_upd --
 *     Find and save the update that needs to be deleted from the history store later
 */
static int
__rec_find_and_save_delete_hs_upd(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_INSERT *ins,
  WT_ROW *rip, WTI_UPDATE_SELECT *upd_select)
{
    WT_UPDATE *delete_tombstone, *delete_upd;

    delete_tombstone = NULL;

    for (delete_upd = upd_select->tombstone != NULL ? upd_select->tombstone : upd_select->upd;
         delete_upd != NULL; delete_upd = delete_upd->next) {
        if (delete_upd->txnid == WT_TXN_ABORTED)
            continue;

        if (F_ISSET(delete_upd, WT_UPDATE_TO_DELETE_FROM_HS)) {
            WT_ASSERT_ALWAYS(session,
              F_ISSET(delete_upd, WT_UPDATE_HS | WT_UPDATE_RESTORED_FROM_HS),
              "Attempting to remove an update from the history store in WiredTiger, but the "
              "update was missing.");
            if (delete_upd->type == WT_UPDATE_TOMBSTONE)
                delete_tombstone = delete_upd;
            else {
                WT_RET(
                  __rec_delete_hs_upd_save(session, r, ins, rip, delete_upd, delete_tombstone));
                break;
            }
        }
    }

    WT_ASSERT_ALWAYS(session, delete_tombstone == NULL || delete_upd != NULL,
      "If we delete a tombstone from the history store, we must also delete the update.");

    return (0);
}

/*
 * __rec_need_save_upd --
 *     Return if we need to save the update chain
 */
static WT_INLINE bool
__rec_need_save_upd(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WTI_UPDATE_SELECT *upd_select,
  WT_CELL_UNPACK_KV *vpack, bool has_newer_updates)
{
    WT_UPDATE *upd;
    bool supd_restore, visible_all;

    if (F_ISSET(r, WT_REC_REWRITE_DELTA))
        return (false);

    if (WT_TIME_WINDOW_HAS_PREPARE(&(upd_select->tw)))
        return (true);

    if (F_ISSET(r, WT_REC_EVICT) && has_newer_updates)
        return (true);

    /*
     * We need to save the update chain to build the delta. Don't save the update chain if the
     * selected update is already durable.
     */
    if (upd_select->upd != NULL && WT_DELTA_LEAF_ENABLED(session)) {
        if (upd_select->tombstone != NULL) {
            if (!F_ISSET(upd_select->tombstone, WT_UPDATE_DURABLE | WT_UPDATE_PREPARE_DURABLE))
                return (true);

            /* Save the update if we overwrite the previous prepared update. */
            if (F_ISSET(upd_select->tombstone, WT_UPDATE_PREPARE_DURABLE) &&
              !WT_TIME_WINDOW_HAS_STOP_PREPARE(&upd_select->tw))
                return (true);
        }

        if (upd_select->upd->type == WT_UPDATE_TOMBSTONE) {
            /*
             * Save the update if we haven't deleted the key from the disk image. We may have
             * written the tombstone to disk already but we still need to do another delta to remove
             * it from disk.
             *
             * Deleting the key with a stop timestamp in the delta is not saving disk space but
             * actually increases our disk usage. We need to write a full image to really delete
             * these keys. But if we don't do that, we will have a lot of deleted keys in memory and
             * search will be less efficient. Particularly it will be a problem for the history
             * store.
             */
            if (!F_ISSET(upd_select->upd, WT_UPDATE_DELETE_DURABLE))
                return (true);
        } else {
            if (!F_ISSET(upd_select->upd, WT_UPDATE_DURABLE | WT_UPDATE_PREPARE_DURABLE))
                return (true);

            /* Save the update if we overwrite the previous prepared update. */
            if (F_ISSET(upd_select->upd, WT_UPDATE_PREPARE_DURABLE) &&
              !WT_TIME_WINDOW_HAS_START_PREPARE(&upd_select->tw))
                return (true);
        }
    }

    /* No need to save the update chain if we want to delete the key from the disk image. */
    if (upd_select->upd != NULL && upd_select->upd->type == WT_UPDATE_TOMBSTONE)
        return (false);

    /*
     * Don't save updates for any reconciliation that doesn't involve history store (in-memory
     * database, metadata, and history store reconciliation itself), except when the selected stop
     * time point or the selected start time point is not globally visible for in memory database.
     */
    if (!F_ISSET(r, WT_REC_HS) && !F_ISSET(r, WT_REC_IN_MEMORY))
        return (false);

    /* When in checkpoint, no need to save update if no onpage value is selected. */
    if (F_ISSET(r, WT_REC_CHECKPOINT) && upd_select->upd == NULL)
        return (false);

    if (WT_TIME_WINDOW_HAS_STOP(&upd_select->tw))
        visible_all = __wt_txn_tw_stop_visible_all(session, &upd_select->tw);
    else
        visible_all = __wt_txn_tw_start_visible_all(session, &upd_select->tw);

    if (visible_all)
        return (false);

    /*
     * Update chains are only need to be saved when there are:
     * 1. Newer uncommitted updates or database is configured for in-memory storage.
     * 2. On-disk entry exists.
     * 3. Valid updates exist in the update chain to be written to the history store.
     */
    supd_restore = F_ISSET(r, WT_REC_EVICT) &&
      (has_newer_updates || F_ISSET(S2C(session), WT_CONN_IN_MEMORY) ||
        F_ISSET(S2BT(session), WT_BTREE_IN_MEMORY));

    if (!supd_restore && vpack == NULL && upd_select->upd != NULL) {
        upd = upd_select->upd;
        while (upd->next != NULL) {
            upd = upd->next;
            if (upd->txnid != WT_TXN_ABORTED)
                return (true);
        }
        return (false);
    }

    return (true);
}

/*
 * __timestamp_no_ts_fix --
 *     If we found a tombstone with a time point earlier than the update it applies to, which can
 *     happen if the application performs operations without timestamps, make it invisible by making
 *     the start time point match the stop time point of the tombstone. We don't guarantee that
 *     older readers will be able to continue reading content that has been made invisible by
 *     updates without timestamps. Note that we carefully don't take this path when the stop time
 *     point is equal to the start time point. While unusual, it is permitted for a single
 *     transaction to insert and then remove a record. We don't want to generate a warning in that
 *     case.
 */
static WT_INLINE bool
__timestamp_no_ts_fix(WT_SESSION_IMPL *session, WT_TIME_WINDOW *select_tw)
{
    char time_string[WT_TIME_STRING_SIZE];

    /*
     * When supporting read-uncommitted it was possible for the stop_txn to be less than the
     * start_txn, this is no longer true so assert that we don't encounter it.
     */
    WT_ASSERT(session, select_tw->stop_txn >= select_tw->start_txn);
    WT_ASSERT(session,
      !WT_TIME_WINDOW_HAS_STOP_PREPARE(select_tw) ||
        select_tw->stop_prepare_ts >= select_tw->start_ts);
    if (!WT_TIME_WINDOW_HAS_STOP_PREPARE(select_tw) && select_tw->stop_ts < select_tw->start_ts) {
        WT_ASSERT(session, select_tw->stop_ts == WT_TS_NONE);
        __wt_verbose(session, WT_VERB_TIMESTAMP,
          "Warning: fixing remove without a timestamp earlier than value; time window %s",
          __wt_time_window_to_string(select_tw, time_string));

        select_tw->durable_start_ts = select_tw->durable_stop_ts;
        select_tw->start_ts = select_tw->stop_ts;
        return (true);
    }
    return (false);
}

/*
 * __rec_validate_upd_chain --
 *     Check the update chain for conditions that would prevent its insertion into the history
 *     store. Return EBUSY if the update chain cannot be inserted into the history store at this
 *     time.
 */
static int
__rec_validate_upd_chain(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_UPDATE *select_upd,
  WT_TIME_WINDOW *select_tw, WT_CELL_UNPACK_KV *vpack)
{
    WT_UPDATE *prev_upd, *upd;

    /*
     * There is no selected update to go to disk as such we don't need to check the updates
     * following it.
     */
    if (select_upd == NULL)
        return (0);

    /*
     * No need to check updates without timestamps for any reconciliation that doesn't involve
     * history store (in-memory database, metadata, and history store reconciliation itself).
     */
    if (!F_ISSET(r, WT_REC_HS))
        return (0);

    /*
     * If eviction reconciliation starts before checkpoint, it is fine to evict updates without
     * timestamps.
     */
    if (!F_ISSET(r, WT_REC_CHECKPOINT_RUNNING))
        return (0);

    /* Cannot delete the update from history store when checkpoint is running. */
    if (r->delete_hs_upd_next > 0) {
        WT_STAT_CONN_DSRC_INCR(session, cache_eviction_blocked_remove_hs_race_with_checkpoint);
        return (EBUSY);
    }

    /*
     * The selected time window may contain information that isn't visible given the selected
     * update, as such we have to check it separately. This is true when there is a tombstone ahead
     * of the selected update.
     */
    if (select_tw->stop_ts < select_tw->start_ts) {
        WT_ASSERT_ALWAYS(
          session, select_tw->stop_ts == WT_TS_NONE, "No stop timestamp found for selected update");
        WT_STAT_CONN_DSRC_INCR(session, cache_eviction_blocked_no_ts_checkpoint_race_2);
        return (EBUSY);
    }

    /*
     * Rollback to stable may restore older updates from the data store or history store. In this
     * case, the restored update has older update than the onpage value, which is expected.
     * Reconciliation may restore the onpage value to the update chain. In this case, no need to
     * check further as the value is the same as the onpage value which means we processed this
     * update chain in a previous round of reconciliation. If we have a prepared update restored
     * from the onpage value, no need to check as well because the update chain should only contain
     * prepared updates from the same transaction.
     */
    if (F_ISSET(select_upd,
          WT_UPDATE_RESTORED_FROM_DS | WT_UPDATE_RESTORED_FROM_HS |
            WT_UPDATE_PREPARE_RESTORED_FROM_DS))
        return (0);

    /* Loop forward from update after the selected on-page update. */
    for (prev_upd = select_upd, upd = select_upd->next; upd != NULL; upd = upd->next) {
        if (upd->txnid == WT_TXN_ABORTED)
            continue;

        char ts_string[4][WT_TS_INT_STRING_SIZE];
        WT_ASSERT_ALWAYS(session,
          prev_upd->prepare_state == WT_PREPARE_INPROGRESS ||
            prev_upd->upd_start_ts == prev_upd->upd_durable_ts ||
            prev_upd->upd_durable_ts >= upd->upd_durable_ts,
          "Durable timestamps cannot be out of order for prepared updates: "
          "prev_upd->upd_start_ts=%s, prev_upd->prepare_ts_ts=%s, "
          "prev_upd->upd_durable_ts=%s, prev_upd->flags=%" PRIu16
          ", upd->upd_durable_ts=%s, upd->flags=%" PRIu16,
          __wt_timestamp_to_string(prev_upd->upd_start_ts, ts_string[0]),
          __wt_timestamp_to_string(prev_upd->prepare_ts, ts_string[1]),
          __wt_timestamp_to_string(prev_upd->upd_durable_ts, ts_string[2]), prev_upd->flags,
          __wt_timestamp_to_string(upd->upd_durable_ts, ts_string[3]), upd->flags);

        /* Validate that the updates older than us have older timestamps. */
        if (prev_upd->upd_start_ts < upd->upd_start_ts) {
            WT_ASSERT_ALWAYS(session, prev_upd->upd_start_ts == WT_TS_NONE,
              "Previous update missing start timestamp");
            WT_STAT_CONN_DSRC_INCR(session, cache_eviction_blocked_no_ts_checkpoint_race_4);
            return (EBUSY);
        }

        /*
         * Rollback to stable may restore older updates from the data store or history store. In
         * this case, the restored update has older update than the onpage value, which is expected.
         * Reconciliation may restore the onpage value to the update chain. In this case, no need to
         * check further as the value is the same as the onpage value. If we have a committed
         * prepared update restored from the onpage value, no need to check further as well because
         * the update chain after it should only contain committed prepared updates from the same
         * transaction.
         */
        if (F_ISSET(upd,
              WT_UPDATE_RESTORED_FROM_DS | WT_UPDATE_RESTORED_FROM_HS |
                WT_UPDATE_PREPARE_RESTORED_FROM_DS))
            return (0);

        prev_upd = upd;
    }

    /*
     * Check that the on-page time window has a timestamp. Don't check against ondisk prepared
     * update. It is either committed or rolled back if we are here. If we haven't seen an update
     * with the flag WT_UPDATE_RESTORED_FROM_DS we check against the ondisk value.
     *
     * In the case of checkpoint reconciliation the ondisk value could be an update in the middle of
     * the update chain but checkpoint won't replace the page image as such it will be the previous
     * reconciliations ondisk value that we will be comparing against.
     */
    if (vpack != NULL && !WT_TIME_WINDOW_HAS_PREPARE(&(vpack->tw))) {
        char ts_string[4][WT_TS_INT_STRING_SIZE];
        if (WT_TIME_WINDOW_HAS_STOP(&vpack->tw))
            WT_ASSERT_ALWAYS(session,
              prev_upd->prepare_state == WT_PREPARE_INPROGRESS ||
                prev_upd->upd_start_ts == prev_upd->upd_durable_ts ||
                prev_upd->upd_durable_ts >= vpack->tw.durable_stop_ts,
              "Stop: Durable timestamps cannot be out of order for prepared updates: "
              "prev_upd->upd_start_ts=%s, prev_upd->prepare_ts=%s, prev_upd->upd_durable_ts=%s, "
              "prev_upd->flags=%" PRIu16 ", vpack->tw.durable_stop_ts=%s",
              __wt_timestamp_to_string(prev_upd->upd_start_ts, ts_string[0]),
              __wt_timestamp_to_string(prev_upd->prepare_ts, ts_string[1]),
              __wt_timestamp_to_string(prev_upd->upd_durable_ts, ts_string[2]), prev_upd->flags,
              __wt_timestamp_to_string(vpack->tw.durable_stop_ts, ts_string[3]));
        else
            WT_ASSERT_ALWAYS(session,
              prev_upd->prepare_state == WT_PREPARE_INPROGRESS ||
                prev_upd->upd_start_ts == prev_upd->upd_durable_ts ||
                prev_upd->upd_durable_ts >= vpack->tw.durable_start_ts,
              "Start: Durable timestamps cannot be out of order for prepared updates: "
              "prev_upd->upd_start_ts=%s, prev_upd->prepare_ts=%s, prev_upd->upd_durable_ts=%s, "
              "prev_upd->flags=%" PRIu16 ", vpack->tw.durable_start_ts=%s",
              __wt_timestamp_to_string(prev_upd->upd_start_ts, ts_string[0]),
              __wt_timestamp_to_string(prev_upd->prepare_ts, ts_string[1]),
              __wt_timestamp_to_string(prev_upd->upd_durable_ts, ts_string[2]), prev_upd->flags,
              __wt_timestamp_to_string(vpack->tw.durable_start_ts, ts_string[3]));

        if (prev_upd->upd_start_ts == WT_TS_NONE) {
            if (vpack->tw.start_ts != WT_TS_NONE ||
              (WT_TIME_WINDOW_HAS_STOP(&vpack->tw) && vpack->tw.stop_ts != WT_TS_NONE)) {
                WT_STAT_CONN_DSRC_INCR(session, cache_eviction_blocked_no_ts_checkpoint_race_1);
                return (EBUSY);
            }
        } else
            /*
             * Rollback to stable may recover updates from the history store that is out of order to
             * the on-disk value. Normally these updates have the WT_UPDATE_RESTORED_FROM_HS flag on
             * them. However, in rare cases, if the newer update becomes globally visible, the
             * restored update may be removed by the obsolete check. This may lead to an out of
             * order edge case but it is benign. Check the global visibility of the update and
             * ignore this case.
             */
            WT_ASSERT(session,
              __wt_txn_upd_visible_all(session, prev_upd) ||
                (prev_upd->upd_start_ts >= vpack->tw.start_ts &&
                  (!WT_TIME_WINDOW_HAS_STOP(&vpack->tw) ||
                    prev_upd->upd_start_ts >= vpack->tw.stop_ts)));
    }

    return (0);
}

/*
 * __rec_calc_upd_memsize --
 *     Calculate the saved update size.
 */
static WT_INLINE size_t
__rec_calc_upd_memsize(WT_UPDATE *onpage_upd, WT_UPDATE *tombstone, size_t upd_memsize)
{
    WT_UPDATE *upd;

    /*
     * The total update size only contains uncommitted updates. Add the size for the rest of the
     * chain.
     *
     * FIXME-WT-9182: figure out what should be included in the calculation of the size of the saved
     * update chains.
     */
    if (onpage_upd != NULL) {
        for (upd = tombstone != NULL ? tombstone : onpage_upd; upd != NULL; upd = upd->next)
            if (upd->txnid != WT_TXN_ABORTED)
                upd_memsize += WT_UPDATE_MEMSIZE(upd);
    }
    return (upd_memsize);
}

/*
 * __rec_upd_select --
 *     Select the update to write to disk image.
 */
static int
__rec_upd_select(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_UPDATE *first_upd,
  WTI_UPDATE_SELECT *upd_select, WT_UPDATE **first_txn_updp, bool *has_newer_updatesp,
  size_t *upd_memsizep)
{
    WT_CONNECTION_IMPL *conn;
    WT_UPDATE *upd;
    wt_timestamp_t max_ts;
    uint64_t max_txn, session_txnid, txnid;
    uint8_t prepare_state;
    bool is_hs_page, seen_prepare;

    conn = S2C(session);
    max_ts = WT_TS_NONE;
    max_txn = WT_TXN_NONE;
    is_hs_page = F_ISSET(session->dhandle, WT_DHANDLE_HS);
    session_txnid = __wt_atomic_loadv64(&WT_SESSION_TXN_SHARED(session)->id);
    seen_prepare = false;

    for (upd = first_upd; upd != NULL; upd = upd->next) {
        if ((txnid = upd->txnid) == WT_TXN_ABORTED)
            continue;

        /*
         * Give up if the update is from this transaction and on the metadata file or disaggregated
         * shared metadata file.
         */
        if ((WT_IS_METADATA(session->dhandle) || WT_IS_DISAGG_META(session->dhandle)) &&
          txnid != WT_TXN_NONE && txnid == session_txnid)
            return (__wt_set_return(session, EBUSY));

        /*
         * Track the first update in the chain that is not aborted and the maximum transaction ID.
         */
        if (*first_txn_updp == NULL)
            *first_txn_updp = upd;

        /*
         * Special handling for application threads evicting their own updates.
         */
        if (!is_hs_page && F_ISSET(r, WT_REC_APP_EVICTION_SNAPSHOT) && txnid == session_txnid) {
            *upd_memsizep += WT_UPDATE_MEMSIZE(upd);
            *has_newer_updatesp = true;
            continue;
        }
        /*
         * Check whether the update was committed before reconciliation started. The global commit
         * point can move forward during reconciliation so we use a cached copy to avoid races when
         * a concurrent transaction commits or rolls back while we are examining its updates. This
         * check is not required for history store updates as they are implicitly committed. As
         * prepared transaction IDs are globally visible, need to check the update state as well.
         *
         * There are several cases we should select the update irrespective of visibility. See the
         * detailed scenarios in the definition of WT_UPDATE_SELECT_FOR_DS.
         *
         * These scenarios can happen if the current reconciliation has a limited visibility of
         * updates compared to one of the previous reconciliations. This is important as it is never
         * ok to undo the work of the previous reconciliations.
         */
        if (!F_ISSET(upd, WT_UPDATE_SELECT_FOR_DS) && !is_hs_page &&
          (F_ISSET(r, WT_REC_VISIBLE_ALL) ? (r->last_running <= txnid) :
                                            !__txn_visible_id(session, txnid))) {
            /*
             * Rare case: metadata writes at read uncommitted isolation level, eviction may see a
             * committed update followed by uncommitted updates. Give up in that case because we
             * can't discard the uncommitted updates.
             */
            if (upd_select->upd != NULL) {
                WT_ASSERT_ALWAYS(session, WT_IS_METADATA(session->dhandle),
                  "Uncommitted update followed by committed update in a non-metadata file");
                return (__wt_set_return(session, EBUSY));
            }

            *upd_memsizep += WT_UPDATE_MEMSIZE(upd);
            *has_newer_updatesp = true;
            continue;
        }

        /*
         * Only checkpoint should ever encounter resolving prepared transactions. If it does, then
         * it needs to wait to see whether they should be included or not.
         */
        WT_READ_ONCE(prepare_state, upd->prepare_state);
        while (prepare_state == WT_PREPARE_LOCKED) {
            WT_ASSERT_ALWAYS(session, F_ISSET(r, WT_REC_CHECKPOINT),
              "Eviction should never occur on a page that has resolving prepared records.");
            /*
             * FIXME: WT-14897. This while loop can be removed if we start to use the new prepared
             * timestamp field.
             */
            __wt_sleep(0, 100);
            WT_READ_ONCE(prepare_state, upd->prepare_state);
        }

        /*
         * An interesting case this code will need to deal with is the case where a prepare (start)
         * timestamp is old enough that it should be included in a checkpoint, but the commit
         * timestamp is new enough that it should be excluded. If non-precise checkpoints are
         * configured, the full record can be included and rollback-to-stable will fix up content on
         * recovery (it will need to be able to do that for out-front evictions anyway). For precise
         * checkpoints the reconciliation code will need to write a record as it was before it was
         * committed, and also leave the page/update in a state that makes sense (i.e: we might need
         * a new flag like WT_UPDATE_DS, but indicating that it's partially in the datastore).
         * Question: does this become tricky if a prepare makes multiple changes to the same key?
         */
        if (prepare_state == WT_PREPARE_INPROGRESS) {
            WT_ASSERT_ALWAYS(session,
              upd_select->upd == NULL || upd_select->upd->txnid == upd->txnid,
              "Cannot have two different prepared transactions active on the same key");
            /*
             * Don't save the record if it's prepare time is greater than the checkpoint timestamp
             * when preserve prepared is enabled.
             */
            if (F_ISSET(r, WT_REC_CHECKPOINT) &&
              (!F_ISSET(conn, WT_CONN_PRESERVE_PREPARED) ||
                upd->upd_start_ts > conn->txn_global.checkpoint_timestamp)) {
                *upd_memsizep += WT_UPDATE_MEMSIZE(upd);
                *has_newer_updatesp = true;
                seen_prepare = true;
                continue;
            } else {
                /*
                 * If we are not in eviction, we must be in salvage to reach here. Since salvage
                 * only works on data on-disk, the prepared update must be restored from the disk.
                 * No need for us to rollback the prepared update in salvage here. If there is still
                 * content for that key left in the history store, rollback to stable will bring it
                 * back to the data store later. Otherwise, it removes the key.
                 */
                WT_ASSERT_ALWAYS(session,
                  F_ISSET(r, WT_REC_CHECKPOINT) || F_ISSET(r, WT_REC_EVICT) ||
                    (F_ISSET(r, WT_REC_VISIBILITY_ERR) &&
                      F_ISSET(upd, WT_UPDATE_PREPARE_RESTORED_FROM_DS)),
                  "Should never salvage a prepared update not from disk.");
                /* Prepared updates cannot be resolved concurrently to eviction and salvage. */
                WT_ASSERT_ALWAYS(session, upd->prepare_state == WT_PREPARE_INPROGRESS,
                  "Should never concurrently resolve a prepared update during reconciliation if we "
                  "are not in a checkpoint.");
            }
        }

        /*
         * Don't write any update that is not stable if precise checkpoint is enabled.
         *
         * If we are rewriting the page restored from deltas on the standby, we may see the pinned
         * stable timestamp behind the shared checkpoint timestamp. Check the update flag to write
         * it anyway.
         *
         * FIXME-WT-14902: currently we only support this mode from startup. If we want to enable
         * this through reconfiguration, we need to ensure we have run a rollback to stable before
         * we run the first checkpoint with the precise mode.
         */
        if (F_ISSET(conn, WT_CONN_PRECISE_CHECKPOINT) &&
          !F_ISSET(upd, WT_UPDATE_RESTORED_FROM_DELTA) &&
          upd->upd_durable_ts > r->rec_start_pinned_stable_ts) {
            WT_ASSERT(session, !is_hs_page);
            *upd_memsizep += WT_UPDATE_MEMSIZE(upd);
            *has_newer_updatesp = true;
            continue;
        }

        /*
         * Always select the newest visible update if precise checkpoint is not enabled. Otherwise,
         * select the first update that is smaller or equal to the pinned timestamp.
         */
        if (upd_select->upd == NULL)
            upd_select->upd = upd;

        /* Track the selected update transaction id and timestamp. */
        if (max_txn < txnid)
            max_txn = txnid;

        if (upd->upd_start_ts > max_ts)
            max_ts = upd->upd_start_ts;

        /*
         * We only need to walk the whole update chain if we are evicting metadata as it is written
         * with read uncommitted isolation and we may see a committed update followed by uncommitted
         * updates
         */
        if (!F_ISSET(r, WT_REC_EVICT) || !WT_IS_METADATA(session->dhandle))
            break;
    }

    /*
     * Track the most recent transaction in the page. We store this in the tree at the end of
     * reconciliation in the service of checkpoints, it is used to avoid discarding trees from
     * memory when they have changes required to satisfy a snapshot read.
     */
    if (r->max_txn < max_txn)
        r->max_txn = max_txn;

    /* Update the maximum timestamp. */
    if (max_ts > r->max_ts)
        r->max_ts = max_ts;

    /*
     * We should never select an update that has been written to the history store except checkpoint
     * writes the update that is older than a prepared update or we need to first delete the update
     * from the history store.
     */
    WT_ASSERT_ALWAYS(session,
      upd_select->upd == NULL || !F_ISSET(upd_select->upd, WT_UPDATE_HS) ||
        F_ISSET(upd_select->upd, WT_UPDATE_TO_DELETE_FROM_HS) ||
        (!F_ISSET(r, WT_REC_EVICT) && seen_prepare),
      "Selected update that has already been written to the history store");

    return (0);
}

/*
 * __rec_fill_tw_from_upd_select --
 *     Fill the time window information and the selected update.
 */
static int
__rec_fill_tw_from_upd_select(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL_UNPACK_KV *vpack, WTI_UPDATE_SELECT *upd_select)
{
    WT_TIME_WINDOW *select_tw;
    WT_UPDATE *last_upd, *tombstone, *upd;

    upd = upd_select->upd;
    last_upd = tombstone = NULL;
    select_tw = &upd_select->tw;

    /*
     * The start timestamp is determined by the commit timestamp when the key is first inserted (or
     * last updated). The end timestamp is set when a key/value pair becomes invalid, either because
     * of a remove or a modify/update operation on the same key.
     */

    /*
     * If the newest is a tombstone then select the update before it and set the end of the
     * visibility window to its time point as appropriate to indicate that we should return "not
     * found" for reads after this point.
     *
     * Otherwise, leave the end of the visibility window at the maximum possible value to indicate
     * that the value is visible to any timestamp/transaction id ahead of it.
     */
    if (upd->type == WT_UPDATE_TOMBSTONE) {
        WT_TIME_WINDOW_SET_STOP(select_tw, upd, upd->prepare_state);
        tombstone = upd_select->tombstone = upd;

        /* Find the update this tombstone applies to. */
        if (!__wt_txn_upd_visible_all(session, upd)) {
            while (upd->next != NULL && upd->next->txnid == WT_TXN_ABORTED)
                upd = upd->next;

            WT_ASSERT(session, upd->next == NULL || upd->next->txnid != WT_TXN_ABORTED);
            upd_select->upd = upd = upd->next;
            /* We should not see multiple consecutive tombstones. */
            WT_ASSERT_ALWAYS(session, upd == NULL || upd->type != WT_UPDATE_TOMBSTONE,
              "Consecutive tombstones found on the update chain");
        }
    }

    if (upd != NULL)
        /* The beginning of the validity window is the selected update's time point. */
        WT_TIME_WINDOW_SET_START(select_tw, upd, upd->prepare_state);
    else if (select_tw->stop_ts != WT_TS_NONE || select_tw->stop_txn != WT_TXN_NONE) {
        WT_ASSERT_ALWAYS(
          session, tombstone != NULL, "The only contents of the update list is a single tombstone");

        /*
         * The fixed-length column-store implicitly fills the gap with empty records of single
         * tombstones. We are done with update selection if there is no on-disk entry.
         */
        if (vpack == NULL && S2BT(session)->type == BTREE_COL_FIX) {
            upd_select->upd = tombstone;
            return (0);
        }

        WT_ASSERT_ALWAYS(
          session, vpack != NULL && vpack->type != WT_CELL_DEL, "No on-disk value is found");
        WT_ASSERT_ALWAYS(
          session, !WT_TIME_WINDOW_HAS_PREPARE(&(vpack->tw)), "On-disk value is a prepared update");

        /* Move the pointer to the last update on the update chain. */
        for (last_upd = tombstone; last_upd->next != NULL; last_upd = last_upd->next)
            /* Tombstone is the only non-aborted update on the update chain. */
            WT_ASSERT(session, last_upd->next->txnid == WT_TXN_ABORTED);

        /*
         * It's possible to have a tombstone as the only update in the update list. If we reconciled
         * before with only a single update and then read the page back into cache, we'll have an
         * empty update list. And applying a delete on top of that will result in ONLY a tombstone
         * in the update list.
         *
         * In this case, we should leave the selected update unset to indicate that we want to keep
         * the same on-disk value but set the stop time point to indicate that the validity window
         * ends when this tombstone started. (Note: this may have been true at one point, but
         * currently we either append the onpage value and return that, or return the tombstone
         * itself; there is no case that returns no update but sets the time window.)
         *
         * If the tombstone is restored from the disk except for building delta or the history
         * store, the onpage value and the history store value should have been restored together.
         * Therefore, we should not end up here.
         */
        WT_ASSERT_ALWAYS(session,
          (WT_DELTA_LEAF_ENABLED(session) && !F_ISSET(tombstone, WT_UPDATE_RESTORED_FROM_HS)) ||
            (!F_ISSET(tombstone, WT_UPDATE_RESTORED_FROM_DS | WT_UPDATE_RESTORED_FROM_HS)),
          "A tombstone written to the disk image except for disaggregated storage or history store "
          "should be accompanied by the full value.");
        WT_RET(__rec_append_orig_value(session, page, tombstone, vpack));

        /*
         * We may have updated the global transaction concurrently and the tombstone is now globally
         * visible. In this case, the on page value is not appended. Verify that.
         */
        if (last_upd->next != NULL) {
            WT_ASSERT_ALWAYS(session,
              last_upd->next->txnid ==
                  (F_ISSET(S2C(session), WT_CONN_RECOVERING) ? WT_TXN_NONE : vpack->tw.start_txn) &&
                last_upd->next->upd_start_ts == vpack->tw.start_ts &&
                last_upd->next->type == WT_UPDATE_STANDARD && last_upd->next->next == NULL,
              "Tombstone is globally visible, but the tombstoned update is on the update "
              "chain");
            upd_select->upd = last_upd->next;
            WT_TIME_WINDOW_SET_START(select_tw, last_upd->next, last_upd->next->prepare_state);
        } else {
            /*
             * It's possible that onpage value is not appended if the tombstone becomes globally
             * visible because the oldest transaction id or the oldest timestamp is moved
             * concurrently.
             *
             * If the tombstone is aborted concurrently, we should still have appended the onpage
             * value.
             */
            WT_ASSERT_ALWAYS(session,
              tombstone->txnid != WT_TXN_ABORTED && __wt_txn_upd_visible_all(session, tombstone) &&
                upd_select->upd == NULL,
              "Tombstone has been aborted, but the previously tombstoned update is not on "
              "the update chain");
            upd_select->upd = tombstone;
        }
    }
    return (0);
}

/*
 * __wti_rec_upd_select --
 *     Return the update in a list that should be written (or NULL if none can be written).
 */
int
__wti_rec_upd_select(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_INSERT *ins, WT_ROW *rip,
  WT_CELL_UNPACK_KV *vpack, WTI_UPDATE_SELECT *upd_select)
{
    WT_PAGE *page;
    WT_UPDATE *first_txn_upd, *first_upd, *onpage_upd, *upd;
    size_t upd_memsize;
    bool has_newer_updates, supd_restore, upd_saved;

    /*
     * The "saved updates" return value is used independently of returning an update we can write,
     * both must be initialized.
     */
    WTI_UPDATE_SELECT_INIT(upd_select);

    page = r->page;
    first_txn_upd = onpage_upd = upd = NULL;
    upd_memsize = 0;
    has_newer_updates = supd_restore = upd_saved = false;

    /*
     * If called with a WT_INSERT item, use its WT_UPDATE list (which must exist), otherwise check
     * for an on-page row-store WT_UPDATE list (which may not exist). Return immediately if the item
     * has no updates.
     */
    if (ins != NULL)
        first_upd = ins->upd;
    else {
        /* Note: ins is never null for columns. */
        WT_ASSERT(session, rip != NULL && page->type == WT_PAGE_ROW_LEAF);
        if ((first_upd = WT_ROW_UPDATE(page, rip)) == NULL)
            return (0);
    }

    WT_RET(__rec_upd_select(
      session, r, first_upd, upd_select, &first_txn_upd, &has_newer_updates, &upd_memsize));

    /* Keep track of the selected update. */
    upd = upd_select->upd;

    WT_ASSERT_ALWAYS(session,
      upd == NULL || (upd->txnid != WT_TXN_ABORTED && upd->type != WT_UPDATE_RESERVE),
      "Reconciliation should never see an aborted or reserved update");

    /*
     * The checkpoint transaction is special. Make sure we never write metadata updates from a
     * checkpoint in a concurrent session.
     */
    WT_ASSERT_ALWAYS(session,
      !WT_IS_METADATA(session->dhandle) || upd == NULL || upd->txnid == WT_TXN_NONE ||
        upd->txnid != __wt_atomic_loadv64(&S2C(session)->txn_global.checkpoint_txn_shared.id) ||
        WT_SESSION_IS_CHECKPOINT(session),
      "Metadata updates written from a checkpoint in a concurrent session");

    /* If all of the updates were aborted, quit. */
    if (first_txn_upd == NULL) {
        WT_ASSERT_ALWAYS(session, upd == NULL,
          "__wt_rec_upd_select has selected an update when none are present on the update chain");
        if (first_upd != NULL)
            r->cache_upd_chain_all_aborted = true;
        return (0);
    }

    /*
     * We expect the page to be clean after reconciliation. If there are invisible updates, abort
     * eviction.
     */
    if (has_newer_updates && F_ISSET(r, WT_REC_CLEAN_AFTER_REC | WT_REC_VISIBILITY_ERR)) {
        if (F_ISSET(r, WT_REC_VISIBILITY_ERR))
            WT_RET_PANIC(session, EINVAL, "reconciliation error, update not visible");
        return (__wt_set_return(session, EBUSY));
    }

    /* If an update was selected, record that we're making progress. */
    if (upd != NULL)
        r->update_used = true;

    if (upd != NULL)
        WT_RET(__rec_fill_tw_from_upd_select(session, page, vpack, upd_select));

    /* Mark the page dirty after reconciliation. */
    if (has_newer_updates)
        r->leave_dirty = true;

    onpage_upd = upd_select->upd != NULL && upd_select->upd->type == WT_UPDATE_TOMBSTONE ?
      NULL :
      upd_select->upd;

    /*
     * If we have done a prepared rollback, we may have restored a history store value to the update
     * chain but the same value is left in the history store. Save it to delete it from the history
     * store later.
     */
    WT_RET(__rec_find_and_save_delete_hs_upd(session, r, ins, rip, upd_select));

    /* Check the update chain for conditions that could prevent it's eviction. */
    WT_RET(__rec_validate_upd_chain(session, r, onpage_upd, &upd_select->tw, vpack));

    /*
     * Set the flag if the selected tombstone has no timestamp. Based on this flag, the caller
     * functions perform the history store truncation for this key.
     */
    if (!F_ISSET(session->dhandle, WT_DHANDLE_HS) && upd_select->tombstone != NULL &&
      !F_ISSET(upd_select->tombstone, WT_UPDATE_RESTORED_FROM_DS | WT_UPDATE_RESTORED_FROM_HS)) {
        upd = upd_select->upd;

        /*
         * The selected update can be the tombstone itself when the tombstone is globally visible.
         * Compare the tombstone's timestamp with either the next update in the update list or the
         * on-disk cell timestamp to determine if the tombstone is discarding a timestamp.
         */
        if (upd_select->tombstone == upd) {
            upd = upd->next;

            /* Loop until a valid update is found. */
            while (upd != NULL && upd->txnid == WT_TXN_ABORTED)
                upd = upd->next;
        }

        if ((upd != NULL && upd->upd_start_ts > upd_select->tombstone->upd_start_ts) ||
          (vpack != NULL && vpack->tw.start_ts > upd_select->tombstone->upd_start_ts))
            upd_select->no_ts_tombstone = true;
    }

    /*
     * Fixup any missing timestamps, assert that checkpoint wasn't running when this round of
     * reconciliation started.
     *
     * Returning EBUSY here is okay as the previous call to validate the update chain wouldn't have
     * caught the situation where only a tombstone is selected.
     */
    if (__timestamp_no_ts_fix(session, &upd_select->tw) && F_ISSET(r, WT_REC_HS) &&
      F_ISSET(r, WT_REC_CHECKPOINT_RUNNING)) {
        /* Catch this case in diagnostic builds. */
        WT_STAT_CONN_DSRC_INCR(session, cache_eviction_blocked_no_ts_checkpoint_race_3);
        WT_ASSERT(session, false);
        return (EBUSY);
    }

    /*
     * The update doesn't have any further updates that need to be written to the history store,
     * skip saving the update as saving the update will cause reconciliation to think there is work
     * that needs to be done when there might not be.
     *
     * Additionally history store reconciliation is not set skip saving an update.
     */
    if (__rec_need_save_upd(session, r, upd_select, vpack, has_newer_updates)) {
        /*
         * We should restore the update chains to the new disk image if there are newer updates in
         * eviction, or for cases that don't support history store, such as an in-memory database.
         */
        supd_restore = F_ISSET(r, WT_REC_EVICT) &&
          (has_newer_updates || F_ISSET(S2C(session), WT_CONN_IN_MEMORY) ||
            F_ISSET(S2BT(session), WT_BTREE_IN_MEMORY));

        upd_memsize = __rec_calc_upd_memsize(onpage_upd, upd_select->tombstone, upd_memsize);
        WT_RET(__rec_update_save(session, r, ins, rip, onpage_upd, upd_select->tombstone,
          &upd_select->tw, supd_restore, upd_memsize));
        upd_saved = upd_select->upd_saved = true;
    }

    /*
     * Mark the selected update (and potentially the tombstone preceding it) as being destined for
     * the data store. Subsequent reconciliations should know that they can select this update
     * regardless of visibility.
     */
    if (upd_select->upd != NULL)
        F_SET(upd_select->upd, WT_UPDATE_DS);
    if (upd_select->tombstone != NULL)
        F_SET(upd_select->tombstone, WT_UPDATE_DS);

    /* Track whether we need to do update restore eviction. */
    if (supd_restore)
        r->cache_write_restore_invisible = true;

    /*
     * Paranoia: check that we didn't choose an update that has since been rolled back.
     */
    WT_ASSERT_ALWAYS(session, upd_select->upd == NULL || upd_select->upd->txnid != WT_TXN_ABORTED,
      "Updated selected that has since been rolled back");

    /*
     * Returning an update means the original on-page value might be lost, and that's a problem if
     * there's a reader that needs it, make a copy of the on-page value. We do that any time there
     * are saved updates (we may need the original on-page value to terminate the update chain, for
     * example, in the case of an update that modifies the original value). Additionally, make a
     * copy of the on-page value if the value is an overflow item and anything other than the
     * on-page cell is being written. This is because the value's backing overflow blocks aren't
     * part of the page, and they are physically removed by checkpoint writing this page, that is,
     * the checkpoint doesn't include the overflow blocks so they're removed and future readers of
     * this page won't be able to find them.
     *
     * We never append prepared updates back to the onpage value. If it is a prepared full update,
     * it is already on the update chain. If it is a prepared tombstone, the onpage value is already
     * appended to the update chain when the page is read into memory.
     */
    if (upd_select->upd != NULL && vpack != NULL && vpack->type != WT_CELL_DEL &&
      !WT_TIME_WINDOW_HAS_PREPARE(&(vpack->tw)) &&
      (upd_saved || F_ISSET(vpack, WT_CELL_UNPACK_OVERFLOW)))
        WT_RET(__rec_append_orig_value(session, page, upd_select->upd, vpack));

    __wti_rec_time_window_clear_obsolete(session, upd_select, NULL, r);

    WT_ASSERT(
      session, upd_select->tw.stop_txn != WT_TXN_MAX || upd_select->tw.stop_ts == WT_TS_MAX);

    return (0);
}

/*
 * __wt_rec_in_progress --
 *     Whether we're currently reconciling.
 */
bool
__wt_rec_in_progress(WT_SESSION_IMPL *session)
{
    WTI_RECONCILE *rec = session->reconcile;

    return (!(rec == NULL && rec->ref == NULL));
}
