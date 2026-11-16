#if WITH_DEV_AUTOMATION_TESTS
#include "LakeFish.h"
#include "LakeGame.h"
#include "LakeMap.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

namespace
{
    ALakeFish* LandingFish(UWorld* World, float Depth, float Stamina)
    {
        const FVector At(LakeMap::Centre, LakeFish::SurfaceZ - Depth);
        ALakeCharacter* Angler = World->SpawnActor<ALakeCharacter>(At - FVector(300, 0, 0), FRotator::ZeroRotator);
        ALakeFish* Fish = World->SpawnActor<ALakeFish>(At, FRotator::ZeroRotator);
        Fish->Setup(EFishSpecies::Perch, 0.3f, At, -1, 17);
        Fish->SetActorRotation(FRotator::ZeroRotator);
        Fish->Hook(Angler);
        Fish->TestStamina() = Fish->TestMaxStamina() * Stamina;
        Fish->GetFight().FightIndividual = 1;
        return Fish;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeFishDrawnHeadTest, "QuietLake.Fishing.Landing.DrawnHead", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeFishDrawnHeadTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    for (float FPS : {30.0f, 60.0f, 120.0f})
    {
        ALakeFish* Fish = LandingFish(World, 300, 0.5f);
        float HeadFirst = 0, Samples = 0;
        for (int32 I = 0; I < FPS * 4; ++I)
        {
            Fish->ApplyLineForce(FVector(-300, 0, 0));
            Fish->Simulate(1 / FPS);
            if (I >= FPS * 2)
            {
                HeadFirst += FVector::DotProduct(Fish->GetActorForwardVector(), FVector(-1, 0, 0));
                ++Samples;
            }
        }
        AddInfo(FString::Printf(TEXT("%.0f fps: head along pull %.3f, velocity %s"), FPS, HeadFirst / Samples, *Fish->TestVel().ToCompactString()));
        TestTrue(TEXT("A fish overcome by steady pull follows head first within two seconds"), HeadFirst / Samples > 0.6f);
    }
    ALakeFish* Free = LandingFish(World, 200, 1);
    Free->TestVel() = FVector(80, 0, 0);
    Free->ApplyLineForce(FVector(5, 0, 0));
    Free->GetFight().MoveHooked(FVector(80, 0, 0), 80, 0.1f);
    TestTrue(TEXT("Swimming head first along a light line does not surrender steering"), Free->GetFight().Drawn < 0.1f);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeFishSurfaceRestTest, "QuietLake.Fishing.Landing.SurfaceRest", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeFishSurfaceRestTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ALakeFish* Fish = LandingFish(World, 1, 0.08f);
    auto& Fight = Fish->GetFight();
    const FVector Hook(Fish->MouthLocation().X, Fish->MouthLocation().Y, LakeFish::SurfaceZ + 1);
    float Side = 0, Effort = 0;
    const float Before = Fish->StaminaFraction();
    for (int32 I = 0; I < 360; ++I)
    {
        // A compliant line holds the mouth near the surface, without fixing the body pose.
        FVector Pull = (Hook - Fish->MouthLocation()) * 60 - Fight.Swim.V[0] * 3;
        Pull.Z = FMath::Max(0.0, Pull.Z);
        Fish->ApplyLineForce(Pull.GetClampedToMaxSize(600), 60);
        Fish->Simulate(1 / 60.0f);
        if (I >= 240) { Side += FMath::Abs(Fish->GetActorRightVector().Z); Effort += Fight.Swim.Drive; }
    }
    AddInfo(FString::Printf(TEXT("Surface: side %.3f effort %.3f stamina %.3f -> %.3f water %.3f pose %s"), Side / 120, Effort / 120, Before, Fish->StaminaFraction(), Fish->BodyInWater(), *Fish->GetActorRotation().ToCompactString()));
    TestTrue(TEXT("A spent surface fish lies on its side"), Side / 120 > 0.7f);
    TestTrue(TEXT("A spent surface fish only twitches"), Effort / 120 < 0.12f);
    TestTrue(TEXT("Holding the head at the surface does not recharge a spent fish"), Fish->StaminaFraction() <= Before + 0.01f);

    // Small waves do not switch it back to an escape bout; lowering it well under water does.
    for (float Depth : {4.0f, -2.0f, 5.0f})
    {
        Fish->SetActorLocation(FVector(LakeMap::Centre, LakeFish::SurfaceZ - Depth));
        Fish->ApplyLineForce(FVector::ZeroVector);
        FVector Desired;
        float MaxSpeed = 100;
        Fight.UpdateFightMotion(1 / 60.0f, Desired, MaxSpeed);
        TestTrue(TEXT("A spent fish stays quiet through the surface band"), Desired.IsNearlyZero() && MaxSpeed == 0);
    }
    Fish->SetActorLocation(FVector(LakeMap::Centre, LakeFish::SurfaceZ - 100));
    Fish->TestVel() = FVector::ZeroVector;
    const float Spent = Fish->StaminaFraction();
    Fight.MoveHooked(FVector::ZeroVector, 0, 0.05f);
    TestFalse(TEXT("Lowering the fish releases its surface rest"), Fight.bSurfaceRest);
    TestTrue(TEXT("A submerged pause can restore stamina again"), Fish->StaminaFraction() > Spent);

    ALakeFish* Fresh = LandingFish(World, 200, 1);
    float DeepEffort = 0;
    for (int32 I = 0; I < 180; ++I)
    {
        Fresh->ApplyLineForce(FVector(-80, 0, 0));
        Fresh->Simulate(1 / 60.0f);
        DeepEffort += Fresh->GetFight().Swim.Drive;
    }
    TestTrue(TEXT("A fresh submerged fish still fights"), DeepEffort / 180 > 0.3f);
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeFishAirborneTest, "QuietLake.Fishing.Landing.Airborne", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeFishAirborneTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ALakeFish* Fish = LandingFish(World, -100, 0.7f);
    auto& Fight = Fish->GetFight();
    Fight.MoveHooked(FVector(160, 0, 0), 160, 0.05f);
    FVector Desired;
    float MaxSpeed = 100;
    Fight.UpdateFightMotion(0.05f, Desired, MaxSpeed);
    TestTrue(TEXT("An airborne fish has no swimming destination"), Desired.IsNearlyZero() && MaxSpeed == 0);
    TestTrue(TEXT("An airborne fish falls rather than swimming away"), Fish->TestVel().Z < -20 && Fight.Swim.Drive < 0.12f);

    for (EFishSpecies Species : {EFishSpecies::Perch, EFishSpecies::Crucian, EFishSpecies::Pike})
    for (float Weight : {LakeFish::Info(Species).MinWeight, LakeFish::Info(Species).MaxWeight})
    {
        const FVector Mouth(LakeMap::Centre, LakeFish::SurfaceZ + 150);
        ALakeFish* Hanging = World->SpawnActor<ALakeFish>(Mouth, FRotator::ZeroRotator);
        Hanging->Setup(Species, Weight, Mouth, -1, 17);
        Hanging->LiftOut();
        float MaxError = 0;
        for (int32 I = 0; I < 360; ++I)
        {
            Hanging->HangOnLine(Mouth, FVector::ForwardVector, 1 / 60.0f);
            MaxError = FMath::Max(MaxError, float(FVector::Dist(Hanging->MouthLocation(), Mouth)));
        }
        AddInfo(FString::Printf(TEXT("Hanging %s %.2f kg: mouth error %.5f, pose %s"), *LakeFish::Info(Species).Id, Weight, MaxError, *Hanging->GetActorRotation().ToCompactString()));
        TestTrue(TEXT("Hanging fish stays attached to the hook throughout the turn"), MaxError < 0.01f);
        TestTrue(TEXT("A fully airborne fish hangs head up"), Hanging->GetActorForwardVector().Z > 0.94f);
    }
    World->DestroyWorld(false);
    return true;
}
#endif
