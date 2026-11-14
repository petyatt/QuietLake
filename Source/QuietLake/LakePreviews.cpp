// Scripted scenes for checking the angler without playing (see CLAUDE.md): -LakePreview films the
// angler after the environment shots, -BitePreview each motion of the float, -LandPreview a fish
// lifted out of the water. Each one sets the scene, takes screenshots into Saved/, logs what it
// measured (QUIET_LAKE_* lines) and quits.
#include "LakeGame.h"
#include "LakeFood.h"
#include "LakeLife.h"
#include "LakeHUD.h"
#include "LakeMap.h"
#include "LakeRod.h"
#include "LakeWeather.h"
#include "LakeMenu.h"
#include "InputKeyEventArgs.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UnrealClient.h"

#if !UE_BUILD_SHIPPING
void ALakeCharacter::RunRodDiagnostics(float DeltaSeconds)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("RodDiagnostics"))) return;
    RodDiagnosticTime += DeltaSeconds;
    auto* PC = Cast<ALakePlayerController>(GetController());
    if (IsLocallyControlled() && PC && FParse::Param(FCommandLine::Get(), TEXT("RodNetworkTest")))
    {
        auto Key = [PC](FKey K, EInputEvent E, float V = 1.0f)
        { PC->InputKey(FInputKeyEventArgs::CreateSimulated(K, E, V)); };
        if (RodDiagnosticStage == 0 && RodDiagnosticTime >= 12) { PC->CloseMenu(); RodDiagnosticStage = 1; }
        if (RodDiagnosticStage == 1 && RodDiagnosticTime >= 14)
        { Key(EKeys::D, IE_Pressed); Key(EKeys::LeftMouseButton, IE_Pressed); RodDiagnosticStage = 2; }
        if (RodDiagnosticTime >= 14.2f && RodDiagnosticTime < 14.8f) Key(EKeys::MouseY, IE_Axis, 650 * DeltaSeconds);
        if (RodDiagnosticStage == 2 && RodDiagnosticTime >= 14.6f)
        { Key(EKeys::LeftMouseButton, IE_Released, 0); RodDiagnosticStage = 3; }
        if (RodDiagnosticTime >= 15 && RodDiagnosticTime < 15.6f) Key(EKeys::MouseY, IE_Axis, -650 * DeltaSeconds);
        if (RodDiagnosticStage == 3 && RodDiagnosticTime >= 16)
        { Key(EKeys::D, IE_Released, 0); RodDiagnosticStage = 4; }
    }
    if (RodDiagnosticTime < NextRodDiagnosticLog || !RodVisual) return;
    NextRodDiagnosticLog = RodDiagnosticTime + 0.1f;
    const FRotator View = GetBaseAimRotation();
    UE_LOG(LogTemp, Display, TEXT("ROD_DIAG t=%.3f name=%s role=%d local=%d pos=%s view=%.2f,%.2f aim=%.2f,%.2f state=%d held=%d flying=%d hand=%d cast=%.3f target=%s keel=%s shot=%s"),
        ServerNow(), *GetName(), (int32)GetLocalRole(), IsLocallyControlled(), *GetActorLocation().ToCompactString(),
        FRotator::NormalizeAxis(View.Pitch), View.Yaw, SwingPitch, SwingYaw, (int32)FishingState, bCharging, bRigFlying, bRigInHand,
        CastServerTime, *FloatPosition.ToCompactString(), *RodVisual->FloatKeel().ToCompactString(), *RodVisual->ShotLocation().ToCompactString());
}

void ALakeCharacter::RunPreviews()
{
    RunFishingBench();
    // -LookFrom=X,Y,Yaw,Pitch (world cm, degrees): a still camera exactly there, for reproducing a
    // player's screenshot, plus the ground profile straight ahead.
    if (FString Look; FParse::Value(FCommandLine::Get(), TEXT("LookFrom="), Look, false) && GetNetMode() == NM_Standalone)
    {
        TArray<FString> Parts;
        Look.ParseIntoArray(Parts, TEXT(","), true);
        FTimerHandle Fallback;
        GetWorldTimerManager().SetTimer(Fallback, [] { FPlatformMisc::RequestExit(false); }, 16.0f, false);
        if (Parts.Num() == 4)
        {
            const FVector2D At(FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]));
            const float Yaw = FCString::Atof(*Parts[2]), Pitch = FCString::Atof(*Parts[3]);
            FTimerHandle Snap, Exit;
            GetWorldTimerManager().SetTimer(Snap, [this, At, Yaw, Pitch]
            {
                const FVector Eye(At, ALakeEnvironment::GroundHeight(At.X, At.Y) + 170.0f);
                auto* Cam = GetWorld()->SpawnActor<ACameraActor>(Eye, FRotator(Pitch, Yaw, 0));
                Cam->GetCameraComponent()->SetFieldOfView(70);
                if (APlayerController* PC = Cast<APlayerController>(GetController()))
                {
                    PC->SetViewTarget(Cam);
                    if (PC->MyHUD) PC->MyHUD->bShowHUD = false;
                }
                FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("LookFrom.png"), true, false);
                FString Line = FString::Printf(TEXT("QUIET_LAKE_LOOKFROM at (%.0f, %.0f) yaw %.0f |"), At.X, At.Y, Yaw);
                const FVector Ahead = FRotator(0, Yaw, 0).Vector();
                const FVector2D Dir(Ahead.X, Ahead.Y);
                for (float D = 0.0f; D <= 40.0f; D += 2.0f)
                {
                    const FVector2D P = At + Dir * D * 100.0f;
                    Line += FString::Printf(TEXT(" %.0fm:%.1f"), D, LakeMap::Ground(P.X, P.Y) - LakeMap::SurfaceZ);
                }
                UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
            }, 12.0f, false);
            GetWorldTimerManager().SetTimer(Exit, [] { FPlatformMisc::RequestExit(false); }, 13.5f, false);
            return;
        }
    }
    // -BottomPreview: the lake bed — weed beds seen from above and from under the water, the rolling
    // bottom with shoals and hollows, the sunken trees and log heaps, the weedy warm shallows.
    if (FParse::Param(FCommandLine::Get(), TEXT("BottomPreview")) && GetNetMode() == NM_Standalone)
    {
        if (ALakeWeather* Weather = ALakeWeather::Get(GetWorld()))
        {
            FLakeWeatherState Clear;
            Clear.Cloud = 0.3f; Clear.Fog = 0; Clear.Rain = 0; Clear.Wind = 0.1f; Clear.WindYaw = 40;
            Weather->SetWeatherNow(Clear);
            Weather->SetHours(11.0f);
        }
        auto BedAt = [](const FVector2D& Q) { return LakeMap::Ground(Q.X, Q.Y); };
        auto Shot = [this](const FString& File, const FVector& Eye, const FVector& Look, float Fov)
        {
            auto* Cam = GetWorld()->SpawnActor<ACameraActor>(Eye, (Look - Eye).Rotation());
            if (!Cam) return;
            Cam->GetCameraComponent()->SetFieldOfView(Fov);
            if (APlayerController* PC = Cast<APlayerController>(GetController()))
            {
                PC->SetViewTarget(Cam);
                if (PC->MyHUD) PC->MyHUD->bShowHUD = false;
            }
            FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / File, true, false);
        };
        // The two weed beds (the same places as LakeMap's), the middle of the lake, the warm south bay.
        const FVector2D Off = FVector2D(108, 4) * 100, Hump = FVector2D(70, -8) * 100;
        const FVector2D Middle(2000, -3000), Bay(-11625, 10125);
        struct FShot { float At; const TCHAR* File; FVector Eye; FVector Look; float Fov; };
        TArray<FShot> Shots;
        // A shallow, weedy place without lilies in front of it: the best place to see the bed from above.
        FVector2D Weedy = Off;
        {
            float Best = -1;
            const FBox2D Bounds = LakeMap::LakeBounds();
            for (float Y = Bounds.Min.Y; Y <= Bounds.Max.Y; Y += 400)
                for (float X = Bounds.Min.X; X <= Bounds.Max.X; X += 400)
                {
                    const FVector2D P(X, Y);
                    if (!LakeMap::IsWater(P.X, P.Y)) continue;
                    const float D = LakeMap::Depth(P.X, P.Y);
                    if (D < 70 || D > 210) continue;
                    if (LakeMap::SpotAmount(P) > 0.05f || LakeMap::LilyAmount(P.X, P.Y) > 0.05f) continue;
                    const float Score = LakeMap::WeedAmount(P) + 0.5f * LakeMap::BackwaterAmount(P.X, P.Y)
                        + 0.25f * FMath::PerlinNoise2D(P / 900);
                    if (Score > Best) { Best = Score; Weedy = P; }
                }
        }
        {
            // Nearly straight down, so the sun's glare off the surface does not hide the bed.
            const FVector Eye(Weedy - FVector2D(140, 90), LakeMap::SurfaceZ + 360);
            Shots.Add({ 4.0f, TEXT("Bottom_Above.png"), Eye, FVector(Weedy + FVector2D(60, 40), LakeMap::SurfaceZ - 140), 55 });
        }
        {
            const FVector2D P = Off;
            const float Bed = LakeMap::Ground(P.X, P.Y);
            const FVector Eye(P, Bed + FMath::Min(110.0f, (LakeMap::SurfaceZ - Bed) * 0.45f));
            const FVector2D Dir = FVector2D(1, 0).GetRotated(30);
            Shots.Add({ 6.0f, TEXT("Bottom_Weeds.png"), Eye, FVector(P + Dir * 480, BedAt(P + Dir * 480) + 30), 75 });
        }
        {
            const FVector2D P = Hump;
            const float Bed = LakeMap::Ground(P.X, P.Y);
            const FVector Eye(P, Bed + FMath::Min(120.0f, (LakeMap::SurfaceZ - Bed) * 0.45f));
            const FVector2D Dir = FVector2D(1, 0).GetRotated(200);
            Shots.Add({ 8.0f, TEXT("Bottom_Hump.png"), Eye, FVector(P + Dir * 520, BedAt(P + Dir * 520) + 30), 75 });
        }
        {
            const FVector2D P = Middle;
            const float Bed = LakeMap::Ground(P.X, P.Y);
            const FVector Eye(P, Bed + 150.0f);
            const FVector2D Dir = FVector2D(1, 0).GetRotated(120);
            Shots.Add({ 10.0f, TEXT("Bottom_Deep.png"), Eye, FVector(P + Dir * 900, BedAt(P + Dir * 900) + 60), 75 });
        }
        {
            // The heap of old logs by the fishing spot of that name.
            for (const LakeMap::FSpot& Spot : LakeMap::Spots())
            {
                if (!FString(Spot.Name).Contains(TEXT("Коряжник"))) continue;
                const FVector2D P = Spot.Location - Spot.Outward * 500;
                const float Bed = LakeMap::Ground(P.X, P.Y);
                const FVector Eye(P, Bed + 130.0f);
                const FVector2D Dir = -Spot.Outward;
                Shots.Add({ 11.5f, TEXT("Bottom_Logs.png"), Eye, FVector(P + Dir * 420, BedAt(P + Dir * 420) + 40), 75 });
                break;
            }
        }
        {
            const FVector Eye(Bay, LakeMap::SurfaceZ + 220);
            Shots.Add({ 12.0f, TEXT("Bottom_Bay.png"), Eye, FVector(Bay + FVector2D(-700, 500), LakeMap::SurfaceZ - 160), 62 });
        }
        {
            // Along the surface over the shallows, the weed tops showing through the water.
            const FVector Eye(Weedy - FVector2D(900, 500), LakeMap::SurfaceZ + 125);
            Shots.Add({ 14.0f, TEXT("Bottom_Surface.png"), Eye, FVector(Weedy + FVector2D(500, 300), LakeMap::SurfaceZ - 55), 70 });
        }
        for (const FShot& S : Shots)
        {
            FTimerHandle Handle;
            const FShot Copy = S;
            GetWorldTimerManager().SetTimer(Handle, [this, Shot, Copy] { Shot(Copy.File, Copy.Eye, Copy.Look, Copy.Fov); }, S.At, false);
        }
        FTimerHandle Exit;
        GetWorldTimerManager().SetTimer(Exit, [] { FPlatformMisc::RequestExit(false); }, 18.0f, false);
        return;
    }
    // -ScreenshotTest: presses F12 through the real input path and checks that a file appears.
    if (FParse::Param(FCommandLine::Get(), TEXT("ScreenshotTest")) && GetNetMode() == NM_Standalone)
    {
        FTimerHandle Press, Check, Exit;
        GetWorldTimerManager().SetTimer(Press, [this]
        {
            if (APlayerController* PC = Cast<APlayerController>(GetController()))
            {
                PC->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::F12, IE_Pressed, 1.0f));
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_SCREENSHOT_TEST pressed"));
            }
        }, 12.0f, false);
        GetWorldTimerManager().SetTimer(Check, [this]
        {
            TArray<FString> Files;
            IFileManager::Get().FindFiles(Files, *(FPaths::ProjectSavedDir() / TEXT("Screenshots/*.png")), true, false);
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_SCREENSHOT_TEST %d files in Saved/Screenshots"), Files.Num());
        }, 14.0f, false);
        GetWorldTimerManager().SetTimer(Exit, [] { FPlatformMisc::RequestExit(false); }, 15.0f, false);
        return;
    }
    // -FisherPreview: the angler from the front, the side and the back, then the face close up.
    if (FParse::Param(FCommandLine::Get(), TEXT("FisherPreview")) && GetNetMode() == NM_Standalone)
    {
        struct FShot { float YawOffset, Distance, Height, TargetZ, Fov; const TCHAR* File; };
        static const FShot Shots[] = {
            { 0.0f, 520.0f, 25.0f, 35.0f, 42.0f, TEXT("Fisher_Front.png") },
            { 90.0f, 520.0f, 25.0f, 35.0f, 42.0f, TEXT("Fisher_Side.png") },
            { 180.0f, 520.0f, 25.0f, 35.0f, 42.0f, TEXT("Fisher_Back.png") },
            { 30.0f, 170.0f, 5.0f, 68.0f, 30.0f, TEXT("Fisher_Face.png") },   // face, collar and cap close up
        };
        TWeakObjectPtr<ACameraActor> Cam;
        APlayerController* PC = Cast<APlayerController>(GetController());
        if (PC && PC->MyHUD) PC->MyHUD->bShowHUD = false;
        // A third-person body: the head and the cap are hidden only in the first person.
        bThirdPerson = true;
        // Neutral light for judging the colours: clear noon instead of the dawn the game starts in.
        if (ALakeWeather* Weather = ALakeWeather::Get(GetWorld()))
        {
            FLakeWeatherState Clear;
            Clear.Cloud = 0.15f;
            Clear.Fog = 0.10f;
            Clear.Rain = 0.0f;
            Clear.Wind = 0.10f;
            Weather->SetWeatherNow(Clear);
            Weather->SetHours(13.0f);
        }
        for (int32 I = 0; I < UE_ARRAY_COUNT(Shots); ++I)
        {
            const FShot Shot = Shots[I];
            const float At = 14.0f + I * 2.5f;
            FTimerHandle Pose, Snap;
            GetWorldTimerManager().SetTimer(Pose, [this, Shot, &Cam, PC]
            {
                const FVector Target = GetActorLocation() + FVector(0, 0, Shot.TargetZ);
                const FVector Dir = FRotator(0, GetActorRotation().Yaw + Shot.YawOffset, 0).Vector();
                const FVector From = Target + Dir * Shot.Distance + FVector(0, 0, Shot.Height);
                ACameraActor* Camera = Cam.Get();
                if (!Camera)
                {
                    Camera = GetWorld()->SpawnActor<ACameraActor>(From, (Target - From).Rotation());
                    Cam = Camera;
                }
                if (Camera)
                {
                    Camera->SetActorLocationAndRotation(From, (Target - From).Rotation());
                    Camera->GetCameraComponent()->SetFieldOfView(Shot.Fov);
                    if (PC) PC->SetViewTarget(Camera);
                }
            }, At - 1.0f, false);
            GetWorldTimerManager().SetTimer(Snap, [Shot]
            {
                FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / Shot.File, true, false);
            }, At, false);
        }
        FTimerHandle Exit;
        GetWorldTimerManager().SetTimer(Exit, [] { FPlatformMisc::RequestExit(false); }, 14.0f + UE_ARRAY_COUNT(Shots) * 2.5f + 1.0f, false);
        return;
    }
    // -PlantPreview: the plants grown in the Procedural Vegetation Editor (Tools/CreateGrownPlants.py)
    // stood in a row on the bank behind the angler, the shore plants (Tools/GenerateShorePlants.py) in
    // front of them, at clear noon: the whole row, the conifers, the broadleaves and shrubs, the reeds
    // and sedges, then the bark of the birch, the alder and the pine close up. Frames
    // Saved/Plants_*.png, each plant's size in QUIET_LAKE_PLANT lines.
    if (FParse::Param(FCommandLine::Get(), TEXT("PlantPreview")) && GetNetMode() == NM_Standalone)
    {
        // Trees along the back row, shrubs and shore plants in front of them; Along is metres along the
        // bank, Back metres inland. Mesh /Game/Plants/<Folder>/<Name>.
        struct FPlant { const TCHAR* Folder; const TCHAR* Name; float Along, Back; };
        static const FPlant Plants[] = {
            { TEXT("PV"), TEXT("SM_PV_Pine_A"), -27, 34 }, { TEXT("PV"), TEXT("SM_PV_Pine_B"), -18, 34 },
            { TEXT("PV"), TEXT("SM_PV_Spruce_A"), -9, 34 }, { TEXT("PV"), TEXT("SM_PV_Spruce_B"), 0, 34 },
            { TEXT("PV"), TEXT("SM_PV_Birch_A"), 9, 34 }, { TEXT("PV"), TEXT("SM_PV_Birch_B"), 18, 34 },
            { TEXT("PV"), TEXT("SM_PV_Alder_A"), 27, 34 },
            { TEXT("PV"), TEXT("SM_PV_Shrub_A"), 4, 24 }, { TEXT("PV"), TEXT("SM_PV_Shrub_B"), 13, 24 },
            { TEXT("Shore"), TEXT("SM_Reed_A"), -22, 16 }, { TEXT("Shore"), TEXT("SM_Reed_B"), -18.5f, 16 },
            { TEXT("Shore"), TEXT("SM_Bulrush"), -15, 16 }, { TEXT("Shore"), TEXT("SM_Sedge_A"), -12.5f, 16 },
            { TEXT("Shore"), TEXT("SM_Sedge_B"), -10.8f, 16 },
        };
        // Camera: Along/Back of the eye and of the target (metres), their heights (metres) and the field of view.
        struct FShot { float FromAlong, FromBack, FromUp, ToAlong, ToBack, ToUp, Fov; const TCHAR* File; };
        static const FShot Shots[] = {
            { 0, -12, 4, 0, 34, 8, 70, TEXT("Plants_Row.png") },
            { -13, 8, 3, -13, 34, 9, 62, TEXT("Plants_Conifers.png") },
            { 15, 58, 2.5f, 15, 30, 6, 62, TEXT("Plants_Broadleaf.png") },        // from inland, the sun behind the camera
            { -16, 22.5f, 1.7f, -16, 16, 1.0f, 60, TEXT("Plants_Shore.png") },
            { 9, 37.5f, 1.6f, 9, 34, 1.8f, 50, TEXT("Plants_BirchBark.png") },
            { 27, 37.5f, 1.6f, 27, 34, 1.8f, 50, TEXT("Plants_AlderBark.png") },
            { -27, 37.5f, 1.6f, -27, 34, 2.2f, 50, TEXT("Plants_PineBark.png") },
        };
        APlayerController* PC = Cast<APlayerController>(GetController());
        if (PC && PC->MyHUD) PC->MyHUD->bShowHUD = false;
        if (ALakeWeather* Weather = ALakeWeather::Get(GetWorld()))
        {
            FLakeWeatherState Clear;
            Clear.Cloud = 0.15f;
            Clear.Fog = 0.10f;
            Clear.Rain = 0.0f;
            Clear.Wind = 0.10f;
            Weather->SetWeatherNow(Clear);
            Weather->SetHours(13.0f);
        }
        // The bank's axes at the angler: Out inland, Along the waterline.
        TSharedRef<FVector2D> Base = MakeShared<FVector2D>(), Out = MakeShared<FVector2D>(), Along = MakeShared<FVector2D>();
        auto Point = [Base, Out, Along](float AlongM, float BackM, float UpM)
        {
            const FVector2D P = *Base + *Along * (AlongM * 100.0f) + *Out * (BackM * 100.0f);
            return FVector(P.X, P.Y, LakeMap::Ground(P.X, P.Y) + UpM * 100.0f);
        };
        FTimerHandle Plant;
        GetWorldTimerManager().SetTimer(Plant, [this, Base, Out, Along, Point]
        {
            *Base = FVector2D(GetActorLocation());
            *Out = LakeMap::NearestShore(*Base).Outward.GetSafeNormal();
            *Along = FVector2D(-Out->Y, Out->X);
            for (const FPlant& Entry : Plants)
            {
                const FString Path = FString::Printf(TEXT("/Game/Plants/%s/%s.%s"), Entry.Folder, Entry.Name, Entry.Name);
                UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Path);
                if (!Mesh) { UE_LOG(LogTemp, Warning, TEXT("QUIET_LAKE_PLANT %s missing"), Entry.Name); continue; }
                AStaticMeshActor* Actor = GetWorld()->SpawnActor<AStaticMeshActor>(Point(Entry.Along, Entry.Back, 0), FRotator::ZeroRotator);
                if (!Actor) continue;
                Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
                Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
                const FBox Box = Mesh->GetBoundingBox();
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_PLANT %s height %.1f m, crown %.1f x %.1f m"),
                    Entry.Name, Box.Max.Z / 100.0f, Box.GetSize().X / 100.0f, Box.GetSize().Y / 100.0f);
            }
        }, 11.0f, false);
        TSharedRef<TWeakObjectPtr<ACameraActor>> Cam = MakeShared<TWeakObjectPtr<ACameraActor>>();
        for (int32 I = 0; I < UE_ARRAY_COUNT(Shots); ++I)
        {
            const FShot Shot = Shots[I];
            const float At = 15.0f + I * 3.0f;
            FTimerHandle Pose, Snap;
            GetWorldTimerManager().SetTimer(Pose, [this, Shot, Cam, PC, Point]
            {
                const FVector From = Point(Shot.FromAlong, Shot.FromBack, Shot.FromUp), Target = Point(Shot.ToAlong, Shot.ToBack, Shot.ToUp);
                ACameraActor* Camera = Cam->Get();
                if (!Camera) { Camera = GetWorld()->SpawnActor<ACameraActor>(From, (Target - From).Rotation()); *Cam = Camera; }
                if (!Camera) return;
                Camera->SetActorLocationAndRotation(From, (Target - From).Rotation());
                Camera->GetCameraComponent()->SetFieldOfView(Shot.Fov);
                if (PC) PC->SetViewTarget(Camera);
            }, At - 1.5f, false);
            GetWorldTimerManager().SetTimer(Snap, [Shot]
            {
                FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / Shot.File, false, false);
            }, At, false);
        }
        FTimerHandle Exit;
        GetWorldTimerManager().SetTimer(Exit, [] { FPlatformMisc::RequestExit(false); }, 15.0f + UE_ARRAY_COUNT(Shots) * 3.0f + 1.0f, false);
        return;
    }
    // -CasePreview: the rod case on the back, its ghost (B), laid on the ground, the prompt over it, the
    // lid open with its window, a rod put away and taken out, a rod laid on the ground (R held), then
    // three rods cast one after another and left fishing on their stands. Frames Saved/Case_*.png,
    // the state at each step in QUIET_LAKE_CASE lines.
    if (FParse::Param(FCommandLine::Get(), TEXT("CasePreview")) && GetNetMode() == NM_Standalone)
    {
        auto At = [this](float Seconds, TFunction<void()> Action)
        {
            FTimerHandle Handle;
            GetWorldTimerManager().SetTimer(Handle, [Action] { Action(); }, Seconds, false);
        };
        // bUI: with the HUD (the prompt, the window).
        auto Shot = [this](const TCHAR* Name, bool bUI)
        {
            if (const APlayerController* PC = Cast<APlayerController>(GetController())) if (PC->MyHUD) PC->MyHUD->bShowHUD = bUI;
            FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / Name, bUI, false);
        };
        TSharedRef<TWeakObjectPtr<ACameraActor>> Cam = MakeShared<TWeakObjectPtr<ACameraActor>>();
        // A camera placed relative to the angler: Back cm behind him, Side to his right, Up; looking at a point Ahead of him at Height.
        auto LookFrom = [this, Cam](float Back, float Side, float Up, float Ahead, float Height, float Fov)
        {
            const FVector Fwd = FRotator(0, GetActorRotation().Yaw, 0).Vector(), Right = FRotator(0, GetActorRotation().Yaw + 90, 0).Vector();
            const FVector Feet = GetActorLocation() - FVector(0, 0, 90);
            const FVector From = Feet - Fwd * Back + Right * Side + FVector(0, 0, Up);
            const FVector Target = Feet + Fwd * Ahead + FVector(0, 0, Height);
            ACameraActor* Camera = Cam->Get();
            if (!Camera) { Camera = GetWorld()->SpawnActor<ACameraActor>(From, (Target - From).Rotation()); *Cam = Camera; }
            Camera->SetActorLocationAndRotation(From, (Target - From).Rotation());
            Camera->GetCameraComponent()->SetFieldOfView(Fov);
            if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetViewTarget(Camera);
        };
        auto Log = [this](const TCHAR* Step)
        {
            FString Rods_;
            for (int32 Rod = 0; Rod < Rods.Num(); ++Rod)
                Rods_ += FString::Printf(TEXT(" rod%d=%d/slot%d/state%d"), Rod, (int32)Rods[Rod].Place, (int32)Rods[Rod].CaseSlot, (int32)Rods[Rod].State);
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_CASE %s case=%d open=%d window=%d hands=%d kind=%d fishing=%d placing=%d valid=%d%s status=%s"), Step, (int32)CasePlace, bCaseOpen,
                IsCaseWindowOpen(), HasRodInHands(), (int32)RodKind, (int32)FishingState, (int32)Placing, bPlaceValid, *Rods_, *Status);
        };
        At(4.0f, [this, LookFrom]
        {
            if (ALakeWeather* Weather = ALakeWeather::Get(GetWorld()))
            {
                FLakeWeatherState Calm;
                Calm.Cloud = 0.2f; Calm.Fog = 0.05f; Calm.Rain = 0; Calm.Wind = 0.03f; Calm.WindYaw = 40;
                Weather->SetWeatherNow(Calm);
                Weather->SetHours(12.0f);
            }
            FVector2D Facing;
            const FVector2D Spot = LakeMap::StartSpot(&Facing) + Facing * 60;
            SetActorLocation(FVector(Spot, ALakeEnvironment::GroundHeight(Spot.X, Spot.Y) + 95), false, nullptr, ETeleportType::TeleportPhysics);
            // Facing the bank for the case (it goes on the land behind him once he turns to the water).
            if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetControlRotation(FRotator(-12, FMath::RadiansToDegrees(FMath::Atan2(-Facing.Y, -Facing.X)), 0));
            bThirdPerson = true;
            FloatDepth = 40;
            LookFrom(-60, 330, 150, 0, 110, 50);   // from behind his right, the case across the back
        });
        At(5.0f, [LookFrom] { LookFrom(260, 140, 170, 0, 110, 45); });
        At(6.5f, [Log] { Log(TEXT("back")); });
        At(7.0f, [Shot] { Shot(TEXT("Case_Back.png"), false); });
        // In the first person the case on the back stays out of the view.
        At(7.3f, [this]
        {
            bThirdPerson = false;
            FishingCamera->SetActive(true);
            ThirdPersonCamera->SetActive(false);
            if (APlayerController* PC = Cast<APlayerController>(GetController())) { PC->SetControlRotation(FRotator(-35, GetActorRotation().Yaw + 40, 0)); PC->SetViewTarget(this); }
        });
        At(7.7f, [Shot] { Shot(TEXT("Case_FirstPerson.png"), false); });
        At(7.9f, [this] { bThirdPerson = true; if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetControlRotation(FRotator(-12, GetActorRotation().Yaw - 40, 0)); });
        // B: the ghost under the aim (the camera looks down at the ground in front of him).
        At(8.0f, [this, LookFrom] { CaseKey(); LookFrom(230, -60, 230, 120, 0, 55); });
        At(9.5f, [Log] { Log(TEXT("ghost")); });
        At(10.0f, [Shot] { Shot(TEXT("Case_Ghost.png"), true); });
        At(10.5f, [this] { Fish(); });   // LMB: laid there
        At(11.5f, [this, LookFrom] { LookFrom(160, -170, 130, 110, 0, 55); });
        At(12.5f, [Log] { Log(TEXT("ground")); });
        At(13.0f, [Shot] { Shot(TEXT("Case_Ground.png"), false); });
        // Looking at it: the prompt; open it.
        At(13.5f, [this]
        {
            // Look at the case from the angler's own eyes (first person, as in play).
            bThirdPerson = false;
            FishingCamera->SetActive(true);
            ThirdPersonCamera->SetActive(false);
            if (APlayerController* PC = Cast<APlayerController>(GetController()))
            {
                const FVector Eye = FishingCamera->GetComponentLocation();
                PC->SetControlRotation((FVector(CaseBase) + FVector(0, 0, 5) - Eye).Rotation());
                PC->SetViewTarget(this);
            }
        });
        At(14.5f, [Shot] { Shot(TEXT("Case_Prompt.png"), true); });
        At(15.0f, [this] { InteractPressed(); });
        At(15.7f, [this, Log] { InteractReleased(); Log(TEXT("open")); });
        At(16.5f, [this, LookFrom]
        {
            bThirdPerson = true;
            LookFrom(100, 0, 100, 0, 0, 50);   // back to a camera of its own
            // The open lid and the rods inside, close up from the side.
            const FVector Case = FVector(CaseBase);
            const FVector Across = FRotator(0, CaseYaw + 90, 0).Vector();
            const FVector From = Case + Across * 45 + FVector(0, 0, 150);   // above it from the angler's side: the lid opens away from him
            if (APlayerController* PC = Cast<APlayerController>(GetController()))
                if (AActor* View = PC->GetViewTarget()) View->SetActorLocationAndRotation(From, (Case - From).Rotation());
        });
        At(17.2f, [Shot] { Shot(TEXT("Case_OpenLid.png"), false); });
        At(17.8f, [Shot] { Shot(TEXT("Case_Open.png"), true); });
        // The medium rod into the middle compartment: three in the case, the hands empty.
        At(18.5f, [this, Log] { ServerMoveRod_Implementation((uint8)ERodKind::Medium, 1); Log(TEXT("stowed")); });
        At(19.5f, [Shot] { Shot(TEXT("Case_Stowed.png"), true); });
        // The light one out; the case closed.
        At(20.5f, [this, Log] { ServerMoveRod_Implementation((uint8)ERodKind::Light, 3); CloseCaseWindow(); Log(TEXT("light out")); });
        // R held: the rod's ghost on the ground, laid there, and picked up again.
        At(21.5f, [this, LookFrom] { LookFrom(-60, 520, 260, 170, 0, 65); LayPressed(); });   // from the side: the rod's ghost lies across the view
        At(23.8f, [this, Log] { Log(TEXT("rod ghost")); });
        At(24.0f, [Shot] { Shot(TEXT("Case_RodGhost.png"), true); });
        At(24.3f, [this] { LayReleased(); Fish(); });
        At(25.5f, [this, Log, LookFrom] { LookFrom(-60, 520, 260, 170, 0, 65); Log(TEXT("rod on the ground")); });
        At(26.0f, [Shot] { Shot(TEXT("Case_RodGround.png"), false); });
        At(26.5f, [this, Log] { ServerTakeRod_Implementation((uint8)ERodKind::Light); Log(TEXT("rod picked up")); });
        // Three casts, each rod onto its own stand; the next one from the case.
        const float CastAt[] = { 28.0f, 37.0f, 46.0f };
        const ERodKind Next[] = { ERodKind::Medium, ERodKind::Heavy, ERodKind::Count };
        const float Turn[] = { -14.0f, 0.0f, 14.0f };
        for (int32 C = 0; C < 3; ++C)
        {
            const float T = CastAt[C];
            At(T - 0.6f, [this, C, Turn]
            {
                FVector2D Facing;
                LakeMap::StartSpot(&Facing);
                if (APlayerController* PC = Cast<APlayerController>(GetController()))
                    PC->SetControlRotation(FRotator(-12, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)) + Turn[C], 0));
                SwingPitch = 0;
            });
            for (int32 I = 0; I <= 100; ++I)
                At(T + I * 0.025f, [this, I]
                {
                    const float U = I * 0.025f;
                    const float Target = U < 0.3f ? 0.0f : U < 0.65f ? 35 * FMath::SmoothStep(0.3f, 0.65f, U) : U < 1.0f ? 35.0f : FMath::Lerp(35.0f, -30.0f, FMath::SmoothStep(1.0f, 1.6f, U));
                    const float ViewOffset = FMath::FindDeltaAngleDegrees(-12.0f, GetControlRotation().Pitch);
                    MoveRodAxis(Target - SwingPitch - ViewOffset, true);
                });
            At(T, [this] { NextActionTime = 0; ServerFish_Implementation(); });
            At(T + 0.4f, [this] { ServerRelease_Implementation(); });
            At(T + 5.0f, [this, Log] { Log(TEXT("cast")); ServerToggleRod_Implementation(); Log(TEXT("on its stand")); });
            if (Next[C] != ERodKind::Count)
                At(T + 6.0f, [this, Next, C, Log]
                {
                    ServerCaseAction_Implementation(0);
                    ServerMoveRod_Implementation((uint8)Next[C], 3);
                    ServerCaseAction_Implementation(1);
                    Log(TEXT("next rod"));
                });
        }
        At(52.5f, [LookFrom] { LookFrom(380, 260, 330, 300, 0, 65); });
        At(54.0f, [Log] { Log(TEXT("three stands")); });
        At(54.5f, [Shot] { Shot(TEXT("Case_ThreeStands.png"), true); });
        // The case back on the back.
        At(55.0f, [this, Log] { ServerCaseAction_Implementation(2); Log(TEXT("worn")); });
        At(56.5f, [] { FPlatformMisc::RequestExit(false); });
        return;
    }
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
        At(63.0f, [this] { NextActionTime = 0; ServerFish_Implementation(); });   // LMB held: lift the rig out of the water
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
        At(7.2f, [this] { ServerRelease_Implementation(); });
        At(8.5f, [this]
        {
            const float Water = LakeFish::SurfaceZ - ALakeEnvironment::GroundHeight(FloatPosition.X, FloatPosition.Y);
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_BITE sounded %.0f cm of water"), Water);
            NextActionTime = 0;
            ServerFish_Implementation();   // LMB held: lift the rig out
            FloatDepth = FMath::Clamp(FMath::RoundToFloat(Water + 2), MinDepth, MaxDepth);
        });
        // (Lifting the rig out and taking it in the hand takes about 6 s.)
        At(16.0f, [this] { NextActionTime = 0; ServerFish_Implementation(); });
        At(16.0f + CastLoad, [this] { ServerRelease_Implementation(); });
        // The cast from the hand lands a little elsewhere: the depth set for where it lay (as if sounded there).
        At(19.5f, [this]
        {
            const float Water = LakeFish::SurfaceZ - ALakeEnvironment::GroundHeight(FloatPosition.X, FloatPosition.Y);
            FloatDepth = HookDepth = FMath::Clamp(FMath::RoundToFloat(Water + 2), MinDepth, MaxDepth);
            PlaceBait();
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_BITE landed on %.0f cm of water, bait set at %.0f cm"), Water, FloatDepth);
        });
        At(20.0f, [this]
        {
            if (!RodVisual) return;
            const FVector Float = RodVisual->BobberLocation();
            const FVector From = Float + GetActorRightVector() * 80 - GetActorForwardVector() * 40 + FVector(0, 0, 18);
            auto* Cam = GetWorld()->SpawnActor<ACameraActor>(From, (Float - From).Rotation());
            Cam->GetCameraComponent()->SetFieldOfView(35);
            if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetViewTarget(Cam);
        });
        At(20.60f, [Shot] { Shot(TEXT("Bite_Still.png")); });
        struct FDemo { float At; EBiteMotion Motion; float Duration, Amount; EFishingState State; };
        const FDemo Demos[] = {
            { 21.0f, EBiteMotion::Tremble, 1.6f, 0.8f, EFishingState::Waiting },
            { 23.0f, EBiteMotion::Peck, 0.35f, 1.8f, EFishingState::Waiting },
            { 25.0f, EBiteMotion::Lift, 2.0f, 3.2f, EFishingState::Bite },
            { 28.0f, EBiteMotion::Drift, 3.0f, 8.0f, EFishingState::Bite },
            { 31.5f, EBiteMotion::Sink, 2.5f, 55.0f, EFishingState::Bite },
            { 34.5f, EBiteMotion::Hold, 3.0f, 0.0f, EFishingState::Bite },   // a perch swallowing, a pike turning its prey
        };
        for (const FDemo& Demo : Demos)
            At(Demo.At, [this, Demo]
            {
                FishingState = Demo.State;
                FBiteStep Step;
                Step.Motion = Demo.Motion; Step.Duration = Demo.Duration; Step.Amount = Demo.Amount; Step.bTake = Demo.State == EFishingState::Bite;
                OnBiteMotion(nullptr, Step);
            });
        At(21.50f, [Shot] { Shot(TEXT("Bite_Tremble.png")); });
        At(23.17f, [Shot] { Shot(TEXT("Bite_Peck.png")); });
        At(26.00f, [Shot] { Shot(TEXT("Bite_Lift.png")); });
        auto Aim = [this]
        {
            if (!RodVisual) return;
            const FVector Float = RodVisual->BobberLocation();
            const FVector From = Float + GetActorRightVector() * 70 - GetActorForwardVector() * 30 + FVector(0, 0, 22);
            auto* Cam = GetWorld()->SpawnActor<ACameraActor>(From, (Float - From).Rotation());
            Cam->GetCameraComponent()->SetFieldOfView(35);
            if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetViewTarget(Cam);
        };
        At(27.40f, Aim);
        At(27.60f, [Shot] { Shot(TEXT("Bite_Lie.png")); });
        At(29.80f, Aim);
        At(30.00f, [Shot] { Shot(TEXT("Bite_Drift.png")); });
        At(32.00f, [Shot] { Shot(TEXT("Bite_Sink.png")); });
        At(35.80f, Aim);
        At(36.00f, [Shot] { Shot(TEXT("Bite_Hold.png")); });
        for (int32 I = 0; I <= 170; ++I)
            At(20.50f + I * 0.1f, [this, I]
            {
                if (RodVisual) UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_BITE t=%.1f motion=%d antenna above water=%.1f cm tilt=%.0f keel %.1f shot %.1f hook %.1f (bottom %.1f)"), 20.5f + I * 0.1f, (int32)BiteMotion,
                    RodVisual->FloatTip().Z - LakeFish::SurfaceZ, RodVisual->GetBiteTilt(), RodVisual->FloatKeel().Z - LakeFish::SurfaceZ,
                    RodVisual->ShotLocation().Z - LakeFish::SurfaceZ, RodVisual->HookLocation().Z - LakeFish::SurfaceZ,
                    ALakeEnvironment::GroundHeight(RodVisual->HookLocation().X, RodVisual->HookLocation().Y) - LakeFish::SurfaceZ);
            });
        At(38.0f, [] { FPlatformMisc::RequestExit(false); });
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
        At(7.2f, [this] { ServerRelease_Implementation(); });
        At(9.0f, [this]
        {
            const FVector Bait = BaitLocation();
            ALakeFish* Fish = GetWorld()->SpawnActor<ALakeFish>(Bait, FRotator(0, GetActorRotation().Yaw + 90, 0));
            Fish->Setup(EFishSpecies::Perch, 0.32f, Bait, -1);
            Fish->SetActorLocation(Bait - Fish->GetActorQuat().RotateVector(Fish->MouthOffset()));
            Fish->Hook(this); // preview fixture, not the gameplay strike path
            HookedFish = Fish; LineFish = Fish; FishingState = EFishingState::Fight;
            Tension = 0; FightTime = 0; bReeling = false;
            FTimerHandle Draw;
            GetWorldTimerManager().SetTimer(Draw, [this] { bReeling = true; }, 2.0f, false);
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
        // The fish is taken in the free hand for a moment before it goes into the keepnet: film it
        // there, whenever exactly the catch comes.
        At(15.2f, [this, LookFrom]
        {
            LookFrom(GetActorLocation() + GetActorRightVector() * 235 + GetActorForwardVector() * 150 + FVector(0, 0, 55),
                     HandPoint(0) - FVector(0, 0, 18), 52);
        });
        {
            auto Probe = MakeShared<FTimerHandle>();
            GetWorldTimerManager().SetTimer(*Probe, FTimerDelegate::CreateLambda([this, Shot, Probe]
            {
                if (!bFishInHand) return;
                Shot(TEXT("Land_InHand.png"));
                GetWorldTimerManager().ClearTimer(*Probe);
            }), 0.05f, true);
        }
        for (int32 I = 0; I <= 160; ++I)
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
        At(21.0f, [Shot] { Shot(TEXT("Land_Final.png")); });
        At(27.0f, [] { FPlatformMisc::RequestExit(false); });
    }
    // -FightPreview: a hooked fish played from the bank on a clear morning, filmed from the side at
    // the water's edge: how it holds and runs under the surface, what its body does and how heavy it
    // feels on the line. The angler's hand on the rod is imitated: pull while the line is slack, give
    // line when it is hard.
    if (FParse::Param(FCommandLine::Get(), TEXT("FightPreview")) && GetNetMode() == NM_Standalone)
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
                Calm.Cloud = 0.2f; Calm.Fog = 0; Calm.Rain = 0; Calm.Wind = 0.03f; Calm.WindYaw = 40;
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
            FloatDepth = 150;
        });
        At(6.0f, [this] { NextActionTime = 0; ServerFish_Implementation(); });
        At(7.2f, [this] { ServerRelease_Implementation(); });
        At(9.0f, [this]
        {
            const FVector Bait = BaitLocation();
            ALakeFish* Fish = GetWorld()->SpawnActor<ALakeFish>(Bait, FRotator(0, GetActorRotation().Yaw + 90, 0));
            if (FParse::Param(FCommandLine::Get(), TEXT("LiveRigPreview")))
            {
                Fish->Setup(EFishSpecies::Perch, FParse::Param(FCommandLine::Get(), TEXT("LiveRigHeavy")) ? 0.8f : 0.18f, Bait, -1);
                Fish->Random.Initialize(714);
                Fish->SetActorLocation(Bait - Fish->MouthOffset() + FVector(5, 0, 0));
                Fish->State = ALakeFish::EState::Investigate; Fish->TargetAngler = this;
                auto Timer = MakeShared<FTimerHandle>();
                auto SeenBite = MakeShared<float>(-1);
                auto Previous = MakeShared<int32>(-1);
                auto FightStart = MakeShared<float>(-1);
                auto Released = MakeShared<bool>(false);
                GetWorldTimerManager().SetTimer(*Timer, FTimerDelegate::CreateLambda([this, Fish, SeenBite, Previous, FightStart, Released]
                {
                    const float Now = ServerNow();
                    if (FishingState == EFishingState::Bite && *SeenBite < 0) *SeenBite = Now;
                    if (FishingState == EFishingState::Bite && Now - *SeenBite > 0.2f)
                    {
                        ServerFish_Implementation();
                        if (Now - *SeenBite < 0.31f && !FParse::Param(FCommandLine::Get(), TEXT("LiveRigLmbOnly"))) ServerCancel_Implementation();
                    }
                    if (FishingState == EFishingState::Fight || FishingState == EFishingState::Landing)
                    {
                        if (Tension > 0.65f) ServerRelease_Implementation();
                        else if (Tension < 0.3f) ServerFish_Implementation();
                    }
                    if (FishingState == EFishingState::Fight)
                    {
                        if (*FightStart < 0) *FightStart = Now;
                        if (!*Released && Now - *FightStart > 0.5f)
                        {
                            ServerRelease_Implementation(); *Released = true;
                            UE_LOG(LogTemp, Display, TEXT("LIVE_RIG release lift=%.2f tension=%.3f"), RodLift, Tension);
                        }
                        if (*Released && Now - *FightStart < 0.8f) ServerRelease_Implementation();
                    }
                    if (*Previous != int32(FishingState) || FishingState == EFishingState::Bite ||
                        (FishingState == EFishingState::Fight && Now - *FightStart < 2))
                        UE_LOG(LogTemp, Display, TEXT("LIVE_RIG t=%.2f state=%d fish=%d lift=%.2f elev=%.1f force=%.1f contactOld=%.3f contactNow=%.3f impulse=%.2f grip=%.3f caught=%d status=%s"),
                            Now, int32(FishingState), int32(Fish->State), RodLift, RodElevation, LoadOnFish(Fish).Size(),
                            FVector::Dist(Fish->MouthLocation(), BaitPoint), FVector::Dist(Fish->MouthLocation(), RodVisual->HookLocation()), HookImpulse, Fish->HookGrip, CatchCount, *Status);
                    *Previous = int32(FishingState);
                    if (CatchCount > 0)
                    {
                        UE_LOG(LogTemp, Display, TEXT("LIVE_RIG SUCCESS caught=%d inHand=%d weight=%.2f t=%.2f grip=%.3f"), CatchCount, bFishInHand ? 1 : 0, Fish->WeightKg, Now, Fish->HookGrip);
                        FPlatformMisc::RequestExit(false);
                    }
                }), 0.1f, true);
                return;
            }
            Fish->Setup(EFishSpecies::Crucian, 0.8f, Bait, -1);
            Fish->SetActorLocation(Bait - Fish->GetActorQuat().RotateVector(Fish->MouthOffset()));
            Fish->Hook(this); // preview fixture, not the gameplay strike path
            HookedFish = Fish; LineFish = Fish; FishingState = EFishingState::Fight;
            Tension = 0; FightTime = 0; bReeling = false;
            FTimerHandle Draw;
            GetWorldTimerManager().SetTimer(Draw, [this] { bReeling = true; }, 2.0f, false);
        });
        {
            auto Play = MakeShared<FTimerHandle>();
            GetWorldTimerManager().SetTimer(*Play, FTimerDelegate::CreateLambda([this]
            {
                if (FishingState != EFishingState::Fight) return;
                if (Tension > 0.5f) ServerRelease_Implementation();
                else if (Tension < 0.25f) ServerFish_Implementation();
            }), 0.2f, true);
        }
        auto Watch = [this, Shot, LookFrom, At](float T, const TCHAR* Name)
        {
            At(T, [this, Name, Shot, LookFrom]
            {
                ALakeFish* Fish = HookedFish.Get();
                if (!Fish) return;
                const FVector P = Fish->GetActorLocation();
                // Under the surface beside the fish: the murk is thick, so stay close.
                if (FParse::Param(FCommandLine::Get(), TEXT("ContactPreview")))
                {
                    const FVector Mouth = Fish->MouthLocation();
                    LookFrom(Mouth + GetActorRightVector() * 65 + FVector(0, 0, 5), Mouth, 42);
                }
                else LookFrom(P + GetActorRightVector() * 200 + FVector(0, 0, 45), P, 48);
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_FIGHT %s depth %.0f cm, speed %.0f, stamina %.2f, tension %.2f, awake %d, body yaw rate %.0f, on the line %d, mouth to hook %.0f cm"),
                    Name, P.Z - LakeFish::SurfaceZ, Fish->SpeedNow(), Fish->StaminaFraction(), Tension,
                    Fish->IsFightAwake() ? 1 : 0, Fish->ShownYawRate(), LineFish ? 1 : 0,
                    RodVisual ? (float)FVector::Dist(Fish->MouthLocation(), RodVisual->HookLocation()) : -1.0f);
                Shot(Name);
            });
        };
        if (!FParse::Param(FCommandLine::Get(), TEXT("LiveRigPreview")))
        {
        Watch(10.0f, TEXT("Fight_1.png"));
        Watch(11.4f, TEXT("Fight_2.png"));
        Watch(12.8f, TEXT("Fight_3.png"));
        Watch(14.6f, TEXT("Fight_4.png"));
        Watch(17.0f, TEXT("Fight_5.png"));
        }
        else Watch(14.5f, TEXT("LiveRig.png"));
        At(FParse::Param(FCommandLine::Get(), TEXT("LiveRigPreview")) ? 85.0f : 24.0f, [] { FPlatformMisc::RequestExit(false); });
    }
    // -MouthPreview: the head of every species close up, to see the mouth a hook goes into.
    if (FParse::Param(FCommandLine::Get(), TEXT("MouthPreview")) && GetNetMode() == NM_Standalone)
    {
        auto At = [this](float Seconds, TFunction<void()> Action)
        {
            FTimerHandle Handle;
            GetWorldTimerManager().SetTimer(Handle, [Action] { Action(); }, Seconds, false);
        };
        auto Shot = [](const FString& Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / Name, false, false); };
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
                FLakeWeatherState Clear;
                Clear.Cloud = 0.15f; Clear.Fog = 0; Clear.Rain = 0; Clear.Wind = 0.04f;
                Weather->SetWeatherNow(Clear);
                Weather->SetHours(12.0f);
            }
            FVector2D Facing;
            const FVector2D Spot = LakeMap::StartSpot(&Facing);
            SetActorLocation(FVector(Spot + Facing * 40, ALakeEnvironment::GroundHeight(Spot.X, Spot.Y) + 95), false, nullptr, ETeleportType::TeleportPhysics);
            if (APlayerController* PC = Cast<APlayerController>(GetController()))
            {
                PC->SetControlRotation(FRotator(-8, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)), 0));
                if (PC->MyHUD) PC->MyHUD->bShowHUD = false;
            }
        });
        auto Current = MakeShared<TWeakObjectPtr<ALakeFish>>();
        for (int32 I = 0; I < LakeFish::NumSpecies(); ++I)
        {
            At(6.0f + I, [this, I, Current]
            {
                FVector2D Facing;
                const FVector2D Edge = LakeMap::StartSpot(&Facing);
                const EFishSpecies Sp = (EFishSpecies)I;
                const FVector P(Edge + Facing * 330, LakeFish::SurfaceZ - 35);
                const FRotator Facing0(0, FMath::RadiansToDegrees(FMath::Atan2(-Facing.Y, -Facing.X)), 0);
                ALakeFish* Fish = GetWorld()->SpawnActor<ALakeFish>(P, Facing0);
                Fish->Setup(Sp, LakeFish::Info(Sp).MaxWeight * 0.6f, P, -1);
                *Current = Fish;
            });
            At(6.55f + I, [this, I, Current, LookFrom, Shot]
            {
                ALakeFish* Fish = Current->Get();
                if (!Fish) return;
                const FVector Forward = Fish->GetActorForwardVector();
                const FVector Head = Fish->MouthLocation();
                LookFrom(Head + Forward * 52 + FVector(0, 0, 9), Head - Forward * 8, 40);
                const EFishSpecies Sp = (EFishSpecies)I;
                const FString Name = FString::Printf(TEXT("Mouth_%s.png"), *LakeFish::Info(Sp).Id);
                Shot(Name);
            });
            // The whole fish from the side, for its fins (-FinDebug blows the marked fins out).
            At(6.68f + I, [this, I, Current, LookFrom, Shot]
            {
                ALakeFish* Fish = Current->Get();
                if (!Fish) return;
                const float Len = LakeFish::LengthCm(Fish->Species, Fish->WeightKg);
                const FVector Middle = Fish->MouthLocation() - Fish->GetActorForwardVector() * (Len * 0.5f);
                LookFrom(Middle + Fish->GetActorRightVector() * (Len * 1.9f) + FVector(0, 0, Len * 0.15f), Middle, 40);
                Shot(FString::Printf(TEXT("Fin_%s.png"), *LakeFish::Info((EFishSpecies)I).Id));
            });
            At(6.85f + I, [Current] { if (ALakeFish* Fish = Current->Get()) Fish->Destroy(); });
        }
        At(7.0f + LakeFish::NumSpecies(), [] { FPlatformMisc::RequestExit(false); });
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
        // One wide view from the side for the whole cast: the angler, the swing, the rig's flight
        // and where it comes down (out to ~11 m and up to ~6 m).
        auto Side = [this, LookFrom]
        {
            LookFrom(GetActorLocation() + GetActorRightVector() * 1300 + GetActorForwardVector() * 450 + FVector(0, 0, 250),
                     GetActorLocation() + GetActorForwardVector() * 450 + FVector(0, 0, 200), 75);
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
            { 6.0f, 1.0f, -12, ERodKind::Medium },     // a full swing
            { 16.0f, 0.25f, -12, ERodKind::Medium },   // a hurried, small one
            { 26.0f, 1.4f, -12, ERodKind::Medium },    // a slow one
            { 36.0f, 1.0f, -12, ERodKind::Heavy },     // a heavy fibreglass pole: slower, softer
            { 46.0f, 1.0f, -12, ERodKind::Light },     // a light carbon pole: quick
        };
        // The cast is scripted as the mouse would make it: ready at 45 degrees, the rod raised
        // (RaiseTo, over 0.35 s from 0.3 s), LMB let go as it rises (ReleaseAt), the rod held up a
        // moment while the rig swings out, then lowered (LowerTo) to lay it on the water.
        float RaiseTo = 35, LowerTo = -30, ReleaseAt = 0.4f;
        FParse::Value(FCommandLine::Get(), TEXT("RaiseTo="), RaiseTo);
        FParse::Value(FCommandLine::Get(), TEXT("LowerTo="), LowerTo);
        FParse::Value(FCommandLine::Get(), TEXT("ReleaseAt="), ReleaseAt);
        for (const FTry& Try : Tries)
        {
            for (int32 I = 0; I <= 100; ++I)
                At(Try.At + I * 0.025f, [this, I, RaiseTo, LowerTo, Try]
                {
                    const float U = I * 0.025f;
                    const float Target = U < 0.3f ? 0.0f : U < 0.65f ? RaiseTo * FMath::SmoothStep(0.3f, 0.65f, U)
                               : U < 1.0f ? RaiseTo : FMath::Lerp(RaiseTo, LowerTo, FMath::SmoothStep(1.0f, 1.6f, U));
                    const float ViewOffset = FMath::FindDeltaAngleDegrees(Try.Pitch, GetControlRotation().Pitch);
                    MoveRodAxis(Target - SwingPitch - ViewOffset, true);
                });
            At(Try.At - 0.5f, [this, Try]
            {
                UseRod(Try.Rod);
                if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetControlRotation(FRotator(Try.Pitch, GetActorRotation().Yaw, 0));
            });
            At(Try.At, [this] { NextActionTime = 0; ServerFish_Implementation(); });
            // (LMB held as long as the try says: the longer, the farther the rod's sweep carries the rig.)
            At(Try.At + Try.Held, [this] { ServerRelease_Implementation(); });
            (void)ReleaseAt;
            At(Try.At + Try.Held + 1.7f, [this]
            {
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_CASTRESULT result=%d severity=%.2f out=%.0f cm hook beyond float=%.0f cm: %s"), (int32)CastResult, CastSeverity,
                    FVector::Dist2D(FloatPosition, GetActorLocation()), FVector::DotProduct(FVector(CastHookLanding) - FloatPosition, GetActorForwardVector()), *Status);
            });
            // LMB held: the rod goes up and the rig comes out to the left hand (~5 s) before the next cast.
            At(Try.At + Try.Held + 2.2f, [this] { NextActionTime = 0; ServerFish_Implementation(); });
        }
        // -CastQuick: only the first cast (for tuning).
        if (FParse::Param(FCommandLine::Get(), TEXT("CastQuick"))) At(15.0f, [] { FPlatformMisc::RequestExit(false); });
        // The clean cast, shot by shot.
        At(6.5f, [Shot] { Shot(TEXT("Cast_Hold.png")); });
        At(6.72f, [Shot] { Shot(TEXT("Cast_Push.png")); });
        At(7.0f, [Shot] { Shot(TEXT("Cast_Air.png")); });
        At(7.55f, [Shot] { Shot(TEXT("Cast_Land.png")); });
        At(9.5f, [Shot] { Shot(TEXT("Cast_Stand.png")); });
        At(11.2f, [Shot] { Shot(TEXT("Cast_Lift.png")); });
        At(14.8f, [Shot] { Shot(TEXT("Cast_Catch.png")); });
        At(16.5f, [Shot] { Shot(TEXT("Cast_Early.png")); });
        At(27.3f, [Shot] { Shot(TEXT("Cast_Near.png")); });
        At(36.6f, [Shot] { Shot(TEXT("Cast_HeavyHold.png")); });
        At(37.15f, [Shot] { Shot(TEXT("Cast_HeavySwing.png")); });
        At(46.75f, [Shot] { Shot(TEXT("Cast_LightSwing.png")); });
        for (int32 I = 0; I <= 1080; ++I)
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
        // Walking along the bank with the rig in hand: the float should stay on the line, not dangle.
        At(55.5f, Side);
        for (int32 I = 0; I < 50; ++I)
            At(56.0f + I * 0.04f, [this] { AddMovementInput(-GetActorRightVector(), 1.0f); });
        At(57.0f, [Shot] { Shot(TEXT("Cast_Walk.png")); });
        // A deep setting at the deepest fishing spot (the float standing on a long leader), then the
        // same set deeper than the water (the shot on the bottom, the float lying): it should land
        // and settle, not bounce.
        At(60.0f, [this]
        {
            const LakeMap::FSpot* Deepest = nullptr;
            float Most = 0;
            for (const LakeMap::FSpot& S : LakeMap::Spots())
            {
                const FVector2D Out = S.Location - S.Outward * 700;
                const float Water = LakeFish::SurfaceZ - ALakeEnvironment::GroundHeight(Out.X, Out.Y);
                if (Water > Most) { Most = Water; Deepest = &S; }
            }
            const FVector2D Spot = Deepest->Location + Deepest->Outward * 60;
            SetActorLocation(FVector(Spot, ALakeEnvironment::GroundHeight(Spot.X, Spot.Y) + 95), false, nullptr, ETeleportType::TeleportPhysics);
            if (APlayerController* PC = Cast<APlayerController>(GetController()))
                PC->SetControlRotation(FRotator(-12, FMath::RadiansToDegrees(FMath::Atan2(-Deepest->Outward.Y, -Deepest->Outward.X)), 0));
            UseRod(ERodKind::Medium);
            FloatDepth = FMath::Clamp(FMath::RoundToFloat(Most - 40), 100.0f, 300.0f);
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_DEEP spot %s, %.0f cm of water 7 m out, depth set %.0f cm"), Deepest->Name, Most, FloatDepth);
        });
        At(62.0f, Side);
        At(62.5f, [this] { NextActionTime = 0; ServerFish_Implementation(); });
        At(62.5f + CastLoad + 0.1f, [this] { ServerRelease_Implementation(); });
        At(63.6f, [this, LookFrom]
        {
            LookFrom(FloatPosition + GetActorRightVector() * 220 - GetActorForwardVector() * 140 + FVector(0, 0, 90), FloatPosition, 45);
        });
        At(66.0f, [Shot] { Shot(TEXT("Cast_Deep.png")); });
        for (int32 I = 0; I <= 100; ++I)
            At(63.1f + I * 0.05f, [this, I]
            {
                if (!RodVisual) return;
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_DEEP t=%.2f float %.1f antenna %.1f tilt %.0f shot %.0f hook %.0f (water %.0f) tip above water %.0f, tip to float %.0f"), I * 0.05f,
                    RodVisual->BobberLocation().Z - LakeFish::SurfaceZ, RodVisual->FloatTip().Z - LakeFish::SurfaceZ, RodVisual->GetBiteTilt(),
                    RodVisual->ShotLocation().Z - LakeFish::SurfaceZ, RodVisual->HookLocation().Z - LakeFish::SurfaceZ,
                    LakeFish::SurfaceZ - ALakeEnvironment::GroundHeight(FloatPosition.X, FloatPosition.Y), RodVisual->Tip().Z - LakeFish::SurfaceZ,
                    FVector::Dist(RodVisual->Tip(), RodVisual->FloatKeel()));
            });
        At(68.6f, [this] { NextActionTime = 0; ServerFish_Implementation(); });
        At(69.5f, [this] { FloatDepth = MaxDepth; });
        At(74.5f, Side);
        At(75.0f, [this] { NextActionTime = 0; ServerFish_Implementation(); });
        At(75.0f + CastLoad + 0.1f, [this] { ServerRelease_Implementation(); });
        At(76.1f, [this, LookFrom]
        {
            LookFrom(FloatPosition + GetActorRightVector() * 220 - GetActorForwardVector() * 140 + FVector(0, 0, 90), FloatPosition, 45);
        });
        At(79.0f, [Shot] { Shot(TEXT("Cast_DeepLying.png")); });
        for (int32 I = 0; I <= 100; ++I)
            At(75.6f + I * 0.05f, [this, I]
            {
                if (!RodVisual) return;
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LYING t=%.2f float %.1f antenna %.1f tilt %.0f tip above water %.0f, tip to keel %.0f (line %.0f), float to its place %.0f, place from angler %.0f"), I * 0.05f,
                    RodVisual->BobberLocation().Z - LakeFish::SurfaceZ, RodVisual->FloatTip().Z - LakeFish::SurfaceZ, RodVisual->GetBiteTilt(), RodVisual->Tip().Z - LakeFish::SurfaceZ,
                    FVector::Dist(RodVisual->Tip(), RodVisual->FloatKeel()), ALakeRod::LineLength - HookDepth, FVector::Dist2D(RodVisual->FloatKeel(), FloatPosition), FVector::Dist2D(FloatPosition, GetActorLocation()));
            });
        At(81.0f, [] { FPlatformMisc::RequestExit(false); });
    }
    // -LinePreview: a fresh cross wind at the camp: the line blown out in a belly, the float
    // drifting downwind until the line holds it; then a rig with the bait on the bottom staying put.
    if (FParse::Param(FCommandLine::Get(), TEXT("LinePreview")) && GetNetMode() == NM_Standalone)
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
            FVector2D Facing;
            const FVector2D Spot = LakeMap::StartSpot(&Facing) + Facing * 60;
            const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X));
            if (ALakeWeather* Weather = ALakeWeather::Get(GetWorld()))
            {
                FLakeWeatherState Breeze;
                Breeze.Cloud = 0.3f; Breeze.Fog = 0; Breeze.Rain = 0; Breeze.Wind = 0.55f; Breeze.WindYaw = Yaw + 90;
                Weather->SetWeatherNow(Breeze);
                Weather->SetHours(9.0f);
            }
            SetActorLocation(FVector(Spot, ALakeEnvironment::GroundHeight(Spot.X, Spot.Y) + 95), false, nullptr, ETeleportType::TeleportPhysics);
            if (APlayerController* PC = Cast<APlayerController>(GetController()))
            {
                PC->SetControlRotation(FRotator(-12, Yaw, 0));
                if (PC->MyHUD) PC->MyHUD->bShowHUD = false;
            }
            FloatDepth = 20;
        });
        At(5.5f, [this, LookFrom]
        {
            LookFrom(GetActorLocation() + FVector(0, 0, 750) - GetActorForwardVector() * 250 - GetActorRightVector() * 100,
                     GetActorLocation() + GetActorForwardVector() * 500 + GetActorRightVector() * 150 + FVector(0, 0, -100), 70);
        });
        At(6.0f, [this] { NextActionTime = 0; ServerFish_Implementation(); });
        At(6.6f, [this] { ServerRelease_Implementation(); });
        At(10.0f, [Shot] { Shot(TEXT("Line_Belly.png")); });
        At(25.0f, [Shot] { Shot(TEXT("Line_Drift.png")); });
        At(31.0f, [this] { NextActionTime = 0; ServerFish_Implementation(); });
        At(34.0f, [this] { FloatDepth = MaxDepth; });
        At(36.0f, [this] { NextActionTime = 0; ServerFish_Implementation(); });
        At(36.6f, [this] { ServerRelease_Implementation(); });
        At(48.0f, [Shot] { Shot(TEXT("Line_Anchored.png")); });
        for (int32 I = 0; I <= 84; ++I)
            At(7.0f + I * 0.5f, [this, I]
            {
                if (!RodVisual) return;
                auto Rel = [this](FVector P)
                {
                    const FVector D = P - GetActorLocation();
                    return FString::Printf(TEXT("(%.0f, %.0f)"), FVector::DotProduct(D, GetActorForwardVector()), FVector::DotProduct(D, GetActorRightVector()));
                };
                int32 OnWater = 0;
                const float Belly = RodVisual->LineBelly(&OnWater);
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LINE t=%.1f state=%d float at %s shown %s, belly %.0f cm, %d of 41 line points on the water, wind %.1f m/s"), 7.0f + I * 0.5f, (int32)FishingState,
                    *Rel(FloatPosition), *Rel(RodVisual->BobberLocation()), Belly, OnWater, ALakeWeather::Get(GetWorld()) ? ALakeWeather::Get(GetWorld())->WindVelocity().Size() / 100 : 0.0f);
            });
        At(50.0f, [] { FPlatformMisc::RequestExit(false); });
    }
    // -GroundbaitPreview: at the camp on a calm morning the angler throws three balls of groundbait
    // 6 m out (the ball in flight, the rings); then for four minutes the log counts the fish within
    // 3 m of it and of as good a spot 15 m along the bank without any (QUIET_LAKE_GROUNDBAIT), and
    // the F3 panel is filmed with what the fish feed on.
    if (FParse::Param(FCommandLine::Get(), TEXT("GroundbaitPreview")) && GetNetMode() == NM_Standalone)
    {
        auto At = [this](float Seconds, TFunction<void()> Action)
        {
            FTimerHandle Handle;
            GetWorldTimerManager().SetTimer(Handle, [Action] { Action(); }, Seconds, false);
        };
        auto Shot = [](const TCHAR* Name, bool bUI) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / Name, bUI, false); };
        struct FState { FVector2D Spot = FVector2D::ZeroVector, Control = FVector2D::ZeroVector; };
        TSharedRef<FState> St = MakeShared<FState>();
        At(4.0f, [this, St]
        {
            FVector2D Facing;
            const FVector2D Stand = LakeMap::StartSpot(&Facing) + Facing * 60;
            const FVector2D Right(-Facing.Y, Facing.X);
            const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X));
            if (ALakeWeather* Weather = ALakeWeather::Get(GetWorld()))
            {
                FLakeWeatherState Calm;
                Calm.Cloud = 0.25f; Calm.Fog = 0; Calm.Rain = 0; Calm.Wind = 0.05f; Calm.WindYaw = Yaw;
                Weather->SetWeatherNow(Calm);
                Weather->SetHours(8.0f);
            }
            SetActorLocation(FVector(Stand, ALakeEnvironment::GroundHeight(Stand.X, Stand.Y) + 95), false, nullptr, ETeleportType::TeleportPhysics);
            if (APlayerController* PC = Cast<APlayerController>(GetController()))
            {
                PC->SetControlRotation(FRotator(-14, Yaw, 0));
                if (PC->MyHUD) PC->MyHUD->bShowHUD = false;
            }
            Bait = EBait::Bread;
            St->Spot = Stand + Facing * 600;
            // As good a spot without groundbait: as far out, 15 m along the bank.
            St->Control = St->Spot + Right * 1500;
            if (!LakeMap::IsWater(St->Control.X, St->Control.Y) || LakeMap::Depth(St->Control.X, St->Control.Y) < 60) St->Control = St->Spot - Right * 1500;
        });
        At(5.5f, [this, St]
        {
            const FVector Spot(St->Spot, LakeFish::SurfaceZ);
            const FVector From = GetActorLocation() - GetActorRightVector() * 450 - GetActorForwardVector() * 150 + FVector(0, 0, 180);
            auto* Cam = GetWorld()->SpawnActor<ACameraActor>(From, (FMath::Lerp(GetActorLocation(), Spot, 0.55f) - From).Rotation());
            Cam->GetCameraComponent()->SetFieldOfView(62);
            if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetViewTarget(Cam);
        });
        for (int32 Ball = 0; Ball < 3; ++Ball)
            At(6.0f + 1.5f * Ball, [this, St] { NextGroundbait = 0; ServerThrowGroundbait_Implementation(FVector(St->Spot, LakeFish::SurfaceZ)); });
        At(6.45f, [Shot] { Shot(TEXT("Groundbait_Throw.png"), false); });
        At(7.1f, [Shot] { Shot(TEXT("Groundbait_Plop.png"), false); });
        for (int32 I = 0; I <= 48; ++I)
            At(10.0f + I * 5.0f, [this, St, I]
            {
                const ALakeLife* Life = ALakeLife::Get(GetWorld());
                if (!Life) return;
                auto Count = [Life](const FVector2D& P, int32& OnIt, FString& Kinds)
                {
                    TMap<FString, int32> By;
                    int32 N = 0;
                    Life->ForEachFishNear(FVector(P, LakeFish::SurfaceZ), 300, [&](ALakeFish* Fish)
                    {
                        if (FVector2D::Distance(FVector2D(Fish->GetActorLocation()), P) > 300 || !Fish->IsFree()) return;
                        ++N;
                        ++By.FindOrAdd(LakeFish::Info(Fish->Species).Noun);
                        OnIt += Fish->GetFeedOn() == EFishFood::Groundbait;
                    });
                    for (const TPair<FString, int32>& K : By) Kinds += FString::Printf(TEXT(" %s %d"), *K.Key, K.Value);
                    return N;
                };
                int32 OnIt = 0, ControlOnIt = 0;
                FString Kinds, ControlKinds;
                const int32 Near = Count(St->Spot, OnIt, Kinds), Control = Count(St->Control, ControlOnIt, ControlKinds);
                float Balls = 0;
                for (const FGroundbaitSpot& G : Life->Food().Groundbait()) Balls += G.Balls;
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_GROUNDBAIT t=%.0f s after the first ball: within 3 m of it %d (%d feeding on it):%s | of the spot without it %d:%s | %.1f balls left"),
                    10.0f + I * 5.0f - 6.0f, Near, OnIt, *Kinds, Control, *ControlKinds, Balls);
            });
        At(130.0f, [this]
        {
            if (APlayerController* PC = Cast<APlayerController>(GetController()))
            {
                PC->SetViewTarget(this);
                if (ALakeHUD* HUD = Cast<ALakeHUD>(PC->GetHUD())) { HUD->bShowHUD = true; HUD->bShowLife = true; }
            }
        });
        At(133.0f, [Shot] { Shot(TEXT("Groundbait_Panel.png"), true); });
        At(252.0f, [] { FPlatformMisc::RequestExit(false); });
    }
    // -RodPreview: the blank at work, filmed from the side: a cast with each rod (light, medium,
    // heavy) with the tip's bend logged through the draw, the release and the swing after it, then
    // a perch struck and played (the rod bending under the line's pull).
    if (FParse::Param(FCommandLine::Get(), TEXT("RodPreview")) && GetNetMode() == NM_Standalone)
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
                Calm.Cloud = 0.3f; Calm.Fog = 0; Calm.Rain = 0; Calm.Wind = 0.0f; Calm.WindYaw = 40;
                Weather->SetWeatherNow(Calm);
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
            FloatDepth = 40;
        });
        At(5.5f, [this, LookFrom]
        {
            LookFrom(GetActorLocation() + GetActorRightVector() * 650 + GetActorForwardVector() * 220 + FVector(0, 0, 200),
                     GetActorLocation() + GetActorForwardVector() * 220 + FVector(0, 0, 280), 75);
        });
        struct FTry { float At; ERodKind Rod; const TCHAR* Name; };
        const FTry Tries[] = { { 6.0f, ERodKind::Light, TEXT("light") }, { 14.0f, ERodKind::Medium, TEXT("medium") }, { 22.0f, ERodKind::Heavy, TEXT("heavy") } };
        for (const FTry& Try : Tries)
        {
            At(Try.At - 1.0f, [this, Try] { UseRod(Try.Rod); });
            At(Try.At, [this] { NextActionTime = 0; ServerFish_Implementation(); });
            At(Try.At + 0.8f, [this] { ServerRelease_Implementation(); });
            At(Try.At + 5.0f, [this] { NextActionTime = 0; ServerFish_Implementation(); });
            for (int32 I = 0; I <= 150; ++I)
                At(Try.At - 0.5f + I * 0.02f, [this, Try, I]
                {
                    if (RodVisual) UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_ROD %s t=%.2f bend %.1f cm tip above water %.0f"), Try.Name, -0.5f + I * 0.02f, RodVisual->TipBend(), RodVisual->Tip().Z - LakeFish::SurfaceZ);
                });
        }
        At(14.75f, [Shot] { Shot(TEXT("Rod_Draw.png")); });
        At(15.05f, [Shot] { Shot(TEXT("Rod_Whip.png")); });
        // A perch struck and played on the medium rod.
        At(29.0f, [this] { UseRod(ERodKind::Medium); });
        At(30.0f, [this] { NextActionTime = 0; ServerFish_Implementation(); });
        At(30.8f, [this] { ServerRelease_Implementation(); });
        At(33.0f, [this]
        {
            const FVector Bait = BaitLocation();
            ALakeFish* Fish = GetWorld()->SpawnActor<ALakeFish>(Bait, FRotator(0, GetActorRotation().Yaw + 90, 0));
            Fish->Setup(EFishSpecies::Perch, 0.6f, Bait, -1);
            Fish->SetActorLocation(Bait - Fish->GetActorQuat().RotateVector(Fish->MouthOffset()));
            Fish->Hook(this); // preview fixture, not the gameplay strike path
            HookedFish = Fish; LineFish = Fish; FishingState = EFishingState::Fight;
            Tension = 0; FightTime = 0; bReeling = false;
            FTimerHandle Draw;
            GetWorldTimerManager().SetTimer(Draw, [this] { bReeling = true; }, 2.0f, false);
        });
        At(35.0f, [Shot] { Shot(TEXT("Rod_Fight.png")); });
        At(37.0f, [Shot] { Shot(TEXT("Rod_Fight2.png")); });
        for (int32 I = 0; I <= 70; ++I)
            At(33.0f + I * 0.1f, [this, I]
            {
                if (RodVisual) UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_RODFIGHT t=%.1f state=%d tension %.2f bend %.1f cm"), I * 0.1f, (int32)FishingState, Tension, RodVisual->TipBend());
            });
        At(41.0f, [] { FPlatformMisc::RequestExit(false); });
    }
    // -HandsPreview: the angler's hands close up from the front-left: the rig in the hand, drawn
    // back, the push, waiting with both hands on the rod, and catching the rig after a retrieve.
    if (FParse::Param(FCommandLine::Get(), TEXT("HandsPreview")) && GetNetMode() == NM_Standalone)
    {
        auto At = [this](float Seconds, TFunction<void()> Action)
        {
            FTimerHandle Handle;
            GetWorldTimerManager().SetTimer(Handle, [Action] { Action(); }, Seconds, false);
        };
        auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / Name, false, false); };
        auto Close = [this]
        {
            auto* Cam = GetWorld()->SpawnActor<ACameraActor>(GetActorLocation() + GetActorForwardVector() * 110 - GetActorRightVector() * 55 + FVector(0, 0, 35),
                                                             FRotator::ZeroRotator);
            Cam->SetActorRotation((GetActorLocation() + GetActorForwardVector() * 32 + FVector(0, 0, 22) - Cam->GetActorLocation()).Rotation());
            Cam->GetCameraComponent()->SetFieldOfView(55);
            if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetViewTarget(Cam);
        };
        At(4.0f, [this]
        {
            if (ALakeWeather* Weather = ALakeWeather::Get(GetWorld()))
            {
                FLakeWeatherState Calm;
                Calm.Cloud = 0.3f; Calm.Fog = 0; Calm.Rain = 0; Calm.Wind = 0.0f; Calm.WindYaw = 40;
                Weather->SetWeatherNow(Calm);
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
        At(5.0f, Close);
        At(5.5f, [Shot] { Shot(TEXT("Hands_Holding.png")); });
        At(6.0f, [this] { NextActionTime = 0; ServerFish_Implementation(); });
        At(6.55f, [Shot] { Shot(TEXT("Hands_Drawn.png")); });
        At(6.6f, [this] { ServerRelease_Implementation(); });
        At(6.85f, [Shot] { Shot(TEXT("Hands_Push.png")); });
        At(11.0f, [Shot] { Shot(TEXT("Hands_Waiting.png")); });
        At(11.5f, [this] { NextActionTime = 0; ServerFish_Implementation(); });
        At(13.0f, [Shot] { Shot(TEXT("Hands_Lifting.png")); });
        for (int32 I = 0; I <= 120; ++I)
            At(11.5f + I * 0.1f, [this, I]
            {
                if (!RodVisual) return;
                const FVector D = RodVisual->ShotLocation() - GetActorLocation();
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_HANDS t=%.1f in hand %d, shot (%.0f, %.0f, %.0f)"), I * 0.1f, bRigInHand ? 1 : 0,
                    FVector::DotProduct(D, GetActorForwardVector()), FVector::DotProduct(D, GetActorRightVector()), D.Z);
            });
        At(15.0f, [Shot] { Shot(TEXT("Hands_Caught.png")); });
        At(20.0f, [Shot] { Shot(TEXT("Hands_Settled.png")); });
        At(24.0f, [] { FPlatformMisc::RequestExit(false); });
    }
    // -LeaderPreview: the line under the float close up, the camera following the rig: held in the
    // hand, the swing, in the air, landing, sinking, lying, lifted out and caught. The leader's
    // bow and slack are logged (QUIET_LAKE_LEADER).
    if (FParse::Param(FCommandLine::Get(), TEXT("LeaderPreview")) && GetNetMode() == NM_Standalone)
    {
        auto At = [this](float Seconds, TFunction<void()> Action)
        {
            FTimerHandle Handle;
            GetWorldTimerManager().SetTimer(Handle, [Action] { Action(); }, Seconds, false);
        };
        auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / Name, false, false); };
        float Depth = 60;
        FParse::Value(FCommandLine::Get(), TEXT("Depth="), Depth);
        At(4.0f, [this, Depth]
        {
            if (ALakeWeather* Weather = ALakeWeather::Get(GetWorld()))
            {
                FLakeWeatherState Calm;
                Calm.Cloud = 0.3f; Calm.Fog = 0; Calm.Rain = 0; Calm.Wind = 0.03f; Calm.WindYaw = 40;
                Weather->SetWeatherNow(Calm);
                Weather->SetHours(10.0f);
            }
            FVector2D Facing;
            const FVector2D Spot = LakeMap::StartSpot(&Facing) + Facing * 60;
            SetActorLocation(FVector(Spot, ALakeEnvironment::GroundHeight(Spot.X, Spot.Y) + 95), false, nullptr, ETeleportType::TeleportPhysics);
            if (APlayerController* PC = Cast<APlayerController>(GetController()))
            {
                PC->SetControlRotation(FRotator(-12, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)), 0));
                if (PC->MyHUD) PC->MyHUD->bShowHUD = false;
            }
            FloatDepth = Depth;
        });
        // -FirstPerson: stay in the angler's own camera (as in the game) and film the lift out in a series.
        const bool bFirstPerson = FParse::Param(FCommandLine::Get(), TEXT("FirstPerson"));
        if (bFirstPerson)
            for (int32 I = 0; I < 6; ++I)
                At(12.05f + I * 0.3f, [Shot, I] { Shot(*FString::Printf(TEXT("Leader_FP_%d.png"), I)); });
        // A camera beside the rig, 1.6 m off to the side of the angler's facing, following it smoothly.
        if (!bFirstPerson) At(5.0f, [this]
        {
            auto* Cam = GetWorld()->SpawnActor<ACameraActor>(GetActorLocation(), FRotator::ZeroRotator);
            Cam->GetCameraComponent()->SetFieldOfView(50);
            if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetViewTarget(Cam);
            TWeakObjectPtr<ACameraActor> Weak = Cam;
            FTimerHandle Handle;
            GetWorldTimerManager().SetTimer(Handle, [this, Weak]
            {
                if (!Weak.IsValid() || !RodVisual) return;
                const FVector Mid = (RodVisual->FloatKeel() + RodVisual->HookLocation()) * 0.5f;
                FVector Want = Mid - GetActorForwardVector() * 110 + GetActorRightVector() * 90 + FVector(0, 0, 35);
                Want.Z = FMath::Max(Want.Z, FMath::Max(LakeFish::SurfaceZ, ALakeEnvironment::GroundHeight(Want.X, Want.Y)) + 25);
                const FVector From = FMath::Lerp(Weak->GetActorLocation(), Want, 0.5f);
                Weak->SetActorLocationAndRotation(From, (Mid - From).Rotation());
            }, 0.01f, true);
        });
        At(5.8f, [Shot] { Shot(TEXT("Leader_Hold.png")); });
        // Holding performs the stroke; releasing lowers the rod.
        At(6.0f, [this] { NextActionTime = 0; ServerFish_Implementation(); });
        At(7.2f, [this] { ServerRelease_Implementation(); });
        At(6.9f, [Shot] { Shot(TEXT("Leader_Swing.png")); });
        At(7.4f, [Shot] { Shot(TEXT("Leader_Air1.png")); });
        At(7.65f, [Shot] { Shot(TEXT("Leader_Air2.png")); });
        At(7.9f, [Shot] { Shot(TEXT("Leader_Air3.png")); });
        At(8.4f, [Shot] { Shot(TEXT("Leader_Land.png")); });
        At(8.9f, [Shot] { Shot(TEXT("Leader_Sink.png")); });
        At(9.0f, [Shot] { Shot(TEXT("Leader_Lying.png")); });
        // LMB held 1 s, then let go: the rod stops where it is; held again, it goes on up to the hand.
        At(9.5f, [this] { NextActionTime = 0; ServerFish_Implementation(); });
        At(9.9f, [Shot] { Shot(TEXT("Leader_Pull.png")); });
        At(10.5f, [this] { ServerRelease_Implementation(); });
        At(11.8f, [this] { UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_PULL after: %s"), *Status); });
        At(12.0f, [this] { NextActionTime = 0; ServerFish_Implementation(); });
        At(12.6f, [Shot] { Shot(TEXT("Leader_Lift1.png")); });
        At(13.1f, [Shot] { Shot(TEXT("Leader_Lift2.png")); });
        At(13.6f, [Shot] { Shot(TEXT("Leader_Lift3.png")); });
        At(18.0f, [Shot] { Shot(TEXT("Leader_Caught.png")); });
        for (int32 I = 0; I <= 440; ++I)
            At(5.5f + I * 0.05f, [this, I]
            {
                if (!RodVisual) return;
                float Bow, Chord, Length;
                RodVisual->LeaderShape(Bow, Chord, Length);
                const FVector K = RodVisual->FloatKeel(), S = RodVisual->ShotLocation(), H = RodVisual->HookLocation();
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LEADER t=%.2f state=%d fly=%d hand=%d retr=%d prog=%.2f bait=%d out=%.0f bow=%.1f chord=%.1f len=%.1f float=(%.0f,%.0f,%.0f) shot=(%.0f,%.0f,%.0f) hook=(%.0f,%.0f,%.0f)"),
                    I * 0.05f, (int32)FishingState, bRigFlying, bRigInHand, bRetrieving, RetrieveLift, HasBaitInWater(), FVector::Dist2D(FloatPosition, GetActorLocation()),
                    Bow, Chord, Length, K.X, K.Y, K.Z, S.X, S.Y, S.Z, H.X, H.Y, H.Z);
            });
        At(24.0f, [Shot] { Shot(TEXT("Leader_Final.png")); });
        At(28.0f, [] { FPlatformMisc::RequestExit(false); });
    }
    // -RetrievePreview: the rig lifted out after a cast, LMB held: the rod rises upright, the rig leaves
    // the water and swings in, the left hand reaches out and takes it by the shot. Filmed from the
    // side (a series), close at the hands, and from the angler's eyes with a stop half way (LMB let go,
    // then held again). Logged as QUIET_LAKE_RETRIEVE (-Depth= sets the float).
    if (FParse::Param(FCommandLine::Get(), TEXT("RetrievePreview")) && GetNetMode() == NM_Standalone)
    {
        auto At = [this](float Seconds, TFunction<void()> Action)
        {
            FTimerHandle Handle;
            GetWorldTimerManager().SetTimer(Handle, [Action] { Action(); }, Seconds, false);
        };
        auto Shot = [](const FString& Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / Name, false, false); };
        auto LookFrom = [this](FVector From, FVector Target, float Fov)
        {
            auto* Cam = GetWorld()->SpawnActor<ACameraActor>(From, (Target - From).Rotation());
            Cam->GetCameraComponent()->SetFieldOfView(Fov);
            if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetViewTarget(Cam);
        };
        float Depth = 60;
        FParse::Value(FCommandLine::Get(), TEXT("Depth="), Depth);
        At(4.0f, [this, Depth]
        {
            if (ALakeWeather* Weather = ALakeWeather::Get(GetWorld()))
            {
                FLakeWeatherState Calm;
                Calm.Cloud = 0.3f; Calm.Fog = 0; Calm.Rain = 0; Calm.Wind = 0.03f; Calm.WindYaw = 40;
                Weather->SetWeatherNow(Calm);
                Weather->SetHours(10.0f);
            }
            FVector2D Facing;
            const FVector2D Spot = LakeMap::StartSpot(&Facing) + Facing * 60;
            SetActorLocation(FVector(Spot, ALakeEnvironment::GroundHeight(Spot.X, Spot.Y) + 95), false, nullptr, ETeleportType::TeleportPhysics);
            if (APlayerController* PC = Cast<APlayerController>(GetController()))
            {
                PC->SetControlRotation(FRotator(-12, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)), 0));
                if (PC->MyHUD) PC->MyHUD->bShowHUD = false;
            }
            UseRod(ERodKind::Medium);
            FloatDepth = HookDepth = Depth;
            const FTransform& T = GetActorTransform();
            const FVector Arm = T.InverseTransformPosition(GetMesh()->GetBoneLocation(TEXT("upperarm_l")));
            const FVector Hand = T.InverseTransformPosition(GetMesh()->GetBoneLocation(TEXT("hand_l")));
            const FVector Used = T.InverseTransformPosition(LeftShoulder());
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_RETRIEVE bones: upperarm_l (%.0f, %.0f, %.0f) hand_l (%.0f, %.0f, %.0f), shoulder used (%.0f, %.0f, %.0f)"),
                Arm.X, Arm.Y, Arm.Z, Hand.X, Hand.Y, Hand.Z, Used.X, Used.Y, Used.Z);
        });
        // The cast as the mouse makes it (as in -CastPreview): the rod raised, LMB let go, lowered.
        auto CastAt = [this, At](float T)
        {
            At(T - 0.5f, [this] { if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetControlRotation(FRotator(-12, GetActorRotation().Yaw, 0)); });
            for (int32 I = 0; I <= 100; ++I)
                At(T + I * 0.025f, [this, I]
                {
                    const float U = I * 0.025f;
                    const float Target = U < 0.3f ? 0.0f : U < 0.65f ? 35 * FMath::SmoothStep(0.3f, 0.65f, U)
                               : U < 1.0f ? 35.0f : FMath::Lerp(35.0f, -30.0f, FMath::SmoothStep(1.0f, 1.6f, U));
                    MoveRodAxis(Target - SwingPitch - FMath::FindDeltaAngleDegrees(-12.0f, GetControlRotation().Pitch), true);
                });
            At(T, [this] { NextActionTime = 0; ServerFish_Implementation(); });
            At(T + 0.4f, [this] { ServerRelease_Implementation(); });
            // The rod back to the waiting angle before the lift.
            At(T + 3.0f, [this] { MoveRodAxis(-SwingPitch, true); });
            At(T + 3.5f, [this] { if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetControlRotation(FRotator(-8, GetActorRotation().Yaw, 0)); });
        };
        // A longer cast (as in -LeaderPreview): LMB held 1.2 s with the rod still, the rig swung out ~3.5 m.
        auto LongCastAt = [this, At](float T)
        {
            At(T - 0.5f, [this] { if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetControlRotation(FRotator(-12, GetActorRotation().Yaw, 0)); });
            At(T, [this] { NextActionTime = 0; ServerFish_Implementation(); });
            At(T + 1.2f, [this] { ServerRelease_Implementation(); });
        };
        const bool bLong = FParse::Param(FCommandLine::Get(), TEXT("LongCast"));
        auto LiftAt = [this, At](float T) { At(T, [this] { NextActionTime = 0; ServerFish_Implementation(); }); };
        // 1. From the side, the whole lift in a series.
        At(5.5f, [this, LookFrom]
        {
            LookFrom(GetActorLocation() + GetActorRightVector() * 900 + GetActorForwardVector() * 150 + FVector(0, 0, 210),
                     GetActorLocation() + GetActorForwardVector() * 150 + FVector(0, 0, 210), 80);
        });
        if (bLong) LongCastAt(6.0f); else CastAt(6.0f);
        At(10.8f, [Shot] { Shot(TEXT("Retrieve_Side_Wait.png")); });
        LiftAt(11.0f);
        for (int32 I = 0; I < 12; ++I) At(11.3f + I * 0.4f, [Shot, I] { Shot(FString::Printf(TEXT("Retrieve_Side_%02d.png"), I)); });
        // 2. Close at the hands: the left hand going out to the shot and taking it.
        if (bLong) LongCastAt(19.0f); else CastAt(19.0f);
        At(23.5f, [this, LookFrom]
        {
            // Over the left shoulder, from behind (the reeds are in front of the bank).
            LookFrom(GetActorLocation() - GetActorForwardVector() * 110 - GetActorRightVector() * 120 + FVector(0, 0, 115),
                     GetActorLocation() + GetActorForwardVector() * 60 - GetActorRightVector() * 20 + FVector(0, 0, 35), 60);
        });
        LiftAt(24.0f);
        At(24.0f, [this, Shot]
        {
            // A series once the hand starts out for the rig.
            TSharedRef<int32> Taken = MakeShared<int32>(-1);
            FTimerHandle Handle;
            GetWorldTimerManager().SetTimer(Handle, [this, Shot, Taken]
            {
                if (*Taken < 0 && RigReach() > 0.05f) *Taken = 0;
                if (*Taken >= 0 && *Taken < 40)
                {
                    if (*Taken % 5 == 0) Shot(FString::Printf(TEXT("Retrieve_Hands_%d.png"), *Taken / 5));
                    ++*Taken;
                }
            }, 0.05f, true);
        });
        // 3. From the angler's eyes, LMB let go half way (the rod stops) and held again.
        At(30.5f, [this] { if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetViewTarget(this); });
        if (bLong) LongCastAt(31.0f); else CastAt(31.0f);
        LiftAt(36.0f);
        At(37.0f, [this] { ServerRelease_Implementation(); });
        At(37.9f, [Shot] { Shot(TEXT("Retrieve_FP_Stopped.png")); });
        LiftAt(38.5f);
        for (int32 I = 0; I < 8; ++I) At(39.0f + I * 0.4f, [Shot, I] { Shot(FString::Printf(TEXT("Retrieve_FP_%d.png"), I)); });
        // The mouse during the lift: the camera turns 12 degrees right and looks 8 degrees up with the rod.
        for (int32 I = 0; I < 20; ++I) At(39.2f + I * 0.05f, [this] { MoveRodAxis(0.6f, false); MoveRodAxis(0.4f, true); });
        for (int32 I = 0; I <= 760; ++I)
            At(10.5f + I * 0.05f, [this, I]
            {
                if (!RodVisual) return;
                const FTransform& T = GetActorTransform();
                const FVector Tip = T.InverseTransformPosition(RodVisual->Tip()), S = T.InverseTransformPosition(RodVisual->ShotLocation());
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_RETRIEVE t=%.2f state=%d retr=%d held=%d prog=%.2f hand=%d elev=%.0f yaw=%+.0f tip=(%.0f,%.0f,%.0f) shot=(%.0f,%.0f,%.0f) speed=%.0f shoulder=%.0f reach=%.2f float=%.0f view=(%.1f,%.1f) steer=%.1f"),
                    10.5f + I * 0.05f, (int32)FishingState, bRetrieving, bRetrieveHeld, RetrieveLift, bRigInHand, RodElevation,
                    FMath::FindDeltaAngleDegrees(GetActorRotation().Yaw, RodYaw), Tip.X, Tip.Y, Tip.Z, S.X, S.Y, S.Z,
                    (RodVisual->ShotVelocity() - GetVelocity()).Size(), FVector::Dist(RodVisual->ShotLocation(), LeftShoulder()), RigReach(),
                    FVector::Dist2D(RodVisual->FloatKeel(), GetActorLocation()), FRotator::NormalizeAxis(GetControlRotation().Pitch),
                    FRotator::NormalizeAxis(GetControlRotation().Yaw), RigSteerPitch);
            });
        At(44.0f, [] { FPlatformMisc::RequestExit(false); });
    }
}
#endif
