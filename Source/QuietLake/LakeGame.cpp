#include "LakeGame.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Engine/Canvas.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "DrawDebugHelpers.h"
#include "Materials/MaterialInterface.h"
#include "Engine/SkyLight.h"
#include "Components/SkyLightComponent.h"

ALakeCharacter::ALakeCharacter()
{
    bReplicates = true;
    PrimaryActorTick.bCanEverTick = true;
    GetCharacterMovement()->MaxWalkSpeed = 280;
    auto* Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
    Camera->SetupAttachment(GetCapsuleComponent());
    Camera->SetRelativeLocation(FVector(0, 0, 64));
    Camera->bUsePawnControlRotation = true;
    FishingCamera = Camera;
    auto MakePart = [this](const TCHAR* Name, const TCHAR* MeshPath)
    {
        auto* Part = CreateDefaultSubobject<UStaticMeshComponent>(Name);
        Part->SetupAttachment(GetCapsuleComponent());
        Part->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, MeshPath));
        Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Part->SetCastShadow(false);
        return Part;
    };
    Rod = MakePart(TEXT("Rod"), TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
    Bobber = MakePart(TEXT("Bobber"), TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    Line = MakePart(TEXT("Line"), TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
    Rod->SetMaterial(0, LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Wood.M_Wood")));
    auto* Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
    Body->SetupAttachment(GetCapsuleComponent());
    Body->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder")));
    Body->SetRelativeScale3D(FVector(0.5, 0.5, 1.6));
    Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Body->SetOwnerNoSee(true);
}
void ALakeCharacter::SetupPlayerInputComponent(UInputComponent* Input)
{
    Super::SetupPlayerInputComponent(Input);
    Input->BindAxis("Forward", this, &ALakeCharacter::Forward);
    Input->BindAxis("Right", this, &ALakeCharacter::Right);
    Input->BindAxis("Turn", this, &APawn::AddControllerYawInput);
    Input->BindAxis("Look", this, &APawn::AddControllerPitchInput);
    Input->BindAction("Fish", IE_Pressed, this, &ALakeCharacter::Fish);
    Input->BindAction("Fish", IE_Released, this, &ALakeCharacter::Release);
    Input->BindAction("Cancel", IE_Pressed, this, &ALakeCharacter::Cancel);
    Input->BindAction("Jump", IE_Pressed, this, &ACharacter::Jump);
    Input->BindAction("Jump", IE_Released, this, &ACharacter::StopJumping);
}
void ALakeCharacter::Forward(float Value) { if (!FishingState) AddMovementInput(GetActorForwardVector(), Value); }
void ALakeCharacter::Right(float Value) { if (!FishingState) AddMovementInput(GetActorRightVector(), Value); }
void ALakeCharacter::Fish() { ServerFish(); }
void ALakeCharacter::Cancel() { ServerCancel(); }
void ALakeCharacter::Release() { ServerRelease(); }
void ALakeCharacter::ServerRelease_Implementation() { bReeling = false; }
void ALakeCharacter::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (HasAuthority())
    {
        const FVector Position = GetActorLocation();
        const bool OnPier = Position.X >= -450 && Position.X <= 610 && Position.Y >= -830 && Position.Y <= -570;
        if (ALakeEnvironment::IsWater(Position) && !OnPier)
        {
            // Swimming is not implemented: keep the player on the nearest bank.
            FVector2D Direction((Position.X - 2200) / 2200, Position.Y / 2800);
            Direction = Direction.GetSafeNormal();
            const float X = 2200 + Direction.X * 2200 * 1.04f;
            const float Y = Direction.Y * 2800 * 1.04f;
            SetActorLocation(FVector(X,Y,ALakeEnvironment::GroundHeight(X,Y) + GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 5));
            GetCharacterMovement()->StopMovementImmediately();
            FinishFishing(TEXT("Глубокая вода. Рыбачьте с берега или пирса."));
        }
        else if (FMath::Abs(Position.Y) > 11000 || Position.X < -9000 || Position.X > 13000)
        {
            SetActorLocation(FVector(-550,0,130));
            GetCharacterMovement()->StopMovementImmediately();
            FinishFishing(TEXT("Вы вернулись к месту рыбалки."));
        }
    }
    if (HasAuthority() && FishingState == 3) UpdateFight(DeltaSeconds);
    if (GetNetMode() == NM_DedicatedServer) return;
    const FRotator View = IsLocallyControlled() ? FishingCamera->GetComponentRotation() : GetBaseAimRotation();
    const FVector Start = IsLocallyControlled() ? FishingCamera->GetComponentLocation() : GetActorLocation() + FVector(0,0,64);
    const FVector Grip = Start + View.RotateVector(FVector(35, 22, -24));
    const FVector Tip = Grip + View.RotateVector(FVector(240, -12, 65 - Tension * 35));
    auto Segment = [](UStaticMeshComponent* Part, FVector A, FVector B, float Width)
    {
        Part->SetWorldLocation((A + B) * 0.5);
        Part->SetWorldRotation(FRotationMatrix::MakeFromZ(B - A).Rotator());
        Part->SetWorldScale3D(FVector(Width / 100, Width / 100, FVector::Distance(A, B) / 100));
    };
    Segment(Rod, Grip, Tip, 1.7f);
    Bobber->SetVisibility(FishingState != 0);
    Line->SetVisibility(FishingState != 0);
    if (FishingState)
    {
        FVector Position = FloatPosition;
        Position.Z = -18 + FMath::Sin(GetWorld()->GetTimeSeconds() * (FishingState == 2 ? 14 : 2)) * (FishingState == 2 ? 9 : 2);
        Bobber->SetWorldLocation(Position);
        Bobber->SetWorldScale3D(FVector(0.09, 0.09, 0.22));
        Segment(Line, Tip, Position, 0.3f);
    }
}
void ALakeCharacter::FinishFishing(const FString& Message)
{
    GetWorldTimerManager().ClearTimer(FishingTimer);
    FishingState = 0;
    Tension = 0;
    bReeling = false;
    Status = Message;
    ForceNetUpdate();
}
void ALakeCharacter::UpdateFight(float DeltaSeconds)
{
    if (FishingState != 3) return;
    FightTime += DeltaSeconds;
    const float Resistance = 0.5f + 0.5f * FMath::Sin(FightTime * 1.7f);
    Tension = FMath::Clamp(Tension + DeltaSeconds * (bReeling ? 0.20f + Resistance * 0.22f : -0.50f), 0.0f, 1.0f);
    FishDistance = FMath::Clamp(FishDistance + DeltaSeconds * (bReeling ? -90.0f + Resistance * 35 : 24.0f), 130.0f, 1200.0f);
    FloatPosition = CastOrigin + CastDirection * FishDistance;
    FloatPosition.X = FMath::Max(40.0, FloatPosition.X);
    FloatPosition.Z = -18;
    if (Tension >= 1) FinishFishing(TEXT("Леска оборвалась! Чаще отпускайте ЛКМ. ЛКМ — новый заброс."));
    else if (FightTime >= 60) FinishFishing(TEXT("Рыба сошла: вываживание затянулось. ЛКМ — новый заброс."));
    else if (FishDistance <= 140)
    {
        ++CatchCount;
        const TCHAR* Species[] = { TEXT("Окунь"), TEXT("Плотва"), TEXT("Карась") };
        FinishFishing(FString::Printf(TEXT("%s, %.2f кг! ЛКМ — забросить снова."), Species[SpeciesIndex], FishWeight));
    }
}
void ALakeCharacter::ServerFish_Implementation()
{
    if (FishingState == 3) { bReeling = true; return; }
    const double Now = GetWorld()->GetTimeSeconds();
    if (Now < NextActionTime) return;
    NextActionTime = Now + 0.3;
    if (FishingState == 0)
    {
        const FVector Facing = FRotator(0, GetControlRotation().Yaw, 0).Vector();
        const FVector Target = GetActorLocation() + Facing * 850;
        if (!ALakeEnvironment::IsWater(Target))
        {
            Status = TEXT("Посмотрите на озеро и подойдите ближе к берегу.");
            return;
        }
        FloatPosition = FVector(Target.X, Target.Y, 8);
        CastOrigin = GetActorLocation();
        CastDirection = Facing;
        FishingState = 1;
        Status = TEXT("Ждём поклёвку. Следите за поплавком. ПКМ — убрать снасть.");
        GetWorldTimerManager().SetTimer(FishingTimer, this, &ALakeCharacter::Bite, FMath::FRandRange(4.0f, 10.0f), false);
    }
    else if (FishingState == 1)
    {
        ServerCancel_Implementation();
        Status = TEXT("Рано подсекли! ЛКМ — забросить снова.");
    }
    else
    {
        GetWorldTimerManager().ClearTimer(FishingTimer);
        FishingState = 3;
        Tension = 0.2f;
        FishDistance = 850;
        FightTime = 0;
        bReeling = true;
        SpeciesIndex = FMath::RandRange(0, 2);
        FishWeight = FMath::FRandRange(0.12f, 1.8f);
        Status = TEXT("Держите ЛКМ — тянуть. Отпускайте, чтобы снизить натяжение.");
    }
}
void ALakeCharacter::ServerCancel_Implementation()
{
    FinishFishing(TEXT("Снасть убрана. ЛКМ — забросить."));
}
void ALakeCharacter::Bite() { if (FishingState != 1) return; FishingState = 2; Status = TEXT("ПОКЛЁВКА! Нажмите ЛКМ — подсечь!"); GetWorldTimerManager().SetTimer(FishingTimer, this, &ALakeCharacter::Escape, 2.0f, false); }
void ALakeCharacter::Escape() { if (FishingState == 2) FinishFishing(TEXT("Рыба ушла. ЛКМ — попробовать снова.")); }
void ALakeCharacter::EndPlay(const EEndPlayReason::Type Reason) { GetWorldTimerManager().ClearTimer(FishingTimer); Super::EndPlay(Reason); }
void ALakeCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ALakeCharacter, FishingState);
    DOREPLIFETIME(ALakeCharacter, FloatPosition);
    DOREPLIFETIME(ALakeCharacter, Tension);
    DOREPLIFETIME_CONDITION(ALakeCharacter, FishDistance, COND_OwnerOnly);
    DOREPLIFETIME_CONDITION(ALakeCharacter, CatchCount, COND_OwnerOnly);
    DOREPLIFETIME_CONDITION(ALakeCharacter, Status, COND_OwnerOnly);
}
void ALakeHUD::DrawHUD()
{
    Super::DrawHUD();
    auto* Fisher = Cast<ALakeCharacter>(GetOwningPawn());
    if (!Fisher || !Canvas) return;
    DrawRect(FLinearColor(0.015f, 0.025f, 0.04f, 0.85f), 20, 20, FMath::Min(900.0f, Canvas->ClipX - 40), Fisher->FishingState == 3 ? 210 : 130);
    DrawText(TEXT("ТИХОЕ ОЗЕРО"), FLinearColor::White, 40, 35, nullptr, 1.5f);
    DrawText(Fisher->Status, Fisher->FishingState == 2 ? FLinearColor::Yellow : FLinearColor::White, 40, 75);
    DrawText(FString::Printf(TEXT("Улов: %d   |   WASD — ходьба   Мышь — обзор   ПКМ — убрать снасть"), Fisher->CatchCount), FLinearColor::White, 40, 110);
    if (Fisher->FishingState == 3)
    {
        DrawText(FString::Printf(TEXT("Натяжение: %.0f%%   |   До рыбы: %.1f м"), Fisher->Tension * 100, Fisher->FishDistance / 100), FLinearColor::White, 40, 145);
        DrawRect(FLinearColor(0.12f,0.15f,0.18f), 40, 175, 420, 18);
        DrawRect(Fisher->Tension > 0.75f ? FLinearColor::Red : FLinearColor(0.2f,0.8f,0.5f), 40, 175, 420 * Fisher->Tension, 18);
        if (Fisher->Tension > 0.75f) DrawText(TEXT("ОТПУСТИТЕ ЛКМ!"), FLinearColor::Yellow, 480, 175);
    }
    DrawText(TEXT("+"), FLinearColor::White, Canvas->ClipX / 2, Canvas->ClipY / 2);
}
ALakeGameMode::ALakeGameMode() { DefaultPawnClass = ALakeCharacter::StaticClass(); HUDClass = ALakeHUD::StaticClass(); }
void ALakeGameMode::BeginPlay()
{
    Super::BeginPlay();
    GetWorld()->SpawnActor<ALakeEnvironment>();
}
AActor* ALakeGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
    const int32 Slot = NextSpawnSlot++ % 8;
    return GetWorld()->SpawnActor<APlayerStart>(FVector(-550 - (Slot / 4) * 160, -400 + (Slot % 4) * 180, 130), FRotator(-8,0,0));
}

