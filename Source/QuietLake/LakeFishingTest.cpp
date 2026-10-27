#if WITH_DEV_AUTOMATION_TESTS
#include "LakeGame.h"
#include "LakeRod.h"
#include "LakeFish.h"
#include "LakeMap.h"
#include "LakeLife.h"
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
    // A cast is LMB held for the full sweep and released; the bait fishes once the float lands.
    auto Cast = [Fisher]()
    {
        Fisher->NextActionTime = 0;
        Fisher->ServerFish_Implementation();
        Fisher->ChargeServerStart -= ALakeCharacter::ChargeTime;
        Fisher->ServerRelease_Implementation();
        Fisher->BaitActiveTime = 0;
    };
    // Lifting the rig out takes a few seconds; skip to its end.
    auto Settle = [Fisher]()
    {
        if (!Fisher->bRetrieving) return;
        Fisher->RetrieveServerTime -= ALakeCharacter::RetrieveTime;
        Fisher->UpdateRetrieve();
    };
    Controller->SetControlRotation(FRotator(0, FacingYaw + 180, 0));
    Cast();
    TestEqual(TEXT("Cast toward land rejected"), Fisher->FishingState, 0);
    TestFalse(TEXT("Backswing ends after a rejected cast"), Fisher->bCharging);
    Controller->SetControlRotation(FRotator(0, FacingYaw, 0));
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    TestTrue(TEXT("Pressing LMB starts the sweep"), Fisher->bCharging && Fisher->FishingState == 0);
    Fisher->ChargeServerStart -= ALakeCharacter::ChargeTime * 0.5f;
    Fisher->ServerRelease_Implementation();
    const float HalfCast = FVector::Dist2D(Fisher->FloatPosition, Fisher->GetActorLocation());
    Fisher->BaitActiveTime = 0;
    TestEqual(TEXT("Cast toward water accepted"), Fisher->FishingState, 1);
    TestTrue(TEXT("Bait hangs in the water"), ALakeEnvironment::IsWater(Fisher->BaitLocation()) && Fisher->BaitLocation().Z < LakeFish::SurfaceZ);
    Fisher->ServerFish_Implementation();
    TestEqual(TEXT("Rapid repeated input ignored"), Fisher->FishingState, 1);
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    TestEqual(TEXT("Early strike gives no catch"), Fisher->CatchCount, 0);
    TestTrue(TEXT("Early strike lifts the rig out slowly"), Fisher->bRetrieving && Fisher->FishingState == 1 && !Fisher->HasBaitInWater());
    Settle();
    TestEqual(TEXT("Early strike ends cast"), Fisher->FishingState, 0);
    Cast();
    const float FullCast = FVector::Dist2D(Fisher->FloatPosition, Fisher->GetActorLocation());
    TestTrue(FString::Printf(TEXT("Holding LMB longer casts farther (%.0f vs %.0f cm)"), FullCast, HalfCast), FullCast > HalfCast + 100 && FullCast < 1000);
    Fisher->ServerCancel_Implementation();
    Settle();

    // A real perch finds the worm on its own, tests it and takes it.
    Cast();
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
        if (Step % 40 == 0 && IsValid(Perch))
            AddInfo(FString::Printf(TEXT("t=%.0f dist=%.0f fishZ=%.0f tension=%.2f pull=%d stamina=%.2f"), Step * 0.05f, Fisher->FishDistance, Perch->GetActorLocation().Z, Fisher->Tension, Fisher->bReeling, Perch->StaminaFraction()));
        if (IsValid(Perch)) Perch->Simulate(0.05f);
    }
    AddInfo(TEXT("Fight ended: ") + Fisher->Status);
    Settle();
    TestEqual(TEXT("Controlled reeling lands the perch"), Fisher->CatchCount, 1);
    TestEqual(TEXT("Successful catch ends cast"), Fisher->FishingState, 0);

    // The same careful play takes longer with a heavy fish than with a light one of its species.
    auto FightSeconds = [&](float Weight)
    {
        Cast();
        TestEqual(TEXT("Cast for the crucian fight"), Fisher->FishingState, 1);
        for (int32 Pose = 0; Pose < 60; ++Pose) Fisher->UpdateRodPose(0.05f);   // the rod settles to the waiting pose
        ALakeFish* Fish = SpawnFish(World, EFishSpecies::Crucian, Weight, Fisher->BaitLocation());
        Fisher->OnFishTake(Fish);
        Fisher->NextActionTime = 0;
        Fisher->ServerFish_Implementation();
        int32 Step = 0;
        for (; Step < 2400 && Fisher->FishingState == 3; ++Step)
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
    TestEqual(TEXT("Take sinks the bobber"), Fisher->FishingState, 2);
    Fisher->Escape();
    Settle();
    TestEqual(TEXT("Missed take awards no fish"), Fisher->CatchCount, 1);
    TestEqual(TEXT("Missed take ends cast"), Fisher->FishingState, 0);
    Cast();
    Fisher->ServerCancel_Implementation();
    TestTrue(TEXT("Cancel starts lifting the rig"), Fisher->bRetrieving);
    Settle();
    TestEqual(TEXT("Cancel ends cast"), Fisher->FishingState, 0);

    // A big pike against a reel that is never released breaks the line.
    Cast();
    ALakeFish* Pike = SpawnFish(World, EFishSpecies::Pike, 3.2f, Fisher->BaitLocation());
    Fisher->OnFishTake(Pike);
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    for (int32 Step = 0; Step < 600 && Fisher->FishingState == 3; ++Step) { Fisher->UpdateFight(0.05f); Pike->Simulate(0.05f); }
    TestEqual(TEXT("Continuous reeling against a pike breaks the line"), Fisher->FishingState, 0);
    TestEqual(TEXT("Broken line awards no fish"), Fisher->CatchCount, 1);
    TestFalse(TEXT("Pike is free again"), Pike->IsHooked());

    // Never reeling: the fight times out.
    Cast();
    ALakeFish* Crucian = SpawnFish(World, EFishSpecies::Crucian, 0.5f, Fisher->BaitLocation());
    Fisher->OnFishTake(Crucian);
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    Fisher->ServerRelease_Implementation();
    for (int32 Step = 0; Step < 3000 && Fisher->FishingState == 3; ++Step) { Fisher->UpdateFight(0.05f); Crucian->Simulate(0.05f); }
    TestEqual(TEXT("Abandoned fight times out"), Fisher->FishingState, 0);
    TestEqual(TEXT("Timeout awards no fish"), Fisher->CatchCount, 1);

    // Fixed line: walking away from the water drags the float after the angler.
    Fisher->SetActorLocation(Back(0));
    Cast();
    for (int32 Step = 0; Step < 40; ++Step) Fisher->UpdateRodPose(0.05f);
    Fisher->ConstrainFloat();
    const FVector FloatBefore = Fisher->FloatPosition;
    TestTrue(TEXT("The float lands within reach of the 6 m line"), FVector::Dist2D(FloatBefore, Fisher->GetActorLocation()) < 1200);
    TestTrue(TEXT("Walking with the line out is allowed"), Fisher->CanWalk());
    Fisher->SetActorLocation(Back(200));
    for (int32 Step = 0; Step < 40; ++Step) Fisher->UpdateRodPose(0.05f);
    Fisher->ConstrainFloat();
    TestTrue(TEXT("Stepping back drags the float towards the bank"), Fisher->FishingState == 0 || FVector2D::DotProduct(FVector2D(Fisher->FloatPosition - FloatBefore), Facing) < -100);
    Fisher->SetActorLocation(Back(1140));
    for (int32 Step = 0; Step < 40; ++Step) Fisher->UpdateRodPose(0.05f);
    if (Fisher->FishingState == 1) Fisher->ConstrainFloat();
    Settle();
    TestEqual(TEXT("Walking far inland pulls the rig out of the water"), Fisher->FishingState, 0);
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
    TestEqual(TEXT("Deep cast accepted"), Fisher->FishingState, 1);
    for (int32 Step = 0; Step < 60; ++Step) Fisher->UpdateRodPose(0.05f);
    Fisher->ConstrainFloat();
    TestTrue(FString::Printf(TEXT("Float reaches the water with a 1 m main line (tip %.0f cm above it)"), Fisher->RodTip().Z - LakeFish::SurfaceZ),
        Fisher->FishingState == 1 && Fisher->RodTip().Z - LakeFish::SurfaceZ < ALakeRod::LineLength - Fisher->HookDepth);
    Fisher->ServerCancel_Implementation();
    Settle();
    for (int32 I = 0; I < 1000; ++I) Fisher->ServerChangeDepth_Implementation(-1);
    TestEqual(TEXT("Depth has a minimum"), Fisher->FloatDepth, ALakeCharacter::MinDepth);
    Fisher->FloatDepth = 50;
    Cast();
    TestTrue(TEXT("Bait hangs at the set depth"), FMath::IsNearlyEqual(Fisher->BaitLocation().Z, ExpectedBaitZ(), 1.0f) && Fisher->HookDepth == 50.0f);
    Fisher->ServerChangeDepth_Implementation(1);
    TestEqual(TEXT("Depth cannot change after the cast"), Fisher->FloatDepth, 50.0f);
    Fisher->ServerCancel_Implementation();
    Settle();
    Fisher->FloatDepth = 100;

    // Rod on the stand: allowed while waiting; a take must be struck from next to the stand.
    Cast();
    Fisher->ServerToggleRod_Implementation();
    TestTrue(TEXT("Rod goes onto the stand while waiting"), Fisher->bRodOnStand);
    TestTrue(TEXT("Angler may walk away from a rod on the stand"), Fisher->CanWalk());
    ALakeFish* Taker = SpawnFish(World, EFishSpecies::Perch, 0.2f, Fisher->BaitLocation());
    Fisher->OnFishTake(Taker);
    Fisher->SetActorLocation(Fisher->StandBase + FVector(-600, 0, 100));
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    TestEqual(TEXT("Cannot strike from far away"), Fisher->FishingState, 2);
    Fisher->SetActorLocation(Fisher->StandBase + FVector(-80, 0, 100));
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    TestEqual(TEXT("Grabbing the rod from the stand strikes"), Fisher->FishingState, 3);
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
    Fisher->ChargeServerStart -= ALakeCharacter::ChargeTime;
    Fisher->ServerRelease_Implementation();
    Fisher->BaitActiveTime = 0;
    TestEqual(TEXT("Cast for the crucian"), Fisher->FishingState, 1);
    TSet<EBiteMotion> Seen;
    EBiteMotion FirstTake = EBiteMotion::None;
    for (int32 Try = 0; Try < 8 && FirstTake == EBiteMotion::None; ++Try)
    {
        ALakeFish* Crucian = SpawnFish(World, EFishSpecies::Crucian, 0.6f, Fisher->BaitLocation() + FVector(150, 0, 0));
        for (int32 Step = 0; Step < 2400 && Fisher->FishingState == 1; ++Step)
        {
            Crucian->Simulate(0.05f);
            Seen.Add(Fisher->BiteMotion);
        }
        if (Fisher->FishingState == 2) FirstTake = Fisher->BiteMotion;
        else Crucian->Destroy();
    }
    AddInfo(FString::Printf(TEXT("Crucian take shown as motion %d; the float trembled first: %d"), (int32)FirstTake, Seen.Contains(EBiteMotion::Tremble)));
    TestTrue(TEXT("The crucian tests the bait first: the float trembles"), Seen.Contains(EBiteMotion::Tremble));
    TestTrue(TEXT("Its take lifts the float or leads it off"), FirstTake == EBiteMotion::Lift || FirstTake == EBiteMotion::Drift);
    World->DestroyWorld(false);
    return true;
}
#endif
