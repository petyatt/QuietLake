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
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "TimerManager.h"
#include "GameFramework/PlayerController.h"

struct FLakeFishingFixture
{
    // Fish behaviour fixtures start with an already immersed rig; Casting tests the real stroke.
    static void ImmersedRig(ALakeCharacter* Fisher)
    {
        Fisher->FinishFishing(TEXT("fixture"));
        Fisher->HookDepth = Fisher->FloatDepth;
        Fisher->FishingState = EFishingState::Waiting;
        Fisher->bRigInHand = false;
        Fisher->bRigFlying = false;
        Fisher->CastServerTime = -10;
        Fisher->FloatPosition = Fisher->GetActorLocation() + Fisher->GetActorForwardVector() * 650;
        Fisher->FloatPosition.Z = LakeFish::SurfaceZ;
        Fisher->PlaceBait();
        Fisher->BaitActiveTime = -10;
        Fisher->NextActionTime = 0;
    }
};
namespace
{
    // Seed >= 0 fixes the fish's chance, so the result does not depend on how many objects the
    // engine happened to create before (assets loaded by other parts of the game shift the ids).
    ALakeFish* SpawnFish(UWorld* World, EFishSpecies Species, float Weight, FVector Location, int32 School = -1, int32 Seed = -1)
    {
        ALakeFish* Fish = World->SpawnActor<ALakeFish>(Location, FRotator::ZeroRotator);
        Fish->Setup(Species, Weight, Location, School, Seed);
        return Fish;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeFishingTest, "QuietLake.Fishing.ServerRules", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeFishingTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    // Stand at the water's edge facing the lake.
    // Stand at the water's edge of the start opening, facing the lake; Back(cm) steps away from it.
    FVector2D Facing;
    const FVector2D Edge = LakeMap::StartSpot(&Facing) + Facing * 90;
    const float FacingYaw = FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X));
    auto Back = [&](float Distance) { return FVector(Edge - Facing * Distance, 100); };
    ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(Back(0), FRotator(0, FacingYaw, 0));
    APlayerController* Controller = World->SpawnActor<APlayerController>();
    Controller->Possess(Fisher);
    auto Cast = [Fisher]() { FLakeFishingFixture::ImmersedRig(Fisher); };
    // Lifting the rig out takes a few seconds; skip to its end.
    auto Settle = [Fisher]()
    {
        // Lifting a fish out ends with it in the free hand; it is held there a moment, then put away.
        for (int32 I = 0; I < 4 && Fisher->FishingState == EFishingState::Landing; ++I)
        {
            Fisher->LandProgress = 1;
            Fisher->bReeling = true;
            if (Fisher->bFishInHand) Fisher->HandFishStart = -1000.0f;   // skip the hold
            Fisher->UpdateLanding();
        }
        if (!Fisher->bRetrieving) return;
        Fisher->RetrieveLift = 1;
        Fisher->UpdateRetrieve(0);
    };
    // LMB after the cast, held to the end: the rig is drawn out of the water.
    auto DrawOut = [Fisher, &Settle]()
    {
        Fisher->NextActionTime = 0;
        Fisher->ServerFish_Implementation();
        Settle();
    };
    // The strike needs contact and load (the hook physics): the line is taken up first, as the angler
    // does by stepping back, and the hook resolves on the spot by contact and the strike's load.
    auto StrikeHome = [&](ALakeFish* Fish)
    {
        Fisher->NextActionTime = 0;
        Fisher->ServerCancel_Implementation();
        for (int32 Step = 0; Step < 8 && Fisher->FishingState == EFishingState::Bite && IsValid(Fish); ++Step)
        {
            Fish->SetActorLocation(Fisher->RodTip() + FVector(Facing, 0) * (ALakeRod::LineLength + 30));
            Fish->State = ALakeFish::EState::Holding;
            Fish->TargetAngler = Fisher;
            Fisher->BaitPoint = Fish->MouthLocation();
            Fisher->ResolveHook(0.05f);
        }
    };
    Controller->SetControlRotation(FRotator(0, FacingYaw, 0));
    Cast();
    Fisher->NextActionTime = 1;
    TestTrue(TEXT("Bait hangs in the water"), ALakeEnvironment::IsWater(Fisher->BaitLocation()) && Fisher->BaitLocation().Z < LakeFish::SurfaceZ);
    Fisher->ServerFish_Implementation();
    TestTrue(TEXT("Rapid repeated input ignored"), Fisher->FishingState == EFishingState::Waiting);
    Fisher->NextActionTime = 0;
    Fisher->ServerCancel_Implementation();
    TestEqual(TEXT("A strike with no bite gives no catch"), Fisher->CatchCount, 0);
    TestTrue(TEXT("A strike with no bite leaves the rig fishing"), Fisher->FishingState == EFishingState::Waiting && !Fisher->bRetrieving && Fisher->HasBaitInWater());
    Fisher->ServerFish_Implementation();
    TestTrue(TEXT("LMB after the cast draws the rig out"), Fisher->bRetrieving && !Fisher->HasBaitInWater());
    Settle();
    TestTrue(TEXT("Drawn out, the cast ends"), Fisher->FishingState == EFishingState::Idle);
    Cast();
    DrawOut();

    // A real perch finds the worm on its own, tests it and takes it.
    Cast();
    const FVector Bait = Fisher->BaitLocation();
    ALakeFish* Perch = SpawnFish(World, EFishSpecies::Perch, 0.35f, Bait + FVector(320, 120, 0));
    for (int32 Step = 0; Step < 6000 && Fisher->FishingState == EFishingState::Waiting; ++Step) Perch->Simulate(0.05f);
    TestTrue(TEXT("Perch takes the worm by itself"), Fisher->FishingState == EFishingState::Bite);
    TestEqual(TEXT("Taking the bait is not a catch"), Fisher->CatchCount, 0);
    // Strike once it stands swallowing the worm: then the hook always goes home (pulling the float
    // under, a perch is hooked nine times in ten).
    for (int32 Step = 0; Step < 200 && Fisher->FishingState == EFishingState::Bite && Perch->StrikeHookChance() < 1; ++Step) Perch->Simulate(0.05f);
    // A taking perch on a slack line cannot be struck home: take the line up first, as the angler
    // does by stepping back or lifting the rod, so the strike has load to transmit.
    Perch->SetActorLocation(Fisher->RodTip() + FVector(Facing, 0) * (ALakeRod::LineLength + 30));
    Fisher->BaitPoint = Perch->MouthLocation();
    Fisher->HookImpulse = 0;
    Fisher->NextActionTime = 0;
    Fisher->ServerCancel_Implementation();
    // Resolve on the spot, keeping the perch where the line is taut: the hook needs contact and the
    // strike's transmitted load, not just the button.
    for (int32 Step = 0; Step < 8 && Fisher->FishingState == EFishingState::Bite; ++Step)
    {
        if (IsValid(Perch))
        {
            Perch->SetActorLocation(Fisher->RodTip() + FVector(Facing, 0) * (ALakeRod::LineLength + 30));
            Fisher->BaitPoint = Perch->MouthLocation();
            Fisher->ResolveHook(0.05f);
        }
    }
    TestTrue(TEXT("Strike hooks the fish"), Fisher->FishingState == EFishingState::Fight);
    TestTrue(TEXT("Fish knows it is hooked"), Perch->IsHooked());
    for (int32 Step = 0; Step < 3000 && Fisher->FishingState == EFishingState::Fight; ++Step)
    {
        if (Fisher->Tension > 0.55f) Fisher->ServerRelease_Implementation();
        else if (Fisher->Tension < 0.2f) Fisher->ServerFish_Implementation();
        Fisher->UpdateFight(0.05f);
        if (Step % 40 == 0 && IsValid(Perch))
            AddInfo(FString::Printf(TEXT("t=%.0f dist=%.0f fishZ=%.0f tension=%.2f pull=%d stamina=%.2f"), Step * 0.05f, Fisher->FishDistance, Perch->GetActorLocation().Z, Fisher->Tension, Fisher->bReeling, Perch->StaminaFraction()));
        if (IsValid(Perch)) Perch->Simulate(0.05f);
    }
    AddInfo(TEXT("Fight ended: ") + Fisher->Status);
    TestTrue(TEXT("A tired perch is lifted out of the water"), Fisher->FishingState == EFishingState::Landing && IsValid(Perch) && Perch->IsLifted());
    TestEqual(TEXT("A fish counts only once in the hand"), Fisher->CatchCount, 0);
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    Fisher->ServerCancel_Implementation();
    TestTrue(TEXT("Input does not interrupt the lift"), Fisher->FishingState == EFishingState::Landing);
    Settle();
    TestEqual(TEXT("Controlled reeling lands the perch"), Fisher->CatchCount, 1);
    TestTrue(TEXT("Successful catch ends cast"), Fisher->FishingState == EFishingState::Idle);

    // The same careful play takes longer with a heavy fish than with a light one of its species.
    auto FightSeconds = [&](float Weight)
    {
        Cast();
        TestTrue(TEXT("Cast for the crucian fight"), Fisher->FishingState == EFishingState::Waiting);
        for (int32 Pose = 0; Pose < 60; ++Pose) Fisher->UpdateRodPose(0.05f);   // the rod settles to the waiting pose
        ALakeFish* Fish = SpawnFish(World, EFishSpecies::Crucian, Weight, Fisher->BaitLocation());
        Fisher->OnFishTake(Fish);
        StrikeHome(Fish);
        int32 Step = 0;
        for (; Step < 2400 && Fisher->FishingState == EFishingState::Fight; ++Step)
        {
            if (Fisher->Tension > 0.55f) Fisher->ServerRelease_Implementation();
            else if (Fisher->Tension < 0.2f) Fisher->ServerFish_Implementation();
            Fisher->UpdateFight(0.05f);
            if (IsValid(Fish)) Fish->Simulate(0.05f);
        }
        AddInfo(FString::Printf(TEXT("Crucian %.2f kg: %s"), Weight, *Fisher->Status));
        Settle();
        const bool bLanded = Fisher->Status.StartsWith(TEXT("Карась"));
        return bLanded ? Step * 0.05f : -1.0f;
    };
    const int32 CaughtBefore = Fisher->CatchCount;
    const float Light = FightSeconds(0.18f);
    const float Heavy = FightSeconds(0.85f);
    AddInfo(FString::Printf(TEXT("Crucian fight: 0.18 kg %.1f s, 0.85 kg %.1f s"), Light, Heavy));
    TestTrue(TEXT("Both crucians are landed"), Light > 0 && Heavy > 0);
    TestTrue(TEXT("A heavy fish takes clearly longer to land"), Heavy > Light * 1.5f);
    Fisher->CatchCount = CaughtBefore;

    // A take that is not struck in time: the fish spits the bait.
    Cast();
    ALakeFish* Roach = SpawnFish(World, EFishSpecies::Roach, 0.2f, Fisher->BaitLocation() + FVector(0, 300, 0));
    Fisher->OnFishTake(Roach);
    TestTrue(TEXT("Take sinks the bobber"), Fisher->FishingState == EFishingState::Bite);
    Fisher->Escape();
    TestEqual(TEXT("Missed take awards no fish"), Fisher->CatchCount, 1);
    TestTrue(TEXT("Missed take leaves the rig fishing"), Fisher->FishingState == EFishingState::Waiting && !Fisher->bRetrieving && Fisher->HasBaitInWater());
    TestTrue(TEXT("The fish lets go of the bait"), Roach->IsFree());
    DrawOut();
    TestTrue(TEXT("The rig can then be lifted out"), Fisher->FishingState == EFishingState::Idle);
    Cast();
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    TestTrue(TEXT("LMB after the cast starts drawing the rig out"), Fisher->bRetrieving);
    // The rod rises only while LMB is held; let go, it stops where it is; held again, it goes on.
    // (A rig lying far out is lifted more slowly: RetrieveSlow.)
    Fisher->UpdateRetrieve(ALakeCharacter::RetrieveTime * Fisher->RetrieveSlow * 0.4f);
    const float Held = Fisher->RetrieveLift;
    TestTrue(TEXT("Holding LMB raises the rod"), Held > 0.35f && Held < 0.45f);
    Fisher->ServerRelease_Implementation();
    Fisher->UpdateRetrieve(ALakeCharacter::RetrieveTime);
    TestTrue(TEXT("Releasing LMB lowers the rod back"), !Fisher->bRetrieving && Fisher->RetrieveLift < 0.05f);
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    Fisher->UpdateRetrieve(ALakeCharacter::RetrieveTime * Fisher->RetrieveSlow * 0.1f);
    TestTrue(TEXT("Holding LMB again starts from the lowered rod"), Fisher->bRetrieving && Fisher->RetrieveLift > 0.05f && Fisher->RetrieveLift < Held);
    Fisher->UpdateRetrieve(ALakeCharacter::RetrieveTime * Fisher->RetrieveSlow);
    TestTrue(TEXT("Raised to the top the rig is out"), !Fisher->bRetrieving && Fisher->FishingState == EFishingState::Idle);

    // An early LMB during a bite must not cut the lift short: when the fish lets go, the rod settles
    // smoothly, and a still held button carries the lift on into drawing the rig out.
    Cast();
    ALakeFish* EarlyTaker = SpawnFish(World, EFishSpecies::Perch, 0.35f, Fisher->BaitLocation());
    Fisher->OnFishTake(EarlyTaker);
    TestTrue(TEXT("Early lift: a bite starts"), Fisher->FishingState == EFishingState::Bite);
    Fisher->bReeling = true;   // LMB held during the bite raises the rod
    for (int32 Step = 0; Step < 20; ++Step) { Fisher->UpdateRodPose(0.05f); Fisher->Tick(0.05f); }
    TestTrue(TEXT("Holding LMB during a bite raises the rod"), Fisher->RodLift > 0.5f);
    Fisher->UpdateRodPose(0.01f);
    const float Raised = Fisher->RodElevation;
    const float LiftShown = Fisher->RodLiftShown;
    Fisher->Escape();   // the fish lets go before the strike
    TestTrue(TEXT("A held LMB carries the lift on into the draw-out"), Fisher->bRetrieving && Fisher->bRetrieveHeld);
    Fisher->UpdateRodPose(0.05f);
    TestTrue(TEXT("The rod does not snap down when the fish lets go"), Fisher->RodLiftShown > 0.5f * LiftShown && Fisher->RodElevation > 0.85f * Raised);
    // Back to a plain waiting cast for the rest of the test.
    Fisher->ServerRelease_Implementation();
    Fisher->UpdateRetrieve(ALakeCharacter::RetrieveTime);

    // A fixed line breaks beyond its existing stretch limit, regardless of a random fight profile.
    Cast();
    ALakeFish* Pike = SpawnFish(World, EFishSpecies::Pike, 3.2f, Fisher->BaitLocation());
    Fisher->OnFishTake(Pike);
    StrikeHome(Pike);
    Fisher->ServerFish_Implementation();
    // (The rod's bend and the give of the arms take up about 4.4 m before the line parts.)
    Pike->SetActorLocation(Fisher->RodTip() + FVector(Facing, 0) * (ALakeRod::LineLength + 520));
    Fisher->UpdateFight(0.05f);
    TestTrue(TEXT("Exceeding the fixed line's stretch limit breaks it"), Fisher->FishingState == EFishingState::Idle);
    TestEqual(TEXT("Broken line awards no fish"), Fisher->CatchCount, 1);
    TestFalse(TEXT("Pike is free again"), Pike->IsHooked());

    // Never reeling: the fight times out.
    Cast();
    ALakeFish* Crucian = SpawnFish(World, EFishSpecies::Crucian, 0.5f, Fisher->BaitLocation());
    Fisher->OnFishTake(Crucian);
    StrikeHome(Crucian);
    Fisher->ServerRelease_Implementation();
    // (On a slack line it may throw the hook at any time; if it does not, the fight ends at its limit, 240 s.)
    for (int32 Step = 0; Step < 5000 && Fisher->FishingState == EFishingState::Fight; ++Step) { Fisher->UpdateFight(0.05f); Crucian->Simulate(0.05f); }
    TestTrue(TEXT("Abandoned fight times out"), Fisher->FishingState == EFishingState::Idle);
    TestEqual(TEXT("Timeout awards no fish"), Fisher->CatchCount, 1);

    // Fixed line: walking away from the water drags the float after the angler.
    Fisher->SetActorLocation(Back(0));
    Cast();
    for (int32 Step = 0; Step < 40; ++Step) Fisher->UpdateRodPose(0.05f);
    Fisher->ConstrainFloat();
    // Walking only tows a taut line; start this case at its geometric reach.
    Fisher->FloatPosition += FVector(Facing, 0) * 600;
    Fisher->ConstrainFloat();
    const FVector FloatBefore = Fisher->FloatPosition;
    TestTrue(TEXT("The float lands within reach of the 6 m line"), FVector::Dist2D(FloatBefore, Fisher->GetActorLocation()) < 1200);
    Fisher->ConsumeMovementInputVector();
    Fisher->Forward(1);
    TestTrue(TEXT("Walking with the line out is allowed"), !Fisher->ConsumeMovementInputVector().IsNearlyZero());
    Fisher->SetActorLocation(Back(200));
    for (int32 Step = 0; Step < 40; ++Step) Fisher->UpdateRodPose(0.05f);
    Fisher->ConstrainFloat();
    TestTrue(TEXT("Stepping back drags the float towards the bank"), Fisher->FishingState == EFishingState::Idle || FVector2D::DotProduct(FVector2D(Fisher->FloatPosition - FloatBefore), Facing) < -100);
    Fisher->SetActorLocation(Back(1140));
    for (int32 Step = 0; Step < 40; ++Step) Fisher->UpdateRodPose(0.05f);
    if (Fisher->FishingState == EFishingState::Waiting) Fisher->ConstrainFloat();
    Settle();
    TestTrue(TEXT("Walking far inland pulls the rig out of the water"), Fisher->FishingState == EFishingState::Idle);
    Fisher->SetActorLocation(Back(0));
    for (int32 Step = 0; Step < 40; ++Step) Fisher->UpdateRodPose(0.05f);

    // Depth: set in 1 cm steps before the cast, at most 20% of the rod; deeper than the water, the bait lies on the bottom.
    auto ExpectedBaitZ = [Fisher]()
    {
        const FVector F = Fisher->FloatPosition;
        return FMath::Max(LakeFish::SurfaceZ - Fisher->FloatDepth, ALakeEnvironment::GroundHeight(F.X, F.Y) + 3);
    };
    Fisher->FloatDepth = 100;
    for (int32 I = 0; I < 30; ++I) Fisher->ServerChangeDepth_Implementation(-1);
    TestEqual(TEXT("Depth changes in 1 cm steps"), Fisher->FloatDepth, 70.0f);
    TestEqual(TEXT("The hanging rig follows the depth at once"), Fisher->HookDepth, 70.0f);
    for (int32 I = 0; I < 1000; ++I) Fisher->ServerChangeDepth_Implementation(1);
    TestTrue(TEXT("Depth is at most 15-20% short of the rod"), Fisher->FloatDepth == ALakeCharacter::MaxDepth && Fisher->FloatDepth <= 0.85f * ALakeRod::RodLength && Fisher->FloatDepth >= 0.8f * ALakeRod::RodLength);
    // A deep float leaves a short main line: the rod is lowered so the float still sits on the water.
    Cast();
    TestTrue(TEXT("Deep cast accepted"), Fisher->FishingState == EFishingState::Waiting);
    for (int32 Step = 0; Step < 60; ++Step) Fisher->UpdateRodPose(0.05f);
    Fisher->ConstrainFloat();
    TestTrue(FString::Printf(TEXT("Float reaches the water with a 1 m main line (tip %.0f cm above it)"), Fisher->RodTip().Z - LakeFish::SurfaceZ),
        Fisher->FishingState == EFishingState::Waiting && Fisher->RodTip().Z - LakeFish::SurfaceZ < ALakeRod::LineLength - Fisher->HookDepth);
    DrawOut();
    for (int32 I = 0; I < 1000; ++I) Fisher->ServerChangeDepth_Implementation(-1);
    TestEqual(TEXT("Depth has a minimum"), Fisher->FloatDepth, ALakeCharacter::MinDepth);
    Fisher->FloatDepth = 50;
    Cast();
    TestTrue(TEXT("Bait hangs at the set depth"), FMath::IsNearlyEqual(Fisher->BaitLocation().Z, ExpectedBaitZ(), 1.0f) && Fisher->HookDepth == 50.0f);
    Fisher->ServerChangeDepth_Implementation(1);
    TestEqual(TEXT("Depth cannot change after the cast"), Fisher->FloatDepth, 50.0f);
    DrawOut();
    Fisher->FloatDepth = 100;

    // Rod on the stand: allowed while waiting, its tackle goes on fishing; a take must be struck from next to the stand.
    Cast();
    Fisher->ServerToggleRod_Implementation();
    const int32 OnStand = (int32)Fisher->RodKind;
    TestTrue(TEXT("Rod goes onto the stand while waiting"), Fisher->Rods[OnStand].Place == ERodPlace::Stand && !Fisher->HasRodInHands());
    TestTrue(TEXT("The tackle on the stand goes on fishing"), Fisher->HasBaitInWater(OnStand));
    Fisher->ConsumeMovementInputVector();
    Fisher->Forward(1);
    TestTrue(TEXT("Angler may walk away from a rod on the stand"), !Fisher->ConsumeMovementInputVector().IsNearlyZero());
    ALakeFish* Taker = SpawnFish(World, EFishSpecies::Perch, 0.2f, Fisher->BaitLocation(OnStand));
    Taker->TargetRig = OnStand;
    Fisher->OnFishTake(Taker);
    TestTrue(TEXT("A take on the stand"), Fisher->Rods[OnStand].State == EFishingState::Bite);
    const FVector StandBase = Fisher->Rods[OnStand].Base;
    Fisher->SetActorLocation(StandBase + FVector(-600, 0, 100));
    Fisher->NextActionTime = 0;
    Fisher->ServerCancel_Implementation();
    TestTrue(TEXT("Cannot strike from far away"), Fisher->Rods[OnStand].State == EFishingState::Bite && !Fisher->HasRodInHands());
    Fisher->SetActorLocation(StandBase + FVector(-80, 0, 100));
    StrikeHome(Taker);
    TestTrue(TEXT("Grabbing the rod from the stand strikes"), Fisher->FishingState == EFishingState::Fight);
    TestTrue(TEXT("Rod is back in the hands"), Fisher->HasRodInHands() && Fisher->Rods[OnStand].Place == ERodPlace::Hands);
    Fisher->ServerCancel_Implementation();
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
    // The pond's life as in the game: schools follow their leaders.
    ALakeLife* Life = World->SpawnActor<ALakeLife>();
    TArray<ALakeFish*> Fish;
    const FVector Centre(LakeMap::Centre, SurfaceZ - 100);
    for (int32 I = 0; I < 5; ++I) Fish.Add(SpawnFish(World, EFishSpecies::Perch, 0.2f, Centre + FVector(I * 40, 0, 0), 0));
    FRandomStream Places(11);
    Fish.Add(SpawnFish(World, EFishSpecies::Roach, 0.1f, PickSpot(EFishSpecies::Roach, Places, LakeMap::Centre, 0)));
    Fish.Add(SpawnFish(World, EFishSpecies::Crucian, 0.4f, FVector(3600, 0, SurfaceZ - 60)));
    Fish.Add(SpawnFish(World, EFishSpecies::Pike, 2.0f, PickSpot(EFishSpecies::Pike, Places, LakeMap::Centre, 0)));
    bool bInWater = true;
    float PathLength = 0;
    for (int32 Step = 0; Step < 3000; ++Step)
        for (ALakeFish* F : Fish)
        {
            if (F == Fish[0]) Life->Step(0.05f);
            const FVector Before = F->GetActorLocation();
            F->Simulate(0.05f);
            if (F == Fish[0]) PathLength += FVector::Dist(Before, F->GetActorLocation());
            const FVector P = F->GetActorLocation();
            bInWater &= ALakeEnvironment::IsWater(P) && P.Z <= SurfaceZ && P.Z >= ALakeFish::BottomZ(P.X, P.Y);
        }
    TestTrue(TEXT("Fish stay in the water for 150 s"), bInWater);
    FVector SchoolCentre = FVector::ZeroVector;
    for (int32 I = 0; I < 5; ++I) SchoolCentre += Fish[I]->GetActorLocation() / 5;
    float Spread = 0;
    for (int32 I = 0; I < 5; ++I) Spread = FMath::Max(Spread, FVector::Dist(Fish[I]->GetActorLocation(), SchoolCentre));
    TestTrue(FString::Printf(TEXT("Perch school stays together (spread %.0f cm)"), Spread), Spread < 700);
    TestTrue(FString::Printf(TEXT("Fish actually swim (%.0f m in 150 s)"), PathLength / 100), PathLength > 1000);

    ALakeFish* Scared = Fish[5];
    const FVector Before = Scared->GetActorLocation();
    Scared->Spook(Before - Scared->GetActorForwardVector() * 50, 3);
    for (int32 Step = 0; Step < 40; ++Step) Scared->Simulate(0.05f);
    AddInfo(FString::Printf(TEXT("Spooked roach: %.0f cm in 2 s from %s (shore %.0f cm, water %.0f cm), state free %d"), FVector::Dist(Before, Scared->GetActorLocation()),
        *Before.ToCompactString(), LakeMap::ShoreDistance(Before.X, Before.Y), LakeMap::Depth(Before.X, Before.Y), Scared->IsFree()));
    TestTrue(TEXT("A spooked fish darts away"), FVector::Dist(Before, Scared->GetActorLocation()) > 150);
    World->DestroyWorld(false);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeWeatherMoodTest, "QuietLake.Fishing.WeatherMood", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeWeatherMoodTest::RunTest(const FString& Parameters)
{
    using namespace LakeFish;
    auto Make = [](float Sun, bool bMorning, float Water, float Cloud, float Rain, float Wind)
    {
        FFishConditions C;
        C.SunElevation = Sun; C.bMorning = bMorning; C.WaterTempC = Water;
        C.Cloud = Cloud; C.Rain = Rain; C.Wind = Wind; C.StableHours = 8;
        return C;
    };
    const FFishConditions Noon = Make(50, false, 20, 0.1f, 0, 0.05f);
    const FFishConditions GreyDawn = Make(6, true, 18, 0.9f, 0.15f, 0.25f);
    const FFishConditions WarmNight = Make(-25, false, 21, 0.3f, 0, 0.05f);
    const float PikeNoon = Activity(EFishSpecies::Pike, Noon), PikeGrey = Activity(EFishSpecies::Pike, GreyDawn);
    AddInfo(FString::Printf(TEXT("Pike: bright calm noon %.2f, grey drizzly dawn %.2f"), PikeNoon, PikeGrey));
    TestTrue(TEXT("Pike bite best in grey, breezy, drizzly weather at dawn"), PikeGrey > PikeNoon * 1.8f);
    const float CrucianNight = Activity(EFishSpecies::Crucian, WarmNight), PerchNight = Activity(EFishSpecies::Perch, WarmNight);
    AddInfo(FString::Printf(TEXT("Warm night: crucian %.2f, perch %.2f"), CrucianNight, PerchNight));
    TestTrue(TEXT("Crucian feed on warm nights, perch hardly"), CrucianNight > PerchNight * 3);
    TestTrue(TEXT("Perch prefer a light rain to a downpour"),
        Activity(EFishSpecies::Perch, Make(30, true, 18, 0.9f, 0.15f, 0.2f)) > Activity(EFishSpecies::Perch, Make(30, true, 18, 1.0f, 0.9f, 0.7f)) * 1.5f);
    FFishConditions Before = Make(30, false, 22, 0.6f, 0, 0.2f), After = Before;
    Before.PressureTrend = -1.5f;
    After.PressureTrend = 1.5f;
    After.Shock = 1;
    After.StableHours = 0;
    TestTrue(TEXT("Crucian feed as the pressure falls and sulk right after a front"), Activity(EFishSpecies::Crucian, Before) > Activity(EFishSpecies::Crucian, After) * 2);
    TestTrue(TEXT("Roach dislike cold water"), Activity(EFishSpecies::Roach, Make(30, true, 9, 0.5f, 0, 0.1f)) < Activity(EFishSpecies::Roach, Make(30, true, 19, 0.5f, 0, 0.1f)) * 0.6f);
    TestTrue(TEXT("Hot bright days push fish deeper than grey ones"), DepthShiftM(EFishSpecies::Roach, Make(55, false, 26, 0.05f, 0, 0.05f)) > DepthShiftM(EFishSpecies::Roach, Make(30, false, 18, 0.9f, 0, 0.2f)) + 0.6f);
    TestTrue(TEXT("Bright sun on a glassy lake makes fish wary"), IsBrightCalm(Noon) && !IsBrightCalm(GreyDawn));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakePondLifeTest, "QuietLake.Fishing.PondLife", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakePondLifeTest::RunTest(const FString& Parameters)
{
    using namespace LakeFish;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ALakeLife* Life = World->SpawnActor<ALakeLife>();
    Life->SpawnFry();
    TestTrue(FString::Printf(TEXT("Fry shoals live in the shallows (%d)"), Life->Fry.Num()), Life->Fry.Num() >= 8);
    FRandomStream Random(5);
    TArray<ALakeFish*> All;
    auto Spawn = [&](EFishSpecies Species, float Weight, FVector At, int32 School = -1)
    {
        ALakeFish* Fish = SpawnFish(World, Species, Weight, At, School);
        All.Add(Fish);
        return Fish;
    };
    bool bInWater = true;
    auto Run = [&](float Seconds)
    {
        for (int32 Step = 0; Step < Seconds / 0.05f; ++Step)
        {
            Life->Step(0.05f);
            for (ALakeFish* Fish : All)
            {
                if (!IsValid(Fish)) continue;
                Fish->Simulate(0.05f);
                const FVector P = Fish->GetActorLocation();
                bInWater &= ALakeEnvironment::IsWater(P) && P.Z <= SurfaceZ && P.Z >= ALakeFish::BottomZ(P.X, P.Y);
            }
        }
    };

    // Needs drive the choice: a hungry, rested fish feeds; a tired, full one rests.
    const FVector FeedSpot = PickSpot(EFishSpecies::Crucian, Random, LakeMap::Centre, 0, nullptr, EFishGoal::Feed);
    ALakeFish* Crucian = Spawn(EFishSpecies::Crucian, 0.5f, FeedSpot);
    int32 Feeds = 0, Rests = 0;
    for (int32 I = 0; I < 20; ++I)
    {
        Crucian->Hunger = 1; Crucian->Energy = 1;
        Crucian->Decide();
        Feeds += Crucian->GetGoal() == EFishGoal::Feed;
        Crucian->Hunger = 0.05f; Crucian->Energy = 0.1f;
        Crucian->Decide();
        Rests += Crucian->GetGoal() == EFishGoal::Rest;
    }
    TestTrue(FString::Printf(TEXT("Hungry fish feed (%d/20), tired full ones rest (%d/20)"), Feeds, Rests), Feeds >= 18 && Rests >= 18);

    // A feeding crucian roots in the silt: bubbles come up, and it gets full.
    Crucian->Hunger = 1; Crucian->Energy = 1;
    Crucian->SetGoal(EFishGoal::Feed, 600);
    Run(180);   // the spot it picks may be a couple of minutes' swim away at a crucian's pace
    AddInfo(FString::Printf(TEXT("Crucian after 3 min of feeding: hunger %.2f, bubbles %d; at %s target %s, goal %d, at spot %d, water %.0f cm, state %d"),
        Crucian->GetHunger(), Life->Stats().Signs[(int32)ELakeSign::Bubbles], *Crucian->GetActorLocation().ToString(), *Crucian->RoamTarget.ToString(),
        (int32)Crucian->GetGoal(), Crucian->bAtSpot, LakeMap::Depth(Crucian->GetActorLocation().X, Crucian->GetActorLocation().Y), (int32)Crucian->State));
    TestTrue(TEXT("A rooting crucian sends up bubbles"), Life->Stats().Signs[(int32)ELakeSign::Bubbles] > 0);
    TestTrue(TEXT("Feeding fills the fish"), Crucian->GetHunger() < 0.8f);

    // Resting restores its strength.
    Crucian->Energy = 0.15f; Crucian->Hunger = 0;
    Crucian->SetGoal(EFishGoal::Rest, 600);
    Run(90);
    AddInfo(FString::Printf(TEXT("Crucian after 1.5 min of rest: energy %.2f"), Crucian->GetEnergy()));
    TestTrue(TEXT("Resting restores energy"), Crucian->GetEnergy() > 0.5f);

    // A school does what its leader does.
    const FVector RoachHome = PickSpot(EFishSpecies::Roach, Random, LakeMap::Centre, 0);
    TArray<ALakeFish*> Roach;
    for (int32 I = 0; I < 6; ++I) Roach.Add(Spawn(EFishSpecies::Roach, 0.15f, RoachHome + FVector(I * 30, 0, 0), 7));
    Life->Step(0);
    ALakeFish* Leader = Life->SchoolLeader(7);
    TestNotNull(TEXT("The school has a leader"), Leader);
    if (Leader) { Leader->Energy = 0.2f; Leader->SetGoal(EFishGoal::Rest, 600); }
    Run(12);
    int32 Following = 0;
    for (ALakeFish* Fish : Roach) Following += IsValid(Fish) && Fish->GetGoal() == EFishGoal::Rest;
    TestEqual(TEXT("The whole school follows the leader"), Following, 6);

    // Hungry perch drive the fry: rushes, spray, and they fill up.
    const FVector Fry = Life->FryLocation(0);
    const FVector2D Deeper = LakeMap::DeeperDirection(Fry.X, Fry.Y);
    TArray<ALakeFish*> Perch;
    for (int32 I = 0; I < 5; ++I)
    {
        const FVector2D P = FVector2D(Fry) + Deeper * (300 + I * 40);
        Perch.Add(Spawn(EFishSpecies::Perch, 0.25f, FVector(P, FMath::Max(ALakeFish::BottomZ(P.X, P.Y) + 20, SurfaceZ - 60)), 8));
    }
    for (ALakeFish* Fish : Perch) { Fish->Hunger = 1; Fish->Energy = 1; }
    Life->Step(0);
    if (ALakeFish* PerchLead = Life->SchoolLeader(8)) PerchLead->SetGoal(EFishGoal::Feed, 600);
    const int32 ScatterBefore = Life->Stats().Signs[(int32)ELakeSign::Scatter];
    Run(60);
    float PerchHunger = 0;
    for (ALakeFish* Fish : Perch) PerchHunger += Fish->GetHunger() / Perch.Num();
    AddInfo(FString::Printf(TEXT("Perch hunt: %d rushes at the fry, mean hunger %.2f"), Life->Stats().FryAttacks, PerchHunger));
    TestTrue(TEXT("Perch drive the fry"), Life->Stats().FryAttacks >= 3 && Life->Stats().Signs[(int32)ELakeSign::Scatter] > ScatterBefore);
    TestTrue(TEXT("Hunting fills the perch"), PerchHunger < 0.8f);

    // A pike in ambush strikes at a small fish swimming past its snout.
    const FVector Ambush = PickSpot(EFishSpecies::Pike, Random, LakeMap::Centre, 0, nullptr, EFishGoal::Feed);
    ALakeFish* Pike = Spawn(EFishSpecies::Pike, 2.5f, Ambush);
    Pike->Hunger = 1;
    Pike->SetGoal(EFishGoal::Feed, 600);
    for (int32 Wait = 0; Wait < 18 && !Pike->bAtSpot; ++Wait)
    {
        Run(5);
    }
    AddInfo(FString::Printf(TEXT("Pike at %s target %s, water %.0f cm, shore %.0f cm, goal %d, state %d"), *Pike->GetActorLocation().ToString(), *Pike->RoamTarget.ToString(),
        LakeMap::Depth(Pike->GetActorLocation().X, Pike->GetActorLocation().Y), LakeMap::ShoreDistance(Pike->GetActorLocation().X, Pike->GetActorLocation().Y), (int32)Pike->GetGoal(), (int32)Pike->State));
    TestTrue(TEXT("The pike settles in its ambush"), Pike->bAtSpot);
    const int32 StrikesBefore = Life->Stats().PikeStrikes;
    for (int32 Try = 0; Try < 6 && Life->Stats().PikeStrikes == StrikesBefore && IsValid(Pike); ++Try)
    {
        const FVector Front = Pike->MouthLocation() + Pike->GetActorForwardVector() * 90;
        Spawn(EFishSpecies::Roach, 0.12f, FVector(Front.X, Front.Y, FMath::Max(Front.Z, ALakeFish::BottomZ(Front.X, Front.Y) + 10)));
        Pike->NextLook = 0;
        Run(4);
    }
    AddInfo(FString::Printf(TEXT("Pike: %d strikes, %d fish eaten"), Life->Stats().PikeStrikes, Life->Stats().Eaten));
    TestTrue(TEXT("The pike strikes from ambush"), Life->Stats().PikeStrikes > StrikesBefore);

    TestTrue(TEXT("Every fish stays in the water"), bInWater);
    World->DestroyWorld(false);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeBitePatternsTest, "QuietLake.Fishing.BitePatterns", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeBitePatternsTest::RunTest(const FString& Parameters)
{
    using namespace LakeFish;
    FRandomStream R(3);
    struct FSummary { float Before = 0, Window = 0; int32 Tries = 0; EBiteMotion Take = EBiteMotion::None; float TakeAmount = 0; };
    auto Summarise = [](const TArray<FBiteStep>& Bite)
    {
        FSummary S;
        for (const FBiteStep& Step : Bite)
        {
            if (Step.bTake) { if (S.Take == EBiteMotion::None) { S.Take = Step.Motion; S.TakeAmount = Step.Amount; } S.Window += Step.Duration; }
            else { S.Before += Step.Duration; S.Tries += Step.Motion != EBiteMotion::None; }
        }
        return S;
    };
    constexpr int32 N = 300;
    int32 CrucianLifts = 0, CrucianOffBottomLifts = 0, RoachTested = 0, PerchFastSinks = 0, PerchSwallows = 0, PikeEarlyMisses = 0, PikeLateHooks = 0;
    float CrucianBefore = 0, PerchBefore = 0, SmallTries = 0, BigTries = 0, EagerWindow = 0, SluggishWindow = 0;
    float Window[4] = {};   // the four built-in species
    for (int32 I = 0; I < N; ++I)
    {
        const FSummary Crucian = Summarise(MakeBite(EFishSpecies::Crucian, 0.5f, 1.0f, true, false, R));
        CrucianLifts += Crucian.Take == EBiteMotion::Lift;
        CrucianBefore += Crucian.Before / N;
        CrucianOffBottomLifts += Summarise(MakeBite(EFishSpecies::Crucian, 0.5f, 1.0f, false, false, R)).Take == EBiteMotion::Lift;
        SmallTries += Summarise(MakeBite(EFishSpecies::Crucian, 0.1f, 1.0f, true, false, R)).Tries / float(N);
        BigTries += Summarise(MakeBite(EFishSpecies::Crucian, 0.9f, 1.0f, true, false, R)).Tries / float(N);
        EagerWindow += Summarise(MakeBite(EFishSpecies::Crucian, 0.5f, 1.2f, false, false, R)).Window / N;
        SluggishWindow += Summarise(MakeBite(EFishSpecies::Crucian, 0.5f, 0.2f, false, true, R)).Window / N;
        const TArray<FBiteStep> Roach = MakeBite(EFishSpecies::Roach, 0.5f, 1.0f, false, false, R);
        const int32 RoachTake = Roach.IndexOfByPredicate([](const FBiteStep& S) { return S.bTake; });
        const int32 RoachTry = Roach.IndexOfByPredicate([](const FBiteStep& S) { return !S.bTake && (S.Motion == EBiteMotion::Peck || S.Motion == EBiteMotion::Tremble); });
        RoachTested += RoachTake == INDEX_NONE || (RoachTry != INDEX_NONE && RoachTry < RoachTake);
        Window[(int32)EFishSpecies::Roach] += Summarise(Roach).Window / N;
        const TArray<FBiteStep> PerchBite = MakeBite(EFishSpecies::Perch, 0.5f, 1.0f, true, false, R);
        const FSummary Perch = Summarise(PerchBite);
        PerchFastSinks += Perch.Take == EBiteMotion::Sink && Perch.TakeAmount >= 45;
        PerchSwallows += PerchBite.Last().Motion == EBiteMotion::Hold && PerchBite.Last().bTake && PerchBite.Last().Hook >= 1;
        PerchBefore += Perch.Before / N;
        Window[(int32)EFishSpecies::Perch] += Perch.Window / N;
        Window[(int32)EFishSpecies::Crucian] += Crucian.Window / N;
        const TArray<FBiteStep> Pike = MakeBite(EFishSpecies::Pike, 0.5f, 1.0f, false, false, R);
        Window[(int32)EFishSpecies::Pike] += Summarise(Pike).Window / N;
        const int32 PikeTake = Pike.IndexOfByPredicate([](const FBiteStep& S) { return S.bTake; });
        PikeEarlyMisses += PikeTake != INDEX_NONE && Pike[PikeTake].Hook < 0.6f;
        PikeLateHooks += Pike.Last().bTake && Pike.Last().Hook >= 0.9f;
    }
    AddInfo(FString::Printf(TEXT("Crucian: lifts %d/%d from the bottom, %d off it; %.1f s of testing; tries small %.1f, big %.1f; take window eager %.1f s, sluggish %.1f s"),
        CrucianLifts, N, CrucianOffBottomLifts, CrucianBefore, SmallTries, BigTries, EagerWindow, SluggishWindow));
    AddInfo(FString::Printf(TEXT("Perch: %.1f s before the take, fast sinks %d/%d, swallowing %d/%d; roach tested first %d/%d; pike: early strike unsure %d/%d, last run sure %d/%d"),
        PerchBefore, PerchFastSinks, N, PerchSwallows, N, RoachTested, N, PikeEarlyMisses, N, PikeLateHooks, N));
    AddInfo(FString::Printf(TEXT("Time to strike (undisturbed, favourite bait): perch %.1f s, roach %.1f s, crucian %.1f s, pike %.1f s"),
        Window[0], Window[1], Window[2], Window[3]));
    TestTrue(TEXT("A crucian with the bait on the bottom usually lifts the float"), CrucianLifts > N * 0.7f);
    TestEqual(TEXT("No lift when the bait hangs off the bottom"), CrucianOffBottomLifts, 0);
    TestTrue(TEXT("Small crucian keep trembling the float longer than big ones"), SmallTries > BigTries + 1.0f);
    TestTrue(TEXT("A sluggish crucian lets go sooner"), SluggishWindow < EagerWindow * 0.8f);
    TestTrue(TEXT("Roach test the bait (pecks, trembles) before taking it"), RoachTested == N);
    TestTrue(TEXT("Perch pull the float under sharply"), PerchFastSinks == N);
    TestTrue(TEXT("Then perch stand swallowing the worm: a late strike still hooks"), PerchSwallows == N);
    TestTrue(TEXT("A pike carrying the bait across its jaws is often missed, on its second run it is hooked"), PikeEarlyMisses == N && PikeLateHooks == N);
    TestTrue(TEXT("Time enough to strike: roach 2.5 s, crucian and perch 4 s, pike 7 s"),
        Window[1] >= 2.5f && Window[2] >= 4.0f && Window[0] >= 4.0f && Window[3] >= 7.0f);
    TestTrue(TEXT("Perch take far more boldly than crucian"), PerchBefore < CrucianBefore * 0.4f);

    // A real crucian at a bait lying on the bottom: trembles first, then lifts or leads the float off.
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    FVector2D Facing;
    const FVector2D Edge = LakeMap::StartSpot(&Facing) + Facing * 90;
    ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(FVector(Edge, 100), FRotator(0, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)), 0));
    APlayerController* Controller = World->SpawnActor<APlayerController>();
    Controller->Possess(Fisher);
    Controller->SetControlRotation(Fisher->GetActorRotation());
    Fisher->FloatDepth = ALakeCharacter::MaxDepth;       // lies on the bottom
    Fisher->Bait = EBait::Bread;
    FLakeFishingFixture::ImmersedRig(Fisher);
    TestTrue(TEXT("Cast for the crucian"), Fisher->FishingState == EFishingState::Waiting);
    TSet<EBiteMotion> Seen;
    EBiteMotion FirstTake = EBiteMotion::None;
    for (int32 Try = 0; Try < 8 && FirstTake == EBiteMotion::None; ++Try)
    {
        ALakeFish* Crucian = SpawnFish(World, EFishSpecies::Crucian, 0.6f, Fisher->BaitLocation() + FVector(150, 0, 0), -1, 500 + Try);
        for (int32 Step = 0; Step < 2400 && Fisher->FishingState == EFishingState::Waiting; ++Step)
        {
            Crucian->Simulate(0.05f);
            Seen.Add(Fisher->BiteMotion);
        }
        if (Fisher->FishingState == EFishingState::Bite) FirstTake = Fisher->BiteMotion;
        else Crucian->Destroy();
    }
    AddInfo(FString::Printf(TEXT("Crucian take shown as motion %d; the float trembled first: %d"), (int32)FirstTake, Seen.Contains(EBiteMotion::Tremble)));
    TestTrue(TEXT("The crucian tests the bait first: the float trembles"), Seen.Contains(EBiteMotion::Tremble));
    TestTrue(TEXT("Its take lifts the float or leads it off"), FirstTake == EBiteMotion::Lift || FirstTake == EBiteMotion::Drift);

    // Bites as the angler sees them, with real fish and nobody striking: how long each species tests
    // the bait, how long it holds it (the time there is to strike), and whether it comes back after
    // letting go. Logged as QUIET_LAKE_BITE_SURVEY.
    struct FKind { EFishSpecies Species; EBait Bait; bool bBottom; float Weight; };
    const FKind Kinds[] = { { EFishSpecies::Roach, EBait::Maggot, false, 0.15f }, { EFishSpecies::Perch, EBait::Worm, false, 0.3f },
                            { EFishSpecies::Crucian, EBait::Bread, true, 1.2f }, { EFishSpecies::Pike, EBait::Worm, false, 2.0f } };
    struct FSurvey { int32 Trials = 0, Takes = 0, Returns = 0, LeftUntaken = 0; float Testing = 0, Window = 0, MinWindow = 1e9f, MaxWindow = 0; };
    FSurvey Survey[4];
    constexpr float Dt = 0.05f;
    for (const FKind& Kind : Kinds)
    {
        FSurvey& Sv = Survey[(int32)Kind.Species];
        for (int32 Trial = 0; Trial < 12; ++Trial)
        {
            Fisher->Bait = Kind.Bait;
            Fisher->FloatDepth = ALakeCharacter::MaxDepth;
            FLakeFishingFixture::ImmersedRig(Fisher);
            if (!Kind.bBottom)
            {
                const FVector At = Fisher->BaitLocation();
                Fisher->FloatDepth = FMath::Max(30.0f, LakeFish::SurfaceZ - ALakeFish::BottomZ(At.X, At.Y) - 25);
                FLakeFishingFixture::ImmersedRig(Fisher);
            }
            ALakeFish* Fish = SpawnFish(World, Kind.Species, Kind.Weight, Fisher->BaitLocation() + FVector(150, 0, 0), -1,
                1000 * ((int32)Kind.Species + 1) + Trial);
            Fish->Hunger = 1;
            ++Sv.Trials;
            float Touch = -1, TakeAt = -1;
            int32 TakesHere = 0;
            for (int32 Step = 0; Step < 1800; ++Step)
            {
                const float T = Step * Dt;
                const EFishingState Before = Fisher->FishingState;
                Fish->Simulate(Dt);
                ++GFrameCounter;
                World->GetTimerManager().Tick(Dt);
                if (Touch < 0 && Fish->State == ALakeFish::EState::Nibble) Touch = T;
                if (Before != EFishingState::Bite && Fisher->FishingState == EFishingState::Bite)
                {
                    TakeAt = T;
                    if (TakesHere == 0 && Touch >= 0) Sv.Testing += T - Touch;
                    ++TakesHere;
                }
                if (Before == EFishingState::Bite && Fisher->FishingState != EFishingState::Bite && TakeAt >= 0)
                {
                    const float W = T - TakeAt;
                    if (TakesHere == 1) { Sv.Window += W; Sv.MinWindow = FMath::Min(Sv.MinWindow, W); Sv.MaxWindow = FMath::Max(Sv.MaxWindow, W); }
                    TakeAt = -1;
                }
            }
            Sv.Takes += TakesHere > 0;
            Sv.Returns += TakesHere > 1;
            Sv.LeftUntaken += Touch >= 0 && TakesHere == 0;
            Fish->Destroy();
        }
        const int32 Takes = FMath::Max(1, Sv.Takes);
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_BITE_SURVEY %s on %s: took it %d of %d, tested %.1f s first, held it %.1f s (%.1f-%.1f) when not struck, came back and took again %d, left without taking %d"),
            *LakeFish::Info(Kind.Species).Name, LakeFish::BaitName(Kind.Bait), Sv.Takes, Sv.Trials, Sv.Testing / Takes, Sv.Window / Takes,
            Sv.Takes ? Sv.MinWindow : 0.0f, Sv.MaxWindow, Sv.Returns, Sv.LeftUntaken);
    }
    const FSurvey& SRoach = Survey[(int32)EFishSpecies::Roach], &SPerch = Survey[(int32)EFishSpecies::Perch];
    const FSurvey& SCrucian = Survey[(int32)EFishSpecies::Crucian], &SPike = Survey[(int32)EFishSpecies::Pike];
    TestTrue(TEXT("Real fish take their favourite baits"), SRoach.Takes >= 6 && SPerch.Takes >= 9 && SCrucian.Takes >= 6 && SPike.Takes >= 1);
    TestTrue(TEXT("Real fish hold the bait long enough to strike"),
        SRoach.Window / FMath::Max(1, SRoach.Takes) >= 2.5f && SPerch.Window / FMath::Max(1, SPerch.Takes) >= 4.0f
        && SCrucian.Window / FMath::Max(1, SCrucian.Takes) >= 4.0f && SPike.Window / FMath::Max(1, SPike.Takes) >= 7.0f);
    TestTrue(TEXT("A missed fish comes back for another try"), SRoach.Returns >= 1 && SPerch.Returns >= 1 && SCrucian.Returns >= 1);
    TestTrue(TEXT("A crucian does not test the bait for ever"), SCrucian.Testing / FMath::Max(1, SCrucian.Takes) < 14.0f);

    // The rod jerked while a roach tries the bait: the bait jumps, the roach backs off at once.
    Fisher->Bait = EBait::Maggot;
    Fisher->FloatDepth = 80;
    FLakeFishingFixture::ImmersedRig(Fisher);
    ALakeFish* Shy = SpawnFish(World, EFishSpecies::Roach, 0.15f, Fisher->BaitLocation() + FVector(100, 0, 0), -1, 900);
    Shy->Hunger = 1;
    for (int32 Step = 0; Step < 600 && Shy->State != ALakeFish::EState::Nibble; ++Step) Shy->Simulate(Dt);
    const bool bTrying = Shy->State == ALakeFish::EState::Nibble;
    Fisher->RodElevationSpeed = 120;
    Shy->Simulate(Dt);
    Fisher->RodElevationSpeed = 0;
    TestTrue(TEXT("A roach trying the bait backs off when the rod is jerked"), bTrying && Shy->State != ALakeFish::EState::Nibble && Fisher->FishingState == EFishingState::Waiting);
    World->DestroyWorld(false);
    return true;
}

// Casting from the hand: the rod tip must be loaded first; the float goes where the angler looks.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeCastingTest, "QuietLake.Fishing.Casting", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeCastingTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    FVector2D Facing;
    const FVector2D Bank = LakeMap::StartSpot(&Facing);
    const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X));
    auto Exercise = [&](float Fps, float Hold)
    {
        ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(FVector(Bank, ALakeEnvironment::GroundHeight(Bank.X, Bank.Y) + 95), FRotator(0, Yaw, 0));
        APlayerController* PC = World->SpawnActor<APlayerController>();
        PC->Possess(Fisher);
        PC->SetControlRotation(FRotator(-12, Yaw, 0));
        Fisher->RodVisual = World->SpawnActor<ALakeRod>();
        Fisher->FloatDepth = Fisher->HookDepth = 60;
        Fisher->bRigInHand = true;
        const float Dt = 1 / Fps;
        auto Step = [&]()
        {
            // Advance phase clocks because this isolated world's clock does not tick.
            if (Fisher->bCharging) Fisher->ChargeServerStart -= Dt;
            Fisher->CastServerTime -= Dt;
            Fisher->RigGripTime -= Dt;
            Fisher->Tick(Dt);
        };
        auto Run = [&](float Seconds) { for (int32 I = 0; I < FMath::RoundToInt(Seconds * Fps); ++I) Step(); };
        Run(2);
        const FVector Ready = Fisher->RodVisual->ShotLocation();
        Fisher->NextActionTime = 0;
        Fisher->ServerFish_Implementation();
        Run(Hold);
        TestFalse(TEXT("Fingers open during the held stroke"), Fisher->bRigInHand);
        TestTrue(TEXT("Rig moves before button release"), FVector::Dist(Ready, Fisher->RodVisual->ShotLocation()) > 30);
        const FVector Before = Fisher->RodVisual->ShotLocation(), Velocity = Fisher->RodVisual->ShotVelocity();
        Fisher->ServerRelease_Implementation();
        TestTrue(TEXT("Release preserves rig position and momentum"), Before.Equals(Fisher->RodVisual->ShotLocation(), 0.001) && Velocity.Equals(Fisher->RodVisual->ShotVelocity(), 0.001));
        float Highest = Fisher->RodVisual->FloatKeel().Z;
        for (int32 I = 0; I < Fps * 6; ++I) { Step(); Highest = FMath::Max(Highest, float(Fisher->RodVisual->FloatKeel().Z)); }
        const FVector Landed = Fisher->RodVisual->FloatKeel();
        AddInfo(FString::Printf(TEXT("Physical cast %.0f fps hold %.2f: out %.0f z %.0f highest %.0f state %d retrieving %d"), Fps, Hold, FVector::Dist2D(Landed, Fisher->GetActorLocation()), Landed.Z, Highest, int32(Fisher->FishingState), Fisher->bRetrieving));
        TestFalse(TEXT("Cast remains finite"), Landed.ContainsNaN());
        TestTrue(TEXT("Cast settles on water"), !Fisher->bRigFlying && !Fisher->bRetrieving && ALakeEnvironment::IsWater(Landed) && Fisher->RodVisual->FloatDown());
        TestTrue(TEXT("Fixed length line bounds the cast"), FVector::Dist(Landed, Fisher->RodVisual->Tip()) < ALakeRod::LineLength - Fisher->HookDepth + 15);
        // The held pendulum can rise above the angler, but cannot fly above the raised pole.
        TestTrue(TEXT("Cast stays below the pole's overhead reach"), Highest < Fisher->GetActorLocation().Z + ALakeRod::RodLength);
        Fisher->BaitActiveTime = -10;
        Fisher->NextActionTime = 0;
        Fisher->ServerFish_Implementation();
        Run(0.8f);
        const float Pull = Fisher->RetrieveLift;
        Fisher->ServerRelease_Implementation();
        Run(2);
        // Letting go of LMB lowers the rod back to the aim; the rig simply stays in the water.
        TestTrue(TEXT("Release lowers the rod back to the aim"), Pull > 0.1f && Fisher->RetrieveLift < 0.05f && !Fisher->bRetrieving && !Fisher->bRigInHand);
        Fisher->NextActionTime = 0;
        Fisher->ServerFish_Implementation();
        float Jump = 0, Ahead = 1000, Nearest = 10000, NearSpeed = 0;
        FVector Prev = Fisher->RodVisual->ShotLocation();
        for (int32 I = 0; I < Fps * 16; ++I)
        {
            Step();
            const FVector Shot = Fisher->RodVisual->ShotLocation();
            Jump = FMath::Max(Jump, float(FVector::Dist(Prev, Shot)));
            const float Gap = FVector::Dist(Shot, Fisher->HandPoint(0));
            if (Gap < Nearest) { Nearest = Gap; NearSpeed = Fisher->RodVisual->ShotVelocity().Size(); }
            const FVector Local = Fisher->GetActorTransform().InverseTransformPosition(Shot);
            if (Local.Size2D() < 150) Ahead = FMath::Min(Ahead, float(Local.X));
            Prev = Shot;
        }
        AddInfo(FString::Printf(TEXT("Physical retrieve %.0f fps: hand=%d progress=%.2f distance=%.0f shot=%s tip=%s jump=%.1f ahead=%.0f"), Fps, Fisher->bRigInHand, Fisher->RetrieveLift, FVector::Dist(Prev, Fisher->HandPoint(0)), *Fisher->GetActorTransform().InverseTransformPosition(Prev).ToCompactString(), *Fisher->GetActorTransform().InverseTransformPosition(Fisher->RodVisual->Tip()).ToCompactString(), Jump, Ahead));
        AddInfo(FString::Printf(TEXT("Nearest hand %.1f cm at %.0f cm/s"), Nearest, NearSpeed));
        // The lift draws the rig out of the water on the line; the angler steers it into the hand.
        TestTrue(TEXT("Held lift draws the rig out of the water"), Fisher->RodVisual->FloatKeel().Z > LakeFish::SurfaceZ - 5 && Fisher->RodVisual->HookLocation().Z > LakeFish::SurfaceZ - 20);
        TestTrue(TEXT("The swinging rig stays in front of the angler"), Ahead > -15);
        TestTrue(TEXT("Retrieval has no positional jump"), Jump < 1600 / Fps);
        Fisher->RodVisual->Destroy(); Fisher->Destroy(); PC->Destroy();
        return Landed;
    };
    const FVector Base = Exercise(60, 1.2f);
    for (float Fps : { 30.0f, 120.0f })
        TestTrue(TEXT("Frame rate preserves physical landing within 60 cm"), FVector::Dist2D(Base, Exercise(Fps, 1.2f)) < 60);
    Exercise(60, 0.6f);
    // A lifted fish keeps its weight and position when the player lets it back down.
    for (float Weight : { 0.18f, 0.6f })
    {
        ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(FVector(Bank, ALakeEnvironment::GroundHeight(Bank.X, Bank.Y) + 95), FRotator(0, Yaw, 0));
        APlayerController* PC = World->SpawnActor<APlayerController>(); PC->Possess(Fisher);
        PC->SetControlRotation(FRotator(-12, Yaw, 0));
        Fisher->RodVisual = World->SpawnActor<ALakeRod>();
        Fisher->FloatDepth = Fisher->HookDepth = 60;
        Fisher->bRigInHand = true;
        auto Run = [&](float Seconds)
        {
            for (int32 I = 0; I < Seconds * 60; ++I)
            {
                Fisher->ChargeServerStart -= 1 / 60.0f; Fisher->CastServerTime -= 1 / 60.0f; Fisher->RigGripTime -= 1 / 60.0f;
                Fisher->Tick(1 / 60.0f);
            }
        };
        Run(2); Fisher->StartCast(); Run(1.2f); Fisher->ServerRelease_Implementation(); Run(5);
        ALakeFish* Fish = SpawnFish(World, EFishSpecies::Perch, Weight, Fisher->RodVisual->HookLocation());
        Fish->Hook(Fisher); Fisher->HookedFish = Fish; Fisher->LineFish = Fish;
        Fisher->FishingState = EFishingState::Fight;
        for (int32 I = 0; I < 7200 && Fisher->FishingState == EFishingState::Fight; ++I)
        {
            if (Fisher->Tension > 0.55f) Fisher->ServerRelease_Implementation();
            else if (Fisher->Tension < 0.2f) Fisher->ServerFish_Implementation();
            Run(1 / 60.0f);
            if (IsValid(Fish)) Fish->Simulate(1 / 60.0f);
        }
        AddInfo(FString::Printf(TEXT("Physical fight %.2f kg: state %d time %.1f status %s"), Weight, int32(Fisher->FishingState), Fisher->FightTime, *Fisher->Status));
        TestTrue(TEXT("Real rod and fish simulation lead to a lift"), Fisher->FishingState == EFishingState::Landing);
        if (Fisher->FishingState != EFishingState::Landing)
        {
            Fisher->RodVisual->Destroy(); Fisher->Destroy(); PC->Destroy(); if (IsValid(Fish)) Fish->Destroy();
            continue;
        }
        Run(0.7f);
        const float Progress = Fisher->LandProgress;
        const FVector Before = Fisher->RodVisual->HookLocation();
        Fisher->ServerRelease_Implementation();
        TestTrue(TEXT("Fish release keeps hook position"), Before.Equals(Fisher->RodVisual->HookLocation(), 0.001));
        Run(0.4f);
        TestTrue(TEXT("Releasing lowers a fish instead of completing its lift"), Fisher->LandProgress < Progress && Fisher->CatchCount == 0);
        for (int32 I = 0; I < 600 && Fisher->FishingState == EFishingState::Landing; ++I) Run(1 / 60.0f);
        AddInfo(FString::Printf(TEXT("Lowered fish %.2f: state=%d lift=%.2f progress=%.2f hook=%s water=%d elevation=%.0f"), Weight, int32(Fisher->FishingState), Fisher->RodLift, Fisher->LandProgress, *Fisher->RodVisual->HookLocation().ToCompactString(), ALakeEnvironment::IsWater(Fisher->RodVisual->HookLocation()), Fisher->RodElevation));
        TestTrue(TEXT("A fully lowered fish resumes swimming on the hook"), Fisher->FishingState == EFishingState::Fight && !Fish->IsLifted());
        Fisher->ServerFish_Implementation();
        for (int32 I = 0; I < 600 && Fisher->FishingState == EFishingState::Fight; ++I)
        {
            Run(1 / 60.0f); if (IsValid(Fish)) Fish->Simulate(1 / 60.0f);
        }
        Run(20);
        AddInfo(FString::Printf(TEXT("Physical fish %.2f kg: caught %d state %d hook-to-hand %.0f bend %.0f"), Weight, Fisher->CatchCount, int32(Fisher->FishingState), FVector::Dist(Fisher->RodVisual->HookLocation(), Fisher->HandPoint(0)), Fisher->RodVisual->TipBend()));
        TestEqual(TEXT("A physically lifted fish reaches the hand"), Fisher->CatchCount, 1);
        Fisher->RodVisual->Destroy(); Fisher->Destroy(); PC->Destroy(); if (IsValid(Fish)) Fish->Destroy();
    }


    World->DestroyWorld(false);
    return true;
}

// Wind: it drifts a float whose bait hangs free downwind until the line holds it; a rig on the bottom stays put.
// The pendulum cast by holding LMB: no throw, the rig swings out on its line; the longer LMB is held,
// the farther it comes down; held to the end, the rod lays it down by itself; LMB again lifts it out
// and the free hand takes the shot within its reach, in front of the body, not off to the right.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeCastHoldTest, "QuietLake.Fishing.CastHold", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeCastHoldTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    FVector2D Facing;
    const FVector2D Bank = LakeMap::StartSpot(&Facing);
    const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X));
    struct FResult { float Out = 0; bool bLanded = false, bByItself = false, bCaught = false; float StartSpeed = 0, CatchGap = 1e6f, CatchAside = 0; };
    auto Cast = [&](float Hold, float Fps)
    {
        FResult R;
        ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(FVector(Bank, ALakeEnvironment::GroundHeight(Bank.X, Bank.Y) + 95), FRotator(0, Yaw, 0));
        APlayerController* PC = World->SpawnActor<APlayerController>();
        PC->Possess(Fisher);
        PC->SetControlRotation(FRotator(-8, Yaw, 0));
        Fisher->RodVisual = World->SpawnActor<ALakeRod>();
        Fisher->FloatDepth = Fisher->HookDepth = 110;
        Fisher->bRigInHand = true;
        const float Dt = 1 / Fps;
        auto Step = [&]()
        {
            // (This world's clock does not tick: move the phase clocks back instead.)
            if (Fisher->bCharging) Fisher->ChargeServerStart -= Dt;
            Fisher->CastServerTime -= Dt;
            Fisher->RigGripTime -= Dt;
            Fisher->Tick(Dt);
        };
        auto Run = [&](float Seconds) { for (int32 I = 0; I < FMath::RoundToInt(Seconds * Fps); ++I) Step(); };
        Run(2);
        Fisher->NextActionTime = 0;
        Fisher->ServerFish_Implementation();
        for (int32 I = 0; I < FMath::RoundToInt(Hold * Fps) && Fisher->bCharging; ++I)
        {
            Step();
            if (I == FMath::RoundToInt(0.15f * Fps)) R.StartSpeed = Fisher->RodVisual->ShotVelocity().Size();
        }
        R.bByItself = !Fisher->bCharging;
        if (Fisher->bCharging) Fisher->ServerRelease_Implementation();
        Run(6);
        const FVector Float = Fisher->RodVisual->FloatKeel();
        R.bLanded = Fisher->FishingState == EFishingState::Waiting && !Fisher->bRigFlying && ALakeEnvironment::IsWater(Float) && Fisher->RodVisual->FloatDown();
        R.Out = FVector::Dist2D(Float, Fisher->GetActorLocation());
        // LMB again: the rod comes up, the rig swings in and the free hand takes it.
        Fisher->BaitActiveTime = -10;
        Fisher->NextActionTime = 0;
        Fisher->ServerFish_Implementation();
        for (int32 I = 0; I < FMath::RoundToInt(14 * Fps) && !Fisher->bRigInHand; ++I)
        {
            Step();
            if (Fisher->bRigInHand)
            {
                const FVector Shot = Fisher->RodVisual->ShotLocation();
                R.CatchGap = FVector::Dist(Shot, Fisher->LeftShoulder());
                R.CatchAside = FVector::DotProduct(Shot - Fisher->GetActorLocation(), Fisher->GetActorRightVector());
            }
        }
        R.bCaught = Fisher->bRigInHand;
        AddInfo(FString::Printf(TEXT("Cast held %.1f s at %.0f fps: %s %.0f cm out%s, first speed %.0f cm/s, caught %d at %.0f cm from the shoulder, %.0f cm aside"),
            Hold, Fps, R.bLanded ? TEXT("landed") : TEXT("NOT landed"), R.Out, R.bByItself ? TEXT(" (laid by itself)") : TEXT(""), R.StartSpeed, R.bCaught ? 1 : 0, R.CatchGap, R.CatchAside));
        Fisher->RodVisual->Destroy(); Fisher->Destroy(); PC->Destroy();
        return R;
    };
    const FResult Short = Cast(0.4f, 60), Middle = Cast(1.0f, 60), Long = Cast(2.1f, 60), Held = Cast(4.0f, 60), Slow = Cast(1.0f, 30);
    for (const FResult* R : { &Short, &Middle, &Long, &Held, &Slow })
    {
        TestTrue(TEXT("Every cast comes down on the water"), R->bLanded);
        TestTrue(TEXT("The rig is not thrown: it starts its swing from the hand's rest"), R->StartSpeed < 200);
        TestTrue(TEXT("LMB again lifts the rig out and the free hand takes it within its reach"), R->bCaught && R->CatchGap <= ALakeCharacter::ArmReach + 1);
        TestTrue(TEXT("Lifted out, the rig comes in to the middle of the body or left of it, not off to the right"), R->CatchAside < 20);
    }
    TestTrue(TEXT("The longer LMB is held, the farther the rig comes down"), Short.Out + 100 < Middle.Out && Middle.Out + 50 < Long.Out);
    TestTrue(TEXT("A short hold lays the rig close, under the rod tip"), Short.Out < 450);
    TestTrue(TEXT("A full swing carries the rig out past 6.5 m"), Long.Out > 650);
    TestTrue(TEXT("Held to the end, the rod lays the rig down by itself, well out"), Held.bByItself && Held.Out > Middle.Out);
    TestTrue(TEXT("The frame rate does not change where the cast comes down"), FMath::Abs(Slow.Out - Middle.Out) < 60);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeWindDriftTest, "QuietLake.Fishing.WindDrift", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeWindDriftTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    FVector2D Facing;
    const FVector2D Edge = LakeMap::StartSpot(&Facing) + Facing * 90;
    const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X));
    ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(FVector(Edge, 100), FRotator(0, Yaw, 0));
    APlayerController* Controller = World->SpawnActor<APlayerController>();
    Controller->Possess(Fisher);
    Controller->SetControlRotation(FRotator(-12, Yaw, 0));
    ALakeWeather* Weather = World->SpawnActor<ALakeWeather>();
    FLakeWeatherState Breeze;
    Breeze.Wind = 0.5f;             // 6 m/s
    Breeze.WindYaw = Yaw + 90;      // across the cast
    Weather->SetWeatherNow(Breeze);
    const FVector Downwind = FRotator(0, Breeze.WindYaw, 0).Vector();
    auto Cast = [&](float Depth)
    {
        Fisher->FloatDepth = Depth;
        FLakeFishingFixture::ImmersedRig(Fisher);
        for (int32 Pose = 0; Pose < 60; ++Pose) Fisher->UpdateRodPose(0.05f);
    };
    auto Blow = [&](float Seconds)
    {
        for (float T = 0; T < Seconds; T += 0.1f) { Fisher->DriftFloat(0.1f); Fisher->ConstrainFloat(); }
    };
    auto LiftOut = [&]()
    {
        Fisher->ServerCancel_Implementation();
        if (Fisher->bRetrieving) { Fisher->RetrieveLift = 1; Fisher->UpdateRetrieve(0); }
    };
    // A shallow bait hangs free: the float drifts downwind, and the line holds it within reach.
    Cast(20);
    TestTrue(TEXT("Cast for the drift"), Fisher->FishingState == EFishingState::Waiting);
    const FVector Start = Fisher->FloatPosition;
    Blow(5);
    const float Early = FVector::DotProduct(Fisher->FloatPosition - Start, Downwind);
    TestTrue(FString::Printf(TEXT("A fresh breeze drifts a free float downwind (%.0f cm in 5 s)"), Early), Early > 15 && Early < 80);
    Blow(60);
    const FVector Tip = Fisher->RodTip();
    const float MainLine = ALakeRod::LineLength - Fisher->HookDepth;
    const float Reach = FMath::Sqrt(FMath::Max(0.0f, MainLine * MainLine - FMath::Square(FMath::Max(0.0f, Tip.Z - LakeFish::SurfaceZ))));
    TestTrue(FString::Printf(TEXT("The line holds the drifting float (%.0f cm from the tip, reach %.0f)"), FVector::Dist2D(Fisher->FloatPosition, Tip), Reach),
        FVector::Dist2D(Fisher->FloatPosition, Tip) <= Reach + 5 && Fisher->FishingState == EFishingState::Waiting);
    TestTrue(TEXT("The bait follows the float"), FVector::Dist2D(Fisher->BaitLocation(), Fisher->FloatPosition) < 1);
    LiftOut();
    // The bait on the bottom anchors the rig.
    Cast(ALakeCharacter::MaxDepth);
    Fisher->ConstrainFloat();       // the short line first draws the float within its reach
    const FVector Anchored = Fisher->FloatPosition;
    Blow(20);
    TestTrue(TEXT("A rig on the bottom stays put in the wind"), FVector::Dist2D(Fisher->FloatPosition, Anchored) < 1);
    LiftOut();
    // No wind, no drift.
    FLakeWeatherState Calm = Breeze;
    Calm.Wind = 0;
    Weather->SetWeatherNow(Calm);
    Cast(20);
    const FVector Still = Fisher->FloatPosition;
    Blow(20);
    TestTrue(TEXT("In a calm the float stays"), FVector::Dist2D(Fisher->FloatPosition, Still) < 1);
    LiftOut();
    World->DestroyWorld(false);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeRodControlTest, "QuietLake.Fishing.RodControl", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeRodControlTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    FVector2D Facing;
    const FVector2D Bank = LakeMap::StartSpot(&Facing);
    const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X));
    const FVector Place(Bank, ALakeEnvironment::GroundHeight(Bank.X, Bank.Y) + 95);
    ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(Place, FRotator(0, Yaw, 0));
    APlayerController* PC = World->SpawnActor<APlayerController>();
    PC->Possess(Fisher);
    PC->SetControlRotation(FRotator(0, Yaw, 0));
    Fisher->MoveRodAxis(10, false);
    TestTrue(TEXT("Aside the camera turns with the rod, without LMB"),
        FMath::Abs(FMath::FindDeltaAngleDegrees(Yaw, PC->GetControlRotation().Yaw) - 10) < 0.01f && Fisher->SwingYaw == 0);
    Fisher->MoveRodAxis(110, false);
    TestTrue(TEXT("The whole mouse motion turns the camera"), FMath::Abs(FMath::FindDeltaAngleDegrees(Yaw, PC->GetControlRotation().Yaw) - 120) < 0.02f);
    Fisher->MoveRodAxis(-120, false);
    Fisher->SwingPitch = 0;
    auto RodAngle = [&] { return ALakeCharacter::SwingBase + FRotator::NormalizeAxis(PC->GetControlRotation().Pitch) + Fisher->SwingPitch; };
    const float RodBefore = RodAngle();
    Fisher->Look(-8);
    TestTrue(TEXT("Forward mouse raises the rod"), RodAngle() > RodBefore + 5);
    TestTrue(TEXT("The camera rises with the rod"), FRotator::NormalizeAxis(PC->GetControlRotation().Pitch) > 5);
    // Casting: the camera stays within its limits, the rod goes on to the ground and to vertical.
    Fisher->bCharging = true;
    const float CamMin = IConsoleManager::Get().FindConsoleVariable(TEXT("lake.CastCameraMin"))->GetFloat();
    const float CamMax = IConsoleManager::Get().FindConsoleVariable(TEXT("lake.CastCameraMax"))->GetFloat();
    Fisher->MoveRodAxis(-400, true);
    TestTrue(TEXT("Casting, the camera stops at its lowest pitch and the rod goes on to the ground"),
        FMath::Abs(FRotator::NormalizeAxis(PC->GetControlRotation().Pitch) - CamMin) < 0.01f && FMath::Abs(RodAngle() - Fisher->RodLowestAngle) < 0.01f);
    Fisher->MoveRodAxis(Fisher->RodHighestAngle - Fisher->RodLowestAngle + 100, true);
    TestTrue(TEXT("Casting, the camera stops at its highest pitch and the rod goes on to vertical"),
        FMath::Abs(FRotator::NormalizeAxis(PC->GetControlRotation().Pitch) - CamMax) < 0.01f && FMath::Abs(RodAngle() - Fisher->RodHighestAngle) < 0.01f);
    Fisher->MoveRodAxis(-(Fisher->RodHighestAngle - (ALakeCharacter::SwingBase + CamMax)), true);
    TestTrue(TEXT("Lowered again, the rod returns to the camera before the camera moves"),
        FMath::Abs(Fisher->SwingPitch) < 0.01f && FMath::Abs(FRotator::NormalizeAxis(PC->GetControlRotation().Pitch) - CamMax) < 0.01f);
    Fisher->bCharging = false;
    PC->SetControlRotation(FRotator(0, PC->GetControlRotation().Yaw, 0));
    Fisher->SwingPitch = 0;
    Fisher->Look(-8);
    const float AimBeforePress = Fisher->SwingPitch;
    Fisher->StartCast();
    TestEqual(TEXT("LMB does not reset the aim"), Fisher->SwingPitch, AimBeforePress);
    auto CheckWalk = [&]
    {
        Fisher->ConsumeMovementInputVector();
        Fisher->Forward(1);
        TestTrue(TEXT("Forward movement input reaches CharacterMovement during casting"), FVector::DotProduct(Fisher->ConsumeMovementInputVector(), Fisher->GetActorForwardVector()) > 0.9f);
        Fisher->Right(-1);
        TestTrue(TEXT("Strafing input reaches CharacterMovement during casting"), FVector::DotProduct(Fisher->ConsumeMovementInputVector(), Fisher->GetActorRightVector()) < -0.9f);
    };
    CheckWalk();
    for (bool bInFlight : { false, true })
    {
        Fisher->bCharging = !bInFlight;
        Fisher->bRigFlying = bInFlight;
        Fisher->SwingYaw = Fisher->SwingPitch = 0;
        PC->SetControlRotation(FRotator(0, Yaw, 0));
        Fisher->MoveRodAxis(24, false);
        Fisher->MoveRodAxis(24, true);
        TestTrue(TEXT("The head follows near twenty degrees during hold and flight on both axes"),
            FMath::FindDeltaAngleDegrees(Yaw, PC->GetControlRotation().Yaw) > 4 && PC->GetControlRotation().Pitch > 4);
        CheckWalk();
    }
    Fisher->ServerSwing_Implementation(999, -999);
    TestEqual(TEXT("The server clamps remote pitch"), Fisher->SwingPitch, Fisher->RodPitchUpLimit);
    TestEqual(TEXT("The server clamps remote yaw"), Fisher->SwingYaw, -Fisher->RodYawLimit);
    Fisher->FinishFishing(TEXT("test"));

    World->DestroyWorld(false);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeFishMotionTest, "QuietLake.Fishing.FishMotion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeFishMotionTest::RunTest(const FString& Parameters)
{
    using namespace LakeFish;

    // Clients: a fish swimming a circle, updates at 12 Hz arriving a little unevenly, shown at
    // 60 fps. The shown fish keeps on the path without stepping; the old easing towards the last
    // update lagged behind and jerked at every update.
    {
        FFishNetSmoothing Smooth;
        FVector Old = FVector::ZeroVector;
        FRandomStream R(3);
        const float Radius = 150, Speed = 30, Omega = Speed / Radius;
        auto Path = [&](double T) { return FVector(Radius * FMath::Cos(Omega * T), Radius * FMath::Sin(Omega * T), -100); };
        auto PathVel = [&](double T) { return FVector(-Speed * FMath::Sin(Omega * T), Speed * FMath::Cos(Omega * T), 0); };
        double NextUpdate = 0;
        float MaxError = 0, MaxAccel = 0, OldMaxAccel = 0, MaxYawJump = 0;
        FVector PrevP = FVector::ZeroVector, PrevV = FVector::ZeroVector, OldPrevP = FVector::ZeroVector, OldPrevV = FVector::ZeroVector;
        float PrevYaw = 0;
        const float Dt = 1.0f / 60;
        for (int32 Frame = 0; Frame < 600; ++Frame)
        {
            const double T = Frame * Dt;
            if (T >= NextUpdate)
            {
                FVector Q = Path(T);
                Q = FVector(FMath::RoundToFloat(Q.X * 10) / 10, FMath::RoundToFloat(Q.Y * 10) / 10, Q.Z);
                Smooth.Receive(Q, PathVel(T), PathVel(T).Rotation(), T);
                if (Frame == 0) Old = Q;
                NextUpdate = T + 1.0 / 12 + R.FRandRange(-0.025f, 0.025f);
            }
            Smooth.Advance(Dt, T + Dt);
            Old = FMath::VInterpTo(Old, Path(NextUpdate - 1.0 / 12), Dt, 6);
            if (Frame > 60)
            {
                MaxError = FMath::Max(MaxError, (float)FVector::Dist(Smooth.Location, Path(T + Dt)));
                const FVector V = (Smooth.Location - PrevP) / Dt, OldV = (Old - OldPrevP) / Dt;
                if (Frame > 62)
                {
                    MaxAccel = FMath::Max(MaxAccel, (float)((V - PrevV).Size() / Dt));
                    OldMaxAccel = FMath::Max(OldMaxAccel, (float)((OldV - OldPrevV).Size() / Dt));
                }
                MaxYawJump = FMath::Max(MaxYawJump, FMath::Abs(FMath::FindDeltaAngleDegrees(PrevYaw, Smooth.Rotation.Yaw)));
                PrevV = V;
                OldPrevV = OldV;
            }
            PrevP = Smooth.Location;
            OldPrevP = Old;
            PrevYaw = Smooth.Rotation.Yaw;
        }
        AddInfo(FString::Printf(TEXT("Client smoothing: error %.1f cm, max acceleration %.0f cm/s2 (old easing %.0f), max heading step %.2f deg/frame"), MaxError, MaxAccel, OldMaxAccel, MaxYawJump));
        TestTrue(TEXT("The shown fish keeps to the server's path"), MaxError < 5);
        TestTrue(TEXT("The shown fish moves without steps between updates"), MaxAccel < 60 && MaxAccel < OldMaxAccel * 0.25f);
        TestTrue(TEXT("The shown heading turns smoothly"), MaxYawJump < 1.0f);
    }

    // A client that stands still for a long frame (18 s building the world after joining): the
    // engine moves its game clock on by only 0.4 s, but the fish swam on for 18 s. Told the real
    // time, the client starts the fish afresh from the next update; with the game clock alone it
    // took the 3.6 m as a guessing error and raced the fish back at metres a second.
    {
        auto Stall = [](bool bRealTime, bool& bOutRestarted, float& OutMaxSpeed)
        {
            const FVector V(20, 0, 0);
            auto Path = [&V](double S) { return FVector(0, 0, -100) + V * S; };   // the server runs in real time
            FFishNetSmoothing Smooth;
            const float Dt = 1.0f / 60;
            double T = 0, Real = 0, NextUpdate = 0;
            FVector Prev = FVector::ZeroVector;
            bool bHavePrev = false;
            bOutRestarted = false;
            OutMaxSpeed = 0;
            for (int32 Frame = 0; Frame < 300; ++Frame)
            {
                // As in the engine: packets first, then the clocks move on, then the actors tick.
                if (Real >= NextUpdate)
                {
                    Smooth.Receive(Path(Real), V, V.Rotation(), T, bRealTime ? Real : -1.0);
                    NextUpdate = Real + 1.0 / 12;
                }
                if (Smooth.bRestarted) { bOutRestarted = true; Smooth.bRestarted = false; bHavePrev = false; }
                const float FrameReal = Frame == 120 ? 18.0f : Dt, FrameGame = FMath::Min(FrameReal, 0.4f);
                T += FrameGame;
                Real += FrameReal;
                Smooth.Advance(FrameGame, T);
                if (bHavePrev && Frame > 121) OutMaxSpeed = FMath::Max(OutMaxSpeed, (float)FVector::Dist(Smooth.Location, Prev) / Dt);
                Prev = Smooth.Location;
                bHavePrev = Frame != 120;   // nothing is seen moving during the stalled frame itself
            }
        };
        bool bRestarted = false, bOldRestarted = false;
        float MaxSpeed = 0, OldMaxSpeed = 0;
        Stall(true, bRestarted, MaxSpeed);
        Stall(false, bOldRestarted, OldMaxSpeed);
        AddInfo(FString::Printf(TEXT("Client stall: restarted %d, max shown speed %.0f cm/s (game clock only: %.0f)"), (int32)bRestarted, MaxSpeed, OldMaxSpeed));
        TestTrue(TEXT("After the client stood still the fish starts afresh"), bRestarted);
        TestTrue(TEXT("After the client stood still the fish swims on at its own speed"), MaxSpeed < 30 && OldMaxSpeed > 300);
    }

    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ALakeLife* Life = World->SpawnActor<ALakeLife>();
    FRandomStream Random(9);
    const FVector Open(LakeMap::Centre, SurfaceZ - 120);

    // A turn about: asked to go back the way it came, a fish swims round in an arc - the turn rate
    // builds up and dies away, it keeps moving, and a big fish turns more slowly than a small one.
    auto TurnAbout = [&](EFishSpecies Species, float Weight, float& OutSeconds, float& OutMinSpeed, float& OutMaxRate, float& OutMaxRateStep)
    {
        ALakeFish* Fish = SpawnFish(World, Species, Weight, Open);
        const FFishSpeciesInfo& S = Info(Species);
        const float Dt = 0.02f;
        auto Swim = [&](const FVector& Want) { Fish->SimTime += Dt; Fish->Move(Want, S.CruiseSpeed, Dt); };
        Fish->SetActorRotation(FRotator::ZeroRotator);
        Fish->Yaw = 0;
        for (int32 I = 0; I < 150; ++I) Swim(FVector(S.CruiseSpeed, 0, 0));
        OutSeconds = -1; OutMinSpeed = 1e9f; OutMaxRate = 0; OutMaxRateStep = 0;
        float PrevRate = Fish->YawRate;
        for (int32 I = 0; I < 600; ++I)
        {
            Swim(FVector(-S.CruiseSpeed, 0, 0));
            OutMinSpeed = FMath::Min(OutMinSpeed, (float)Fish->Velocity().Size());
            OutMaxRate = FMath::Max(OutMaxRate, FMath::Abs(Fish->YawRate));
            OutMaxRateStep = FMath::Max(OutMaxRateStep, FMath::Abs(Fish->YawRate - PrevRate) / Dt);
            PrevRate = Fish->YawRate;
            if (OutSeconds < 0 && FMath::Abs(FMath::FindDeltaAngleDegrees(Fish->Yaw, 180)) < 10) OutSeconds = (I + 1) * Dt;
        }
        Fish->Destroy();
    };
    for (EFishSpecies Species : { EFishSpecies::Perch, EFishSpecies::Roach, EFishSpecies::Crucian, EFishSpecies::Pike })
    {
        const FFishSpeciesInfo& S = Info(Species);
        const FFishGait& G = Gait(Species);
        float SmallTime, SmallMin, SmallRate, SmallStep, BigTime, BigMin, BigRate, BigStep;
        TurnAbout(Species, S.MinWeight, SmallTime, SmallMin, SmallRate, SmallStep);
        TurnAbout(Species, S.MaxWeight, BigTime, BigMin, BigRate, BigStep);
        AddInfo(FString::Printf(TEXT("%s turns about: small %.1f s (slowest %.0f cm/s, %.0f deg/s, %.0f deg/s2), big %.1f s (%.0f cm/s, %.0f deg/s)"),
            *S.Name, SmallTime, SmallMin, SmallRate, SmallStep, BigTime, BigMin, BigRate));
        TestTrue(FString::Printf(TEXT("%s comes round"), *S.Name), SmallTime > 0 && BigTime > 0);
        TestTrue(FString::Printf(TEXT("%s turns in an arc, not on the spot"), *S.Name), SmallMin > 0.2f * S.CruiseSpeed && BigMin > 0.2f * S.CruiseSpeed);
        TestTrue(FString::Printf(TEXT("%s turn rate builds up gradually"), *S.Name), SmallStep <= G.TurnAccel * 1.25f * 1.01f && SmallRate <= S.TurnRate * 1.25f * 1.01f);
        TestTrue(FString::Printf(TEXT("A big %s turns more slowly"), *S.Name), BigTime > SmallTime * 1.15f);
    }

    // A school of roach: they keep their spacing, line up and move as one.
    TArray<ALakeFish*> School;
    const FVector Home = PickSpot(EFishSpecies::Roach, Random, LakeMap::Centre, 0);
    for (int32 I = 0; I < 8; ++I) School.Add(SpawnFish(World, EFishSpecies::Roach, 0.08f + 0.03f * I, Home + FVector((I % 4) * 30, (I / 4) * 30, 0), 3));
    bool bInWater = true;
    for (int32 Step = 0; Step < 1800; ++Step)
    {
        Life->Step(0.05f);
        for (ALakeFish* Fish : School)
        {
            Fish->Simulate(0.05f);
            const FVector P = Fish->GetActorLocation();
            bInWater &= LakeMap::IsWater(P.X, P.Y) && P.Z <= SurfaceZ && P.Z >= ALakeFish::BottomZ(P.X, P.Y);
        }
    }
    const ALakeLife::FMotionStats& M = Life->Motion[(int32)EFishSpecies::Roach];
    const float Spacing = M.SchoolSamples ? M.Spacing / M.SchoolSamples : 0, Order = M.SchoolSamples ? M.Order / M.SchoolSamples : 0;
    AddInfo(FString::Printf(TEXT("Roach school over 90 s: nearest mate %.2f lengths, order %.2f, speed %.1f cm/s, still %.0f%%, spins %.2f%%, max turn %.0f deg/s"),
        Spacing, Order, M.Samples ? M.Speed / M.Samples : 0.0, M.Samples ? 100.0f * M.Still / M.Samples : 0.0f, M.Samples ? 100.0f * M.Spins / M.Samples : 0.0f, M.MaxTurn));
    TestTrue(TEXT("School mates keep about a body length or two apart"), Spacing > 0.5f && Spacing < 3.0f);
    TestTrue(TEXT("The school swims lined up"), Order > 0.7f);
    TestTrue(TEXT("Fish do not spin on the spot"), M.Samples > 0 && M.Spins * 100 < M.Samples);
    TestTrue(TEXT("The school stays in the water"), bInWater);

    // Disturbed: a mild one makes it turn calmly away, a close splash makes it bolt.
    ALakeFish* Shy = School[3];
    const FVector Source = Shy->GetActorLocation() + Shy->GetActorRightVector() * 150;
    const float Before = FVector::Dist2D(Shy->GetActorLocation(), Source);
    Life->Disturb(Source, 600, 0.45f);
    for (int32 Step = 0; Step < 80; ++Step) { Life->Step(0.05f); for (ALakeFish* Fish : School) Fish->Simulate(0.05f); }
    const float After = FVector::Dist2D(Shy->GetActorLocation(), Source);
    AddInfo(FString::Printf(TEXT("Shying away: %.0f -> %.0f cm from the plop in 4 s, free %d"), Before, After, Shy->IsFree()));
    TestTrue(TEXT("A mild disturbance: the fish swims away and stays at ease"), Shy->IsFree() && After > Before + 60);
    Shy->Startle(Shy->GetActorLocation() + FVector(40, 0, 0), 0.9f);
    TestFalse(TEXT("A splash right beside it: it bolts"), Shy->IsFree());

    // Sizes: the model follows the weight in length and build.
    const FFishSpeciesInfo& CrucianInfo = LakeFish::Info(EFishSpecies::Crucian);
    TestTrue(TEXT("A big crucian is deeper-bodied than a small one"), Girth(EFishSpecies::Crucian, CrucianInfo.MaxWeight) > Girth(EFishSpecies::Crucian, CrucianInfo.MinWeight) + 0.1f);
    TestTrue(TEXT("Big fish are calmer"), SpawnFish(World, EFishSpecies::Perch, 0.8f, Open)->Agility < SpawnFish(World, EFishSpecies::Perch, 0.08f, Open + FVector(200, 0, 0))->Agility);

    // A feeding crucian roots nose down at the bottom.
    const FVector FeedSpot = PickSpot(EFishSpecies::Crucian, Random, LakeMap::Centre, 0, nullptr, EFishGoal::Feed);
    ALakeFish* Crucian = SpawnFish(World, EFishSpecies::Crucian, 0.5f, FeedSpot);
    Crucian->Hunger = 1;
    Crucian->SetGoal(EFishGoal::Feed, 600);
    Crucian->RoamTarget = FeedSpot;
    Crucian->bAtSpot = true;
    float Steepest = 0, Lowest = 1000;
    for (int32 Step = 0; Step < 600; ++Step)
    {
        Life->Step(0.05f);
        Crucian->Simulate(0.05f);
        if (Crucian->bRooting)
        {
            Steepest = FMath::Min(Steepest, (float)Crucian->GetActorRotation().Pitch);
            Lowest = FMath::Min(Lowest, (float)(Crucian->GetActorLocation().Z - ALakeFish::BottomZ(Crucian->GetActorLocation().X, Crucian->GetActorLocation().Y)));
        }
    }
    AddInfo(FString::Printf(TEXT("Crucian rooting: nose down to %.0f deg, %.0f cm over the bottom"), Steepest, Lowest));
    TestTrue(TEXT("A feeding crucian roots nose down at the bottom"), Steepest < -15 && Lowest < 20);
    World->DestroyWorld(false);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeRipplesTest, "QuietLake.Fishing.Ripples", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeRipplesTest::RunTest(const FString& Parameters)
{
    auto AllWater = [](float, float) { return true; };
    constexpr float Dt = 1.0f / 60.0f;
    auto PeakAlongX = [](const FLakeRippleSim& Sim, FVector2D From, float MaxR, float* OutHeight = nullptr)
    {
        float Best = 0, BestR = 0;
        for (float R = 4; R <= MaxR; R += 1)
        {
            const float H = FMath::Abs(Sim.HeightAt(From + FVector2D(R, 0)));
            if (H > Best) { Best = H; BestR = R; }
        }
        if (OutHeight) *OutHeight = Best;
        return BestR;
    };

    // A drop sends out a ring that travels at about the wave speed and fades.
    {
        FLakeRippleSim Sim;
        Sim.Init(256, 4.0f, FVector2D::ZeroVector, AllWater);
        Sim.Push(FVector2D::ZeroVector, 4.0f, -60.0f);
        for (int32 I = 0; I < 30; ++I) Sim.Step(Dt);
        const float Early = Sim.Energy();
        for (int32 I = 0; I < 30; ++I) Sim.Step(Dt);
        float Height = 0;
        const float Radius = PeakAlongX(Sim, FVector2D::ZeroVector, 300, &Height);
        AddInfo(FString::Printf(TEXT("Ring after 1 s: radius %.0f cm (wave speed %.0f cm/s), height %.2f mm"), Radius, Sim.WaveSpeed, Height * 10));
        TestTrue(TEXT("The ring runs out at about the wave speed"), Radius > 0.55f * Sim.WaveSpeed && Radius < 1.35f * Sim.WaveSpeed);
        for (int32 I = 0; I < 120; ++I) Sim.Step(Dt);
        AddInfo(FString::Printf(TEXT("Energy 0.5 s %.4f, 3 s %.4f"), Early, Sim.Energy()));
        TestTrue(TEXT("The waves die out"), Sim.Energy() < 0.5f * Early && Sim.Energy() > 0);
    }

    // A downpour for ten seconds: the surface settles to a steady chop and never builds up.
    {
        FLakeRippleSim Sim;
        Sim.Init(128, 4.0f, FVector2D::ZeroVector, AllWater);
        FRandomStream Random(7);
        auto Rms = [&Sim] { return FMath::Sqrt(Sim.Energy() / Sim.H.Num()); };
        float Half = 0;
        for (int32 I = 0; I < 600; ++I)
        {
            // About 2.5 drops per square metre and second, as in the game.
            Sim.Drop(FVector2D(Random.FRandRange(-250, 250), Random.FRandRange(-250, 250)), 1.5f, 35.0f);
            Sim.Step(Dt);
            if (I == 299) Half = Rms();
        }
        AddInfo(FString::Printf(TEXT("Downpour: mean height %.2f mm after 5 s, %.2f mm after 10 s"), Half * 10, Rms() * 10));
        TestTrue(TEXT("Steady rain keeps the surface steady"), FMath::IsFinite(Rms()) && Rms() < 1.4f * Half && Rms() < 0.2f);
    }

    // A source faster than the waves (a fish darting just under the surface) leaves a V wake:
    // still water ahead of it, two arms opening behind at sin(a) = c / v.
    {
        FLakeRippleSim Sim;
        Sim.Init(256, 4.0f, FVector2D::ZeroVector, AllWater);
        const float Speed = 80.0f;
        FVector2D Src(-300, 0);
        for (int32 I = 0; I < 240; ++I)
        {
            Src.X += Speed * Dt;
            Sim.Push(Src + FVector2D(4, 0), 3.0f, 400.0f * Dt);
            Sim.Push(Src - FVector2D(2, 0), 4.0f, -400.0f / 1.78f * Dt);
            Sim.Step(Dt);
        }
        const float Behind = 80.0f;
        float ArmY = 0, Arm = 0, Ahead = 0;
        for (float Y = 12; Y <= 150; Y += 1)
        {
            const float H = FMath::Max(FMath::Abs(Sim.HeightAt(Src + FVector2D(-Behind, Y))), FMath::Abs(Sim.HeightAt(Src + FVector2D(-Behind, -Y))));
            if (H > Arm) { Arm = H; ArmY = Y; }
        }
        for (float X = 20; X <= 80; X += 2)
            for (float Y = -60; Y <= 60; Y += 4)
                Ahead = FMath::Max(Ahead, FMath::Abs(Sim.HeightAt(Src + FVector2D(X, Y))));
        const float Expected = Behind * FMath::Tan(FMath::Asin(Sim.WaveSpeed / Speed));
        AddInfo(FString::Printf(TEXT("Wake 80 cm behind: arm at %.0f cm (expected %.0f), %.2f mm; ahead %.2f mm"), ArmY, Expected, Arm * 10, Ahead * 10));
        TestTrue(TEXT("The wake opens in a V behind a fast source"), ArmY > 0.6f * Expected && ArmY < 1.5f * Expected);
        TestTrue(TEXT("Water ahead of a fast source stays still"), Ahead < 0.3f * Arm);
    }

    // Moving the window keeps every wave where it was in the world.
    {
        FLakeRippleSim Sim;
        Sim.Init(128, 4.0f, FVector2D::ZeroVector, AllWater);
        Sim.Push(FVector2D(20, -10), 4.0f, -60.0f);
        for (int32 I = 0; I < 40; ++I) Sim.Step(Dt);
        const FVector2D Probe[] = { FVector2D(30, 0), FVector2D(0, 35), FVector2D(-20, -40) };
        float Before[3];
        for (int32 I = 0; I < 3; ++I) Before[I] = Sim.HeightAt(Probe[I]);
        Sim.Recentre(FVector2D(83, -47), AllWater);
        float Worst = 0;
        for (int32 I = 0; I < 3; ++I) Worst = FMath::Max(Worst, FMath::Abs(Sim.HeightAt(Probe[I]) - Before[I]));
        TestTrue(TEXT("The window moves without moving the waves"), Worst < 1e-5f && FMath::Abs(Before[0]) > 1e-4f);
    }

    // The bank: the water ends at X = 100; it stays flat there and the waves turn back.
    {
        auto Bank = [](float X, float) { return X < 100; };
        FLakeRippleSim Sim;
        Sim.Init(128, 4.0f, FVector2D::ZeroVector, Bank);
        Sim.Push(FVector2D(40, 0), 4.0f, -60.0f);
        float Land = 0;
        for (int32 I = 0; I < 120; ++I)
        {
            Sim.Step(Dt);
            for (float X = 110; X < 200; X += 8) Land = FMath::Max(Land, FMath::Abs(Sim.HeightAt(FVector2D(X, 0))));
        }
        TestTrue(TEXT("No waves on the land"), Land == 0);
    }

    // A fish just under the surface leaves a small wake that grows with the fish, much smaller than
    // the old one (12 * sqrt(length / 20), fading over 12 cm); 25 cm down it leaves none.
    {
        auto Wake = [&](float Length, float Over, float Speed, bool bOld)
        {
            FLakeRippleSim Sim;
            Sim.Init(256, 4.0f, FVector2D::ZeroVector, AllWater);
            FVector2D Src(-300, 0);
            float Peak = 0;
            const float Push = bOld ? FMath::Exp(-Over / 12.0f) * Speed * FMath::Sqrt(Length / 20.0f) * 12.0f : LakeRipples::FishWake(Length, Over, Speed);
            for (int32 I = 0; I < 180; ++I)
            {
                Src.X += Speed * Dt;
                const float Width = FMath::Max(0.18f * Length, 2.5f);
                Sim.Push(Src + FVector2D(0.35f * Length, 0), Width, Push * Dt);
                Sim.Push(Src - FVector2D(0.1f * Length, 0), Width * 1.3f, -Push / 1.69f * Dt);
                Sim.Step(Dt);
                for (float X = -100; X <= 40; X += 4)
                    for (float Y = -60; Y <= 60; Y += 4) Peak = FMath::Max(Peak, FMath::Abs(Sim.HeightAt(Src + FVector2D(X, Y))));
            }
            return Peak;
        };
        const float Small = Wake(10, 2, 30, false), Mid = Wake(25, 2, 30, false), Big = Wake(50, 2, 30, false), Old = Wake(25, 2, 30, true);
        const float Dash = Wake(20, 0, 160, false), OldDash = Wake(20, 0, 160, true);
        AddInfo(FString::Printf(TEXT("Fish wakes 2 cm under at 30 cm/s: 10 cm fish %.2f mm, 25 cm %.2f mm (old %.2f mm), 50 cm %.2f mm; a 20 cm perch dashing at the top %.2f mm (old %.2f mm)"),
            Small * 10, Mid * 10, Old * 10, Big * 10, Dash * 10, OldDash * 10));
        TestTrue(TEXT("A fish's wake grows with the fish"), Small < 0.6f * Mid && Mid < 0.6f * Big);
        TestTrue(TEXT("A fish's wake is many times smaller than it was"), Mid < 0.1f * Old && Dash < 0.1f * OldDash);
        TestTrue(TEXT("A fish's wake is millimetres high"), Mid < 0.35f && Big < 1.0f && Dash < 0.35f);
        TestTrue(TEXT("25 cm down a fish leaves no wake"), LakeRipples::FishWake(40, 26, 100) == 0);
    }
    return true;
}

// Live Fish stocks the pond: every species in its numbers, a school in reach of every fishing spot,
// and a fish taken from a school comes back into the same school.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeStockingTest, "QuietLake.Fishing.Stocking", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeStockingTest::RunTest(const FString& Parameters)
{
    // As the passports say (the pond is being restocked stage by stage: maybe empty yet); no fry without fish.
    {
        UWorld* Plain = UWorld::CreateWorld(EWorldType::Game, false);
        ALakeLife* PlainLife = Plain->SpawnActor<ALakeLife>();
        PlainLife->StockPond();
        int32 Want = 0, Got = 0;
        for (int32 Sp = 0; Sp < LakeFish::NumSpecies(); ++Sp) Want += LakeFish::Info((EFishSpecies)Sp).Stock.Count;
        for (TActorIterator<ALakeFish> It(Plain); It; ++It) ++Got;
        AddInfo(FString::Printf(TEXT("Stocked from the passports: %d fish of %d, %d fry shoals"), Got, Want, PlainLife->FryShoals().Num()));
        TestTrue(TEXT("The passports' counts are stocked"), Got <= Want && Got >= Want * 9 / 10);
        if (Want == 0) TestTrue(TEXT("No fry in an empty pond"), PlainLife->FryShoals().IsEmpty());
        Plain->DestroyWorld(false);
    }

    // The whole old stock, set over the passports: how Live Fish lays out a full pond.
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ALakeLife* Life = World->SpawnActor<ALakeLife>();
    Life->StockOverride = { { TEXT("perch"), 144 }, { TEXT("roach"), 208 }, { TEXT("crucian"), 240 }, { TEXT("pike"), 30 },
        { TEXT("bream"), 150 }, { TEXT("rudd"), 120 }, { TEXT("bleak"), 150 } };
    Life->StockPond();
    Life->Step(0);
    TestTrue(FString::Printf(TEXT("Fry shoals come with the fish (%d)"), Life->FryShoals().Num()), Life->FryShoals().Num() >= 8);
    // By species (a species file added for a try counts too).
    TArray<int32> Count;
    Count.Init(0, LakeFish::NumSpecies());
    TMap<int32, int32> Members;
    TArray<ALakeFish*> All;
    bool bInWater = true;
    for (TActorIterator<ALakeFish> It(World); It; ++It)
    {
        ALakeFish* Fish = *It;
        All.Add(Fish);
        ++Count[(int32)Fish->Species];
        if (Fish->GetSchool() >= 0) ++Members.FindOrAdd(Fish->GetSchool());
        bInWater &= LakeMap::IsWater(Fish->GetActorLocation().X, Fish->GetActorLocation().Y);
    }
    AddInfo(FString::Printf(TEXT("Stocked: %d perch, %d roach, %d crucian, %d pike in %d schools"), Count[0], Count[1], Count[2], Count[3], Members.Num()));
    TestEqual(TEXT("Perch: as many as set"), Count[(int32)EFishSpecies::Perch], 144);
    TestEqual(TEXT("Roach: as many as set"), Count[(int32)EFishSpecies::Roach], 208);
    TestEqual(TEXT("Crucian"), Count[(int32)EFishSpecies::Crucian], 240);
    TestEqual(TEXT("Pike"), Count[(int32)EFishSpecies::Pike], 30);
    TestTrue(TEXT("Every fish is in the water"), bInWater);

    // Crucian keep in loose groups by size at their own spots (stock.sizes, stock.groups, stock.spots
    // in the passport): small ones together, big ones alone or in twos; none at the other spots.
    {
        const FFishSpeciesInfo& C = LakeFish::Info(EFishSpecies::Crucian);
        const TConstArrayView<LakeMap::FSpot> AllSpots = LakeMap::Spots();
        auto ToWater = [](const LakeMap::FSpot& Spot, FVector2D P)
        {
            FVector2D A = Spot.Location - Spot.Outward * 200, B = Spot.Location - Spot.Outward * 600;
            if (!Spot.bBoards)
            {
                const LakeMap::FPier& Pier = LakeMap::Pier();
                A = Pier.Root + Pier.Dir * (Pier.Length + 200);
                B = Pier.Root + Pier.Dir * (Pier.Length + 600);
            }
            const float T = FMath::Clamp(FVector2D::DotProduct(P - A, B - A) / (B - A).SizeSquared(), 0.0f, 1.0f);
            return (float)FVector2D::Distance(P, A + (B - A) * T);
        };
        TArray<int32> AtSpot;
        AtSpot.Init(0, AllSpots.Num());
        TMap<FIntPoint, int32> SmallHomes, BigHomes;
        int32 Crucians = 0, Small = 0, BigCount = 0;
        bool bWeights = true;
        for (const ALakeFish* Fish : All)
        {
            if (Fish->Species != EFishSpecies::Crucian) continue;
            ++Crucians;
            bWeights &= Fish->WeightKg >= C.MinWeight - 0.001f && Fish->WeightKg <= C.MaxWeight + 0.001f;
            const FVector2D Home(Fish->GetHomeAnchor());
            const FIntPoint Key(FMath::RoundToInt(Home.X), FMath::RoundToInt(Home.Y));
            if (Fish->WeightKg < 0.3f) { ++Small; ++SmallHomes.FindOrAdd(Key); }
            if (Fish->WeightKg > 1.0f) { ++BigCount; ++BigHomes.FindOrAdd(Key); }
            int32 Owner = INDEX_NONE;
            float Best = 600;
            for (int32 I = 0; I < AllSpots.Num(); ++I)
                if (const float D = ToWater(AllSpots[I], Home); D < Best) { Best = D; Owner = I; }
            if (Owner != INDEX_NONE) ++AtSpot[Owner];
        }
        int32 BiggestBig = 0;
        for (const TPair<FIntPoint, int32>& H : BigHomes) BiggestBig = FMath::Max(BiggestBig, H.Value);
        FString Where, Stray;
        for (int32 I = 0; I < AllSpots.Num(); ++I)
        {
            const bool bListed = C.Stock.Spots.Contains(AllSpots[I].Name);
            if (bListed) Where += FString::Printf(TEXT(" %s %d,"), AllSpots[I].Name, AtSpot[I]);
            if (bListed && AtSpot[I] < 3) Stray += FString::Printf(TEXT(" too few at %s (%d),"), AllSpots[I].Name, AtSpot[I]);
            if (!bListed && AtSpot[I] > 0) Stray += FString::Printf(TEXT(" %d at %s,"), AtSpot[I], AllSpots[I].Name);
        }
        AddInfo(FString::Printf(TEXT("Crucian: %d, under 0.3 kg %d in %d groups, over 1 kg %d in %d groups; at its spots:%s"),
            Crucians, Small, SmallHomes.Num(), BigCount, BigHomes.Num(), *Where));
        TestEqual(TEXT("Crucian: as many as set"), Crucians, 240);
        TestTrue(TEXT("Crucian: weights within the passport's"), bWeights);
        TestTrue(FString::Printf(TEXT("Crucian: most are small, as the sizes say (%d of %d under 0.3 kg)"), Small, Crucians), Small >= Crucians * 50 / 100 && Small <= Crucians * 70 / 100);
        TestTrue(TEXT("Crucian: the small ones keep in groups (at least 4 a group on average)"), SmallHomes.Num() * 4 <= Small);
        TestTrue(TEXT("Crucian: the big ones alone or in twos"), BiggestBig <= 2);
        TestTrue(FString::Printf(TEXT("Crucian: at its own spots only (%s)"), *Stray), Stray.IsEmpty());

        // Schooling species keep in schools by size at their own spots (stock.groups, stock.spots):
        // big schools of small fish, small schools of big ones; none at the other spots.
        auto CheckSchools = [&](EFishSpecies Species, float SmallUpTo, float BigFrom, int32 BigMost)
        {
            const FFishSpeciesInfo& R = LakeFish::Info(Species);
            TMap<int32, TArray<const ALakeFish*>> BySchool;
            int32 Fish = 0, Schooled = 0;
            bool bWeights = true;
            for (const ALakeFish* F : All)
            {
                if (F->Species != Species) continue;
                ++Fish;
                bWeights &= F->WeightKg >= R.MinWeight - 0.001f && F->WeightKg <= R.MaxWeight + 0.001f;
                if (F->GetSchool() >= 0) { BySchool.FindOrAdd(F->GetSchool()).Add(F); ++Schooled; }
            }
            TArray<int32> SchoolsAt;
            SchoolsAt.Init(0, AllSpots.Num());
            int32 SmallSchools = 0, SmallInThem = 0, BiggestOfBig = 0;
            for (const TPair<int32, TArray<const ALakeFish*>>& It : BySchool)
            {
                float Lightest = 1e9f, Heaviest = 0;
                for (const ALakeFish* F : It.Value) { Lightest = FMath::Min(Lightest, F->WeightKg); Heaviest = FMath::Max(Heaviest, F->WeightKg); }
                if (Heaviest < SmallUpTo) { ++SmallSchools; SmallInThem += It.Value.Num(); }
                if (Lightest >= BigFrom) BiggestOfBig = FMath::Max(BiggestOfBig, It.Value.Num());
                const FVector2D Home(It.Value[0]->GetHomeAnchor());
                int32 Nearest = INDEX_NONE;
                float Best = 600;
                for (int32 I = 0; I < AllSpots.Num(); ++I)
                    if (const float D = ToWater(AllSpots[I], Home); D < Best) { Best = D; Nearest = I; }
                if (Nearest != INDEX_NONE) ++SchoolsAt[Nearest];
            }
            FString At, Wrong;
            for (int32 I = 0; I < AllSpots.Num(); ++I)
            {
                const bool bListed = R.Stock.Spots.Contains(AllSpots[I].Name);
                if (bListed) At += FString::Printf(TEXT(" %s %d,"), AllSpots[I].Name, SchoolsAt[I]);
                if (bListed && SchoolsAt[I] == 0) Wrong += FString::Printf(TEXT(" none at %s,"), AllSpots[I].Name);
                if (!bListed && SchoolsAt[I] > 0) Wrong += FString::Printf(TEXT(" %d at %s,"), SchoolsAt[I], AllSpots[I].Name);
            }
            AddInfo(FString::Printf(TEXT("%s: %d in %d schools; small ones %d in %d schools, the biggest school of big ones %d; schools at its spots:%s"),
                *R.Id, Fish, BySchool.Num(), SmallInThem, SmallSchools, BiggestOfBig, *At));
            TestTrue(FString::Printf(TEXT("%s: weights within the passport's"), *R.Id), bWeights);
            TestEqual(FString::Printf(TEXT("%s: every one in a school"), *R.Id), Schooled, Fish);
            TestTrue(FString::Printf(TEXT("%s: the small ones in big schools (10 or more on average)"), *R.Id), SmallSchools > 0 && SmallInThem >= SmallSchools * 10);
            TestTrue(FString::Printf(TEXT("%s: the big ones in small schools"), *R.Id), BiggestOfBig <= BigMost);
            TestTrue(FString::Printf(TEXT("%s: schools at its own spots only (%s)"), *R.Id, *Wrong), Wrong.IsEmpty());
        };
        CheckSchools(EFishSpecies::Roach, 0.2f, 0.5f, 6);
        CheckSchools(EFishSpecies::Perch, 0.15f, 0.35f, 5);
        for (int32 Sp = 0; Sp < LakeFish::NumSpecies(); ++Sp)
        {
            const FString Id = LakeFish::Info((EFishSpecies)Sp).Id;
            if (Id == TEXT("bream")) CheckSchools((EFishSpecies)Sp, 0.4f, 1.5f, 7);
            if (Id == TEXT("rudd")) CheckSchools((EFishSpecies)Sp, 0.2f, 0.55f, 6);
            if (Id == TEXT("bleak")) CheckSchools((EFishSpecies)Sp, 0.035f, 0.035f, 24);
        }
        // Pike: each alone, its own patch at a spot (stock.groups apart), only at its own spots.
        {
            const FFishSpeciesInfo& P = LakeFish::Info(EFishSpecies::Pike);
            TArray<FVector2D> PikeHomes;
            TArray<int32> PikeAt;
            PikeAt.Init(0, AllSpots.Num());
            for (const ALakeFish* F : All)
                if (F->Species == EFishSpecies::Pike) PikeHomes.Add(FVector2D(F->GetHomeAnchor()));
            float Closest = 1e9f;
            for (int32 I = 0; I < PikeHomes.Num(); ++I)
            {
                for (int32 J = I + 1; J < PikeHomes.Num(); ++J) Closest = FMath::Min(Closest, (float)FVector2D::Distance(PikeHomes[I], PikeHomes[J]));
                int32 Nearest = INDEX_NONE;
                float Best = 600;
                for (int32 K = 0; K < AllSpots.Num(); ++K)
                    if (const float D = ToWater(AllSpots[K], PikeHomes[I]); D < Best) { Best = D; Nearest = K; }
                if (Nearest != INDEX_NONE) ++PikeAt[Nearest];
            }
            FString At, Wrong;
            for (int32 I = 0; I < AllSpots.Num(); ++I)
            {
                const bool bListed = P.Stock.Spots.Contains(AllSpots[I].Name);
                if (bListed) At += FString::Printf(TEXT(" %s %d,"), AllSpots[I].Name, PikeAt[I]);
                if (bListed && PikeAt[I] == 0) Wrong += FString::Printf(TEXT(" none at %s,"), AllSpots[I].Name);
                if (!bListed && PikeAt[I] > 0) Wrong += FString::Printf(TEXT(" %d at %s,"), PikeAt[I], AllSpots[I].Name);
            }
            AddInfo(FString::Printf(TEXT("pike: %d, the closest two %.0f cm apart; at its spots:%s"), PikeHomes.Num(), Closest, *At));
            TestTrue(TEXT("pike: each keeps its own patch (6 m apart, a little less where a spot's water is short)"), Closest >= 450);
            TestTrue(FString::Printf(TEXT("pike: at its own spots only (%s)"), *Wrong), Wrong.IsEmpty());
        }
    }

    // Every spot has fish in reach - a school, a loose group or a lone pike (each species keeps to its
    // own spots): within a perch's sense of the water its pole reaches (2-6 m out from the waterline;
    // from the pier, beyond its end).
    FString Empty;
    for (const LakeMap::FSpot& Spot : LakeMap::Spots())
    {
        FVector2D A = Spot.Location - Spot.Outward * 200, B = Spot.Location - Spot.Outward * 600;
        if (!Spot.bBoards)
        {
            const LakeMap::FPier& Pier = LakeMap::Pier();
            A = Pier.Root + Pier.Dir * (Pier.Length + 200);
            B = Pier.Root + Pier.Dir * (Pier.Length + 600);
        }
        bool bSchool = false;
        for (const ALakeFish* Fish : All)
        {
            const FVector2D P(Fish->GetActorLocation());
            const float T = FMath::Clamp(FVector2D::DotProduct(P - A, B - A) / (B - A).SizeSquared(), 0.0f, 1.0f);
            bSchool |= FVector2D::Distance(P, A + (B - A) * T) < 650;
        }
        if (!bSchool) Empty += FString(TEXT(" ")) + Spot.Name;
    }
    TestTrue(FString::Printf(TEXT("Fish in reach of every fishing spot (none at:%s)"), *Empty), Empty.IsEmpty());

    // A fish caught out of a school: a few game hours later another of its kind swims with the same school.
    ALakeFish* Caught = nullptr;
    for (ALakeFish* Fish : All) if (Fish->GetSchool() >= 0) { Caught = Fish; break; }
    if (Caught)
    {
        const int32 School = Caught->GetSchool();
        const int32 Before = Members[School];
        Life->Restock(Caught);
        Caught->Destroy();
        Life->Step(0);
        // The timer manager ticks once an engine frame, and a new timer starts counting at its next
        // tick: two frames of the restock time and a second.
        const float RestockSeconds = ALakeLife::RestockHours * ALakeWeather::DayMinutes * 60.0f / 24.0f;
        for (int32 Frame = 0; Frame < 2; ++Frame)
        {
            ++GFrameCounter;
            World->GetTimerManager().Tick(RestockSeconds + 1);
        }
        Life->Step(0);
        int32 After = 0;
        for (TActorIterator<ALakeFish> It(World); It; ++It) After += It->GetSchool() == School;
        const ALakeFish* Lead = Life->SchoolLeader(School);
        float Farthest = 0;
        for (TActorIterator<ALakeFish> It(World); It; ++It)
            if (It->GetSchool() == School && Lead) Farthest = FMath::Max(Farthest, (float)FVector::Dist2D(It->GetActorLocation(), Lead->GetActorLocation()));
        AddInfo(FString::Printf(TEXT("School %d: %d fish before the catch, %d after the restock, farthest %.0f cm from its leader"), School, Before, After, Farthest));
        TestEqual(TEXT("The school is whole again"), After, Before);
        TestTrue(TEXT("The new fish turned up with its school"), Farthest < 400);
    }
    World->DestroyWorld(false);
    return true;
}

// The species passports (Content/LiveFish/Species): the files read without problems and in order,
// each fish has its model; the example species in the README there is read from its file alone, a
// misspelt field and a missing diet are reported.
// Exercise the production Hooked path with fixed random streams; no frame-rate-dependent rolls.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeFightProfilesTest, "QuietLake.Fishing.FightProfiles", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeFightProfilesTest::RunTest(const FString& Parameters)
{
    using namespace LakeFish;
    using EPhase = ALakeFish::EFightPhase;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    const FVector Open(LakeMap::Centre, SurfaceZ - 80);
    ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(Open - FVector(600, 0, -150), FRotator::ZeroRotator);
    struct FStats { int32 Bursts = 0, Gaps = 0, Dives = 0, Surfaces = 0; float Rest = 0, Pull = 0; };
    TMap<FString, FStats> Stats;
    for (int32 I = 0; I < NumSpecies(); ++I)
    {
        const EFishSpecies Sp = (EFishSpecies)I;
        const FFishSpeciesInfo& S = Info(Sp);
        ALakeFish* Fish = SpawnFish(World, Sp, FMath::Lerp(S.MinWeight, S.MaxWeight, 0.4f), Open);
        Fish->Random.Initialize(1234 + I);
        Fish->FightIndividual = 1;
        Fish->Hook(Fisher);
        Fish->WakeOnLine();   // the line is taut here: the fish feels the hook and fights
        FStats& T = Stats.Add(S.Id);
        // Each frame uses the actual actor simulation, including motion, stamina and profile phases.
        EPhase Previous = Fish->FightPhase;
        for (int32 Step = 0; Step < 2400; ++Step)
        {
            Fish->SetActorLocation(Open);
            Fish->Stamina = Fish->MaxStamina; // isolate the species' fresh rhythm
            Fish->ApplyLineForce(FVector(-40, 0, 0));   // a taut line: on a slack one it makes off instead
            Fish->Simulate(0.025f);
            T.Bursts += Fish->FightPhase == EPhase::Burst && Previous != EPhase::Burst;
            T.Gaps += Fish->FightPhase == EPhase::Gap && Previous != EPhase::Gap;
            T.Rest += Fish->FightPhase == EPhase::Pause ? 0.025f : 0;
            T.Pull += Fish->FightPhase == EPhase::Pull ? 0.025f : 0;
            Previous = Fish->FightPhase;
            if (Step == 0) TestTrue(S.Id + TEXT(": Hook starts a new target immediately"), FVector::Dist(Fish->RoamTarget, Open) > 100);
        }
        for (int32 Pick = 0; Pick < 1000; ++Pick)
        {
            Fish->SetActorLocation(Open);
            Fish->Stamina = Fish->MaxStamina;
            Fish->ChooseFightTarget();
            const FVector Target = Fish->RoamTarget;
            T.Dives += Target.Z < ALakeFish::BottomZ(Target.X, Target.Y) + 20;
            T.Surfaces += Target.Z > SurfaceZ - 20;
        }
        // Under a sustained opposing load, effort drains the reserve; a genuine pause restores it.
        Fish->SetActorLocation(Open);
        Fish->Vel = FVector::ZeroVector;
        Fish->SetActorRotation(FRotator::ZeroRotator);
        Fish->Stamina = Fish->MaxStamina * 0.6f;
        Fish->FightPhase = EPhase::Pull;
        const float BeforeLoad = Fish->Stamina;
        for (int32 Step = 0; Step < 40; ++Step)
        {
            Fish->ApplyLineForce(FVector(-S.Acceleration * S.Strength * Fish->WeightKg, 0, 0));
            Fish->MoveHooked(FVector(S.BurstSpeed, 0, 0), S.BurstSpeed, 0.025f);
        }
        TestTrue(S.Id + TEXT(": resisting the line costs stamina"), Fish->Stamina < BeforeLoad - 0.2f);
        // A rest is a rest only on a slack line: leaning against a pull is work too. (The body's
        // stamina alone: on a slack line a fresh fish would rather make off, see ChooseEscapeTarget.)
        Fish->ApplyLineForce(FVector::ZeroVector);
        Fish->FightPhase = EPhase::Pause;
        Fish->FightRemaining = 4;
        Fish->Vel = FVector::ZeroVector;
        Fish->RoamTarget = Fish->GetActorLocation();
        const float BeforeRest = Fish->Stamina;
        Fish->MoveHooked(FVector::ZeroVector, 0, 0.05f);
        TestTrue(S.Id + TEXT(": Hooked pause really recovers stamina"), Fish->Stamina > BeforeRest);
        // Given slack, a fish with strength left makes off somewhere in the water instead of waiting.
        Fish->SetActorLocation(Open);
        Fish->Stamina = Fish->MaxStamina;
        Fish->FightPhase = EPhase::Pause;
        Fish->FightRemaining = 4;
        Fish->bWasSlack = false;
        Fish->ApplyLineForce(FVector::ZeroVector);
        Fish->Simulate(0.025f);
        TestTrue(S.Id + TEXT(": on a slack line it makes off to water of its choosing"), Fish->FightPhase != EPhase::Pause
            && LakeMap::IsWater(Fish->RoamTarget.X, Fish->RoamTarget.Y) && FVector::Dist2D(Fish->RoamTarget, Open) > 100);
        // Neither a current burst nor the common movement tail can restore a tired fish to full speed.
        Fish->FightPhase = EPhase::Burst;
        Fish->FightRemaining = 2;
        Fish->Stamina = Fish->MaxStamina;
        FVector Desired; float FreshSpeed = 0, TiredSpeed = 0;
        Fish->UpdateFightMotion(0.01f, Desired, FreshSpeed);
        Fish->Stamina = Fish->MaxStamina * 0.1f;
        Fish->UpdateFightMotion(0.01f, Desired, TiredSpeed);
        TestTrue(S.Id + TEXT(": even bursts weaken with fatigue"), TiredSpeed < FreshSpeed * 0.3f);
        Fish->FightPhase = EPhase::Pause;
        Fish->FightRemaining = 10;
        Fish->Stamina = Fish->MaxStamina;
        Fish->Vel = FVector::ZeroVector;
        Fish->MoveHooked(FVector::ZeroVector, 0, 0.05f);
        TestTrue(S.Id + TEXT(": recovery never overfills reserve"), Fish->Stamina <= Fish->MaxStamina);
        TestTrue(S.Id + TEXT(": stamina never negative"), Fish->Stamina >= 0);
        AddInfo(FString::Printf(TEXT("%s: bursts=%d gaps=%d rest=%.1fs pull=%.1fs dives=%d surface=%d /1000"), *S.Id, T.Bursts, T.Gaps, T.Rest, T.Pull, T.Dives, T.Surfaces));
        Fish->Destroy();
    }
    TestTrue(TEXT("Perch produce many short series through Hooked"), Stats[TEXT("perch")].Gaps > 12 && Stats[TEXT("perch")].Bursts > Stats[TEXT("crucian")].Bursts * 2);
    TestTrue(TEXT("Crucian have longer rests than roach"), Stats[TEXT("crucian")].Rest > Stats[TEXT("roach")].Rest);
    TestTrue(TEXT("Bream favour heavy sustained pulls and depth"), Stats[TEXT("bream")].Pull > Stats[TEXT("perch")].Pull && Stats[TEXT("bream")].Dives > 400);
    TestTrue(TEXT("Rudd keep higher than roach and bream"), Stats[TEXT("rudd")].Surfaces > Stats[TEXT("roach")].Surfaces * 2 && Stats[TEXT("rudd")].Dives < 100);
    TestTrue(TEXT("Bleak work with the default fight profile"), Stats.Contains(TEXT("bleak")) && Stats[TEXT("bleak")].Bursts > 0);

    ALakeFish* Pike = SpawnFish(World, EFishSpecies::Pike, 1.4f, Open);
    Pike->Hook(Fisher);
    Pike->Random.Initialize(77);
    Pike->FightIndividual = 1;
    // Find a permitted probabilistic leap, then follow the real ballistic movement.
    for (int32 I = 0; I < 100 && !Pike->bFightJump; ++I)
    {
        Pike->SetActorLocation(FVector(Open.X, Open.Y, SurfaceZ - 8));
        Pike->Stamina = Pike->MaxStamina;
        Pike->StartFightBurst();
    }
    TestTrue(TEXT("A fresh pike can launch a hooked leap"), Pike->bFightJump);
    TestFalse(TEXT("A leaping pike cannot be landed"), Pike->ReadyToLand());
    const float LaunchStamina = Pike->Stamina;
    float Peak = -1000;
    for (int32 Step = 0; Step < 100; ++Step)
    {
        Pike->MoveHooked(FVector::ZeroVector, 0, 0.01f);
        Peak = FMath::Max(Peak, float(Pike->GetActorLocation().Z - SurfaceZ));
    }
    TestTrue(TEXT("The fish itself clears the water and returns"), Peak > 25 && !Pike->bFightJump && Pike->GetActorLocation().Z <= SurfaceZ);
    TestTrue(TEXT("The leap does not regenerate stamina in flight"), Pike->Stamina <= LaunchStamina);
    Pike->SetActorLocation(FVector(Open.X, Open.Y, SurfaceZ - 8));
    for (int32 I = 0; I < 100; ++I) Pike->StartFightBurst();
    TestFalse(TEXT("The jump cooldown prevents repeated leaps"), Pike->bFightJump);
    // Applied line force is still integrated in the airborne branch.
    Pike->bFightJump = true;
    Pike->Vel = FVector(0, 0, 200);
    Pike->ApplyLineForce(FVector(140, 0, 0));
    Pike->MoveHooked(FVector::ZeroVector, 0, 0.01f);
    TestTrue(TEXT("The line remains attached and affects an airborne fish"), Pike->Vel.X > 0.5);
    Pike->bFightJump = false;
    Pike->NextFightJump = 0;
    Pike->Stamina = Pike->MaxStamina * 0.1f;
    for (int32 I = 0; I < 100; ++I) Pike->StartFightBurst();
    TestFalse(TEXT("An exhausted pike cannot leap"), Pike->bFightJump);

    FVector2D Facing;
    const FVector2D Edge = LakeMap::StartSpot(&Facing);
    Fisher->SetActorLocation(FVector(Edge, 100));
    Fisher->SetActorRotation(FVector(Facing, 0).Rotation());
    const FVector NearBank(Edge + Facing * 180, SurfaceZ - 15);
    bool bFinalRun = false;
    for (int32 Seed = 1; Seed <= 40 && !bFinalRun; ++Seed)
    {
        Pike->SetActorLocation(NearBank);
        Pike->Hook(Fisher);
        Pike->Random.Initialize(Seed);
        Pike->StateTime = 4;
        Pike->Stamina = Pike->MaxStamina * 0.3f;
        Fisher->FishingState = EFishingState::Fight;
        Fisher->HookedFish = Pike;
        Fisher->bReeling = true;
        Fisher->FightTime = 4;
        Fisher->UpdateFight(0.01f);
        bFinalRun = Pike->bShoreRun;
    }
    TestTrue(TEXT("A final run gets a chance before automatic landing"), bFinalRun && Fisher->FishingState == EFishingState::Fight);
    TestTrue(TEXT("The final run heads away from the angler"), FVector::DotProduct(Pike->RoamTarget - NearBank, FVector(Facing, 0)) > 0);
    Pike->bShoreRun = false;
    for (int32 I = 0; I < 100; ++I) Pike->ReadyToLand();
    TestFalse(TEXT("The final run cannot be rolled over and over"), Pike->bShoreRun);
    Pike->Hook(Fisher);
    TestTrue(TEXT("Rehook clears old manoeuvre state"), !Pike->bShoreTried && !Pike->bFightJump && Pike->FightRemaining == 0);
    // Size and individuality influence the production burst timing.
    ALakeFish* Perch = SpawnFish(World, EFishSpecies::Perch, 0.3f, Open);
    Perch->Hook(Fisher);
    Perch->Random.Initialize(9); Perch->Size01 = 0; Perch->FightIndividual = 1;
    Perch->StartFightBurst(); const float SmallBurst = Perch->FightRemaining;
    Perch->Random.Initialize(9); Perch->Size01 = 1;
    Perch->StartFightBurst(); const float BigBurst = Perch->FightRemaining;
    Perch->Random.Initialize(9); Perch->FightIndividual = 1.15f;
    Perch->StartFightBurst();
    TestTrue(TEXT("Size and individual pace alter bouts"), BigBurst > SmallBurst * 1.5f && Perch->FightRemaining < BigBurst);
    TestFalse(TEXT("Perch have no hooked leap"), Perch->bFightJump);

    EFishSpecies Bream = EFishSpecies::Crucian;
    for (int32 I = 0; I < NumSpecies(); ++I) if (Info((EFishSpecies)I).Id == TEXT("bream")) Bream = (EFishSpecies)I;
    FRandomStream Bites(44);
    int32 BottomLifts = 0, FloatingLifts = 0;
    for (int32 I = 0; I < 300; ++I)
    {
        for (const FBiteStep& Step : MakeBite(Bream, 0.5f, 1, true, false, Bites)) BottomLifts += Step.Motion == EBiteMotion::Lift;
        for (const FBiteStep& Step : MakeBite(Bream, 0.5f, 1, false, false, Bites)) FloatingLifts += Step.Motion == EBiteMotion::Lift;
    }
    TestTrue(TEXT("Bream lift when feeding from the bottom"), BottomLifts > 200);
    TestEqual(TEXT("Bream never lift a suspended bait as if it lay on bottom"), FloatingLifts, 0);
    // Bad profile data is diagnosed by the actual JSON reader, including nested typos.
    FString File;
    FFileHelper::LoadFileToString(File, *(FPaths::ProjectContentDir() / TEXT("LiveFish/Species/bleak.json")));
    File.InsertAt(File.Find(TEXT("\"stock\"")), TEXT("\"fight\":{\"pullMin\":-1,\"seriesMax\":0,\"jumpChance\":2,\"recovry\":1},\n"));
    FFishSpeciesInfo Invalid; TArray<FString> Problems;
    ReadSpecies(File, TEXT("invalid-fight.json"), Invalid, Problems);
    TestTrue(TEXT("Invalid fight values and misspelled fields are reported"), Problems.Num() >= 4);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeContactPhysicsTest, "QuietLake.Fishing.ContactPhysics", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeContactPhysicsTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    const FVector Open(LakeMap::Centre, LakeFish::SurfaceZ - 100);
    ALakeCharacter* Angler = World->SpawnActor<ALakeCharacter>(Open - FVector(300, 0, 0), FRotator::ZeroRotator);
    for (EFishSpecies Sp : { EFishSpecies::Crucian, EFishSpecies::Perch, EFishSpecies::Pike })
    {
        const auto& S = LakeFish::Info(Sp);
        for (float Weight : { S.MinWeight, S.MaxWeight })
        {
            ALakeFish* F = SpawnFish(World, Sp, Weight, Open);
            F->SetActorRotation(FRotator(25, 63, 0));
            const FVector Expected = F->Body->GetComponentTransform().TransformPosition(S.MouthPoint * S.MeshLength);
            TestTrue(TEXT("Physical mouth equals the scaled model attachment"), FVector::Dist(Expected, F->MouthLocation()) < 0.01f);
            TestTrue(TEXT("Contact is small and scales with the mouth"), F->ContactRadius() >= 0.4f && F->ContactRadius() <= 2);
            F->State = ALakeFish::EState::Nibble; F->TargetAngler = Angler; F->BiteStepEnd = 10000;
            Angler->FishingState = EFishingState::Waiting;
            Angler->BaitPoint = F->MouthLocation() + FVector(5,2,-1);
            for (int32 Dock = 0; Dock < 100; ++Dock) F->Simulate(0.02f);
            TestTrue(TEXT("Fins bring the actual mouth into bait contact without snapping"), FVector::Dist(F->MouthLocation(), Angler->BaitPoint) <= F->ContactRadius());
            F->Hook(Angler);
            const FVector Start = F->GetActorLocation();
            for (int32 I = 0; I < 100; ++I) { F->ApplyLineForce(FVector::ZeroVector); F->Simulate(0.05f); }
            TestFalse(TEXT("Slack never wakes full fighting by elapsed time"), F->IsFightAwake());
            TestTrue(TEXT("Slack fish remains alive and swims"), FVector::Dist(Start, F->GetActorLocation()) > 1);
            F->ApplyLineForce(FVector(100, 0, 0));
            TestTrue(TEXT("A real load awakens resistance"), F->IsFightAwake());
            F->Stamina = F->MaxStamina * 0.5f;
            F->FightPhase = ALakeFish::EFightPhase::Pull;
            F->Vel = FVector::ZeroVector;
            const float Before = F->Stamina;
            F->MoveHooked(FVector(100, 0, 0), 100, 0.05f);
            TestTrue(TEXT("Muscular work consumes stamina"), F->Stamina < Before);
            F->Vel = FVector::ZeroVector; F->PendingLineForce = FVector::ZeroVector;
            F->FightPhase = ALakeFish::EFightPhase::Pause;
            const float Rest = F->Stamina;
            F->MoveHooked(FVector::ZeroVector, 0, 0.05f);
            TestTrue(TEXT("A real pause recovers stamina"), F->Stamina > Rest);
            F->SetActorLocation(FVector(Open.X, Open.Y, LakeFish::SurfaceZ - 1));
            F->Vel = FVector(0, 0, 120);
            F->MoveHooked(FVector::ZeroVector, 0, 0.02f);
            TestTrue(TEXT("Surface has no hard position ceiling"), F->GetActorLocation().Z > LakeFish::SurfaceZ - 1);
            F->LiftOut();
            for (int32 I = 0; I < 40; ++I)
            {
                const FVector Hook = Open + FVector(0, I * 0.1f, 120);
                F->HangOnLine(Hook, FVector::UpVector, 0.02f);
                TestTrue(TEXT("Hanging mouth stays on hook while body swings"), FVector::Dist(F->MouthLocation(), Hook) < 0.01f);
                TestFalse(TEXT("Hanging body remains finite"), F->GetActorLocation().ContainsNaN());
            }
        }
    }
    TestTrue(TEXT("Fully submerged body is supported"), ALakeRod::FishSupport(LakeFish::SurfaceZ - 1, 30) == 1);
    TestTrue(TEXT("Head first emergence transfers load gradually"), ALakeRod::FishSupport(LakeFish::SurfaceZ + 8, 30) > 0 && ALakeRod::FishSupport(LakeFish::SurfaceZ + 8, 30) < 1);
    TestTrue(TEXT("Body clear of water loads full weight"), ALakeRod::FishSupport(LakeFish::SurfaceZ + 30, 30) == 0);
    auto Load = [](float Distance, float Speed = 0) { return ALakeRod::TetherForce(FVector::ZeroVector, FVector(Distance, 0, 0), FVector(Speed, 0, 0), 600, 0.4f); };
    TestTrue(TEXT("Slack gives zero force, even with moving endpoints"), Load(590, 100).IsNearlyZero());
    TestTrue(TEXT("Fish moving out takes up slack"), Load(620).X < 0);
    const FVector PlayerPull = ALakeRod::TetherForce(FVector(-20,0,0), FVector(600,0,0), FVector::ZeroVector, 600, 0.4f);
    TestTrue(TEXT("Player moving away produces the same stretch force"), (PlayerPull - Load(620)).IsNearlyZero());
    TestTrue(TEXT("Damping opposes outward motion"), Load(620, 100).Size() > Load(620).Size());
    // The tackle: a softer blank loads the line less for the same pull-out, the arms behind it give
    // too, and with matched tackle the line is the weakest link of every rod.
    for (ERodKind Kind : { ERodKind::Light, ERodKind::Medium, ERodKind::Heavy })
    {
        const FLakeTackle Gear = ALakeCharacter::TackleOf(Kind);
        TestTrue(TEXT("A rod breaks from its bend: it stands a pull of its own"), Gear.RodBreak > 1000);
        TestTrue(TEXT("The hook's hold wears below the line's strength"), Gear.HookSafe < Gear.LineBreak);
        TestTrue(TEXT("The arms give behind the blank"), ALakeCharacter::HandSeries(Gear) > 0.1f && ALakeCharacter::HandSeries(Gear) < 0.6f);
    }
    const FLakeTackle Soft = ALakeCharacter::TackleOf(ERodKind::Light), Stiff = ALakeCharacter::TackleOf(ERodKind::Heavy);
    TestTrue(TEXT("Held across the line the light pole breaks before the line; the heavy one outlasts it"), Soft.RodBreak < Soft.LineBreak && Stiff.RodBreak > Stiff.LineBreak);
    auto Spring = [](const FLakeTackle& Gear) { return ALakeRod::TetherForce(FVector::ZeroVector, FVector(700, 0, 0), FVector::ZeroVector, 600, 0.4f, Gear.RodSpring).Size() * ALakeCharacter::HandSeries(Gear); };
    TestTrue(TEXT("A softer rod loads the line less for the same run"), Spring(Soft) < Spring(Stiff));
    ALakeFish* F = SpawnFish(World, EFishSpecies::Perch, 0.4f, Open);
    auto Holding = [&](float Reach)
    {
        F->State = ALakeFish::EState::Holding; F->TargetAngler = Angler; F->BiteNow.Hook = 1;
        Angler->FishingState = EFishingState::Bite; Angler->BaitFish = F; Angler->BiteMotion = EBiteMotion::Hold;
        F->SetActorRotation(FRotator::ZeroRotator);
        F->SetActorLocation(Angler->RodTip() + FVector(Reach,0,0) - F->MouthOffset());
        Angler->BaitPoint = F->MouthLocation();
    };
    Holding(400); Angler->Strike(); Angler->ResolveHook(0.1f);
    TestFalse(TEXT("Strike on slack cannot magically set hook"), F->IsHooked());
    Holding(620); Angler->BaitPoint += FVector(0,30,0); Angler->Strike(); Angler->ResolveHook(0.1f);
    TestFalse(TEXT("Remote contact cannot set hook under load"), F->IsHooked());
    Holding(620); Angler->Strike(); Angler->ResolveHook(0.1f);
    TestTrue(TEXT("Contact and a transmitted strike set hook"), F->IsHooked());
    Holding(620); Angler->StrikeUntil = -1; Angler->HookImpulse = 0; Angler->ResolveHook(0.1f);
    TestTrue(TEXT("Fish can hook itself by loading the held bait"), F->IsHooked());
    F->HookGrip = 0.001f; F->ApplyLineForce(FVector(-10000,0,0));
    F->MoveHooked(FVector(100,0,0),100,0.1f);
    TestFalse(TEXT("An unreliable hook can give under severe opposing load"), F->IsHooked());

    // Use the physical rod: its continuous aim used to overwrite the bite lift and strike.
    FVector2D ShoreFacing;
    const FVector2D Shore = LakeMap::StartSpot(&ShoreFacing) - ShoreFacing * 200;
    Angler->SetActorLocation(FVector(Shore, ALakeEnvironment::GroundHeight(Shore.X, Shore.Y) + 100));
    APlayerController* InputController = World->SpawnActor<APlayerController>();
    InputController->Possess(Angler);
    Angler->RodVisual = World->SpawnActor<ALakeRod>();
    FLakeFishingFixture::ImmersedRig(Angler);
    Angler->ServerFish_Implementation();
    Angler->UpdateRetrieve(0.5f);
    TestTrue(TEXT("LMB without a bite starts and advances retrieval"), Angler->bRetrieving && Angler->RetrieveLift > 0);
    Angler->ServerRelease_Implementation();
    const float HeldLift = Angler->RetrieveLift;
    Angler->UpdateRetrieve(0.25f);
    TestTrue(TEXT("Release lowers the rod without a bite"), HeldLift > 0.0f && Angler->RetrieveLift < HeldLift);

    FLakeFishingFixture::ImmersedRig(Angler);
    Holding(400);
    F->State = ALakeFish::EState::Nibble; // isolate pose assertions; real loaded takes are tested above and in LiveRigPreview
    Angler->RodLift = 0; Angler->Tension = 0; Angler->StrikeServerTime = -100;
    Angler->StrikeUntil = -1; Angler->HookImpulse = 0;
    F->BiteStepEnd = 10000;
    auto TargetElevation = [&](float Now)
    {
        float Elevation, Stiffness, Yaw; FVector Grip;
        Angler->RodPoseTarget(0, Now, Elevation, Grip, Stiffness, Yaw);
        return Elevation;
    };
    const float BeforeLift = TargetElevation(Angler->ServerNow());
    // The same take on the rod left on its stand: LMB beside it takes the rod and lifts.
    Angler->FishingState = EFishingState::Waiting; Angler->BaitFish = nullptr;
    const int32 StandRod = (int32)Angler->RodKind;
    Angler->ParkHandRod(ERodPlace::Stand, Angler->GetActorLocation(), Angler->GetActorRotation().Yaw);
    F->TargetRig = StandRod;
    Angler->OnFishTake(F, 60);
    Angler->ServerFish_Implementation();
    TestTrue(TEXT("LMB takes a nearby rod from the stand during a bite"), Angler->HasRodInHands() && Angler->FishingState == EFishingState::Bite && Angler->bReeling);
    TestFalse(TEXT("LMB itself does not hook the fish"), F->IsHooked());
    Angler->Tick(0.5f);
    TestTrue(TEXT("Held LMB raises the physical rod target during a bite"),
        Angler->FishingState == EFishingState::Bite && TargetElevation(Angler->ServerNow()) > BeforeLift + 2);
    const float Raised = Angler->RodLift;
    Angler->ServerRelease_Implementation();
    Angler->Tick(0.1f);
    TestTrue(TEXT("Release yields the rod during a bite"), !Angler->bReeling && Angler->RodLift < Raised);
    Angler->ServerFish_Implementation();
    const float BeforeYield = Angler->RodLift;
    Angler->MoveRodAxis(-3, true);
    Angler->Tick(0.05f);
    TestTrue(TEXT("Mouse down yields while LMB stays held"), Angler->bReeling && Angler->RodLift < BeforeYield);
    Angler->YieldUntil = -1;
    Angler->Tension = 0;
    const float BeforeStrike = TargetElevation(Angler->ServerNow());
    Angler->ServerCancel_Implementation();
    TestTrue(TEXT("RMB gives the physical rod a strike stroke during a bite"),
        TargetElevation(Angler->StrikeServerTime + 0.15f) > BeforeStrike + 14);
    TestFalse(TEXT("RMB still needs transmitted force to hook"), F->IsHooked());

    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeSpeciesFilesTest, "QuietLake.Fishing.SpeciesFiles", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeSpeciesFilesTest::RunTest(const FString& Parameters)
{
    for (const FString& Problem : LakeFish::SpeciesProblems()) AddError(TEXT("Species file ") + Problem);
    TestTrue(TEXT("At least the four species"), LakeFish::NumSpecies() >= 4);
    TestTrue(TEXT("Perch, roach, crucian and pike in their places"), LakeFish::Info(EFishSpecies::Perch).Id == TEXT("perch") && LakeFish::Info(EFishSpecies::Roach).Id == TEXT("roach")
        && LakeFish::Info(EFishSpecies::Crucian).Id == TEXT("crucian") && LakeFish::Info(EFishSpecies::Pike).Id == TEXT("pike"));
    for (int32 Sp = 0; Sp < LakeFish::NumSpecies(); ++Sp)
    {
        const FFishSpeciesInfo& S = LakeFish::Info((EFishSpecies)Sp);
        TestNotNull(FString::Printf(TEXT("%s: its model loads"), *S.Id), LoadObject<UStaticMesh>(nullptr, *S.Mesh));
        bool bBaits = S.Baits.Num() > 0;
        for (const TPair<FString, float>& Bait : S.Baits) bBaits &= Bait.Value >= 0 && Bait.Value <= 1;
        TestTrue(FString::Printf(TEXT("%s: likes for its baits, 0..1"), *S.Id), bBaits);
        TestTrue(FString::Printf(TEXT("%s: a whole bite"), *S.Id), !S.Bite.Take.IsEmpty() && S.Bite.Take.ContainsByPredicate([](const FFishBiteMove& M) { return M.bTake; }));
        TestTrue(FString::Printf(TEXT("%s: a stock count (0: not in the pond yet) and a school size"), *S.Id), S.Stock.Count >= 0 && S.Stock.SchoolSize >= 1);
    }
    // A bream, as one would add it: the example in the species README, read as its file alone (so the
    // example always works).
    FString Readme;
    FFileHelper::LoadFileToString(Readme, *(FPaths::ProjectContentDir() / TEXT("LiveFish/Species/README.md")));
    const int32 From = Readme.Find(TEXT("```json"));
    const int32 To = From >= 0 ? Readme.Find(TEXT("```"), ESearchCase::CaseSensitive, ESearchDir::FromStart, From + 7) : -1;
    TestTrue(TEXT("The species README has its example"), From >= 0 && To > From);
    FFishSpeciesInfo Bream;
    TArray<FString> Problems;
    const bool bRead = From >= 0 && To > From && LakeFish::ReadSpecies(Readme.Mid(From + 7, To - From - 7), TEXT("README example (bream.json)"), Bream, Problems);
    for (const FString& Problem : Problems) AddInfo(Problem);
    TestTrue(TEXT("A new species is read from its file"), bRead && Problems.IsEmpty() && Bream.Name == TEXT("Лещ") && Bream.Order == 4 && Bream.Stock.SchoolSize == 10
        && Bream.Bite.Take.Num() == 2 && Bream.Bite.Take[0].Motion == EBiteMotion::Drift && Bream.Bite.TakeFromBottom.Num() == 2
        && Bream.Bite.TakeFromBottom[0].Motion == EBiteMotion::Lift && FMath::IsNearlyEqual(Bream.BaitLike(EBait::Maggot), 0.9f) && Bream.Feeding.bRootsInSilt
        && FMath::IsNearlyEqual(Bream.Feeding.Wariness, 0.7f) && FMath::IsNearlyEqual(LakeFish::Info(EFishSpecies::Perch).Diet.Benthos, 0.7f)
        && FMath::IsNearlyEqual(Bream.Diet.Plankton, 0.35f) && FMath::IsNearlyEqual(Bream.DietBig.Benthos, 1.0f));
    // A species without a diet is reported (it would feed the old way, by its habitat table only).
    Problems.Reset();
    FFishSpeciesInfo NoDiet;
    LakeFish::ReadSpecies(TEXT(R"JSON({"order":4,"name":"Лещ","noun":"лещ","mesh":"/Game/Fish/SM_Fish_Crucian.SM_Fish_Crucian","bite":{"take":[{"motion":"Lift"}]}})JSON"), TEXT("nodiet.json"), NoDiet, Problems);
    TestTrue(TEXT("A species without a diet is reported"), Problems.ContainsByPredicate([](const FString& P) { return P.Contains(TEXT("diet")); }));
    Problems.Reset();
    FFishSpeciesInfo Typo;
    LakeFish::ReadSpecies(TEXT(R"JSON({"order":4,"name":"Лещ","noun":"лещ","mesh":"/Game/Fish/SM_Fish_Crucian.SM_Fish_Crucian","senseRadus":500,"bite":{"take":[{"motion":"Lift","hok":0.8}]}})JSON"), TEXT("typo.json"), Typo, Problems);
    TestTrue(TEXT("Misspelt fields are reported, also inside a bite move"), Problems.ContainsByPredicate([](const FString& P) { return P.Contains(TEXT("senseRadus")); })
        && Problems.ContainsByPredicate([](const FString& P) { return P.Contains(TEXT("bite.take[0]")) && P.Contains(TEXT("hok")); }));
    return true;
}

// Live Fish between the species: prey keeps away from a predator big enough to eat it and notices a
// pike lying still only close by, a fish trying the bait backs off when a pike comes up, a pike eats
// small fish of any kind (a small perch too) but leaves a big one, a big perch dashes at a small
// roach, and a bigger, bolder fish pushes a small one off the bait (not the other way round).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeInteractionsTest, "QuietLake.Fishing.Interactions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeInteractionsTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ALakeLife* Life = World->SpawnActor<ALakeLife>();
    constexpr float Dt = 0.05f;
    // Open water in front of the camp, about 1.7 m deep.
    FVector2D Facing;
    const FVector2D Start = LakeMap::StartSpot(&Facing);
    const FVector2D Right(-Facing.Y, Facing.X);
    const FVector2D Open = Start + Facing * 1400;
    auto At = [](FVector2D P) { return FVector(P, FMath::Lerp(LakeFish::SurfaceZ - 10, ALakeFish::BottomZ(P.X, P.Y) + 10, 0.5f)); };
    auto Spawn = [&](EFishSpecies Species, float Weight, FVector2D P) { ALakeFish* Fish = SpawnFish(World, Species, Weight, At(P)); Fish->Hunger = 1; return Fish; };
    auto Step = [&](const TArray<ALakeFish*>& Fish) { Life->Step(Dt); for (ALakeFish* F : Fish) if (IsValid(F)) F->Simulate(Dt); };
    auto Gone = [](TArray<ALakeFish*>& Fish) { for (ALakeFish* F : Fish) if (IsValid(F)) F->Destroy(); Fish.Reset(); };

    // 1. A pike swimming by 2.5 m off: the roach takes fright and draws away from it.
    {
        ALakeFish* Pike = Spawn(EFishSpecies::Pike, 2.0f, Open);
        ALakeFish* Roach = Spawn(EFishSpecies::Roach, 0.15f, Open + Right * 250);
        TArray<ALakeFish*> Fish = { Pike, Roach };
        // Seeded: how far the roach turns away depends on its own random decisions, and the test
        // must not pass or fail on a lucky draw.
        Pike->Random.Initialize(3110);
        Roach->Random.Initialize(3111);
        const FVector RoachStart = Roach->GetActorLocation();
        float Before = FVector::Dist(Pike->GetActorLocation(), Roach->GetActorLocation()), Fear = 0, Fastest = 0;
        for (int32 I = 0; I < 80; ++I)
        {
            Pike->Vel = FVector(Facing, 0) * 25;   // it keeps swimming
            Step({ Roach });
            Fear = FMath::Max(Fear, Roach->FearNow());
            Fastest = FMath::Max(Fastest, float(Roach->Vel.Size()));
        }
        const float After = FVector::Dist(Pike->GetActorLocation(), Roach->GetActorLocation());
        AddInfo(FString::Printf(TEXT("Roach by a swimming pike: fear %.2f, %.0f -> %.0f cm, moved %.0f cm at up to %.0f cm/s"), Fear, Before, After, FVector::Dist(RoachStart, Roach->GetActorLocation()), Fastest));
        TestTrue(TEXT("A roach fears a swimming pike and draws away"), Fear > 0.15f && After > Before + 15);
        Gone(Fish);
    }
    // 2. A pike lying still is noticed only close by (camouflage).
    {
        ALakeFish* Pike = Spawn(EFishSpecies::Pike, 2.0f, Open);
        ALakeFish* Far = Spawn(EFishSpecies::Roach, 0.15f, Open + Right * 250);
        ALakeFish* Close = Spawn(EFishSpecies::Roach, 0.15f, Open - Right * 90);
        TArray<ALakeFish*> Fish = { Pike, Far, Close };
        Pike->Vel = FVector::ZeroVector;
        Life->Step(0);
        for (ALakeFish* F : { Far, Close }) { F->NextThreatLook = 0; F->SenseThreats(Dt); }
        AddInfo(FString::Printf(TEXT("Still pike: roach at 2.5 m fear %.2f, at 0.9 m %.2f"), Far->FearNow(), Close->FearNow()));
        TestTrue(TEXT("A pike lying still is noticed only close by"), Far->FearNow() == 0 && Close->FearNow() > 0.1f);
        Gone(Fish);
    }
    // 3. A roach trying the bait backs off when a pike swims up.
    {
        ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(FVector(Start + Facing * 90, 100), FRotator(0, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)), 0));
        APlayerController* Controller = World->SpawnActor<APlayerController>();
        Controller->Possess(Fisher);
        Controller->SetControlRotation(Fisher->GetActorRotation());
        Fisher->Bait = EBait::Maggot;
        Fisher->FloatDepth = 80;
        FLakeFishingFixture::ImmersedRig(Fisher);
        ALakeFish* Roach = SpawnFish(World, EFishSpecies::Roach, 0.15f, Fisher->BaitLocation() + FVector(100, 0, 0));
        Roach->Hunger = 1;
        TArray<ALakeFish*> Fish = { Roach };
        for (int32 I = 0; I < 600 && !Roach->IsNibbling(); ++I) Step(Fish);
        const bool bTrying = Roach->IsNibbling();
        ALakeFish* Pike = SpawnFish(World, EFishSpecies::Pike, 2.5f, Roach->GetActorLocation() + FVector(0, 80, 0));
        Pike->SetGoal(EFishGoal::Feed, 1000);   // a hunting pike
        Fish.Add(Pike);
        for (int32 I = 0; I < 40 && Roach->IsNibbling(); ++I) { Pike->Vel = FVector(0, -30, 0); Step({ Roach }); }
        TestTrue(TEXT("A roach trying the bait backs off when a pike swims up"), bTrying && !Roach->IsNibbling());

        // 6. At the bait: a big perch pushes a small roach off it, a small roach cannot push a perch.
        Pike->Destroy();
        Roach->Destroy();
        FLakeFishingFixture::ImmersedRig(Fisher);
        Roach = SpawnFish(World, EFishSpecies::Roach, 0.15f, Fisher->BaitLocation() + FVector(100, 0, 0));
        Roach->Hunger = 1;
        Fish = { Roach };
        for (int32 I = 0; I < 1200 && !Roach->IsNibbling(); ++I) Step(Fish);
        const bool bRoachOn = Roach->IsNibbling();
        ALakeFish* Big = SpawnFish(World, EFishSpecies::Perch, 0.6f, Fisher->BaitLocation() + FVector(30, 0, 0));
        const bool bPushed = Fisher->ClaimBait(Big) && !Roach->IsNibbling();
        AddInfo(FString::Printf(TEXT("At the bait: roach %.2f kg clout %.2f, perch %.2f kg clout %.2f"), Roach->WeightKg, Roach->Clout(), Big->WeightKg, Big->Clout()));
        TestTrue(TEXT("A big perch pushes a small roach off the bait"), bRoachOn && bPushed);
        FLakeFishingFixture::ImmersedRig(Fisher);
        ALakeFish* Perch = SpawnFish(World, EFishSpecies::Perch, 0.3f, Fisher->BaitLocation() + FVector(150, 0, 0));
        Perch->Hunger = 1;
        Big->Destroy();
        Roach->Destroy();
        Fish = { Perch };
        for (int32 I = 0; I < 1200 && !Perch->IsNibbling(); ++I) Step(Fish);
        ALakeFish* Small = SpawnFish(World, EFishSpecies::Roach, 0.06f, Fisher->BaitLocation() + FVector(30, 0, 0));
        TestTrue(TEXT("A small roach cannot push a perch off the bait"), Perch->IsNibbling() && !Fisher->ClaimBait(Small) && Perch->IsNibbling());
        Small->Destroy();
        Gone(Fish);
    }
    // 4. A pike eats small fish of any kind - a small perch too - but leaves a big perch alone.
    {
        ALakeFish* Pike = Spawn(EFishSpecies::Pike, 2.5f, Open);
        Pike->SetGoal(EFishGoal::Feed, 1000);
        const int32 EatenBefore = Life->Stats().Eaten;
        int32 Tries = 0;
        bool bBigStruck = false;
        ALakeFish* Big = Spawn(EFishSpecies::Perch, 0.78f, Open + Right * 120);
        for (; Tries < 40 && Life->Stats().Eaten == EatenBefore; ++Tries)
        {
            // It lies where it is, facing ahead.
            Pike->bAtSpot = true;
            Pike->LieUntil = Pike->SimTime + 100;
            Pike->RoamTarget = Pike->GetActorLocation();
            Pike->LieFacing = Pike->GetActorForwardVector();
            const FVector Front = Pike->MouthLocation() + Pike->GetActorForwardVector() * 80;
            ALakeFish* Small = SpawnFish(World, EFishSpecies::Perch, 0.1f, FVector(Front.X, Front.Y, FMath::Max(Front.Z, ALakeFish::BottomZ(Front.X, Front.Y) + 10)));
            for (int32 I = 0; I < 60 && IsValid(Small); ++I)
            {
                Step({ Pike, Small, Big });
                bBigStruck |= Pike->Prey.Get() == Big;
            }
            if (IsValid(Small)) Small->Destroy();
        }
        AddInfo(FString::Printf(TEXT("Pike 2.5 kg: ate a 0.1 kg perch after %d tries; struck at the 0.78 kg perch: %d"), Tries, bBigStruck));
        TestTrue(TEXT("A pike eats a small perch"), Life->Stats().Eaten > EatenBefore);
        TestFalse(TEXT("A pike leaves a perch too big for it alone"), bBigStruck);
        Big->Destroy();
        Pike->Destroy();
    }
    // 5. A big perch dashes at a small roach right in front of it.
    {
        ALakeFish* Perch = Spawn(EFishSpecies::Perch, 0.8f, Open);
        Perch->SetGoal(EFishGoal::Feed, 1000);
        const int32 StrikesBefore = Life->Stats().PikeStrikes;
        int32 Tries = 0;
        for (; Tries < 40 && Life->Stats().PikeStrikes == StrikesBefore; ++Tries)
        {
            const FVector Front = Perch->MouthLocation() + Perch->GetActorForwardVector() * 70;
            ALakeFish* Small = SpawnFish(World, EFishSpecies::Roach, 0.06f, FVector(Front.X, Front.Y, FMath::Max(Front.Z, ALakeFish::BottomZ(Front.X, Front.Y) + 10)));
            for (int32 I = 0; I < 30 && IsValid(Small) && Life->Stats().PikeStrikes == StrikesBefore; ++I) Step({ Perch, Small });
            if (IsValid(Small)) Small->Destroy();
        }
        AddInfo(FString::Printf(TEXT("Big perch: dashed at a small roach after %d tries"), Tries));
        TestTrue(TEXT("A big perch dashes at a small roach"), Life->Stats().PikeStrikes > StrikesBefore);
        Perch->Destroy();
    }
    // 7. Fry dart off from a perch swimming at them before it strikes, away from it.
    {
        FVector2D Shallow = Start + Facing * 300;
        for (float Out = 200; Out < 1500 && !Life->FryCanBe(Shallow); Out += 50) Shallow = Start + Facing * Out;
        FFryShoal Shoal;
        Shoal.Location = FVector(Shallow, LakeFish::SurfaceZ - 9);
        Shoal.Target = Shoal.Location;
        Shoal.NextWander = 1e6f;
        Life->Fry.Reset();
        Life->Fry.Add(Shoal);
        ALakeFish* Perch = Spawn(EFishSpecies::Perch, 0.3f, Shallow + Right * 230);
        Perch->SetGoal(EFishGoal::Feed, 1000);   // a hunting perch
        const int32 FlightsBefore = Life->Stats().FryFlights;
        for (int32 I = 0; I < 40 && Life->Stats().FryFlights == FlightsBefore; ++I) { Perch->Vel = FVector(-Right, 0) * 40; Life->Step(Dt); }
        const FFryShoal& Fled = Life->Fry[0];
        const float From = FVector::Dist2D(Fled.Location, Perch->GetActorLocation()), To = FVector::Dist2D(Fled.Target, Perch->GetActorLocation());
        AddInfo(FString::Printf(TEXT("Fry by a hunting perch 2.3 m off: darted %d, heading from %.0f to %.0f cm off it"), Life->Stats().FryFlights - FlightsBefore, From, To));
        TestTrue(TEXT("Fry dart off from a hunting perch swimming at them, away from it"), Life->Stats().FryFlights > FlightsBefore && To > From + 150);
        Perch->Destroy();
    }
    World->DestroyWorld(false);
    return true;
}

// Live Fish food: the plankton sinks by day and rises at night, insects hatch on calm warm
// evenings, each kind of place holds its own food, an eaten patch grows back (plankton fast, silt
// fauna slowly), a crucian roots for silt fauna, eats its patch down and moves on, roach feed at
// the top at dusk and deep at noon, a big perch hunts fry while a small one eats plankton and
// bloodworm, and a bait in the layer a fish feeds in is found far more readily.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeFoodTest, "QuietLake.Fishing.Food", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeFoodTest::RunTest(const FString& Parameters)
{
    // 1-2. Time and weather.
    FFishConditions Noon;
    Noon.SunElevation = 55; Noon.Cloud = 0.1f; Noon.WaterTempC = 20; Noon.Wind = 0.1f; Noon.bMorning = false;
    FFishConditions Grey = Noon; Grey.Cloud = 0.95f;
    FFishConditions Dusk = Noon; Dusk.SunElevation = 2;
    FFishConditions Night = Noon; Night.SunElevation = -25;
    FFishConditions Windy = Dusk; Windy.Wind = 0.8f;
    AddInfo(FString::Printf(TEXT("Plankton at %.1f m at noon, %.1f m under a grey sky, %.1f m at dusk, %.1f m at night"),
        LakeFood::PlanktonDepthM(Noon), LakeFood::PlanktonDepthM(Grey), LakeFood::PlanktonDepthM(Dusk), LakeFood::PlanktonDepthM(Night)));
    TestTrue(TEXT("Plankton hangs deep in bright day, near the top at night, higher under a grey sky"),
        LakeFood::PlanktonDepthM(Noon) > 1.8f && LakeFood::PlanktonDepthM(Night) < 0.5f && LakeFood::PlanktonDepthM(Grey) < LakeFood::PlanktonDepthM(Noon) - 0.4f);
    const float InsNoon = LakeFood::Season(EFishFood::Insects, Noon), InsDusk = LakeFood::Season(EFishFood::Insects, Dusk);
    const float InsNight = LakeFood::Season(EFishFood::Insects, Night), InsWind = LakeFood::Season(EFishFood::Insects, Windy);
    AddInfo(FString::Printf(TEXT("Insects: noon %.2f, calm dusk %.2f, windy dusk %.2f, night %.2f"), InsNoon, InsDusk, InsWind, InsNight));
    TestTrue(TEXT("Insects hatch on a calm warm evening, few at noon, hardly any at night or in a strong wind"),
        InsDusk > 2.5f * InsNoon && InsNight < 0.1f && InsWind < 0.5f * InsDusk);

    // 3. Places.
    auto Find = [](TFunctionRef<bool(float, float)> Want, FVector2D& Out)
    {
        const FBox2D B = LakeMap::LakeBounds();
        for (float Y = B.Min.Y; Y < B.Max.Y; Y += 700)
            for (float X = B.Min.X; X < B.Max.X; X += 700)
                if (LakeMap::IsWater(X, Y) && Want(X, Y)) { Out = FVector2D(X, Y); return true; }
        return false;
    };
    FVector2D Lily, Open, Reed, Slope;
    const bool bLily = Find([](float X, float Y) { const float D = LakeMap::Depth(X, Y); return LakeMap::LilyAmount(X, Y) > 0.8f && D > 70 && D < 150 && LakeMap::ShoreDistance(X, Y) > 400; }, Lily);
    const bool bOpen = Find([](float X, float Y) { return LakeMap::Depth(X, Y) > 200 && LakeMap::ZonesAt(X, Y).Open > 0.9f; }, Open);
    const bool bReed = Find([](float X, float Y) { return LakeMap::Depth(X, Y) > 110 && LakeMap::LilyAmount(X, Y) < 0.1f && LakeMap::ZonesAt(X, Y).ReedEdge > 0.8f; }, Reed);
    const bool bSlope = Find([](float X, float Y) { return LakeMap::ZonesAt(X, Y).DropOff > 0.85f && LakeMap::LilyAmount(X, Y) < 0.1f; }, Slope);
    AddInfo(FString::Printf(TEXT("Places found: lily bed %d, open water %d, reed edge %d, drop-off %d"), bLily, bOpen, bReed, bSlope));
    const bool bPlaces = bLily && bOpen && bReed && bSlope;
    TestTrue(TEXT("The lake has lily beds, open water, a reed edge and a drop-off"), bPlaces);
    if (!bPlaces) return false;
    auto R = [](EFishFood F, const FVector2D& P) { return LakeFood::Richness(F, P.X, P.Y); };
    AddInfo(FString::Printf(TEXT("Silt fauna: lilies %.2f, drop-off %.2f; insects: reed edge %.2f, open %.2f; plankton: open %.2f, lilies %.2f; weed: lilies %.2f, open %.2f"),
        R(EFishFood::Benthos, Lily), R(EFishFood::Benthos, Slope), R(EFishFood::Insects, Reed), R(EFishFood::Insects, Open),
        R(EFishFood::Plankton, Open), R(EFishFood::Plankton, Lily), R(EFishFood::Plants, Lily), R(EFishFood::Plants, Open)));
    TestTrue(TEXT("Silt fauna is richest in the weedy shallows, poor on the drop-off"), R(EFishFood::Benthos, Lily) > R(EFishFood::Benthos, Slope) + 0.2f);
    TestTrue(TEXT("Insects are by the reeds more than over the open water"), R(EFishFood::Insects, Reed) > R(EFishFood::Insects, Open) + 0.3f);
    TestTrue(TEXT("Plankton is in the open water more than among the lilies"), R(EFishFood::Plankton, Open) > R(EFishFood::Plankton, Lily) + 0.2f);
    TestTrue(TEXT("Weed grows in the lily beds, not in the open water"), R(EFishFood::Plants, Lily) > R(EFishFood::Plants, Open) + 0.5f);

    // 4. Eaten down and growing back.
    {
        FLakeFoodField Field;
        Field.Init();
        Field.Eat(EFishFood::Benthos, Lily, 0.6f, 1.0f);
        Field.Eat(EFishFood::Plankton, Open, 0.6f, 1.0f);
        const float Silt0 = Field.Left(EFishFood::Benthos, Lily), Plank0 = Field.Left(EFishFood::Plankton, Open);
        for (int32 I = 0; I < 150; ++I) Field.Step(1);
        const float Silt1 = Field.Left(EFishFood::Benthos, Lily), Plank1 = Field.Left(EFishFood::Plankton, Open);
        AddInfo(FString::Printf(TEXT("Eaten down: silt fauna %.2f -> %.2f, plankton %.2f -> %.2f in 150 s"), Silt0, Silt1, Plank0, Plank1));
        TestTrue(TEXT("Feeding eats a patch down"), Silt0 < 0.75f && Plank0 < 0.75f);
        TestTrue(TEXT("Plankton grows back in minutes, silt fauna far slower"), Plank1 - Plank0 > 0.4f * (1 - Plank0) && Silt1 - Silt0 < 0.2f * (1 - Silt0));
    }

    // The pond with its weather: a calm clear day.
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ALakeLife* Life = World->SpawnActor<ALakeLife>();
    ALakeWeather* Weather = World->SpawnActor<ALakeWeather>();
    FLakeWeatherState Calm;
    Calm.Cloud = 0.1f; Calm.Fog = 0; Calm.Rain = 0; Calm.Wind = 0.06f;
    Weather->SetWeatherNow(Calm);
    Life->Step(0);   // lays out the food
    constexpr float Dt = 0.05f;
    auto SetSun = [&](float Lo, float Hi)
    {
        for (float H = 12; H < 24; H += 0.05f)
        {
            Weather->SetHours(H);
            const float Sun = Weather->FishConditions().SunElevation;
            if (Sun >= Lo && Sun <= Hi) return H;
        }
        return -1.0f;
    };
    auto Spawn = [&](EFishSpecies Species, float Weight, FVector2D P)
    {
        ALakeFish* Fish = SpawnFish(World, Species, Weight, FVector(P, FMath::Lerp(LakeFish::SurfaceZ - 10, ALakeFish::BottomZ(P.X, P.Y) + 10, 0.5f)));
        Fish->Hunger = 1;
        Fish->NextMoodCheck = 0;
        return Fish;
    };
    auto Choose = [](ALakeFish* Fish) { Fish->SetGoal(EFishGoal::Cruise, 1000); Fish->SetGoal(EFishGoal::Feed, 1000); };

    // 5. A crucian in the lilies roots for silt fauna, eats its patch down and moves on.
    {
        // The weather the earlier tests left behind must not decide this scenario: pin a calm clear
        // day, so the food the crucian finds is the same in a full-suite run as when run alone.
        FLakeWeatherState Pinned;
        Pinned.Cloud = 0.2f;
        Pinned.Fog = 0.25f;
        Pinned.Wind = 0.05f;
        Weather->SetWeatherNow(Pinned);
        constexpr int32 Seed = -646273053; // Decision stream from the failing full-suite run.
        TestTrue(TEXT("Noon found"), SetSun(45, 70) > 0);
        // Fresh food and fixed decisions: unrelated actor allocations must not pick this scenario.
        Life->Food() = FLakeFoodField();
        Life->Step(0);
        ALakeFish* Crucian = Spawn(EFishSpecies::Crucian, 0.4f, Lily);
        Crucian->Random.Initialize(Seed);
        Crucian->WanderSeed = 0;
        Crucian->Energy = 1;
        int32 OnSilt = 0;
        for (int32 I = 0; I < 20; ++I) { Choose(Crucian); OnSilt += Crucian->FeedOn == EFishFood::Benthos; }
        AddInfo(FString::Printf(TEXT("Crucian in the lilies: on silt fauna %d of 20"), OnSilt));
        TestTrue(TEXT("A crucian feeds mostly on the silt fauna"), OnSilt >= 12);
        for (int32 K = 0; K < 50 && Crucian->FeedOn != EFishFood::Benthos; ++K) Choose(Crucian);
        const FVector2D First(Crucian->RoamTarget);
        const float Bottom = ALakeFish::BottomZ(First.X, First.Y);
        TestTrue(TEXT("It feeds at the bottom"), Crucian->RoamTarget.Z < Bottom + 20);
        int32 Steps = 0;
        // Neighbouring 5 m cells can meet centimetres away. Verify arrival and feeding in richer
        // benthos, not an arbitrary distance to a target the fish may never reach.
        FVector2D Patch = First;
        bool bPatch = false;
        auto MovedOn = [&]
        {
            const FVector2D Here(Crucian->GetActorLocation());
            const FLakeFoodField& Food = Life->Food();
            const FFishConditions C = Weather->FishConditions();
            // Rooting comes and goes (it roots, shuffles, roots again), so arrival is taken as
            // having settled at a food spot, not as being mid-root at the sampling instant. It may
            // stay on the silt fauna or switch to another natural food that is richer now — both
            // are a valid answer to a patch eaten down.
            if (!bPatch || !Crucian->bAtSpot) return false;
            if (Crucian->FeedOn == EFishFood::Benthos)
                return Food.Left(EFishFood::Benthos, Patch) < 0.8f
                    && Food.Left(EFishFood::Benthos, Here) > Food.Left(EFishFood::Benthos, Patch) + 0.2f
                    && Food.Available(EFishFood::Benthos, Here, C) > Food.Available(EFishFood::Benthos, Patch, C);
            return LakeFood::IsNatural(Crucian->FeedOn) && Food.Left(EFishFood::Benthos, Patch) < 0.8f
                && Food.Available(Crucian->FeedOn, Here, C) > Food.Available(EFishFood::Benthos, Patch, C);
        };
        for (; Steps < 8000 && !MovedOn(); ++Steps)
        {
            Crucian->Hunger = 1;
            Life->Step(Dt);
            Crucian->Simulate(Dt);
            if (!bPatch && Crucian->bRooting) { bPatch = true; Patch = FVector2D(Crucian->GetActorLocation()); }
        }
        const float Left = Life->Food().Left(EFishFood::Benthos, Patch);
        AddInfo(FString::Printf(TEXT("Crucian seed %d: patch %.2f, feeding patch %.2f, %.0f cm off after %.0f s"), Seed,
            Left, Life->Food().Left(EFishFood::Benthos, FVector2D(Crucian->GetActorLocation())), (float)FVector2D::Distance(Patch, FVector2D(Crucian->GetActorLocation())), Steps * Dt));
        TestTrue(FString::Printf(TEXT("A crucian moves on to richer food after eating its patch down (seed %d)"), Seed), MovedOn());
        Crucian->Destroy();
    }
    // A depleted patch must trigger a fresh food search, across several decision streams.
    for (int32 Seed : { -646273053, 1, 7919, 20261031 })
    {
        Life->Food() = FLakeFoodField();
        Life->Step(0);
        ALakeFish* Crucian = Spawn(EFishSpecies::Crucian, 0.4f, Lily);
        Crucian->Random.Initialize(Seed);
        for (int32 K = 0; K < 50; ++K)
        {
            Choose(Crucian);
            if (Crucian->FeedOn == EFishFood::Benthos) break;
        }
        TestTrue(TEXT("A benthos feeding patch found"), Crucian->FeedOn == EFishFood::Benthos);
        Crucian->SetActorLocation(Crucian->RoamTarget);
        Crucian->bAtSpot = Crucian->bRooting = true;
        Crucian->PhaseUntil = 10;
        Crucian->NextFoodCheck = 0;
        const FVector2D Patch(Crucian->GetActorLocation());
        Life->Food().Eat(EFishFood::Benthos, Patch, 100, Crucian->WeightKg);
        Crucian->Simulate(Dt);
        const EFishFood Food = Crucian->FeedOn;
        const float Found = Life->Food().Available(Food, FVector2D(Crucian->RoamTarget), Weather->FishConditions());
        // Switching to plants is also a valid response; continuing on empty benthos is not.
        TestTrue(FString::Printf(TEXT("An exhausted patch triggers a search for available natural food (seed %d)"), Seed),
            LakeFood::IsNatural(Food) && Found > 0.08f && !Crucian->bAtSpot && !Crucian->bRooting);
        AddInfo(FString::Printf(TEXT("Exhausted patch seed %d: food %s, target availability %.2f"), Seed, LakeFood::Name(Food), Found));
        Crucian->Destroy();
    }
    // 6. Roach feed at the top at dusk (insects, plankton risen) and deep at noon (plankton sunk).
    {
        auto Survey = [&](float& Depth, int32& OnInsects)
        {
            ALakeFish* Roach = Spawn(EFishSpecies::Roach, 0.1f, Reed);
            Depth = 0;
            OnInsects = 0;
            for (int32 I = 0; I < 30; ++I)
            {
                Choose(Roach);
                Depth += (LakeFish::SurfaceZ - Roach->RoamTarget.Z) / 100 / 30;
                OnInsects += Roach->FeedOn == EFishFood::Insects;
            }
            Roach->Destroy();
        };
        float NoonDepth, DuskDepth;
        int32 NoonInsects, DuskInsects;
        SetSun(45, 70);
        Survey(NoonDepth, NoonInsects);
        const float DuskHour = SetSun(-2, 4);
        Survey(DuskDepth, DuskInsects);
        AddInfo(FString::Printf(TEXT("Roach by the reeds: noon feeds at %.2f m (insects %d of 30), dusk (%.1f h) at %.2f m (insects %d of 30)"), NoonDepth, NoonInsects, DuskHour, DuskDepth, DuskInsects));
        TestTrue(TEXT("Roach feed near the top at dusk, deeper at noon"), DuskHour > 0 && DuskDepth < NoonDepth - 0.5f && DuskInsects >= 10);
    }
    // 7. Near fry, a big perch hunts them; a small one mostly eats plankton and bloodworm.
    {
        SetSun(45, 70);
        FVector2D Shallow = Reed;
        bool bFry = false;
        for (float A = 0; A < 360 && !bFry; A += 30)
            for (float Out = 300; Out < 900 && !bFry; Out += 100)
            {
                Shallow = Reed + FVector2D(FMath::Cos(FMath::DegreesToRadians(A)), FMath::Sin(FMath::DegreesToRadians(A))) * Out;
                bFry = Life->FryCanBe(Shallow);
            }
        TestTrue(TEXT("A place for fry near the reeds"), bFry);
        FFryShoal Shoal;
        Shoal.Location = FVector(Shallow, LakeFish::SurfaceZ - 9);
        Shoal.Target = Shoal.Location;
        Shoal.NextWander = 1e6f;
        Life->Fry.Reset();
        Life->Fry.Add(Shoal);
        auto Hunts = [&](float Weight)
        {
            ALakeFish* Perch = Spawn(EFishSpecies::Perch, Weight, Reed);
            int32 N = 0;
            for (int32 I = 0; I < 30; ++I) { Choose(Perch); N += Perch->FeedOn == EFishFood::Fry; }
            Perch->Destroy();
            return N;
        };
        const int32 Big = Hunts(0.8f), Small = Hunts(0.08f);
        AddInfo(FString::Printf(TEXT("Perch near fry: 0.8 kg went for the fry %d of 30 times, 0.08 kg %d of 30"), Big, Small));
        TestTrue(TEXT("A big perch hunts fry, a small one mostly eats plankton and bloodworm"), Big >= 21 && Small <= Big - 8);
    }
    // 8. Bait in the layer a fish feeds in.
    {
        const float In = LakeFish::BaitInterest(EFishSpecies::Roach, EBait::Maggot, 0.1f, 0.1f), Off = LakeFish::BaitInterest(EFishSpecies::Roach, EBait::Maggot, 0.9f, 0.1f);
        AddInfo(FString::Printf(TEXT("Roach feeding at the top: maggot at the top %.2f, on the bottom %.2f"), In, Off));
        TestTrue(TEXT("A bait in the layer a fish feeds in is found far more readily"), In > 3 * Off);
    }
    World->DestroyWorld(false);
    return true;
}

// Groundbait: its smell builds over a minute and reaches farther with time, more balls smell
// stronger, it dissolves; roach and crucian go for bread groundbait, a worm in it draws perch, a
// pike pays it no heed; a ball thrown by the angler flies to the float, sinks and lies there; a
// school of roach 10 m off comes to it and feeds on the bottom (without it, it does not come); a
// crucian roots in it and eats it down.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeGroundbaitTest, "QuietLake.Fishing.Groundbait", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeGroundbaitTest::RunTest(const FString& Parameters)
{
    FVector2D Facing;
    const FVector2D Start = LakeMap::StartSpot(&Facing);
    const FVector2D Spot = Start + Facing * 600;
    // 1. Smell and dissolving.
    {
        FLakeFoodField Field;
        Field.Init();
        Field.AddGroundbait(Spot, EBait::Bread);
        const FGroundbaitSpot& G = Field.Groundbait()[0];
        const FVector2D Near = Spot + Facing * 300, Far = Spot + Facing * 1200;
        Field.Step(10);
        const float Near10 = Field.Scent(G, Near), Far10 = Field.Scent(G, Far);
        for (int32 I = 0; I < 170; ++I) Field.Step(1);
        const float Near180 = Field.Scent(G, Near), Far180 = Field.Scent(G, Far);
        const float One = Field.Scent(G, Near);
        for (int32 I = 0; I < 4; ++I) Field.AddGroundbait(Spot, EBait::Bread);
        const float Five = Field.Scent(Field.Groundbait()[0], Near);
        const float Before = Field.Groundbait()[0].Balls;
        for (int32 I = 0; I < 600; ++I) Field.Step(1);
        const float After = Field.Groundbait().Num() ? Field.Groundbait()[0].Balls : 0.0f;
        AddInfo(FString::Printf(TEXT("Smell of one ball: 3 m %.2f -> %.2f, 12 m %.2f -> %.2f (10 s -> 3 min); five balls at 3 m %.2f; %.1f balls -> %.1f in 10 min"), Near10, Near180, Far10, Far180, Five, Before, After));
        TestTrue(TEXT("The smell builds over a minute and reaches farther with time"), Near180 > 2 * Near10 && Far10 == 0 && Far180 > 0.02f);
        TestTrue(TEXT("More balls smell stronger"), Five > One + 0.2f);
        TestTrue(TEXT("Groundbait dissolves (half in 10 min)"), After > 0.4f * Before && After < 0.6f * Before);
    }
    // 2. Who goes for it.
    {
        FGroundbaitSpot Bread, Worm;
        Bread.Balls = Worm.Balls = 3;
        Bread.Added[(int32)EBait::Bread] = 3;
        Worm.Added[(int32)EBait::Worm] = 3;
        auto Appeal = [](EFishSpecies Species, float Weight, const FGroundbaitSpot& G) { return LakeFood::GroundbaitAppeal(LakeFish::DietOf(Species, LakeFish::Size01(Species, Weight)), G); };
        const float Roach = Appeal(EFishSpecies::Roach, 0.15f, Bread), Crucian = Appeal(EFishSpecies::Crucian, 0.4f, Bread);
        const float PerchBread = Appeal(EFishSpecies::Perch, 0.15f, Bread), PerchWorm = Appeal(EFishSpecies::Perch, 0.15f, Worm), Pike = Appeal(EFishSpecies::Pike, 2.0f, Worm);
        AddInfo(FString::Printf(TEXT("Appeal: roach %.2f, crucian %.2f to bread; perch %.2f to bread, %.2f with worm; pike %.2f"), Roach, Crucian, PerchBread, PerchWorm, Pike));
        TestTrue(TEXT("Roach and crucian go for bread groundbait"), Roach > 0.8f && Crucian > 0.9f);
        TestTrue(TEXT("A worm mixed in draws perch; a pike pays it no heed"), PerchWorm > PerchBread + 0.2f && Pike == 0);
        TestTrue(TEXT("The hook bait it was mixed with"), LakeFood::GroundbaitMix(Worm) == EBait::Worm && LakeFood::GroundbaitMix(Bread) == EBait::Bread);
    }
    // A pond with its weather: a calm clear day.
    constexpr float Dt = 0.05f;
    auto MakeWorld = [](UWorld*& World, ALakeLife*& Life)
    {
        World = UWorld::CreateWorld(EWorldType::Game, false);
        Life = World->SpawnActor<ALakeLife>();
        ALakeWeather* Weather = World->SpawnActor<ALakeWeather>();
        FLakeWeatherState Calm;
        Calm.Cloud = 0.1f; Calm.Fog = 0; Calm.Rain = 0; Calm.Wind = 0.06f;
        Weather->SetWeatherNow(Calm);
        Weather->SetHours(10);
        Life->Step(0);
    };
    // 3. A ball thrown by the angler.
    {
        UWorld* World;
        ALakeLife* Life;
        MakeWorld(World, Life);
        const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X));
        ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(FVector(Start + Facing * 90, 100), FRotator(0, Yaw, 0));
        APlayerController* Controller = World->SpawnActor<APlayerController>();
        Controller->Possess(Fisher);
        Controller->SetControlRotation(Fisher->GetActorRotation());
        Fisher->Bait = EBait::Maggot;
        FLakeFishingFixture::ImmersedRig(Fisher);
        const int32 Had = Fisher->GroundbaitLeft;
        Fisher->ServerThrowGroundbait_Implementation(Fisher->GroundbaitTarget());
        for (int32 I = 0; I < 300 && Life->Food().Groundbait().IsEmpty(); ++I) Life->Step(Dt);
        const bool bLies = Life->Food().Groundbait().Num() == 1;
        const float Off = bLies ? FVector2D::Distance(Life->Food().Groundbait()[0].Location, FVector2D(Fisher->FloatPosition)) : -1.0f;
        AddInfo(FString::Printf(TEXT("Thrown: %d -> %d balls, lies %.0f cm from the float; %s"), Had, Fisher->GroundbaitLeft, Off, *Fisher->Status));
        TestTrue(TEXT("A ball goes to the float, sinks and lies on the bottom, mixed with the hook bait"), Fisher->GroundbaitLeft == Had - 1 && bLies && Off < 120
            && LakeFood::GroundbaitMix(Life->Food().Groundbait()[0]) == EBait::Maggot);
        World->DestroyWorld(false);
    }
    // 4. A school of roach 5 m off smells groundbait and comes to feed on the bottom there. Two
    //    things decide it, and the check sets both of them up instead of hoping for them:
    //     - the smell has to pass the 0.03 the fish act on. Its reach grows from 600 cm to 1500 over
    //       three minutes (LakeFood::Scent), so after a minute and a half it is 0.005 at 10 m. The
    //       old form of this check waited that long, the smell never counted, and whether the school
    //       came was decided by where it happened to browse; now the balls lie three minutes first.
    //     - the groundbait has to beat what the fish finds on its own, and it is worth only 0.5-1 of
    //       its value (LakeFood::GroundbaitAppeal): among rich natural food that is a coin toss, so
    //       the place is eaten out first and the check states that it was.
    auto School = [&](bool bGroundbait, float& NearSeconds, float& CameAfter, float& FeedDepth, float& Scent, float& GroundbaitSeconds, float& Natural)
    {
        UWorld* World;
        ALakeLife* Life;
        MakeWorld(World, Life);
        const FVector2D Away = Spot + Facing * 500;
        if (bGroundbait)
        {
            for (int32 I = 0; I < 5; ++I) Life->Food().AddGroundbait(Spot, EBait::Bread);
            for (int32 I = 0; I < 180; ++I) Life->Step(1);   // its smell has three minutes to spread
        }
        // Eat the natural food out where the school is, so the smell is the only good food there.
        for (int32 X = -2000; X <= 2000; X += 250)
            for (int32 Y = -2000; Y <= 2000; Y += 250)
                for (EFishFood F : { EFishFood::Plankton, EFishFood::Insects, EFishFood::Benthos, EFishFood::Plants })
                    Life->Food().Eat(F, Away + FVector2D(X, Y), 1000.0f, 1.0f);
        const ALakeWeather* Weather = ALakeWeather::Get(World);
        const FFishConditions Conditions = Weather ? Weather->FishConditions() : FFishConditions();
        const FFishDiet Diet = LakeFish::DietOf(EFishSpecies::Roach, 0.12f);
        Natural = 0;
        for (EFishFood F : { EFishFood::Plankton, EFishFood::Insects, EFishFood::Benthos, EFishFood::Plants })
            Natural = FMath::Max(Natural, Diet.Of(F) * Life->Food().Available(F, Away, Conditions));
        TArray<ALakeFish*> Fish;
        for (int32 I = 0; I < 5; ++I)
        {
            const FVector2D P = Away + FVector2D(I * 40.0f, (I % 2) * 40.0f);
            ALakeFish* Roach = SpawnFish(World, EFishSpecies::Roach, 0.12f, FVector(P, FMath::Lerp(LakeFish::SurfaceZ - 10, ALakeFish::BottomZ(P.X, P.Y) + 10, 0.5f)), 4242);
            Roach->Random.Initialize(4242 + 17 * I);   // the same decisions whatever ran before
            Roach->WanderSeed = 0;
            Roach->Hunger = 1;
            Fish.Add(Roach);
        }
        Life->Step(0);
        Scent = 0;
        Life->Food().SmellGroundbait(Away, Scent);
        for (ALakeFish* Roach : Fish) Roach->SetGoal(EFishGoal::Feed, 1000);
        NearSeconds = 0;
        CameAfter = -1;
        FeedDepth = 0;
        GroundbaitSeconds = 0;
        int32 Deep = 0;
        for (int32 Steps = 0; Steps < 6000; ++Steps)   // 5 min
        {
            Life->Step(Dt);
            for (ALakeFish* Roach : Fish) { Roach->Hunger = FMath::Max(Roach->Hunger, 0.6f); Roach->Simulate(Dt); }
            const ALakeFish* Lead = Life->SchoolLeader(4242);
            if (Lead && Lead->GetFeedOn() == EFishFood::Groundbait) GroundbaitSeconds += Dt;
            if (Lead && FVector2D::Distance(FVector2D(Lead->GetActorLocation()), Spot) < 250)
            {
                NearSeconds += Dt;
                if (CameAfter < 0) CameAfter = Steps * Dt;
                FeedDepth += (LakeFish::SurfaceZ - Lead->GetActorLocation().Z) / FMath::Max(1.0f, LakeFish::SurfaceZ - ALakeFish::BottomZ(Lead->GetActorLocation().X, Lead->GetActorLocation().Y));
                ++Deep;
            }
        }
        FeedDepth = Deep ? FeedDepth / Deep : 0;
        World->DestroyWorld(false);
    };
    {
        float WithNear, WithCame, WithDepth, WithoutNear, WithoutCame, WithoutDepth;
        float WithScent, WithoutScent, WithOnIt, WithoutOnIt, WithNatural, WithoutNatural;
        School(true, WithNear, WithCame, WithDepth, WithScent, WithOnIt, WithNatural);
        School(false, WithoutNear, WithoutCame, WithoutDepth, WithoutScent, WithoutOnIt, WithoutNatural);
        // How far the smell carries at all: the balls' reach stops well short of 30 m whatever the age.
        FLakeFoodField Far;
        Far.Init();
        Far.AddGroundbait(Spot, EBait::Bread);
        for (int32 I = 0; I < 600; ++I) Far.Step(1);
        const float FarScent = Far.Scent(Far.Groundbait()[0], Spot + Facing * 3000);
        AddInfo(FString::Printf(TEXT("Roach school 5 m off: smell %.3f at the school, natural food left %.2f; with groundbait came after %.0f s, %.0f s of 5 min by it and %.0f s on the groundbait, at %.2f of the depth; without it %.0f s by the spot and %.0f s on groundbait; smell at 30 m %.3f"),
            WithScent, WithNatural, WithCame, WithNear, WithOnIt, WithDepth, WithoutNear, WithoutOnIt, FarScent));
        TestTrue(TEXT("Five balls smell strongly enough at 5 m after three minutes"), WithScent > 0.4f);
        TestTrue(TEXT("There is no groundbait smell without it"), WithoutScent < 0.001f);
        TestTrue(TEXT("The natural food where the school is has been eaten out"), WithNatural < 0.1f);
        TestTrue(TEXT("A school 5 m off comes to groundbait and feeds near the bottom there"), WithCame >= 0 && WithCame < 120 && WithNear > 120 && WithDepth > 0.6f);
        TestTrue(TEXT("It feeds on that groundbait"), WithOnIt > 60);
        TestTrue(TEXT("Without groundbait none of it is eaten"), WithoutOnIt < 0.001f);
        TestTrue(TEXT("The smell does not carry 30 m"), FarScent < 0.001f);
    }
    // 5. A crucian roots in it and eats it down.
    {
        UWorld* World;
        ALakeLife* Life;
        MakeWorld(World, Life);
        for (int32 I = 0; I < 3; ++I) Life->Food().AddGroundbait(Spot, EBait::Bread);
        for (int32 I = 0; I < 90; ++I) Life->Step(1);
        const float Before = Life->Food().Groundbait()[0].Balls;
        const FVector2D P = Spot + Facing * 300;
        ALakeFish* Crucian = SpawnFish(World, EFishSpecies::Crucian, 0.4f, FVector(P, ALakeFish::BottomZ(P.X, P.Y) + 15));
        Crucian->Hunger = 1;
        Crucian->NextMoodCheck = 0;
        int32 OnIt = 0;
        for (int32 I = 0; I < 10; ++I) { Crucian->SetGoal(EFishGoal::Cruise, 1000); Crucian->SetGoal(EFishGoal::Feed, 1000); OnIt += Crucian->FeedOn == EFishFood::Groundbait; }
        for (int32 K = 0; K < 30 && Crucian->FeedOn != EFishFood::Groundbait; ++K) { Crucian->SetGoal(EFishGoal::Cruise, 1000); Crucian->SetGoal(EFishGoal::Feed, 1000); }
        bool bRooted = false;
        for (int32 Steps = 0; Steps < 2400; ++Steps)   // 2 min
        {
            Crucian->Hunger = FMath::Max(Crucian->Hunger, 0.6f);
            Life->Step(Dt);
            Crucian->Simulate(Dt);
            bRooted |= Crucian->bRooting && Crucian->FeedOn == EFishFood::Groundbait;
        }
        const float After = Life->Food().Groundbait().Num() ? Life->Food().Groundbait()[0].Balls : 0.0f;
        const float Dissolved = Before * FMath::Exp(-120 * UE_LN2 / FLakeFoodField::DissolveSeconds);
        AddInfo(FString::Printf(TEXT("Crucian 3 m from it: went for it %d of 10 times; rooted in it %d; %.2f balls -> %.2f (dissolving alone: %.2f)"), OnIt, bRooted, Before, After, Dissolved));
        TestTrue(TEXT("A crucian goes for groundbait, roots in it and eats it down"), OnIt >= 6 && bRooted && After < Dissolved - 0.1f);
        World->DestroyWorld(false);
    }
    return true;
}

// Live Fish learns from the angler: a fish that came off the hook is wary of that bait above all
// (a pricked one less so), and forgets over game hours (a perch soon, a crucian slowly); a school
// that saw one of its own hooked grows wary too; catches build up pressure at a place, which dies
// away over hours, and fish avoid a much-fished place.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeLearningTest, "QuietLake.Fishing.Learning", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeLearningTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ALakeLife* Life = World->SpawnActor<ALakeLife>();
    Life->Step(0);
    const float GameHour = ALakeWeather::DayMinutes * 60.0f / 24.0f;
    FVector2D Facing;
    const FVector2D Start = LakeMap::StartSpot(&Facing);
    const FVector2D Open = Start + Facing * 900;
    auto At = [](FVector2D P) { return FVector(P, FMath::Lerp(LakeFish::SurfaceZ - 10, ALakeFish::BottomZ(P.X, P.Y) + 10, 0.5f)); };
    ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(FVector(Start + Facing * 90, 100), FRotator(0, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)), 0));
    APlayerController* Controller = World->SpawnActor<APlayerController>();
    Controller->Possess(Fisher);
    Fisher->Bait = EBait::Maggot;
    auto Forget = [&](TArray<ALakeFish*> Fish, float Hours)
    {
        for (int32 I = 0, N = FMath::CeilToInt(Hours * GameHour / 0.5f); I < N; ++I) for (ALakeFish* F : Fish) F->Simulate(0.5f);
    };

    // 1. Off the hook on a maggot: wary of maggots above all; pricked, less so; forgotten in hours.
    {
        ALakeFish* Lost = SpawnFish(World, EFishSpecies::Roach, 0.15f, At(Open));
        Lost->Hook(Fisher);
        Lost->Release(10);
        ALakeFish* Pricked = SpawnFish(World, EFishSpecies::Roach, 0.15f, At(Open + Facing * 200));
        Pricked->State = ALakeFish::EState::Holding;
        Pricked->TargetAngler = Fisher;
        Pricked->Release(10);
        const float Maggot = Lost->Willingness(EBait::Maggot), Worm = Lost->Willingness(EBait::Worm), PrickedMaggot = Pricked->Willingness(EBait::Maggot);
        Forget({ Lost }, LakeFish::Info(EFishSpecies::Roach).Learning.ForgetHours);
        const float Later = Lost->Willingness(EBait::Maggot);
        AddInfo(FString::Printf(TEXT("Roach off the hook on a maggot: maggot %.2f, worm %.2f; pricked: maggot %.2f; %.0f game hours later maggot %.2f"),
            Maggot, Worm, PrickedMaggot, LakeFish::Info(EFishSpecies::Roach).Learning.ForgetHours, Later));
        TestTrue(TEXT("A fish off the hook is wary of that bait above all"), Maggot < 0.35f && Worm > Maggot + 0.2f && Worm < 0.9f);
        TestTrue(TEXT("A pricked fish is less wary than one that was hooked"), PrickedMaggot > Maggot + 0.1f && PrickedMaggot < 0.8f);
        TestTrue(TEXT("It forgets over game hours"), Later > Maggot + 0.2f);
        Lost->Destroy();
        Pricked->Destroy();
    }
    // 2. A perch forgets soon, a crucian slowly.
    {
        ALakeFish* Perch = SpawnFish(World, EFishSpecies::Perch, 0.2f, At(Open));
        ALakeFish* Crucian = SpawnFish(World, EFishSpecies::Crucian, 0.4f, At(Open + Facing * 300));
        Perch->Frighten(0.8f, (int32)EBait::Worm);
        Crucian->Frighten(0.8f, (int32)EBait::Worm);
        Forget({ Perch, Crucian }, 4);
        AddInfo(FString::Printf(TEXT("Four game hours after the same fright: perch caution %.2f, crucian %.2f"), Perch->CautionNow(), Crucian->CautionNow()));
        TestTrue(TEXT("A perch forgets sooner than a crucian"), Perch->CautionNow() < 0.6f * Crucian->CautionNow());
        Perch->Destroy();
        Crucian->Destroy();
    }
    // 3. A school that saw one of its own hooked grows wary too.
    {
        TArray<ALakeFish*> School;
        for (int32 I = 0; I < 5; ++I) School.Add(SpawnFish(World, EFishSpecies::Roach, 0.15f, At(Open + FVector2D(I * 40.0f, 0)), 77));
        Life->Step(0);
        School[0]->Hook(Fisher);
        float Least = 1;
        for (int32 I = 1; I < 5; ++I) Least = FMath::Min(Least, School[I]->Willingness(EBait::Maggot));
        AddInfo(FString::Printf(TEXT("School mates of a hooked roach: least willing to take a maggot %.2f"), Least));
        TestTrue(TEXT("School mates that saw one hooked grow wary"), Least < 0.85f);
        for (ALakeFish* F : School) F->Destroy();
    }
    // 4. Pressure: three catches at a place, gone halfway in four game hours; fish keep away from it.
    {
        const FVector2D Far20 = Start + Facing * 2000;   // open water 20 m out
        const FVector Place(Far20, 0);
        Life->Pressure.Reset();   // (the fish lost above left some)
        auto Choices = [&]()
        {
            ALakeFish* Roach = SpawnFish(World, EFishSpecies::Roach, 0.15f, At(Far20), 78);
            Life->Step(0);
            int32 Near = 0;
            for (int32 I = 0; I < 60; ++I)
            {
                Roach->SetGoal(EFishGoal::Feed, 1000);
                Roach->SetGoal(EFishGoal::Cruise, 1000);
                Near += FVector2D::Distance(FVector2D(Roach->RoamTarget), Far20) < 600;
            }
            Roach->Destroy();
            return Near;
        };
        const int32 Calm = Choices();
        for (int32 I = 0; I < 3; ++I) Life->AddPressure(Place, 1);
        const float Now = Life->PressureAt(Place), Far = Life->PressureAt(Place + FVector(2000, 0, 0));
        const int32 Fished = Choices();
        for (int32 I = 0, N = FMath::CeilToInt(ALakeLife::PressureHours * GameHour); I < N; ++I) Life->Step(1);
        const float Later = Life->PressureAt(Place);
        AddInfo(FString::Printf(TEXT("Pressure after three catches %.2f (20 m off %.2f), %.0f game hours later %.2f; a roach went within 6 m of the place %d of 60 times, %d when it was fished"),
            Now, Far, ALakeLife::PressureHours, Later, Calm, Fished));
        TestTrue(TEXT("Catches build up pressure at a place, not 20 m off"), Now > 2.5f && Far < 0.1f);
        TestTrue(TEXT("Pressure dies away over game hours"), Later > 0.4f * Now && Later < 0.6f * Now);
        TestTrue(TEXT("Fish keep away from a much-fished place"), Fished < Calm * 0.75f);
    }
    World->DestroyWorld(false);
    return true;
}

// The water by depth: steady calm warm weather layers it (a thermocline, cold stale water under
// it), wind mixes it; dense weed is short of oxygen at dawn after a warm calm night and rich in it
// in the afternoon; fish keep to water they can live in (a roach rises out of the stale bottom
// layer, leaves the lilies at such a dawn while a crucian stays), and the worst off gulp air.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeWaterTest, "QuietLake.Fishing.Water", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeWaterTest::RunTest(const FString& Parameters)
{
    // The deepest water and dense lilies.
    FVector2D Deep = FVector2D::ZeroVector, Lily = FVector2D::ZeroVector;
    {
        const FBox2D B = LakeMap::LakeBounds();
        float Most = 0, MostWeed = 0;
        for (float Y = B.Min.Y; Y < B.Max.Y; Y += 500)
            for (float X = B.Min.X; X < B.Max.X; X += 500)
            {
                if (!LakeMap::IsWater(X, Y)) continue;
                const float D = LakeMap::Depth(X, Y), W = LakeWater::WeedAt(X, Y);
                if (D > Most) { Most = D; Deep = FVector2D(X, Y); }
                if (W > MostWeed && D > 70) { MostWeed = W; Lily = FVector2D(X, Y); }
            }
    }
    const float DeepM = LakeMap::Depth(Deep.X, Deep.Y) / 100, LilyM = LakeMap::Depth(Lily.X, Lily.Y) / 100;
    FFishConditions Hot;   // a steady calm hot afternoon
    Hot.Hour = 15; Hot.SunElevation = 40; Hot.Cloud = 0.1f; Hot.Wind = 0.05f; Hot.WaterTempC = 23; Hot.StableHours = 24;
    FFishConditions Windy = Hot; Windy.Wind = 0.6f;
    FFishConditions Dawn = Hot; Dawn.Hour = 5; Dawn.SunElevation = -3; Dawn.bMorning = true;
    FFishConditions WindyDawn = Dawn; WindyDawn.Wind = 0.6f;
    // 1. Layers.
    const float Thermo = LakeWater::ThermoclineM(Hot);
    AddInfo(FString::Printf(TEXT("Calm hot day: layered %.2f, thermocline %.1f m; %.1f C at 1 m, %.1f C at %.1f m; %.1f mg/l at 1 m, %.1f at the bottom. Windy: layered %.2f, %.1f C and %.1f mg/l at the bottom"),
        LakeWater::Stratification(Hot), Thermo, LakeWater::TempC(Hot, 1), LakeWater::TempC(Hot, DeepM - 0.2f), DeepM - 0.2f,
        LakeWater::OxygenMgL(Hot, Deep.X, Deep.Y, 1), LakeWater::OxygenMgL(Hot, Deep.X, Deep.Y, DeepM - 0.2f),
        LakeWater::Stratification(Windy), LakeWater::TempC(Windy, DeepM - 0.2f), LakeWater::OxygenMgL(Windy, Deep.X, Deep.Y, DeepM - 0.2f)));
    TestTrue(TEXT("Steady calm heat layers the water: cold, stale water under a thermocline at 2-3 m"), LakeWater::Stratification(Hot) > 0.7f && Thermo > 2 && Thermo < 3
        && LakeWater::TempC(Hot, 1) - LakeWater::TempC(Hot, DeepM - 0.2f) > 4 && LakeWater::OxygenMgL(Hot, Deep.X, Deep.Y, DeepM - 0.2f) < 3 && LakeWater::OxygenMgL(Hot, Deep.X, Deep.Y, 1) > 7);
    TestTrue(TEXT("Wind mixes it"), LakeWater::Stratification(Windy) < 0.2f && LakeWater::OxygenMgL(Windy, Deep.X, Deep.Y, DeepM - 0.2f) > 6);
    // 2. Weed.
    const float LilyDawn = LakeWater::OxygenMgL(Dawn, Lily.X, Lily.Y, LilyM - 0.1f), LilyDay = LakeWater::OxygenMgL(Hot, Lily.X, Lily.Y, LilyM - 0.1f), LilyWind = LakeWater::OxygenMgL(WindyDawn, Lily.X, Lily.Y, LilyM - 0.1f);
    AddInfo(FString::Printf(TEXT("Lilies (%.1f m deep, weed %.2f) near the bottom: %.1f mg/l at a calm dawn, %.1f in the afternoon, %.1f at a windy dawn"), LilyM, LakeWater::WeedAt(Lily.X, Lily.Y), LilyDawn, LilyDay, LilyWind));
    TestTrue(TEXT("Dense weed is short of oxygen at dawn after a warm calm night, rich in it in the afternoon"), LilyDawn < 3 && LilyDay > 9 && LilyWind > LilyDawn + 2);
    // 3. Fish.
    const float BottomZ = ALakeFish::BottomZ(Deep.X, Deep.Y) + 10;
    const float RoachZ = LakeWater::ToLivable(EFishSpecies::Roach, Hot, Deep.X, Deep.Y, BottomZ);
    const float RoachLily = LakeWater::Livable(EFishSpecies::Roach, Dawn, Lily.X, Lily.Y), CrucianLily = LakeWater::Livable(EFishSpecies::Crucian, Dawn, Lily.X, Lily.Y);
    AddInfo(FString::Printf(TEXT("Roach sent to the bottom of the deep water on a calm hot day keeps at %.1f m; at a calm dawn the lilies suit a roach %.2f, a crucian %.2f; gasping there: perch %d, crucian %d"),
        (LakeFish::SurfaceZ - RoachZ) / 100, RoachLily, CrucianLily, LakeWater::IsGasping(EFishSpecies::Perch, LilyDawn), LakeWater::IsGasping(EFishSpecies::Crucian, LilyDawn)));
    TestTrue(TEXT("A roach keeps above the stale bottom layer"), (LakeFish::SurfaceZ - RoachZ) / 100 < Thermo + 0.8f);
    TestTrue(TEXT("At such a dawn a roach leaves the lilies, a crucian stays"), RoachLily < 0.5f && CrucianLily > 0.8f);
    TestTrue(TEXT("A perch there gulps air, a crucian does not"), LakeWater::IsGasping(EFishSpecies::Perch, LilyDawn) && !LakeWater::IsGasping(EFishSpecies::Crucian, LilyDawn));
    return true;
}

// Mixed schools: a school of roach and one of rudd of a size join when they meet; perch never join
// roach, nor small roach big rudd; frightened, the mixed school scatters by kind; the biggest fish leads.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeMixedSchoolsTest, "QuietLake.Fishing.MixedSchools", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeMixedSchoolsTest::RunTest(const FString& Parameters)
{
    auto Id = [](const TCHAR* Name) { for (int32 I = 0; I < LakeFish::NumSpecies(); ++I) if (LakeFish::Info((EFishSpecies)I).Id == Name) return (EFishSpecies)I; return EFishSpecies::MaxSpecies; };
    const EFishSpecies Rudd = Id(TEXT("rudd")), Bleak = Id(TEXT("bleak")), Bream = Id(TEXT("bream"));
    TestTrue(TEXT("Rudd, bleak and bream live in the pond"), Rudd != EFishSpecies::MaxSpecies && Bleak != EFishSpecies::MaxSpecies && Bream != EFishSpecies::MaxSpecies);
    if (Rudd == EFishSpecies::MaxSpecies) return false;
    TestTrue(TEXT("Who schools with whom"), LakeFish::SchoolsTogether(EFishSpecies::Roach, Rudd) && LakeFish::SchoolsTogether(Bleak, EFishSpecies::Roach)
        && LakeFish::SchoolsTogether(Bream, EFishSpecies::Roach) && !LakeFish::SchoolsTogether(EFishSpecies::Perch, EFishSpecies::Roach) && !LakeFish::SchoolsTogether(Rudd, Bleak));

    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ALakeLife* Life = World->SpawnActor<ALakeLife>();
    FVector2D Facing;
    const FVector2D Open = LakeMap::StartSpot(&Facing) + Facing * 1500;
    const FVector2D Right(-Facing.Y, Facing.X);
    auto At = [](FVector2D P) { return FVector(P, FMath::Lerp(LakeFish::SurfaceZ - 10, ALakeFish::BottomZ(P.X, P.Y) + 10, 0.5f)); };
    auto School = [&](EFishSpecies Species, float Weight, FVector2D P, int32 Id)
    {
        TArray<ALakeFish*> Fish;
        for (int32 I = 0; I < 6; ++I)
        {
            ALakeFish* F = SpawnFish(World, Species, Weight * (1 + 0.03f * I), At(P + FVector2D((I % 3) * 40.0f, (I / 3) * 40.0f)), Id);
            F->SetGoal(EFishGoal::Cruise, 1000);
            Fish.Add(F);
        }
        return Fish;
    };
    auto Ids = [](const TArray<ALakeFish*>& A, const TArray<ALakeFish*>& B) { TSet<int32> S; for (ALakeFish* F : A) S.Add(F->GetSchool()); for (ALakeFish* F : B) S.Add(F->GetSchool()); return S.Num(); };
    auto Gone = [](TArray<ALakeFish*>& Fish) { for (ALakeFish* F : Fish) F->Destroy(); Fish.Reset(); };
    auto Check = [&] { Life->Step(0); Life->UpdateSchools(); Life->Step(0); };

    // 1. Roach and rudd of a size meet: one school, the biggest fish leading it.
    TArray<ALakeFish*> Roach = School(EFishSpecies::Roach, 0.15f, Open, 101), Red = School(Rudd, 0.16f, Open + Right * 200, 102);
    Check();
    const ALakeFish* Lead = Life->SchoolLeader(Roach[0]->GetSchool());
    float Biggest = 0;
    for (const TArray<ALakeFish*>* Group : { &Roach, &Red }) for (const ALakeFish* F : *Group) Biggest = FMath::Max(Biggest, F->WeightKg);
    AddInfo(FString::Printf(TEXT("Roach and rudd 2 m apart: %d school(s); leader %s %.3f kg (biggest %.3f)"), Ids(Roach, Red), Lead ? *LakeFish::Info(Lead->Species).Noun : TEXT("-"), Lead ? Lead->WeightKg : 0.0f, Biggest));
    TestTrue(TEXT("Roach and rudd of a size join"), Ids(Roach, Red) == 1);
    TestTrue(TEXT("The biggest fish leads"), Lead && FMath::IsNearlyEqual(Lead->WeightKg, Biggest));
    // 2. Frightened, the mixed school scatters by kind.
    const int32 SplitsBefore = Life->Stats().Splits;
    for (ALakeFish* F : Roach) F->Fear = 0.9f;
    for (ALakeFish* F : Red) F->Fear = 0.9f;
    Check();
    bool bByKind = true;
    for (ALakeFish* F : Roach) bByKind &= F->GetSchool() == Roach[0]->GetSchool();
    for (ALakeFish* F : Red) bByKind &= F->GetSchool() == Red[0]->GetSchool();
    AddInfo(FString::Printf(TEXT("Frightened: %d school(s), split %d"), Ids(Roach, Red), Life->Stats().Splits - SplitsBefore));
    TestTrue(TEXT("A frightened mixed school scatters by kind"), Ids(Roach, Red) == 2 && bByKind);
    Gone(Roach);
    Gone(Red);
    // 3. Perch never join roach; small roach do not join big rudd.
    TArray<ALakeFish*> Perch = School(EFishSpecies::Perch, 0.15f, Open, 103), Roach2 = School(EFishSpecies::Roach, 0.15f, Open + Right * 200, 104);
    Check();
    const int32 PerchRoach = Ids(Perch, Roach2);
    Gone(Perch);
    Gone(Roach2);
    TArray<ALakeFish*> Small = School(EFishSpecies::Roach, 0.06f, Open, 105), BigRed = School(Rudd, 0.4f, Open + Right * 200, 106);
    Check();
    const int32 SmallBig = Ids(Small, BigRed);
    AddInfo(FString::Printf(TEXT("Perch and roach side by side: %d schools; roach 60 g and rudd 400 g: %d schools"), PerchRoach, SmallBig));
    TestTrue(TEXT("Perch never join roach, nor small roach big rudd"), PerchRoach == 2 && SmallBig == 2);
    Gone(Small);
    Gone(BigRed);
    World->DestroyWorld(false);
    return true;
}
#endif

// Fish keep off the bank and under the surface: perch hunting fry by the bank and a roach bolting
// towards it swim along it instead of into it; every species swims under its passport clearance
// (surfaceClearance) except at its surface moments (a sip, a gulp of air, a rush at the fry).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeBanksAndDepthTest, "QuietLake.Fishing.BanksAndDepth", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeBanksAndDepthTest::RunTest(const FString& Parameters)
{
    using namespace LakeFish;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ALakeLife* Life = World->SpawnActor<ALakeLife>();
    Life->SpawnFry();
    FRandomStream Random(17);
    TArray<ALakeFish*> All;
    const float Dt = 0.05f;
    float Closest = 1e9f;          // nearest any fish came to the water's edge (cm)
    int32 Above = 0, Samples = 0;   // samples over its ceiling outside a surface moment
    auto Run = [&](float Seconds)
    {
        for (int32 Step = 0; Step < Seconds / Dt; ++Step)
        {
            Life->Step(Dt);
            for (ALakeFish* Fish : All)
            {
                if (!IsValid(Fish)) continue;
                Fish->Simulate(Dt);
                const FVector P = Fish->GetActorLocation();
                Closest = FMath::Min(Closest, LakeMap::ShoreDistance(P.X, P.Y));
                if (!Fish->AtSurfaceMoment())
                {
                    ++Samples;
                    Above += P.Z > Fish->CeilingZ() + 3;
                }
            }
        }
    };
    auto Hits = [&] { int32 N = 0; for (ALakeFish* Fish : All) N += IsValid(Fish) ? Fish->BankHits : 0; return N; };
    auto Clear = [&] { for (ALakeFish* Fish : All) if (IsValid(Fish)) Fish->Destroy(); All.Reset(); Closest = 1e9f; Above = Samples = 0; };

    // 1. Perch drive the fry shoals closest to the bank: they hunt, but never ram the bank.
    TArray<FFryShoal> Shoals = Life->Fry;
    Shoals.Sort([](const FFryShoal& A, const FFryShoal& B) { return LakeMap::ShoreDistance(A.Location.X, A.Location.Y) < LakeMap::ShoreDistance(B.Location.X, B.Location.Y); });
    int32 Hunts = 0;
    float PerchClosest = 1e9f;
    for (int32 K = 0; K < FMath::Min(3, Shoals.Num()); ++K)
    {
        const FFryShoal Keep = Shoals[K];
        Life->Fry.Reset();
        Life->Fry.Add(Keep);
        const FVector2D Out = -LakeMap::NearestShore(FVector2D(Keep.Location)).Outward;
        const int32 Attacks = Life->Stats().FryAttacks;
        for (int32 I = 0; I < 5; ++I)
        {
            const FVector2D P = FVector2D(Keep.Location) + Out * (350 + I * 40) + FVector2D(-Out.Y, Out.X) * (I - 2) * 40.0f;
            ALakeFish* Perch = SpawnFish(World, EFishSpecies::Perch, 0.15f + 0.05f * I, FVector(P, FMath::Max(ALakeFish::BottomZ(P.X, P.Y) + 20, SurfaceZ - 60)), 20 + K);
            Perch->Hunger = 1;
            Perch->Energy = 1;
            All.Add(Perch);
        }
        Life->Step(0);
        if (ALakeFish* Lead = Life->SchoolLeader(20 + K)) Lead->SetGoal(EFishGoal::Feed, 600);
        Run(90);
        Hunts += Life->Stats().FryAttacks - Attacks;
        AddInfo(FString::Printf(TEXT("Perch at fry %.0f cm from the bank: %d rushes, came within %.0f cm of the edge, %d times stopped by the bank, %.1f%% over their ceiling"),
            LakeMap::ShoreDistance(Keep.Location.X, Keep.Location.Y), Life->Stats().FryAttacks - Attacks, Closest, Hits(), Samples ? 100.0f * Above / Samples : 0.0f));
        TestEqual(TEXT("Perch hunting by the bank never swim into it"), Hits(), 0);
        PerchClosest = FMath::Min(PerchClosest, Closest);
        Clear();
    }
    TestTrue(TEXT("Perch still drive the fry by the bank"), Hunts >= 3);
    TestTrue(TEXT("Perch keep half a metre of water off the bank"), PerchClosest > 50);

    // 2. A roach frightened from the lake side bolts towards the bank: it turns along it.
    {
        const FVector2D Edge = FVector2D(LakeMap::Shoreline()[LakeMap::Shoreline().Num() / 3].Location);
        FVector2D Out = -LakeMap::NearestShore(Edge).Outward;
        FVector2D P = Edge + Out * 150;
        for (float D = 150; D < 1500 && LakeMap::Depth(P.X, P.Y) < 60; D += 50) P = Edge + Out * D;
        Out = -LakeMap::NearestShore(P).Outward;
        ALakeFish* Roach = SpawnFish(World, EFishSpecies::Roach, 0.15f, FVector(P, FMath::Max(ALakeFish::BottomZ(P.X, P.Y) + 15, SurfaceZ - 40)));
        All.Add(Roach);
        const float Start = LakeMap::ShoreDistance(P.X, P.Y);
        Roach->Spook(FVector(P + Out * 120, Roach->GetActorLocation().Z), 4);
        Run(5);
        AddInfo(FString::Printf(TEXT("Roach bolting towards the bank from %.0f cm: came within %.0f cm, stopped by the bank %d times"), Start, Closest, Hits()));
        TestEqual(TEXT("A fish bolting towards the bank does not swim into it"), Hits(), 0);
        TestTrue(TEXT("A fish bolting towards the bank keeps off it"), Closest > 30);
        Clear();
    }

    // 3. Every species under its clearance: the targets it picks, and where it swims for a minute
    // and a half in open water (a fish put at the very top goes down to its layer).
    for (int32 Index = 0; Index < NumSpecies(); ++Index)
    {
        const EFishSpecies Species = (EFishSpecies)Index;
        const FFishSpeciesInfo& S = Info(Species);
        int32 HighSpots = 0;
        for (int32 I = 0; I < 200; ++I)
        {
            const FVector Spot = PickSpot(Species, Random, LakeMap::Centre, 0);
            HighSpots += Spot.Z > CeilingAt(Species, Spot.X, Spot.Y, S.MeshLength) + 0.1f;
        }
        TestEqual(FString::Printf(TEXT("%s picks places under its clearance"), *S.Name), HighSpots, 0);
        for (int32 I = 0; I < 6; ++I)
        {
            const FVector Spot = PickSpot(Species, Random, LakeMap::Centre, 0);
            All.Add(SpawnFish(World, Species, FMath::Lerp(S.MinWeight, S.MaxWeight, 0.15f * I), Spot));
        }
        const FVector Open(LakeMap::Centre, SurfaceZ - 5);
        ALakeFish* Top = SpawnFish(World, Species, FMath::Lerp(S.MinWeight, S.MaxWeight, 0.3f), Open);
        Top->RoamTarget = FVector(LakeMap::Centre + FVector2D(1500, 0), SurfaceZ - 5);
        All.Add(Top);
        Run(10);
        const float TopOver = Top->GetActorLocation().Z - Top->CeilingZ();
        Run(80);
        const float Over = Samples ? 100.0f * Above / Samples : 0.0f;
        AddInfo(FString::Printf(TEXT("%s: clearance %.0f cm, ceiling in open water %.0f cm under; %.2f%% of the time over it; one put at the top is %.0f cm over it after 10 s"),
            *S.Name, S.SurfaceClearance, SurfaceZ - CeilingAt(Species, LakeMap::Centre.X, LakeMap::Centre.Y, LengthCm(Species, FMath::Lerp(S.MinWeight, S.MaxWeight, 0.3f))), Over, TopOver));
        TestTrue(FString::Printf(TEXT("%s swims under its clearance"), *S.Name), Over < 1.0f);
        TestTrue(FString::Printf(TEXT("%s put at the top goes down to its layer"), *S.Name), TopOver < 3);
        TestEqual(FString::Printf(TEXT("%s in open water never meets the bank"), *S.Name), Hits(), 0);
        Clear();
    }

    // 4. Surface moments: sipping insects or gulping air lets a fish up to the top; the bleak lives
    // there, the bream far below.
    {
        ALakeFish* Roach = SpawnFish(World, EFishSpecies::Roach, 0.15f, FVector(LakeMap::Centre, SurfaceZ - 60));
        All.Add(Roach);
        const float Usual = Roach->CeilingZ();
        Roach->SetGoal(EFishGoal::Feed, 600);
        Roach->FeedOn = EFishFood::Insects;
        TestTrue(TEXT("A roach sipping insects may come up to the top"), Roach->AtSurfaceMoment() && Roach->CeilingZ() > Usual + 10);
        Roach->FeedOn = EFishFood::Plankton;
        Roach->SurfaceUntil = Roach->SimTime + 3;
        TestTrue(TEXT("A fish gulping air may come up to the top"), Roach->AtSurfaceMoment());
        Roach->SurfaceUntil = -1;
        TestFalse(TEXT("Feeding on plankton is no surface moment"), Roach->AtSurfaceMoment());
        auto Named = [](const TCHAR* Id) { for (int32 I = 0; I < NumSpecies(); ++I) if (Info((EFishSpecies)I).Id == Id) return (EFishSpecies)I; return EFishSpecies::Roach; };
        const float Bleak = CeilingAt(Named(TEXT("bleak")), LakeMap::Centre.X, LakeMap::Centre.Y, 12), Bream = CeilingAt(Named(TEXT("bream")), LakeMap::Centre.X, LakeMap::Centre.Y, 40);
        TestTrue(TEXT("The bleak keeps to the top, the bream well under it"), Bleak > SurfaceZ - 6 && Bream < SurfaceZ - 40);
        Clear();
    }
    World->DestroyWorld(false);
    return true;
}

#if WITH_DEV_AUTOMATION_TESTS
// The rod case: on the back it stays shut; laid on land within reach (not in the water, not far), it
// opens; a rod goes in only with its rig in the free hand, one comes out into empty hands or swaps
// with the one held; walking away closes it; a rod is laid on the ground and picked up again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeRodCaseTest, "QuietLake.Fishing.RodCase", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeRodCaseTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    FVector2D Facing;
    const FVector2D Edge = LakeMap::StartSpot(&Facing) + Facing * 90;
    const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X));
    auto OnLand = [&](float Back, float Side = 0)
    {
        const FVector2D P = Edge - Facing * Back + FVector2D(-Facing.Y, Facing.X) * Side;
        return FVector(P, LakeMap::Ground(P.X, P.Y));
    };
    ALakeCharacter* A = World->SpawnActor<ALakeCharacter>(OnLand(150) + FVector(0, 0, 95), FRotator(0, Yaw, 0));
    APlayerController* PC = World->SpawnActor<APlayerController>();
    PC->Possess(A);
    PC->SetControlRotation(FRotator(0, Yaw, 0));
    const int32 Light = (int32)ERodKind::Light, Medium = (int32)ERodKind::Medium, Heavy = (int32)ERodKind::Heavy;
    auto Place = [A](int32 Rod) { return A->Rods[Rod].Place; };
    TestTrue(TEXT("Starts with the medium rod in the hands, the light and heavy ones in the case on the back"),
        A->HasRodInHands() && A->RodKind == ERodKind::Medium && Place(Light) == ERodPlace::Case && Place(Heavy) == ERodPlace::Case && A->CasePlace == ECasePlace::Back);

    // On the back it cannot be opened, nor rods moved.
    A->ServerCaseAction_Implementation(0);
    TestFalse(TEXT("The case does not open on the back"), A->bCaseOpen);
    A->ServerMoveRod_Implementation(Medium, 1);
    TestTrue(TEXT("No rod goes into a shut case"), Place(Medium) == ERodPlace::Hands);
    // Not into the water, not too far.
    const FVector Water(Edge + Facing * 250, LakeFish::SurfaceZ);
    TestTrue(TEXT("(the test point is water)"), LakeMap::IsWater(Water.X, Water.Y));
    A->ServerPlaceCase_Implementation(Water, Yaw + 90);
    TestTrue(TEXT("The case is not laid in the water"), A->CasePlace == ECasePlace::Back);
    A->ServerPlaceCase_Implementation(OnLand(150 + 600), Yaw + 90);
    TestTrue(TEXT("The case is not laid out of reach"), A->CasePlace == ECasePlace::Back);
    A->ServerPlaceCase_Implementation(OnLand(150 + 110), Yaw + 90);
    TestTrue(TEXT("The case is laid on the ground within reach, shut"), A->CasePlace == ECasePlace::Ground && !A->bCaseOpen && FVector::Dist2D(A->CaseBase, OnLand(260)) < 5);
    A->ServerCaseAction_Implementation(0);
    TestTrue(TEXT("On the ground it opens"), A->bCaseOpen);

    // A rod goes in only with its rig in the free hand.
    FLakeFishingFixture::ImmersedRig(A);
    A->ServerMoveRod_Implementation(Medium, 1);
    TestTrue(TEXT("Not with the tackle in the water"), Place(Medium) == ERodPlace::Hands && A->FishingState == EFishingState::Waiting);
    A->FinishFishing(TEXT("out"));
    A->bCharging = true;
    A->ServerMoveRod_Implementation(Medium, 1);
    TestTrue(TEXT("Not during the cast"), Place(Medium) == ERodPlace::Hands);
    A->bCharging = false;
    A->bRigInHand = false;
    A->ServerMoveRod_Implementation(Medium, 1);
    TestTrue(TEXT("Not with the rig swinging on the line"), Place(Medium) == ERodPlace::Hands);
    A->bRigInHand = true;
    A->ServerMoveRod_Implementation(Medium, 1);
    TestTrue(TEXT("Put away into the middle compartment"), !A->HasRodInHands() && Place(Medium) == ERodPlace::Case && A->Rods[Medium].CaseSlot == 1);
    A->NextActionTime = 0;
    A->ServerFish_Implementation();
    TestTrue(TEXT("LMB with empty hands does not cast"), !A->bCharging && A->FishingState == EFishingState::Idle && !A->HasRodInHands());

    // Out into empty hands; dropped on the hands, the one held swaps into its compartment.
    A->ServerMoveRod_Implementation(Heavy, 3);
    TestTrue(TEXT("The heavy rod comes out of the third compartment"), A->HasRodInHands() && A->RodKind == ERodKind::Heavy && A->bRigInHand);
    A->ServerMoveRod_Implementation(Light, 3);
    TestTrue(TEXT("Dragged onto the hands, the light rod swaps with the heavy one"),
        A->RodKind == ERodKind::Light && Place(Heavy) == ERodPlace::Case && A->Rods[Heavy].CaseSlot == 0);
    int32 InHands = 0;
    for (const FLakeRodSlot& Slot : A->Rods) InHands += Slot.Place == ERodPlace::Hands;
    TestEqual(TEXT("Only one rod is ever in the hands"), InHands, 1);
    A->ServerMoveRod_Implementation(Medium, 0);
    TestTrue(TEXT("Inside the case two rods change compartments"), A->Rods[Medium].CaseSlot == 0 && A->Rods[Heavy].CaseSlot == 1);
    TestTrue(TEXT("The case holds the other two in different compartments"), A->Rods[Medium].CaseSlot != A->Rods[Heavy].CaseSlot);

    // Walking away closes it; put on the back it is shut.
    A->SetActorLocation(OnLand(150 + 900) + FVector(0, 0, 95));
    A->UpdateParkedRods(0.1f);
    TestFalse(TEXT("Walking away closes the case"), A->bCaseOpen);
    A->SetActorLocation(OnLand(150) + FVector(0, 0, 95));
    A->ServerCaseAction_Implementation(0);
    A->ServerCaseAction_Implementation(2);
    TestTrue(TEXT("Put on the back, shut"), A->CasePlace == ECasePlace::Back && !A->bCaseOpen);

    // R held: the rod laid on the ground, then picked up; not with the tackle in the water.
    A->ServerLayRod_Implementation(OnLand(150 + 80, 60), Yaw);
    TestTrue(TEXT("The rod is laid on the ground"), Place(Light) == ERodPlace::Ground && !A->HasRodInHands());
    A->SetActorLocation(OnLand(150 + 900) + FVector(0, 0, 95));
    A->ServerTakeRod_Implementation(Light);
    TestTrue(TEXT("A rod on the ground is not taken from afar"), Place(Light) == ERodPlace::Ground);
    A->SetActorLocation(OnLand(150) + FVector(0, 0, 95));
    A->ServerTakeRod_Implementation(Light);
    TestTrue(TEXT("Picked up, the rig in the free hand"), A->HasRodInHands() && A->RodKind == ERodKind::Light && A->bRigInHand);
    FLakeFishingFixture::ImmersedRig(A);
    A->ServerLayRod_Implementation(OnLand(150 + 80, 60), Yaw);
    TestTrue(TEXT("Not onto the ground with the tackle in the water"), A->HasRodInHands() && A->FishingState == EFishingState::Waiting);
    World->DestroyWorld(false);
    return true;
}

// Three rods out at once: cast, onto its stand, the next from the case. Each stand rod fishes on
// its own: a fish tries or takes one bait while the others wait; a take not struck is let go and the
// rig fishes on; RMB beside the stand takes that rod (its fish with it) and strikes; a fish looking
// for food finds a bait on a stand by itself.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeThreeRodsTest, "QuietLake.Fishing.ThreeRods", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeThreeRodsTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    FVector2D Facing;
    const FVector2D Edge = LakeMap::StartSpot(&Facing) + Facing * 90;
    const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X));
    const FVector Stand(Edge, 100);
    ALakeCharacter* A = World->SpawnActor<ALakeCharacter>(Stand, FRotator(0, Yaw, 0));
    APlayerController* PC = World->SpawnActor<APlayerController>();
    PC->Possess(A);
    const int32 Light = (int32)ERodKind::Light, Medium = (int32)ERodKind::Medium, Heavy = (int32)ERodKind::Heavy;
    // Each cast a little further round, so the floats lie apart.
    auto CastAndStand = [&](float Turn)
    {
        A->SetActorRotation(FRotator(0, Yaw + Turn, 0));
        PC->SetControlRotation(FRotator(0, Yaw + Turn, 0));
        FLakeFishingFixture::ImmersedRig(A);
        A->ServerToggleRod_Implementation();
    };
    const FVector2D Behind = Edge - Facing * 120;
    A->ServerPlaceCase_Implementation(FVector(Behind, LakeMap::Ground(Behind.X, Behind.Y)), Yaw + 90);
    CastAndStand(-25);
    TestTrue(TEXT("The first rod fishes on its stand, the hands are free"), A->IsParkedRig(Medium) && A->HasBaitInWater(Medium) && !A->HasRodInHands());
    A->ServerCaseAction_Implementation(0);
    A->ServerMoveRod_Implementation(Light, 3);
    CastAndStand(0);
    A->ServerMoveRod_Implementation(Heavy, 3);
    CastAndStand(25);
    int32 Fishing = 0;
    for (int32 Rod = 0; Rod < ALakeCharacter::NumRods; ++Rod) Fishing += A->IsParkedRig(Rod) && A->HasBaitInWater(Rod);
    TestEqual(TEXT("Three rods fish on their stands"), Fishing, 3);
    TestTrue(TEXT("Each on its own stand"), FVector::Dist2D(A->Rods[Light].Base, A->Rods[Medium].Base) > 40 && FVector::Dist2D(A->Rods[Light].Base, A->Rods[Heavy].Base) > 40
        && FVector::Dist2D(A->Rods[Medium].Base, A->Rods[Heavy].Base) > 40);
    TestTrue(TEXT("Three floats apart on the water"), FVector::Dist2D(A->BaitLocation(Light), A->BaitLocation(Medium)) > 100 && FVector::Dist2D(A->BaitLocation(Light), A->BaitLocation(Heavy)) > 100);

    // One bait taken, the others wait; a fish may try another one at the same time.
    ALakeFish* Taker = SpawnFish(World, EFishSpecies::Perch, 0.3f, A->BaitLocation(Light));
    Taker->TargetRig = Light;
    A->OnFishTake(Taker);
    TestTrue(TEXT("A take on one stand leaves the others waiting"),
        A->Rods[Light].State == EFishingState::Bite && A->Rods[Medium].State == EFishingState::Waiting && A->Rods[Heavy].State == EFishingState::Waiting);
    ALakeFish* Trier = SpawnFish(World, EFishSpecies::Roach, 0.2f, A->BaitLocation(Heavy));
    Trier->TargetRig = Heavy;
    TestTrue(TEXT("Another fish may try a bait on another stand"), A->ClaimBait(Trier) && A->Rods[Heavy].BaitFish.Get() == Trier);
    A->ReleaseBaitClaim(Trier);
    TestTrue(TEXT("and let it be"), !A->Rods[Heavy].BaitFish.IsValid());
    // Not struck in time: the fish lets go, the rig fishes on.
    A->Rods[Light].EscapeAt = A->ServerNow() - 1;
    A->UpdateParkedRods(0.05f);
    TestTrue(TEXT("A take not struck: the fish lets go, the rig fishes on"), A->Rods[Light].State == EFishingState::Waiting && A->HasBaitInWater(Light) && Taker->IsFree());

    // RMB beside the stand with a take: that rod comes to the hands, its fish on the bait, and strikes.
    ALakeFish* Second = SpawnFish(World, EFishSpecies::Perch, 0.3f, A->BaitLocation(Light));
    Second->TargetRig = Light;
    A->OnFishTake(Second);
    A->SetActorLocation(FVector(A->Rods[Light].Base) + FVector(0, 0, 95) - FRotator(0, Yaw, 0).Vector() * 60);
    A->NextActionTime = 0;
    A->ServerCancel_Implementation();
    TestTrue(TEXT("RMB beside the stand takes the rod with the take on"), A->HasRodInHands() && A->RodKind == ERodKind::Light && A->FishingState == EFishingState::Bite && A->BaitFish.Get() == Second);
    TestTrue(TEXT("and strikes"), A->StrikeUntil > 0);
    TestTrue(TEXT("The other two go on fishing"), A->HasBaitInWater(Medium) && A->HasBaitInWater(Heavy));
    A->Escape();
    A->ServerToggleRod_Implementation();
    TestTrue(TEXT("Back on its stand, fishing"), A->IsParkedRig(Light) && A->HasBaitInWater(Light));

    // A perch looking for food finds one of the baits on the stands and takes it by itself.
    ALakeFish* Perch = SpawnFish(World, EFishSpecies::Perch, 0.35f, A->BaitLocation(Medium) + FVector(320, 120, 0));
    bool bTaken = false;
    for (int32 Step = 0; Step < 6000 && !bTaken; ++Step)
    {
        Perch->Simulate(0.05f);
        A->UpdateParkedRods(0.05f);
        for (const FLakeRodSlot& Slot : A->Rods) bTaken |= Slot.State == EFishingState::Bite;
    }
    TestTrue(TEXT("A perch finds a bait on a stand by itself and takes it"), bTaken);
    World->DestroyWorld(false);
    return true;
}
// A new match starts with empty hands: the rods are packed in the case on the back, and the first rod
// comes out when the angler takes it (the game stows them a moment after the pawn is possessed).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeStartLoadoutTest, "QuietLake.Fishing.StartLoadout", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeStartLoadoutTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    FVector2D Facing;
    const FVector2D Edge = LakeMap::StartSpot(&Facing) + Facing * 120;
    const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X));
    ALakeCharacter* A = World->SpawnActor<ALakeCharacter>(FVector(Edge, LakeMap::Ground(Edge.X, Edge.Y) + 95), FRotator(0, Yaw, 0));
    APlayerController* PC = World->SpawnActor<APlayerController>();
    PC->Possess(A);
    A->StowRodsInCase();
    const int32 Light = (int32)ERodKind::Light, Medium = (int32)ERodKind::Medium, Heavy = (int32)ERodKind::Heavy;
    TestFalse(TEXT("Empty hands at the start"), A->HasRodInHands());
    TestTrue(TEXT("All three rods are in the case"), A->Rods[Light].Place == ERodPlace::Case
        && A->Rods[Medium].Place == ERodPlace::Case && A->Rods[Heavy].Place == ERodPlace::Case);
    TestTrue(TEXT("Each in its own compartment"), A->Rods[Light].CaseSlot == 0 && A->Rods[Medium].CaseSlot == 1
        && A->Rods[Heavy].CaseSlot == 2);
    TestTrue(TEXT("The case is on the back and shut"), A->CasePlace == ECasePlace::Back && !A->bCaseOpen);
    TestTrue(TEXT("No tackle in the water, nothing is cast"), A->FishingState == EFishingState::Idle && !A->bCharging && !A->bRigInHand);
    World->DestroyWorld(false);
    return true;
}
#endif
