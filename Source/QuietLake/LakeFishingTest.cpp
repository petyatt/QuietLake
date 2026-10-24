#if WITH_DEV_AUTOMATION_TESTS
#include "LakeGame.h"
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeFishingTest, "QuietLake.Fishing.ServerRules", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeFishingTest::RunTest(const FString& Parameters)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ALakeCharacter* Fisher = World->SpawnActor<ALakeCharacter>(FVector(-550, 0, 100), FRotator::ZeroRotator);
    APlayerController* Controller = World->SpawnActor<APlayerController>();
    Controller->Possess(Fisher);
    Controller->SetControlRotation(FRotator(0, 180, 0));
    Fisher->ServerFish_Implementation();
    TestEqual(TEXT("Cast toward land rejected"), Fisher->FishingState, 0);
    Fisher->NextActionTime = 0;
    Controller->SetControlRotation(FRotator::ZeroRotator);
    Fisher->ServerFish_Implementation();
    TestEqual(TEXT("Cast toward water accepted"), Fisher->FishingState, 1);
    Fisher->ServerFish_Implementation();
    TestEqual(TEXT("Rapid repeated input ignored"), Fisher->FishingState, 1);
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    TestEqual(TEXT("Early strike gives no catch"), Fisher->CatchCount, 0);
    TestEqual(TEXT("Early strike ends cast"), Fisher->FishingState, 0);
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    Fisher->Bite();
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    TestEqual(TEXT("Strike begins fight without awarding fish"), Fisher->CatchCount, 0);
    TestEqual(TEXT("Fight starts"), Fisher->FishingState, 3);
    for (int32 Step = 0; Step < 1000 && Fisher->FishingState == 3; ++Step)
    {
        if (Fisher->Tension > 0.65f) Fisher->ServerRelease_Implementation();
        else if (Fisher->Tension < 0.25f) Fisher->ServerFish_Implementation();
        Fisher->UpdateFight(0.05f);
    }
    TestEqual(TEXT("Controlled reeling awards one fish"), Fisher->CatchCount, 1);
    TestEqual(TEXT("Successful catch ends cast"), Fisher->FishingState, 0);
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    Fisher->Bite();
    Fisher->Escape();
    TestEqual(TEXT("Missed bite awards no fish"), Fisher->CatchCount, 1);
    TestEqual(TEXT("Missed bite ends cast"), Fisher->FishingState, 0);
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    Fisher->ServerCancel_Implementation();
    TestEqual(TEXT("Cancel ends cast"), Fisher->FishingState, 0);
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    Fisher->Bite();
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    for (int32 Step = 0; Step < 100 && Fisher->FishingState == 3; ++Step) Fisher->UpdateFight(0.05f);
    TestEqual(TEXT("Continuous reeling breaks the line"), Fisher->FishingState, 0);
    TestEqual(TEXT("Broken line awards no fish"), Fisher->CatchCount, 1);
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    Fisher->Bite();
    Fisher->NextActionTime = 0;
    Fisher->ServerFish_Implementation();
    Fisher->ServerRelease_Implementation();
    for (int32 Step = 0; Step < 1300 && Fisher->FishingState == 3; ++Step) Fisher->UpdateFight(0.05f);
    TestEqual(TEXT("Abandoned fight times out"), Fisher->FishingState, 0);
    TestEqual(TEXT("Timeout awards no fish"), Fisher->CatchCount, 1);
    World->DestroyWorld(false);
    return true;
}
#endif
