#if WITH_DEV_AUTOMATION_TESTS
#include "LakeFish.h"
#include "LakeGame.h"
#include "LakeMap.h"
#include "LakeRod.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeHeavyLiftInputTest, "QuietLake.Fishing.HeavyLanding.HeldLift", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeHeavyLiftInputTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    for (ERodKind Kind : {ERodKind::Light, ERodKind::Medium, ERodKind::Heavy})
    {
        ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>();
        Fisher->RodKind = Kind;
        Fisher->FishingState = EFishingState::Fight;
        Fisher->RodLift = 0.2f;
        Fisher->bReeling = true;
        Fisher->HandStrain = 1;
        for (int32 I = 0; I < 60; ++I) Fisher->UpdateRodHands(1 / 60.0f, 1500);
        const float Held = Fisher->RodLift;
        TestTrue(TEXT("Held input can keep raising a heavy load below tackle strength"), Held > 0.25f);
        Fisher->bReeling = false;
        Fisher->UpdateRodHands(0.1f, 1500);
        TestTrue(TEXT("Releasing gives to the load instead of continuing the haul"), Fisher->RodLift < Held);
    }
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeHeavyLandingTest, "QuietLake.Fishing.HeavyLanding.PhysicalLift", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeHeavyLandingTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    FVector2D Facing;
    const FVector2D Bank = LakeMap::StartSpot(&Facing);
    const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X));
    for (ERodKind Kind : {ERodKind::Light, ERodKind::Medium, ERodKind::Heavy})
    for (float Weight : {1.2f, 4.0f})
    {
        ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(FVector(Bank, ALakeEnvironment::GroundHeight(Bank.X, Bank.Y) + 95), FRotator(0, Yaw, 0));
        Fisher->Rods[int32(ERodKind::Medium)].Place = ERodPlace::Case;
        Fisher->RodKind = Kind;
        Fisher->Rods[int32(Kind)].Place = ERodPlace::Hands;
        APlayerController* PC = World->SpawnActor<APlayerController>();
        PC->Possess(Fisher);
        PC->SetControlRotation(FRotator(-12, Yaw, 0));
        Fisher->RodVisual = World->SpawnActor<ALakeRod>();
        Fisher->FloatDepth = Fisher->HookDepth = 60;
        Fisher->bRigInHand = true;
        auto Step = [&]
        {
            Fisher->ChargeServerStart -= 1 / 60.0f;
            Fisher->CastServerTime -= 1 / 60.0f;
            Fisher->RigGripTime -= 1 / 60.0f;
            Fisher->LandServerTime -= 1 / 60.0f;
            Fisher->Tick(1 / 60.0f);
        };
        auto Run = [&](int32 Steps) { for (int32 I = 0; I < Steps; ++I) Step(); };
        Run(120);
        Fisher->StartCast(); Run(72);
        Fisher->ServerRelease_Implementation(); Run(300);
        const FVector At = Fisher->RodVisual->HookLocation();
        ALakeFish* Fish = World->SpawnActor<ALakeFish>(At, FRotator::ZeroRotator);
        Fish->Setup(Weight > 2 ? EFishSpecies::Pike : EFishSpecies::Perch, Weight, At, -1, 17);
        Fish->Hook(Fisher);
        Fish->TestStamina() = 0;
        Fisher->HookedFish = Fisher->LineFish = Fish;
        Fisher->FishingState = EFishingState::Fight;
        Fisher->bReeling = true;
        float Peak = 0;
        bool Lifted = false;
        for (int32 I = 0; I < 3600 && Fisher->FishingState != EFishingState::Idle && Fisher->CatchCount == 0; ++I)
        {
            Step();
            Peak = FMath::Max(Peak, Fisher->LineLoadGf);
            Lifted |= Fisher->FishingState == EFishingState::Landing;
            if (Fish->IsHooked() && !Fish->IsLifted()) Fish->Simulate(1 / 60.0f);
        }
        AddInfo(FString::Printf(TEXT("Rod %d, heavy %.1f kg: lifted=%d caught=%d state=%d peak=%.0f lift=%.2f water=%.2f status=%s"), int32(Kind), Weight, Lifted, Fisher->CatchCount, int32(Fisher->FishingState), Peak, Fisher->RodLift, Fish->BodyInWater(), *Fisher->Status));
        if (Weight < 2)
        {
            TestTrue(TEXT("A tired heavy fish within tackle strength can be lifted"), Lifted);
            TestEqual(TEXT("The lifted heavy fish reaches the hand instead of stalling again"), Fisher->CatchCount, 1);
        }
        else
        {
            TestTrue(TEXT("Insisting on an overweight fish ends in tackle failure, not a permanent stall"), Fisher->FishingState == EFishingState::Idle && (Fisher->Status.Contains(TEXT("Леска оборвалась")) || Fisher->Status.Contains(TEXT("Удилище не выдержало"))));
            TestEqual(TEXT("A broken rig awards no catch"), Fisher->CatchCount, 0);
        }
    }
    // Immediately below/above each rig's actual strength, with the rod across the line.
    // This exercises the same failure path used while a fish hangs after leaving the water.
    for (ERodKind Kind : {ERodKind::Light, ERodKind::Medium, ERodKind::Heavy})
    for (float Share : {0.99f, 1.01f})
    {
        ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>();
        Fisher->RodKind = Kind;
        const FLakeTackle Gear = Fisher->Tackle();
        const FVector Axis = Fisher->RodHold().GetUnitAxis(EAxis::X);
        const FVector Across = FVector::CrossProduct(Axis, FVector::UpVector).GetSafeNormal(UE_SMALL_NUMBER, FVector::RightVector);
        const FVector At = Fisher->RodTip() + Across * 600;
        ALakeFish* Fish = World->SpawnActor<ALakeFish>(At, FRotator::ZeroRotator);
        Fish->Setup(EFishSpecies::Pike, Gear.Weakest() * Share / 1000, At, -1, 17);
        Fish->Hook(Fisher);
        Fisher->HookedFish = Fisher->LineFish = Fish;
        Fisher->FishingState = EFishingState::Landing;
        Fisher->UpdateLiftedFish(0);
        if (Share < 1)
            TestTrue(TEXT("Below strength the hanging fish is not rejected by weight"), Fisher->FishingState == EFishingState::Landing);
        else
        {
            TestTrue(TEXT("Crossing strength ends the fight"), Fisher->FishingState == EFishingState::Idle);
            TestTrue(TEXT("The weaker loaded element determines the break message"), Fisher->Status.Contains(Kind == ERodKind::Light ? TEXT("Удилище не выдержало") : TEXT("Леска оборвалась")));
            TestFalse(TEXT("Tackle failure releases the fish"), Fish->IsHooked());
            TestEqual(TEXT("Tackle failure does not award a fish"), Fisher->CatchCount, 0);
        }
    }
    World->DestroyWorld(false);
    return true;
}
#endif
