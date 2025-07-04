import {
    getCollectionName,
    getCommandName,
    getExplainCommand,
    getInnerCommand,
    isInternalDbName,
    isSystemCollectionName
} from "jstests/libs/cmd_object_utils.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {
    everyWinningPlan,
    getNestedProperties,
    isIdhackOrExpress
} from "jstests/libs/query/analyze_plan.js";
import {QuerySettingsIndexHintsTests} from "jstests/libs/query/query_settings_index_hints_tests.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

/**
 * Override which applies 'bad' query settings over supported commands in order to test the fallback
 * mechanism. Asserts that the query plans generated by the fallback should be identical to those
 * generated without any query settings.
 */
function runCommandOverride(conn, dbName, _cmdName, cmdObj, clientFunction, makeFuncArgs) {
    let resultWithQuerySettings = undefined;
    const computeAndStoreResult = () => {
        resultWithQuerySettings = clientFunction.apply(conn, makeFuncArgs(cmdObj));
    };

    const assertFallbackPlanMatchesOriginalPlan = () => {
        // Initialize the 'db' instance.
        const db = (() => {
            if (isInternalDbName(dbName)) {
                // Query settings cannot be set over internal databases.
                return undefined;
            }
            try {
                return conn.getDB(dbName);
            } catch (e) {
                // Bail when faced with 'getDB()' exceptions, such as invalid database names.
                return undefined;
            }
        })();
        if (!db) {
            return;
        }

        const innerCmd = getInnerCommand(cmdObj);
        if (!QuerySettingsUtils.isSupportedCommand(getCommandName(innerCmd))) {
            return;
        }

        const explain = db.runCommand(getExplainCommand(innerCmd));
        if (!explain.ok) {
            // Some commands such as $collStats cannot be explained and will lead to failures.
            return;
        }

        if (explain.hasOwnProperty("warning")) {
            // The explain output exceeded the 16MB BSON size limit and was truncated.
            return;
        }

        // If the query explain has no 'winningPlan', we can not assert for query settings
        // fallback.
        if (getNestedProperties(explain, "winningPlan").length === 0) {
            return;
        }

        const isIdHackQuery =
            everyWinningPlan(explain, (winningPlan) => isIdhackOrExpress(db, winningPlan));
        if (isIdHackQuery) {
            // Query settings cannot be applied over IDHACK or Express queries.
            return;
        }

        const collectionName = getCollectionName(db, innerCmd);
        if (!collectionName || isSystemCollectionName(collectionName)) {
            // Can't test the fallback on queries not involving any collections or queries targeting
            // the system collection:
            // - Queries not involving any collections will always yield to EOF plans.
            // - Query settings validate against queries targeting system collections.
            return;
        }

        if (innerCmd.hasOwnProperty("rawData")) {
            // TODO(SERVER-106438): Review interaction of setQuerySettings with rawData
            // Query settings can't be applied on rawData queries
            return;
        }

        const ns = {db: dbName, coll: collectionName};
        const qsutils = new QuerySettingsUtils(db, collectionName);
        qsutils.onSetQuerySettings(computeAndStoreResult);
        const qstests = new QuerySettingsIndexHintsTests(qsutils);
        const representativeQuery = qsutils.makeQueryInstance(innerCmd);
        qstests.assertQuerySettingsFallback(representativeQuery, ns, explain);
    };
    OverrideHelpers.withPreOverrideRunCommand(assertFallbackPlanMatchesOriginalPlan);
    return resultWithQuerySettings || clientFunction.apply(conn, makeFuncArgs(cmdObj));
}

// Override the default runCommand with our custom version.
OverrideHelpers.overrideRunCommand(runCommandOverride);

// Always apply the override if a test spawns a parallel shell.
OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/implicit_query_settings_fallback.js");
