#if WITH_DEV_AUTOMATION_TESTS
#include "LakeTestCommon.h"

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
        Crucian->TestHunger() = 1; Crucian->TestEnergy() = 1;
        Crucian->TestDecide();
        Feeds += Crucian->GetGoal() == EFishGoal::Feed;
        Crucian->TestHunger() = 0.05f; Crucian->TestEnergy() = 0.1f;
        Crucian->TestDecide();
        Rests += Crucian->GetGoal() == EFishGoal::Rest;
    }
    TestTrue(FString::Printf(TEXT("Hungry fish feed (%d/20), tired full ones rest (%d/20)"), Feeds, Rests), Feeds >= 18 && Rests >= 18);

    // A feeding crucian roots in the silt: bubbles come up, and it gets full.
    Crucian->TestHunger() = 1; Crucian->TestEnergy() = 1;
    Crucian->TestSetGoal(EFishGoal::Feed, 600);
    Run(180);   // the spot it picks may be a couple of minutes' swim away at a crucian's pace
    AddInfo(FString::Printf(TEXT("Crucian after 3 min of feeding: hunger %.2f, bubbles %d; at %s target %s, goal %d, at spot %d, water %.0f cm, state %d"),
        Crucian->GetHunger(), Life->Stats().Signs[(int32)ELakeSign::Bubbles], *Crucian->GetActorLocation().ToString(), *Crucian->TestRoamTarget().ToString(),
        (int32)Crucian->GetGoal(), Crucian->TestbAtSpot(), LakeMap::Depth(Crucian->GetActorLocation().X, Crucian->GetActorLocation().Y), (int32)Crucian->TestState()));
    TestTrue(TEXT("A rooting crucian sends up bubbles"), Life->Stats().Signs[(int32)ELakeSign::Bubbles] > 0);
    TestTrue(TEXT("Feeding fills the fish"), Crucian->GetHunger() < 0.8f);

    // Resting restores its strength.
    Crucian->TestEnergy() = 0.15f; Crucian->TestHunger() = 0;
    Crucian->TestSetGoal(EFishGoal::Rest, 600);
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
    if (Leader) { Leader->TestEnergy() = 0.2f; Leader->TestSetGoal(EFishGoal::Rest, 600); }
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
    for (ALakeFish* Fish : Perch) { Fish->TestHunger() = 1; Fish->TestEnergy() = 1; }
    Life->Step(0);
    if (ALakeFish* PerchLead = Life->SchoolLeader(8)) PerchLead->TestSetGoal(EFishGoal::Feed, 600);
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
    Pike->TestHunger() = 1;
    Pike->TestSetGoal(EFishGoal::Feed, 600);
    for (int32 Wait = 0; Wait < 18 && !Pike->TestbAtSpot(); ++Wait)
    {
        Run(5);
    }
    AddInfo(FString::Printf(TEXT("Pike at %s target %s, water %.0f cm, shore %.0f cm, goal %d, state %d"), *Pike->GetActorLocation().ToString(), *Pike->TestRoamTarget().ToString(),
        LakeMap::Depth(Pike->GetActorLocation().X, Pike->GetActorLocation().Y), LakeMap::ShoreDistance(Pike->GetActorLocation().X, Pike->GetActorLocation().Y), (int32)Pike->GetGoal(), (int32)Pike->TestState()));
    TestTrue(TEXT("The pike settles in its ambush"), Pike->TestbAtSpot());
    const int32 StrikesBefore = Life->Stats().PikeStrikes;
    for (int32 Try = 0; Try < 6 && Life->Stats().PikeStrikes == StrikesBefore && IsValid(Pike); ++Try)
    {
        const FVector Front = Pike->MouthLocation() + Pike->GetActorForwardVector() * 90;
        Spawn(EFishSpecies::Roach, 0.12f, FVector(Front.X, Front.Y, FMath::Max(Front.Z, ALakeFish::BottomZ(Front.X, Front.Y) + 10)));
        Pike->TestNextLook() = 0;
        Run(4);
    }
    AddInfo(FString::Printf(TEXT("Pike: %d strikes, %d fish eaten"), Life->Stats().PikeStrikes, Life->Stats().Eaten));
    TestTrue(TEXT("The pike strikes from ambush"), Life->Stats().PikeStrikes > StrikesBefore);

    TestTrue(TEXT("Every fish stays in the water"), bInWater);
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

// Wind waves on the lake (engine Water Gerstner waves): their energy matches the sea state a wind
// raises over the lake's fetch, a calm leaves no waves, and the big waves run with the wind.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeWavesTest, "QuietLake.Fishing.Waves", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeWavesTest::RunTest(const FString& Parameters)
{
    auto Energy = [](const TArray<FGerstnerWave>& Waves)
    {
        float Sum = 0;
        for (const FGerstnerWave& W : Waves) Sum += W.Amplitude * W.Amplitude;
        return FMath::Sqrt(8 * Sum);   // the significant height these waves make
    };
    TArray<FGerstnerWave> Calm, Breeze, Strong;
    ULakeWaveGenerator::MakeWaves(0.5f, 0, Calm);
    ULakeWaveGenerator::MakeWaves(5.0f, 90, Breeze);
    ULakeWaveGenerator::MakeWaves(12.0f, 90, Strong);
    float CalmMax = 0;
    for (const FGerstnerWave& W : Calm) CalmMax = FMath::Max(CalmMax, W.Amplitude);
    TestTrue(TEXT("A calm raises no waves"), CalmMax < 0.01f);
    TestTrue(FString::Printf(TEXT("A breeze: waves a few cm high (%.1f cm)"), Energy(Breeze)), Energy(Breeze) > 1.5f && Energy(Breeze) < 4.5f);
    TestTrue(FString::Printf(TEXT("A strong wind on a sheltered pond: under a hand high (%.1f cm)"), Energy(Strong)), Energy(Strong) > 4.5f && Energy(Strong) < 10);
    TestTrue(TEXT("The energy is the sea state's"), FMath::IsNearlyEqual(Energy(Strong), ULakeWaveGenerator::SignificantHeight(12), 0.1f));
    TestTrue(TEXT("Stronger wind, longer waves"), ULakeWaveGenerator::PeakWavelength(12) > ULakeWaveGenerator::PeakWavelength(5));
    // The heights only: the same wavelengths and directions whatever the wind, so it can change
    // without the surface jumping.
    bool bSameSet = Breeze.Num() == Strong.Num();
    for (int32 I = 0; bSameSet && I < Breeze.Num(); ++I)
        bSameSet = Breeze[I].WaveLength == Strong[I].WaveLength && Breeze[I].Direction.Equals(Strong[I].Direction);
    TestTrue(TEXT("A change of wind only changes the heights"), bSameSet);
    // Most of the energy runs with the wind (towards +Y here); none against it.
    float With = 0, Against = 0;
    for (const FGerstnerWave& W : Strong)
        (W.Direction.Y > 0 ? With : Against) += W.Amplitude * W.Amplitude;
    TestTrue(TEXT("The waves run with the wind"), With > 20 * Against);
    for (const FGerstnerWave& W : Strong)
        TestTrue(TEXT("Crests stay waves (no loops)"), W.Steepness <= 0.5f && W.Amplitude * 2 * PI / W.WaveLength < 0.4f);
    // Without a Water lake (the procedural world) the surface is the still level.
    TestEqual(TEXT("No Water lake: flat"), ULakeWaves::SurfaceAt(nullptr, FVector2D(0, 0)), LakeFish::SurfaceZ);
    return true;
}

#endif
