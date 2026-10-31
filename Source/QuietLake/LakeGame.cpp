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
#include "HAL/IConsoleManager.h"

// The camera's pitch limits while casting (console: lake.CastCameraMin -25), so they can be tuned live.
static TAutoConsoleVariable<float> CVarCastCameraMin(TEXT("lake.CastCameraMin"), -20.0f,
    TEXT("Lowest camera pitch while casting, degrees; the rod goes on lower without the camera."));
static TAutoConsoleVariable<float> CVarCastCameraMax(TEXT("lake.CastCameraMax"), 15.0f,
    TEXT("Highest camera pitch while casting, degrees; the rod goes on higher without the camera."));
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
    // The rod is simulated on every machine, a dedicated server included: the server takes where a
    // cast lands and when the hand catches the rig from its own run of it.
    RodVisual = GetWorld()->SpawnActor<ALakeRod>();
    if (RodVisual) RodVisual->AddTickPrerequisiteActor(this);
#if !UE_BUILD_SHIPPING
    RunPreviews();
#endif
}
void ALakeCharacter::SetupPlayerInputComponent(UInputComponent* Input)
{
    Super::SetupPlayerInputComponent(Input);
    Input->BindAxis("Forward", this, &ALakeCharacter::Forward);
    Input->BindAxis("Right", this, &ALakeCharacter::Right);
    Input->BindAxis("Turn", this, &ALakeCharacter::Turn);
    Input->BindAxis("Look", this, &ALakeCharacter::Look);
    Input->BindAction("Fish", IE_Pressed, this, &ALakeCharacter::Fish);
    Input->BindAction("Fish", IE_Released, this, &ALakeCharacter::Release);
    Input->BindAction("Cancel", IE_Pressed, this, &ALakeCharacter::Cancel);
    Input->BindAction("Cancel", IE_Released, this, &ALakeCharacter::CancelReleased);
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
    Input->BindAction("Groundbait", IE_Pressed, this, &ALakeCharacter::ThrowGroundbait);
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
// CharacterMovement keeps normal walking and network prediction in every fishing phase.
void ALakeCharacter::Forward(float Value) { AddMovementInput(GetActorForwardVector(), Value); }
void ALakeCharacter::Right(float Value) { AddMovementInput(GetActorRightVector(), Value); }
void ALakeCharacter::Fish()
{
    ServerFish();
}
void ALakeCharacter::Turn(float Value)
{
    MoveRodAxis(Value * RodMouseSensitivity, false);
}
void ALakeCharacter::Look(float Value)
{
    MoveRodAxis(-Value * RodMouseSensitivity, true);
}
void ALakeCharacter::MoveRodAxis(float Delta, bool bPitch)
{
    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC || PC->IsLookInputIgnored() || !FMath::IsFinite(Delta) || Delta == 0) return;
    if (IsSteeringRig())
    {
        // The rig or the fish swings in on the line while the rod rises by itself: the camera moves
        // as usual. Aside the angler turns and the rod with him (the rig's swing follows it); up and
        // down the rod is also lifted a little more or less (the rig comes in closer or farther).
        const float D = FMath::Clamp(Delta, -720.0f, 720.0f);
        FRotator View = PC->GetControlRotation();
        if (bPitch)
        {
            View.Pitch = FMath::Clamp(FRotator::NormalizeAxis(View.Pitch) + D, -80.0f, 80.0f);
            RigSteerPitch = FMath::Clamp(RigSteerPitch + D * 0.5f, -RigSteerPitchLimit, RigSteerPitchLimit);
            if (HasAuthority()) ForceNetUpdate();
        }
        else View.Yaw = FRotator::NormalizeAxis(View.Yaw + D);
        PC->SetControlRotation(View);
        return;
    }
    if (bPitch && !bRodOnStand)
    {
        // Up and down the camera goes with the rod (the rod's angle includes the view's pitch).
        // Casting (LMB held, or the rig in flight) the camera stays between lake.CastCameraMin and
        // lake.CastCameraMax, so the water stays in view, and the rod alone goes on, down to the
        // ground or up to vertical; turned back, the rod first returns to the camera, then the
        // camera follows it again. Otherwise the camera only stops at its own limits.
        float D = FMath::Clamp(Delta, -720.0f, 720.0f);
        // Move Value by D, but not past [Lo, Hi] (never further out if it already is): D keeps the rest.
        auto Take = [&D](float& Value, float Lo, float Hi)
        {
            const float New = FMath::Clamp(Value + D, FMath::Min(Value, Lo), FMath::Max(Value, Hi));
            D -= New - Value;
            Value = New;
        };
        const bool bCasting = bCharging || bRigFlying;
        const float CamLo = bCasting ? FMath::Clamp(CVarCastCameraMin.GetValueOnGameThread(), -80.0f, 80.0f) : -80.0f;
        const float CamHi = bCasting ? FMath::Clamp(CVarCastCameraMax.GetValueOnGameThread(), CamLo, 80.0f) : 80.0f;
        constexpr float Any = 1000;
        FRotator View = PC->GetControlRotation();
        float Pitch = FMath::Clamp(FRotator::NormalizeAxis(View.Pitch), -80.0f, 80.0f);
        if (D < 0)
        {
            Take(SwingPitch, 0, Any);
            Take(Pitch, CamLo, 80);
            Take(SwingPitch, RodLowestAngle - RodBase() - Pitch, Any);
        }
        else
        {
            Take(SwingPitch, -Any, 0);
            Take(Pitch, -80, CamHi);
            Take(SwingPitch, -Any, RodHighestAngle - RodBase() - Pitch);
        }
        View.Pitch = Pitch;
        PC->SetControlRotation(View);
        return;
    }
    // Aside (and on the stand, also up and down) the camera turns with the rod: the rod keeps its
    // place beside the view (SwingYaw), so the whole mouse motion turns the view.
    FRotator View = PC->GetControlRotation();
    const float D = FMath::Clamp(Delta, -720.0f, 720.0f);
    if (bPitch) View.Pitch = FMath::Clamp(FRotator::NormalizeAxis(View.Pitch) + D, -80.0f, 80.0f);
    else View.Yaw = FRotator::NormalizeAxis(View.Yaw + D);
    PC->SetControlRotation(View);
}
void ALakeCharacter::ServerSwing_Implementation(float Pitch, float Yaw)
{
    if (!FMath::IsFinite(Pitch) || !FMath::IsFinite(Yaw)) return;
    SwingPitch = FMath::Clamp(Pitch, -RodPitchDownLimit, RodPitchUpLimit);
    SwingYaw = FMath::Clamp(Yaw, -RodYawLimit, RodYawLimit);
}
void ALakeCharacter::ServerSteer_Implementation(float Pitch)
{
    if (!FMath::IsFinite(Pitch)) return;
    RigSteerPitch = FMath::Clamp(Pitch, -RigSteerPitchLimit, RigSteerPitchLimit);
}
bool ALakeCharacter::IsSteeringRig() const
{
    if (!RodVisual || bRodOnStand || bCharging) return false;
    return bRetrieving || FishingState == EFishingState::Landing || (FishingState == EFishingState::Idle && !bRigInHand);
}
void ALakeCharacter::Cancel() { ServerCancel(); }
void ALakeCharacter::CancelReleased() { ServerStopRetrieve(); }
void ALakeCharacter::ServerStopRetrieve_Implementation() { bRetrieveHeld = false; }
void ALakeCharacter::Release() { ServerReleaseWithAim(SwingPitch, SwingYaw); }
void ALakeCharacter::ServerReleaseWithAim_Implementation(float Pitch, float Yaw)
{
    // The last aim cannot be lost or reordered behind release on an unreliable channel.
    ServerSwing_Implementation(Pitch, Yaw);
    ServerRelease_Implementation();
}
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
bool ALakeCharacter::HasBaitInWater() const { return FishingState == EFishingState::Waiting && !bRetrieving && !bRigFlying && ServerNow() >= BaitActiveTime; }
FVector ALakeCharacter::HandPoint(float Draw) const
{
    // A closed hand carries its contact point; it never pulls a distant rig towards itself.
    const FVector Rest(46, -20, 22);
    const FVector Local = bRigInHand ? FMath::Lerp(RigGripLocal, Rest,
        FMath::SmoothStep(0.0f, 0.8f, ServerNow() - RigGripTime)) : Rest;
    return GetActorLocation() + GetActorForwardVector() * Local.X + GetActorRightVector() * Local.Y + FVector(0, 0, Local.Z);
}
FVector ALakeCharacter::LeftShoulder() const
{
    // The mannequin's left upper arm joint (a fixed point: a dedicated server does not animate the mesh).
    return GetActorTransform().TransformPosition(FVector(10, -17, 52));
}
float ALakeCharacter::RigReach() const
{
    // The rig swinging in on the line, in front of the body: the hand goes out to meet it as it comes
    // within an arm and a half, and is on it at arm's length.
    if (!RodVisual || bRigInHand || bRodOnStand || bCharging || !(FishingState == EFishingState::Idle || bRetrieving)) return 0;
    const FVector Shot = RodVisual->ShotLocation();
    if (FVector::DotProduct(Shot - GetActorLocation(), GetActorForwardVector()) < -5) return 0;
    return 1 - FMath::SmoothStep(ArmReach - 4, ArmReach + 55, (float)FVector::Dist(Shot, LeftShoulder()));
}
void ALakeCharacter::ServerRelease_Implementation()
{
    bReeling = false;
    if (bRetrieving && bRetrieveHeld && !bRigInHand)
        Status = TEXT("Удилище остановлено. Держите ЛКМ — поднимать дальше.");
    bRetrieveHeld = false;
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
void ALakeCharacter::StartCast()
{
    bCharging = true;
    ChargeServerStart = ServerNow();
    HookDepth = FloatDepth;
    Status = TEXT("Держите ЛКМ — маятниковый мах. Отпустите — опустить снасть в воду.");
    ForceNetUpdate();
}
void ALakeCharacter::DoCast(float Held)
{
    // Release only lowers the rod. The rig retains the momentum acquired during the held stroke.
    CastResult = ECastResult::Good;
    CastSeverity = 0;
    FishingState = EFishingState::Waiting;
    CastDropElevation = RodElevation;
    CastDropGrip = RodGrip;
    CastFlight = 0;
    CastServerTime = ServerNow();
    Status = TEXT("Оснастка опускается в воду…");
    bRigInHand = false;
    bRigFlying = true;
    BaitActiveTime = CastServerTime + 1000;
    ForceNetUpdate();
}
void ALakeCharacter::OnRigLanded(FVector Where)
{
    bRigFlying = false;
    if (RodVisual)
    {
        CastFlight = FMath::Max(0.0f, ServerNow() - CastServerTime);
        CastHookLanding = RodVisual->HookLocation();
    }
    if (!ALakeEnvironment::IsWater(Where))
    {
        StartRetrieve(TEXT("Оснастка упала на берег. Поднимите её и забросьте ещё раз."), TEXT("Снасть убрана. ЛКМ — забросить."), true);
        return;
    }
    const FVector Target(Where.X, Where.Y, LakeFish::SurfaceZ);
    FloatPosition = Target;
    PlaceBait();
    BaitActiveTime = FMath::Min(BaitActiveTime, ServerNow() + 0.1f);
    // Laid out quietly the rig barely disturbs the fish; let go too soon it comes down in a heap and splashes.
    const bool bClean = CastResult == ECastResult::Good;
    const float Bad = bClean ? 0.0f : 0.4f + 0.6f * CastSeverity;
    const FString Verdict = RodVisual ? FString::Printf(TEXT("Оснастка легла в %.1f м."), FVector::Dist2D(Target, GetActorLocation()) / 100)
        : bClean ? FString(TEXT("Хороший заброс: оснастка легла ровно."))
        : CastSeverity < 0.5f ? FString(TEXT("Чуть рано отпустили: оснастка легла ближе и комом."))
        : FString(TEXT("Рано отпустили: удилище не успело согнуться, оснастка упала комом."));
    const float Water = LakeFish::SurfaceZ - ALakeEnvironment::GroundHeight(FloatPosition.X, FloatPosition.Y);
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_LANDED %.0f cm from the angler (who stands %.0f cm from the water), %.0f cm from the bank, water %.0f cm, float set %.0f cm"),
        FVector::Dist2D(Target, GetActorLocation()), -LakeMap::ShoreDistance(GetActorLocation().X, GetActorLocation().Y),
        LakeMap::ShoreDistance(Target.X, Target.Y), Water, FloatDepth);
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
        // Further off the fish only turn away from the plop, the more so the clumsier the cast.
        Life->Disturb(Target, Radius * 3, bClean ? 0.4f : 0.55f + 0.35f * Bad);
        Life->AddPressure(Target, bClean ? 0.03f : 0.1f);
        Life->EmitSign(ELakeSign::Rise, Target, bClean ? 0.35f : 0.6f + 0.6f * Bad, EFishSpecies::Roach, 0.1f, 0, true);
        const FVector Hook = RodVisual ? RodVisual->HookLocation() : FVector(CastHookLanding);
        if (ALakeEnvironment::IsWater(Hook) && FVector::Dist2D(Hook, Target) > 30) Life->EmitSign(ELakeSign::Rise, FVector(Hook.X, Hook.Y, LakeFish::SurfaceZ), 0.25f, EFishSpecies::Roach, 0.05f, 0, true);
    }
    ForceNetUpdate();
}
void ALakeCharacter::ThrowGroundbait() { ServerThrowGroundbait(GroundbaitTarget()); }

FVector ALakeCharacter::GroundbaitTarget() const
{
    // To the float when the rig is in the water; otherwise where the angler looks, as far as a ball goes by hand.
    FVector Target = FloatPosition;
    if (FishingState == EFishingState::Idle || bRigFlying || !LakeMap::IsWater(Target.X, Target.Y))
    {
        FVector Eye = GetActorLocation();
        FRotator View = GetControlRotation();
        if (Controller) Controller->GetPlayerViewPoint(Eye, View);
        const FVector Dir = View.Vector();
        float Ahead = 600;
        if (Dir.Z < -0.02f) Ahead = FMath::Clamp((Eye.Z - LakeFish::SurfaceZ) / -Dir.Z * (float)Dir.Size2D(), 200.0f, 1200.0f);
        Target = GetActorLocation() + Dir.GetSafeNormal2D() * Ahead;
    }
    return Target;
}

void ALakeCharacter::ServerThrowGroundbait_Implementation(FVector_NetQuantize Target)
{
    if (GroundbaitLeft <= 0) { Status = TEXT("Прикормка кончилась."); return; }
    if (ServerNow() < NextGroundbait) return;
    ALakeLife* Life = ALakeLife::Get(GetWorld());
    if (!Life) return;
    // By hand a ball goes up to about 12 m, a little off the mark (the further, the wider).
    const FVector2D Here(GetActorLocation());
    FVector2D To(Target);
    if (FVector2D::Distance(To, Here) > 1200) To = Here + (To - Here).GetSafeNormal() * 1200;
    const float Spread = 25 + 0.04f * FVector2D::Distance(To, Here);
    To += FVector2D(FMath::FRandRange(-Spread, Spread), FMath::FRandRange(-Spread, Spread));
    if (!LakeMap::IsWater(To.X, To.Y)) { Status = TEXT("Туда прикормку не бросить: там не вода."); return; }
    NextGroundbait = ServerNow() + 1.2f;
    --GroundbaitLeft;
    const FVector Facing = FRotator(0, GetControlRotation().Yaw, 0).Vector();
    Life->ThrowGroundbait(GetActorLocation() + FVector(0, 0, 55) + Facing * 30, FVector(To, LakeFish::SurfaceZ), Bait);
    static const TCHAR* With[] = { TEXT("с червём"), TEXT("с опарышем"), TEXT("с хлебом") };
    Status = FString::Printf(TEXT("Бросили шар прикормки %s. Рыба подойдёт на запах через минуту-другую. Шаров осталось: %d."),
        With[FMath::Clamp((int32)Bait, 0, 2)], GroundbaitLeft);
}

void ALakeCharacter::ServerToggleRod_Implementation()
{
    if (bRodOnStand)
    {
        if (FVector::Dist2D(GetActorLocation(), StandBase) > 260) { Status = TEXT("Подойдите к удочке, чтобы взять её."); return; }
        bRodOnStand = false;
        Status = FishingState != EFishingState::Idle ? TEXT("Удочка в руках.") : TEXT("Удочка в руках. Держите ЛКМ — чем дольше, тем дальше заброс.");
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
    if (HasAuthority() && bCharging && bRigInHand && ServerNow() - ChargeServerStart >= 0.18f)
    {
        bRigInHand = false;
        ForceNetUpdate();
    }
    if (HasAuthority() && FishingState == EFishingState::Landing)
    {
        LandProgress = FMath::Clamp(LandProgress + DeltaSeconds * (bReeling ? 1.0f : -0.7f) / LandLiftTime, 0.0f, 1.0f);
        if (!bReeling) RodLift = FMath::Max(0.0f, RodLift - DeltaSeconds * 0.5f);
    }
    if (IsLocallyControlled() && !IsSteeringRig() && RigSteerPitch != 0)
    {
        RigSteerPitch = FMath::FInterpTo(RigSteerPitch, 0.0f, DeltaSeconds, 4.0f);
        if (FMath::Abs(RigSteerPitch) < 0.05f) RigSteerPitch = 0;
    }
    if (IsLocallyControlled() && !HasAuthority() && ServerNow() >= NextAimSend)
    {
        ServerSwing(SwingPitch, SwingYaw);
        ServerSteer(RigSteerPitch);
        NextAimSend = ServerNow() + 0.033f;
    }
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
    if (HasAuthority() && !RodVisual && FishingState == EFishingState::Waiting && !bRetrieving && ServerNow() >= BaitActiveTime + 1.0f) DriftFloat(DeltaSeconds);
    if (HasAuthority() && !RodVisual && (FishingState == EFishingState::Waiting || FishingState == EFishingState::Bite) && ServerNow() >= BaitActiveTime + 1.0f) ConstrainFloat();
    if (HasAuthority() && FishingState == EFishingState::Fight) UpdateFight(DeltaSeconds);
    if (HasAuthority() && bRetrieving) UpdateRetrieve(DeltaSeconds);
    if (HasAuthority() && FishingState == EFishingState::Landing) UpdateLanding();
    UpdateVisuals(DeltaSeconds);
#if !UE_BUILD_SHIPPING
    RunRodDiagnostics(DeltaSeconds);
#endif
    if (HasAuthority() && RodVisual)
    {
        if (FishingState == EFishingState::Waiting && !bRigFlying && !bRetrieving)
        {
            const FVector K = RodVisual->FloatKeel();
            FloatPosition = FVector(K.X, K.Y, LakeFish::SurfaceZ);
            BaitPoint = RodVisual->HookLocation();
        }
        // The cast rig comes down where the simulation puts it (on the water, or on the bank).
        if (bRigFlying && FishingState == EFishingState::Waiting && RodVisual->FloatDown()) OnRigLanded(RodVisual->FloatKeel());
        // Being lifted out: the float is where the simulation has it.
        if (bRetrieving && FishingState == EFishingState::Waiting)
        {
            const FVector K = RodVisual->FloatKeel();
            if (ALakeEnvironment::IsWater(K)) FloatPosition = FVector(K.X, K.Y, LakeFish::SurfaceZ);
        }
        // The free hand takes the rig by the shot as it swings in within arm's reach, in front of
        // the body and not too fast (else it waits for the next swing; the rod damps them).
        if (RigReach() > 0 && FVector::Dist(RodVisual->ShotLocation(), LeftShoulder()) < ArmReach
            && (RodVisual->ShotVelocity() - GetVelocity()).Size() < CatchSpeed)
        {
            const FVector Offset = RodVisual->ShotLocation() - GetActorLocation();
            RigGripLocal = FVector(FVector::DotProduct(Offset, GetActorForwardVector()), FVector::DotProduct(Offset, GetActorRightVector()), Offset.Z);
            RigGripTime = ServerNow();
            bRigInHand = true;
            ForceNetUpdate();
        }
    }
}
float ALakeCharacter::WaitElevation(float Pitch, float Depth) const
{
    // Normally ~20 deg. The float must still reach the water: keep the tip no higher above the
    // surface than ~85% of the main line left above the float.
    const float MainLine = ALakeRod::LineLength - Depth;
    const float Free = ALakeRod::RodLength - ALakeRod::GripPoint;
    const float Hand = GetActorLocation().Z + 4 - LakeFish::SurfaceZ;
    const float MaxSin = FMath::Clamp((0.85f * MainLine - Hand) / Free, -0.45f, 1.0f);   // from a high bank the tip goes down towards the water
    return FMath::Min(20 + 0.3f * Pitch, FMath::RadiansToDegrees(FMath::Asin(MaxSin)));
}
void ALakeCharacter::RodPoseTarget(float Pitch, float Now, float& OutElevation, FVector& OutGrip, float& OutStiffness, float& OutYaw) const
{
    // Right hand position relative to the body (X forward, Y right, Z up from the hips) and the
    // rod's elevation above the horizon for each phase. With the line as long as the rod, a pole
    // is carried almost upright so the float rig hangs clear of the ground.
    const FVector Idle(24, 20, 14), Forward(40, 16, 12), Waiting(36, 12, 8);
    // Bringing the rig in or lifting a fish out: the rod goes up almost upright and the hands come up
    // and in without moving aside; the rod only leans over to the left as it rises, so its tip
    // ends over the free hand and the rig (or the fish) swings in to it. Where the tip is seen from
    // the eyes is held for that (the yaw is worked out each frame, see HoldSide below); turning the
    // view turns it with the angler.
    const FVector Catch(16, 12, 18);
    constexpr float CatchElevation = 85;
    constexpr float LeanSide = -28;            // tip seen this far left of the view at the top of the lift
    const FVector LandGrip(18, 14, 34);
    constexpr float LandElevation = 86;
    // Seen from the eyes (the first-person camera), how far right of the view the tip is for a pose,
    // and the yaw that puts it at a given angle. The rod is taken as straight, length RodLength.
    const FVector2D Eye(14, 0);
    auto TipSide = [&Eye](const FVector& G, float Elev, float YawDeg)
    {
        const float L = ALakeRod::RodLength * FMath::Cos(FMath::DegreesToRadians(Elev));
        const float Y = FMath::DegreesToRadians(YawDeg);
        const FVector2D T = FVector2D(G) + L * FVector2D(FMath::Cos(Y), FMath::Sin(Y)) - Eye;
        return FMath::RadiansToDegrees(FMath::Atan2(T.Y, T.X));
    };
    auto YawFor = [&Eye](const FVector& G, float Elev, float SideDeg)
    {
        const float L = FMath::Max(ALakeRod::RodLength * FMath::Cos(FMath::DegreesToRadians(Elev)), 1.0f);
        const float A = FMath::DegreesToRadians(SideDeg);
        const FVector2D D = FVector2D(G) - Eye;
        const float K = D.X * FMath::Sin(A) - D.Y * FMath::Cos(A);
        return FMath::RadiansToDegrees(A + FMath::Asin(FMath::Clamp(K / L, -1.0f, 1.0f)));
    };
    bool bHoldSide = false;
    float HoldSide = 0;
    float Elevation = IdleElevation(Pitch);
    FVector Grip = Idle;
    float Stiffness = 7;                       // rad/s: a long rod has weight and lags behind
    float Yaw = -4;                            // the rod a little across the body, so the free hand reaches it
    if (FishingState == EFishingState::Idle && bRigInHand && !bRodOnStand)
    {
        Elevation = SwingBase;
        Grip = FVector(4 + 50 * FMath::Cos(FMath::DegreesToRadians(-45.0f)), 18, 52 + 50 * FMath::Sin(FMath::DegreesToRadians(-45.0f)));
        Stiffness = 5;
    }
    if (FishingState == EFishingState::Idle && !bRigInHand && !bCharging)
    {
        Elevation = CatchElevation;
        Grip = Catch;
        bHoldSide = true;
        HoldSide = LeanSide;
        Stiffness = 4;
    }
    // The swing: the right hand swings on an arc about the shoulder with the rod (raised: up and
    // forward, lowered: down and back), like the arm of a pendulum.
    auto SwingArc = [](float Pitch)
    {
        const float Arm = FMath::DegreesToRadians(-45.0f + 0.9f * Pitch);   // below the horizontal
        return FVector(4 + 50 * FMath::Cos(Arm), 18, 52 + 50 * FMath::Sin(Arm));
    };
    const FVector SwingGrip = SwingArc(SwingPitch);
    if (bCharging || bRigFlying)
    {
        Elevation = SwingBase + SwingPitch;
        Yaw = 7 + SwingYaw;
        Grip = SwingGrip;
        Stiffness = 14;
    }
    else if (FishingState != EFishingState::Idle)
    {
        const float Since = Now - CastServerTime;
        Elevation = WaitElevation(Pitch, HookDepth);
        Grip = Waiting;
        if (Since >= 0 && Since < 1.5f) Stiffness = 3.5f;   // after the cast the heavy pole settles slowly
        if (bRetrieving) Stiffness = 9;   // (the pose of the lift: RetrieveTipPlan below)
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
            const float L = LandProgress;
            const float A = FMath::InterpEaseInOut(0.0f, 1.0f, L, 2.0f);
            const float FightElevation = FMath::Lerp(22.0f, 80.0f, RodLift);
            const FVector FightGrip = FMath::Lerp(FVector(36, 18, 8), FVector(20, 18, 24), RodLift);
            Elevation = FMath::Lerp(FightElevation, LandElevation, A);
            Grip = FMath::Lerp(FightGrip, LandGrip, A);
            bHoldSide = true;
            HoldSide = FMath::Lerp(TipSide(FightGrip, FightElevation, 7 + SwingYaw), LeanSide, A);
            Stiffness = 6;
        }
        const float SinceStrike = Now - StrikeServerTime;
        if (SinceStrike >= 0 && SinceStrike < 0.3f) { Elevation += 15 * FMath::Sin(PI * SinceStrike / 0.3f); Stiffness = 20; }
    }
    if (RodVisual && !bRodOnStand)
    {
        // One continuous aim in every fishing phase, including after release and splashdown.
        // LMB in a fight still adds a gradual lift; retrieval/landing assist the hands near the end.
        // Hold low for an underhand cast, then lower the tip before the rig reaches the water.
        const float Aim = FMath::Clamp(RodBase() + Pitch + SwingPitch, RodLowestAngle, RodHighestAngle);
        float Assist = 0;
        if (FishingState == EFishingState::Idle && !bRigInHand && !bCharging) Assist = 1;
        if (bRetrieving) Assist = 1;
        if (FishingState == EFishingState::Landing) Assist = 1;
        const float Lift = FishingState == EFishingState::Fight ? RodLift * 25 - Tension * 10 : 0;
        if (!bRetrieving) Elevation = FMath::Lerp(FMath::Clamp(Aim + Lift, RodLowestAngle, RodHighestAngle),
            FMath::Clamp(Elevation + SwingPitch * 0.2f, RodLowestAngle, 100.0f), Assist);
        const float Arm = FMath::DegreesToRadians(-45.0f + 0.75f * (Elevation - SwingBase));
        const FVector AimGrip(4 + 50 * FMath::Cos(Arm), 18, 52 + 50 * FMath::Sin(Arm));
        Grip = FMath::Lerp(FRotator(0, SwingYaw * 0.4f, 0).RotateVector(AimGrip), Grip, Assist);
        // Keep mouse steering available even while the automatic hand-catching assist is active.
        if (!bRetrieving && !(FishingState == EFishingState::Idle && !bRigInHand)) Yaw = 7 + SwingYaw;
        Stiffness = bCharging || bRigFlying ? 10 : 9;
    }
    // Lifting without swinging sideways: the yaw that keeps the tip where it was seen.
    if (bHoldSide && !bRodOnStand)
    {
        Elevation = FMath::Clamp(Elevation + RigSteerPitch, RodLowestAngle, 85.0f);
        Yaw = YawFor(Grip, Elevation, HoldSide);
    }
    if (bRetrieving && !bRodOnStand)
    {
        // Lifting the rig out: the rod rises to almost upright. First its tip goes up over where the
        // rig lies, so the line lifts the rig straight up out of the water (not dragging it out
        // across the water away from the angler); then the tip comes in over the free hand, and the
        // rig swings in under it and hangs at the hand. Once out of the water its swing is damped by
        // moving the tip against it (the hands follow a swinging weight); nothing pushes the rig.
        const float Free = ALakeRod::RodLength - ALakeRod::GripPoint;
        const float R = FMath::Clamp(RetrieveProgress, 0.0f, 1.0f);
        FVector2D Tip = RetrieveTipPlan(R, Grip);
        if (RodVisual)
        {
            const FVector Shot = RodVisual->ShotLocation();
            const float InAir = ALakeEnvironment::IsWater(Shot) ? FMath::SmoothStep(0.0f, 25.0f, float(Shot.Z) - LakeFish::SurfaceZ) : 1.0f;
            const FVector Swing = GetActorTransform().InverseTransformVectorNoScale(RodVisual->ShotVelocity() - GetVelocity());
            Tip -= (FVector2D(Swing) * 0.6f).GetClampedToMaxSize(100) * InAir * FMath::SmoothStep(0.3f, 0.7f, R);
        }
        const FVector2D Offset = Tip - FVector2D(Grip);
        Elevation = FMath::Clamp(FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(float(Offset.Size()) / Free, 0.0f, 1.0f))) + RigSteerPitch, RodLowestAngle, 88.0f);
        Yaw = FMath::RadiansToDegrees(FMath::Atan2(Offset.Y, Offset.X));
    }
    if (RodVisual && !bRodOnStand && !bRigInHand && !bRetrieving &&
        (FishingState == EFishingState::Landing || (FishingState == EFishingState::Idle && !bCharging)))
    {
        // Brake the pendulum by moving its support against its velocity, using the rod only.
        // No force or position correction is applied to the rig or fish here.
        const FVector Speed = FishingState == EFishingState::Landing ? RodVisual->HookVelocity() : RodVisual->ShotVelocity();
        FVector Brake = -GetActorTransform().InverseTransformVectorNoScale(Speed - GetVelocity()) * 0.8f;
        Brake.Z = 0;
        Brake = Brake.GetClampedToMaxSize(200);
        const FVector WantedTip = FVector(46, -20, 0) + Brake;
        const FVector Offset = WantedTip - FVector(Grip.X, Grip.Y, 0);
        const float BrakeElevation = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(float(Offset.Size()) / (ALakeRod::RodLength - ALakeRod::GripPoint), 0.0f, 1.0f)));
        const float BrakeYaw = FMath::RadiansToDegrees(FMath::Atan2(Offset.Y, Offset.X));
        const float Blend = FishingState == EFishingState::Landing ? FMath::SmoothStep(0.25f, 0.8f, LandProgress) : 1.0f;
        Elevation = FMath::Lerp(Elevation, BrakeElevation + RigSteerPitch, Blend);
        Yaw += FMath::FindDeltaAngleDegrees(Yaw, BrakeYaw) * Blend;
    }
    if (bCharging && !bRodOnStand)
    {
        const float Stroke = FMath::SmoothStep(0.0f, 0.65f * RodHeft(), Now - ChargeServerStart);
        Elevation = FMath::Lerp(38.0f, 70.0f, Stroke) + FMath::Clamp(Pitch + SwingPitch, -8.0f, 8.0f);
        Grip = FMath::Lerp(FVector(28, 18, 14), FVector(52, 16, 24), Stroke);
        Yaw = SwingYaw;
        Stiffness = 7;
    }
    else if (bRigFlying && !bRodOnStand)
    {
        const float Drop = FMath::SmoothStep(0.0f, 0.85f * RodHeft(), Now - CastServerTime);
        Elevation = FMath::Lerp(CastDropElevation, WaitElevation(Pitch, HookDepth), Drop);
        Grip = FMath::Lerp(CastDropGrip, Waiting, Drop);
        Yaw = SwingYaw;
        Stiffness = 7;
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
    const float TargetYaw = View.Yaw + YawOffset;
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
#if !UE_BUILD_SHIPPING
    // -RodSideLog: where the rod tip is seen from the camera (degrees, + right of the view centre).
    static const bool bSideLog = FParse::Param(FCommandLine::Get(), TEXT("RodSideLog"));
    if (bSideLog && RodVisual && FishingCamera && IsLocallyControlled())
    {
        static double NextSideLog = 0;
        if (FPlatformTime::Seconds() >= NextSideLog)
        {
            NextSideLog = FPlatformTime::Seconds() + 0.1;
            const FVector Cam = FishingCamera->GetComponentLocation();
            const float TipSide = FMath::FindDeltaAngleDegrees(View.Yaw, (RodVisual->Tip() - Cam).Rotation().Yaw);
            const float HandSide = FMath::FindDeltaAngleDegrees(View.Yaw, (GetActorTransform().TransformPosition(RodGrip) - Cam).Rotation().Yaw);
            const FVector Shot = GetActorTransform().InverseTransformPosition(RodVisual->ShotLocation());
            const FVector TipLocal = GetActorTransform().InverseTransformPosition(RodVisual->Tip());
            UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_ROD_SIDE steer=%+.1f state=%d retrieve=%d progress=%.2f tip=%+.1f deg hands=%+.1f deg rodyaw=%+.1f actoryaw=%+.1f grip=(%.0f,%.0f,%.0f) tiplocal=(%.0f,%.0f,%.0f) shot=(%.0f,%.0f,%.0f) to hand %.0f"),
                RigSteerPitch, (int32)FishingState, bRetrieving ? 1 : 0, RetrieveProgress, TipSide, HandSide,
                FMath::FindDeltaAngleDegrees(View.Yaw, RodYaw), FMath::FindDeltaAngleDegrees(View.Yaw, GetActorRotation().Yaw), RodGrip.X, RodGrip.Y, RodGrip.Z,
                TipLocal.X, TipLocal.Y, TipLocal.Z, Shot.X, Shot.Y, Shot.Z, FVector::Dist(RodVisual->ShotLocation(), HandPoint(0)));
        }
    }
#endif
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
    In.Facing = FRotator(0, GetActorRotation().Yaw, 0).Vector();
    In.Tension = Tension;
    if (const ALakeWeather* Weather = ALakeWeather::Get(GetWorld())) In.Wind = Weather->WindVelocity();
    In.Nibbles = NibbleCounter;
    In.BiteMotion = BiteMotion;
    In.BiteElapsed = Now - BiteStart;
    In.BiteDuration = BiteDuration;
    In.BiteAmount = BiteAmount;
    In.bReeling = false;
    In.bFishLowered = FishingState == EFishingState::Landing && !bReeling;
    In.bOnStand = bRodOnStand || StandAlpha > 0;
    In.StandBase = StandBase;
    In.StandYaw = StandYaw;
    In.LineFish = LineFish;
    // The left hand and the rig: it holds the rig by the shot until the cast lets it go, and catches
    // it again as it swings by (the server decides when).
    const float HookHold = bRigInHand && !bRodOnStand && StandAlpha <= 0 ? 1.0f : 0.0f;
    In.HookHold = HookHold;
    In.bRigFlying = bRigFlying;
    In.bFreeRig = bCharging || bRigFlying || bRetrieving;
    In.RodHeft = RodHeft();
    const float Draw = 0.0f;   // the hand just holds the shot in front of the body
    // The held rig follows a real contact with the fingers.
    In.HandPoint = HandPoint(Draw);
    const float LandAlpha = FishingState == EFishingState::Landing && RodVisual ? 1 - FMath::SmoothStep(30.0f, 70.0f,
        (float)FVector::Dist(RodVisual->HookLocation(), HandPoint(0))) : 0.0f;
    if (FishingState == EFishingState::Landing && LineFish)
    {
        In.FishGrams = LineFish->WeightKg * 1000;
        In.FishLength = LakeFish::LengthCm(LineFish->Species, LineFish->WeightKg);
        In.HandPoint = GetActorLocation() + GetActorForwardVector() * 45 - GetActorRightVector() * 14 + FVector(0, 0, 22);
    }
    RodVisual->UpdateRod(In, DeltaSeconds);
    // A fish lifted out hangs on the hook wherever the rod's line has swung it.
    if (FishingState == EFishingState::Landing && LineFish) LineFish->HangOnLine(RodVisual->HookLocation(), RodVisual->HookLineUp(), DeltaSeconds);

    // Hands: the right hand at the grip; the left hand on the rod above it, or on the rig (the shot
    // pinched in the fingers), or reaching for the rig as it swings near.
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
        // Reaching out for the rig as it swings near, before catching it.
        const float Reach = RigReach();
        const float OnRig = FMath::Max(FMath::Max(LandAlpha, HookHold), Reach);
        Pose.Left.Point = FMath::Lerp(Hold.TransformPosition(FVector(ALakeRod::GripPoint + 10, 0, 0)), LandAlpha > 0 ? RodVisual->HookLocation() : RodVisual->ShotLocation(), OnRig);
        // When holding the rig, fingers point forward and the thumb side points up.
        Pose.Left.Axis = FMath::Lerp(Axis, Up, OnRig).GetSafeNormal();
        Pose.Left.Down = FMath::Lerp(Right, Fwd, OnRig).GetSafeNormal();
        Pose.Left.Pole = GetActorLocation() - Right * 45 - Up * 35 + Fwd * 10;
        Pose.Left.Alpha = HandsAlpha;
        Pose.Left.Pinch = OnRig;
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
    bRetrieveHeld = false;
    bRetrieveAuto = false;
    bRigFlying = false;
    if (!RodVisual) bRigInHand = true;   // (no simulation to catch it in automated tests)
    FishingState = EFishingState::Idle;
    Tension = 0;
    bReeling = false;
    Status = Message;
    ForceNetUpdate();
}
void ALakeCharacter::StartRetrieve(const FString& Message, const FString& EndMessage, bool bAuto)
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
    bRigFlying = false;
    RetrieveServerTime = ServerNow();
    RetrieveProgress = 0;
    // Started by a press of LMB: the rod rises while it is held.
    bRetrieveHeld = !bAuto;
    bRetrieveAuto = bAuto;
    RetrieveFromElevation = RodElevation;
    RetrieveFromGrip = RodGrip;
    RetrieveFromYaw = FMath::FindDeltaAngleDegrees(GetControlRotation().Yaw, RodYaw);
    {
        const FVector Rig = GetActorTransform().InverseTransformPosition(RodVisual ? RodVisual->ShotLocation() : FloatPosition);
        RetrieveRigLocal = FVector2D(Rig);
    }
    RetrieveEndMessage = EndMessage;
    Status = Message;
    ForceNetUpdate();
}
FVector2D ALakeCharacter::RetrieveTipPlan(float R, FVector& OutGrip) const
{
    // Lifting the rig out, at progress R: the hands come up to the chest; the tip first goes up over
    // where the rig lay (within what the rod reaches raised to ~35 degrees), then in over the free hand.
    const float Free = ALakeRod::RodLength - ALakeRod::GripPoint;
    OutGrip = FMath::Lerp(RetrieveFromGrip, FVector(RetrieveGripX, RetrieveGripY, RetrieveGripZ), FMath::SmoothStep(0.0f, 1.0f, R));
    const float FromYaw = FMath::DegreesToRadians(RetrieveFromYaw);
    const FVector2D TipFrom = FVector2D(RetrieveFromGrip) + Free * FMath::Cos(FMath::DegreesToRadians(RetrieveFromElevation)) * FVector2D(FMath::Cos(FromYaw), FMath::Sin(FromYaw));
    const FVector2D OverRig = FVector2D(OutGrip) + (RetrieveRigLocal - FVector2D(OutGrip)).GetClampedToSize(60.0f, Free * 0.82f);
    return FMath::Lerp(FMath::Lerp(TipFrom, OverRig, FMath::SmoothStep(0.0f, 0.55f, R)), FVector2D(RetrieveTipX, RetrieveTipY), FMath::SmoothStep(0.4f, 1.0f, R));
}
float ALakeCharacter::RetrieveTipSpeed(float R) const
{
    // How far the planned tip moves per unit of progress around R (cm), up and down included.
    const float Free = ALakeRod::RodLength - ALakeRod::GripPoint;
    auto TipAt = [this, Free](float P)
    {
        FVector G;
        const FVector2D T = RetrieveTipPlan(P, G);
        const float Out = FMath::Min(float((T - FVector2D(G)).Size()) / Free, 1.0f);
        return FVector(T, G.Z + Free * FMath::Sqrt(1 - Out * Out));
    };
    const float R0 = FMath::Clamp(R, 0.0f, 0.98f);
    return FVector::Dist(TipAt(R0 + 0.02f), TipAt(R0)) / 0.02f;
}
void ALakeCharacter::UpdateRetrieve(float DeltaSeconds)
{
    if (!bRetrieving) return;
    // The rig taken in the hand: it is out of the water.
    if (bRigInHand && RodVisual && ServerNow() - RigGripTime >= 0.8f)
    { FinishFishing(RetrieveEndMessage); return; }
    // The rod rises only while LMB is held; let go, it stops and holds where it is. A long springy
    // pole swung fast whips the light rig out through the line, so the hands keep the tip slow: quick
    // while the line is slack, slow once it is taut and the rig is drawn out of the water, and slower
    // still if the rig is coming fast.
    float Rate = 1 / RetrieveTime;
    if (RodVisual)
    {
        const float Speed = (RodVisual->ShotVelocity() - GetVelocity()).Size();
        const float Taut = FVector::Dist(RodVisual->Tip(), RodVisual->FloatKeel()) / FMath::Max(ALakeRod::LineLength - HookDepth, 1.0f);
        const bool bWet = ALakeEnvironment::IsWater(RodVisual->ShotLocation()) && RodVisual->ShotLocation().Z < LakeFish::SurfaceZ + 5;
        const float TipSpeed = bWet ? FMath::Lerp(RetrieveTipFast, RetrieveTipSlow, FMath::SmoothStep(0.85f, 1.0f, Taut)) : RetrieveTipAir;
        Rate = FMath::Min(Rate, TipSpeed / FMath::Max(RetrieveTipSpeed(RetrieveProgress), 1.0f))
            * FMath::GetMappedRangeValueClamped(FVector2f(300, 550), FVector2f(1.0f, 0.3f), Speed);
    }
    if (bRetrieveHeld || bRetrieveAuto) RetrieveProgress = FMath::Min(1.0f, RetrieveProgress + Rate * DeltaSeconds);
    if (RetrieveProgress >= 1 && !RodVisual) FinishFishing(RetrieveEndMessage);
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
void ALakeCharacter::DriftFloat(float DeltaSeconds)
{
    // The wind drives the surface water and catches the line lying on it: a float with the bait
    // hanging free drifts downwind (about 7 cm/s in a fresh 5 m/s breeze) until the line holds it
    // (ConstrainFloat); a rig with the bait or the shot on the bottom stays put, and it never drifts
    // into the shallows.
    const ALakeWeather* Weather = ALakeWeather::Get(GetWorld());
    if (!Weather) return;
    const float Water = LakeFish::SurfaceZ - ALakeEnvironment::GroundHeight(FloatPosition.X, FloatPosition.Y);
    if (FloatDepth > Water - 5) return;
    FVector Drift = Weather->WindVelocity() * DriftShare;
    Drift.Z = 0;
    const FVector Next = FloatPosition + Drift * DeltaSeconds;
    if (!ALakeEnvironment::IsWater(Next) || LakeFish::SurfaceZ - ALakeEnvironment::GroundHeight(Next.X, Next.Y) < FloatDepth + 10) return;
    FloatPosition = Next;
    PlaceBait();
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
        StartRetrieve(TEXT("Снасть вышла на берег."), TEXT("Вы вытащили снасть из воды. ЛКМ — забросить снова."), true);
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
    LandProgress = 0;
    Tension = 0;
    LineFish = Fish;
    Fish->LiftOut();
    const FVector P = Fish->GetActorLocation();
    if (ALakeLife* Life = ALakeLife::Get(GetWorld()))
        Life->EmitSign(ELakeSign::Splash, FVector(P.X, P.Y, LakeFish::SurfaceZ), FMath::Min(0.4f + Fish->WeightKg * 0.5f, 1.4f), Fish->Species, Fish->WeightKg, 0, true);
    Status = TEXT("Поднимаете рыбу из воды… Держите ЛКМ: рыба придёт к левой руке.");
    ForceNetUpdate();
}
void ALakeCharacter::UpdateLanding()
{
    if (!bReeling && RodVisual && LandProgress <= 0 && RodVisual->HookLocation().Z < LakeFish::SurfaceZ - 3
        && ALakeEnvironment::IsWater(RodVisual->HookLocation()))
    {
        if (ALakeFish* Fish = HookedFish.Get()) Fish->LowerIntoWater(RodVisual->HookVelocity());
        FishingState = EFishingState::Fight;
        RodLift = 0;
        ForceNetUpdate();
        return;
    }
    // In the hand: the catch counts, the fish goes into the keepnet and the rig hangs from the tip.
    if (!bReeling || (RodVisual && (FVector::Dist(RodVisual->HookLocation(), HandPoint(0)) > CatchReach
        || (RodVisual->HookVelocity() - GetVelocity()).Size() > 150))) return;
    if (!RodVisual && LandProgress < 1) return;
    ALakeFish* Fish = HookedFish.Get();
    if (!Fish) { FinishFishing(TEXT("Рыба сошла. ЛКМ — новый заброс.")); return; }
    ++CatchCount;
    const FFishSpeciesInfo& S = LakeFish::Info(Fish->Species);
    const FString Message = FString::Printf(TEXT("%s, %.2f кг (%.0f см)! ЛКМ — забросить снова."), *S.Name, Fish->WeightKg, LakeFish::LengthCm(Fish->Species, Fish->WeightKg));
    HookedFish = nullptr;
    LineFish = nullptr;
    // Live Fish puts another of its kind in its place after a while; the place feels fished.
    if (ALakeLife* Life = ALakeLife::Get(GetWorld())) { Life->AddPressure(FloatPosition, 1); Life->Restock(Fish); }
    Fish->Destroy();
    // The fish is collected at contact; the free rig still has to reach the fingers.
    bRigInHand = false;
    FinishFishing(Message);
}
void ALakeCharacter::ServerFish_Implementation()
{
    if (FishingState == EFishingState::Bite) { Strike(); bReeling = FishingState == EFishingState::Fight; return; }
    if (FishingState == EFishingState::Fight) { bReeling = true; return; }
    if (FishingState == EFishingState::Landing) { bReeling = true; return; }
    if (bRetrieving)
    {
        if (!bRigInHand) Status = TEXT("Поднимаете удилище… Снасть придёт в левую руку.");
        bRetrieveHeld = true;
        return;
    }
    if (bRigFlying) return;
    const double Now = GetWorld()->GetTimeSeconds();
    if (Now < NextActionTime) return;
    NextActionTime = Now + 0.3;
    if (FishingState == EFishingState::Idle)
    {
        if (bRodOnStand) { Status = TEXT("Сначала возьмите удочку: R."); return; }
        if (bCharging) return;
        if (RodVisual && !bRigInHand) { Status = TEXT("Оснастка ещё качается на леске: рыбак поймает её рукой, тогда можно забросить."); return; }
        StartCast();
        return;
    }
    // Held, the rod rises and draws the rig out of the water to the free hand; let go, it stops.
    if (ServerNow() < BaitActiveTime || !TakeRodFromStand()) return;
    if (ALakeFish* Fish = BaitFish.Get(); Fish && FishingState == EFishingState::Waiting) Fish->Spook(BaitPoint, 4);
    StartRetrieve(TEXT("Поднимаете удилище… Держите ЛКМ: снасть выйдет из воды и придёт в левую руку."),
                  TEXT("Снасть в левой руке. Держите ЛКМ — маятниковый мах."));
}
void ALakeCharacter::ServerCancel_Implementation()
{
    if (bCharging) { ServerRelease_Implementation(); return; }
    if (FishingState == EFishingState::Bite) { Strike(); return; }
    if (FishingState != EFishingState::Waiting || bRetrieving || bRigFlying || ServerNow() < BaitActiveTime) return;
    if (!TakeRodFromStand()) return;
    // A strike with no bite: the rod jerks, the rig stays; a fish only trying the bait is frightened off.
    StrikeServerTime = ServerNow();
    if (ALakeFish* Fish = BaitFish.Get())
    {
        Fish->Spook(BaitPoint, 4);
        BaitFish = nullptr;
        Status = TEXT("Рано подсекли! Рыба только пробовала наживку: подсекайте (ПКМ), когда поплавок уйдёт под воду, ляжет или поедет.");
    }
    else Status = TEXT("Подсечка впустую: поклёвки не было.");
    ForceNetUpdate();
}
void ALakeCharacter::Strike()
{
    ALakeFish* Fish = BaitFish.Get();
    if (bRodOnStand)
    {
        // Rod on the stand: grab it and strike in one move, if close enough.
        if (FVector::Dist2D(GetActorLocation(), StandBase) > 260) { Status = TEXT("ПОКЛЁВКА! Бегите к удочке!"); return; }
        bRodOnStand = false;
    }
    StrikeServerTime = ServerNow();
    GetWorldTimerManager().ClearTimer(FishingTimer);
    auto FishOn = [this](const TCHAR* Message)
    {
        // Missed: the fish is gone, the rig stays and fishes on.
        BaitFish = nullptr;
        LineFish = nullptr;
        BiteMotion = EBiteMotion::None;
        FishingState = EFishingState::Waiting;
        PlaceBait();
        Status = Message;
        ForceNetUpdate();
    };
    if (!Fish) { FishOn(TEXT("Пусто: рыба уже отпустила наживку.")); return; }
    // Whether the hook goes home depends on where the bite is: struck as the float only starts to
    // rise, the fish may hold the bait by its lips alone (once it lies down, is led off or goes under,
    // the hook goes home); a pike running with the bait across its jaws is often missed - wait for
    // it to stop and go again.
    float HookChance = Fish->StrikeHookChance();
    if (BiteMotion == EBiteMotion::Lift) HookChance = BiteAmount >= 2.5f && ServerNow() - BiteStart > BiteDuration * 0.55f ? 0.95f : 0.7f;
    if (FMath::FRand() > HookChance)
    {
        Fish->Release(10);
        const FString& Own = LakeFish::Info(Fish->Species).Bite.EarlyStrikeText;
        FishOn(BiteMotion != EBiteMotion::Lift && !Own.IsEmpty() ? *Own
            : TEXT("Подсекли, но рыба не засеклась: она едва приподняла наживку. Подсекайте, когда поплавок ляжет, поедет или утонет."));
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
    bReeling = false;
    RodLift = 0;
    Status = TEXT("Засекли! Держите ЛКМ — поднимать удилище и тянуть, отпускайте, когда рыба рвётся. Можно отходить от воды.");
    ForceNetUpdate();
}
bool ALakeCharacter::ClaimBait(ALakeFish* Fish)
{
    if (FishingState != EFishingState::Waiting || bRetrieving) return false;
    if (ALakeFish* Holder = BaitFish.Get(); Holder && Holder != Fish)
    {
        // A clearly bigger, bolder fish pushes a smaller one off a bait it is only trying.
        if (!Fish || !Holder->IsNibbling() || Fish->Clout() < Holder->Clout() * 1.4f) return false;
        Holder->PushedOff(Fish);
    }
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
    Status = TEXT("ПОКЛЁВКА! Подсекайте — ПКМ!");
    // A fish set on the bait directly holds it about as long as its species does (a hungry one longer).
    if (Window < 0)
        Window = (Fish ? LakeFish::Info(Fish->Species).Bite.DirectHold : 3.0f)
            * FMath::Lerp(0.75f, 1.2f, FMath::Clamp((Fish ? Fish->Appetite() : 1.0f) / 1.2f, 0.0f, 1.0f));
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
    if (FishingState != EFishingState::Idle || bCharging) { Status = TEXT("Глубину можно менять только до заброса: сначала вытащите снасть (ЛКМ)."); return; }
    FloatDepth = FMath::Clamp(FMath::RoundToFloat(FloatDepth + Delta), MinDepth, MaxDepth);
    HookDepth = FloatDepth;      // the float slides along the hanging rig right away
    Status = FString::Printf(TEXT("Глубина спуска наживки: %.0f см."), FloatDepth);
}
void ALakeCharacter::Escape()
{
    if (FishingState != EFishingState::Bite) return;
    // Not struck in time: the fish lets go of the bait and swims off. The rig stays where the fish
    // left it and keeps fishing: the float stands up again, and the angler may wait or draw it out (LMB).
    if (ALakeFish* Holder = BaitFish.Get()) Holder->DropBait();
    BaitFish = nullptr;
    LineFish = nullptr;
    BiteMotion = EBiteMotion::None;
    FishingState = EFishingState::Waiting;
    PlaceBait();
    Status = TEXT("Рыба выпустила наживку. Поплавок встал на место — не дёргайте снасть: голодная рыба может вернуться.");
    ForceNetUpdate();
}
float ALakeCharacter::RigDisturbance() const
{
    if (bRetrieving) return 1.0f;
    return FMath::SmoothStep(15.0f, 60.0f, FMath::Abs(RodElevationSpeed) + FMath::Abs(RodYawSpeed));
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
    DOREPLIFETIME(ALakeCharacter, CastDropElevation);
    DOREPLIFETIME(ALakeCharacter, CastDropGrip);
    DOREPLIFETIME(ALakeCharacter, RigGripLocal);
    DOREPLIFETIME(ALakeCharacter, RigGripTime);
    DOREPLIFETIME(ALakeCharacter, LandProgress);
    DOREPLIFETIME(ALakeCharacter, bRetrieving);
    DOREPLIFETIME(ALakeCharacter, RetrieveServerTime);
    DOREPLIFETIME(ALakeCharacter, RetrieveFromElevation);
    DOREPLIFETIME(ALakeCharacter, RetrieveFromGrip);
    DOREPLIFETIME(ALakeCharacter, RetrieveFromYaw);
    DOREPLIFETIME(ALakeCharacter, RetrieveRigLocal);
    DOREPLIFETIME(ALakeCharacter, RetrieveProgress);
    DOREPLIFETIME(ALakeCharacter, bRetrieveHeld);
    DOREPLIFETIME(ALakeCharacter, CastServerTime);
    DOREPLIFETIME(ALakeCharacter, CastFlight);
    DOREPLIFETIME(ALakeCharacter, CastResult);
    DOREPLIFETIME(ALakeCharacter, RodKind);
    DOREPLIFETIME(ALakeCharacter, GroundbaitLeft);
    DOREPLIFETIME(ALakeCharacter, bRigInHand);
    DOREPLIFETIME_CONDITION(ALakeCharacter, SwingPitch, COND_SkipOwner);
    DOREPLIFETIME_CONDITION(ALakeCharacter, SwingYaw, COND_SkipOwner);
    DOREPLIFETIME_CONDITION(ALakeCharacter, RigSteerPitch, COND_SkipOwner);
    DOREPLIFETIME(ALakeCharacter, bRigFlying);
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

