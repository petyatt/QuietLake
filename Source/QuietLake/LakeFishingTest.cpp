#if WITH_DEV_AUTOMATION_TESTS
#include "LakeGame.h"
#include "LakeFish.h"
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

namespace
{
    ALakeFish* SpawnFish(UWorld* World, EFishSpecies Species, float Weight, FVector Location, int32 School = -1)
    {
        ALakeFish* Fish = World->SpawnActor<ALakeFish>(Location, FRotator::ZeroRotator);
        Fish->Setup(Species, Weight, Location, School);
        return Fish;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeFishingTest, "QuietLake.Fishing.ServerRules", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeFishingTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    // Stand at the water's edge facing the lake.
    ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(FVector(-60, 0, 100), FRotator::ZeroRotator);
    APlayerController* Controller = World->SpawnActor<APlayerController>();
    Controller->Possess(Fisher);
    Controller->SetControlRotation(FRotator(0, 180, 0));
    Fisher->ServerFish_Implementation();
    TestEqual(TEXT("Cast toward land rejected"), Fisher->FishingState, 0);
    Fisher->NextActionTime = 0;
    Controller->SetControlRotation(FRotator::ZeroRotator);
    Fisher->ServerFish_Implementation();
    TestEqual(TEXT("Cast toward water accepted"), Fisher->FishingState, 1);
    TestTrue(TEXT("Bait hangs in the water"), ALakeEnvironment::IsWater(Fisher->BaitLocation()) && Fisher->BaitLocation().Z < LakeFish::SurfaceZ);
    Fisher->ServerFish_Implementation();
    TestEqual(TEXT("Rapid repeated input ignored"), Fisher->FishingState, 1);
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    TestEqual(TEXT("Early strike gives no catch"), Fisher->CatchCount, 0);
    TestEqual(TEXT("Early strike ends cast"), Fisher->FishingState, 0);

    // A real perch finds the worm on its own, tests it and takes it.
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    const FVector Bait = Fisher->BaitLocation();
    ALakeFish* Perch = SpawnFish(World, EFishSpecies::Perch, 0.35f, Bait + FVector(320, 120, 0));
    for (int32 Step = 0; Step < 6000 && Fisher->FishingState == 1; ++Step) Perch->Simulate(0.05f);
    TestEqual(TEXT("Perch takes the worm by itself"), Fisher->FishingState, 2);
    TestEqual(TEXT("Taking the bait is not a catch"), Fisher->CatchCount, 0);
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    TestEqual(TEXT("Strike hooks the fish"), Fisher->FishingState, 3);
    TestTrue(TEXT("Fish knows it is hooked"), Perch->IsHooked());
    for (int32 Step = 0; Step < 3000 && Fisher->FishingState == 3; ++Step)
    {
        if (Fisher->Tension > 0.55f) Fisher->ServerRelease_Implementation();
        else if (Fisher->Tension < 0.2f) Fisher->ServerFish_Implementation();
        Fisher->UpdateFight(0.05f);
        if (IsValid(Perch)) Perch->Simulate(0.05f);
    }
    TestEqual(TEXT("Controlled reeling lands the perch"), Fisher->CatchCount, 1);
    TestEqual(TEXT("Successful catch ends cast"), Fisher->FishingState, 0);

    // A take that is not struck in time: the fish spits the bait.
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    ALakeFish* Roach = SpawnFish(World, EFishSpecies::Roach, 0.2f, Fisher->BaitLocation() + FVector(0, 300, 0));
    Fisher->OnFishTake(Roach);
    TestEqual(TEXT("Take sinks the bobber"), Fisher->FishingState, 2);
    Fisher->Escape();
    TestEqual(TEXT("Missed take awards no fish"), Fisher->CatchCount, 1);
    TestEqual(TEXT("Missed take ends cast"), Fisher->FishingState, 0);
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    Fisher->ServerCancel_Implementation();
    TestEqual(TEXT("Cancel ends cast"), Fisher->FishingState, 0);

    // A big pike against a reel that is never released breaks the line.
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    ALakeFish* Pike = SpawnFish(World, EFishSpecies::Pike, 3.2f, Fisher->BaitLocation());
    Fisher->OnFishTake(Pike);
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    for (int32 Step = 0; Step < 600 && Fisher->FishingState == 3; ++Step) { Fisher->UpdateFight(0.05f); Pike->Simulate(0.05f); }
    TestEqual(TEXT("Continuous reeling against a pike breaks the line"), Fisher->FishingState, 0);
    TestEqual(TEXT("Broken line awards no fish"), Fisher->CatchCount, 1);
    TestFalse(TEXT("Pike is free again"), Pike->IsHooked());

    // Never reeling: the fight times out.
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    ALakeFish* Crucian = SpawnFish(World, EFishSpecies::Crucian, 0.5f, Fisher->BaitLocation());
    Fisher->OnFishTake(Crucian);
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    Fisher->ServerRelease_Implementation();
    for (int32 Step = 0; Step < 3000 && Fisher->FishingState == 3; ++Step) { Fisher->UpdateFight(0.05f); Crucian->Simulate(0.05f); }
    TestEqual(TEXT("Abandoned fight times out"), Fisher->FishingState, 0);
    TestEqual(TEXT("Timeout awards no fish"), Fisher->CatchCount, 1);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeFishBehaviourTest, "QuietLake.Fishing.FishBehaviour", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeFishBehaviourTest::RunTest(const FString& Parameters)
{
    using namespace LakeFish;
    TestTrue(TEXT("Perch prefers worm to bread"), BaitInterest(EFishSpecies::Perch, EBait::Worm, 0.5f) > 2 * BaitInterest(EFishSpecies::Perch, EBait::Bread, 0.5f));
    TestTrue(TEXT("Roach prefers maggot to worm"), BaitInterest(EFishSpecies::Roach, EBait::Maggot, 0.3f) > BaitInterest(EFishSpecies::Roach, EBait::Worm, 0.3f));
    TestTrue(TEXT("Crucian prefers bread to worm"), BaitInterest(EFishSpecies::Crucian, EBait::Bread, 0.9f) > BaitInterest(EFishSpecies::Crucian, EBait::Worm, 0.9f));
    TestTrue(TEXT("Crucian finds bait near the bottom more often"), BaitInterest(EFishSpecies::Crucian, EBait::Bread, 0.9f) > BaitInterest(EFishSpecies::Crucian, EBait::Bread, 0.05f));
    TestTrue(TEXT("Pike ignores bread"), BaitInterest(EFishSpecies::Pike, EBait::Bread, 0.5f) == 0);
    TestTrue(TEXT("Heavier fish are longer"), LengthCm(EFishSpecies::Pike, 3) > LengthCm(EFishSpecies::Pike, 1));
    TestTrue(TEXT("A 200 g perch is about 25 cm"), FMath::IsNearlyEqual(LengthCm(EFishSpecies::Perch, 0.2f), 25.2f, 1.5f));

    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    TArray<ALakeFish*> Fish;
    const FVector Centre(2200, 0, SurfaceZ - 100);
    for (int32 I = 0; I < 5; ++I) Fish.Add(SpawnFish(World, EFishSpecies::Perch, 0.2f, Centre + FVector(I * 40, 0, 0), 0));
    Fish.Add(SpawnFish(World, EFishSpecies::Roach, 0.1f, FVector(2200, 2200, SurfaceZ - 40)));
    Fish.Add(SpawnFish(World, EFishSpecies::Crucian, 0.4f, FVector(3600, 0, SurfaceZ - 60)));
    Fish.Add(SpawnFish(World, EFishSpecies::Pike, 2.0f, FVector(2200, -2300, SurfaceZ - 50)));
    bool bInWater = true;
    for (int32 Step = 0; Step < 3000; ++Step)
        for (ALakeFish* F : Fish)
        {
            F->Simulate(0.05f);
            const FVector P = F->GetActorLocation();
            bInWater &= ALakeEnvironment::IsWater(P) && P.Z <= SurfaceZ && P.Z >= ALakeFish::BottomZ(P.X, P.Y);
        }
    TestTrue(TEXT("Fish stay in the water for 150 s"), bInWater);
    FVector SchoolCentre = FVector::ZeroVector;
    for (int32 I = 0; I < 5; ++I) SchoolCentre += Fish[I]->GetActorLocation() / 5;
    float Spread = 0;
    for (int32 I = 0; I < 5; ++I) Spread = FMath::Max(Spread, FVector::Dist(Fish[I]->GetActorLocation(), SchoolCentre));
    TestTrue(FString::Printf(TEXT("Perch school stays together (spread %.0f cm)"), Spread), Spread < 700);
    TestTrue(TEXT("Fish actually swim"), FVector::Dist(Fish[0]->GetActorLocation(), Centre) > 100);

    ALakeFish* Scared = Fish[5];
    const FVector Before = Scared->GetActorLocation();
    Scared->Spook(Before - Scared->GetActorForwardVector() * 50, 3);
    for (int32 Step = 0; Step < 40; ++Step) Scared->Simulate(0.05f);
    TestTrue(TEXT("A spooked fish darts away"), FVector::Dist(Before, Scared->GetActorLocation()) > 150);
    World->DestroyWorld(false);
    return true;
}
#endif
