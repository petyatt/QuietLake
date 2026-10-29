#if WITH_DEV_AUTOMATION_TESTS
#include "LakeGame.h"
#include "HAL/IConsoleManager.h"
#include "LakeRod.h"
#include "LakeFish.h"
#include "LakeMap.h"
#include "LakeLife.h"
#include "LakeWeather.h"
#include "LakeRipples.h"
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
    // Stand at the water's edge of the start opening, facing the lake; Back(cm) steps away from it.
    FVector2D Facing;
    const FVector2D Edge = LakeMap::StartSpot(&Facing) + Facing * 90;
    const float FacingYaw = FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X));
    auto Back = [&](float Distance) { return FVector(Edge - Facing * Distance, 100); };
    ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(Back(0), FRotator(0, FacingYaw, 0));
    APlayerController* Controller = World->SpawnActor<APlayerController>();
    Controller->Possess(Fisher);
    // A cast is LMB held to full strength, then released; the bait fishes once the float lands.
    auto Cast = [Fisher]()
    {
        Fisher->NextActionTime = 0;
        Fisher->ServerFish_Implementation();
        Fisher->ChargeServerStart -= Fisher->CastChargeTime();
        Fisher->ServerRelease_Implementation();
        Fisher->BaitActiveTime = 0;
        Fisher->CastServerTime -= 5;   // the test world's clock stands still: the stroke is over, the rig has landed
    };
    // Lifting the rig out takes a few seconds; skip to its end.
    auto Settle = [Fisher]()
    {
        // Lifting a fish out ends with it in the hand.
        if (Fisher->FishingState == EFishingState::Landing)
        {
            Fisher->LandServerTime -= ALakeCharacter::LandTime;
            Fisher->UpdateLanding();
            return;
        }
        if (!Fisher->bRetrieving) return;
        Fisher->RetrieveProgress = 1;
        Fisher->UpdateRetrieve(0);
    };
    // LMB after the cast, held to the end: the rig is drawn out of the water.
    auto DrawOut = [Fisher, &Settle]()
    {
        Fisher->NextActionTime = 0;
        Fisher->ServerFish_Implementation();
        Settle();
    };
    Controller->SetControlRotation(FRotator(0, FacingYaw + 180, 0));
    Cast();
    TestTrue(TEXT("Cast toward land rejected"), Fisher->FishingState == EFishingState::Idle);
    TestFalse(TEXT("Casting ends after a rejected cast"), Fisher->bCharging);
    // A bare click casts just past the water's edge.
    Controller->SetControlRotation(FRotator(0, FacingYaw, 0));
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    TestTrue(TEXT("Pressing LMB starts the cast"), Fisher->bCharging && Fisher->FishingState == EFishingState::Idle);
    Fisher->ServerRelease_Implementation();
    const float NearCast = FVector::Dist2D(Fisher->FloatPosition, Fisher->GetActorLocation());
    Controller->SetControlRotation(FRotator(0, FacingYaw, 0));
    Fisher->BaitActiveTime = 0;
    TestTrue(TEXT("Cast toward water accepted"), Fisher->FishingState == EFishingState::Waiting);
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
    const float FullCast = FVector::Dist2D(Fisher->FloatPosition, Fisher->GetActorLocation());
    TestTrue(FString::Printf(TEXT("LMB held longer casts farther (%.0f vs %.0f cm)"), FullCast, NearCast), FullCast > NearCast + 100 && FullCast < 1000);
    DrawOut();

    // A real perch finds the worm on its own, tests it and takes it.
    Cast();
    const FVector Bait = Fisher->BaitLocation();
    ALakeFish* Perch = SpawnFish(World, EFishSpecies::Perch, 0.35f, Bait + FVector(320, 120, 0));
    for (int32 Step = 0; Step < 6000 && Fisher->FishingState == EFishingState::Waiting; ++Step) Perch->Simulate(0.05f);
    TestTrue(TEXT("Perch takes the worm by itself"), Fisher->FishingState == EFishingState::Bite);
    TestEqual(TEXT("Taking the bait is not a catch"), Fisher->CatchCount, 0);
    Fisher->NextActionTime = 0;
    Fisher->ServerCancel_Implementation();
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
        Fisher->NextActionTime = 0;
        Fisher->ServerCancel_Implementation();
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
    // It comes out only while LMB is held; let go, the rod goes back down and the rig fishes on.
    Fisher->UpdateRetrieve(ALakeCharacter::RetrieveTime * 0.4f);
    const float Held = Fisher->RetrieveProgress;
    TestTrue(TEXT("Holding LMB draws the rig out"), Held > 0.35f && Held < 0.45f);
    Fisher->ServerRelease_Implementation();
    Fisher->UpdateRetrieve(ALakeCharacter::RetrieveTime * 0.1f);
    TestTrue(TEXT("Releasing LMB does not reverse the retrieval"), Fisher->bRetrieving && Fisher->RetrieveProgress > Held);
    Fisher->UpdateRetrieve(ALakeCharacter::RetrieveTime);
    TestTrue(TEXT("A single click finishes the retrieval"), !Fisher->bRetrieving && Fisher->FishingState == EFishingState::Idle);

    // A big pike against a reel that is never released breaks the line.
    Cast();
    ALakeFish* Pike = SpawnFish(World, EFishSpecies::Pike, 3.2f, Fisher->BaitLocation());
    Fisher->OnFishTake(Pike);
    Fisher->NextActionTime = 0;
    Fisher->ServerCancel_Implementation();
    Fisher->ServerFish_Implementation();
    for (int32 Step = 0; Step < 600 && Fisher->FishingState == EFishingState::Fight; ++Step) { Fisher->UpdateFight(0.05f); Pike->Simulate(0.05f); }
    TestTrue(TEXT("Continuous reeling against a pike breaks the line"), Fisher->FishingState == EFishingState::Idle);
    TestEqual(TEXT("Broken line awards no fish"), Fisher->CatchCount, 1);
    TestFalse(TEXT("Pike is free again"), Pike->IsHooked());

    // Never reeling: the fight times out.
    Cast();
    ALakeFish* Crucian = SpawnFish(World, EFishSpecies::Crucian, 0.5f, Fisher->BaitLocation());
    Fisher->OnFishTake(Crucian);
    Fisher->NextActionTime = 0;
    Fisher->ServerCancel_Implementation();
    Fisher->ServerRelease_Implementation();
    for (int32 Step = 0; Step < 3000 && Fisher->FishingState == EFishingState::Fight; ++Step) { Fisher->UpdateFight(0.05f); Crucian->Simulate(0.05f); }
    TestTrue(TEXT("Abandoned fight times out"), Fisher->FishingState == EFishingState::Idle);
    TestEqual(TEXT("Timeout awards no fish"), Fisher->CatchCount, 1);

    // Fixed line: walking away from the water drags the float after the angler.
    Fisher->SetActorLocation(Back(0));
    Cast();
    for (int32 Step = 0; Step < 40; ++Step) Fisher->UpdateRodPose(0.05f);
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

    // Rod on the stand: allowed while waiting; a take must be struck from next to the stand.
    Cast();
    Fisher->ServerToggleRod_Implementation();
    TestTrue(TEXT("Rod goes onto the stand while waiting"), Fisher->bRodOnStand);
    Fisher->ConsumeMovementInputVector();
    Fisher->Forward(1);
    TestTrue(TEXT("Angler may walk away from a rod on the stand"), !Fisher->ConsumeMovementInputVector().IsNearlyZero());
    ALakeFish* Taker = SpawnFish(World, EFishSpecies::Perch, 0.2f, Fisher->BaitLocation());
    Fisher->OnFishTake(Taker);
    Fisher->SetActorLocation(Fisher->StandBase + FVector(-600, 0, 100));
    Fisher->NextActionTime = 0;
    Fisher->ServerCancel_Implementation();
    TestTrue(TEXT("Cannot strike from far away"), Fisher->FishingState == EFishingState::Bite);
    Fisher->SetActorLocation(Fisher->StandBase + FVector(-80, 0, 100));
    Fisher->NextActionTime = 0;
    Fisher->ServerCancel_Implementation();
    TestTrue(TEXT("Grabbing the rod from the stand strikes"), Fisher->FishingState == EFishingState::Fight);
    TestFalse(TEXT("Rod is back in the hands"), Fisher->bRodOnStand);
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
    int32 CrucianLifts = 0, CrucianOffBottomLifts = 0, RoachWithPecks = 0, PerchFastSinks = 0;
    float CrucianBefore = 0, PerchBefore = 0, SmallTries = 0, BigTries = 0, EagerWindow = 0, SluggishWindow = 0;
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
        RoachWithPecks += Roach.ContainsByPredicate([](const FBiteStep& S) { return S.Motion == EBiteMotion::Peck && !S.bTake; });
        const FSummary Perch = Summarise(MakeBite(EFishSpecies::Perch, 0.5f, 1.0f, true, false, R));
        PerchFastSinks += Perch.Take == EBiteMotion::Sink && Perch.TakeAmount >= 45;
        PerchBefore += Perch.Before / N;
    }
    AddInfo(FString::Printf(TEXT("Crucian: lifts %d/%d from the bottom, %d off it; %.1f s of testing; tries small %.1f, big %.1f; take window eager %.1f s, sluggish %.1f s"),
        CrucianLifts, N, CrucianOffBottomLifts, CrucianBefore, SmallTries, BigTries, EagerWindow, SluggishWindow));
    AddInfo(FString::Printf(TEXT("Perch: %.1f s before the take, fast sinks %d/%d; roach with pecks %d/%d"), PerchBefore, PerchFastSinks, N, RoachWithPecks, N));
    TestTrue(TEXT("A crucian with the bait on the bottom usually lifts the float"), CrucianLifts > N * 0.7f);
    TestEqual(TEXT("No lift when the bait hangs off the bottom"), CrucianOffBottomLifts, 0);
    TestTrue(TEXT("Small crucian keep trembling the float longer than big ones"), SmallTries > BigTries + 1.0f);
    TestTrue(TEXT("A sluggish crucian lets go sooner"), SluggishWindow < EagerWindow * 0.8f);
    TestTrue(TEXT("Roach peck before taking"), RoachWithPecks == N);
    TestTrue(TEXT("Perch pull the float under sharply"), PerchFastSinks == N);
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
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    Fisher->ChargeServerStart -= ALakeCharacter::CastLoad;
    Fisher->ServerRelease_Implementation();
    Fisher->BaitActiveTime = 0;
    TestTrue(TEXT("Cast for the crucian"), Fisher->FishingState == EFishingState::Waiting);
    TSet<EBiteMotion> Seen;
    EBiteMotion FirstTake = EBiteMotion::None;
    for (int32 Try = 0; Try < 8 && FirstTake == EBiteMotion::None; ++Try)
    {
        ALakeFish* Crucian = SpawnFish(World, EFishSpecies::Crucian, 0.6f, Fisher->BaitLocation() + FVector(150, 0, 0));
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
    World->DestroyWorld(false);
    return true;
}

// Casting from the hand: the rod tip must be loaded first; the float goes where the angler looks.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeCastingTest, "QuietLake.Fishing.Casting", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeCastingTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    FVector2D Facing;
    const FVector2D Edge = LakeMap::StartSpot(&Facing) + Facing * 90;
    const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X));
    ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(FVector(Edge, 100), FRotator(0, Yaw, 0));
    APlayerController* Controller = World->SpawnActor<APlayerController>();
    Controller->Possess(Fisher);
    // LMB held for Held seconds while looking Pitch degrees down: how far out the float went.
    auto Throw = [&](float Held, float Pitch)
    {
        Controller->SetControlRotation(FRotator(Pitch, Yaw, 0));
        Fisher->NextActionTime = 0;
        Fisher->ServerFish_Implementation();
        Fisher->ChargeServerStart -= Held;
        Fisher->ServerRelease_Implementation();
        Fisher->BaitActiveTime = 0;
        return FVector::Dist2D(Fisher->FloatPosition, Fisher->GetActorLocation());
    };
    auto LiftOut = [&]()
    {
        Fisher->NextActionTime = 0;
        Fisher->ServerFish_Implementation();
        if (Fisher->bRetrieving) { Fisher->RetrieveProgress = 1; Fisher->UpdateRetrieve(0); }
    };
    const FVector Ahead(Facing, 0);
    // The cast is automatic: the longer LMB is held, the farther it goes, up to the line's reach.
    const float Full = Throw(Fisher->CastChargeTime(), 0);
    TestTrue(TEXT("A full cast lands in the water"), Fisher->FishingState == EFishingState::Waiting && Fisher->CastResult == ECastResult::Good && Fisher->HasBaitInWater());
    TestTrue(TEXT("The hook lands beyond the float"), FVector::DotProduct(FVector(Fisher->CastHookLanding) - Fisher->FloatPosition, Ahead) > 30);
    TestTrue(FString::Printf(TEXT("A full cast goes as far as the line reaches (%.0f of %.0f cm)"), Full, Fisher->CastReach()), FMath::Abs(Full - Fisher->CastReach()) < 60);
    LiftOut();
    const float Longer = Throw(Fisher->CastChargeTime() * 5, 0);
    TestTrue(FString::Printf(TEXT("Holding on does not cast beyond the line (%.0f cm)"), Longer), FMath::Abs(Longer - Full) < 1);
    LiftOut();
    const float Half = Throw(Fisher->CastChargeTime() * 0.5f, 0);
    TestTrue(FString::Printf(TEXT("Held half as long, it goes less far (%.0f vs %.0f cm)"), Half, Full), Half < Full - 100);
    LiftOut();
    const float Click = Throw(0, 0);
    TestTrue(FString::Printf(TEXT("A bare click casts just past the edge (%.0f cm)"), Click), Fisher->FishingState == EFishingState::Waiting && Click < Half - 50);
    LiftOut();
    const float Down = Throw(Fisher->CastChargeTime(), -40);
    TestTrue(FString::Printf(TEXT("Looking down does not shorten it: the hold decides (%.0f cm)"), Down), FMath::Abs(Down - Full) < 150);
    LiftOut();
    World->DestroyWorld(false);
    return true;
}

// Wind: it drifts a float whose bait hangs free downwind until the line holds it; a rig on the bottom stays put.
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
        Fisher->NextActionTime = 0;
        Fisher->ServerFish_Implementation();
        Fisher->ChargeServerStart -= Fisher->CastChargeTime() * 0.6f;
        Fisher->ServerRelease_Implementation();
        Fisher->BaitActiveTime = 0;
        Fisher->CastServerTime -= 5;
        for (int32 Pose = 0; Pose < 60; ++Pose) Fisher->UpdateRodPose(0.05f);
    };
    auto Blow = [&](float Seconds)
    {
        for (float T = 0; T < Seconds; T += 0.1f) { Fisher->DriftFloat(0.1f); Fisher->ConstrainFloat(); }
    };
    auto LiftOut = [&]()
    {
        Fisher->ServerCancel_Implementation();
        if (Fisher->bRetrieving) { Fisher->RetrieveProgress = 1; Fisher->UpdateRetrieve(0); }
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
// Exercise the actual input and rod simulation, rather than DoCast's no-rod rules fixture.
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

    // The automatic cast with the real rod: LMB held to full strength, let go; the rig flies on an arc
    // to the place picked for it and comes down there, at any frame rate, also walking.
    auto Flight = [&](float Fps, bool bWalk = false)
    {
        Fisher->FinishFishing(TEXT("test"));
        Fisher->SwingPitch = Fisher->SwingYaw = 0;
        Fisher->bRigInHand = true;
        Fisher->bRodPoseReady = false;
        Fisher->SetActorLocation(Place);
        PC->SetControlRotation(FRotator(-12, Yaw, 0));
        if (Fisher->RodVisual) Fisher->RodVisual->Destroy();
        Fisher->RodVisual = World->SpawnActor<ALakeRod>();
        Fisher->HookDepth = Fisher->FloatDepth = 100;
        const float Dt = 1 / Fps;
        bool bMoving = false;
        auto Step = [&]
        {
            if (bMoving) Fisher->SetActorLocation(Fisher->GetActorLocation() + Fisher->GetActorRightVector() * 100 * Dt);
            Fisher->UpdateRodPose(Dt);
            Fisher->UpdateVisuals(Dt);
            // (The test world's clock stands still: the flight's own clock is moved on.)
            if (Fisher->bRigFlying) Fisher->CastServerTime -= Dt;
        };
        for (int32 I = 0; I < Fps * 2; ++I) Step();
        Fisher->StartCast();
        Fisher->ChargeServerStart -= Fisher->CastChargeTime();
        bMoving = bWalk;
        for (int32 I = 0; I < Fps * 0.6f; ++I) Step();
        const FVector Held = Fisher->RodVisual->FloatKeel();
        if (bWalk) TestTrue(FString::Printf(TEXT("The held rig follows the walking hand (%.1f cm)"), FVector::Dist(Fisher->RodVisual->ShotLocation(), Fisher->HandPoint(0))), FVector::Dist(Fisher->RodVisual->ShotLocation(), Fisher->HandPoint(0)) < 30);
        Fisher->ServerReleaseWithAim_Implementation(Fisher->SwingPitch, 0);
        TestTrue(TEXT("Release starts the cast's flight, with no bait active yet"), Fisher->bRigFlying && !Fisher->HasBaitInWater());
        TestTrue(TEXT("Release does not teleport the rig"), FVector::Dist(Held, Fisher->RodVisual->FloatKeel()) < 0.01f);
        const FVector Target = Fisher->FloatPosition;
        const float BeforeMove = PC->GetControlRotation().Yaw;
        Fisher->Turn(4);
        TestTrue(TEXT("Mouse still turns the rod with the camera after release"), FMath::FindDeltaAngleDegrees(BeforeMove, PC->GetControlRotation().Yaw) > 1);
        Fisher->Turn(-4);
        float Highest = Held.Z;
        for (int32 I = 0; I < Fps * (Fisher->CastFlight + 0.2f); ++I)
        {
            Step();
            Highest = FMath::Max(Highest, (float)Fisher->RodVisual->FloatKeel().Z);
        }
        TestTrue(TEXT("The underhand cast does not climb above the release point"), Highest <= Held.Z + 25);
        const FVector Result = Fisher->RodVisual->FloatKeel();
        if (bWalk) TestTrue(TEXT("The character moves throughout the cast"), FVector::Dist(Fisher->GetActorLocation(), Place) > 80);
        TestFalse(TEXT("The flight stays finite"), Result.ContainsNaN());
        TestTrue(TEXT("The fixed line holds the float within reach"), FVector::Dist(Result, Fisher->RodVisual->Tip()) <= ALakeRod::LineLength - Fisher->HookDepth + 10);
        TestTrue(FString::Printf(TEXT("The cast comes down where it was aimed (%.0f cm off)"), FVector::Dist2D(Result, Target)), FVector::Dist2D(Result, Target) < 80 && (bWalk || Fisher->RodVisual->FloatDown()));   // (walking on, the angler lifts it again)
        const float AimPitch = Fisher->SwingPitch, AimYaw = Fisher->SwingYaw;
        Fisher->OnRigLanded(Result);
        TestTrue(TEXT("Splashdown preserves the hand aim"), Fisher->SwingPitch == AimPitch && Fisher->SwingYaw == AimYaw);
        TestEqual(TEXT("With the rig in the water the rod is held lower"), Fisher->RodBase(), Fisher->RodWaitAngle);
        const float BeforeWaitingMove = PC->GetControlRotation().Yaw;
        Fisher->Turn(-4);
        TestTrue(TEXT("The rod remains steerable with the rig in the water"), FMath::FindDeltaAngleDegrees(BeforeWaitingMove, PC->GetControlRotation().Yaw) < -1);
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_CONTROL fps=%.0f held=%s target=%s flight=%s bend=%.1f"), Fps, *Held.ToCompactString(), *Target.ToCompactString(), *Result.ToCompactString(), Fisher->RodVisual->TipBend());
        if (!bWalk)
        {
            Fisher->NextActionTime = 0;
            Fisher->BaitActiveTime = -100;
            Fisher->ServerFish_Implementation();
            Fisher->ServerRelease_Implementation();
            float MostRight = -1000;
            for (int32 I = 0; I < Fps * 8; ++I)
            {
                Fisher->Tick(Dt);
                const FVector LocalShot = Fisher->RodVisual->ShotLocation() - Fisher->GetActorLocation();
                if (LocalShot.Size2D() < 220)
                    MostRight = FMath::Max(MostRight, (float)FVector::DotProduct(LocalShot, Fisher->GetActorRightVector()));
            }
            TestTrue(TEXT("Retrieval finishes in the hand after a single click"), Fisher->bRigInHand && !Fisher->bRetrieving && Fisher->FishingState == EFishingState::Idle);
            TestTrue(FString::Printf(TEXT("Rig is at the left hand (%.1f cm)"), FVector::Dist(Fisher->RodVisual->ShotLocation(), Fisher->HandPoint(0))), FVector::Dist(Fisher->RodVisual->ShotLocation(), Fisher->HandPoint(0)) < 10);
            TestTrue(TEXT("Rig does not return over the right shoulder"), MostRight < 10);
        }
        return Result;
    };
    const FVector Base = Flight(60);
    Flight(60, true);
    for (float Fps : { 30.0f, 120.0f })
    {
        const FVector AtRate = Flight(Fps);
        TestTrue(FString::Printf(TEXT("The cast is the same at %.0f fps"), Fps), FVector::Dist2D(AtRate, Base) < 60);
    }
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
            S.Name, SmallTime, SmallMin, SmallRate, SmallStep, BigTime, BigMin, BigRate));
        TestTrue(FString::Printf(TEXT("%s comes round"), S.Name), SmallTime > 0 && BigTime > 0);
        TestTrue(FString::Printf(TEXT("%s turns in an arc, not on the spot"), S.Name), SmallMin > 0.2f * S.CruiseSpeed && BigMin > 0.2f * S.CruiseSpeed);
        TestTrue(FString::Printf(TEXT("%s turn rate builds up gradually"), S.Name), SmallStep <= G.TurnAccel * 1.25f * 1.01f && SmallRate <= S.TurnRate * 1.25f * 1.01f);
        TestTrue(FString::Printf(TEXT("A big %s turns more slowly"), S.Name), BigTime > SmallTime * 1.15f);
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
    TestTrue(TEXT("A big crucian is deeper-bodied than a small one"), Girth(EFishSpecies::Crucian, 0.9f) > Girth(EFishSpecies::Crucian, 0.15f) + 0.1f);
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
    return true;
}
#endif
