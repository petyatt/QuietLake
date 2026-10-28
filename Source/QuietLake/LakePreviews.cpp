// Scripted scenes for checking the angler without playing (see CLAUDE.md): -LakePreview films the
// angler after the environment shots, -BitePreview each motion of the float, -LandPreview a fish
// lifted out of the water. Each one sets the scene, takes screenshots into Saved/, logs what it
// measured (QUIET_LAKE_* lines) and quits.
#include "LakeGame.h"
#include "LakeMap.h"
#include "LakeRod.h"
#include "LakeWeather.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UnrealClient.h"

#if !UE_BUILD_SHIPPING
void ALakeCharacter::RunPreviews()
{
    // -LakePreview: after the environment shots, film the angler: idle, bait close-up,
    // backswing, the cast, the float on the water and the rod on its stand.
    if (FParse::Param(FCommandLine::Get(), TEXT("LakePreview")) && GetNetMode() == NM_Standalone)
    {
        // The environment shots end at 103 s; the times below were laid out from 50 s.
        constexpr float Offset = 61.0f;
        auto At = [this, Offset](float Seconds, TFunction<void()> Action)
        {
            FTimerHandle Handle;
            GetWorldTimerManager().SetTimer(Handle, [Action] { Action(); }, Seconds + Offset, false);
        };
        auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / Name, false, false); };
        At(50.0f, [this]
        {
            APlayerController* PC = Cast<APlayerController>(GetController());
            if (!PC) return;
            PC->SetViewTarget(this);
            FVector2D Facing;
            const FVector2D Spot = LakeMap::StartSpot(&Facing) + FVector2D(-Facing.Y, Facing.X) * 300;
            SetActorLocation(FVector(Spot, ALakeEnvironment::GroundHeight(Spot.X, Spot.Y) + 95), false, nullptr, ETeleportType::TeleportPhysics);
            PC->SetControlRotation(FRotator(-6, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)) + 8, 0));
            if (!bThirdPerson) ToggleView();
        });
        At(54.0f, [Shot] { Shot(TEXT("Fisher_Idle.png")); });
        // Depth control: the float slides up the line towards the tip, then back.
        At(54.1f, [this] { ServerChangeDepth_Implementation(250); });
        for (int32 I = 0; I <= 18; ++I)
            At(54.1f + I * 0.1f, [this, I]
            {
                if (RodVisual) UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_DEPTHLOG t=%.1f hookdepth=%.0f tipZ=%.0f floatZ=%.0f hookZ=%.0f"), I * 0.1f, HookDepth,
                    RodVisual->Tip().Z, RodVisual->BobberLocation().Z, RodVisual->HookLocation().Z);
            });
        At(55.0f, [this, Shot]
        {
            if (RodVisual) UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_DEPTH depth=%.0f float above hook=%.0f cm"), FloatDepth, RodVisual->BobberLocation().Z - RodVisual->HookLocation().Z);
            Shot(TEXT("Fisher_Depth.png"));
        });
        At(55.1f, [this] { ServerChangeDepth_Implementation(-250); });
        At(55.93f, [this]
        {
            if (!RodVisual) return;
            const FVector HookAt = RodVisual->HookLocation();
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_ROD tip=%s hook=%s hook above ground=%.0f cm float above ground=%.0f cm"), *RodVisual->Tip().ToString(), *HookAt.ToString(),
                HookAt.Z - ALakeEnvironment::GroundHeight(HookAt.X, HookAt.Y), RodVisual->BobberLocation().Z - ALakeEnvironment::GroundHeight(RodVisual->BobberLocation().X, RodVisual->BobberLocation().Y));
            const FVector CamPos = HookAt + GetActorRightVector() * 30 - GetActorForwardVector() * 10 + FVector(0, 0, 6);
            auto* Cam = GetWorld()->SpawnActor<ACameraActor>(CamPos, (HookAt - CamPos).Rotation());
            Cam->GetCameraComponent()->SetFieldOfView(40);
            if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetViewTarget(Cam);
        });
        At(56.0f, [Shot] { Shot(TEXT("Fisher_Bait.png")); });
        At(57.0f, [this]
        {
            if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetViewTarget(this);
            NextActionTime = 0;
            ServerFish_Implementation();
        });
        At(57.6f, [Shot] { Shot(TEXT("Fisher_Sweep.png")); });
        for (int32 I = 0; I <= 30; ++I)
            At(57.0f + I * 0.1f, [this, I]
            {
                if (RodVisual) UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_CAST t=%.1f float=%s tip=%s"), I * 0.1f, *RodVisual->BobberLocation().ToString(), *RodVisual->Tip().ToString());
            });
        At(57.0f + CastLoad, [this] { ServerRelease_Implementation(); });
        At(58.4f, [Shot] { Shot(TEXT("Fisher_Cast.png")); });
        At(61.5f, [Shot] { Shot(TEXT("Fisher_Waiting.png")); });
        auto LookFrom = [this](FVector From, FVector At)
        {
            auto* Cam = GetWorld()->SpawnActor<ACameraActor>(From, (At - From).Rotation());
            Cam->GetCameraComponent()->SetFieldOfView(50);
            if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetViewTarget(Cam);
        };
        At(61.9f, [this, LookFrom]
        {
            if (!RodVisual) return;
            const FVector Float = RodVisual->BobberLocation();
            LookFrom(Float + GetActorRightVector() * 110 - GetActorForwardVector() * 70 + FVector(0, 0, 35), Float);
        });
        At(62.0f, [Shot] { Shot(TEXT("Fisher_Float.png")); });
        // A nibble: the float should only dip a little and settle, never leave the water.
        At(62.3f, [this] { ++NibbleCounter; });
        for (int32 I = 0; I <= 10; ++I)
            At(62.3f + I * 0.1f, [this, I]
            {
                if (RodVisual) UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_NIBBLE t=%.1f float above water=%.1f cm"), I * 0.1f, RodVisual->BobberLocation().Z - LakeFish::SurfaceZ);
            });
        At(62.8f, [this, LookFrom]
        {
            // Side view of the angler and the whole line for the retrieve.
            LookFrom(GetActorLocation() + GetActorRightVector() * 900 + GetActorForwardVector() * 450 + FVector(0, 0, 120),
                     GetActorLocation() + GetActorForwardVector() * 500 + FVector(0, 0, 120));
        });
        At(63.0f, [this] { ServerCancel_Implementation(); });          // lift the rig out of the water
        for (int32 I = 0; I <= 26; ++I)
            At(63.0f + I * 0.1f, [this, I]
            {
                if (RodVisual) UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_RETRIEVE t=%.1f float=%s tip=%s"), I * 0.1f, *RodVisual->BobberLocation().ToString(), *RodVisual->Tip().ToString());
            });
        At(64.2f, [Shot] { Shot(TEXT("Fisher_Retrieve.png")); });
        At(65.7f, [this] { if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetViewTarget(this); });
        At(65.9f, [this] { ServerToggleRod_Implementation(); });
        At(68.0f, [Shot] { Shot(TEXT("Fisher_Stand.png")); });
    }
    // -BitePreview: a cast at the camp on a clear morning, then each motion of the float in turn,
    // filmed close up and logged (height above the water, lean).
    if (FParse::Param(FCommandLine::Get(), TEXT("BitePreview")) && GetNetMode() == NM_Standalone)
    {
        auto At = [this](float Seconds, TFunction<void()> Action)
        {
            FTimerHandle Handle;
            GetWorldTimerManager().SetTimer(Handle, [Action] { Action(); }, Seconds, false);
        };
        auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / Name, false, false); };
        At(4.0f, [this]
        {
            if (ALakeWeather* Weather = ALakeWeather::Get(GetWorld()))
            {
                FLakeWeatherState Calm;
                Calm.Cloud = 0.3f; Calm.Fog = 0; Calm.Rain = 0; Calm.Wind = 0.03f; Calm.WindYaw = 40;
                Weather->SetWeather(Calm, 1);
                Weather->SetHours(9.0f);
            }
            FVector2D Facing;
            const FVector2D Spot = LakeMap::StartSpot(&Facing) + Facing * 60;
            SetActorLocation(FVector(Spot, ALakeEnvironment::GroundHeight(Spot.X, Spot.Y) + 95), false, nullptr, ETeleportType::TeleportPhysics);
            if (APlayerController* PC = Cast<APlayerController>(GetController()))
            {
                PC->SetControlRotation(FRotator(-10, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)), 0));
                if (PC->MyHUD) PC->MyHUD->bShowHUD = false;
            }
            FloatDepth = 100;
        });
        // Sound the depth first, as anglers do, then set the bait just on the bottom (the shot
        // hanging 8 cm above it) and cast to the same place: the classic setting for a lift bite.
        At(6.0f, [this] { NextActionTime = 0; ServerFish_Implementation(); });
        At(6.0f + CastLoad, [this] { ServerRelease_Implementation(); });
        At(8.5f, [this]
        {
            const float Water = LakeFish::SurfaceZ - ALakeEnvironment::GroundHeight(FloatPosition.X, FloatPosition.Y);
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_BITE sounded %.0f cm of water"), Water);
            ServerCancel_Implementation();
            FloatDepth = FMath::Clamp(FMath::RoundToFloat(Water + 2), MinDepth, MaxDepth);
        });
        At(12.0f, [this] { NextActionTime = 0; ServerFish_Implementation(); });
        At(12.0f + CastLoad, [this] { ServerRelease_Implementation(); });
        At(16.0f, [this]
        {
            if (!RodVisual) return;
            const FVector Float = RodVisual->BobberLocation();
            const FVector From = Float + GetActorRightVector() * 80 - GetActorForwardVector() * 40 + FVector(0, 0, 18);
            auto* Cam = GetWorld()->SpawnActor<ACameraActor>(From, (Float - From).Rotation());
            Cam->GetCameraComponent()->SetFieldOfView(35);
            if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetViewTarget(Cam);
        });
        At(16.60f, [Shot] { Shot(TEXT("Bite_Still.png")); });
        struct FDemo { float At; EBiteMotion Motion; float Duration, Amount; EFishingState State; };
        const FDemo Demos[] = {
            { 17.0f, EBiteMotion::Tremble, 1.6f, 0.8f, EFishingState::Waiting },
            { 19.0f, EBiteMotion::Peck, 0.35f, 1.8f, EFishingState::Waiting },
            { 21.0f, EBiteMotion::Lift, 2.0f, 3.2f, EFishingState::Bite },
            { 24.0f, EBiteMotion::Drift, 3.0f, 8.0f, EFishingState::Bite },
            { 27.5f, EBiteMotion::Sink, 2.5f, 55.0f, EFishingState::Bite },
        };
        for (const FDemo& Demo : Demos)
            At(Demo.At, [this, Demo]
            {
                FishingState = Demo.State;
                FBiteStep Step;
                Step.Motion = Demo.Motion; Step.Duration = Demo.Duration; Step.Amount = Demo.Amount; Step.bTake = Demo.State == EFishingState::Bite;
                OnBiteMotion(nullptr, Step);
            });
        At(17.50f, [Shot] { Shot(TEXT("Bite_Tremble.png")); });
        At(19.17f, [Shot] { Shot(TEXT("Bite_Peck.png")); });
        At(22.00f, [Shot] { Shot(TEXT("Bite_Lift.png")); });
        auto Aim = [this]
        {
            if (!RodVisual) return;
            const FVector Float = RodVisual->BobberLocation();
            const FVector From = Float + GetActorRightVector() * 70 - GetActorForwardVector() * 30 + FVector(0, 0, 22);
            auto* Cam = GetWorld()->SpawnActor<ACameraActor>(From, (Float - From).Rotation());
            Cam->GetCameraComponent()->SetFieldOfView(35);
            if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetViewTarget(Cam);
        };
        At(23.40f, Aim);
        At(23.60f, [Shot] { Shot(TEXT("Bite_Lie.png")); });
        At(25.80f, Aim);
        At(26.00f, [Shot] { Shot(TEXT("Bite_Drift.png")); });
        At(28.00f, [Shot] { Shot(TEXT("Bite_Sink.png")); });
        for (int32 I = 0; I <= 140; ++I)
            At(16.50f + I * 0.1f, [this, I]
            {
                if (RodVisual) UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_BITE t=%.1f motion=%d antenna above water=%.1f cm tilt=%.0f keel %.1f shot %.1f hook %.1f (bottom %.1f)"), 16.5f + I * 0.1f, (int32)BiteMotion,
                    RodVisual->FloatTip().Z - LakeFish::SurfaceZ, RodVisual->GetBiteTilt(), RodVisual->FloatKeel().Z - LakeFish::SurfaceZ,
                    RodVisual->ShotLocation().Z - LakeFish::SurfaceZ, RodVisual->HookLocation().Z - LakeFish::SurfaceZ,
                    ALakeEnvironment::GroundHeight(RodVisual->HookLocation().X, RodVisual->HookLocation().Y) - LakeFish::SurfaceZ);
            });
        At(31.0f, [] { FPlatformMisc::RequestExit(false); });
    }
    // -LandPreview: a perch struck at the camp on a clear morning and lifted out of the water on
    // the line, filmed from the side and close up, its swing logged.
    if (FParse::Param(FCommandLine::Get(), TEXT("LandPreview")) && GetNetMode() == NM_Standalone)
    {
        auto At = [this](float Seconds, TFunction<void()> Action)
        {
            FTimerHandle Handle;
            GetWorldTimerManager().SetTimer(Handle, [Action] { Action(); }, Seconds, false);
        };
        auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / Name, false, false); };
        auto LookFrom = [this](FVector From, FVector Target, float Fov)
        {
            auto* Cam = GetWorld()->SpawnActor<ACameraActor>(From, (Target - From).Rotation());
            Cam->GetCameraComponent()->SetFieldOfView(Fov);
            if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetViewTarget(Cam);
        };
        At(4.0f, [this]
        {
            if (ALakeWeather* Weather = ALakeWeather::Get(GetWorld()))
            {
                FLakeWeatherState Calm;
                Calm.Cloud = 0.3f; Calm.Fog = 0; Calm.Rain = 0; Calm.Wind = 0.03f; Calm.WindYaw = 40;
                Weather->SetWeather(Calm, 1);
                Weather->SetHours(9.0f);
            }
            FVector2D Facing;
            const FVector2D Spot = LakeMap::StartSpot(&Facing) + Facing * 60;
            SetActorLocation(FVector(Spot, ALakeEnvironment::GroundHeight(Spot.X, Spot.Y) + 95), false, nullptr, ETeleportType::TeleportPhysics);
            if (APlayerController* PC = Cast<APlayerController>(GetController()))
            {
                PC->SetControlRotation(FRotator(-10, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)), 0));
                if (PC->MyHUD) PC->MyHUD->bShowHUD = false;
            }
            FloatDepth = 60;
        });
        At(6.0f, [this] { NextActionTime = 0; ServerFish_Implementation(); });
        At(6.0f + CastLoad, [this] { ServerRelease_Implementation(); });
        At(9.0f, [this]
        {
            const FVector Bait = BaitLocation();
            ALakeFish* Fish = GetWorld()->SpawnActor<ALakeFish>(Bait, FRotator(0, GetActorRotation().Yaw + 90, 0));
            Fish->Setup(EFishSpecies::Perch, 0.32f, Bait, -1);
            OnFishTake(Fish);
            NextActionTime = 0;
            ServerFish_Implementation();
        });
        At(10.4f, [this, LookFrom]
        {
            LookFrom(GetActorLocation() + GetActorRightVector() * 650 + GetActorForwardVector() * 230 + FVector(0, 0, 40),
                     GetActorLocation() + GetActorForwardVector() * 230 + FVector(0, 0, 130), 60);
        });
        At(10.5f, [this] { if (ALakeFish* Fish = HookedFish.Get()) StartLanding(Fish); });
        At(11.0f, [Shot] { Shot(TEXT("Land_Out.png")); });
        At(11.7f, [Shot] { Shot(TEXT("Land_Swing.png")); });
        At(12.0f, [Shot] { Shot(TEXT("Land_Hang.png")); });
        At(12.2f, [this, LookFrom]
        {
            if (!RodVisual) return;
            const FVector Hook = RodVisual->HookLocation();
            LookFrom(Hook + GetActorRightVector() * 110 + GetActorForwardVector() * 30 + FVector(0, 0, 5), Hook - FVector(0, 0, 10), 45);
        });
        At(12.3f, [Shot] { Shot(TEXT("Land_Close.png")); });
        At(13.5f, [this, LookFrom]
        {
            LookFrom(GetActorLocation() + GetActorRightVector() * 260 + GetActorForwardVector() * 160 + FVector(0, 0, 60),
                     GetActorLocation() + GetActorForwardVector() * 30 + FVector(0, 0, 20), 55);
        });
        At(13.75f, [Shot] { Shot(TEXT("Land_Hand.png")); });
        for (int32 I = 0; I <= 40; ++I)
            At(10.5f + I * 0.1f, [this, I]
            {
                if (!RodVisual) return;
                const FVector Hook = RodVisual->HookLocation() - GetActorLocation();
                const ALakeFish* Fish = LineFish;
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LAND t=%.1f state=%d hook ahead %.0f side %.0f up %.0f (above water %.0f) tip up %.0f fish axis Z %.2f"),
                    I * 0.1f, (int32)FishingState, FVector::DotProduct(Hook, GetActorForwardVector()), FVector::DotProduct(Hook, GetActorRightVector()), Hook.Z,
                    RodVisual->HookLocation().Z - LakeFish::SurfaceZ, RodVisual->Tip().Z - GetActorLocation().Z, Fish ? Fish->GetActorForwardVector().Z : 0.0f);
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LAND    tip ahead %.0f, tip to hook %.0f, float %s shot %s"), FVector::DotProduct(RodVisual->Tip() - GetActorLocation(), GetActorForwardVector()),
                    FVector::Dist(RodVisual->Tip(), RodVisual->HookLocation()), *(RodVisual->FloatKeel() - GetActorLocation()).ToCompactString(), *(RodVisual->ShotLocation() - GetActorLocation()).ToCompactString());
            });
        At(15.0f, [] { FPlatformMisc::RequestExit(false); });
    }
    // -CastPreview: casts from the hand at the camp on a clear morning, filmed from the side: a
    // clean one (the rig in the hand, the swing, the rig in the air, the landing close up, the float
    // standing up), one let go too soon and a clean one aimed nearer; the rig's path and each
    // result are logged.
    if (FParse::Param(FCommandLine::Get(), TEXT("CastPreview")) && GetNetMode() == NM_Standalone)
    {
        auto At = [this](float Seconds, TFunction<void()> Action)
        {
            FTimerHandle Handle;
            GetWorldTimerManager().SetTimer(Handle, [Action] { Action(); }, Seconds, false);
        };
        auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / Name, false, false); };
        auto LookFrom = [this](FVector From, FVector Target, float Fov)
        {
            auto* Cam = GetWorld()->SpawnActor<ACameraActor>(From, (Target - From).Rotation());
            Cam->GetCameraComponent()->SetFieldOfView(Fov);
            if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetViewTarget(Cam);
        };
        auto Side = [this, LookFrom]
        {
            LookFrom(GetActorLocation() + GetActorRightVector() * 750 + GetActorForwardVector() * 350 + FVector(0, 0, 120),
                     GetActorLocation() + GetActorForwardVector() * 350 + FVector(0, 0, 150), 70);
        };
        At(4.0f, [this]
        {
            if (ALakeWeather* Weather = ALakeWeather::Get(GetWorld()))
            {
                FLakeWeatherState Calm;
                Calm.Cloud = 0.3f; Calm.Fog = 0; Calm.Rain = 0; Calm.Wind = 0.03f; Calm.WindYaw = 40;
                Weather->SetWeather(Calm, 1);
                Weather->SetHours(9.0f);
            }
            FVector2D Facing;
            const FVector2D Spot = LakeMap::StartSpot(&Facing) + Facing * 60;
            SetActorLocation(FVector(Spot, ALakeEnvironment::GroundHeight(Spot.X, Spot.Y) + 95), false, nullptr, ETeleportType::TeleportPhysics);
            if (APlayerController* PC = Cast<APlayerController>(GetController()))
            {
                PC->SetControlRotation(FRotator(-12, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)), 0));
                if (PC->MyHUD) PC->MyHUD->bShowHUD = false;
            }
            FloatDepth = 30;
        });
        At(5.5f, Side);
        struct FTry { float At; float Held; float Pitch; ERodKind Rod; };
        const FTry Tries[] = {
            { 6.0f, CastLoad + 0.1f, -12, ERodKind::Medium },     // clean, as far as the rod reaches
            { 14.0f, 0.12f, -12, ERodKind::Medium },              // let go too soon: short, in a heap
            { 22.0f, CastLoad + 0.1f, -30, ERodKind::Medium },    // clean, nearer: looking down at the water
            { 30.0f, CastLoad * 1.5f, -12, ERodKind::Heavy },     // a heavy fibreglass pole: slower, softer
            { 38.0f, CastLoad + 0.1f, -12, ERodKind::Light },     // a light carbon pole: quick
        };
        for (const FTry& Try : Tries)
        {
            At(Try.At - 0.5f, [this, Try]
            {
                RodKind = Try.Rod;
                if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetControlRotation(FRotator(Try.Pitch, GetActorRotation().Yaw, 0));
            });
            At(Try.At, [this] { NextActionTime = 0; ServerFish_Implementation(); });
            At(Try.At + Try.Held, [this] { ServerRelease_Implementation(); });
            At(Try.At + Try.Held + 1.7f, [this]
            {
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_CASTRESULT result=%d severity=%.2f out=%.0f cm hook beyond float=%.0f cm: %s"), (int32)CastResult, CastSeverity,
                    FVector::Dist2D(FloatPosition, GetActorLocation()), FVector::DotProduct(FVector(CastHookLanding) - FloatPosition, GetActorForwardVector()), *Status);
            });
            At(Try.At + Try.Held + 4.0f, [this] { ServerCancel_Implementation(); });
        }
        // The clean cast, shot by shot.
        At(6.5f, [Shot] { Shot(TEXT("Cast_Hold.png")); });
        At(6.75f, [Shot] { Shot(TEXT("Cast_Swing.png")); });
        At(7.0f, [Shot] { Shot(TEXT("Cast_Air.png")); });
        At(7.05f, [this, LookFrom]
        {
            LookFrom(FloatPosition + GetActorRightVector() * 260 - GetActorForwardVector() * 160 + FVector(0, 0, 110), FloatPosition + GetActorForwardVector() * 50, 50);
        });
        At(7.55f, [Shot] { Shot(TEXT("Cast_Land.png")); });
        At(9.5f, [Shot] { Shot(TEXT("Cast_Stand.png")); });
        At(10.0f, Side);
        At(12.6f, [Shot] { Shot(TEXT("Cast_Catch.png")); });
        At(14.5f, [Shot] { Shot(TEXT("Cast_Early.png")); });
        At(23.3f, [Shot] { Shot(TEXT("Cast_Near.png")); });
        At(30.6f, [Shot] { Shot(TEXT("Cast_HeavyHold.png")); });
        At(31.15f, [Shot] { Shot(TEXT("Cast_HeavySwing.png")); });
        At(38.75f, [Shot] { Shot(TEXT("Cast_LightSwing.png")); });
        for (int32 I = 0; I <= 800; ++I)
            At(6.0f + I * 0.05f, [this, I]
            {
                if (!RodVisual) return;
                auto Rel = [this](FVector P)
                {
                    const FVector D = P - GetActorLocation();
                    return FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), FVector::DotProduct(D, GetActorForwardVector()), FVector::DotProduct(D, GetActorRightVector()), D.Z);
                };
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_CASTPATH t=%.2f swing=%d tip %s float %s shot %s hook %s"), 6.0f + I * 0.05f, bCharging ? 1 : 0,
                    *Rel(RodVisual->Tip()), *Rel(RodVisual->BobberLocation()), *Rel(RodVisual->ShotLocation()), *Rel(RodVisual->HookLocation()));
            });
        At(46.0f, [] { FPlatformMisc::RequestExit(false); });
    }
}
#endif
