// -FishingBench: the whole of a catch played by a scripted hand, measured and logged (BENCH lines):
// the cast for a given hold, the bite as the float shows it, the strike, the fight sampled four times
// a second, the lift out. Several fish in one run:
//   -BenchCases=roach:0.15,crucian:0.8,cast:6   (cast:<metres> only casts at a spot that far out)
// The hand plays as any player does: LMB held from the strike until the fish is in the hand.
// -BenchDirect hooks the fish without waiting for its bite.
#include "LakeGame.h"
#include "LakeFish.h"
#include "LakeMap.h"
#include "LakeRod.h"
#include "LakeWeather.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UnrealClient.h"

namespace
{
    struct FBenchCase
    {
        bool bCastOnly = false;
        EFishSpecies Species = EFishSpecies::Roach;
        float Weight = 0.15f;
        bool bEases = true;
        float Hold = 1.0f;
    };
    struct FBench
    {
        TArray<FBenchCase> Cases;
        int32 Index = 0, Stage = 0, Caught = 0, Attempts = 0, Samples = 0, Shots = 0;
        float StageStart = 0, NextLog = 0, BiteSeen = -1, FightStart = -1, ActAt = -1;
        bool bWantHeld = false, bHeld = false, bStruck = false, bNamed = false;
        EBiteMotion LastMotion = EBiteMotion::None;
        EFishingState LastState = EFishingState::Idle;
        TWeakObjectPtr<ALakeFish> Fish;
        float MaxTension = 0, SumTension = 0, HardTime = 0, SlackTime = 0, MaxSpeed = 0, MaxDepth = 0, MaxOut = 0, HeldTime = 0;
        FTimerHandle Timer;
        int32 Frames = 0;
        float RateFrom = 0;
        uint64 RateFrame = 0;
    };
}

void ALakeCharacter::RunFishingBench()
{
#if !UE_BUILD_SHIPPING
    if (!FParse::Param(FCommandLine::Get(), TEXT("FishingBench")) || GetNetMode() != NM_Standalone) return;
    auto Bench = MakeShared<FBench>();
    FString List = TEXT("cast:3,cast:6,cast:9,roach:0.12,crucian:0.8");
    FParse::Value(FCommandLine::Get(), TEXT("BenchCases="), List, false);
    TArray<FString> Items;
    List.ParseIntoArray(Items, TEXT(","), true);
    for (const FString& Item : Items)
    {
        TArray<FString> Parts;
        Item.ParseIntoArray(Parts, TEXT(":"), true);
        if (Parts.Num() < 2) continue;
        FBenchCase Case;
        if (Parts[0].Equals(TEXT("cast"), ESearchCase::IgnoreCase))
        {
            Case.bCastOnly = true;
            Case.Hold = FCString::Atof(*Parts[1]);
        }
        else
        {
            bool bFound = false;
            for (int32 S = 0; S < LakeFish::NumSpecies(); ++S)
                if (LakeFish::Info((EFishSpecies)S).Id.Equals(Parts[0], ESearchCase::IgnoreCase)) { Case.Species = (EFishSpecies)S; bFound = true; }
            if (!bFound) { UE_LOG(LogTemp, Warning, TEXT("BENCH unknown species %s"), *Parts[0]); continue; }
            Case.Weight = FCString::Atof(*Parts[1]);
            Case.bEases = Parts.Num() < 3 || !Parts[2].Equals(TEXT("hold"), ESearchCase::IgnoreCase);
            if (Parts.Num() > 3) Case.Hold = FCString::Atof(*Parts[3]);
        }
        Bench->Cases.Add(Case);
    }
    const bool bDirect = FParse::Param(FCommandLine::Get(), TEXT("BenchDirect"));
    const bool bShots = FParse::Param(FCommandLine::Get(), TEXT("BenchShots"));
    float Depth = 110;
    FParse::Value(FCommandLine::Get(), TEXT("BenchDepth="), Depth);

    GetWorldTimerManager().SetTimer(Bench->Timer, FTimerDelegate::CreateWeakLambda(this, [this, Bench, bDirect, bShots, Depth]
    {
        FBench& B = *Bench;
        const float Now = ServerNow();
        auto Go = [&B, Now](int32 Stage) { B.Stage = Stage; B.StageStart = Now; };
        const float In = Now - B.StageStart;
        auto Hold = [this, &B](bool bDown)
        {
            if (bDown == B.bHeld) return;
            B.bHeld = bDown;
            if (bDown) { NextActionTime = 0; ServerFish_Implementation(); } else ServerRelease_Implementation();
        };
        // The frame rate the measurements were taken at: a starved machine runs the physics in slow motion.
        ++B.Frames;
        if (Now - B.RateFrom >= 10)
        {
            UE_LOG(LogTemp, Display, TEXT("BENCH frame rate %.0f fps"), (GFrameCounter - B.RateFrame) / (Now - B.RateFrom));
            B.RateFrom = Now; B.RateFrame = GFrameCounter;
        }
        if (B.Index >= B.Cases.Num())
        {
            UE_LOG(LogTemp, Display, TEXT("BENCH done: %d cases"), B.Cases.Num());
            GetWorldTimerManager().ClearTimer(B.Timer);
            FPlatformMisc::RequestExit(false);
            return;
        }
        const FBenchCase& Case = B.Cases[B.Index];
        const FString Label = Case.bCastOnly ? FString::Printf(TEXT("cast %.1f m"), Case.Hold)
            : FString::Printf(TEXT("%s %.2f kg %s"), *LakeFish::Info(Case.Species).Id, Case.Weight, TEXT(""));
        switch (B.Stage)
        {
        case 0:   // the place, the weather, the rod
            if (Now < 4) return;
            if (ALakeWeather* Weather = ALakeWeather::Get(GetWorld()))
            {
                FLakeWeatherState Calm;
                Calm.Cloud = 0.2f; Calm.Fog = 0; Calm.Rain = 0; Calm.Wind = 0.03f; Calm.WindYaw = 40;
                Weather->SetWeather(Calm, 1);
                Weather->SetHours(9.0f);
            }
            {
                FVector2D Facing;
                const FVector2D Spot = LakeMap::StartSpot(&Facing) + Facing * 60;
                SetActorLocation(FVector(Spot, ALakeEnvironment::GroundHeight(Spot.X, Spot.Y) + 95), false, nullptr, ETeleportType::TeleportPhysics);
                if (APlayerController* PC = Cast<APlayerController>(GetController()))
                    PC->SetControlRotation(FRotator(-10, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)), 0));
            }
            FloatDepth = Depth;
            Go(1);
            return;
        case 1:   // ready to cast: the rig in the free hand
            if (FishingState == EFishingState::Idle && bRigInHand && In > 1.0f)
            {
                // Look at the water this far out (cast:<metres>; a fish case casts 5 m).
                if (APlayerController* PC = Cast<APlayerController>(GetController()))
                {
                    const float Out = (Case.bCastOnly ? Case.Hold : 8.0f) * 100;
                    const float Down = FMath::RadiansToDegrees(FMath::Atan2(GetActorLocation().Z + 64 - LakeFish::SurfaceZ, Out));
                    PC->SetControlRotation(FRotator(-Down, PC->GetControlRotation().Yaw, 0));
                }
                UE_LOG(LogTemp, Display, TEXT("BENCH case %d: %s"), B.Index, *Label);
                Hold(true);
                Go(2);
            }
            else if (In > 30)
            {
                UE_LOG(LogTemp, Display, TEXT("BENCH case %d: the rig never came to the hand (state %d, in hand %d): %s"), B.Index, (int32)FishingState, bRigInHand ? 1 : 0, *Status);
                FinishFishing(TEXT("bench reset"));
                bRigInHand = true;
                B.StageStart = Now;
            }
            return;
        case 2:   // the swing held, then let go
            // (-BenchShots: the aim ring as the player sees it, LMB held.)
            if (bShots && In >= 0.4f && B.Shots == 0 && Case.bCastOnly)
            {
                ++B.Shots;
                FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / FString::Printf(TEXT("Bench_aim_%d.png"), B.Index), true, false);
            }
            if (In >= (bShots && Case.bCastOnly ? 1.0f : 0.6f)) { Hold(false); Go(3); }
            return;
        case 3:   // the rig comes down
            if (FishingState == EFishingState::Waiting && !bRigFlying && In > 0.3f)
            {
                const FVector Float = RodVisual ? RodVisual->FloatKeel() : FVector(FloatPosition);
                const FVector Hook = RodVisual ? RodVisual->HookLocation() : FVector(BaitPoint);
                UE_LOG(LogTemp, Display, TEXT("BENCH cast aimed %.0f cm out (reach %.0f): flight %.2f s, float %.0f cm out, %.0f cm aside, hook %.0f cm beyond the float, water there %.0f cm | %s"),
                    FVector::Dist2D(FVector(CastTarget), GetActorLocation()), CastReach(), CastFlight, FVector::DotProduct(Float - GetActorLocation(), GetActorForwardVector()),
                    FVector::DotProduct(Float - GetActorLocation(), GetActorRightVector()),
                    FVector::DotProduct(Hook - Float, GetActorForwardVector()), LakeMap::Depth(Float.X, Float.Y), *Status);
                Go(Case.bCastOnly ? 8 : 4);
            }
            else if (In > 8)
            {
                UE_LOG(LogTemp, Display, TEXT("BENCH cast aimed %.2f m: no landing (state %d flying %d retrieving %d) | %s"), Case.Hold, (int32)FishingState, bRigFlying ? 1 : 0, bRetrieving ? 1 : 0, *Status);
                Go(8);
            }
            return;
        case 4:   // the rig settles, then the fish comes
        {
            if (In < 2.5f) return;
            const FVector Bait = BaitLocation();
            ALakeFish* Fish = GetWorld()->SpawnActor<ALakeFish>(Bait, FRotator(0, GetActorRotation().Yaw + 90, 0));
            Fish->Setup(Case.Species, Case.Weight, Bait, -1, 700 + B.Index);
            B.Fish = Fish;
            B.BiteSeen = -1; B.bStruck = false; B.Attempts = 0; B.LastMotion = EBiteMotion::None;
            if (bDirect)
            {
                Fish->SetActorLocation(Bait - Fish->GetActorQuat().RotateVector(Fish->MouthOffset()));
                Fish->Hook(this);
                HookedFish = Fish; LineFish = Fish; FishingState = EFishingState::Fight;
                Tension = 0; FightTime = 0; FightHint = -1; SlackTime = 0; bReeling = false; RodLift = 0;
            }
            else
            {
                Fish->SetActorLocation(Bait - Fish->GetActorQuat().RotateVector(Fish->MouthOffset()) + FVector(40, 0, 0));
                Fish->State = ALakeFish::EState::Investigate;
                Fish->TargetAngler = this;
            }
            Go(5);
            return;
        }
        case 5:   // the bite: what the float does, and the strike
        {
            if (BiteMotion != B.LastMotion)
            {
                B.LastMotion = BiteMotion;
                UE_LOG(LogTemp, Display, TEXT("BENCH   float t=%.1f motion %d amount %.1f state %d | %s"), In, (int32)BiteMotion, BiteAmount, (int32)FishingState, *Status);
            }
            if (FishingState == EFishingState::Fight)
            {
                B.FightStart = Now; B.NextLog = 0; B.Samples = 0;
                B.MaxTension = B.SumTension = B.HardTime = B.SlackTime = B.MaxSpeed = B.MaxDepth = B.MaxOut = B.HeldTime = 0;
                B.bWantHeld = true; B.ActAt = -1; B.bNamed = false;
                UE_LOG(LogTemp, Display, TEXT("BENCH   hooked after %.1f s, %d strikes | %s"), In, B.Attempts, *Status);
                Hold(true);
                Go(6);
                return;
            }
            if (FishingState == EFishingState::Bite)
            {
                if (B.BiteSeen < 0) { B.BiteSeen = Now; B.bStruck = false; }
                // A human sees the float go and strikes about a third of a second later.
                if (!B.bStruck && Now - B.BiteSeen > 0.35f)
                {
                    B.bStruck = true; ++B.Attempts;
                    ServerCancel_Implementation();
                    Hold(true);
                }
            }
            else
            {
                if (B.BiteSeen >= 0) { UE_LOG(LogTemp, Display, TEXT("BENCH   missed (strike %d) | %s"), B.Attempts, *Status); Hold(false); }
                B.BiteSeen = -1;
                if (FishingState == EFishingState::Idle || In > 70 || B.Attempts >= 4)
                {
                    UE_LOG(LogTemp, Display, TEXT("BENCH result %s: NOT HOOKED after %.0f s, %d strikes | %s"), *Label, In, B.Attempts, *Status);
                    if (ALakeFish* Fish = B.Fish.Get()) Fish->Destroy();
                    Go(8);
                }
            }
            return;
        }
        case 6:   // the fight and the lift out
        {
            // Whatever took the bait: a wild fish may get there before the one put by it.
            ALakeFish* Fish = HookedFish.Get() ? HookedFish.Get() : LineFish.Get();
            if (Fish && !B.bNamed)
            {
                B.bNamed = true;
                UE_LOG(LogTemp, Display, TEXT("BENCH   on the hook: %s %.2f kg"), *LakeFish::Info(Fish->Species).Id, Fish->WeightKg);
            }
            const float T = Now - B.FightStart;
            const bool bOn = FishingState == EFishingState::Fight || FishingState == EFishingState::Landing;
            if (bOn && Fish)
            {
                // The hand follows the bar: past the notch it lets go, a quarter of a second late.
                bool bWant = true;
                const float Notch = Tackle().HookSafe / Tackle().Weakest();
                if (false && Case.bEases && FishingState == EFishingState::Fight) bWant = B.bWantHeld ? Tension < Notch : Tension < Notch * 0.6f;
                if (bWant != B.bWantHeld) { B.bWantHeld = bWant; B.ActAt = Now + 0.25f; }
                if (B.ActAt >= 0 && Now >= B.ActAt) { Hold(B.bWantHeld); B.ActAt = -1; }
                const FVector Mouth = Fish->MouthLocation();
                const float Out = FVector::Dist2D(Mouth, GetActorLocation());
                const float Down = LakeFish::SurfaceZ - Mouth.Z;
                constexpr float Step = 0.05f;
                ++B.Samples;
                B.MaxTension = FMath::Max(B.MaxTension, Tension);
                B.SumTension += Tension;
                if (Tension > 0.75f) B.HardTime += Step;
                if (Tension < 0.05f) B.SlackTime += Step;
                if (B.bHeld) B.HeldTime += Step;
                B.MaxSpeed = FMath::Max(B.MaxSpeed, Fish->SpeedNow());
                B.MaxDepth = FMath::Max(B.MaxDepth, Down);
                B.MaxOut = FMath::Max(B.MaxOut, Out);
                if (Now >= B.NextLog)
                {
                    B.NextLog = Now + 0.25f;
                    UE_LOG(LogTemp, Display, TEXT("BENCH   t=%5.2f st=%d lmb=%d T=%.3f F=%4.0f gf lift=%.2f elev=%4.1f fish: out %3.0f down %3.0f speed %3.0f stamina %.2f phase %d awake %d slack %.1f tip-mouth %3.0f bend %3.0f land %.2f hookz %3.0f grip %.2f"),
                        T, (int32)FishingState, B.bHeld ? 1 : 0, Tension, LoadOnFish(Fish).Size(), RodLift, RodElevation, Out, Down,
                        Fish->SpeedNow(), Fish->StaminaFraction(), (int32)Fish->FightPhase, Fish->IsFightAwake() ? 1 : 0, SlackTime,
                        RodVisual ? (float)FVector::Dist(RodVisual->Tip(), Mouth) : -1.0f, RodVisual ? RodVisual->TipBend() : -1.0f,
                        LandProgress, RodVisual ? float(RodVisual->HookLocation().Z - LakeFish::SurfaceZ) : 0.0f, Fish->HookGrip);
                }
                if (bShots && B.Shots < 3 && ((B.Shots == 0 && T > 2.5f) || (B.Shots == 1 && T > 8) || (B.Shots == 2 && FishingState == EFishingState::Landing)))
                    FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / FString::Printf(TEXT("Bench_%d_%d.png"), B.Index, B.Shots++), true, false);
            }
            const bool bCaught = CatchCount > B.Caught;
            if (bCaught || !bOn || T > 150)
            {
                const float Mean = B.Samples ? B.SumTension / B.Samples : 0;
                UE_LOG(LogTemp, Display, TEXT("BENCH result %s: %s in %.1f s | tension mean %.2f max %.2f, hard %.1f s, slack %.1f s, LMB held %.0f%% | fish top speed %.0f cm/s, deepest %.0f cm, farthest %.0f cm | %s"),
                    *Label, bCaught ? TEXT("CAUGHT") : T > 150 ? TEXT("TIMEOUT") : TEXT("LOST"), T, Mean, B.MaxTension, B.HardTime, B.SlackTime,
                    B.Samples ? 100 * B.HeldTime / (B.Samples * 0.05f) : 0, B.MaxSpeed, B.MaxDepth, B.MaxOut, *Status);
                B.Caught = CatchCount;
                if (T > 150) FinishFishing(TEXT("bench timeout"));
                Go(8);
            }
            return;
        }
        case 8:   // put everything away for the next case
            Hold(false);
            if (In > 0.5f)
            {
                if (ALakeFish* Fish = B.Fish.Get(); Fish && FishingState != EFishingState::Landing && !bFishInHand) Fish->Destroy();
                Go(9);
            }
            return;
        case 9:   // lift the rig out if it is still in the water
            if (FishingState == EFishingState::Waiting && !bRigInHand) Hold(true);
            if ((FishingState == EFishingState::Idle && bRigInHand) || In > 40)
            {
                if (Case.bCastOnly) UE_LOG(LogTemp, Display, TEXT("BENCH   rig back in the hand %.1f s after LMB (in hand %d)"), In, bRigInHand ? 1 : 0);
                Hold(false);
                ++B.Index;
                B.Shots = 0;
                Go(1);
            }
            return;
        }
    }), 0.05f, true);
#endif
}
