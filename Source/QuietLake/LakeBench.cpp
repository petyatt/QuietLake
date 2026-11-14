// -FishingBench: the whole of a catch played by a scripted hand, measured and logged (BENCH lines):
// the cast for a given hold, the bite as the float shows it, the strike, the fight sampled four times
// a second, the lift out. Several fish in one run:
//   -BenchCases=roach:0.15,crucian:0.8,cast:1.2   (cast:<seconds LMB held> only casts; a fish case holds 1.2 s)
// The hand plays as any player does: LMB held from the strike until the fish is in the hand.
// -BenchDirect hooks the fish without waiting for its bite.
#include "LakeGame.h"
#include "LakeFish.h"
#include "LakeMap.h"
#include "LakeRod.h"
#include "LakeWeather.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
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
        bool bSlackBreak = false;        // "ease": LMB let go from 4 to 7 s of the fight (the fish gets slack)
        ERodKind Rod = ERodKind::Medium; // "light" / "heavy": that rod in the hands
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
        TWeakObjectPtr<ACameraActor> SideCam;
        int32 SideShots = 0, LandShots = 0;
        float NextLandShot = 0;
        float MaxTension = 0, SumTension = 0, HardTime = 0, SlackTime = 0, MaxSpeed = 0, MaxDepth = 0, MaxOut = 0, HeldTime = 0, TailFirstTime = 0, MovingTime = 0;
        FTimerHandle Timer;
        int32 Frames = 0;
        float ReleaseOut = 0, ReleaseHeight = 0, ReleasedAt = 0, NextPath = 0, NextCastShot = 0;
        int32 CastShots = 0;
        bool bByItself = false;
        float RateFrom = 0;
        uint64 RateFrame = 0;
    };
}

void ALakeCharacter::RunFishingBench()
{
#if !UE_BUILD_SHIPPING
    if (!FParse::Param(FCommandLine::Get(), TEXT("FishingBench")) || GetNetMode() != NM_Standalone) return;
    auto Bench = MakeShared<FBench>();
    FString List = TEXT("cast:0.3,cast:0.8,cast:1.4,cast:2.5,roach:0.12,crucian:0.8");
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
            for (int32 K = 2; K < Parts.Num(); ++K)
            {
                if (Parts[K].Equals(TEXT("ease"), ESearchCase::IgnoreCase)) Case.bSlackBreak = true;
                else if (Parts[K].Equals(TEXT("light"), ESearchCase::IgnoreCase)) Case.Rod = ERodKind::Light;
                else if (Parts[K].Equals(TEXT("heavy"), ESearchCase::IgnoreCase)) Case.Rod = ERodKind::Heavy;
            }
        }
        Bench->Cases.Add(Case);
    }
    const bool bDirect = FParse::Param(FCommandLine::Get(), TEXT("BenchDirect"));
    const bool bShots = FParse::Param(FCommandLine::Get(), TEXT("BenchShots"));
    // -BenchSide: a camera beside the fish films the fight (under the water, ClearWater shows it) and
    // the lift out (from the side, above the water): Saved/BenchSide_<case>_<n>.png.
    const bool bSide = FParse::Param(FCommandLine::Get(), TEXT("BenchSide"));
    float Depth = 110;
    FParse::Value(FCommandLine::Get(), TEXT("BenchDepth="), Depth);

    // The rig's way during a cast, ten times a second: out ahead, aside (+ right), up from the water,
    // speed, and the rod's angle (cast:<s> cases only, -BenchPath).
    const bool bPath = FParse::Param(FCommandLine::Get(), TEXT("BenchPath"));
    const bool bCastSide = FParse::Param(FCommandLine::Get(), TEXT("BenchSide"));
    auto LogRigPath = [this, bPath, bCastSide](FBench& B, float Now)
    {
        // (-BenchSide on a cast case: the whole cast from the side, a frame every 0.3 s.)
        if (bCastSide && B.Index < B.Cases.Num() && B.Cases[B.Index].bCastOnly && Now >= B.NextCastShot && B.CastShots < 14 && (bCharging || bRigFlying))
        {
            APlayerController* PC = Cast<APlayerController>(GetController());
            if (!B.SideCam.IsValid() && PC)
            {
                B.SideCam = GetWorld()->SpawnActor<ACameraActor>();
                B.SideCam->GetCameraComponent()->SetFieldOfView(70);
                PC->SetViewTarget(B.SideCam.Get());
            }
            if (ACameraActor* Cam = B.SideCam.Get())
            {
                const FVector Ahead = GetActorLocation() + GetActorForwardVector() * 420 + FVector(0, 0, 120);
                const FVector Eye = Ahead + GetActorRightVector() * 1150 + FVector(0, 0, 60);
                Cam->SetActorLocationAndRotation(Eye, (Ahead - Eye).Rotation());
            }
            B.NextCastShot = Now + 0.3f;
            FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / FString::Printf(TEXT("BenchCast_%d_%02d.png"), B.Index, B.CastShots++), false, false);
        }
        if (!bPath || !RodVisual || Now < B.NextPath) return;
        B.NextPath = Now + 0.1f;
        const FVector Shot = RodVisual->ShotLocation() - GetActorLocation();
        const FVector Tip = RodVisual->Tip() - GetActorLocation();
        UE_LOG(LogTemp, Display, TEXT("BENCH   path t=%.1f rod %4.1f lmb %d | shot out %4.0f aside %4.0f up %4.0f speed %4.0f | tip out %4.0f up %4.0f"),
            Now - B.StageStart, RodElevation, bCharging ? 1 : 0, FVector::DotProduct(Shot, GetActorForwardVector()), FVector::DotProduct(Shot, GetActorRightVector()),
            RodVisual->ShotLocation().Z - LakeFish::SurfaceZ, RodVisual->ShotVelocity().Size(), FVector::DotProduct(Tip, GetActorForwardVector()), RodVisual->Tip().Z - LakeFish::SurfaceZ);
    };
    GetWorldTimerManager().SetTimer(Bench->Timer, FTimerDelegate::CreateWeakLambda(this, [this, Bench, bDirect, bShots, bSide, Depth, LogRigPath]
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
        const FString Label = Case.bCastOnly ? FString::Printf(TEXT("cast held %.2f s"), Case.Hold)
            : FString::Printf(TEXT("%s %.2f kg %s"), *LakeFish::Info(Case.Species).Id, Case.Weight, Case.bSlackBreak ? TEXT("ease 4-7 s") : Case.Rod == ERodKind::Light ? TEXT("light rod") : Case.Rod == ERodKind::Heavy ? TEXT("heavy rod") : TEXT(""));
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
            if (RodKind != Case.Rod && FishingState == EFishingState::Idle) { UseRod(Case.Rod); B.StageStart = Now; return; }
            if (FishingState == EFishingState::Idle && bRigInHand && In > 1.0f)
            {
                // Facing the water, looking a little down at it, as a player casting would.
                if (APlayerController* PC = Cast<APlayerController>(GetController()))
                    PC->SetControlRotation(FRotator(-8, PC->GetControlRotation().Yaw, 0));
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
        case 2:   // LMB held for the cast, then let go (or the rig comes down by itself, held to the end)
        {
            LogRigPath(B, Now);
            // (-BenchShots: the cast as the player sees it, at the middle of the hold.)
            const float HoldFor = Case.bCastOnly ? Case.Hold : 1.2f;
            if (bShots && In >= HoldFor * 0.5f && B.Shots == 0 && Case.bCastOnly)
            {
                ++B.Shots;
                FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / FString::Printf(TEXT("Bench_cast_%d.png"), B.Index), true, false);
            }
            const bool bDownByItself = !bCharging && FishingState == EFishingState::Waiting;
            if (In >= HoldFor || bDownByItself)
            {
                B.ReleaseOut = RodVisual ? (float)FVector::DotProduct(RodVisual->ShotLocation() - GetActorLocation(), GetActorForwardVector()) : 0.0f;
                B.ReleaseHeight = RodVisual ? float(RodVisual->ShotLocation().Z - LakeFish::SurfaceZ) : 0.0f;
                B.ReleasedAt = In;
                B.bByItself = bDownByItself;
                Hold(false);
                Go(3);
            }
            return;
        }
        case 3:   // the rig comes down
            if (bRigFlying) LogRigPath(B, Now);
            if (FishingState == EFishingState::Waiting && !bRigFlying && In > 0.3f)
            {
                const FVector Float = RodVisual ? RodVisual->FloatKeel() : FVector(FloatPosition);
                const FVector Hook = RodVisual ? RodVisual->HookLocation() : FVector(BaitPoint);
                UE_LOG(LogTemp, Display, TEXT("BENCH cast held %.2f s%s (shot %.0f cm out, %.0f cm up then): down after %.2f s, float %.0f cm out, %.0f cm aside, hook %.0f cm beyond the float, water there %.0f cm | %s"),
                    B.ReleasedAt, B.bByItself ? TEXT(" (came down by itself)") : TEXT(""), B.ReleaseOut, B.ReleaseHeight, CastFlight, FVector::DotProduct(Float - GetActorLocation(), GetActorForwardVector()),
                    FVector::DotProduct(Float - GetActorLocation(), GetActorRightVector()),
                    FVector::DotProduct(Hook - Float, GetActorForwardVector()), LakeMap::Depth(Float.X, Float.Y), *Status);
                Go(Case.bCastOnly ? 8 : 4);
            }
            else if (In > 8)
            {
                UE_LOG(LogTemp, Display, TEXT("BENCH cast held %.2f s: no landing (state %d flying %d retrieving %d) | %s"), Case.Hold, (int32)FishingState, bRigFlying ? 1 : 0, bRetrieving ? 1 : 0, *Status);
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
                B.MaxTension = B.SumTension = B.HardTime = B.SlackTime = B.MaxSpeed = B.MaxDepth = B.MaxOut = B.HeldTime = B.TailFirstTime = B.MovingTime = 0;
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
                if (Case.bSlackBreak && FishingState == EFishingState::Fight && T > 4 && T < 7) bWant = false;
                (void)Notch;
                if (bWant != B.bWantHeld) { B.bWantHeld = bWant; B.ActAt = Now + 0.25f; }
                if (B.ActAt >= 0 && Now >= B.ActAt) { Hold(B.bWantHeld); B.ActAt = -1; }
                // As the hints say: a fish too heavy to lift is led to the bank by stepping back; one in
                // the shallows is stepped over to.
                if (FishingState == EFishingState::Fight && FightHint == 10) AddMovementInput(-GetActorForwardVector(), 0.5f);
                else if (FishingState == EFishingState::Fight && FightHint == 9) AddMovementInput((Fish->GetActorLocation() - GetActorLocation()).GetSafeNormal2D(), 0.5f);
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
                // (Moving tail first: drawn through the water backwards, which a hooked fish never is.)
                if (FishingState == EFishingState::Fight && Fish->Velocity().Size() > 15)
                {
                    B.MovingTime += Step;
                    if (FVector::DotProduct(Fish->Velocity().GetSafeNormal(), Fish->GetActorForwardVector()) < -0.3f) B.TailFirstTime += Step;
                }
                if (Now >= B.NextLog)
                {
                    B.NextLog = Now + 0.25f;
                    UE_LOG(LogTemp, Display, TEXT("BENCH   t=%5.2f st=%d lmb=%d T=%.3f F=%4.0f gf lift=%.2f elev=%4.1f fish: out %3.0f down %3.0f speed %3.0f stamina %.2f phase %d awake %d slack %.1f tip-mouth %3.0f bend %3.0f land %.2f hookz %3.0f grip %.2f rodload %4.0f inwater %.2f hands %4.0f hook-hand %3.0f line-shoulder %3.0f face %.2f hint %d go %.2f"),
                        T, (int32)FishingState, B.bHeld ? 1 : 0, Tension, LoadOnFish(Fish).Size(), RodLift, RodElevation, Out, Down,
                        Fish->SpeedNow(), Fish->StaminaFraction(), (int32)Fish->FightPhase, Fish->IsFightAwake() ? 1 : 0, SlackTime,
                        RodVisual ? (float)FVector::Dist(RodVisual->Tip(), Mouth) : -1.0f, RodVisual ? RodVisual->TipBend() : -1.0f,
                        LandProgress, RodVisual ? float(RodVisual->HookLocation().Z - LakeFish::SurfaceZ) : 0.0f, Fish->HookGrip, RodLoadGf, Fish->BodyInWater(), HandLoadGf,
                        RodVisual ? (float)FVector::Dist(RodVisual->HookLocation(), HandPoint(0)) : -1.0f,
                        RodVisual ? FMath::PointDistToSegment(LeftShoulder(), RodVisual->Tip(), RodVisual->HookLocation()) : -1.0f,
                        // (Facing the pull: 1 head towards the rod tip, -1 tail towards it.)
                        (float)FVector::DotProduct(Fish->GetActorForwardVector(), ((RodVisual ? RodVisual->Tip() : RodTip()) - Mouth).GetSafeNormal()), FightHint,
                        (float)FVector::DotProduct(Fish->GetActorForwardVector(), Fish->Velocity().GetSafeNormal()));
                }
                if (bShots && B.Shots < 2 && ((B.Shots == 0 && T > 2.5f) || (B.Shots == 1 && T > 8)))
                    FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / FString::Printf(TEXT("Bench_%d_%d.png"), B.Index, B.Shots++), true, false);
                // The lift out as the angler sees it, every 0.6 s.
                if (bShots && FishingState == EFishingState::Landing && Now >= B.NextLandShot && B.LandShots < 8)
                {
                    B.NextLandShot = Now + 0.6f;
                    FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / FString::Printf(TEXT("Bench_%d_land%d.png"), B.Index, B.LandShots++), true, false);
                }
                if (bSide)
                {
                    // Beside the fish, square to the line from the angler: under the water while it
                    // fights, above the water (wider) once it is being lifted out.
                    APlayerController* PC = Cast<APlayerController>(GetController());
                    if (!B.SideCam.IsValid() && PC)
                    {
                        B.SideCam = GetWorld()->SpawnActor<ACameraActor>();
                        B.SideCam->GetCameraComponent()->SetFieldOfView(60);
                        PC->SetViewTarget(B.SideCam.Get());
                        if (PC->MyHUD) PC->MyHUD->bShowHUD = false;
                    }
                    if (ACameraActor* Cam = B.SideCam.Get())
                    {
                        const FVector P = Fish->GetActorLocation();
                        const FVector Out2D = (P - GetActorLocation()).GetSafeNormal2D();
                        const FVector Side(-Out2D.Y, Out2D.X, 0);
                        const bool bAir = FishingState == EFishingState::Landing;
                        // (At the top, from just above the water: how much of it is out.)
                        const bool bTop = !bAir && Fish->MouthLocation().Z > LakeFish::SurfaceZ - 25;
                        const FVector Eye = bAir ? FMath::Lerp(GetActorLocation(), P, 0.5f) + Side * 420 + FVector(0, 0, 60)
                            : P + Side * 230 - Out2D * 40 + FVector(0, 0, bTop ? 40 : 10);
                        const FVector Look = bAir ? FMath::Lerp(GetActorLocation(), P, 0.55f) + FVector(0, 0, 60) : P;
                        Cam->SetActorLocationAndRotation(FVector(Eye.X, Eye.Y, bAir || bTop ? Eye.Z : FMath::Min(Eye.Z, LakeFish::SurfaceZ - 15)), (Look - Eye).Rotation());
                    }
                    static const float Times[] = { 0.3f, 0.6f, 0.9f, 1.2f, 1.5f, 1.8f, 2.2f, 2.6f, 3.0f, 4.0f, 5.0f, 6.5f, 8.0f, 10.0f, 12.0f, 15.0f, 18.0f, 22.0f };
                    const bool bFightShot = FishingState == EFishingState::Fight && B.SideShots < UE_ARRAY_COUNT(Times) && T > Times[B.SideShots];
                    const bool bLandShot = FishingState == EFishingState::Landing && B.SideShots < 20 && LandProgress > 0.3f * (B.SideShots >= 100 ? 0 : 1)
                        && Now >= B.NextLandShot;
                    if (bFightShot || bLandShot)
                    {
                        if (bLandShot) B.NextLandShot = Now + 0.7f;
                        const int32 N = bLandShot ? 50 + B.LandShots++ : B.SideShots++;
                        if (bLandShot && B.LandShots > 5) B.NextLandShot = 1e9f;
                        const FVector ShotMouth = Fish->MouthLocation();
                        UE_LOG(LogTemp, Display, TEXT("BENCH   side shot %d at t=%.1f: state %d, mouth %.0f cm under, body pitch %.0f roll %.0f, in water %.2f, hook %.0f cm up, mouth-hook %.0f cm"),
                            N, T, (int32)FishingState, LakeFish::SurfaceZ - ShotMouth.Z, Fish->GetActorRotation().Pitch, Fish->GetActorRotation().Roll, Fish->BodyInWater(),
                            RodVisual ? float(RodVisual->HookLocation().Z - LakeFish::SurfaceZ) : 0.0f, RodVisual ? (float)FVector::Dist(RodVisual->HookLocation(), ShotMouth) : -1.0f);
                        FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / FString::Printf(TEXT("BenchSide_%d_%02d.png"), B.Index, N), false, false);
                    }
                }
            }
            const bool bCaught = CatchCount > B.Caught;
            if (bCaught || !bOn || T > 150)
            {
                const float Mean = B.Samples ? B.SumTension / B.Samples : 0;
                UE_LOG(LogTemp, Display, TEXT("BENCH result %s: %s in %.1f s | tension mean %.2f max %.2f, hard %.1f s, slack %.1f s, LMB held %.0f%% | fish top speed %.0f cm/s, deepest %.0f cm, farthest %.0f cm, tail first %.1f of %.1f s moving | %s"),
                    *Label, bCaught ? TEXT("CAUGHT") : T > 150 ? TEXT("TIMEOUT") : TEXT("LOST"), T, Mean, B.MaxTension, B.HardTime, B.SlackTime,
                    B.Samples ? 100 * B.HeldTime / (B.Samples * 0.05f) : 0, B.MaxSpeed, B.MaxDepth, B.MaxOut, B.TailFirstTime, B.MovingTime, *Status);
                B.Caught = CatchCount;
                if (T > 150) FinishFishing(TEXT("bench timeout"));
                Go(8);
            }
            return;
        }
        case 8:   // put everything away for the next case
            // (The fish in the free hand, as the angler sees it.)
            if (bShots && bFishInHand && In > 0.35f && B.Shots < 9) { B.Shots = 9; FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / FString::Printf(TEXT("Bench_%d_hand.png"), B.Index), true, false); }
            if (bShots && bFishInHand && In < 0.6f) return;
            Hold(false);
            if (In > 0.5f)
            {
                if (ALakeFish* Fish = B.Fish.Get(); Fish && FishingState != EFishingState::Landing && !bFishInHand) Fish->Destroy();
                Go(9);
            }
            return;
        case 9:   // lift the rig out if it is still in the water
            if (bRetrieving || (FishingState == EFishingState::Idle && !bRigInHand)) LogRigPath(B, Now);
            if (FishingState == EFishingState::Waiting && !bRigInHand) Hold(true);
            if ((FishingState == EFishingState::Idle && bRigInHand) || In > 40)
            {
                if (Case.bCastOnly) UE_LOG(LogTemp, Display, TEXT("BENCH   rig back in the hand %.1f s after LMB (in hand %d)"), In, bRigInHand ? 1 : 0);
                Hold(false);
                ++B.Index;
                B.Shots = 0; B.SideShots = 0; B.LandShots = 0; B.NextLandShot = 0; B.CastShots = 0; B.NextCastShot = 0;
                if (ACameraActor* Cam = B.SideCam.Get())
                {
                    Cam->Destroy();
                    if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetViewTarget(this);
                }
                Go(1);
            }
            return;
        }
    }), 0.05f, true);
#endif
}
