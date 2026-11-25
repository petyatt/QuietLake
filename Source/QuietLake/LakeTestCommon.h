// Shared fixtures for the QuietLake automation tests.
//
// Every test creates its own world and must not depend on what earlier tests left behind. The one
// place where it did was the fish seed: ALakeFish::Setup draws its starting hunger, energy, phase
// and wander from the seed, and Seed < 0 made the seed come from GetUniqueID() — the object id,
// which shifts with the allocator and with everything the previous tests created. SpawnFish below
// derives the seed from the fish's own parameters instead, so two runs of the same test give the
// same fish.
#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "LakeGame.h"
#include "HAL/IConsoleManager.h"
#include "LakeRod.h"
#include "LakeFish.h"
#include "LakeMap.h"
#include "LakeLife.h"
#include "LakeFood.h"
#include "LakeWater.h"
#include "LakeWeather.h"
#include "LakeRipples.h"
#include "LakeWaves.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "TimerManager.h"
#include "GameFramework/PlayerController.h"

struct FLakeFishingFixture
{
    static void FreshCast(ALakeCharacter* Fisher) { Fisher->DoCast(1); }
    static void LandAt(ALakeCharacter* Fisher, FVector Target, EBait Bait)
    {
        Fisher->ServerSelectBait_Implementation(Bait);
        Fisher->DoCast(1);
        Fisher->OnRigLanded(Target);
    }
    static void FinishSettling(ALakeCharacter* Fisher) { Fisher->BaitActiveTime = Fisher->ServerNow(); }
    // Fish behaviour fixtures start with an already immersed rig; Casting tests the real stroke.
    static void ImmersedRig(ALakeCharacter* Fisher, EBait Bait = EBait::Count)
    {
        Fisher->FinishFishing(TEXT("fixture"));
        if (Bait != EBait::Count) Fisher->ServerSelectBait_Implementation(Bait);
        Fisher->HookDepth = Fisher->FloatDepth;
        Fisher->FishingState = EFishingState::Waiting;
        Fisher->bRigInHand = false;
        Fisher->bRigFlying = false;
        Fisher->CastServerTime = -10;
        Fisher->FloatPosition = Fisher->GetActorLocation() + Fisher->GetActorForwardVector() * 650;
        Fisher->FloatPosition.Z = LakeFish::SurfaceZ;
        Fisher->PlaceBait();
        Fisher->BaitActiveTime = -10;
        Fisher->bBaitPresent = true;
        Fisher->NextActionTime = 0;
    }
};

/** Deterministic seed from the fish's own parameters: the initial draw must not depend on the
 *  allocator or on the tests that ran before. Same species, weight, place and school — same fish. */
inline int32 LakeTestSeed(EFishSpecies Species, float Weight, FVector Location, int32 School)
{
    uint32 Hash = GetTypeHash(static_cast<int32>(Species));
    Hash = HashCombine(Hash, GetTypeHash(Weight));
    Hash = HashCombine(Hash, GetTypeHash(Location.X + Location.Y * 7.0f + Location.Z * 13.0f));
    Hash = HashCombine(Hash, GetTypeHash(School));
    return static_cast<int32>(Hash & 0x7fffffffu);
}

inline ALakeFish* SpawnFish(UWorld* World, EFishSpecies Species, float Weight, FVector Location, int32 School = -1, int32 Seed = -1)
{
    if (Seed < 0) Seed = LakeTestSeed(Species, Weight, Location, School);
    ALakeFish* Fish = World->SpawnActor<ALakeFish>(Location, FRotator::ZeroRotator);
    Fish->Setup(Species, Weight, Location, School, Seed);
    return Fish;
}

#endif
