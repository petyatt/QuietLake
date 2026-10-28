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
#include "LakeHUD.h"
#include "LakeLife.h"
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
    RunPreviews();
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
    Input->BindAction("NextRod", IE_Pressed, this, &ALakeCharacter::NextRod);
    Input->BindAction("ToggleView", IE_Pressed, this, &ALakeCharacter::ToggleView);
    Input->BindAction("LifePanel", IE_Pressed, this, &ALakeCharacter::ToggleLifePanel);
}
float ALakeCharacter::RodGrams(ERodKind Kind)
{
    return Kind == ERodKind::Light ? 180.0f : Kind == ERodKind::Heavy ? 650.0f : 320.0f;
}
const TCHAR* ALakeCharacter::RodName(ERodKind Kind)
{
    return Kind == ERodKind::Light ? TEXT("лёгкое карбоновое") : Kind == ERodKind::Heavy ? TEXT("тяжёлое стеклопластиковое") : TEXT("среднее композитное");
}
void ALakeCharacter::ServerNextRod_Implementation()
{
    if (FishingState != EFishingState::Idle || bCharging || bRodOnStand) { Status = TEXT("Сменить удилище можно, когда снасть не в воде и удочка в руках."); return; }
    RodKind = (ERodKind)(((int32)RodKind + 1) % (int32)ERodKind::Count);
    Status = FString::Printf(TEXT("Удилище: %s, 6 м, %.0f г."), RodName(RodKind), RodGrams(RodKind));
}
void ALakeCharacter::ToggleLifePanel()
{
    if (const APlayerController* PC = Cast<APlayerController>(GetController()))
        if (ALakeHUD* HUD = Cast<ALakeHUD>(PC->GetHUD())) HUD->bShowLife = !HUD->bShowLife;
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
bool ALakeCharacter::HasBaitInWater() const { return FishingState == EFishingState::Waiting && !bRetrieving && ServerNow() >= BaitActiveTime; }
void ALakeCharacter::ServerRelease_Implementation()
{
    bReeling = false;
    if (bCharging)
    {
        bCharging = false;
        DoCast(ServerNow() - ChargeServerStart);
    }
}
namespace
{
    // Rod elevation (deg) carried at rest: almost upright, so the rig hangs clear of the ground.
    float IdleElevation(float Pitch) { return FMath::Clamp(70 + 0.2f * Pitch, 68.0f, 82.0f); }
}
float ALakeCharacter::CastReach() const
{
    // A pole can only lay the rig as far as its line reaches from the tip held at the waiting
    // angle: the rod's reach plus the line's horizontal span.
    const float Free = ALakeRod::RodLength - ALakeRod::GripPoint;
    const float MainLine = ALakeRod::LineLength - FloatDepth;
    const float Angle = FMath::DegreesToRadians(WaitElevation(FRotator::NormalizeAxis(GetControlRotation().Pitch), FloatDepth));
    const float TipOut = 34 + Free * FMath::Cos(Angle);
    const float Height = FMath::Max(0.0f, GetActorLocation().Z + 4 + Free * FMath::Sin(Angle) - LakeFish::SurfaceZ);
    return TipOut + FMath::Sqrt(FMath::Max(0.0f, MainLine * MainLine - Height * Height)) * 0.95f;
}
namespace
{
    constexpr float MinCast = 250.0f;   // nearest a pole lays the float, cm from the angler
}
FVector ALakeCharacter::CastAimPoint() const
{
    // The float goes where the angler looks: the line of sight meets the water (looking at or above
    // the horizon: as far as the rod reaches).
    const FRotator View = GetControlRotation();
    const float Pitch = FRotator::NormalizeAxis(View.Pitch);
    const float Eye = FMath::Max(50.0f, GetActorLocation().Z + 70 - LakeFish::SurfaceZ);
    const float Reach = CastReach();
    const float Look = Pitch < -1 ? Eye / FMath::Tan(FMath::DegreesToRadians(-Pitch)) : Reach;
    const float Distance = FMath::Clamp(Look, FMath::Min(MinCast, Reach), Reach);
    return FVector(GetActorLocation().X, GetActorLocation().Y, LakeFish::SurfaceZ) + FRotator(0, View.Yaw, 0).Vector() * Distance;
}
void ALakeCharacter::StartCast()
{
    bCharging = true;
    ChargeServerStart = ServerNow();
    Status = TEXT("Оснастка в руке… Дайте кончику удилища согнуться и отпустите ЛКМ — он вынесет оснастку вперёд.");
}
void ALakeCharacter::DoCast(float Held)
{
    // Let go with the tip loaded, the rig swings out and lays out straight where the angler looks.
    // Let go too soon, the rod has not bent: the rig falls short in a heap, the sooner the shorter.
    const float Severity = FMath::Clamp(1 - Held / CastLoadTime(), 0.0f, 1.0f);
    const ECastResult Result = Severity > 0 ? ECastResult::Early : ECastResult::Good;
    const FVector Facing = FRotator(0, GetControlRotation().Yaw, 0).Vector();
    const FVector Side(-Facing.Y, Facing.X, 0);
    const FVector Here(GetActorLocation().X, GetActorLocation().Y, LakeFish::SurfaceZ);
    const float Aimed = FVector::Dist2D(CastAimPoint(), Here);
    const float Distance = FMath::Lerp(Aimed, FMath::Max(MinCast, Aimed * 0.6f), Severity);
    const FVector Target = Here + Facing * Distance;
    if (!ALakeEnvironment::IsWater(Target))
    {
        Status = Result == ECastResult::Good ? TEXT("Поплавок упадёт на берег: подойдите к воде и смотрите туда, куда забросить.")
                                              : TEXT("Рано отпустили: оснастка упала на берег. ЛКМ — ещё раз.");
        return;
    }
    CastResult = Result;
    CastSeverity = Severity;
    FloatPosition = FVector(Target.X, Target.Y, LakeFish::SurfaceZ);
    // Laid out straight, the hook comes down beyond the float by most of the leader (at most
    // 1.2 m); let go too soon, it drops beside the float.
    const float Stretch = FMath::Min(FloatDepth * 0.8f, 120.0f) * FMath::Clamp(1 - Severity * 2.5f, 0.0f, 1.0f);
    CastHookLanding = Stretch > 30 ? FloatPosition + Facing * Stretch
                                   : FloatPosition + Facing * FMath::FRandRange(-10.0f, 10.0f) + Side * FMath::FRandRange(-12.0f, 12.0f);
    PlaceBait();
    FishingState = EFishingState::Waiting;
    // The rig swings out in about a second, a little longer the farther it goes and the heavier the rod.
    CastFlight = 0.5f * RodHeft() + Distance / 2200.0f;
    CastServerTime = ServerNow();
    BaitActiveTime = CastServerTime + 0.1f + CastFlight;
    Status = TEXT("Заброс!");
    // When the rig lands: laid out quietly it barely disturbs the fish, a heap splashes and scares them off.
    FTimerHandle Landing;
    GetWorldTimerManager().SetTimer(Landing, [this, Target]
    {
        if (FishingState != EFishingState::Waiting) return;
        const bool bClean = CastResult == ECastResult::Good;
        const float Bad = bClean ? 0.0f : 0.4f + 0.6f * CastSeverity;
        const FString Verdict = bClean ? TEXT("Хороший заброс: оснастка легла ровно.")
            : CastSeverity < 0.5f ? TEXT("Чуть рано отпустили: оснастка легла ближе и комом.")
            : TEXT("Рано отпустили: удилище не успело согнуться, оснастка упала комом.");
        const float Water = LakeFish::SurfaceZ - ALakeEnvironment::GroundHeight(FloatPosition.X, FloatPosition.Y);
        // The float tells the depth: with the shot (10 cm above the hook) lying on the bottom it
        // has no load and lies flat; with only the bait on the bottom it stands a little higher.
        const FString Depth = FloatDepth > Water + 10
            ? FString::Printf(TEXT("Поплавок лёг: грузило лежит на дне (глубина здесь %.0f см). Уменьшите спуск."), Water)
            : FloatDepth > Water - 5
            ? FString::Printf(TEXT("Наживка лежит на дне (глубина здесь %.0f см). Ждём поклёвку."), Water)
            : FString::Printf(TEXT("Наживка: %s на глубине %.0f см. Ждём поклёвку."), LakeFish::BaitName(Bait), FloatDepth);
        Status = Verdict + TEXT(" ") + Depth;
        const float Radius = bClean ? 100.0f : 160.0f + 120.0f * Bad;
        for (TActorIterator<ALakeFish> It(GetWorld()); It; ++It)
            if (FVector::Dist2D(It->GetActorLocation(), Target) < Radius) It->Spook(Target, bClean ? 1.5f : 2.0f + 2.5f * Bad);
        if (ALakeLife* Life = ALakeLife::Get(GetWorld()))
        {
            Life->EmitSign(ELakeSign::Rise, Target, bClean ? 0.35f : 0.6f + 0.6f * Bad, EFishSpecies::Roach, 0.1f, 0, true);
            if (FVector::Dist2D(FVector(CastHookLanding), FloatPosition) > 30) Life->EmitSign(ELakeSign::Rise, FVector(CastHookLanding), 0.25f, EFishSpecies::Roach, 0.05f, 0, true);
        }
    }, CastFlight, false);
}
void ALakeCharacter::ServerToggleRod_Implementation()
{
    if (bRodOnStand)
    {
        if (FVector::Dist2D(GetActorLocation(), StandBase) > 260) { Status = TEXT("Подойдите к удочке, чтобы взять её."); return; }
        bRodOnStand = false;
        Status = FishingState != EFishingState::Idle ? TEXT("Удочка в руках.") : TEXT("Удочка в руках. Держите ЛКМ — оснастка в руке, отпустите — заброс.");
        return;
    }
    if (FishingState >= EFishingState::Bite || bCharging || bRetrieving) return;
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
        if (FishingState == EFishingState::Bite && BaitFish.IsValid())
        {
            const FVector Mouth = BaitFish->MouthLocation();
            FloatPosition = FMath::VInterpTo(FloatPosition, FVector(Mouth.X, Mouth.Y, LakeFish::SurfaceZ), DeltaSeconds, 2);
        }
        LineFish = FishingState >= EFishingState::Fight ? HookedFish.Get() : FishingState == EFishingState::Bite ? BaitFish.Get() : nullptr;
    }
    if (FishingState != EFishingState::Fight || !HasAuthority()) UpdateRodPose(DeltaSeconds);
    if (HasAuthority() && (FishingState == EFishingState::Waiting || FishingState == EFishingState::Bite) && ServerNow() >= BaitActiveTime + 1.0f) ConstrainFloat();
    if (HasAuthority() && FishingState == EFishingState::Fight) UpdateFight(DeltaSeconds);
    if (HasAuthority() && bRetrieving) UpdateRetrieve();
    if (HasAuthority() && FishingState == EFishingState::Landing) UpdateLanding();
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
void ALakeCharacter::RodPoseTarget(float Pitch, float Now, float& OutElevation, FVector& OutGrip, float& OutStiffness, float& OutYaw) const
{
    // Right hand position relative to the body (X forward, Y right, Z up from the hips) and the
    // rod's elevation above the horizon for each phase. With the line as long as the rod, a pole
    // is carried almost upright so the float rig hangs clear of the ground.
    const FVector Idle(24, 20, 14), Forward(40, 16, 12), Waiting(34, 18, 4);
    // Casting from the hand: the rod nearly upright, the left hand holding the rig at the chest
    // (with the line as long as the rod, the rig cannot come lower).
    const FVector Hand(28, 22, 20);
    constexpr float HandElevation = 80;
    float Elevation = IdleElevation(Pitch);
    FVector Grip = Idle;
    float Stiffness = 7;                       // rad/s: a long rod has weight and lags behind
    const float Yaw = 7;                       // the rod a little to the right of the facing
    if (bCharging)
    {
        Elevation = HandElevation;
        Grip = Hand;
        Stiffness = 5;
    }
    else if (FishingState != EFishingState::Idle)
    {
        const float Since = Now - CastServerTime;
        Elevation = WaitElevation(Pitch, HookDepth);
        Grip = Waiting;
        if (Since >= 0 && Since < 1.5f) Stiffness = 3.5f;   // after the cast the heavy pole settles slowly
        const float Push = 0.3f * RodHeft(), Settle = CastFlight + 0.4f;
        if (Since >= 0 && Since < Settle && !bRetrieving && FishingState == EFishingState::Waiting)
        {
            // Let go: the rod is pushed forward and down at once, the loaded tip throwing the rig
            // out ahead; then it follows the rig down to the waiting angle as it lands.
            constexpr float PushElevation = 38;
            if (Since < Push)
            {
                const float A = FMath::InterpEaseOut(0.0f, 1.0f, Since / Push, 2.0f);
                Elevation = FMath::Lerp(HandElevation, PushElevation, A);
                Grip = FMath::Lerp(Hand, Forward, A);
                Stiffness = 14;
            }
            else
            {
                const float A = FMath::SmoothStep(Push, Settle, Since);
                Elevation = FMath::Lerp(PushElevation, Elevation, A);   // Elevation is the waiting angle here
                Grip = FMath::Lerp(Forward, Waiting, A);
                Stiffness = 7;
            }
        }
        if (bRetrieving)
        {
            // Drawing the rig out: the rod is raised steadily towards the angler, never jerked.
            const float R = FMath::Clamp((Now - RetrieveServerTime) / RetrieveTime, 0.0f, 1.0f);
            const float A = FMath::InterpEaseInOut(0.0f, 1.0f, R, 2.0f);
            Elevation = FMath::Lerp(RetrieveFromElevation, IdleElevation(Pitch), A);
            Grip = FMath::Lerp(Waiting, Idle, A);
            Stiffness = 9;
        }
        if (FishingState == EFishingState::Fight)
        {
            // No reel: holding LMB lifts the rod and draws the fish in, releasing lowers it
            // towards the fish to give line. A strong pull drags the tip down.
            Elevation = FMath::Lerp(22.0f, 80.0f, RodLift) - Tension * 15;
            Grip = FMath::Lerp(FVector(36, 18, 8), FVector(20, 18, 24), RodLift);
            Stiffness = 6;
        }
        if (FishingState == EFishingState::Landing)
        {
            // Lifting the fish out: the rod goes up almost upright and the hands come up and back,
            // so the fish leaves the water and swings in on the line towards the angler.
            const float L = FMath::Clamp((Now - LandServerTime) / (LandTime * 0.3f), 0.0f, 1.0f);
            const float A = FMath::InterpEaseInOut(0.0f, 1.0f, L, 2.0f);
            Elevation = FMath::Lerp(FMath::Lerp(22.0f, 80.0f, RodLift), 87.0f, A);
            Grip = FMath::Lerp(FMath::Lerp(FVector(36, 18, 8), FVector(20, 18, 24), RodLift), FVector(2, 18, 36), A);
            Stiffness = 6;
        }
        const float SinceStrike = Now - StrikeServerTime;
        if (SinceStrike >= 0 && SinceStrike < 0.3f) { Elevation += 15 * FMath::Sin(PI * SinceStrike / 0.3f); Stiffness = 20; }
    }
    OutElevation = Elevation;
    OutGrip = Grip;
    // A heavier rod has more inertia: it follows the hands more slowly and smoothly.
    OutStiffness = Stiffness / RodHeft();
    OutYaw = Yaw;
}
void ALakeCharacter::UpdateRodPose(float DeltaSeconds)
{
    // Critically damped springs towards the target pose: the rod has weight, so it follows the
    // hands and the body's turns with a little lag, except during the quick casting swing.
    const FRotator View = IsLocallyControlled() && GetController() ? GetControlRotation() : GetBaseAimRotation();
    float TargetElevation, Stiffness, YawOffset;
    FVector TargetGrip;
    RodPoseTarget(FRotator::NormalizeAxis(View.Pitch), ServerNow(), TargetElevation, TargetGrip, Stiffness, YawOffset);
    const float TargetYaw = GetActorRotation().Yaw + YawOffset;
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
    if (FishingState == EFishingState::Fight && !bRodOnStand)
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
    In.SinceCast = FishingState != EFishingState::Idle && CastServerTime > 0 ? Now - CastServerTime : -1;
    In.CastFlight = CastFlight;
    In.bSwinging = bCharging;
    In.HookLanding = CastHookLanding;
    In.Facing = FRotator(0, GetActorRotation().Yaw, 0).Vector();
    In.Tension = Tension;
    In.Nibbles = NibbleCounter;
    In.BiteMotion = BiteMotion;
    In.BiteElapsed = Now - BiteStart;
    In.BiteDuration = BiteDuration;
    In.BiteAmount = BiteAmount;
    In.bReeling = false;
    In.bOnStand = bRodOnStand || StandAlpha > 0;
    In.StandBase = StandBase;
    In.StandYaw = StandYaw;
    In.LineFish = LineFish;
    // The left hand and the rig: it catches the line as the rig comes out of the water, keeps the
    // rig while the angler is ready to cast, draws it back to the hip for the cast (bending the rod
    // tip) and lets go on release.
    const float SinceCast = Now - CastServerTime;
    float HookHold = 0;
    if (!bRodOnStand && StandAlpha <= 0)
    {
        if (bCharging || FishingState == EFishingState::Idle) HookHold = 1;
        else if (FishingState == EFishingState::Waiting && bRetrieving)
        {
            const float R = (Now - RetrieveServerTime) / RetrieveTime;
            HookHold = R >= 0.95f ? 1.0f : 0.99f * FMath::SmoothStep(0.45f, 0.95f, R);
        }
        else if (FishingState == EFishingState::Waiting && SinceCast >= 0 && SinceCast < 0.2f) HookHold = 1 - SinceCast / 0.2f;
    }
    In.HookHold = HookHold;
    In.RetrieveT = bRetrieving ? FMath::Clamp((Now - RetrieveServerTime) / RetrieveTime, 0.0f, 1.0f) : -1.0f;
    In.RodHeft = RodHeft();
    const float Draw = FMath::SmoothStep(0.0f, CastLoadTime(), SwingTime(Now));
    In.HandPoint = GetActorLocation() + GetActorForwardVector() * FMath::Lerp(40.0f, 12.0f, Draw) - GetActorRightVector() * 22 + FVector(0, 0, FMath::Lerp(45.0f, 30.0f, Draw));
    const float LandAlpha = FishingState == EFishingState::Landing ? FMath::SmoothStep(0.55f, 0.85f, (Now - LandServerTime) / LandTime) : 0.0f;
    if (FishingState == EFishingState::Landing && LineFish)
    {
        In.FishGrams = LineFish->WeightKg * 1000;
        In.FishLength = LakeFish::LengthCm(LineFish->Species, LineFish->WeightKg);
        In.HandPoint = GetActorLocation() + GetActorForwardVector() * 45 - GetActorRightVector() * 14 + FVector(0, 0, 22);
        In.HandAlpha = LandAlpha;
    }
    RodVisual->UpdateRod(In, DeltaSeconds);
    // A fish lifted out hangs on the hook wherever the rod's line has swung it.
    if (FishingState == EFishingState::Landing && LineFish) LineFish->HangOnLine(RodVisual->HookLocation(), RodVisual->HookLineUp(), DeltaSeconds);

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
        // The free hand: on the fish's mouth when it is lifted out, otherwise on the shot it holds.
        Pose.Left.Point = FMath::Lerp(Hold.TransformPosition(FVector(5, 0, 0)), LandAlpha > 0 ? RodVisual->HookLocation() : RodVisual->ShotLocation(), FMath::Max(LandAlpha, HookHold));
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
    else if (ALakeFish* Holder = BaitFish.Get(); Holder && FishingState == EFishingState::Bite) Holder->Release(10);
    HookedFish = nullptr;
    BaitFish = nullptr;
    LineFish = nullptr;
    BiteMotion = EBiteMotion::None;
    bCharging = false;
    bRetrieving = false;
    FishingState = EFishingState::Idle;
    Tension = 0;
    bReeling = false;
    Status = Message;
    ForceNetUpdate();
}
void ALakeCharacter::StartRetrieve(const FString& Message, const FString& EndMessage)
{
    if (bRetrieving || FishingState == EFishingState::Idle) return;
    // Whatever was on the line swims off; the rig is then simply lifted out by raising the rod,
    // the line drawing it across the water towards the angler.
    GetWorldTimerManager().ClearTimer(FishingTimer);
    if (ALakeFish* Fish = HookedFish.Get()) Fish->Release(12);
    else if (ALakeFish* Holder = BaitFish.Get(); Holder && FishingState == EFishingState::Bite) Holder->Release(10);
    HookedFish = nullptr;
    BaitFish = nullptr;
    LineFish = nullptr;
    BiteMotion = EBiteMotion::None;
    FishingState = EFishingState::Waiting;
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
    if (FishingState != EFishingState::Fight) return;
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
    // Ready to come out: tired, near the surface and within the line's reach of the rod tip (or led
    // into the shallows by the bank). Lifting the rod (LMB) then draws it out of the water.
    const FVector Mouth = Fish->MouthLocation();
    const bool bReady = Fish->StaminaFraction() < 0.35f &&
        ((FVector::Dist2D(Tip, Mouth) < ALakeRod::LineLength * 0.8f && Mouth.Z > LakeFish::SurfaceZ - 70) ||
         (LakeMap::ShoreDistance(Mouth.X, Mouth.Y) < 300 && FishDistance < 700));
    // A short hint as the fight goes: the fish runs, holds, is tiring or can be lifted out.
    const int32 Hint = Tension > 0.75f ? 1 : bReady ? 3 : Fish->StaminaFraction() < 0.35f ? 2 : 0;
    if (Hint != FightHint)
    {
        FightHint = Hint;
        Status = Hint == 1 ? TEXT("Рыба тянет! Отпустите ЛКМ, дайте удилищу пружинить.")
               : Hint == 2 ? TEXT("Рыба устала. Подводите её к себе, к поверхности.")
               : Hint == 3 ? TEXT("Рыба у поверхности. Держите ЛКМ — поднимите её из воды.")
               : TEXT("Рыба водит. Держите ЛКМ — поднимать удилище, отпускайте на рывках.");
    }
    if (Force >= BreakForce) FinishFishing(TEXT("Леска оборвалась! Чаще отпускайте ЛКМ. ЛКМ — новый заброс."));
    else if (FightTime >= 120) FinishFishing(TEXT("Рыба сошла: вываживание затянулось. ЛКМ — новый заброс."));
    else if (bReady && bReeling) StartLanding(Fish);
}
void ALakeCharacter::StartLanding(ALakeFish* Fish)
{
    // The rod goes up; the fish comes out of the water with a splash and swings in on the line.
    FishingState = EFishingState::Landing;
    LandServerTime = ServerNow();
    Tension = 0;
    LineFish = Fish;
    Fish->LiftOut();
    const FVector P = Fish->GetActorLocation();
    if (ALakeLife* Life = ALakeLife::Get(GetWorld()))
        Life->EmitSign(ELakeSign::Splash, FVector(P.X, P.Y, LakeFish::SurfaceZ), FMath::Min(0.4f + Fish->WeightKg * 0.5f, 1.4f), Fish->Species, Fish->WeightKg, 0, true);
    Status = TEXT("Поднимаете рыбу из воды…");
    ForceNetUpdate();
}
void ALakeCharacter::UpdateLanding()
{
    // In the hand: the catch counts, the fish goes into the keepnet and the rig hangs from the tip.
    if (ServerNow() - LandServerTime < LandTime) return;
    ALakeFish* Fish = HookedFish.Get();
    if (!Fish) { FinishFishing(TEXT("Рыба сошла. ЛКМ — новый заброс.")); return; }
    ++CatchCount;
    const FFishSpeciesInfo& S = LakeFish::Info(Fish->Species);
    const FString Message = FString::Printf(TEXT("%s, %.2f кг (%.0f см)! ЛКМ — забросить снова."), S.Name, Fish->WeightKg, LakeFish::LengthCm(Fish->Species, Fish->WeightKg));
    const EFishSpecies Species = Fish->Species;
    HookedFish = nullptr;
    LineFish = nullptr;
    Fish->Destroy();
    for (TActorIterator<ALakeEnvironment> It(GetWorld()); It; ++It) It->ScheduleRespawn(Species);
    FinishFishing(Message);
}
void ALakeCharacter::ServerFish_Implementation()
{
    if (FishingState == EFishingState::Fight) { bReeling = true; return; }
    if (bRetrieving || FishingState == EFishingState::Landing) return;
    const double Now = GetWorld()->GetTimeSeconds();
    if (Now < NextActionTime) return;
    NextActionTime = Now + 0.3;
    if (FishingState == EFishingState::Idle)
    {
        if (bRodOnStand) { Status = TEXT("Сначала возьмите удочку: R."); return; }
        if (bCharging) return;
        StartCast();
    }
    else if (FishingState == EFishingState::Waiting)
    {
        if (ServerNow() < BaitActiveTime || !TakeRodFromStand()) return;
        if (ALakeFish* Fish = BaitFish.Get()) Fish->Spook(BaitPoint, 4);
        StrikeServerTime = ServerNow();
        StartRetrieve(TEXT("Рано подсекли! Рыба только пробовала наживку: ждите подъёма, увода или утопления поплавка."),
                      TEXT("Рано подсекли! Ждите подъёма, увода или утопления поплавка. ЛКМ — забросить снова."));
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
        // Struck as the float only starts to rise, the fish may hold the bait by its lips alone;
        // once it lies down, is led off or goes under, the hook goes home.
        const float HookChance = BiteMotion != EBiteMotion::Lift ? 1.0f
            : BiteAmount >= 2.5f && ServerNow() - BiteStart > BiteDuration * 0.55f ? 0.95f : 0.7f;
        if (FMath::FRand() > HookChance)
        {
            StartRetrieve(TEXT("Подсекли, но рыба не засеклась: она едва приподняла наживку."),
                          TEXT("Рыба не засеклась. Подсекайте, когда поплавок ляжет, пойдёт в сторону или утонет. ЛКМ — заброс."));
            return;
        }
        BiteMotion = EBiteMotion::None;
        Fish->Hook(this);
        HookedFish = Fish;
        BaitFish = nullptr;
        FishingState = EFishingState::Fight;
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
    if (FishingState == EFishingState::Idle || FishingState == EFishingState::Landing || bRetrieving || (FishingState == EFishingState::Waiting && ServerNow() < BaitActiveTime)) return;
    if (!TakeRodFromStand()) return;
    StartRetrieve(TEXT("Поднимаете удилище — снасть выходит из воды…"), TEXT("Снасть убрана. ЛКМ — забросить."));
}
bool ALakeCharacter::ClaimBait(ALakeFish* Fish)
{
    if (FishingState != EFishingState::Waiting || bRetrieving) return false;
    if (BaitFish.IsValid() && BaitFish.Get() != Fish) return false;
    BaitFish = Fish;
    return true;
}
void ALakeCharacter::ReleaseBaitClaim(ALakeFish* Fish)
{
    if (BaitFish.Get() == Fish && FishingState == EFishingState::Waiting) BaitFish = nullptr;
}
void ALakeCharacter::OnFishNibble(ALakeFish* Fish)
{
    if (FishingState != EFishingState::Waiting || bRetrieving) return;
    ++NibbleCounter;
    Status = TEXT("Поплавок вздрагивает — рыба пробует наживку. Ждите…");
    // Each tremble of the float sends out a faint ring.
    if (ALakeLife* Life = ALakeLife::Get(GetWorld())) Life->EmitSign(ELakeSign::Rise, FloatPosition, 0.22f, Fish ? Fish->Species : EFishSpecies::Roach, 0.1f, 0, true);
}
void ALakeCharacter::OnBiteMotion(ALakeFish* Fish, const FBiteStep& Step)
{
    if ((FishingState != EFishingState::Waiting && FishingState != EFishingState::Bite) || bRetrieving) return;
    BiteMotion = Step.Motion;
    BiteStart = ServerNow();
    BiteDuration = Step.Duration;
    BiteAmount = Step.Amount;
    if (Step.Motion != EBiteMotion::None) Status = LakeFish::BiteText(Step.Motion, Step.Amount);
    // Tries at the bait send out faint rings.
    if (Step.Motion == EBiteMotion::Tremble || Step.Motion == EBiteMotion::Peck)
    {
        ++NibbleCounter;
        if (ALakeLife* Life = ALakeLife::Get(GetWorld()))
            Life->EmitSign(ELakeSign::Rise, FloatPosition, Step.Motion == EBiteMotion::Peck ? 0.3f : 0.18f, Fish ? Fish->Species : EFishSpecies::Roach, 0.1f, 0, true);
    }
    ForceNetUpdate();
}
void ALakeCharacter::OnFishTake(ALakeFish* Fish, float Window)
{
    if (FishingState != EFishingState::Waiting || bRetrieving) return;
    BaitFish = Fish;
    FishingState = EFishingState::Bite;
    Status = TEXT("ПОКЛЁВКА! Поплавок ушёл под воду — нажмите ЛКМ, подсекайте!");
    // Cautious fish hold the bait a little longer before spitting it out.
    // A hungry fish takes the bait boldly and holds it longer; a wary one drops it quickly.
    if (Window < 0)
        Window = (Fish && Fish->Species == EFishSpecies::Crucian ? 2.6f : Fish && Fish->Species == EFishSpecies::Pike ? 1.5f : 2.0f)
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
    if (FishingState != EFishingState::Idle || bCharging) { Status = TEXT("Глубину можно менять только до заброса: сначала вытащите снасть (ПКМ)."); return; }
    FloatDepth = FMath::Clamp(FMath::RoundToFloat(FloatDepth + Delta), MinDepth, MaxDepth);
    HookDepth = FloatDepth;      // the float slides along the hanging rig right away
    Status = FString::Printf(TEXT("Глубина спуска наживки: %.0f см."), FloatDepth);
}
void ALakeCharacter::Escape()
{
    if (FishingState != EFishingState::Bite) return;
    // Not struck in time: the fish lets go of the bait and swims off. The rig stays where the fish
    // left it and keeps fishing: the float stands up again, and the angler may wait or recast (RMB).
    if (ALakeFish* Holder = BaitFish.Get()) Holder->DropBait();
    BaitFish = nullptr;
    LineFish = nullptr;
    BiteMotion = EBiteMotion::None;
    FishingState = EFishingState::Waiting;
    PlaceBait();
    Status = TEXT("Рыба бросила наживку. Поплавок встал на место — ждите новой поклёвки или перезабросьте (ПКМ).");
    ForceNetUpdate();
}
void ALakeCharacter::SelectBait(EBait NewBait) { ServerSelectBait(NewBait); }
void ALakeCharacter::ServerSelectBait_Implementation(EBait NewBait)
{
    if (FishingState != EFishingState::Idle || NewBait >= EBait::Count) return;
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
    DOREPLIFETIME(ALakeCharacter, BiteMotion);
    DOREPLIFETIME(ALakeCharacter, LandServerTime);
    DOREPLIFETIME(ALakeCharacter, BiteStart);
    DOREPLIFETIME(ALakeCharacter, BiteDuration);
    DOREPLIFETIME(ALakeCharacter, BiteAmount);
    DOREPLIFETIME(ALakeCharacter, bCharging);
    DOREPLIFETIME(ALakeCharacter, ChargeServerStart);
    DOREPLIFETIME(ALakeCharacter, bRetrieving);
    DOREPLIFETIME(ALakeCharacter, RetrieveServerTime);
    DOREPLIFETIME(ALakeCharacter, RetrieveFromElevation);
    DOREPLIFETIME(ALakeCharacter, CastServerTime);
    DOREPLIFETIME(ALakeCharacter, CastFlight);
    DOREPLIFETIME(ALakeCharacter, CastResult);
    DOREPLIFETIME(ALakeCharacter, RodKind);
    DOREPLIFETIME(ALakeCharacter, CastSeverity);
    DOREPLIFETIME(ALakeCharacter, CastHookLanding);
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
ALakeGameMode::ALakeGameMode()
{
    DefaultPawnClass = ALakeCharacter::StaticClass();
    HUDClass = ALakeHUD::StaticClass();
    PlayerControllerClass = ALakePlayerController::StaticClass();
}
void ALakeGameMode::BeginPlay()
{
    Super::BeginPlay();
    GetWorld()->SpawnActor<ALakeLife>();
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

