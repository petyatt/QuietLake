#if WITH_DEV_AUTOMATION_TESTS
#include "LakeTestCommon.h"

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
    // After the strike, the fish back in the water at the line's reach (held up for the strike, it
    // hung in the air at the line's end).
    auto BackInWater = [&](ALakeFish* Fish)
    {
        if (!IsValid(Fish)) return;
        FVector P = Fisher->RodTip() + FVector(Facing, 0) * (ALakeRod::LineLength - 40);
        P.Z = FMath::Max(ALakeEnvironment::GroundHeight(P.X, P.Y) + 25, LakeFish::SurfaceZ - 70);
        Fish->SetActorLocation(P);
    };
    auto StrikeHome = [&](ALakeFish* Fish)
    {
        Fisher->NextActionTime = 0;
        Fisher->ServerCancel_Implementation();
        for (int32 Step = 0; Step < 8 && Fisher->FishingState == EFishingState::Bite && IsValid(Fish); ++Step)
        {
            Fish->SetActorLocation(Fisher->RodTip() + FVector(Facing, 0) * (ALakeRod::LineLength + 30));
            Fish->TestState() = ALakeFish::EState::Holding;
            Fish->TestTargetAngler() = Fisher;
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

    // A locally interested perch reaches the worm, tests it and takes it. Discovery is covered
    // independently by LocalBait; this fixture exercises the full catch and landing rules.
    Cast();
    const FVector Bait = Fisher->BaitLocation();
    ALakeFish* Perch = SpawnFish(World, EFishSpecies::Perch, 0.35f, Bait - FVector(55, 0, 0));
    Perch->TestState() = ALakeFish::EState::Investigate;
    Perch->TestTargetAngler() = Fisher;
    for (int32 Step = 0; Step < 6000 && Fisher->FishingState == EFishingState::Waiting; ++Step) Perch->Simulate(0.05f);
    TestTrue(TEXT("An interested perch reaches and takes the worm"), Fisher->FishingState == EFishingState::Bite);
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
    BackInWater(Perch);
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
        Fisher->SetActorLocation(Back(0));
        Cast();
        TestTrue(TEXT("Cast for the crucian fight"), Fisher->FishingState == EFishingState::Waiting);
        for (int32 Pose = 0; Pose < 60; ++Pose) Fisher->UpdateRodPose(0.05f);   // the rod settles to the waiting pose
        const FVector TookAt = Fisher->BaitLocation();
        ALakeFish* Fish = SpawnFish(World, EFishSpecies::Crucian, Weight, TookAt);
        Fisher->OnFishTake(Fish);
        StrikeHome(Fish);
        BackInWater(Fish);
        int32 Step = 0;
        for (; Step < 4800 && Fisher->FishingState == EFishingState::Fight; ++Step)
        {
            if (Fisher->Tension > 0.55f) Fisher->ServerRelease_Implementation();
            else if (Fisher->Tension < 0.2f) Fisher->ServerFish_Implementation();
            // Too heavy to lift out: lead it to the bank, stepping back from the water; once it lies
            // in the shallows, step over to it and take it in the hand.
            if (Fisher->FightHint == 10) Fisher->SetActorLocation(Fisher->GetActorLocation() - FVector(Facing, 0) * 50 * 0.05f);
            else if (Fisher->FightHint == 9 && IsValid(Fish))
                Fisher->SetActorLocation(Fisher->GetActorLocation() + (Fish->GetActorLocation() - Fisher->GetActorLocation()).GetSafeNormal2D() * 60 * 0.05f);
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
    AddInfo(FString::Printf(TEXT("Early lift: rod lift %.2f after 1 s, hand load %.0f gf, state %d, reeling %d, yield %.2f now %.2f"), Fisher->RodLift, Fisher->HandLoadGf, (int32)Fisher->FishingState, Fisher->bReeling, Fisher->YieldUntil, Fisher->ServerNow()));
    TestTrue(TEXT("Holding LMB during a bite raises the rod"), Fisher->RodLift > 0.2f);   // (towards upright, slowed by the fish holding the bait)
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

    // Never reeling: left on a slack line, a fish hooked deep (it swallowed the bait) stays on and
    // goes where it likes; one hooked in the edge of the lip soon shakes the hook out.
    auto Abandoned = [&](float Grip)
    {
        Cast();
        const FVector TookAt = Fisher->BaitLocation();
        ALakeFish* Crucian = SpawnFish(World, EFishSpecies::Crucian, 0.5f, TookAt);
        Fisher->OnFishTake(Crucian);
        StrikeHome(Crucian);
        BackInWater(Crucian);
        if (IsValid(Crucian)) Crucian->SetHookGrip(Grip);
        Fisher->ServerRelease_Implementation();
        int32 Step = 0;
        for (; Step < 4800 && Fisher->FishingState == EFishingState::Fight; ++Step) { Fisher->UpdateFight(0.05f); if (IsValid(Crucian)) Crucian->Simulate(0.05f); }
        AddInfo(FString::Printf(TEXT("Abandoned crucian, hook grip %.2f: %s after %.0f s | %s"), Grip,
            Fisher->FishingState == EFishingState::Fight ? TEXT("still on") : TEXT("off"), Step * 0.05f, *Fisher->Status));
        const bool bOn = Fisher->FishingState == EFishingState::Fight;
        if (bOn) Fisher->FinishFishing(TEXT("test over"));
        return bOn;
    };
    TestTrue(TEXT("A fish hooked deep stays on a slack line for minutes"), Abandoned(1.0f));
    TestFalse(TEXT("A fish hooked in the edge of the lip shakes the hook out on a slack line"), Abandoned(0.35f));
    TestEqual(TEXT("A fish left on the line awards no fish"), Fisher->CatchCount, 1);

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
    Taker->TestTargetRig() = OnStand;
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
    for (float Weight : { 0.18f, 0.3f })
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
                Fisher->LandServerTime -= 1 / 60.0f;
                Fisher->Tick(1 / 60.0f);
            }
        };
        Run(2); Fisher->StartCast(); Run(1.2f); Fisher->ServerRelease_Implementation(); Run(5);
        ALakeFish* Fish = SpawnFish(World, EFishSpecies::Perch, Weight, Fisher->RodVisual->HookLocation(), -1, 100 + FMath::RoundToInt(Weight * 1000));
        Fish->Hook(Fisher); Fisher->HookedFish = Fish; Fisher->LineFish = Fish;
        Fisher->FishingState = EFishingState::Fight;
        for (int32 I = 0; I < 7200 && Fisher->FishingState == EFishingState::Fight; ++I)
        {
            if (Fisher->Tension > 0.55f) Fisher->ServerRelease_Implementation();
            else if (Fisher->Tension < 0.2f) Fisher->ServerFish_Implementation();
            Run(1 / 60.0f);
            if (IsValid(Fish)) Fish->Simulate(1 / 60.0f);
            if (I % 120 == 0 && IsValid(Fish))
                AddInfo(FString::Printf(TEXT("  %.0f s: lift %.2f elev %.0f line %.0f hands %.0f rod %.0f bend %.0f mouth %.0f in water %.2f stamina %.2f lmb %d"), I / 60.0f, Fisher->RodLift, Fisher->RodElevation,
                    Fisher->LineLoadGf, Fisher->HandLoadGf, Fisher->RodLoadGf, Fisher->RodVisual->TipBend(), Fish->MouthLocation().Z - LakeFish::SurfaceZ, Fish->BodyInWater(), Fish->StaminaFraction(), Fisher->bReeling));
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
        const EFishingState BeforeRelease = Fisher->FishingState;
        Fisher->ServerRelease_Implementation();
        TestTrue(TEXT("Fish release keeps hook position"), Before.Equals(Fisher->RodVisual->HookLocation(), 0.001));
        Run(0.4f);
        AddInfo(FString::Printf(TEXT("Release during lift %.2f kg: state %d -> %d, progress %.3f -> %.3f, catches %d"),
            Weight, (int32)BeforeRelease, (int32)Fisher->FishingState, Progress, Fisher->LandProgress, Fisher->CatchCount));
        TestTrue(TEXT("Releasing lowers a fish or leaves it already swimming, without awarding a catch"),
            (Fisher->FishingState == EFishingState::Fight || Fisher->LandProgress < Progress) && Fisher->CatchCount == 0);
        for (int32 I = 0; I < 600 && Fisher->FishingState == EFishingState::Landing; ++I) Run(1 / 60.0f);
        AddInfo(FString::Printf(TEXT("Lowered fish %.2f: state=%d lift=%.2f progress=%.2f hook=%s water=%d elevation=%.0f"), Weight, int32(Fisher->FishingState), Fisher->RodLift, Fisher->LandProgress, *Fisher->RodVisual->HookLocation().ToCompactString(), ALakeEnvironment::IsWater(Fisher->RodVisual->HookLocation()), Fisher->RodElevation));
        TestTrue(TEXT("A fully lowered fish resumes swimming on the hook"), Fisher->FishingState == EFishingState::Fight && !Fish->IsLifted());
        Fisher->ServerFish_Implementation();
        // (Lifting again: the fish swims while it is in the water, hangs while it is out.)
        for (int32 I = 0; I < 1800 && Fisher->CatchCount == 0 && Fisher->FishingState != EFishingState::Idle; ++I)
        {
            Run(1 / 60.0f);
            if (IsValid(Fish) && Fisher->FishingState == EFishingState::Fight) Fish->Simulate(1 / 60.0f);
            if (I % 30 == 0 && IsValid(Fish))
                AddInfo(FString::Printf(TEXT("  relift %.1f s: state %d lift %.2f elev %.0f line %.0f hands %.0f mouth %.0f water %.2f out %.0f"), I / 60.0f, int32(Fisher->FishingState),
                    Fisher->RodLift, Fisher->RodElevation, Fisher->LineLoadGf, Fisher->HandLoadGf, Fish->MouthLocation().Z - LakeFish::SurfaceZ, Fish->BodyInWater(),
                    FVector::Dist2D(Fish->MouthLocation(), Fisher->GetActorLocation())));
        }
        AddInfo(FString::Printf(TEXT("Physical fish %.2f kg: caught %d state %d hook-to-hand %.0f bend %.0f lift %.2f hand load %.0f hook speed %.0f hook %.0f cm over the water, elevation %.0f"), Weight, Fisher->CatchCount, int32(Fisher->FishingState),
            FVector::Dist(Fisher->RodVisual->HookLocation(), Fisher->HandPoint(0)), Fisher->RodVisual->TipBend(), Fisher->RodLift, Fisher->HandLoadGf,
            (Fisher->RodVisual->HookVelocity() - Fisher->GetVelocity()).Size(), Fisher->RodVisual->HookLocation().Z - LakeFish::SurfaceZ, Fisher->RodElevation));
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

// The species passports (Content/LiveFish/Species): the files read without problems and in order,
// each fish has its model; the example species in the README there is read from its file alone, a
// misspelt field and a missing diet are reported.
// Exercise the production Hooked path with fixed random streams; no frame-rate-dependent rolls.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeFightProfilesTest, "QuietLake.Fishing.FightProfiles", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeFightProfilesTest::RunTest(const FString& Parameters)
{
    using namespace LakeFish;
    using EPhase = FLakeFishFight::EFightPhase;
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
        Fish->TestRandom().Initialize(1234 + I);
        Fish->GetFight().FightIndividual = 1;
        Fish->Hook(Fisher);
        Fish->WakeOnLine();   // the line is taut here: the fish feels the hook and fights
        FStats& T = Stats.Add(S.Id);
        // Each frame uses the actual actor simulation, including motion, stamina and profile phases.
        EPhase Previous = Fish->GetFight().FightPhase;
        for (int32 Step = 0; Step < 2400; ++Step)
        {
            Fish->SetActorLocation(Open);
            Fish->TestStamina() = Fish->TestMaxStamina(); // isolate the species' fresh rhythm
            Fish->ApplyLineForce(FVector(-40, 0, 0));   // a taut line: on a slack one it makes off instead
            Fish->Simulate(0.025f);
            T.Bursts += Fish->GetFight().FightPhase == EPhase::Burst && Previous != EPhase::Burst;
            T.Gaps += Fish->GetFight().FightPhase == EPhase::Gap && Previous != EPhase::Gap;
            T.Rest += Fish->GetFight().FightPhase == EPhase::Pause ? 0.025f : 0;
            T.Pull += Fish->GetFight().FightPhase == EPhase::Pull ? 0.025f : 0;
            Previous = Fish->GetFight().FightPhase;
            if (Step == 0) TestTrue(S.Id + TEXT(": Hook starts a new target immediately"), FVector::Dist(Fish->TestRoamTarget(), Open) > 100);
        }
        for (int32 Pick = 0; Pick < 1000; ++Pick)
        {
            Fish->SetActorLocation(Open);
            Fish->TestStamina() = Fish->TestMaxStamina();
            Fish->GetFight().ChooseFightTarget();
            const FVector Target = Fish->TestRoamTarget();
            T.Dives += Target.Z < ALakeFish::BottomZ(Target.X, Target.Y) + 20;
            T.Surfaces += Target.Z > SurfaceZ - 20;
        }
        // Under a sustained opposing load, effort drains the reserve; a genuine pause restores it.
        Fish->SetActorLocation(Open);
        Fish->TestVel() = FVector::ZeroVector;
        Fish->SetActorRotation(FRotator::ZeroRotator);
        Fish->TestStamina() = Fish->TestMaxStamina() * 0.6f;
        Fish->GetFight().FightPhase = EPhase::Pull;
        const float BeforeLoad = Fish->TestStamina();
        for (int32 Step = 0; Step < 40; ++Step)
        {
            Fish->ApplyLineForce(FVector(-S.Acceleration * S.Strength * Fish->WeightKg, 0, 0));
            Fish->GetFight().MoveHooked(FVector(S.BurstSpeed, 0, 0), S.BurstSpeed, 0.025f);
        }
        TestTrue(S.Id + TEXT(": resisting the line costs stamina"), Fish->TestStamina() < BeforeLoad - 0.2f);
        // A rest is a rest only on a slack line: leaning against a pull is work too. (The body's
        // stamina alone: on a slack line a fresh fish would rather make off, see ChooseEscapeTarget.)
        Fish->ApplyLineForce(FVector::ZeroVector);
        Fish->GetFight().FightPhase = EPhase::Pause;
        Fish->GetFight().FightRemaining = 4;
        Fish->TestVel() = FVector::ZeroVector;
        Fish->TestRoamTarget() = Fish->GetActorLocation();
        const float BeforeRest = Fish->TestStamina();
        Fish->GetFight().MoveHooked(FVector::ZeroVector, 0, 0.05f);
        TestTrue(S.Id + TEXT(": Hooked pause really recovers stamina"), Fish->TestStamina() > BeforeRest);
        // Given slack, a fish with strength left makes off somewhere in the water instead of waiting.
        Fish->SetActorLocation(Open);
        Fish->TestStamina() = Fish->TestMaxStamina();
        Fish->GetFight().FightPhase = EPhase::Pause;
        Fish->GetFight().FightRemaining = 4;
        Fish->GetFight().bWasSlack = false;
        Fish->ApplyLineForce(FVector::ZeroVector);
        Fish->Simulate(0.025f);
        TestTrue(S.Id + TEXT(": on a slack line it makes off to water of its choosing"), Fish->GetFight().FightPhase != EPhase::Pause
            && LakeMap::IsWater(Fish->TestRoamTarget().X, Fish->TestRoamTarget().Y) && FVector::Dist2D(Fish->TestRoamTarget(), Open) > 100);
        // Neither a current burst nor the common movement tail can restore a tired fish to full speed.
        Fish->GetFight().FightPhase = EPhase::Burst;
        Fish->GetFight().FightRemaining = 2;
        Fish->TestStamina() = Fish->TestMaxStamina();
        FVector Desired; float FreshSpeed = 0, TiredSpeed = 0;
        Fish->GetFight().UpdateFightMotion(0.01f, Desired, FreshSpeed);
        Fish->TestStamina() = Fish->TestMaxStamina() * 0.1f;
        Fish->GetFight().UpdateFightMotion(0.01f, Desired, TiredSpeed);
        TestTrue(S.Id + TEXT(": even bursts weaken with fatigue"), TiredSpeed < FreshSpeed * 0.3f);
        Fish->GetFight().FightPhase = EPhase::Pause;
        Fish->GetFight().FightRemaining = 10;
        Fish->TestStamina() = Fish->TestMaxStamina();
        Fish->TestVel() = FVector::ZeroVector;
        Fish->GetFight().MoveHooked(FVector::ZeroVector, 0, 0.05f);
        TestTrue(S.Id + TEXT(": recovery never overfills reserve"), Fish->TestStamina() <= Fish->TestMaxStamina());
        TestTrue(S.Id + TEXT(": stamina never negative"), Fish->TestStamina() >= 0);
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
    Pike->TestRandom().Initialize(77);
    Pike->GetFight().FightIndividual = 1;
    // Find a permitted probabilistic leap, then follow the real ballistic movement.
    for (int32 I = 0; I < 100 && !Pike->GetFight().bFightJump; ++I)
    {
        Pike->SetActorLocation(FVector(Open.X, Open.Y, SurfaceZ - 8));
        Pike->TestStamina() = Pike->TestMaxStamina();
        Pike->GetFight().StartFightBurst();
    }
    TestTrue(TEXT("A fresh pike can launch a hooked leap"), Pike->GetFight().bFightJump);
    TestFalse(TEXT("A leaping pike cannot be landed"), Pike->ReadyToLand());
    const float LaunchStamina = Pike->TestStamina();
    float Peak = -1000;
    for (int32 Step = 0; Step < 100; ++Step)
    {
        Pike->GetFight().MoveHooked(FVector::ZeroVector, 0, 0.01f);
        Peak = FMath::Max(Peak, float(Pike->GetActorLocation().Z - SurfaceZ));
    }
    TestTrue(TEXT("The fish itself clears the water and returns"), Peak > 25 && !Pike->GetFight().bFightJump && Pike->GetActorLocation().Z <= SurfaceZ);
    TestTrue(TEXT("The leap does not regenerate stamina in flight"), Pike->TestStamina() <= LaunchStamina);
    Pike->SetActorLocation(FVector(Open.X, Open.Y, SurfaceZ - 8));
    for (int32 I = 0; I < 100; ++I) Pike->GetFight().StartFightBurst();
    TestFalse(TEXT("The jump cooldown prevents repeated leaps"), Pike->GetFight().bFightJump);
    // Applied line force is still integrated in the airborne branch.
    Pike->GetFight().bFightJump = true;
    Pike->SetActorLocation(FVector(Open.X, Open.Y, SurfaceZ + 60));   // (in the air: under water the water would hold its head)
    Pike->TestVel() = FVector(0, 0, 200);
    Pike->ApplyLineForce(FVector(140, 0, 0));
    Pike->GetFight().MoveHooked(FVector::ZeroVector, 0, 0.01f);
    TestTrue(TEXT("The line remains attached and affects an airborne fish"), Pike->TestVel().X > 0.5);
    Pike->GetFight().bFightJump = false;
    Pike->GetFight().NextFightJump = 0;
    Pike->TestStamina() = Pike->TestMaxStamina() * 0.1f;
    for (int32 I = 0; I < 100; ++I) Pike->GetFight().StartFightBurst();
    TestFalse(TEXT("An exhausted pike cannot leap"), Pike->GetFight().bFightJump);

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
        Pike->TestRandom().Initialize(Seed);
        Pike->TestStateTime() = 4;
        Pike->TestStamina() = Pike->TestMaxStamina() * 0.3f;
        Fisher->FishingState = EFishingState::Fight;
        Fisher->HookedFish = Pike;
        Fisher->bReeling = true;
        Fisher->FightTime = 4;
        Fisher->UpdateFight(0.01f);
        bFinalRun = Pike->GetFight().bShoreRun;
    }
    TestTrue(TEXT("A final run gets a chance before automatic landing"), bFinalRun && Fisher->FishingState == EFishingState::Fight);
    TestTrue(TEXT("The final run heads away from the angler"), FVector::DotProduct(Pike->TestRoamTarget() - NearBank, FVector(Facing, 0)) > 0);
    Pike->GetFight().bShoreRun = false;
    for (int32 I = 0; I < 100; ++I) Pike->ReadyToLand();
    TestFalse(TEXT("The final run cannot be rolled over and over"), Pike->GetFight().bShoreRun);
    Pike->Hook(Fisher);
    TestTrue(TEXT("Rehook clears old manoeuvre state"), !Pike->GetFight().bShoreTried && !Pike->GetFight().bFightJump && Pike->GetFight().FightRemaining == 0);
    // Size and individuality influence the production burst timing.
    ALakeFish* Perch = SpawnFish(World, EFishSpecies::Perch, 0.3f, Open);
    Perch->Hook(Fisher);
    Perch->TestRandom().Initialize(9); Perch->TestSize01() = 0; Perch->GetFight().FightIndividual = 1;
    Perch->GetFight().StartFightBurst(); const float SmallBurst = Perch->GetFight().FightRemaining;
    Perch->TestRandom().Initialize(9); Perch->TestSize01() = 1;
    Perch->GetFight().StartFightBurst(); const float BigBurst = Perch->GetFight().FightRemaining;
    Perch->TestRandom().Initialize(9); Perch->GetFight().FightIndividual = 1.15f;
    Perch->GetFight().StartFightBurst();
    TestTrue(TEXT("Size and individual pace alter bouts"), BigBurst > SmallBurst * 1.5f && Perch->GetFight().FightRemaining < BigBurst);
    TestFalse(TEXT("Perch have no hooked leap"), Perch->GetFight().bFightJump);

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
            const FVector Expected = F->TestBody()->GetComponentTransform().TransformPosition(S.MouthPoint * S.MeshLength);
            TestTrue(TEXT("Physical mouth equals the scaled model attachment"), FVector::Dist(Expected, F->MouthLocation()) < 0.01f);
            TestTrue(TEXT("Contact is small and scales with the mouth"), F->ContactRadius() >= 0.4f && F->ContactRadius() <= 2);
            F->TestState() = ALakeFish::EState::Nibble; F->TestTargetAngler() = Angler; F->TestAtBait().StepEnd = 10000;
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
            F->TestStamina() = F->TestMaxStamina() * 0.5f;
            F->GetFight().FightPhase = FLakeFishFight::EFightPhase::Pull;
            F->TestVel() = FVector::ZeroVector;
            const float Before = F->TestStamina();
            F->GetFight().MoveHooked(FVector(100, 0, 0), 100, 0.05f);
            TestTrue(TEXT("Muscular work consumes stamina"), F->TestStamina() < Before);
            F->TestVel() = FVector::ZeroVector; F->GetFight().PendingLineForce = FVector::ZeroVector;
            F->GetFight().FightPhase = FLakeFishFight::EFightPhase::Pause;
            const float Rest = F->TestStamina();
            F->GetFight().MoveHooked(FVector::ZeroVector, 0, 0.05f);
            TestTrue(TEXT("A real pause recovers stamina"), F->TestStamina() > Rest);
            F->SetActorLocation(FVector(Open.X, Open.Y, LakeFish::SurfaceZ - 1));
            F->TestVel() = FVector(0, 0, 120);
            F->GetFight().MoveHooked(FVector::ZeroVector, 0, 0.02f);
            TestTrue(TEXT("Surface has no hard position ceiling"), F->GetActorLocation().Z > LakeFish::SurfaceZ - 1);
            F->LiftOut();
            for (int32 I = 0; I < 40; ++I)
            {
                const FVector Hook = Open + FVector(0, I * 0.1f, 120);
                F->HangOnLine(Hook, FVector::UpVector, 0.02f);
                TestTrue(TEXT("Hanging mouth stays on hook while body swings"), FVector::Dist(F->MouthLocation(), Hook) < 0.01f);
                TestFalse(TEXT("Hanging body remains finite"), F->GetActorLocation().ContainsNaN());
            }
            const float Length = LakeFish::LengthCm(Sp, Weight);
            FVector Mouth(Open.X, Open.Y, LakeFish::SurfaceZ + Length * 0.3f);
            const float Horizontal = F->ImmersionAt(Mouth, FQuat::Identity).Fraction;
            const float Vertical = F->ImmersionAt(Mouth, FRotator(90,0,0).Quaternion()).Fraction;
            TestTrue(TEXT("A submerged tail still supports a raised mouth"), Vertical>Horizontal+0.25f);
            float Previous = 1;
            for (int32 Step=0; Step<=100; ++Step)
            {
                Mouth.Z=LakeFish::SurfaceZ+Length*(-0.3f+Step*0.015f);
                const float Supported=F->ImmersionAt(Mouth, FRotator(60,0,0).Quaternion()).Fraction;
                TestTrue(TEXT("Emergence transfers weight continuously"), Supported<=Previous+0.0001f && Previous-Supported<0.12f);
                Previous=Supported;
            }
            TestTrue(TEXT("A fully emerged body has no buoyancy"), Previous<0.001f);
            F->SetActorRotation(FRotator::ZeroRotator);
            F->SetActorLocation(Mouth-F->MouthOffset());
            F->LiftOut();
            for (int32 Step=0; Step<360; ++Step) F->HangOnLine(Mouth, FVector::ForwardVector, 1/60.0f);
            TestTrue(TEXT("An airborne body hangs under its mouth, independently of line angle"), F->GetActorForwardVector().Z>0.94f);
            TestTrue(TEXT("Turning the body never detaches the mouth"), FVector::Dist(F->MouthLocation(),Mouth)<0.01f);
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
        F->TestState() = ALakeFish::EState::Holding; F->TestTargetAngler() = Angler; F->TestAtBait().Step.Hook = 1;
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
    F->GetFight().HookGrip = 0.001f; F->ApplyLineForce(FVector(-10000,0,0));
    F->GetFight().MoveHooked(FVector(100,0,0),100,0.1f);
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
    F->TestState() = ALakeFish::EState::Nibble; // isolate pose assertions; real loaded takes are tested above and in LiveRigPreview
    Angler->RodLift = 0; Angler->Tension = 0; Angler->StrikeServerTime = -100;
    Angler->StrikeUntil = -1; Angler->HookImpulse = 0;
    F->TestAtBait().StepEnd = 10000;
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
    F->TestTargetRig() = StandRod;
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
    Taker->TestTargetRig() = Light;
    A->OnFishTake(Taker);
    TestTrue(TEXT("A take on one stand leaves the others waiting"),
        A->Rods[Light].State == EFishingState::Bite && A->Rods[Medium].State == EFishingState::Waiting && A->Rods[Heavy].State == EFishingState::Waiting);
    ALakeFish* Trier = SpawnFish(World, EFishSpecies::Roach, 0.2f, A->BaitLocation(Heavy));
    Trier->TestTargetRig() = Heavy;
    TestTrue(TEXT("Another fish may try a bait on another stand"), A->ClaimBait(Trier) && A->Rods[Heavy].BaitFish.Get() == Trier);
    A->ReleaseBaitClaim(Trier);
    TestTrue(TEXT("and let it be"), !A->Rods[Heavy].BaitFish.IsValid());
    // Not struck in time: the fish lets go, the rig fishes on.
    A->Rods[Light].EscapeAt = A->ServerNow() - 1;
    A->UpdateParkedRods(0.05f);
    TestTrue(TEXT("A take not struck: the fish lets go, the rig fishes on"), A->Rods[Light].State == EFishingState::Waiting && A->HasBaitInWater(Light) && Taker->IsFree());

    // RMB beside the stand with a take: that rod comes to the hands, its fish on the bait, and strikes.
    ALakeFish* Second = SpawnFish(World, EFishSpecies::Perch, 0.3f, A->BaitLocation(Light));
    Second->TestTargetRig() = Light;
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

    // An interested perch reaches the correct rig on its stand and takes it.
    ALakeFish* Perch = SpawnFish(World, EFishSpecies::Perch, 0.35f, A->BaitLocation(Medium) - FVector(55, 0, 0));
    Perch->TestState() = ALakeFish::EState::Investigate;
    Perch->TestTargetAngler() = A;
    Perch->TestTargetRig() = Medium;
    bool bTaken = false;
    for (int32 Step = 0; Step < 6000 && !bTaken; ++Step)
    {
        Perch->Simulate(0.05f);
        A->UpdateParkedRods(0.05f);
        for (const FLakeRodSlot& Slot : A->Rods) bTaken |= Slot.State == EFishingState::Bite;
    }
    TestTrue(TEXT("An interested perch reaches a bait on the correct stand and takes it"), bTaken);
    for (int32 Rig = 0; Rig < ALakeCharacter::NumRods; ++Rig) A->EscapeParked(Rig);
    ALakeFish* Thief = SpawnFish(World, EFishSpecies::Roach, 0.05f, A->BaitLocation(Heavy), -1, 3101);
    Thief->TestState() = ALakeFish::EState::Nibble;
    Thief->TestTargetAngler() = A;
    Thief->TestTargetRig() = Heavy;
    TestTrue(TEXT("A small fish claims the heavy stand's bait"), A->ClaimBait(Thief));
    A->OnBaitStolen(Thief);
    TestTrue(TEXT("Theft empties only the affected stand's hook"), !A->HasBaitInWater(Heavy) && !A->Rods[Heavy].bBaitPresent
        && A->HasBaitInWater(Light) && A->HasBaitInWater(Medium));
    A->TakeRodInHands(Heavy);
    TestFalse(TEXT("An empty fishing rig stays empty when taken in the hands"), A->bBaitPresent);
    A->FinishFishing(TEXT("test empty rig pickup"));
    A->ParkHandRod(ERodPlace::Ground, Stand, Yaw);
    A->TakeRodInHands(Light);
    TestTrue(TEXT("Another baited rig keeps its bait"), A->bBaitPresent);
    A->FinishFishing(TEXT("test other rig pickup"));
    A->ParkHandRod(ERodPlace::Ground, Stand + FVector(100, 0, 0), Yaw);
    A->TakeRodInHands(Heavy);
    TestFalse(TEXT("An idle empty rig does not inherit another rod's bait"), A->bBaitPresent);
    FLakeFishingFixture::FreshCast(A);
    TestTrue(TEXT("A fresh cast rebaits the formerly empty parked rig"), A->bBaitPresent);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeLocalBaitTest, "QuietLake.Fishing.LocalBait", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeLocalBaitTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    FVector2D Facing;
    const FVector2D Edge = LakeMap::StartSpot(&Facing) + Facing * 90;
    const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X));
    ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(FVector(Edge, 100), FRotator(0, Yaw, 0));
    FLakeFishingFixture::ImmersedRig(Fisher);
    const FVector Bait = Fisher->BaitLocation();
    auto Discoveries = [&](float Distance, bool bFace, float Vertical)
    {
        int32 Found = 0;
        for (int32 I = 0; I < 64; ++I)
        {
            const FVector P = Bait - FVector(Facing, 0) * Distance + FVector(0, 0, Vertical);
            ALakeFish* Fish = SpawnFish(World, EFishSpecies::Perch, 0.15f, P, -1, 101 + I);
            Fish->SetActorRotation(FRotator(0, Yaw + (bFace ? 0 : 180), 0));
            Fish->TestHunger() = 1;
            Fish->TestSetGoal(EFishGoal::Feed, 1000);
            for (int32 J = 0; J < 12 && Fish->IsFree(); ++J) Fish->TestUpdateBait();
            Found += Fish->IsBusy();
            Fish->Destroy();
        }
        return Found;
    };
    TestEqual(TEXT("A hook bait cannot summon fish from five metres"), Discoveries(500, true, 0), 0);
    TestEqual(TEXT("Unseen bait behind a fish is not a known target"), Discoveries(100, false, 0), 0);
    TestEqual(TEXT("A fish does not discover a hook two metres above its feeding layer"), Discoveries(30, true, -200), 0);
    TestTrue(TEXT("A hungry fish can discover suitable bait nearby in front"), Discoveries(55, true, 0) > 20);

    TArray<ALakeFish*> Crowd;
    for (int32 I = 0; I < 10; ++I)
    {
        ALakeFish* Fish = SpawnFish(World, EFishSpecies::Perch, 0.15f, Bait - FVector(Facing, 0) * 55, -1, 200 + I);
        Fish->SetActorRotation(FRotator(0, Yaw, 0));
        Fish->TestHunger() = 1;
        Fish->TestSetGoal(EFishGoal::Feed, 1000);
        Crowd.Add(Fish);
    }
    for (int32 J = 0; J < 12; ++J) for (ALakeFish* Fish : Crowd) Fish->TestUpdateBait();
    int32 Approaching = 0;
    ALakeFish* First = nullptr;
    for (ALakeFish* Fish : Crowd) if (Fish->IsBusy()) { ++Approaching; First = Fish; }
    TestEqual(TEXT("Ten equal fish do not all approach the same rig"), Approaching, 1);
    if (First)
    {
        First->Spook(Bait, 4);
        for (int32 J = 0; J < 12; ++J) for (ALakeFish* Fish : Crowd) if (Fish != First) Fish->TestUpdateBait();
        int32 Successors = 0;
        for (ALakeFish* Fish : Crowd) Successors += Fish->IsBusy();
        TestEqual(TEXT("A scared fish leaves room for one successor"), Successors, 1);
    }
    for (ALakeFish* Fish : Crowd) Fish->Destroy();

    // Same random draw, different sizes: response pace is an individual property, not an
    // obligatory small-then-large script.
    ALakeFish* Quick = SpawnFish(World, EFishSpecies::Perch, 0.05f, Bait - FVector(Facing, 0) * 500, -1, 555);
    ALakeFish* Slow = SpawnFish(World, EFishSpecies::Perch, 1.0f, Bait - FVector(Facing, 0) * 500, -1, 555);
    Quick->TestRandom().Initialize(555); Slow->TestRandom().Initialize(555);
    Quick->TestUpdateBait(); Slow->TestUpdateBait();
    TestTrue(TEXT("Small fish reconsider a bait sooner than mature fish"), Quick->TestNextThink() * 1.5f < Slow->TestNextThink());
    Quick->Destroy(); Slow->Destroy();

    ALakeFish* Small = SpawnFish(World, EFishSpecies::Perch, 0.05f, Bait - FVector(Facing, 0) * 45, -1, 400);
    ALakeFish* Big = SpawnFish(World, EFishSpecies::Perch, 1.0f, Bait - FVector(Facing, 0) * 75, -1, 401);
    Small->TestState() = ALakeFish::EState::Investigate;
    Small->TestTargetAngler() = Fisher;
    Small->TestTargetRig() = (int32)Fisher->RodKind;
    Big->SetActorRotation(FRotator(0, Yaw, 0));
    Big->TestHunger() = 1;
    Big->TestSetGoal(EFishGoal::Feed, 1000);
    for (int32 I = 0; I < 32 && Big->IsFree(); ++I) Big->TestUpdateBait();
    TestTrue(TEXT("A dominant local fish can displace a small approaching fish"), Big->IsBusy() && Small->IsFree());
    Big->TestState() = ALakeFish::EState::Roam;
    Big->TestTargetAngler() = nullptr;
    Small->TestState() = ALakeFish::EState::Holding;
    Small->TestTargetAngler() = Fisher;
    for (int32 I = 0; I < 32; ++I) Big->TestUpdateBait();
    TestTrue(TEXT("Competition does not displace a fish already holding the bait"), Big->IsFree() && Small->IsHoldingBait());
    Small->Destroy(); Big->Destroy();
    TArray<ALakeFish*> Pair;
    for (int32 I = 0; I < 2; ++I)
    {
        ALakeFish* F = SpawnFish(World, EFishSpecies::Perch, 0.15f, Bait - FVector(Facing, 0) * 100, -1, 777);
        F->TestState() = ALakeFish::EState::Investigate;
        F->TestTargetAngler() = Fisher;
        F->TestTargetRig() = (int32)Fisher->RodKind;
        F->SetActorRotation(FRotator(0, Yaw, 0)); F->TestYaw() = Yaw;
        Pair.Add(F);
    }
    for (int32 Step = 0; Step < 40; ++Step) for (ALakeFish* F : Pair) F->Simulate(0.05f);
    TestTrue(TEXT("Approaching fish steer apart even from an overlapping starting position"), FVector::Dist(Pair[0]->GetActorLocation(), Pair[1]->GetActorLocation()) > 8);
    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeFeedingSpaceTest, "QuietLake.Fishing.FeedingSpace", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeFeedingSpaceTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ALakeLife* Life = World->SpawnActor<ALakeLife>();
    World->SpawnActor<ALakeWeather>();
    Life->Food().Init();
    const FVector2D At = LakeMap::Centre;
    for (int32 I = 0; I < 5; ++I) Life->Food().AddGroundbait(At, EBait::Bread);
    Life->Food().Step(180);
    TArray<ALakeFish*> Fish;
    for (int32 I = 0; I < 8; ++I)
    {
        ALakeFish* F = SpawnFish(World, EFishSpecies::Roach, 0.15f, FVector(At + FVector2D(I * 40, 0), ALakeFish::BottomZ(At.X, At.Y) + 20), 9191, 600 + I);
        F->TestHunger() = 1;
        Fish.Add(F);
    }
    Life->Step(0);
    Fish[0]->TestSetGoal(EFishGoal::Feed, 1000);
    Fish[0]->TestFeedOn() = EFishFood::Groundbait;
    Fish[0]->TestRoamTarget() = FVector(At, ALakeFish::BottomZ(At.X, At.Y) + 10);
    for (int32 I = 1; I < Fish.Num(); ++I) Fish[I]->TestSetGoal(EFishGoal::Feed, 1000);
    float MaxSpacing = 0;
    for (int32 I = 1; I < Fish.Num(); ++I)
    {
        TestTrue(TEXT("Members join the groundbait meal"), Fish[I]->TestFeedOn() == EFishFood::Groundbait);
        TestTrue(TEXT("A feeding target stays in water"), LakeMap::IsWater(Fish[I]->TestRoamTarget().X, Fish[I]->TestRoamTarget().Y));
        MaxSpacing = FMath::Max(MaxSpacing, (float)FVector::Dist2D(Fish[I]->TestRoamTarget(), Fish[0]->TestRoamTarget()));
    }
    TestTrue(TEXT("A feeding school has individual destinations across the patch"), MaxSpacing > 80);
    for (ALakeFish* F : Fish) F->Destroy();
    const FVector2D Away = At + FVector2D(700, 0);
    ALakeFish* Crucian = SpawnFish(World, EFishSpecies::Crucian, 0.4f, FVector(Away, ALakeFish::BottomZ(Away.X, Away.Y) + 12), -1, 9001);
    Crucian->TestSetGoal(EFishGoal::Feed, 1000);
    Crucian->TestFeedOn() = EFishFood::Groundbait;
    Crucian->TestHunger() = 1;
    Crucian->TestbAtSpot() = true;
    Crucian->TestNextFoodCheck() = 10000;
    Crucian->TestRoamTarget() = Crucian->GetActorLocation();
    Life->Step(0);
    const float Before = Life->Food().Groundbait()[0].Balls;
    Crucian->Simulate(0.05f);
    TestFalse(TEXT("A fish following scent does not root before reaching the food patch"), Crucian->TestbRooting());
    TestEqual(TEXT("No groundbait is eaten from seven metres away"), Life->Food().Groundbait()[0].Balls, Before);
    Crucian->SetActorLocation(FVector(At, ALakeFish::BottomZ(At.X, At.Y) + 12));
    Crucian->TestRoamTarget() = Crucian->GetActorLocation();
    Crucian->TestbAtSpot() = true;
    Crucian->Simulate(1);
    TestTrue(TEXT("At the patch it eats food and becomes less hungry"), Life->Food().Groundbait()[0].Balls < Before && Crucian->GetHunger() < 0.999f);
    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeSizeHabitatTest, "QuietLake.Fishing.SizeHabitat", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeSizeHabitatTest::RunTest(const FString& Parameters)
{
    // The same valid shallow place must be less attractive to a mature individual, while
    // staying possible: shallow feeding visits and predators hunting fry are not forbidden.
    FVector2D Facing;
    const FVector2D Edge = LakeMap::StartSpot(&Facing);
    bool bChecked = false;
    for (float Off = 150; Off < 3000; Off += 50)
    {
        const FVector2D P = Edge + Facing * Off;
        const float Depth = LakeMap::Depth(P.X, P.Y);
        const float Small = LakeFish::HabitatScore(EFishSpecies::Roach, P.X, P.Y, nullptr, 0);
        if (Depth < 65 || Depth > 180 || Small <= 0.01f) continue;
        const float Large = LakeFish::HabitatScore(EFishSpecies::Roach, P.X, P.Y, nullptr, 1.0f);
        TestTrue(TEXT("Mature fish prefer deeper water rather than merely tolerating more depth"), Large < Small * 0.8f);
        TestTrue(TEXT("Large fish can still make occasional shallow visits"), Large > 0);
        bChecked = true;
        break;
    }
    TestTrue(TEXT("A valid shallow habitat was sampled"), bChecked);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeBaitTheftTest, "QuietLake.Fishing.BaitTheft", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeBaitTheftTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    FVector2D Facing;
    const FVector2D Edge = LakeMap::StartSpot(&Facing) + Facing * 90;
    ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(FVector(Edge, 100), FRotator(0, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)), 0));
    Fisher->Bait = EBait::Maggot;
    int32 Stolen = 0;
    for (int32 I = 0; I < 96; ++I)
    {
        FLakeFishingFixture::ImmersedRig(Fisher);
        ALakeFish* Small = SpawnFish(World, EFishSpecies::Roach, 0.05f, Fisher->BaitLocation(), -1, 7000 + I);
        Small->SetActorLocation(Fisher->BaitLocation() - Small->MouthOffset());
        Small->TestState() = ALakeFish::EState::Nibble;
        Small->TestTargetAngler() = Fisher;
        Small->TestTargetRig() = (int32)Fisher->RodKind;
        Small->TestHunger() = 1;
        Small->TestAtBait().Bite.Tries = 2;
        Fisher->ClaimBait(Small);
        Small->TestUpdateBait();
        if (Fisher->FishingState == EFishingState::Waiting && !Fisher->HasBaitInWater())
        {
            ++Stolen;
            TestTrue(TEXT("A bait thief leaves the hook instead of starting a bite"), Small->IsFree());
            FLakeFishingFixture::FreshCast(Fisher);
            TestTrue(TEXT("A new cast restores hook bait"), Fisher->bBaitPresent);
        }
        Small->Destroy();
    }
    AddInfo(FString::Printf(TEXT("Small roach stole maggot in %d of 96 independent nibbling trials"), Stolen));
    TestTrue(TEXT("Small nibbling fish sometimes remove the hook bait"), Stolen > 0 && Stolen < 40);
    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}

#endif
