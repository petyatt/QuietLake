#include "LakeGame.h"
#include "LakeMap.h"
#include "LakeWeather.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
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
#include "LakeFish.h"
#include "LakeRod.h"
#include "LakeMenu.h"
#include "FisherAnim.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/GameStateBase.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Camera/CameraActor.h"
#include "Misc/CommandLine.h"
#include "UnrealClient.h"

ALakeCharacter::ALakeCharacter()
{
    bReplicates = true;
    PrimaryActorTick.bCanEverTick = true;
    GetCharacterMovement()->MaxWalkSpeed = 280;
    // First-person camera at eye height of the mannequin.
    auto* Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
    Camera->SetupAttachment(GetCapsuleComponent());
    Camera->SetRelativeLocation(FVector(14, 0, 70));
    Camera->bUsePawnControlRotation = true;
    FishingCamera = Camera;
    // Over-the-shoulder camera for watching the cast (V).
    ThirdPersonArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("ThirdPersonArm"));
    ThirdPersonArm->SetupAttachment(GetCapsuleComponent());
    ThirdPersonArm->SetRelativeLocation(FVector(0, 0, 60));
    ThirdPersonArm->TargetArmLength = 330;
    ThirdPersonArm->SocketOffset = FVector(0, 70, 30);
    ThirdPersonArm->bUsePawnControlRotation = true;
    ThirdPersonArm->bDoCollisionTest = false;
    ThirdPersonCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("ThirdPersonCamera"));
    ThirdPersonCamera->SetupAttachment(ThirdPersonArm);
    ThirdPersonCamera->SetAutoActivate(false);
    // Body: the UE5 mannequin driven by the C++ fisher animation (locomotion + hands on the rod).
    USkeletalMeshComponent* Body = GetMesh();
    Body->SetSkeletalMesh(LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple")));
    Body->SetRelativeLocationAndRotation(FVector(0, 0, -89), FRotator(0, -90, 0));
    Body->SetAnimInstanceClass(UFisherAnimInstance::StaticClass());
    Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Body->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
}
void ALakeCharacter::BeginPlay()
{
    Super::BeginPlay();
    if (GetNetMode() != NM_DedicatedServer)
    {
        RodVisual = GetWorld()->SpawnActor<ALakeRod>();
        if (RodVisual) RodVisual->AddTickPrerequisiteActor(this);
    }
#if !UE_BUILD_SHIPPING
    // -LakePreview: after the environment shots, film the angler: idle, bait close-up,
    // backswing, the cast, the float on the water and the rod on its stand.
    if (FParse::Param(FCommandLine::Get(), TEXT("LakePreview")) && GetNetMode() == NM_Standalone)
    {
        // The environment shots end at 75 s; the times below were laid out from 50 s.
        constexpr float Offset = 33.0f;
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
        At(58.0f, [Shot] { Shot(TEXT("Fisher_Sweep.png")); });
        for (int32 I = 0; I <= 30; ++I)
            At(57.0f + I * 0.1f, [this, I]
            {
                if (RodVisual) UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_CAST t=%.1f float=%s tip=%s"), I * 0.1f, *RodVisual->BobberLocation().ToString(), *RodVisual->Tip().ToString());
            });
        At(59.0f, [this] { ServerRelease_Implementation(); });
        At(59.4f, [Shot] { Shot(TEXT("Fisher_Cast.png")); });
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
#endif
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
    Input->BindAction("BaitWorm", IE_Pressed, this, &ALakeCharacter::BaitWorm);
    Input->BindAction("BaitMaggot", IE_Pressed, this, &ALakeCharacter::BaitMaggot);
    Input->BindAction("BaitBread", IE_Pressed, this, &ALakeCharacter::BaitBread);
    // 1 cm per wheel notch or key press; holding + / - repeats.
    Input->BindAction("Shallower", IE_Pressed, this, &ALakeCharacter::Shallower);
    Input->BindAction("Shallower", IE_Repeat, this, &ALakeCharacter::Shallower);
    Input->BindAction("Deeper", IE_Pressed, this, &ALakeCharacter::Deeper);
    Input->BindAction("Deeper", IE_Repeat, this, &ALakeCharacter::Deeper);
    Input->BindAction("ToggleRod", IE_Pressed, this, &ALakeCharacter::ToggleRod);
    Input->BindAction("ToggleView", IE_Pressed, this, &ALakeCharacter::ToggleView);
}
// Walking is possible when no line is out, or when the rod rests on its stand.
bool ALakeCharacter::CanWalk() const { return !bCharging; }
void ALakeCharacter::Forward(float Value) { if (CanWalk()) AddMovementInput(GetActorForwardVector(), Value); }
void ALakeCharacter::Right(float Value) { if (CanWalk()) AddMovementInput(GetActorRightVector(), Value); }
void ALakeCharacter::Fish() { ServerFish(); }
void ALakeCharacter::Cancel() { ServerCancel(); }
void ALakeCharacter::Release() { ServerRelease(); }
void ALakeCharacter::ToggleRod() { ServerToggleRod(); }
void ALakeCharacter::ToggleView()
{
    bThirdPerson = !bThirdPerson;
    FishingCamera->SetActive(!bThirdPerson);
    ThirdPersonCamera->SetActive(bThirdPerson);
}
float ALakeCharacter::ServerNow() const
{
    const AGameStateBase* State = GetWorld()->GetGameState();
    return State ? State->GetServerWorldTimeSeconds() : GetWorld()->GetTimeSeconds();
}
bool ALakeCharacter::HasBaitInWater() const { return FishingState == 1 && !bRetrieving && ServerNow() >= BaitActiveTime; }
void ALakeCharacter::ServerRelease_Implementation()
{
    bReeling = false;
    if (bCharging)
    {
        // The longer LMB was held, the farther forward the rod and the line have gone.
        const float Charge = CastCharge(ServerNow());
        bCharging = false;
        DoCast(Charge);
    }
}
namespace
{
    // Rod elevation (deg) carried at rest: almost upright, so the rig hangs clear of the ground.
    float IdleElevation(float Pitch) { return FMath::Clamp(70 + 0.2f * Pitch, 68.0f, 82.0f); }
}
void ALakeCharacter::DoCast(float Power)
{
    const FVector Facing = FRotator(0, GetControlRotation().Yaw, 0).Vector();
    const float Free = ALakeRod::RodLength - ALakeRod::GripPoint;
    const float MainLine = ALakeRod::LineLength - FloatDepth;
    // The float drops from where the sweep has laid it: the rod lowered by Power, the line out
    // ahead of the tip at the sweep angle, plus a little of the forward swing it still carries.
    const float Elevation = FMath::DegreesToRadians(SweepElevation(IdleElevation(FRotator::NormalizeAxis(GetControlRotation().Pitch)), Power));
    const float LineAngle = FMath::DegreesToRadians(SweepLineAngle(Power));
    const float Laid = 34 + Free * FMath::Cos(Elevation) + MainLine * FMath::Cos(LineAngle) + 0.1f * MainLine * Power;
    // A pole rod can only keep the rig as far as its line reaches from the tip held at the
    // waiting angle: rod reach + the line's horizontal span.
    const float Angle = FMath::DegreesToRadians(WaitElevation(FRotator::NormalizeAxis(GetControlRotation().Pitch), FloatDepth));
    const float TipOut = 34 + Free * FMath::Cos(Angle);
    const float Height = FMath::Max(0.0f, GetActorLocation().Z + 4 + Free * FMath::Sin(Angle) - LakeFish::SurfaceZ);
    const float MaxOut = TipOut + FMath::Sqrt(FMath::Max(0.0f, MainLine * MainLine - Height * Height)) * 0.95f;
    const FVector Target = FVector(GetActorLocation().X, GetActorLocation().Y, LakeFish::SurfaceZ) + Facing * FMath::Min(Laid, MaxOut);
    if (!ALakeEnvironment::IsWater(Target))
    {
        Status = TEXT("Поплавок упадёт на берег: подойдите ближе к воде и смотрите на неё.");
        return;
    }
    FloatPosition = FVector(Target.X, Target.Y, LakeFish::SurfaceZ);
    PlaceBait();
    FishingState = 1;
    CastPower = Power;
    CastFlight = 0.85f;               // the float drops a metre or two onto the water
    CastServerTime = ServerNow();
    BaitActiveTime = CastServerTime + 0.1f + CastFlight;
    Status = TEXT("Заброс!");
    // When the float lands, its splash scares fish right under it for a moment.
    FTimerHandle Landing;
    GetWorldTimerManager().SetTimer(Landing, [this, Target]
    {
        if (FishingState != 1) return;
        const float Water = LakeFish::SurfaceZ - ALakeEnvironment::GroundHeight(FloatPosition.X, FloatPosition.Y);
        Status = FloatDepth > Water - 5
            ? FString::Printf(TEXT("Наживка лежит на дне (глубина здесь %.0f см). Ждём поклёвку."), Water)
            : FString::Printf(TEXT("Наживка: %s на глубине %.0f см. Ждём поклёвку, следите за поплавком."), LakeFish::BaitName(Bait), FloatDepth);
        for (TActorIterator<ALakeFish> It(GetWorld()); It; ++It)
            if (FVector::Dist2D(It->GetActorLocation(), Target) < 220) It->Spook(Target, 2.5f);
    }, CastFlight, false);
}
void ALakeCharacter::ServerToggleRod_Implementation()
{
    if (bRodOnStand)
    {
        if (FVector::Dist2D(GetActorLocation(), StandBase) > 260) { Status = TEXT("Подойдите к удочке, чтобы взять её."); return; }
        bRodOnStand = false;
        Status = FishingState ? TEXT("Удочка в руках.") : TEXT("Удочка в руках. Держите ЛКМ — заброс, отпустите — поплавок ляжет на воду.");
        return;
    }
    if (FishingState >= 2 || bCharging || bRetrieving) return;
    // The stand goes into the ground a little in front of the angler, on land.
    const FVector Facing = FRotator(0, GetControlRotation().Yaw, 0).Vector();
    FVector Base = GetActorLocation() + Facing * 70;
    if (ALakeEnvironment::IsWater(Base)) Base = GetActorLocation() + Facing * 25;
    Base.Z = ALakeEnvironment::GroundHeight(Base.X, Base.Y);
    StandBase = Base;
    StandYaw = GetControlRotation().Yaw;
    bRodOnStand = true;
    Status = TEXT("Удочка на рогатке. Можно отойти. R — взять удочку.");
}
void ALakeCharacter::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (HasAuthority())
    {
        const FVector Position = GetActorLocation();
        if (LakeMap::IsWater(Position.X, Position.Y) && LakeMap::ShoreDistance(Position.X, Position.Y) > 60 && !LakeMap::OnPier(Position))
        {
            // Swimming is not implemented: walk the player back out of the water to the nearest bank.
            FVector2D P(Position.X, Position.Y);
            for (int32 Step = 0; Step < 200 && LakeMap::IsWater(P.X, P.Y); ++Step) P -= LakeMap::DeeperDirection(P.X, P.Y) * 50;
            const FVector2D Out = (P - FVector2D(Position.X, Position.Y)).GetSafeNormal();
            P += Out * 120;
            SetActorLocation(FVector(P, ALakeEnvironment::GroundHeight(P.X, P.Y) + GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 5));
            GetCharacterMovement()->StopMovementImmediately();
            if (!bRodOnStand) FinishFishing(TEXT("Глубокая вода. Рыбачьте с берега или с мостков."));
        }
        else if (!LakeMap::InPlayArea(Position.X, Position.Y))
        {
            const FVector2D Home = LakeMap::StartSpot();
            SetActorLocation(FVector(Home, ALakeEnvironment::GroundHeight(Home.X, Home.Y) + 100));
            GetCharacterMovement()->StopMovementImmediately();
            FinishFishing(TEXT("Вы вернулись к месту рыбалки."));
        }
        // A fish holding the bait drags the float along; clients see which fish is on the line.
        if (FishingState == 2 && BaitFish.IsValid())
        {
            const FVector Mouth = BaitFish->MouthLocation();
            FloatPosition = FMath::VInterpTo(FloatPosition, FVector(Mouth.X, Mouth.Y, LakeFish::SurfaceZ), DeltaSeconds, 2);
        }
        LineFish = FishingState == 3 ? HookedFish.Get() : FishingState == 2 ? BaitFish.Get() : nullptr;
    }
    if (FishingState != 3 || !HasAuthority()) UpdateRodPose(DeltaSeconds);
    if (HasAuthority() && (FishingState == 1 || FishingState == 2) && ServerNow() >= BaitActiveTime) ConstrainFloat();
    if (HasAuthority() && FishingState == 3) UpdateFight(DeltaSeconds);
    if (HasAuthority() && bRetrieving) UpdateRetrieve();
    if (GetNetMode() != NM_DedicatedServer) UpdateVisuals(DeltaSeconds);
}
float ALakeCharacter::WaitElevation(float Pitch, float Depth) const
{
    // Normally ~20 deg. The float must still reach the water: keep the tip no higher above the
    // surface than ~85% of the main line left above the float.
    const float MainLine = ALakeRod::LineLength - Depth;
    const float Free = ALakeRod::RodLength - ALakeRod::GripPoint;
    const float Hand = GetActorLocation().Z + 4 - LakeFish::SurfaceZ;
    const float MaxSin = FMath::Clamp((0.85f * MainLine - Hand) / Free, -0.14f, 1.0f);
    return FMath::Min(20 + 0.3f * Pitch, FMath::RadiansToDegrees(FMath::Asin(MaxSin)));
}
void ALakeCharacter::RodPoseTarget(float Pitch, float Now, float& OutElevation, FVector& OutGrip, float& OutStiffness) const
{
    // Right hand position relative to the body (X forward, Y right, Z up from the hips) and the
    // rod's elevation above the horizon for each phase. With the line as long as the rod, a pole
    // is carried almost upright so the float rig hangs clear of the ground.
    const FVector Idle(24, 20, 14), Forward(40, 16, 12), Waiting(34, 18, 4);
    float Elevation = IdleElevation(Pitch);
    FVector Grip = Idle;
    float Stiffness = 7;                       // rad/s: a long rod has weight and lags behind
    if (bCharging)
    {
        // LMB held: a slow, steady sweep forward and down; the longer it is held, the lower the
        // rod and the farther ahead the line is laid.
        const float C = CastCharge(Now);
        Elevation = SweepElevation(Elevation, C);
        Grip = FMath::Lerp(Idle, Forward, FMath::InterpEaseInOut(0.0f, 1.0f, C, 1.6f));
        Stiffness = 6;
    }
    else if (FishingState > 0)
    {
        const float Since = Now - CastServerTime;
        Elevation = WaitElevation(Pitch, HookDepth);
        Grip = Waiting;
        if (Since >= 0 && Since < 1.5f) Stiffness = 3.5f;   // after the cast the heavy pole settles slowly
        if (bRetrieving)
        {
            // Drawing the rig out: the rod is raised steadily towards the angler, never jerked.
            const float R = FMath::Clamp((Now - RetrieveServerTime) / RetrieveTime, 0.0f, 1.0f);
            const float A = FMath::InterpEaseInOut(0.0f, 1.0f, R, 2.0f);
            Elevation = FMath::Lerp(RetrieveFromElevation, IdleElevation(Pitch), A);
            Grip = FMath::Lerp(Waiting, Idle, A);
            Stiffness = 9;
        }
        if (FishingState == 3)
        {
            // No reel: holding LMB lifts the rod and draws the fish in, releasing lowers it
            // towards the fish to give line. A strong pull drags the tip down.
            Elevation = FMath::Lerp(22.0f, 80.0f, RodLift) - Tension * 15;
            Grip = FMath::Lerp(FVector(36, 18, 8), FVector(20, 18, 24), RodLift);
            Stiffness = 6;
        }
        const float SinceStrike = Now - StrikeServerTime;
        if (SinceStrike >= 0 && SinceStrike < 0.3f) { Elevation += 15 * FMath::Sin(PI * SinceStrike / 0.3f); Stiffness = 20; }
    }
    OutElevation = Elevation;
    OutGrip = Grip;
    OutStiffness = Stiffness;
}
void ALakeCharacter::UpdateRodPose(float DeltaSeconds)
{
    // Critically damped springs towards the target pose: the rod has weight, so it follows the
    // hands and the body's turns with a little lag, except during the quick casting swing.
    const FRotator View = IsLocallyControlled() && GetController() ? GetControlRotation() : GetBaseAimRotation();
    float TargetElevation, Stiffness;
    FVector TargetGrip;
    RodPoseTarget(FRotator::NormalizeAxis(View.Pitch), ServerNow(), TargetElevation, TargetGrip, Stiffness);
    const float TargetYaw = GetActorRotation().Yaw + 7;
    if (!bRodPoseReady)
    {
        RodElevation = TargetElevation; RodGrip = TargetGrip; RodYaw = TargetYaw;
        RodElevationSpeed = RodYawSpeed = 0; RodGripSpeed = FVector::ZeroVector;
        bRodPoseReady = true;
    }
    const float Dt = FMath::Min(DeltaSeconds, 0.05f);
    // Exact step of a critically damped spring: stable for any frame time.
    auto Spring = [Dt, Stiffness](auto& Value, auto& Speed, double Target)
    {
        const double Offset = Value - Target;
        const double Temp = (Speed + Stiffness * Offset) * Dt;
        const double Decay = FMath::Exp(-Stiffness * Dt);
        Value = Target + (Offset + Temp) * Decay;
        Speed = (Speed - Stiffness * Temp) * Decay;
    };
    Spring(RodElevation, RodElevationSpeed, TargetElevation);
    RodYaw = FRotator::NormalizeAxis(RodYaw);
    Spring(RodYaw, RodYawSpeed, RodYaw + FMath::FindDeltaAngleDegrees(RodYaw, TargetYaw));
    for (int32 Axis = 0; Axis < 3; ++Axis) Spring(RodGrip[Axis], RodGripSpeed[Axis], TargetGrip[Axis]);
}
FTransform ALakeCharacter::StandHold() const
{
    // On the stand: the butt rests on the ground behind the fork, the rod over the fork.
    const FVector Dir = FRotator(0, StandYaw, 0).Vector();
    const FVector Fork = StandBase + FVector(0, 0, 62);
    FVector Butt = Fork - Dir * 120;
    Butt.Z = ALakeEnvironment::GroundHeight(Butt.X, Butt.Y) + 3;
    return FTransform(FRotationMatrix::MakeFromXZ((Fork - Butt).GetSafeNormal(), FVector::UpVector).Rotator(), Butt);
}
FTransform ALakeCharacter::RodHold() const
{
    if (bRodOnStand) return StandHold();
    const FVector Axis = FRotator(RodElevation, RodYaw, 0).Vector();
    const FVector GripWorld = GetActorTransform().TransformPosition(RodGrip);
    return FTransform(FRotationMatrix::MakeFromXZ(Axis, FVector::UpVector).Rotator(), GripWorld - Axis * ALakeRod::GripPoint);
}
void ALakeCharacter::UpdateVisuals(float DeltaSeconds)
{
    if (!RodVisual) return;
    const float Now = ServerNow();
    const FRotator View = IsLocallyControlled() && GetController() ? GetControlRotation() : GetBaseAimRotation();
    const float Pitch = FRotator::NormalizeAxis(View.Pitch);
    StandAlpha = FMath::FInterpConstantTo(StandAlpha, bRodOnStand ? 1.0f : 0.0f, DeltaSeconds, 2.5f);

    // Rod pose: in the hands, or resting on the forked stand, blended while putting it down.
    const FVector Axis0 = FRotator(RodElevation, RodYaw, 0).Vector();
    FTransform Hold(FRotationMatrix::MakeFromXZ(Axis0, FVector::UpVector).Rotator(), GetActorTransform().TransformPosition(RodGrip) - Axis0 * ALakeRod::GripPoint);
    // A fish on the line makes the rod tremble in the hands, more the harder it pulls.
    if (FishingState == 3 && !bRodOnStand)
    {
        const float J = Tension * 1.3f + 0.15f;
        Hold.ConcatenateRotation(FRotator(J * FMath::Sin(Now * 23) * FMath::Sin(Now * 6.1f), J * 0.6f * FMath::Sin(Now * 17), 0).Quaternion());
    }
    if (StandAlpha > 0) Hold.Blend(Hold, StandHold(), StandAlpha);
    FRodInput In;
    In.Hold = Hold;
    In.FishingState = FishingState;
    In.Bait = Bait;
    In.FloatTarget = FloatPosition;
    In.HookDepth = HookDepth;
    In.SinceCast = FishingState > 0 && CastServerTime > 0 ? Now - CastServerTime : -1;
    In.CastFlight = CastFlight;
    In.bSweeping = bCharging;
    In.SweepAngle = SweepLineAngle(CastCharge(Now));
    In.Facing = FRotator(0, GetActorRotation().Yaw, 0).Vector();
    In.Tension = Tension;
    In.Nibbles = NibbleCounter;
    In.bReeling = false;
    In.bOnStand = bRodOnStand || StandAlpha > 0;
    In.StandBase = StandBase;
    In.StandYaw = StandYaw;
    In.LineFish = LineFish;
    RodVisual->UpdateRod(In, DeltaSeconds);

    // Hands on the rod: right hand at the grip, left hand at the butt end.
    if (auto* Anim = Cast<UFisherAnimInstance>(GetMesh()->GetAnimInstance()))
    {
        FFisherPoseInput& Pose = Anim->PoseInput;
        Pose.Speed = GetVelocity().Size2D();
        Pose.AimPitch = Pitch;
        const FVector Axis = Hold.GetUnitAxis(EAxis::X), RodUp = Hold.GetUnitAxis(EAxis::Z);
        const FVector Right = GetActorRightVector(), Up = FVector::UpVector, Fwd = GetActorForwardVector();
        const float HandsAlpha = 1 - StandAlpha;
        Pose.Right.Point = Hold.TransformPosition(FVector(ALakeRod::GripPoint + 2, 0, 0));
        Pose.Right.Axis = Axis;
        Pose.Right.Down = -RodUp;
        Pose.Right.Pole = GetActorLocation() + Right * 60 - Up * 60 - Fwd * 20;
        Pose.Right.Alpha = HandsAlpha;
        Pose.Left.Point = Hold.TransformPosition(FVector(5, 0, 0));
        Pose.Left.Axis = Axis;
        Pose.Left.Down = -RodUp;
        Pose.Left.Pole = GetActorLocation() - Right * 60 - Up * 60 - Fwd * 20;
        Pose.Left.Alpha = HandsAlpha;
    }
    // In first person the owner's own head would block the camera.
    if (IsLocallyControlled())
    {
        if (bThirdPerson) GetMesh()->UnHideBoneByName(TEXT("neck_01"));
        else GetMesh()->HideBoneByName(TEXT("neck_01"), PBO_None);
    }
}
void ALakeCharacter::FinishFishing(const FString& Message)
{
    GetWorldTimerManager().ClearTimer(FishingTimer);
    // Whatever was on the line swims off frightened.
    if (ALakeFish* Fish = HookedFish.Get()) Fish->Release(12);
    else if (ALakeFish* Holder = BaitFish.Get(); Holder && FishingState == 2) Holder->Release(10);
    HookedFish = nullptr;
    BaitFish = nullptr;
    LineFish = nullptr;
    bCharging = false;
    bRetrieving = false;
    FishingState = 0;
    Tension = 0;
    bReeling = false;
    Status = Message;
    ForceNetUpdate();
}
void ALakeCharacter::StartRetrieve(const FString& Message, const FString& EndMessage)
{
    if (bRetrieving || FishingState == 0) return;
    // Whatever was on the line swims off; the rig is then simply lifted out by raising the rod,
    // the line drawing it across the water towards the angler.
    GetWorldTimerManager().ClearTimer(FishingTimer);
    if (ALakeFish* Fish = HookedFish.Get()) Fish->Release(12);
    else if (ALakeFish* Holder = BaitFish.Get(); Holder && FishingState == 2) Holder->Release(10);
    HookedFish = nullptr;
    BaitFish = nullptr;
    LineFish = nullptr;
    FishingState = 1;
    Tension = 0;
    bReeling = false;
    bRetrieving = true;
    RetrieveServerTime = ServerNow();
    RetrieveFromElevation = RodElevation;
    RetrieveEndMessage = EndMessage;
    Status = Message;
    ForceNetUpdate();
}
void ALakeCharacter::UpdateRetrieve()
{
    if (bRetrieving && ServerNow() - RetrieveServerTime >= RetrieveTime) FinishFishing(RetrieveEndMessage);
}
bool ALakeCharacter::TakeRodFromStand()
{
    if (!bRodOnStand) return true;
    if (FVector::Dist2D(GetActorLocation(), StandBase) > 260) { Status = TEXT("Подойдите к удочке, чтобы взять её."); return false; }
    bRodOnStand = false;
    return true;
}
FVector ALakeCharacter::RodTip() const
{
    return RodHold().TransformPosition(FVector(ALakeRod::RodLength, 0, 0));
}
void ALakeCharacter::ConstrainFloat()
{
    // The float can be no farther from the tip than the line allows: walking away drags it along,
    // and dragging it onto the bank ends the cast.
    const FVector Tip = RodTip();
    const float MainLine = ALakeRod::LineLength - HookDepth;
    const float Height = FMath::Max(0.0f, Tip.Z - LakeFish::SurfaceZ);
    const float Reach = FMath::Sqrt(FMath::Max(0.0f, MainLine * MainLine - Height * Height));
    FVector Offset = FloatPosition - Tip;
    Offset.Z = 0;
    if (Offset.Size() > Reach)
    {
        FloatPosition = FVector(Tip.X, Tip.Y, LakeFish::SurfaceZ) + Offset.GetSafeNormal() * Reach;
        PlaceBait();
    }
    if (!bRetrieving && !ALakeEnvironment::IsWater(FloatPosition))
        StartRetrieve(TEXT("Снасть вышла на берег."), TEXT("Вы вытащили снасть из воды. ЛКМ — забросить снова."));
}
void ALakeCharacter::UpdateFight(float DeltaSeconds)
{
    if (FishingState != 3) return;
    ALakeFish* Fish = HookedFish.Get();
    if (!Fish) { FinishFishing(TEXT("Рыба сошла. ЛКМ — новый заброс.")); return; }
    FightTime += DeltaSeconds;
    // Holding LMB raises the rod, releasing lowers it. The fish's pull works against the hands:
    // the harder it pulls, the slower the rod comes up, and a strong run drags it down.
    RodLift = FMath::Clamp(RodLift + DeltaSeconds * (bReeling ? 0.35f - 0.6f * Tension * Tension : -0.5f), 0.0f, 1.0f);
    UpdateRodPose(DeltaSeconds);
    // Pole rod: the line has a fixed length, so the fish can only be drawn in by lifting the rod
    // (LMB) or stepping back. The rod's bend is the shock absorber: about 1.4 m of give before the
    // line breaks. Lowering the rod (release LMB) moves the tip towards the fish and eases the pull.
    constexpr float BreakForce = 2600.0f, RodGive = 140.0f;
    const FVector Tip = RodTip();
    const FVector ToTip = Tip - Fish->MouthLocation();
    const float Distance = ToTip.Size();
    const float Stretch = FMath::Max(0.0f, Distance - ALakeRod::LineLength);
    const float Force = Stretch * BreakForce / RodGive;
    Tension = FMath::Clamp(Force / BreakForce, 0.0f, 1.0f);
    Fish->ApplyLineForce(ToTip.GetSafeNormal() * Force);
    FishDistance = FVector::Dist2D(Fish->GetActorLocation(), GetActorLocation());
    FloatPosition = FVector(Fish->GetActorLocation().X, Fish->GetActorLocation().Y, -18);
    const FFishSpeciesInfo& S = LakeFish::Info(Fish->Species);
    // A short hint as the fight goes: the fish runs, holds or is tiring.
    const int32 Hint = Tension > 0.75f ? 1 : Fish->StaminaFraction() < 0.35f ? 2 : 0;
    if (Hint != FightHint)
    {
        FightHint = Hint;
        Status = Hint == 1 ? TEXT("Рыба тянет! Отпустите ЛКМ, дайте удилищу пружинить.")
               : Hint == 2 ? TEXT("Рыба устала. Поднимайте её к поверхности и подводите к берегу.")
               : TEXT("Рыба водит. Держите ЛКМ — поднимать удилище, отпускайте на рывках.");
    }
    if (Force >= BreakForce) FinishFishing(TEXT("Леска оборвалась! Чаще отпускайте ЛКМ. ЛКМ — новый заброс."));
    else if (FightTime >= 120) FinishFishing(TEXT("Рыба сошла: вываживание затянулось. ЛКМ — новый заброс."));
    // Landed: a tired fish drawn up close to the angler near the surface, or led into the shallows.
    else if (Fish->StaminaFraction() < 0.35f &&
             ((FishDistance < 380 && Fish->GetActorLocation().Z > LakeFish::SurfaceZ - 60) ||
              (LakeMap::ShoreDistance(Fish->GetActorLocation().X, Fish->GetActorLocation().Y) < 300 && FishDistance < 900)))
    {
        ++CatchCount;
        const FString Message = FString::Printf(TEXT("%s, %.2f кг (%.0f см)! ЛКМ — забросить снова."), S.Name, Fish->WeightKg, LakeFish::LengthCm(Fish->Species, Fish->WeightKg));
        const EFishSpecies Species = Fish->Species;
        HookedFish = nullptr;
        Fish->Destroy();
        for (TActorIterator<ALakeEnvironment> It(GetWorld()); It; ++It) It->ScheduleRespawn(Species);
        StartRetrieve(Message, Message);
    }
}
void ALakeCharacter::ServerFish_Implementation()
{
    if (FishingState == 3) { bReeling = true; return; }
    if (bRetrieving) return;
    const double Now = GetWorld()->GetTimeSeconds();
    if (Now < NextActionTime) return;
    NextActionTime = Now + 0.3;
    if (FishingState == 0)
    {
        if (bRodOnStand) { Status = TEXT("Сначала возьмите удочку: R."); return; }
        if (bCharging) return;
        bCharging = true;
        ChargeServerStart = ServerNow();
        Status = TEXT("Ведите удилище вперёд… Чем дольше держите ЛКМ, тем дальше заброс. Отпустите — поплавок ляжет на воду.");
    }
    else if (FishingState == 1)
    {
        if (ServerNow() < BaitActiveTime || !TakeRodFromStand()) return;
        if (ALakeFish* Fish = BaitFish.Get()) Fish->Spook(BaitPoint, 4);
        StrikeServerTime = ServerNow();
        StartRetrieve(TEXT("Рано подсекли! Дождитесь, пока поплавок уйдёт под воду."),
                      TEXT("Рано подсекли! Дождитесь, пока поплавок уйдёт под воду. ЛКМ — забросить снова."));
    }
    else
    {
        ALakeFish* Fish = BaitFish.Get();
        if (!Fish) { FinishFishing(TEXT("Пусто: рыба уже отпустила наживку.")); return; }
        if (bRodOnStand)
        {
            // Rod on the stand: grab it and strike in one move, if close enough.
            if (FVector::Dist2D(GetActorLocation(), StandBase) > 260) { Status = TEXT("ПОКЛЁВКА! Бегите к удочке!"); return; }
            bRodOnStand = false;
        }
        StrikeServerTime = ServerNow();
        GetWorldTimerManager().ClearTimer(FishingTimer);
        Fish->Hook(this);
        HookedFish = Fish;
        BaitFish = nullptr;
        FishingState = 3;
        Tension = 0.2f;
        FightTime = 0;
        FightHint = -1;
        bReeling = true;
        RodLift = 0;
        Status = TEXT("Засекли! Держите ЛКМ — поднять удилище и тянуть, отпускайте, когда рыба рвётся. Можно отходить от воды.");
    }
}
void ALakeCharacter::ServerCancel_Implementation()
{
    if (bCharging) { bCharging = false; Status = TEXT("Заброс отменён. ЛКМ — забросить."); return; }
    // Wait for the float to land before lifting it out again.
    if (FishingState == 0 || bRetrieving || (FishingState == 1 && ServerNow() < BaitActiveTime)) return;
    if (!TakeRodFromStand()) return;
    StartRetrieve(TEXT("Поднимаете удилище — снасть выходит из воды…"), TEXT("Снасть убрана. ЛКМ — забросить."));
}
bool ALakeCharacter::ClaimBait(ALakeFish* Fish)
{
    if (FishingState != 1 || bRetrieving) return false;
    if (BaitFish.IsValid() && BaitFish.Get() != Fish) return false;
    BaitFish = Fish;
    return true;
}
void ALakeCharacter::ReleaseBaitClaim(ALakeFish* Fish)
{
    if (BaitFish.Get() == Fish && FishingState == 1) BaitFish = nullptr;
}
void ALakeCharacter::OnFishNibble(ALakeFish* Fish)
{
    if (FishingState != 1 || bRetrieving) return;
    ++NibbleCounter;
    Status = TEXT("Поплавок вздрагивает — рыба пробует наживку. Ждите…");
}
void ALakeCharacter::OnFishTake(ALakeFish* Fish)
{
    if (FishingState != 1 || bRetrieving) return;
    BaitFish = Fish;
    FishingState = 2;
    Status = TEXT("ПОКЛЁВКА! Поплавок ушёл под воду — нажмите ЛКМ, подсекайте!");
    // Cautious fish hold the bait a little longer before spitting it out.
    // A hungry fish takes the bait boldly and holds it longer; a wary one drops it quickly.
    const float Window = (Fish && Fish->Species == EFishSpecies::Crucian ? 2.6f : Fish && Fish->Species == EFishSpecies::Pike ? 1.5f : 2.0f)
        * FMath::Lerp(0.7f, 1.25f, FMath::Clamp((Fish ? Fish->Appetite() : 1.0f) / 1.2f, 0.0f, 1.0f));
    GetWorldTimerManager().SetTimer(FishingTimer, this, &ALakeCharacter::Escape, Window, false);
    ForceNetUpdate();
}
void ALakeCharacter::PlaceBait()
{
    HookDepth = FloatDepth;
    const float Bottom = ALakeEnvironment::GroundHeight(FloatPosition.X, FloatPosition.Y);
    BaitPoint = FVector(FloatPosition.X, FloatPosition.Y, FMath::Max(LakeFish::SurfaceZ - HookDepth, Bottom + 3));
}
void ALakeCharacter::ServerChangeDepth_Implementation(float Delta)
{
    // The float is set on the line before casting; the hook hangs at most 15-20% less than the rod length under it.
    static_assert(MaxDepth >= 0.8f * ALakeRod::RodLength && MaxDepth <= 0.85f * ALakeRod::RodLength, "depth limit is 15-20% short of the rod");
    if (FishingState != 0 || bCharging) { Status = TEXT("Глубину можно менять только до заброса: сначала вытащите снасть (ПКМ)."); return; }
    FloatDepth = FMath::Clamp(FMath::RoundToFloat(FloatDepth + Delta), MinDepth, MaxDepth);
    HookDepth = FloatDepth;      // the float slides along the hanging rig right away
    Status = FString::Printf(TEXT("Глубина спуска наживки: %.0f см."), FloatDepth);
}
void ALakeCharacter::Escape()
{
    if (FishingState == 2) StartRetrieve(TEXT("Рыба выплюнула наживку."), TEXT("Рыба выплюнула наживку. ЛКМ — попробовать снова."));
}
void ALakeCharacter::SelectBait(EBait NewBait) { ServerSelectBait(NewBait); }
void ALakeCharacter::ServerSelectBait_Implementation(EBait NewBait)
{
    if (FishingState != 0 || NewBait >= EBait::Count) return;
    Bait = NewBait;
    Status = FString::Printf(TEXT("Наживка: %s. ЛКМ — забросить."), LakeFish::BaitName(Bait));
}
void ALakeCharacter::EndPlay(const EEndPlayReason::Type Reason)
{
    GetWorldTimerManager().ClearTimer(FishingTimer);
    if (RodVisual) RodVisual->Destroy();
    Super::EndPlay(Reason);
}
void ALakeCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ALakeCharacter, FishingState);
    DOREPLIFETIME(ALakeCharacter, FloatPosition);
    DOREPLIFETIME(ALakeCharacter, Tension);
    DOREPLIFETIME(ALakeCharacter, Bait);
    DOREPLIFETIME(ALakeCharacter, NibbleCounter);
    DOREPLIFETIME(ALakeCharacter, bCharging);
    DOREPLIFETIME(ALakeCharacter, ChargeServerStart);
    DOREPLIFETIME(ALakeCharacter, bRetrieving);
    DOREPLIFETIME(ALakeCharacter, RetrieveServerTime);
    DOREPLIFETIME(ALakeCharacter, RetrieveFromElevation);
    DOREPLIFETIME(ALakeCharacter, CastServerTime);
    DOREPLIFETIME(ALakeCharacter, CastFlight);
    DOREPLIFETIME(ALakeCharacter, CastPower);
    DOREPLIFETIME(ALakeCharacter, StrikeServerTime);
    DOREPLIFETIME(ALakeCharacter, HookDepth);
    DOREPLIFETIME(ALakeCharacter, FloatDepth);
    DOREPLIFETIME(ALakeCharacter, bReeling);
    DOREPLIFETIME(ALakeCharacter, RodLift);
    DOREPLIFETIME(ALakeCharacter, bRodOnStand);
    DOREPLIFETIME(ALakeCharacter, StandBase);
    DOREPLIFETIME(ALakeCharacter, StandYaw);
    DOREPLIFETIME(ALakeCharacter, LineFish);
    DOREPLIFETIME_CONDITION(ALakeCharacter, FishDistance, COND_OwnerOnly);
    DOREPLIFETIME_CONDITION(ALakeCharacter, CatchCount, COND_OwnerOnly);
    DOREPLIFETIME_CONDITION(ALakeCharacter, Status, COND_OwnerOnly);
}
void ALakeHUD::DrawHUD()
{
    Super::DrawHUD();
    auto* Fisher = Cast<ALakeCharacter>(GetOwningPawn());
    if (!Fisher || !Canvas) return;
    if (auto* PC = Cast<ALakePlayerController>(GetOwningPlayerController()); PC && PC->IsMenuOpen()) { PlayStart = -1; return; }
    const float Now = GetWorld()->GetRealTimeSeconds();
    if (PlayStart < 0) PlayStart = Now;
    // Everything scales with the screen height (designed for 1080p) and stays out of the view:
    // a small status card in the bottom-left corner, bars at the bottom centre only when needed.
    const float S = FMath::Max(0.8f, Canvas->ClipY / 1080.0f) * 1.4f;
    UFont* Font = GEngine->GetMediumFont();
    auto Text = [this, Font, S](const FString& Line, FLinearColor Colour, float X, float Y, float Scale)
    {
        DrawText(Line, Colour, X, Y, Font, Scale * S);
    };
    auto Width = [this, Font, S](const FString& Line, float Scale)
    {
        float W = 0, H = 0;
        GetTextSize(Line, W, H, Font, Scale * S);
        return W;
    };

    // Status: a new message shows for 6 s and fades; a bite stays (and is highlighted) while it lasts.
    if (Fisher->Status != LastStatus) { LastStatus = Fisher->Status; StatusTime = Now; }
    const bool bUrgent = Fisher->FishingState == 2 || Fisher->FishingState == 3;
    const float Age = Now - StatusTime;
    const float Alpha = bUrgent ? 1.0f : FMath::Clamp((6.0f - Age) / 1.0f, 0.0f, 1.0f);
    const FString Info = FString::Printf(TEXT("Улов: %d   ·   Наживка: %s   ·   Глубина: %.0f см"), Fisher->CatchCount, LakeFish::BaitName(Fisher->Bait), Fisher->FloatDepth);
    const float Pad = 14 * S, X = 28 * S;
    const float LineH = 26 * S;
    const float CardW = FMath::Max(Width(Info, 0.85f), Alpha > 0 ? Width(LastStatus, 0.95f) : 0.0f) + Pad * 2;
    const float CardH = (Alpha > 0 ? LineH * 2 : LineH) + Pad * 2 - 6 * S;
    const float CardY = Canvas->ClipY - CardH - 28 * S;
    DrawRect(FLinearColor(0.01f, 0.015f, 0.02f, 0.45f), X, CardY, CardW, CardH);
    float Y = CardY + Pad - 4 * S;
    if (Alpha > 0)
    {
        const FLinearColor Colour = Fisher->FishingState == 2 ? FLinearColor(1.0f, 0.82f, 0.25f) : FLinearColor(0.93f, 0.91f, 0.86f, Alpha);
        Text(LastStatus, Colour, X + Pad, Y, 0.95f);
        Y += LineH;
    }
    Text(Info, FLinearColor(0.7f, 0.74f, 0.72f, 0.9f), X + Pad, Y, 0.85f);

    // Line tension while fighting a fish.
    const float BarW = 360 * S, BarH = 8 * S, BarX = (Canvas->ClipX - BarW) / 2, BarY = Canvas->ClipY - 70 * S;
    if (Fisher->FishingState == 3)
    {
        const FLinearColor Fill = Fisher->Tension > 0.75f ? FLinearColor(0.95f, 0.25f, 0.2f) : FLinearColor(0.35f, 0.8f, 0.55f);
        DrawRect(FLinearColor(0, 0, 0, 0.4f), BarX, BarY, BarW, BarH);
        DrawRect(Fill, BarX, BarY, BarW * Fisher->Tension, BarH);
        const FString Label = Fisher->Tension > 0.75f ? TEXT("Отпустите ЛКМ — леска на пределе")
            : FString::Printf(TEXT("Натяжение %.0f%%   ·   до рыбы %.1f м"), Fisher->Tension * 100, Fisher->FishDistance / 100);
        Text(Label, FLinearColor(0.93f, 0.91f, 0.86f), (Canvas->ClipX - Width(Label, 0.8f)) / 2, BarY - 26 * S, 0.8f);
    }
    // Casting power while swinging.
    if (Fisher->bCharging)
    {
        const float Power = Fisher->CastCharge(Fisher->ServerNow());
        DrawRect(FLinearColor(0, 0, 0, 0.4f), BarX, BarY, BarW, BarH);
        DrawRect(FLinearColor(0.95f, 0.75f, 0.2f), BarX, BarY, BarW * Power, BarH);
        const FString Label = TEXT("Дальность заброса");
        Text(Label, FLinearColor(0.93f, 0.91f, 0.86f), (Canvas->ClipX - Width(Label, 0.8f)) / 2, BarY - 26 * S, 0.8f);
    }
    // Bottom right: the time of day, the weather and the air temperature.
    float WeatherTop = Canvas->ClipY - 28 * S;
    if (const ALakeWeather* Weather = ALakeWeather::Get(GetWorld()))
    {
        const float H = Weather->Hours();
        const FString Clock = FString::Printf(TEXT("%02d:%02d"), (int32)H, (int32)(FMath::Frac(H) * 60));
        const int32 T = FMath::RoundToInt(Weather->TemperatureC());
        const FString Line = Weather->Describe() + FString::Printf(TEXT("  ·  %s%d °C"), T > 0 ? TEXT("+") : TEXT(""), T);
        // Pressure and water temperature: what an angler checks to guess the bite.
        const float Trend = Weather->PressureTrend();
        const int32 Water = FMath::RoundToInt(Weather->WaterTempC());
        const FString Line2 = FString::Printf(TEXT("Давление %d мм%s  ·  вода %s%d °C"), FMath::RoundToInt(Weather->PressureMmHg()),
            Trend < -0.3f ? TEXT(", падает") : Trend > 0.3f ? TEXT(", растёт") : TEXT(""), Water > 0 ? TEXT("+") : TEXT(""), Water);
        const float W = FMath::Max3(Width(Clock, 1.35f), Width(Line, 0.8f), Width(Line2, 0.8f)) + Pad * 2;
        const float CardH2 = 30 * S + LineH * 2 + Pad * 2 - 6 * S;
        const float CX = Canvas->ClipX - W - 28 * S, CY = Canvas->ClipY - CardH2 - 28 * S;
        DrawRect(FLinearColor(0.01f, 0.015f, 0.02f, 0.45f), CX, CY, W, CardH2);
        Text(Clock, FLinearColor(0.95f, 0.94f, 0.9f), CX + W - Pad - Width(Clock, 1.35f), CY + Pad - 6 * S, 1.35f);
        Text(Line, FLinearColor(0.75f, 0.78f, 0.76f, 0.95f), CX + W - Pad - Width(Line, 0.8f), CY + Pad + 30 * S, 0.8f);
        Text(Line2, FLinearColor(0.68f, 0.72f, 0.72f, 0.9f), CX + W - Pad - Width(Line2, 0.8f), CY + Pad + 30 * S + LineH, 0.8f);
        WeatherTop = CY;
    }
    // Key hints for the first seconds of play only (the full list is in the Esc menu).
    const float HintAlpha = FMath::Clamp((15.0f - (Now - PlayStart)) / 2.0f, 0.0f, 1.0f);
    if (HintAlpha > 0)
    {
        const FString Hint = TEXT("ЛКМ (держать) — заброс   ·   ПКМ — вытащить   ·   +/− — глубина   ·   R — рогатка   ·   V — вид   ·   1/2/3 — наживка   ·   Esc — меню");
        Text(Hint, FLinearColor(0.85f, 0.87f, 0.85f, 0.75f * HintAlpha), Canvas->ClipX - Width(Hint, 0.75f) - 28 * S, WeatherTop - 30 * S, 0.75f);
    }
    // A small dot in the centre for aiming the cast (first person only).
    if (!Fisher->bThirdPerson)
        DrawRect(FLinearColor(1, 1, 1, 0.55f), Canvas->ClipX / 2 - 1.5f * S, Canvas->ClipY / 2 - 1.5f * S, 3 * S, 3 * S);
}
ALakeGameMode::ALakeGameMode()
{
    DefaultPawnClass = ALakeCharacter::StaticClass();
    HUDClass = ALakeHUD::StaticClass();
    PlayerControllerClass = ALakePlayerController::StaticClass();
}
void ALakeGameMode::BeginPlay()
{
    Super::BeginPlay();
    GetWorld()->SpawnActor<ALakeEnvironment>();
    GetWorld()->SpawnActor<ALakeWeather>();
}
AActor* ALakeGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
    const int32 Slot = NextSpawnSlot++ % 8;
    FVector2D Facing;
    const FVector2D Spot = LakeMap::StartSpot(&Facing);
    const FVector2D Right(-Facing.Y, Facing.X);
    const FVector2D P = Spot - Facing * (300 + (Slot / 4) * 160) + Right * (-400 + (Slot % 4) * 180);
    return GetWorld()->SpawnActor<APlayerStart>(FVector(P, ALakeEnvironment::GroundHeight(P.X, P.Y) + 110), FRotator(-8, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)), 0));
}

