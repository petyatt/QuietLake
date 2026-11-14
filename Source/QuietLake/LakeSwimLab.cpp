#include "LakeSwimLab.h"
#include "LakeFish.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/PointLight.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Engine.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "DrawDebugHelpers.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

ALakeSwimLabMode::ALakeSwimLabMode() { DefaultPawnClass=nullptr; }
void ALakeSwimLabMode::BeginPlay()
{
    Super::BeginPlay();
    GetWorld()->SpawnActor<ALakeSwimLab>();
}
ALakeSwimLab::ALakeSwimLab() { PrimaryActorTick.bCanEverTick=true; }
void ALakeSwimLab::BeginPlay()
{
    Super::BeginPlay();
    bAuto=FParse::Param(FCommandLine::Get(),TEXT("SwimLabAuto"));
    Fish=GetWorld()->SpawnActor<ALakeFish>();
    Fish->Setup(EFishSpecies::Perch,BodyGrams/1000,FVector::ZeroVector,-1,42);
    Fish->SetActorTickEnabled(false);
    Body=Fish->FindComponentByClass<UStaticMeshComponent>();
    BodyLength=LakeFish::LengthCm(EFishSpecies::Perch,BodyGrams/1000);
    Swimmer.Reset(FVector::ZeroVector,BodyLength,BodyGrams);
    Camera=GetWorld()->SpawnActor<ACameraActor>();
    auto* Lens=Camera->GetCameraComponent();
    Lens->SetFieldOfView(50);
    Lens->PostProcessSettings.bOverride_AutoExposureMinBrightness=true;
    Lens->PostProcessSettings.bOverride_AutoExposureMaxBrightness=true;
    Lens->PostProcessSettings.AutoExposureMinBrightness=1;
    Lens->PostProcessSettings.AutoExposureMaxBrightness=1;
    Floor=GetWorld()->SpawnActor<AStaticMeshActor>(FVector(0,0,-24),FRotator::ZeroRotator);
    Floor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
    Floor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Plane.Plane")));
    Floor->SetActorScale3D(FVector(40,40,1));
    Floor->SetActorEnableCollision(false);
    if(auto* Material=LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
    {
        auto* Surface=UMaterialInstanceDynamic::Create(Material,this);
        Surface->SetVectorParameterValue(TEXT("Color"),FLinearColor(0.06f,0.12f,0.16f));
        Floor->GetStaticMeshComponent()->SetMaterial(0,Surface);
    }
    for(FVector Position : {FVector(0,-100,180),FVector(200,150,100),FVector(-200,0,100)})
    {
        auto* Lamp=GetWorld()->SpawnActor<APointLight>(Position,FRotator::ZeroRotator);
        // The lamps travel with the fish (Tick), or a burst would take it out of the light.
        Lamp->GetRootComponent()->SetMobility(EComponentMobility::Movable);
        Lamps.Add(Lamp);
        LampOffsets.Add(Position);
        auto* Light=Cast<UPointLightComponent>(Lamp->GetLightComponent());
        Light->SetIntensity(5000);
        Light->SetAttenuationRadius(2500);
        Light->SetSourceRadius(60);
    }
    if(auto* PC=GetWorld()->GetFirstPlayerController())
    {
        PC->SetViewTarget(Camera);
        PC->SetInputMode(FInputModeGameOnly());
        PC->bShowMouseCursor=false;
    }
}
void ALakeSwimLab::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if(!Fish || !Body || !Camera) return;
    Elapsed+=DeltaSeconds;
    auto* PC=GetWorld()->GetFirstPlayerController();
    float Effort=0,Steering=0;
    if(bAuto)
    {
        // An easy swim with a turn, a glide, a burst with a turn the other way, an easy swim again.
        Effort=Elapsed<8 ? Easy : Elapsed<12 ? 0 : Elapsed<14 ? 1 : Easy;
        Steering=Elapsed>5 && Elapsed<7 ? 0.6f : Elapsed>12.6f && Elapsed<13.4f ? -0.6f : 0;
    }
    else if(PC)
    {
        const bool bBurst=PC->IsInputKeyDown(EKeys::LeftShift);
        Effort=PC->IsInputKeyDown(EKeys::W) ? (bBurst ? 1.0f : Easy) : 0;
        Steering=(PC->IsInputKeyDown(EKeys::A) ? 1.0f : 0)-(PC->IsInputKeyDown(EKeys::D) ? 1.0f : 0);
        if(PC->WasInputKeyJustPressed(EKeys::R)) Swimmer.Reset(FVector::ZeroVector,BodyLength,BodyGrams);
    }
    Swimmer.Advance(DeltaSeconds,Effort,Steering);
    const FQuat Pose=FRotationMatrix::MakeFromXZ(Swimmer.Forward(),FVector::UpVector).ToQuat();
    Fish->SetActorLocationAndRotation(Swimmer.P[0]-Pose.RotateVector(Fish->MouthOffset()),Pose);
    // Deform the existing perch material from simulated joints; no cosmetic swimming wave.
    Body->SetCustomPrimitiveDataFloat(0,0);
    Body->SetCustomPrimitiveDataFloat(1,0);
    for(int I=1;I<5;++I)
    {
        const FVector Direction=Pose.UnrotateVector((Swimmer.P[I+1]-Swimmer.P[I]).GetSafeNormal());
        Body->SetCustomPrimitiveDataFloat(3+2*I,FMath::Atan2(float(-Direction.Y),float(-Direction.X)));
        Body->SetCustomPrimitiveDataFloat(4+2*I,0);
    }
    // Fins: the pectorals paddle at rest and in a turn, the dorsal lies flat at speed.
    const float Pace=Swimmer.Velocity().Size()/BodyLength;
    Body->SetCustomPrimitiveDataFloat(13,FMath::Clamp(0.8f-0.5f*Pace+0.4f*FMath::Abs(Swimmer.Bend),0.1f,1.0f));
    Body->SetCustomPrimitiveDataFloat(14,FMath::Lerp(0.6f,-0.8f,FMath::SmoothStep(1.5f,4.0f,Pace)));
    Body->SetCustomPrimitiveDataFloat(15,6);
    const FVector Centre=Swimmer.Centre();
    const FVector View=Centre+FVector(10,-70,60);
    Camera->SetActorLocationAndRotation(View,(Centre-View).Rotation());
    if(Floor) Floor->SetActorLocation(FVector(Centre.X,Centre.Y,-24));
    for(int I=0;I<Lamps.Num();++I) if(Lamps[I]) Lamps[I]->SetActorLocation(Centre+LampOffsets[I]);
    // The floor's grid, drawn around the fish wherever it has swum to.
    const FVector Cell(FMath::GridSnap(Centre.X,50.0),FMath::GridSnap(Centre.Y,50.0),0);
    for(int I=-20;I<=20;++I)
    {
        DrawDebugLine(GetWorld(),Cell+FVector(-1000,I*50,-23.9),Cell+FVector(1000,I*50,-23.9),FColor(70,110,125),false,0,0,0.3f);
        DrawDebugLine(GetWorld(),Cell+FVector(I*50,-1000,-23.9),Cell+FVector(I*50,1000,-23.9),FColor(70,110,125),false,0,0,0.3f);
    }
    const float Speed=Swimmer.Velocity().Size();
    if(GEngine) GEngine->AddOnScreenDebugMessage(901,0,FColor::White,FString::Printf(
        TEXT("Окунь %.0f см, %.0f г: физическое плавание | W — плыть, Shift+W — бросок, отпустить — скользить | A/D — поворот | R — сначала\nСкорость %.0f см/с (%.1f длины тела в секунду) | хвост %.1f взмаха/с | мышцы на пределе %.0f%%"),
        BodyLength,BodyGrams,Speed,Speed/BodyLength,Swimmer.Drive>0.02f ? Swimmer.Beat : 0.0f,Swimmer.MuscleUse*100));
    if(bAuto)
    {
        const float Shots[]={4,6.5f,11,13.2f,18};
        if(NextShot<5 && Elapsed>=Shots[NextShot])
        {
            FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir()/FString::Printf(TEXT("SwimLab_%d.png"),NextShot),false,false);
            UE_LOG(LogTemp,Display,TEXT("SWIM_LAB t %.1f centre %s speed %.1f cm/s = %.2f lengths/s beat %.1f/s muscles at limit %.0f%% forward %s"),Elapsed,*Centre.ToCompactString(),Speed,Speed/BodyLength,Swimmer.Beat,Swimmer.MuscleUse*100,*Swimmer.Forward().ToCompactString());
            ++NextShot;
        }
        if(Elapsed>20) FPlatformMisc::RequestExit(false);
    }
}
