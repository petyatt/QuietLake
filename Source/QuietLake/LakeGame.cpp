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
#include "LakeFish.h"

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
    Input->BindAction("BaitWorm", IE_Pressed, this, &ALakeCharacter::BaitWorm);
    Input->BindAction("BaitMaggot", IE_Pressed, this, &ALakeCharacter::BaitMaggot);
    Input->BindAction("BaitBread", IE_Pressed, this, &ALakeCharacter::BaitBread);
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
        // Bobber: gentle bob while waiting, a sharp twitch on every nibble,
        // pulled under while a fish holds the bait, dragged along by a hooked fish.
        const float Now = GetWorld()->GetTimeSeconds();
        if (NibbleCounter != SeenNibbles) { SeenNibbles = NibbleCounter; NibbleVisualTime = Now; }
        FVector Position = FloatPosition;
        const float Twitch = FMath::Max(0.0f, 1 - (Now - NibbleVisualTime) / 0.45f);
        if (FishingState == 1) Position.Z = -18 + FMath::Sin(Now * 2) * 2 - Twitch * 7 * FMath::Abs(FMath::Sin(Now * 40));
        else if (FishingState == 2) Position.Z = -34 + FMath::Sin(Now * 14) * 4;
        else Position.Z = -18 + FMath::Sin(Now * 9) * 3;
        Bobber->SetWorldLocation(Position);
        Bobber->SetWorldScale3D(FVector(0.09, 0.09, 0.22));
        Segment(Line, Tip, Position, 0.3f);
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
    FishingState = 0;
    Tension = 0;
    bReeling = false;
    Status = Message;
    ForceNetUpdate();
}
FVector ALakeCharacter::RodTip() const
{
    const FRotator View = GetBaseAimRotation();
    return GetActorLocation() + FVector(0, 0, 64) + View.RotateVector(FVector(275, 10, 41));
}
void ALakeCharacter::UpdateFight(float DeltaSeconds)
{
    if (FishingState != 3) return;
    ALakeFish* Fish = HookedFish.Get();
    if (!Fish) { FinishFishing(TEXT("Рыба сошла. ЛКМ — новый заброс.")); return; }
    FightTime += DeltaSeconds;
    // The line is a stiff spring between the rod tip and the fish's mouth.
    // Holding LMB winds the reel; releasing it opens the drag so the fish can take line freely.
    constexpr float ReelSpeed = 55.0f, LineStiffness = 70.0f, BreakForce = 2600.0f, MaxLine = 3000.0f;
    const FVector Tip = RodTip();
    const FVector ToTip = Tip - Fish->MouthLocation();
    const float Distance = ToTip.Size();
    if (bReeling) LineLength = FMath::Max(60.0f, LineLength - ReelSpeed * DeltaSeconds);
    else LineLength = FMath::Clamp(FMath::Max(LineLength, Distance - 4), 0.0f, MaxLine);
    const float Stretch = FMath::Max(0.0f, Distance - LineLength);
    const float Force = Stretch * LineStiffness;
    Tension = FMath::Clamp(Force / BreakForce, 0.0f, 1.0f);
    Fish->ApplyLineForce(ToTip.GetSafeNormal() * Force);
    FishDistance = FVector::Dist2D(Fish->GetActorLocation(), GetActorLocation());
    FloatPosition = FVector(Fish->GetActorLocation().X, Fish->GetActorLocation().Y, -18);
    const FFishSpeciesInfo& S = LakeFish::Info(Fish->Species);
    if (Force >= BreakForce) FinishFishing(TEXT("Леска оборвалась! Чаще отпускайте ЛКМ. ЛКМ — новый заброс."));
    else if (FightTime >= 120) FinishFishing(TEXT("Рыба сошла: вываживание затянулось. ЛКМ — новый заброс."));
    // Landed: led into the shallows near the angler, or up to the rod tip (from the pier).
    else if (FVector::Dist2D(Fish->GetActorLocation(), Tip) < 220 ||
             (ALakeEnvironment::LakeRadius(Fish->GetActorLocation().X, Fish->GetActorLocation().Y) > 0.9f && FishDistance < 900))
    {
        ++CatchCount;
        const FString Message = FString::Printf(TEXT("%s, %.2f кг (%.0f см)! ЛКМ — забросить снова."), S.Name, Fish->WeightKg, LakeFish::LengthCm(Fish->Species, Fish->WeightKg));
        const EFishSpecies Species = Fish->Species;
        HookedFish = nullptr;
        Fish->Destroy();
        for (TActorIterator<ALakeEnvironment> It(GetWorld()); It; ++It) It->ScheduleRespawn(Species);
        FinishFishing(Message);
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
        // Float rig: the hook hangs up to 1.2 m under the bobber, never on the bottom.
        const float Bottom = ALakeEnvironment::GroundHeight(Target.X, Target.Y);
        BaitPoint = FVector(Target.X, Target.Y, FMath::Max(LakeFish::SurfaceZ - 120, Bottom + 8));
        FishingState = 1;
        Status = FString::Printf(TEXT("Наживка: %s. Ждём поклёвку, следите за поплавком. ПКМ — убрать снасть."), LakeFish::BaitName(Bait));
        // The splash scares fish right under the bobber for a moment.
        for (TActorIterator<ALakeFish> It(GetWorld()); It; ++It)
            if (FVector::Dist2D(It->GetActorLocation(), Target) < 220) It->Spook(Target, 2.5f);
    }
    else if (FishingState == 1)
    {
        if (ALakeFish* Fish = BaitFish.Get()) Fish->Spook(BaitPoint, 4);
        ServerCancel_Implementation();
        Status = TEXT("Рано подсекли! Дождитесь, пока поплавок уйдёт под воду. ЛКМ — забросить снова.");
    }
    else
    {
        ALakeFish* Fish = BaitFish.Get();
        if (!Fish) { FinishFishing(TEXT("Пусто: рыба уже отпустила наживку.")); return; }
        GetWorldTimerManager().ClearTimer(FishingTimer);
        Fish->Hook(this);
        HookedFish = Fish;
        BaitFish = nullptr;
        FishingState = 3;
        Tension = 0.2f;
        FightTime = 0;
        LineLength = FVector::Dist(RodTip(), Fish->MouthLocation());
        bReeling = true;
        Status = TEXT("Засекли! Держите ЛКМ — тянуть, отпускайте, когда рыба рвётся.");
    }
}
void ALakeCharacter::ServerCancel_Implementation()
{
    FinishFishing(TEXT("Снасть убрана. ЛКМ — забросить."));
}
bool ALakeCharacter::ClaimBait(ALakeFish* Fish)
{
    if (FishingState != 1) return false;
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
    if (FishingState != 1) return;
    ++NibbleCounter;
    Status = TEXT("Поплавок вздрагивает — рыба пробует наживку. Ждите…");
}
void ALakeCharacter::OnFishTake(ALakeFish* Fish)
{
    if (FishingState != 1) return;
    BaitFish = Fish;
    FishingState = 2;
    Status = TEXT("ПОКЛЁВКА! Поплавок ушёл под воду — нажмите ЛКМ, подсекайте!");
    // Cautious fish hold the bait a little longer before spitting it out.
    const float Window = Fish && Fish->Species == EFishSpecies::Crucian ? 2.6f : Fish && Fish->Species == EFishSpecies::Pike ? 1.5f : 2.0f;
    GetWorldTimerManager().SetTimer(FishingTimer, this, &ALakeCharacter::Escape, Window, false);
    ForceNetUpdate();
}
void ALakeCharacter::Escape() { if (FishingState == 2) FinishFishing(TEXT("Рыба выплюнула наживку. ЛКМ — попробовать снова.")); }
void ALakeCharacter::SelectBait(EBait NewBait) { ServerSelectBait(NewBait); }
void ALakeCharacter::ServerSelectBait_Implementation(EBait NewBait)
{
    if (FishingState != 0 || NewBait >= EBait::Count) return;
    Bait = NewBait;
    Status = FString::Printf(TEXT("Наживка: %s. ЛКМ — забросить."), LakeFish::BaitName(Bait));
}
void ALakeCharacter::EndPlay(const EEndPlayReason::Type Reason) { GetWorldTimerManager().ClearTimer(FishingTimer); Super::EndPlay(Reason); }
void ALakeCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ALakeCharacter, FishingState);
    DOREPLIFETIME(ALakeCharacter, FloatPosition);
    DOREPLIFETIME(ALakeCharacter, Tension);
    DOREPLIFETIME(ALakeCharacter, Bait);
    DOREPLIFETIME(ALakeCharacter, NibbleCounter);
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
    DrawText(FString::Printf(TEXT("Улов: %d   |   Наживка: %s (1 — червь, 2 — опарыш, 3 — хлеб)   |   ПКМ — убрать снасть"), Fisher->CatchCount, LakeFish::BaitName(Fisher->Bait)), FLinearColor::White, 40, 110);
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

