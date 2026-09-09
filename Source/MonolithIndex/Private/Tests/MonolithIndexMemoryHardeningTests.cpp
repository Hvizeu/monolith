// SPDX-License-Identifier: MIT

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformFileManager.h"
#include "SQLiteDatabase.h"
#include "MonolithCompilerSafeDispatch.h"
#include "MonolithIndexDatabase.h"
#include "MonolithMemoryHelper.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FIndexedActor MakeActor(int64 AssetId, const TCHAR* Name)
	{
		FIndexedActor Actor;
		Actor.AssetId = AssetId;
		Actor.ActorName = Name;
		Actor.ActorClass = TEXT("Actor");
		return Actor;
	}

	bool ExecuteStatement(FMonolithIndexDatabase& Database, const TCHAR* Sql)
	{
		FSQLitePreparedStatement Statement;
		return Statement.Create(*Database.GetRawDatabase(), Sql) && Statement.Execute();
	}

	int64 CountActors(FMonolithIndexDatabase& Database, int64 AssetId)
	{
		FSQLitePreparedStatement Statement;
		if (!Statement.Create(*Database.GetRawDatabase(), TEXT("SELECT COUNT(*) FROM actors WHERE asset_id = ?;")) ||
			!Statement.SetBindingValueByIndex(1, AssetId) ||
			Statement.Step() != ESQLitePreparedStatementStepResult::Row)
		{
			return -1;
		}

		int64 Count = -1;
		Statement.GetColumnValueByIndex(0, Count);
		return Count;
	}

	FString FirstActorName(FMonolithIndexDatabase& Database, int64 AssetId)
	{
		FSQLitePreparedStatement Statement;
		if (!Statement.Create(*Database.GetRawDatabase(), TEXT("SELECT actor_name FROM actors WHERE asset_id = ? ORDER BY id LIMIT 1;")) ||
			!Statement.SetBindingValueByIndex(1, AssetId) ||
			Statement.Step() != ESQLitePreparedStatementStepResult::Row)
		{
			return FString();
		}

		FString Name;
		Statement.GetColumnValueByIndex(0, Name);
		return Name;
	}

	bool DeleteActorsInsideOpenTransaction(FMonolithIndexDatabase& Database, int64 AssetId)
	{
		FSQLitePreparedStatement Statement;
		return Statement.Create(*Database.GetRawDatabase(), TEXT("DELETE FROM actors WHERE asset_id = ?;")) &&
			Statement.SetBindingValueByIndex(1, AssetId) && Statement.Execute();
	}

	FString MakeDatabasePath(const TCHAR* Prefix)
	{
		return FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("MonolithTests"),
			FString::Printf(TEXT("%s_%s.db"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	void DeleteDatabaseFiles(const FString& DbPath)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		PlatformFile.DeleteFile(*DbPath);
		PlatformFile.DeleteFile(*(DbPath + TEXT("-journal")));
		PlatformFile.DeleteFile(*(DbPath + TEXT("-wal")));
		PlatformFile.DeleteFile(*(DbPath + TEXT("-shm")));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMemoryPressureClassificationTest,
	"Monolith.Index.Memory.PressureClassification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMemoryPressureClassificationTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithMemorySnapshot Snapshot;
	Snapshot.TotalPhysicalMB = 32768;
	Snapshot.AvailablePhysicalMB = 8192;
	Snapshot.ProcessPrivateMB = 16384;

	TestEqual(TEXT("the process budget boundary is admitted"),
		FMonolithMemoryHelper::ClassifyMemoryPressure(Snapshot, 16384),
		EMonolithMemoryPressure::None);

	Snapshot.ProcessPrivateMB = 16385;
	TestEqual(TEXT("one MB over the process budget is soft pressure"),
		FMonolithMemoryHelper::ClassifyMemoryPressure(Snapshot, 16384),
		EMonolithMemoryPressure::Soft);

	Snapshot.ProcessPrivateMB = 8000;
	Snapshot.AvailablePhysicalMB = 4095;
	TestEqual(TEXT("below the soft system headroom boundary is soft pressure"),
		FMonolithMemoryHelper::ClassifyMemoryPressure(Snapshot, 16384),
		EMonolithMemoryPressure::Soft);

	Snapshot.AvailablePhysicalMB = 2047;
	TestEqual(TEXT("below the critical system headroom boundary is critical"),
		FMonolithMemoryHelper::ClassifyMemoryPressure(Snapshot, 16384),
		EMonolithMemoryPressure::Critical);

	Snapshot.AvailablePhysicalMB = 8192;
	Snapshot.bHasGPUStats = true;
	Snapshot.GPUBudgetMB = 8192;
	Snapshot.GPUUsedMB = 6964;
	TestEqual(TEXT("the 85 percent GPU boundary is soft pressure"),
		FMonolithMemoryHelper::ClassifyMemoryPressure(Snapshot, 16384),
		EMonolithMemoryPressure::Soft);

	Snapshot.GPUUsedMB = 7374;
	TestEqual(TEXT("GPU headroom below ten percent is critical"),
		FMonolithMemoryHelper::ClassifyMemoryPressure(Snapshot, 16384),
		EMonolithMemoryPressure::Critical);

	Snapshot.bHasGPUStats = false;
	Snapshot.GPUUsedMB = 8192;
	TestEqual(TEXT("unavailable GPU telemetry is ignored"),
		FMonolithMemoryHelper::ClassifyMemoryPressure(Snapshot, 16384),
		EMonolithMemoryPressure::None);

	Snapshot.AvailablePhysicalMB = 1500;
	TestEqual(TEXT("sustained critical system pressure remains blocking"),
		FMonolithMemoryHelper::ClassifyMemoryPressure(Snapshot, 16384),
		EMonolithMemoryPressure::Critical);
	Snapshot.AvailablePhysicalMB = 8192;
	TestEqual(TEXT("pressure relieved after cleanup admits the next load"),
		FMonolithMemoryHelper::ClassifyMemoryPressure(Snapshot, 16384),
		EMonolithMemoryPressure::None);

	TestEqual(TEXT("first soft-pressure observation requests cleanup and a new tick"),
		FMonolithMemoryHelper::GetPressureAction(EMonolithMemoryPressure::Soft, false),
		EMonolithMemoryPressureAction::CleanupAndReassess);
	TestEqual(TEXT("soft pressure after cleanup can proceed"),
		FMonolithMemoryHelper::GetPressureAction(EMonolithMemoryPressure::Soft, true),
		EMonolithMemoryPressureAction::Proceed);
	TestEqual(TEXT("first critical-pressure observation requests cleanup and a new tick"),
		FMonolithMemoryHelper::GetPressureAction(EMonolithMemoryPressure::Critical, false),
		EMonolithMemoryPressureAction::CleanupAndReassess);
	TestEqual(TEXT("sustained critical pressure blocks the next load"),
		FMonolithMemoryHelper::GetPressureAction(EMonolithMemoryPressure::Critical, true),
		EMonolithMemoryPressureAction::Stop);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMemoryTierRoundingTest,
	"Monolith.Index.Memory.RamTierRounding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMemoryTierRoundingTest::RunTest(const FString& /*Parameters*/)
{
	constexpr uint64 GiB = 1024ULL * 1024ULL * 1024ULL;
	TestEqual(TEXT("a hardware-reported 31.817 GiB enters the 32 GB tier"),
		FMonolithMemoryHelper::RoundPhysicalBytesToRamGB(34163589120ULL), 32);
	TestEqual(TEXT("15.75 GiB enters the 16 GB tier"),
		FMonolithMemoryHelper::RoundPhysicalBytesToRamGB(15ULL * GiB + 3ULL * GiB / 4ULL), 16);
	TestEqual(TEXT("15.25 GiB remains below the 16 GB tier"),
		FMonolithMemoryHelper::RoundPhysicalBytesToRamGB(15ULL * GiB + GiB / 4ULL), 15);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithLevelRowsReplacementTest,
	"Monolith.Index.Memory.LevelRowsReplacement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelRowsReplacementTest::RunTest(const FString& /*Parameters*/)
{
	const FString DbPath = MakeDatabasePath(TEXT("LevelRows"));
	FMonolithIndexDatabase Database;
	if (!TestTrue(TEXT("fixture database opens"), Database.Open(DbPath)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Database.Close();
		DeleteDatabaseFiles(DbPath);
	};

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Tests/Monolith/LevelFixture");
	Asset.AssetName = TEXT("LevelFixture");
	Asset.AssetClass = TEXT("World");
	Asset.ModuleName = TEXT("Game");
	const int64 AssetId = Database.InsertAsset(Asset);

	FIndexedAsset OtherAsset = Asset;
	OtherAsset.PackagePath = TEXT("/Game/Tests/Monolith/OtherLevelFixture");
	OtherAsset.AssetName = TEXT("OtherLevelFixture");
	const int64 OtherAssetId = Database.InsertAsset(OtherAsset);
	if (!TestTrue(TEXT("fixture worlds insert"), AssetId > 0 && OtherAssetId > 0))
	{
		return false;
	}

	TestTrue(TEXT("the untouched world receives a baseline row"),
		Database.ReplaceActorsForAsset(OtherAssetId, {MakeActor(OtherAssetId, TEXT("Untouched"))}));
	TestTrue(TEXT("the first replacement succeeds"),
		Database.ReplaceActorsForAsset(AssetId, {MakeActor(AssetId, TEXT("First"))}));
	TestTrue(TEXT("a repeated replacement succeeds"),
		Database.ReplaceActorsForAsset(AssetId,
			{MakeActor(AssetId, TEXT("SecondA")), MakeActor(AssetId, TEXT("SecondB"))}));
	TestEqual(TEXT("repeated replacement does not duplicate rows"), CountActors(Database, AssetId), static_cast<int64>(2));
	TestEqual(TEXT("another world's rows remain untouched"), CountActors(Database, OtherAssetId), static_cast<int64>(1));

	TestTrue(TEXT("an inspected empty world commits an empty replacement"),
		Database.ReplaceActorsForAsset(AssetId, {}));
	TestEqual(TEXT("an empty world clears its old rows"), CountActors(Database, AssetId), static_cast<int64>(0));
	TestEqual(TEXT("empty replacement still preserves another world"), CountActors(Database, OtherAssetId), static_cast<int64>(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithLevelRowsFailureAtomicityTest,
	"Monolith.Index.Memory.LevelRowsFailureAtomicity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelRowsFailureAtomicityTest::RunTest(const FString& /*Parameters*/)
{
	const FString DbPath = MakeDatabasePath(TEXT("LevelRowsFailure"));
	FMonolithIndexDatabase Database;
	if (!TestTrue(TEXT("fixture database opens"), Database.Open(DbPath)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Database.Close();
		DeleteDatabaseFiles(DbPath);
	};

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Tests/Monolith/AtomicLevelFixture");
	Asset.AssetName = TEXT("AtomicLevelFixture");
	Asset.AssetClass = TEXT("World");
	Asset.ModuleName = TEXT("Game");
	const int64 AssetId = Database.InsertAsset(Asset);
	if (!TestTrue(TEXT("fixture world inserts"), AssetId > 0) ||
		!TestTrue(TEXT("baseline replacement succeeds"),
			Database.ReplaceActorsForAsset(AssetId, {MakeActor(AssetId, TEXT("OldComplete"))})))
	{
		return false;
	}

	TestTrue(TEXT("insertion-failure trigger installs"), ExecuteStatement(Database,
		TEXT("CREATE TRIGGER monolith_test_fail_insert BEFORE INSERT ON actors "
			"WHEN NEW.actor_name = 'RejectInsert' BEGIN SELECT RAISE(ABORT, 'forced insert failure'); END;")));
	TestFalse(TEXT("a failed insertion rejects the replacement"),
		Database.ReplaceActorsForAsset(AssetId, {MakeActor(AssetId, TEXT("RejectInsert"))}));
	TestEqual(TEXT("a failed insertion preserves the old complete row"), FirstActorName(Database, AssetId), FString(TEXT("OldComplete")));
	TestTrue(TEXT("insertion-failure trigger drops"), ExecuteStatement(Database, TEXT("DROP TRIGGER monolith_test_fail_insert;")));

	TestTrue(TEXT("deferred commit-failure parent table installs"), ExecuteStatement(Database,
		TEXT("CREATE TABLE monolith_test_parent(id INTEGER PRIMARY KEY);")));
	TestTrue(TEXT("deferred commit-failure child table installs"), ExecuteStatement(Database,
		TEXT("CREATE TABLE monolith_test_deferred(id INTEGER REFERENCES monolith_test_parent(id) DEFERRABLE INITIALLY DEFERRED);")));
	TestTrue(TEXT("commit-failure trigger installs"), ExecuteStatement(Database,
		TEXT("CREATE TRIGGER monolith_test_fail_commit AFTER INSERT ON actors "
			"WHEN NEW.actor_name = 'RejectCommit' BEGIN INSERT INTO monolith_test_deferred(id) VALUES (1); END;")));
	AddExpectedError(TEXT("SQL execution failed: FOREIGN KEY constraint failed"),
		EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("a failed commit rejects the replacement"),
		Database.ReplaceActorsForAsset(AssetId, {MakeActor(AssetId, TEXT("RejectCommit"))}));
	TestEqual(TEXT("a failed commit preserves the old complete row"), FirstActorName(Database, AssetId), FString(TEXT("OldComplete")));
	TestTrue(TEXT("commit-failure trigger drops"), ExecuteStatement(Database, TEXT("DROP TRIGGER monolith_test_fail_commit;")));

	TestTrue(TEXT("interrupted replacement transaction begins"), Database.BeginTransaction());
	TestTrue(TEXT("interrupted replacement clears inside its uncommitted transaction"),
		DeleteActorsInsideOpenTransaction(Database, AssetId));
	TestTrue(TEXT("interrupted replacement inserts a candidate row"),
		Database.InsertActor(MakeActor(AssetId, TEXT("InterruptedNew"))) > 0);
	Database.Close();
	if (!TestTrue(TEXT("database reopens after the interrupted transaction"), Database.Open(DbPath)))
	{
		return false;
	}
	TestEqual(TEXT("reopen observes the old complete row, never the partial candidate"),
		FirstActorName(Database, AssetId), FString(TEXT("OldComplete")));
	TestEqual(TEXT("reopen observes exactly one complete row"), CountActors(Database, AssetId), static_cast<int64>(1));

	FSQLitePreparedStatement IntegrityStatement;
	if (!TestTrue(TEXT("SQLite quick_check prepares"),
		IntegrityStatement.Create(*Database.GetRawDatabase(), TEXT("PRAGMA quick_check;"))) ||
		!TestEqual(TEXT("SQLite quick_check returns a row"),
			IntegrityStatement.Step(), ESQLitePreparedStatementStepResult::Row))
	{
		return false;
	}
	FString IntegrityResult;
	IntegrityStatement.GetColumnValueByIndex(0, IntegrityResult);
	TestEqual(TEXT("SQLite integrity remains valid"), IntegrityResult, FString(TEXT("ok")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDispatchAbortTokenTest,
	"Monolith.Index.Shutdown.DispatchAbortToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDispatchAbortTokenTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithDispatchAbortToken AbortFirst;
	TestTrue(TEXT("an unclaimed dispatch can be withdrawn"), AbortFirst.AbortIfUnclaimed());
	TestTrue(TEXT("a withdrawn dispatch reports aborted"), AbortFirst.IsAborted());
	TestFalse(TEXT("the game thread cannot claim a withdrawn dispatch"), AbortFirst.ClaimForExecution());

	FMonolithDispatchAbortToken ClaimFirst;
	TestTrue(TEXT("the game thread can claim an unclaimed dispatch"), ClaimFirst.ClaimForExecution());
	TestFalse(TEXT("a claimed dispatch cannot be withdrawn while its payload uses worker state"), ClaimFirst.AbortIfUnclaimed());
	TestFalse(TEXT("a claimed dispatch is not reported as aborted"), ClaimFirst.IsAborted());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
