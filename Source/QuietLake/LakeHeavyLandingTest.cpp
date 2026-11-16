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
        Fisher->bReeling = true;
        auto Hold = [&](float Seconds, float Load) { for (int32 I = 0; I < FMath::RoundToInt(Seconds * 60); ++I) Fisher->UpdateRodHands(1 / 60.0f, Load); };
        // A fish in the water pulling harder than the hands drags the rod down, LMB held or not.
        Fisher->RodLift = 0.5f; Fisher->HandStrain = 0;
        Hold(1, 1500);
        TestTrue(TEXT("A pull beyond the hands drags the held rod down"), Fisher->RodLift < 0.5f);
        // A light load is lifted.
        Fisher->RodLift = 0.2f;
        Hold(0.5f, 100);
        TestTrue(TEXT("A light load is lifted"), Fisher->RodLift > 0.2f);
        // Heaving a fish out: the hands manage a load beyond their steady pull, but only briefly.
        Fisher->RodLift = 0.3f; Fisher->HandStrain = 1; Fisher->HeaveReserve = 1;
        Hold(0.5f, 800);
        const float Raised = Fisher->RodLift;
        TestTrue(TEXT("A heave lifts more than the steady pull"), Raised > 0.3f);
        Hold(4.5f, 800);
        TestTrue(TEXT("The heave is spent after a few seconds"), Fisher->HeaveReserve < 0.1f);
        TestTrue(TEXT("Spent arms let the rod down"), Fisher->RodLift < Raised - 0.1f);
        Hold(7, 100);
        TestTrue(TEXT("The arms recover under a light load"), Fisher->HeaveReserve > 0.9f);
        // Let go, the rod gives to the load.
        const float Held = Fisher->RodLift;
        Fisher->bReeling = false;
        Fisher->UpdateRodHands(0.1f, 1500);
        TestTrue(TEXT("Releasing gives to the load"), Fisher->RodLift < Held);
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
    for (float Weight : {0.8f, 1.0f, 1.2f, 4.0f})
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
        if (Weight < 0.9f)
        {
            TestTrue(TEXT("A tired fish the arms can heave is lifted out"), Lifted);
            TestEqual(TEXT("The heaved fish reaches the hand"), Fisher->CatchCount, 1);
        }
        else if (Weight > 2)
        {
            TestFalse(TEXT("Held LMB does not winch out a fish beyond the arms"), Lifted);
            TestTrue(TEXT("Nor does it break the tackle on a tired fish"), Fisher->FishingState == EFishingState::Fight && Fish->IsHooked());
            TestEqual(TEXT("The hint says to lead it to the shallows"), Fisher->FightHint, 10);
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
