#include "Indexers/LevelIndexer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "AssetCompilingManager.h"
#include "MonolithMemoryHelper.h"
#include "MonolithSettings.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/Set.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "EngineUtils.h"
#include "Components/ActorComponent.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "WorldPartition/WorldPartition.h"
#include "Subsystems/WorldSubsystem.h"
#include "Misc/ScopeLock.h"
#include "Runtime/Launch/Resources/Version.h"

namespace
{
	const TCHAR* LevelIndexStateMetaKey = TEXT("level_index_state");

	void ForEachPackageObject(UPackage* Package, TFunctionRef<bool(UObject*)> Operation)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
		ForEachObjectWithPackage(Package, Operation, EGetObjectsFlags::IncludeNestedObjects);
#else
		ForEachObjectWithPackage(Package, Operation, true);
#endif
	}

	class FScopedPackageLoadCapture
	{
	public:
		FScopedPackageLoadCapture()
		{
			Handle = FCoreUObjectDelegates::PackageCreatedForLoad.AddRaw(this, &FScopedPackageLoadCapture::OnPackageCreated);
		}

		~FScopedPackageLoadCapture()
		{
			Stop();
		}

		TArray<UPackage*> Stop()
		{
			if (Handle.IsValid())
			{
				FCoreUObjectDelegates::PackageCreatedForLoad.Remove(Handle);
				Handle.Reset();
			}

			FScopeLock Lock(&Mutex);
			TArray<UPackage*> Result;
			for (const TWeakObjectPtr<UPackage>& WeakPackage : CapturedPackages)
			{
				if (UPackage* Package = WeakPackage.Get())
				{
					Result.AddUnique(Package);
				}
			}
			return Result;
		}

	private:
		void OnPackageCreated(UPackage* Package)
		{
			if (Package && Package != GetTransientPackage())
			{
				FScopeLock Lock(&Mutex);
				CapturedPackages.Add(Package);
			}
		}

		FCriticalSection Mutex;
		FDelegateHandle Handle;
		TArray<TWeakObjectPtr<UPackage>> CapturedPackages;
	};

	void GatherPackageObjects(const TArray<UPackage*>& Packages, TArray<UObject*>& OutObjects)
	{
		for (UPackage* Package : Packages)
		{
			if (!Package)
			{
				continue;
			}
			ForEachPackageObject(Package, [&OutObjects](UObject* Object)
			{
				if (Object)
				{
					OutObjects.AddUnique(Object);
				}
				return true;
			});
		}
	}

	void PrepareWorldForIndexingUnload(UWorld* World)
	{
		if (!World || (GEditor && World == GEditor->GetEditorWorldContext().World()))
		{
			return;
		}

		static UClass* LandscapeSubsystemClass = FindObject<UClass>(nullptr, TEXT("/Script/Landscape.LandscapeSubsystem"));
		const bool bContainsLandscape = LandscapeSubsystemClass && World->GetSubsystemBase(LandscapeSubsystemClass) != nullptr;
		if (bContainsLandscape)
		{
			static UClass* LandscapeProxyClass = FindObject<UClass>(nullptr, TEXT("/Script/Landscape.LandscapeProxy"));
			if (LandscapeProxyClass)
			{
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (AActor* Actor = *It; Actor && Actor->IsA(LandscapeProxyClass))
					{
						Actor->UnregisterAllComponents();
					}
				}
			}
			World->CleanupWorld();
			return;
		}

		if (UWorldPartition* WorldPartition = World->GetWorldPartition())
		{
			if (WorldPartition->IsInitialized())
			{
				WorldPartition->Uninitialize();
			}
		}
	}

	const TCHAR* PressureName(EMonolithMemoryPressure Pressure)
	{
		switch (Pressure)
		{
		case EMonolithMemoryPressure::Soft: return TEXT("soft");
		case EMonolithMemoryPressure::Critical: return TEXT("critical");
		default: return TEXT("none");
		}
	}

	FString DescribePressure(const FString& Position, const FName& PackageName, const FMonolithMemorySnapshot& Snapshot)
	{
		return FString::Printf(
			TEXT("critical memory pressure %s %s (process=%llu MB, available RAM=%llu MB, GPU=%llu/%llu MB%s)"),
			*Position, *PackageName.ToString(), Snapshot.ProcessPrivateMB, Snapshot.AvailablePhysicalMB,
			Snapshot.GPUUsedMB, Snapshot.GPUBudgetMB,
			Snapshot.bHasGPUStats ? TEXT("") : TEXT(", GPU stats unavailable"));
	}

	bool IsReleaseCandidate(UPackage* Package, const TSet<UPackage*>& IndexingOwnedPackages)
	{
		return Package && IndexingOwnedPackages.Contains(Package) && Package != GetTransientPackage() &&
			!Package->IsRooted() && !Package->IsDirty() &&
			!Package->HasAnyPackageFlags(PKG_ContainsScript | PKG_CompiledIn);
	}

	bool CanTearDownWorld(UWorld* World, const TSet<UPackage*>& IndexingOwnedPackages)
	{
		if (!World || World->IsRooted() || (GEditor && World == GEditor->GetEditorWorldContext().World()))
		{
			return false;
		}

		UPackage* Package = World->GetOutermost();
		return IsReleaseCandidate(Package, IndexingOwnedPackages);
	}
}

bool FLevelIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!BeginIndex(DB))
	{
		return false;
	}

	for (;;)
	{
		const EMonolithLevelIndexStepResult Result = ProcessNextWorld(DB);
		if (Result == EMonolithLevelIndexStepResult::Continue)
		{
			continue;
		}
		if (Result == EMonolithLevelIndexStepResult::Complete)
		{
			return FinishIndex(DB);
		}

		ResetSession();
		return false;
	}
}

bool FLevelIndexer::BeginIndex(FMonolithIndexDatabase& DB)
{
	ResetSession();
	if (!IsInGameThread())
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("LevelIndexer: BeginIndex must run on the game thread"));
		return false;
	}

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Find all World assets under indexed paths
	FARFilter Filter;
	if (IndexedPaths.Num() > 0)
	{
		for (const FName& Path : IndexedPaths)
		{
			Filter.PackagePaths.Add(Path);
		}
	}
	else
	{
		Filter.PackagePaths.Add(FName(TEXT("/Game")));
	}
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
	Registry.GetAssets(Filter, WorldAssets);
	WorldAssets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.PackageName.LexicalLess(B.PackageName);
	});

	// Maps are handled separately from bounded post-pass batches because one load
	// can bring in sublevels and render resources.
	for (const FAssetData& WorldData : WorldAssets)
	{
		const int64 LevelAssetId = DB.GetAssetId(WorldData.PackageName.ToString());
		if (LevelAssetId >= 0)
		{
			CandidateAssetIds.Add(WorldData.PackageName, LevelAssetId);
		}
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("LevelIndexer: %d World assets queued; loading one root map per dispatch"),
		CandidateAssetIds.Num());

	if (GetDefault<UMonolithSettings>()->bLogMemoryStats)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("LevelIndexer start"));
	}

	bSessionInitialized = true;
	if (!PersistState(DB, TEXT("in_progress")))
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("LevelIndexer: failed to persist the initial level-pass state"));
		ResetSession();
		return false;
	}
	return true;
}

EMonolithLevelIndexStepResult FLevelIndexer::ProcessNextWorld(FMonolithIndexDatabase& DB)
{
	if (!bSessionInitialized || !IsInGameThread())
	{
		return EMonolithLevelIndexStepResult::Failed;
	}

	while (NextWorldIndex < WorldAssets.Num() &&
		(!CandidateAssetIds.Contains(WorldAssets[NextWorldIndex].PackageName) ||
			ProcessedPackages.Contains(WorldAssets[NextWorldIndex].PackageName)))
	{
		++NextWorldIndex;
	}
	if (NextWorldIndex >= WorldAssets.Num())
	{
		return EMonolithLevelIndexStepResult::Complete;
	}

	const FAssetData& WorldData = WorldAssets[NextWorldIndex];
	const SIZE_T MemoryBudgetMB = static_cast<SIZE_T>(FMonolithMemoryHelper::GetResolvedMemoryBudgetMB());
	FMonolithMemorySnapshot Snapshot = FMonolithMemoryHelper::CaptureMemorySnapshot(true);
	EMonolithMemoryPressure Pressure = FMonolithMemoryHelper::ClassifyMemoryPressure(Snapshot, MemoryBudgetMB);
	const EMonolithMemoryPressureAction PressureAction =
		FMonolithMemoryHelper::GetPressureAction(Pressure, bPressureCleanupAttempted);
	if (PressureAction == EMonolithMemoryPressureAction::CleanupAndReassess)
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("LevelIndexer: %s memory pressure before '%s'; cleaning up before reassessment"),
			PressureName(Pressure), *WorldData.PackageName.ToString());
		FMonolithMemoryHelper::ForceGarbageCollection(true);
		bPressureCleanupAttempted = true;
		if (!PersistState(DB, TEXT("in_progress"), TEXT("waiting for memory cleanup")))
		{
			return EMonolithLevelIndexStepResult::Failed;
		}
		return EMonolithLevelIndexStepResult::Continue;
	}
	if (PressureAction == EMonolithMemoryPressureAction::Stop)
	{
		TerminalReason = DescribePressure(TEXT("before"), WorldData.PackageName, Snapshot);
		if (!PersistState(DB, TEXT("degraded"), TerminalReason))
		{
			return EMonolithLevelIndexStepResult::Failed;
		}
		UE_LOG(LogMonolithIndex, Error, TEXT("LevelIndexer: %s"), *TerminalReason);
		return EMonolithLevelIndexStepResult::Degraded;
	}
	bPressureCleanupAttempted = false;
	++NextWorldIndex;

	FlushAsyncLoading();
	const bool bWasAlreadyLoaded = FindPackage(nullptr, *WorldData.PackageName.ToString()) != nullptr;
	FScopedPackageLoadCapture LoadCapture;
	UPackage* Package = LoadPackage(nullptr, *WorldData.PackageName.ToString(), LOAD_NoWarn | LOAD_Quiet | LOAD_EditorOnly);
	TArray<UPackage*> NewlyLoadedPackages = LoadCapture.Stop();
	if (Package && !bWasAlreadyLoaded)
	{
		NewlyLoadedPackages.AddUnique(Package);
	}

	TArray<UPackage*> PackagesToInspect = NewlyLoadedPackages;
	if (Package)
	{
		PackagesToInspect.AddUnique(Package);
	}
	TArray<UObject*> ObjectsLoadedForIndexing;
	GatherPackageObjects(PackagesToInspect, ObjectsLoadedForIndexing);
	if (ObjectsLoadedForIndexing.Num() > 0)
	{
		FAssetCompilingManager::Get().FinishCompilationForObjects(ObjectsLoadedForIndexing);
	}

	TArray<UWorld*> LoadedWorlds;
	for (UObject* Object : ObjectsLoadedForIndexing)
	{
		if (UWorld* World = Cast<UWorld>(Object))
		{
			LoadedWorlds.AddUnique(World);
		}
	}

	bool bDatabaseFailure = false;
	if (!Package)
	{
		++LevelsFailed;
		ProcessedPackages.Add(WorldData.PackageName);
	}
	else
	{
		for (UWorld* World : LoadedWorlds)
		{
			if (!World)
			{
				continue;
			}

			const FName PackageName(*World->GetOutermost()->GetName());
			const int64* LevelAssetId = CandidateAssetIds.Find(PackageName);
			if (!LevelAssetId || ProcessedPackages.Contains(PackageName))
			{
				continue;
			}

			if (!World->PersistentLevel)
			{
				ProcessedPackages.Add(PackageName);
				++LevelsFailed;
				continue;
			}

			TArray<FIndexedActor> Actors;
			BuildActorRows(World->PersistentLevel, *LevelAssetId, Actors);
			if (!DB.ReplaceActorsForAsset(*LevelAssetId, Actors))
			{
				TerminalReason = FString::Printf(TEXT("actor row replacement failed for %s"), *PackageName.ToString());
				bDatabaseFailure = true;
				break;
			}

			ProcessedPackages.Add(PackageName);
			ActorsInserted += Actors.Num();
			++LevelsProcessed;
		}

		if (!bDatabaseFailure && !ProcessedPackages.Contains(WorldData.PackageName))
		{
			ProcessedPackages.Add(WorldData.PackageName);
			++LevelsFailed;
		}
	}

	TSet<UPackage*> NewlyLoadedSet;
	NewlyLoadedSet.Append(NewlyLoadedPackages);
	TArray<TWeakObjectPtr<UPackage>> ReleaseCandidateRefs;
	TArray<TWeakObjectPtr<UWorld>> ProtectedWorldRefs;
	for (UPackage* LoadedPackage : NewlyLoadedPackages)
	{
		if (IsReleaseCandidate(LoadedPackage, NewlyLoadedSet))
		{
			ReleaseCandidateRefs.Add(LoadedPackage);
		}
	}
	for (UWorld* World : LoadedWorlds)
	{
		if (CanTearDownWorld(World, NewlyLoadedSet))
		{
			PrepareWorldForIndexingUnload(World);
		}
		else if (World)
		{
			ProtectedWorldRefs.Add(World);
		}
	}

	const bool bReleased = FMonolithMemoryHelper::ReleasePackagesLoadedForIndexing(NewlyLoadedPackages);
	if (GetDefault<UMonolithSettings>()->bLogMemoryStats)
	{
		int32 RetainedReleaseCandidates = 0;
		for (const TWeakObjectPtr<UPackage>& PackageRef : ReleaseCandidateRefs)
		{
			RetainedReleaseCandidates += PackageRef.IsValid() ? 1 : 0;
		}
		int32 ProtectedWorldsAlive = 0;
		for (const TWeakObjectPtr<UWorld>& WorldRef : ProtectedWorldRefs)
		{
			ProtectedWorldsAlive += WorldRef.IsValid() ? 1 : 0;
		}
		UE_LOG(LogMonolithIndex, Log,
			TEXT("LevelIndexer: release after %s retained %d/%d candidate package(s); protected worlds alive %d/%d"),
			*WorldData.PackageName.ToString(), RetainedReleaseCandidates, ReleaseCandidateRefs.Num(),
			ProtectedWorldsAlive, ProtectedWorldRefs.Num());
	}
	if (bDatabaseFailure)
	{
		PersistState(DB, TEXT("failed"), TerminalReason);
		UE_LOG(LogMonolithIndex, Error, TEXT("LevelIndexer: %s; previous rows were preserved"), *TerminalReason);
		return EMonolithLevelIndexStepResult::Failed;
	}
	if (!bReleased)
	{
		TerminalReason = FString::Printf(TEXT("Unreal deferred resource cleanup after %s"), *WorldData.PackageName.ToString());
		if (!PersistState(DB, TEXT("degraded"), TerminalReason))
		{
			return EMonolithLevelIndexStepResult::Failed;
		}
		UE_LOG(LogMonolithIndex, Error, TEXT("LevelIndexer: %s"), *TerminalReason);
		return EMonolithLevelIndexStepResult::Degraded;
	}

	if (!PersistState(DB, TEXT("in_progress")))
	{
		return EMonolithLevelIndexStepResult::Failed;
	}

	Snapshot = FMonolithMemoryHelper::CaptureMemorySnapshot(true);
	Pressure = FMonolithMemoryHelper::ClassifyMemoryPressure(Snapshot, MemoryBudgetMB);
	if (Pressure != EMonolithMemoryPressure::None)
	{
		bPressureCleanupAttempted = true;
		UE_LOG(LogMonolithIndex, Log, TEXT("LevelIndexer: %s memory pressure after '%s'; reassessing before the next load"),
			PressureName(Pressure), *WorldData.PackageName.ToString());
		return EMonolithLevelIndexStepResult::Continue;
	}

	if (LevelsProcessed % 10 == 0 || ProcessedPackages.Num() == CandidateAssetIds.Num())
	{
		UE_LOG(LogMonolithIndex, Log, TEXT("LevelIndexer: processed %d / %d levels"),
			ProcessedPackages.Num(), CandidateAssetIds.Num());
		if (GetDefault<UMonolithSettings>()->bLogMemoryStats)
		{
			FMonolithMemoryHelper::LogMemoryStats(FString::Printf(TEXT("LevelIndexer level %d"), ProcessedPackages.Num()));
		}
	}

	return NextWorldIndex >= WorldAssets.Num()
		? EMonolithLevelIndexStepResult::Complete
		: EMonolithLevelIndexStepResult::Continue;
}

bool FLevelIndexer::FinishIndex(FMonolithIndexDatabase& DB)
{
	if (!bSessionInitialized || !IsInGameThread())
	{
		return false;
	}

	bool bStateWritten = true;
	if (LevelsFailed > 0)
	{
		bStateWritten = PersistState(DB, TEXT("partial"),
			FString::Printf(TEXT("%d level package(s) could not be inspected"), LevelsFailed));
	}
	else
	{
		bStateWritten = DB.DeleteMeta(LevelIndexStateMetaKey);
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("LevelIndexer: indexed %d levels, %d actors total, %d load failures"),
		LevelsProcessed, ActorsInserted, LevelsFailed);
	if (GetDefault<UMonolithSettings>()->bLogMemoryStats)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("LevelIndexer complete"));
	}

	ResetSession();
	return bStateWritten;
}

bool FLevelIndexer::PersistState(FMonolithIndexDatabase& DB, const FString& Prefix, const FString& Detail) const
{
	FString State = FString::Printf(TEXT("%s: processed %d/%d levels"),
		*Prefix, ProcessedPackages.Num(), CandidateAssetIds.Num());
	if (!Detail.IsEmpty())
	{
		State += TEXT("; ");
		State += Detail;
	}
	return DB.WriteMeta(LevelIndexStateMetaKey, State);
}

void FLevelIndexer::ResetSession()
{
	WorldAssets.Reset();
	CandidateAssetIds.Reset();
	ProcessedPackages.Reset();
	NextWorldIndex = 0;
	ActorsInserted = 0;
	LevelsProcessed = 0;
	LevelsFailed = 0;
	bSessionInitialized = false;
	bPressureCleanupAttempted = false;
	TerminalReason.Reset();
}

void FLevelIndexer::BuildActorRows(ULevel* Level, int64 AssetId, TArray<FIndexedActor>& OutActors)
{
	if (!Level)
	{
		return;
	}

	for (AActor* Actor : Level->Actors)
	{
		if (!Actor) continue;

		// Skip the world settings and default brush - they're internal
		if (Actor->IsA(AWorldSettings::StaticClass())) continue;

		FIndexedActor IndexedActor;
		IndexedActor.AssetId = AssetId;
		IndexedActor.ActorName = Actor->GetName();
		IndexedActor.ActorClass = Actor->GetClass()->GetName();
		IndexedActor.ActorLabel = Actor->GetActorLabel();
		IndexedActor.Transform = SerializeTransform(Actor->GetActorTransform());
		IndexedActor.Components = SerializeComponents(Actor);

		OutActors.Add(MoveTemp(IndexedActor));
	}
}

FString FLevelIndexer::SerializeTransform(const FTransform& Transform)
{
	auto Obj = MakeShared<FJsonObject>();

	const FVector& Loc = Transform.GetLocation();
	auto LocObj = MakeShared<FJsonObject>();
	LocObj->SetNumberField(TEXT("x"), Loc.X);
	LocObj->SetNumberField(TEXT("y"), Loc.Y);
	LocObj->SetNumberField(TEXT("z"), Loc.Z);
	Obj->SetObjectField(TEXT("location"), LocObj);

	const FRotator Rot = Transform.GetRotation().Rotator();
	auto RotObj = MakeShared<FJsonObject>();
	RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
	RotObj->SetNumberField(TEXT("yaw"), Rot.Yaw);
	RotObj->SetNumberField(TEXT("roll"), Rot.Roll);
	Obj->SetObjectField(TEXT("rotation"), RotObj);

	const FVector& Scale = Transform.GetScale3D();
	auto ScaleObj = MakeShared<FJsonObject>();
	ScaleObj->SetNumberField(TEXT("x"), Scale.X);
	ScaleObj->SetNumberField(TEXT("y"), Scale.Y);
	ScaleObj->SetNumberField(TEXT("z"), Scale.Z);
	Obj->SetObjectField(TEXT("scale"), ScaleObj);

	FString Result;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
	FJsonSerializer::Serialize(Obj, *Writer, true);
	return Result;
}

FString FLevelIndexer::SerializeComponents(const AActor* Actor)
{
	TArray<TSharedPtr<FJsonValue>> CompArray;

	TInlineComponentArray<UActorComponent*> Components;
	Actor->GetComponents(Components);

	for (const UActorComponent* Comp : Components)
	{
		if (!Comp) continue;

		auto CompObj = MakeShared<FJsonObject>();
		CompObj->SetStringField(TEXT("name"), Comp->GetName());
		CompObj->SetStringField(TEXT("class"), Comp->GetClass()->GetName());

		CompArray.Add(MakeShared<FJsonValueObject>(CompObj));
	}

	FString Result;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
	FJsonSerializer::Serialize(CompArray, *Writer);
	return Result;
}
