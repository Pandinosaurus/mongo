/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/catalog_shard_feature_flag_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/database_sharding_runtime.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_migration_critical_section.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/flush_database_cache_updates_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/future.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

/**
 * Inserts a database collection entry with fixed metadata for the `config` or `admin` database. If
 * the entry key already exists, it's not updated.
 */
Status insertDatabaseEntryForBackwardCompatibility(OperationContext* opCtx,
                                                   const DatabaseName& dbName) {
    invariant(dbName == DatabaseName::kAdmin || dbName == DatabaseName::kConfig);

    DBDirectClient client(opCtx);
    auto commandResponse = client.runCommand([&] {
        auto dbMetadata =
            DatabaseType(dbName, ShardId::kConfigServerId, DatabaseVersion::makeFixed());

        write_ops::InsertCommandRequest insertOp(NamespaceString::kConfigCacheDatabasesNamespace);
        insertOp.setDocuments({dbMetadata.toBSON()});
        return insertOp.serialize();
    }());

    auto commandStatus = getStatusFromWriteCommandReply(commandResponse->getCommandReply());
    return commandStatus.code() == ErrorCodes::DuplicateKey ? Status::OK() : commandStatus;
}

template <typename Derived>
class FlushDatabaseCacheUpdatesCmdBase : public TypedCommand<Derived> {
public:
    std::string help() const override {
        return "Internal command which waits for any pending routing table cache updates for a "
               "particular database to be written locally. The operationTime returned in the "
               "response metadata is guaranteed to be at least as late as the last routing table "
               "cache update to the local disk. Takes a 'forceRemoteRefresh' option to make this "
               "node refresh its cache from the config server before waiting for the last refresh "
               "to be persisted.";
    }

    /**
     * We accept any apiVersion, apiStrict, and/or apiDeprecationErrors forwarded with this internal
     * command.
     */
    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    bool adminOnly() const override {
        return true;
    }

    Command::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    class Invocation final : public TypedCommand<Derived>::InvocationBase {
    public:
        using Base = typename TypedCommand<Derived>::InvocationBase;
        using Base::Base;

        /**
         * ns() is the database to flush, with no collection.
         */
        NamespaceString ns() const override {
            return NamespaceString(_dbName());
        }

        bool supportsWriteConcern() const override {
            return Derived::supportsWriteConcern();
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(
                ErrorCodes::Unauthorized,
                "Unauthorized",
                AuthorizationSession::get(opCtx->getClient())
                    ->isAuthorizedForActionsOnResource(
                        ResourcePattern::forClusterResource(Base::request().getDbName().tenantId()),
                        ActionType::internal));
        }

        void typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

            uassert(ErrorCodes::IllegalOperation,
                    "Can't issue _flushDatabaseCacheUpdates from 'eval'",
                    !opCtx->getClient()->isInDirectClient());

            uassert(ErrorCodes::IllegalOperation,
                    "Can't call _flushDatabaseCacheUpdates if in read-only mode",
                    !opCtx->readOnly());

            const auto dbName = _dbName();

            tassert(10250100,
                    "The admin and config databases have fixed metadata that does not need to be "
                    "refreshed",
                    !dbName.isAdminDB() && !dbName.isConfigDB());

            if (feature_flags::gShardAuthoritativeDbMetadataCRUD.isEnabled(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                // When the `ShardAuthoritativeDbMetadataCRUD` feature flag is enabled, this method
                // should no longer be used, as nodes start relying on the `config.shard.catalog`
                // authoritative collections rather than the config server (primary node) or
                // contacting the primary to replicate filtering metadata in the `config.cache`
                // collections (secondary nodes).
                //
                // However, there is a scenario where a lagging secondary node attempts to contact
                // the primary node of the replica set while the secondary is still operating
                // under 8.0 FCV, but the primary has already transitioned to 9.0 FCV. In this case,
                // the lagging secondary will attempt to refresh using this command as part of the
                // old protocol.
                //
                // In that situation, we will first make the secondary node wait for the latest
                // opTime so that it becomes aware of the FCV change. This is no worse than the
                // current behavior, as the existing protocol also relies on the `config.cache`
                // collections for replication.
                //
                // After that, the command will fail, making the secondary aware that it needs
                // to switch to the authoritative model.

                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTime(opCtx);

                uasserted(ErrorCodes::DatabaseMetadataRefreshCanceledDueToFCVTransition,
                          "This command is deprecated, as shards are authoritative for database "
                          "metadata. The secondary node must transition to the authoritative "
                          "refresh model.");
            }

            boost::optional<SharedSemiFuture<void>> criticalSectionSignal;

            {
                // If the primary is in the critical section, secondaries must wait for the commit
                // to finish on the primary in case a secondary's caller has an afterClusterTime
                // inclusive of the commit (and new writes to the committed chunk) that hasn't yet
                // propagated back to this shard. This ensures the read your own writes causal
                // consistency guarantee.
                const auto scopedDsr = DatabaseShardingRuntime::acquireShared(opCtx, dbName);
                criticalSectionSignal =
                    scopedDsr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite);
            }

            if (criticalSectionSignal)
                criticalSectionSignal->get(opCtx);

            if (Base::request().getSyncFromConfig()) {
                LOGV2_DEBUG(21981, 1, "Forcing remote routing table refresh", "db"_attr = dbName);
                uassertStatusOK(
                    FilteringMetadataCache::get(opCtx)->forceDatabaseMetadataRefresh_DEPRECATED(
                        opCtx, dbName));
            }

            FilteringMetadataCache::get(opCtx)->waitForDatabaseFlush(opCtx, dbName);

            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        }

    private:
        DatabaseName _dbName() const {
            return DatabaseNameUtil::deserialize(boost::none,
                                                 Base::request().getCommandParameter(),
                                                 Base::request().getSerializationContext());
        }
    };
};

class FlushDatabaseCacheUpdatesCmd final
    : public FlushDatabaseCacheUpdatesCmdBase<FlushDatabaseCacheUpdatesCmd> {
public:
    using Request = FlushDatabaseCacheUpdates;

    static bool supportsWriteConcern() {
        return false;
    }
};
MONGO_REGISTER_COMMAND(FlushDatabaseCacheUpdatesCmd).forShard();

class FlushDatabaseCacheUpdatesWithWriteConcernCmd final
    : public FlushDatabaseCacheUpdatesCmdBase<FlushDatabaseCacheUpdatesWithWriteConcernCmd> {
public:
    using Request = FlushDatabaseCacheUpdatesWithWriteConcern;

    static bool supportsWriteConcern() {
        return true;
    }
};
MONGO_REGISTER_COMMAND(FlushDatabaseCacheUpdatesWithWriteConcernCmd).forShard();

}  // namespace
}  // namespace mongo
