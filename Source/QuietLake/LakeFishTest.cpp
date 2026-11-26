#if WITH_DEV_AUTOMATION_TESTS
#include "LakeTestCommon.h"

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
        ALakeFish* Crucian = SpawnFish(World, EFishSpecies::Crucian, 0.6f, Fisher->BaitLocation() - FVector(45, 0, 0), -1, 500 + Try);
        // Hold discovery constant: this part checks the take as the angler sees it, not whether a
        // wandering fish finds the bait in two minutes.
        Crucian->TestHunger() = 1;
        Crucian->TestState() = ALakeFish::EState::Investigate;
        Crucian->TestTargetAngler() = Fisher;
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
    // The next survey consists of independent fish, not a crowd held stationary at the hook.
    Fisher->FinishFishing(TEXT("next bite survey"));
    for (TActorIterator<ALakeFish> It(World); It; ++It) It->Destroy();

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
            ALakeFish* Fish = SpawnFish(World, Kind.Species, Kind.Weight, Fisher->BaitLocation() - FVector(45, 0, 0), -1,
                1000 * ((int32)Kind.Species + 1) + Trial);
            Fish->TestHunger() = 1;
            // Hold discovery constant while surveying the species' bite and return patterns.
            Fish->TestState() = ALakeFish::EState::Investigate;
            Fish->TestTargetAngler() = Fisher;
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
                if (Touch < 0 && Fish->TestState() == ALakeFish::EState::Nibble) Touch = T;
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
    ALakeFish* Shy = SpawnFish(World, EFishSpecies::Roach, 0.15f, Fisher->BaitLocation() - FVector(45, 0, 0), -1, 900);
    Shy->TestHunger() = 1;
    Shy->TestState() = ALakeFish::EState::Investigate;
    Shy->TestTargetAngler() = Fisher;
    for (int32 Step = 0; Step < 600 && Shy->TestState() != ALakeFish::EState::Nibble; ++Step) Shy->Simulate(Dt);
    const bool bTrying = Shy->TestState() == ALakeFish::EState::Nibble;
    Fisher->RodElevationSpeed = 120;
    Shy->Simulate(Dt);
    Fisher->RodElevationSpeed = 0;
    TestTrue(TEXT("A roach trying the bait backs off when the rod is jerked"), bTrying && Shy->TestState() != ALakeFish::EState::Nibble && Fisher->FishingState == EFishingState::Waiting);
    // Past its take a crucian may keep the bait a long while (keepHolding), swallowing it ever deeper;
    // a roach lets go once its take is over.
    int32 LongCrucian = 0, LongRoach = 0;
    for (int32 I = 0; I < 200; ++I)
    {
        LongCrucian += Summarise(MakeBite(EFishSpecies::Crucian, 0.5f, 1.0f, true, false, R)).Window > 20;
        LongRoach += Summarise(MakeBite(EFishSpecies::Roach, 0.5f, 1.0f, true, false, R)).Window > 20;
    }
    AddInfo(FString::Printf(TEXT("Bites holding the bait over 20 s: crucian %d of 200, roach %d of 200"), LongCrucian, LongRoach));
    TestTrue(TEXT("A crucian often keeps the bait a long while"), LongCrucian > 30);
    TestEqual(TEXT("A roach lets go once its take is over"), LongRoach, 0);
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
        auto Swim = [&](const FVector& Want) { Fish->TestSimTime() += Dt; Fish->TestMove(Want, S.CruiseSpeed, Dt); };
        Fish->SetActorRotation(FRotator::ZeroRotator);
        Fish->TestYaw() = 0;
        for (int32 I = 0; I < 150; ++I) Swim(FVector(S.CruiseSpeed, 0, 0));
        OutSeconds = -1; OutMinSpeed = 1e9f; OutMaxRate = 0; OutMaxRateStep = 0;
        float PrevRate = Fish->TestYawRate();
        for (int32 I = 0; I < 600; ++I)
        {
            Swim(FVector(-S.CruiseSpeed, 0, 0));
            OutMinSpeed = FMath::Min(OutMinSpeed, (float)Fish->Velocity().Size());
            OutMaxRate = FMath::Max(OutMaxRate, FMath::Abs(Fish->TestYawRate()));
            OutMaxRateStep = FMath::Max(OutMaxRateStep, FMath::Abs(Fish->TestYawRate() - PrevRate) / Dt);
            PrevRate = Fish->TestYawRate();
            if (OutSeconds < 0 && FMath::Abs(FMath::FindDeltaAngleDegrees(Fish->TestYaw(), 180)) < 10) OutSeconds = (I + 1) * Dt;
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
    TestTrue(TEXT("Big fish are calmer"), SpawnFish(World, EFishSpecies::Perch, 0.8f, Open)->TestAgility() < SpawnFish(World, EFishSpecies::Perch, 0.08f, Open + FVector(200, 0, 0))->TestAgility());

    // A feeding crucian roots nose down at the bottom.
    const FVector FeedSpot = PickSpot(EFishSpecies::Crucian, Random, LakeMap::Centre, 0, nullptr, EFishGoal::Feed);
    ALakeFish* Crucian = SpawnFish(World, EFishSpecies::Crucian, 0.5f, FeedSpot);
    Crucian->TestHunger() = 1;
    Crucian->TestSetGoal(EFishGoal::Feed, 600);
    Crucian->TestRoamTarget() = FeedSpot;
    Crucian->TestbAtSpot() = true;
    float Steepest = 0, Lowest = 1000;
    for (int32 Step = 0; Step < 600; ++Step)
    {
        Life->Step(0.05f);
        Crucian->Simulate(0.05f);
        if (Crucian->TestbRooting())
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
    auto Spawn = [&](EFishSpecies Species, float Weight, FVector2D P) { ALakeFish* Fish = SpawnFish(World, Species, Weight, At(P)); Fish->TestHunger() = 1; return Fish; };
    auto Step = [&](const TArray<ALakeFish*>& Fish) { Life->Step(Dt); for (ALakeFish* F : Fish) if (IsValid(F)) F->Simulate(Dt); };
    auto Gone = [](TArray<ALakeFish*>& Fish) { for (ALakeFish* F : Fish) if (IsValid(F)) F->Destroy(); Fish.Reset(); };

    // 1. A pike swimming by 2.5 m off: the roach takes fright and draws away from it.
    {
        ALakeFish* Pike = Spawn(EFishSpecies::Pike, 2.0f, Open);
        ALakeFish* Roach = Spawn(EFishSpecies::Roach, 0.15f, Open + Right * 250);
        TArray<ALakeFish*> Fish = { Pike, Roach };
        // Seeded: how far the roach turns away depends on its own random decisions, and the test
        // must not pass or fail on a lucky draw.
        Pike->TestRandom().Initialize(3110);
        Roach->TestRandom().Initialize(3111);
        const FVector RoachStart = Roach->GetActorLocation();
        float Before = FVector::Dist(Pike->GetActorLocation(), Roach->GetActorLocation()), Fear = 0, Fastest = 0;
        for (int32 I = 0; I < 80; ++I)
        {
            Pike->TestVel() = FVector(Facing, 0) * 25;   // it keeps swimming
            Step({ Roach });
            Fear = FMath::Max(Fear, Roach->FearNow());
            Fastest = FMath::Max(Fastest, float(Roach->TestVel().Size()));
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
        Pike->TestVel() = FVector::ZeroVector;
        Life->Step(0);
        for (ALakeFish* F : { Far, Close }) { F->TestNextThreatLook() = 0; F->TestSenseThreats(Dt); }
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
        ALakeFish* Roach = SpawnFish(World, EFishSpecies::Roach, 0.15f, Fisher->BaitLocation() - FVector(45, 0, 0));
        Roach->TestHunger() = 1;
        Roach->TestState() = ALakeFish::EState::Investigate;
        Roach->TestTargetAngler() = Fisher;
        TArray<ALakeFish*> Fish = { Roach };
        for (int32 I = 0; I < 600 && !Roach->IsNibbling(); ++I) Step(Fish);
        const bool bTrying = Roach->IsNibbling();
        ALakeFish* Pike = SpawnFish(World, EFishSpecies::Pike, 2.5f, Roach->GetActorLocation() + FVector(0, 80, 0));
        Pike->TestSetGoal(EFishGoal::Feed, 1000);   // a hunting pike
        Fish.Add(Pike);
        for (int32 I = 0; I < 40 && Roach->IsNibbling(); ++I) { Pike->TestVel() = FVector(0, -30, 0); Step({ Roach }); }
        TestTrue(TEXT("A roach trying the bait backs off when a pike swims up"), bTrying && !Roach->IsNibbling());

        // 6. At the bait: a big perch pushes a small roach off it, a small roach cannot push a perch.
        Pike->Destroy();
        Roach->Destroy();
        FLakeFishingFixture::ImmersedRig(Fisher);
        Roach = SpawnFish(World, EFishSpecies::Roach, 0.15f, Fisher->BaitLocation() - FVector(45, 0, 0));
        Roach->TestHunger() = 1;
        Roach->TestState() = ALakeFish::EState::Investigate;
        Roach->TestTargetAngler() = Fisher;
        Fish = { Roach };
        for (int32 I = 0; I < 1200 && !Roach->IsNibbling(); ++I) Step(Fish);
        const bool bRoachOn = Roach->IsNibbling();
        ALakeFish* Big = SpawnFish(World, EFishSpecies::Perch, 0.6f, Fisher->BaitLocation() + FVector(30, 0, 0));
        const bool bPushed = Fisher->ClaimBait(Big) && !Roach->IsNibbling();
        AddInfo(FString::Printf(TEXT("At the bait: roach %.2f kg clout %.2f, perch %.2f kg clout %.2f"), Roach->WeightKg, Roach->Clout(), Big->WeightKg, Big->Clout()));
        TestTrue(TEXT("A big perch pushes a small roach off the bait"), bRoachOn && bPushed);
        FLakeFishingFixture::ImmersedRig(Fisher);
        ALakeFish* Perch = SpawnFish(World, EFishSpecies::Perch, 0.3f, Fisher->BaitLocation() - FVector(45, 0, 0));
        Perch->TestHunger() = 1;
        Perch->TestState() = ALakeFish::EState::Investigate;
        Perch->TestTargetAngler() = Fisher;
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
        Pike->TestSetGoal(EFishGoal::Feed, 1000);
        const int32 EatenBefore = Life->Stats().Eaten;
        int32 Tries = 0;
        bool bBigStruck = false;
        ALakeFish* Big = Spawn(EFishSpecies::Perch, 0.78f, Open + Right * 120);
        for (; Tries < 40 && Life->Stats().Eaten == EatenBefore; ++Tries)
        {
            // It lies where it is, facing ahead.
            Pike->TestbAtSpot() = true;
            Pike->TestLieUntil() = Pike->TestSimTime() + 100;
            Pike->TestRoamTarget() = Pike->GetActorLocation();
            Pike->TestLieFacing() = Pike->GetActorForwardVector();
            const FVector Front = Pike->MouthLocation() + Pike->GetActorForwardVector() * 80;
            ALakeFish* Small = SpawnFish(World, EFishSpecies::Perch, 0.1f, FVector(Front.X, Front.Y, FMath::Max(Front.Z, ALakeFish::BottomZ(Front.X, Front.Y) + 10)));
            for (int32 I = 0; I < 60 && IsValid(Small); ++I)
            {
                Step({ Pike, Small, Big });
                bBigStruck |= Pike->TestPrey().Get() == Big;
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
        Perch->TestSetGoal(EFishGoal::Feed, 1000);
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
        Perch->TestSetGoal(EFishGoal::Feed, 1000);   // a hunting perch
        const int32 FlightsBefore = Life->Stats().FryFlights;
        for (int32 I = 0; I < 40 && Life->Stats().FryFlights == FlightsBefore; ++I) { Perch->TestVel() = FVector(-Right, 0) * 40; Life->Step(Dt); }
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
        Fish->TestHunger() = 1;
        Fish->TestNextMoodCheck() = 0;
        return Fish;
    };
    auto Choose = [](ALakeFish* Fish) { Fish->TestSetGoal(EFishGoal::Cruise, 1000); Fish->TestSetGoal(EFishGoal::Feed, 1000); };

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
        Crucian->TestRandom().Initialize(Seed);
        Crucian->TestWanderSeed() = 0;
        Crucian->TestEnergy() = 1;
        int32 OnSilt = 0;
        for (int32 I = 0; I < 20; ++I) { Choose(Crucian); OnSilt += Crucian->TestFeedOn() == EFishFood::Benthos; }
        AddInfo(FString::Printf(TEXT("Crucian in the lilies: on silt fauna %d of 20"), OnSilt));
        TestTrue(TEXT("A crucian feeds mostly on the silt fauna"), OnSilt >= 12);
        for (int32 K = 0; K < 50 && Crucian->TestFeedOn() != EFishFood::Benthos; ++K) Choose(Crucian);
        const FVector2D First(Crucian->TestRoamTarget());
        const float Bottom = ALakeFish::BottomZ(First.X, First.Y);
        TestTrue(TEXT("It feeds at the bottom"), Crucian->TestRoamTarget().Z < Bottom + 20);
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
            if (!bPatch || !Crucian->TestbAtSpot()) return false;
            if (Crucian->TestFeedOn() == EFishFood::Benthos)
                return Food.Left(EFishFood::Benthos, Patch) < 0.8f
                    && Food.Left(EFishFood::Benthos, Here) > Food.Left(EFishFood::Benthos, Patch) + 0.2f
                    && Food.Available(EFishFood::Benthos, Here, C) > Food.Available(EFishFood::Benthos, Patch, C);
            return LakeFood::IsNatural(Crucian->TestFeedOn()) && Food.Left(EFishFood::Benthos, Patch) < 0.8f
                && Food.Available(Crucian->TestFeedOn(), Here, C) > Food.Available(EFishFood::Benthos, Patch, C);
        };
        for (; Steps < 8000 && !MovedOn(); ++Steps)
        {
            Crucian->TestHunger() = 1;
            Life->Step(Dt);
            Crucian->Simulate(Dt);
            if (!bPatch && Crucian->TestbRooting()) { bPatch = true; Patch = FVector2D(Crucian->GetActorLocation()); }
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
        Crucian->TestRandom().Initialize(Seed);
        for (int32 K = 0; K < 50; ++K)
        {
            Choose(Crucian);
            if (Crucian->TestFeedOn() == EFishFood::Benthos) break;
        }
        TestTrue(TEXT("A benthos feeding patch found"), Crucian->TestFeedOn() == EFishFood::Benthos);
        Crucian->SetActorLocation(Crucian->TestRoamTarget());
        Crucian->TestbAtSpot() = Crucian->TestbRooting() = true;
        Crucian->TestPhaseUntil() = 10;
        Crucian->TestNextFoodCheck() = 0;
        const FVector2D Patch(Crucian->GetActorLocation());
        Life->Food().Eat(EFishFood::Benthos, Patch, 100, Crucian->WeightKg);
        Crucian->Simulate(Dt);
        const EFishFood Food = Crucian->TestFeedOn();
        const float Found = Life->Food().Available(Food, FVector2D(Crucian->TestRoamTarget()), Weather->FishConditions());
        // Switching to plants is also a valid response; continuing on empty benthos is not.
        TestTrue(FString::Printf(TEXT("An exhausted patch triggers a search for available natural food (seed %d)"), Seed),
            LakeFood::IsNatural(Food) && Found > 0.08f && !Crucian->TestbAtSpot() && !Crucian->TestbRooting());
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
                Depth += (LakeFish::SurfaceZ - Roach->TestRoamTarget().Z) / 100 / 30;
                OnInsects += Roach->TestFeedOn() == EFishFood::Insects;
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
            for (int32 I = 0; I < 30; ++I) { Choose(Perch); N += Perch->TestFeedOn() == EFishFood::Fry; }
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
            Roach->TestRandom().Initialize(4242 + 17 * I);   // the same decisions whatever ran before
            Roach->TestWanderSeed() = 0;
            Roach->TestHunger() = 1;
            Fish.Add(Roach);
        }
        Life->Step(0);
        Scent = 0;
        Life->Food().SmellGroundbait(Away, Scent);
        for (ALakeFish* Roach : Fish) Roach->TestSetGoal(EFishGoal::Feed, 1000);
        NearSeconds = 0;
        CameAfter = -1;
        FeedDepth = 0;
        GroundbaitSeconds = 0;
        int32 Deep = 0;
        for (int32 Steps = 0; Steps < 6000; ++Steps)   // 5 min
        {
            Life->Step(Dt);
            for (ALakeFish* Roach : Fish) { Roach->TestHunger() = FMath::Max(Roach->TestHunger(), 0.6f); Roach->Simulate(Dt); }
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
        Crucian->TestHunger() = 1;
        Crucian->TestNextMoodCheck() = 0;
        int32 OnIt = 0;
        for (int32 I = 0; I < 10; ++I) { Crucian->TestSetGoal(EFishGoal::Cruise, 1000); Crucian->TestSetGoal(EFishGoal::Feed, 1000); OnIt += Crucian->TestFeedOn() == EFishFood::Groundbait; }
        for (int32 K = 0; K < 30 && Crucian->TestFeedOn() != EFishFood::Groundbait; ++K) { Crucian->TestSetGoal(EFishGoal::Cruise, 1000); Crucian->TestSetGoal(EFishGoal::Feed, 1000); }
        bool bRooted = false;
        for (int32 Steps = 0; Steps < 2400; ++Steps)   // 2 min
        {
            Crucian->TestHunger() = FMath::Max(Crucian->TestHunger(), 0.6f);
            Life->Step(Dt);
            Crucian->Simulate(Dt);
            bRooted |= Crucian->TestbRooting() && Crucian->TestFeedOn() == EFishFood::Groundbait;
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
        Pricked->TestState() = ALakeFish::EState::Holding;
        Pricked->TestTargetAngler() = Fisher;
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
                Roach->TestSetGoal(EFishGoal::Feed, 1000);
                Roach->TestSetGoal(EFishGoal::Cruise, 1000);
                Near += FVector2D::Distance(FVector2D(Roach->TestRoamTarget()), Far20) < 600;
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
            F->TestSetGoal(EFishGoal::Cruise, 1000);
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
    for (ALakeFish* F : Roach) F->TestFear() = 0.9f;
    for (ALakeFish* F : Red) F->TestFear() = 0.9f;
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
                if (!Fish->TestAtSurfaceMoment())
                {
                    ++Samples;
                    Above += P.Z > Fish->TestCeilingZ() + 3;
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
            Perch->TestHunger() = 1;
            Perch->TestEnergy() = 1;
            All.Add(Perch);
        }
        Life->Step(0);
        if (ALakeFish* Lead = Life->SchoolLeader(20 + K)) Lead->TestSetGoal(EFishGoal::Feed, 600);
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
        Top->TestRoamTarget() = FVector(LakeMap::Centre + FVector2D(1500, 0), SurfaceZ - 5);
        All.Add(Top);
        Run(10);
        const float TopOver = Top->GetActorLocation().Z - Top->TestCeilingZ();
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
        const float Usual = Roach->TestCeilingZ();
        Roach->TestSetGoal(EFishGoal::Feed, 600);
        Roach->TestFeedOn() = EFishFood::Insects;
        TestTrue(TEXT("A roach sipping insects may come up to the top"), Roach->TestAtSurfaceMoment() && Roach->TestCeilingZ() > Usual + 10);
        Roach->TestFeedOn() = EFishFood::Plankton;
        Roach->TestSurfaceUntil() = Roach->TestSimTime() + 3;
        TestTrue(TEXT("A fish gulping air may come up to the top"), Roach->TestAtSurfaceMoment());
        Roach->TestSurfaceUntil() = -1;
        TestFalse(TEXT("Feeding on plankton is no surface moment"), Roach->TestAtSurfaceMoment());
        auto Named = [](const TCHAR* Id) { for (int32 I = 0; I < NumSpecies(); ++I) if (Info((EFishSpecies)I).Id == Id) return (EFishSpecies)I; return EFishSpecies::Roach; };
        const float Bleak = CeilingAt(Named(TEXT("bleak")), LakeMap::Centre.X, LakeMap::Centre.Y, 12), Bream = CeilingAt(Named(TEXT("bream")), LakeMap::Centre.X, LakeMap::Centre.Y, 40);
        TestTrue(TEXT("The bleak keeps to the top, the bream well under it"), Bleak > SurfaceZ - 6 && Bream < SurfaceZ - 40);
        Clear();
    }
    World->DestroyWorld(false);
    return true;
}

#endif
