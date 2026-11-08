// The angler's three rods and the rod case: where each rod is (the case, the hands, its stand, the
// ground), the tackle of the rods left fishing on their stands, the case on the back or on the ground,
// and the owner's local interface for them (the ghost under the aim, the prompt over the case, the
// held keys). The server decides everything; every machine draws the rods and the case itself.
#include "LakeGame.h"
#include "LakeHUD.h"
#include "LakeLife.h"
#include "LakeMap.h"
#include "LakeRod.h"
#include "LakeRodCase.h"
#include "LakeWeather.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"

namespace
{
    // Real time, for the held keys (the same on every machine is not needed: they are local).
    float RealNow(const UWorld* World) { return World ? World->GetRealTimeSeconds() : 0.0f; }
    /** The distance from the line Eye + t Dir (t > 0) to the segment AB, and t at the nearest point. */
    float RayToSegment(const FVector& Eye, const FVector& Dir, const FVector& A, const FVector& B, float& OutT)
    {
        FVector OnRay, OnSegment;
        FMath::SegmentDistToSegmentSafe(Eye, Eye + Dir * 1000.0f, A, B, OnRay, OnSegment);
        OutT = FVector::DotProduct(OnRay - Eye, Dir);
        return FVector::Dist(OnRay, OnSegment);
    }
}

// --- rules -----------------------------------------------------------------------------------

bool ALakeCharacter::CanPutRodAway() const
{
    return HasRodInHands() && FishingState == EFishingState::Idle && !bCharging && !bRetrieving && !bRigFlying && bRigInHand && !bFishInHand;
}

namespace
{
    /** Why the rod in the hands cannot be put away now (empty if it can). */
    FString WhyRodBusy(const ALakeCharacter& A)
    {
        if (A.FishingState >= EFishingState::Bite) return TEXT("Рыба на снасти: сначала вываживайте её.");
        if (A.bCharging) return TEXT("Идёт заброс.");
        if (A.FishingState == EFishingState::Waiting || A.bRetrieving || A.bRigFlying) return TEXT("Снасть в воде: сначала вытащите её (ЛКМ).");
        if (!A.bRigInHand) return TEXT("Оснастка качается на леске: подождите, пока рыбак поймает её рукой.");
        return FString();
    }
}

FTransform ALakeCharacter::GroundHoldAt(FVector Base, float Yaw, float Pitch)
{
    // The butt just off the ground, the rod along Yaw, raised by Pitch towards its middle.
    return FTransform(FRotator(Pitch, Yaw, 0), Base + FVector(0, 0, 1.5f));
}

float ALakeCharacter::DistanceToRod(int32 Rod) const
{
    if (!Rods.IsValidIndex(Rod)) return BIG_NUMBER;
    const FLakeRodSlot& Slot = Rods[Rod];
    if (Slot.Place != ERodPlace::Stand && Slot.Place != ERodPlace::Ground) return BIG_NUMBER;
    // A rod on the ground is taken anywhere along its first two metres, a stand rod at the stand.
    FVector2D Near(Slot.Base);
    if (Slot.Place == ERodPlace::Ground)
    {
        const FVector2D Dir = FVector2D(FRotator(0, Slot.Yaw, 0).Vector()).GetSafeNormal();
        Near += Dir * FMath::Clamp(FVector2D::DotProduct(FVector2D(GetActorLocation()) - Near, Dir), 0.0f, 200.0f);
    }
    return FVector2D::Distance(Near, FVector2D(GetActorLocation()));
}

int32 ALakeCharacter::NearestParkedRod(bool bBiteOnly) const
{
    int32 Best = -1;
    float BestDist = Reach;
    for (int32 Rod = 0; Rod < Rods.Num(); ++Rod)
    {
        if (bBiteOnly && !(Rods[Rod].Place == ERodPlace::Stand && Rods[Rod].State == EFishingState::Bite)) continue;
        const float Dist = DistanceToRod(Rod);
        if (Dist <= BestDist) { BestDist = Dist; Best = Rod; }
    }
    return Best;
}

// --- moving a rod ------------------------------------------------------------------------------

void ALakeCharacter::ParkHandRod(ERodPlace Place, FVector Base, float Yaw, float Pitch)
{
    if (!HasRodInHands()) return;
    const int32 Rod = (int32)RodKind;
    FLakeRodSlot& Slot = Rods[Rod];
    // Only a rod on its stand goes on fishing, and only with its rig settled in the water.
    const bool bFishing = Place == ERodPlace::Stand && FishingState == EFishingState::Waiting && !bRetrieving && !bRigFlying;
    Slot.Place = Place;
    Slot.CaseSlot = -1;
    Slot.Base = Base;
    Slot.Yaw = Yaw;
    Slot.Pitch = Pitch;
    Slot.State = bFishing ? EFishingState::Waiting : EFishingState::Idle;
    Slot.FloatPosition = FloatPosition;
    Slot.HookDepth = HookDepth;
    Slot.FloatDepth = FloatDepth;
    Slot.Bait = Bait;
    Slot.Nibbles = NibbleCounter;
    Slot.BiteMotion = bFishing ? BiteMotion : EBiteMotion::None;
    Slot.BiteStart = BiteStart;
    Slot.BiteDuration = BiteDuration;
    Slot.BiteAmount = BiteAmount;
    Slot.BaitPoint = BaitPoint;
    Slot.BaitActiveTime = BaitActiveTime;
    Slot.EscapeAt = -1;
    Slot.LineFish = nullptr;
    Slot.BaitFish = bFishing ? BaitFish : nullptr;
    // A fish trying this bait keeps to this rod.
    if (ALakeFish* Fish = Slot.BaitFish.Get(); Fish && Fish->TargetRig < 0) Fish->TargetRig = Rod;
    // The hands are empty now.
    GetWorldTimerManager().ClearTimer(FishingTimer);
    BaitFish = nullptr;
    LineFish = nullptr;
    HookedFish = nullptr;
    BiteMotion = EBiteMotion::None;
    FishingState = EFishingState::Idle;
    Tension = 0;
    bReeling = false;
    RodLift = 0;
    bCharging = bRetrieving = bRetrieveHeld = bRetrieveAuto = bRigFlying = false;
    bRigInHand = true;
    if (RodVisuals.Num()) RodVisual = nullptr;   // (tests give a single simulation of their own: it stays)
    ForceNetUpdate();
}

void ALakeCharacter::TakeRodInHands(int32 Rod)
{
    if (HasRodInHands() || !Rods.IsValidIndex(Rod) || Rods[Rod].Place == ERodPlace::Hands) return;
    FLakeRodSlot& Slot = Rods[Rod];
    const ERodPlace From = Slot.Place;
    RodKind = (ERodKind)Rod;
    GetWorldTimerManager().ClearTimer(FishingTimer);
    bCharging = bRetrieving = bRetrieveHeld = bRetrieveAuto = bRigFlying = false;
    Tension = 0;
    bReeling = false;
    RodLift = RodLiftShown = 0;
    StrikeUntil = -1;
    HookImpulse = 0;
    HookedFish = nullptr;
    if (From == ERodPlace::Stand && Slot.State != EFishingState::Idle)
    {
        // Its tackle comes with it: the float on the water, a fish trying or holding the bait.
        FishingState = Slot.State;
        FloatPosition = Slot.FloatPosition;
        HookDepth = Slot.HookDepth;
        FloatDepth = Slot.FloatDepth;
        Bait = Slot.Bait;
        NibbleCounter = Slot.Nibbles;
        BiteMotion = Slot.BiteMotion;
        BiteStart = Slot.BiteStart;
        BiteDuration = Slot.BiteDuration;
        BiteAmount = Slot.BiteAmount;
        BaitPoint = Slot.BaitPoint;
        BaitActiveTime = Slot.BaitActiveTime;
        BaitFish = Slot.BaitFish;
        LineFish = Slot.LineFish;
        bRigInHand = false;
        CastServerTime = -100;
        if (FishingState == EFishingState::Bite)
            GetWorldTimerManager().SetTimer(FishingTimer, this, &ALakeCharacter::Escape, FMath::Max(0.1f, Slot.EscapeAt - ServerNow()), false);
    }
    else
    {
        // From the case it is rigged with the bait and depth chosen now; from a stand or the ground it
        // keeps its own. On the ground its hook was caught on the handle: the free hand takes the rig.
        FishingState = EFishingState::Idle;
        if (From != ERodPlace::Case) { Bait = Slot.Bait; FloatDepth = Slot.FloatDepth; }
        HookDepth = FloatDepth;
        BaitFish = nullptr;
        LineFish = nullptr;
        BiteMotion = EBiteMotion::None;
        bRigInHand = From != ERodPlace::Stand;
        RigGripTime = -100;
    }
    Slot.Place = ERodPlace::Hands;
    Slot.CaseSlot = -1;
    Slot.State = EFishingState::Idle;
    Slot.BaitFish = nullptr;
    Slot.LineFish = nullptr;
    Slot.EscapeAt = -1;
    if (RodVisuals.Num()) RodVisual = RodVisuals.IsValidIndex(Rod) ? RodVisuals[Rod].Get() : nullptr;
    if (FishingState == EFishingState::Idle)
        Status = FString::Printf(TEXT("В руках %s удилище, 6 м, %.0f г."), RodName(RodKind), RodGrams(RodKind));
    ForceNetUpdate();
}

void ALakeCharacter::PlaceBaitFor(FLakeRodSlot& Slot) const
{
    const float Bottom = ALakeEnvironment::GroundHeight(Slot.FloatPosition.X, Slot.FloatPosition.Y);
    Slot.BaitPoint = FVector(Slot.FloatPosition.X, Slot.FloatPosition.Y, FMath::Max(LakeFish::SurfaceZ - Slot.HookDepth, Bottom + 3));
}

void ALakeCharacter::EscapeParked(int32 Rod)
{
    if (!IsParkedRig(Rod)) return;
    FLakeRodSlot& Slot = Rods[Rod];
    if (Slot.State != EFishingState::Bite) return;
    if (ALakeFish* Holder = Slot.BaitFish.Get()) Holder->DropBait();
    Slot.BaitFish = nullptr;
    Slot.LineFish = nullptr;
    Slot.BiteMotion = EBiteMotion::None;
    Slot.State = EFishingState::Waiting;
    Slot.EscapeAt = -1;
    PlaceBaitFor(Slot);
    Status = FString::Printf(TEXT("Рыба выпустила наживку на рогатке (%s удилище)."), RodName((ERodKind)Rod));
    ForceNetUpdate();
}

void ALakeCharacter::UpdateParkedRods(float DeltaSeconds)
{
    for (int32 Rod = 0; Rod < Rods.Num(); ++Rod)
    {
        if (!IsParkedRig(Rod)) continue;
        FLakeRodSlot& Slot = Rods[Rod];
        ALakeRod* Visual = RodVisuals.IsValidIndex(Rod) ? RodVisuals[Rod].Get() : nullptr;
        // The float and the bait are where the simulation has them, as for the rod in the hands.
        if (Visual)
        {
            const FVector Keel = Visual->FloatKeel();
            if (ALakeEnvironment::IsWater(Keel)) Slot.FloatPosition = FVector(Keel.X, Keel.Y, LakeFish::SurfaceZ);
            Slot.BaitPoint = Visual->HookLocation();
        }
        if (Slot.State != EFishingState::Bite) continue;
        if (ALakeFish* Fish = Slot.BaitFish.Get())
        {
            // The fish holding the bait feels the line to the rod on its stand; the rod does not strike.
            Slot.LineFish = Fish;
            const FVector Tip = StandHoldAt(Slot.Base, Slot.Yaw).TransformPosition(FVector(ALakeRod::RodLength, 0, 0));
            Fish->ApplyLineForce(Visual ? Visual->PullOnFish(Fish->MouthLocation(), Fish->Velocity(), Fish->WeightKg)
                : ALakeRod::TetherForce(Tip, Fish->MouthLocation(), Fish->Velocity(), ALakeRod::LineLength, Fish->WeightKg));
            if (!Visual)
            {
                const FVector Mouth = Fish->MouthLocation();
                Slot.FloatPosition = FMath::VInterpTo(FVector(Slot.FloatPosition), FVector(Mouth.X, Mouth.Y, LakeFish::SurfaceZ), DeltaSeconds, 2);
            }
        }
        if (ServerNow() >= Slot.EscapeAt) EscapeParked(Rod);
    }
    // Walked away from the open case: it closes.
    if (bCaseOpen && (CasePlace != ECasePlace::Ground || FVector::Dist2D(GetActorLocation(), CaseBase) > Reach + 100))
    {
        bCaseOpen = false;
        ForceNetUpdate();
    }
}

void ALakeCharacter::StowRodsInCase()
{
    if (!HasAuthority()) return;
    for (int32 I = 0; I < Rods.Num(); ++I)
    {
        FLakeRodSlot& Rod = Rods[I];
        Rod.Place = ERodPlace::Case;
        Rod.CaseSlot = (int8)I;
        Rod.Base = FVector::ZeroVector;
        Rod.Yaw = 0;
        Rod.Pitch = 0;
        Rod.State = EFishingState::Idle;
        Rod.LineFish = nullptr;
    }
    RodKind = ERodKind::Medium;
    CasePlace = ECasePlace::Back;
    bCaseOpen = false;
    bCaseWindow = false;
    bRigInHand = false;
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_RODS the angler starts with empty hands: all %d rods in the case"), Rods.Num());
}

void ALakeCharacter::UseRod(ERodKind Kind)
{
    const int32 Rod = (int32)Kind;
    if (!Rods.IsValidIndex(Rod) || (HasRodInHands() && RodKind == Kind)) return;
    if (HasRodInHands())
    {
        // Swap places with the rod wanted, keeping whatever the hands were doing.
        const int32 Held = (int32)RodKind;
        Rods[Held].Place = Rods[Rod].Place == ERodPlace::Case ? ERodPlace::Case : Rods[Rod].Place;
        Rods[Held].CaseSlot = Rods[Rod].CaseSlot;
        Rods[Rod].Place = ERodPlace::Hands;
        Rods[Rod].CaseSlot = -1;
        RodKind = Kind;
        RodVisual = RodVisuals.IsValidIndex(Rod) ? RodVisuals[Rod].Get() : nullptr;
        if (RodVisual) RodVisual->ResetSim();
        return;
    }
    TakeRodInHands(Rod);
}

// --- server requests -----------------------------------------------------------------------

void ALakeCharacter::ServerToggleRod_Implementation()
{
    if (HasRodInHands())
    {
        // R: the rod onto a stand in front of the angler; with the rig in the water it goes on fishing.
        if (FishingState >= EFishingState::Bite || bCharging || bRetrieving || bRigFlying || bFishInHand)
        {
            Status = TEXT("Сейчас удочку на рогатку не поставить.");
            return;
        }
        const FVector Facing = FRotator(0, GetControlRotation().Yaw, 0).Vector();
        const FVector Side = FRotator(0, GetControlRotation().Yaw + 90, 0).Vector();
        FVector Base = GetActorLocation() + Facing * 70;
        if (ALakeEnvironment::IsWater(Base)) Base = GetActorLocation() + Facing * 25;
        // Stands side by side: keep clear of the ones already there.
        for (int32 Try = 0; Try < 4; ++Try)
        {
            bool bClear = true;
            for (const FLakeRodSlot& Other : Rods)
                if (Other.Place == ERodPlace::Stand && FVector::Dist2D(Other.Base, Base) < 45) bClear = false;
            if (bClear) break;
            Base += Side * (Try % 2 ? -110.0f * (Try + 1) / 2 : 55.0f * (Try + 1));
        }
        Base.Z = ALakeEnvironment::GroundHeight(Base.X, Base.Y);
        const bool bFishing = FishingState == EFishingState::Waiting;
        ParkHandRod(ERodPlace::Stand, Base, GetControlRotation().Yaw);
        Status = bFishing ? TEXT("Удочка на рогатке, снасть ловит. Можно достать другую из чехла. R рядом — взять удочку.")
                          : TEXT("Удочка на рогатке. R рядом — взять.");
        return;
    }
    // Empty hands: take the nearest rod from its stand (one with a take first) or the ground.
    if (!TakeNearestRod(true) && !TakeNearestRod(false)) Status = TEXT("Рядом нет удочки на рогатке или на земле.");
}

void ALakeCharacter::ServerLayRod_Implementation(FVector_NetQuantize10 At, float Yaw)
{
    if (!HasRodInHands()) return;
    if (!CanPutRodAway()) { Status = TEXT("Положить удочку на землю можно с оснасткой в руке. ") + WhyRodBusy(*this); return; }
    FVector Ground;
    if (FVector::Dist2D(At, GetActorLocation()) > PlaceReach + 30 || !GroundAt(GetWorld(), FVector2D(At), At.Z, Ground))
    {
        Status = TEXT("Сюда удочку не положить.");
        return;
    }
    // Lying along the slope to its middle (over water it lies on the bank's edge and the surface).
    const FVector2D Mid = FVector2D(Ground) + FVector2D(FRotator(0, Yaw, 0).Vector()) * 300;
    FVector MidGround;
    const float MidZ = GroundAt(GetWorld(), Mid, Ground.Z, MidGround) ? MidGround.Z : FMath::Max(Ground.Z, LakeFish::SurfaceZ);
    const float Pitch = FMath::Clamp(FMath::RadiansToDegrees(FMath::Atan2(MidZ - Ground.Z, 300.0f)), -12.0f, 12.0f);
    ParkHandRod(ERodPlace::Ground, Ground, Yaw, Pitch);
    Status = TEXT("Удочка лежит на земле. Наведите на неё и держите F — взять.");
}

void ALakeCharacter::ServerTakeRod_Implementation(uint8 Rod)
{
    if (!Rods.IsValidIndex(Rod)) return;
    if (HasRodInHands()) { Status = TEXT("Руки заняты: сначала поставьте удочку на рогатку (R) или положите её (держите R)."); return; }
    const FLakeRodSlot& Slot = Rods[Rod];
    if (DistanceToRod(Rod) > Reach)
    {
        Status = TEXT("Подойдите к удочке, чтобы взять её.");
        return;
    }
    TakeRodInHands(Rod);
}

void ALakeCharacter::ServerPlaceCase_Implementation(FVector_NetQuantize10 At, float Yaw)
{
    if (CasePlace != ECasePlace::Back) return;
    FVector Ground;
    if (FVector::Dist2D(At, GetActorLocation()) > PlaceReach + 30 || !GroundAt(GetWorld(), FVector2D(At), At.Z, Ground))
    {
        Status = TEXT("Сюда чехол не положить.");
        return;
    }
    CasePlace = ECasePlace::Ground;
    CaseBase = Ground;
    CaseYaw = Yaw;
    bCaseOpen = false;
    Status = TEXT("Чехол лежит на земле. Наведите на него: «Открыть» или «Надеть на спину» (колесо, держите F).");
    ForceNetUpdate();
}

void ALakeCharacter::ServerCaseAction_Implementation(uint8 Action)
{
    if (Action == 1) { bCaseOpen = false; ForceNetUpdate(); return; }
    if (CasePlace != ECasePlace::Ground || FVector::Dist2D(GetActorLocation(), CaseBase) > Reach + 40)
    {
        Status = TEXT("Подойдите к чехлу.");
        return;
    }
    if (Action == 0)
    {
        bCaseOpen = true;
        Status = TEXT("Чехол открыт: перетаскивайте удочки мышью между чехлом и рукой. Esc — закрыть.");
    }
    else if (Action == 2)
    {
        CasePlace = ECasePlace::Back;
        bCaseOpen = false;
        Status = TEXT("Чехол за спиной. B — снять и положить.");
    }
    ForceNetUpdate();
}

void ALakeCharacter::ServerMoveRod_Implementation(uint8 Rod, uint8 Target)
{
    if (!Rods.IsValidIndex(Rod) || Target > 3) return;
    if (!bCaseOpen || CasePlace != ECasePlace::Ground || FVector::Dist2D(GetActorLocation(), CaseBase) > Reach + 60) return;
    FLakeRodSlot& Moved = Rods[Rod];
    auto InSlot = [this](int32 Slot) { for (int32 R = 0; R < Rods.Num(); ++R) if (Rods[R].Place == ERodPlace::Case && Rods[R].CaseSlot == Slot) return R; return -1; };
    // The rod in the hands into a compartment (the hands take whatever lay there).
    auto Stow = [this](int32 Slot)
    {
        const int32 Hand = (int32)RodKind;
        ParkHandRod(ERodPlace::Case, FVector::ZeroVector, 0);
        Rods[Hand].CaseSlot = (int8)Slot;
        Status = FString::Printf(TEXT("Убрали в чехол %s удилище."), RodName((ERodKind)Hand));
    };
    if (Target == 3)
    {
        if (Moved.Place != ERodPlace::Case) return;
        if (HasRodInHands())
        {
            // Swap: the rod in the hands goes where this one lay.
            if (!CanPutRodAway()) { Status = WhyRodBusy(*this); return; }
            const int32 Slot = Moved.CaseSlot;
            Moved.CaseSlot = -1;
            Stow(Slot);
        }
        TakeRodInHands(Rod);
        ForceNetUpdate();
        return;
    }
    const int32 Occupant = InSlot(Target);
    if (Moved.Place == ERodPlace::Hands)
    {
        if (!CanPutRodAway()) { Status = WhyRodBusy(*this); return; }
        if (Occupant >= 0) Rods[Occupant].CaseSlot = -1;
        Stow(Target);
        if (Occupant >= 0) TakeRodInHands(Occupant);
    }
    else if (Moved.Place == ERodPlace::Case && Occupant != Rod)
    {
        if (Occupant >= 0) Rods[Occupant].CaseSlot = Moved.CaseSlot;
        Moved.CaseSlot = (int8)Target;
    }
    ForceNetUpdate();
}

// --- looks ------------------------------------------------------------------------------------

void ALakeCharacter::UpdateRodVisuals(float DeltaSeconds)
{
    const float Now = ServerNow();
    FVector Wind = FVector::ZeroVector;
    if (const ALakeWeather* Weather = ALakeWeather::Get(GetWorld())) Wind = Weather->WindVelocity();
    for (int32 Rod = 0; Rod < RodVisuals.Num() && Rod < Rods.Num(); ++Rod)
    {
        ALakeRod* Visual = RodVisuals[Rod];
        if (!Visual) continue;
        const FLakeRodSlot& Slot = Rods[Rod];
        if (Slot.Place != RodShownPlace[Rod])
        {
            // Out of the case or into it the rod simply appears or goes; between the hands, a stand
            // and the ground it eases over from where it was.
            const bool bCase = Slot.Place == ERodPlace::Case || RodShownPlace[Rod] == ERodPlace::Case;
            RodBlend[Rod] = bCase ? 0.0f : 1.0f;
            if (RodShownPlace[Rod] == ERodPlace::Case) Visual->ResetSim();
            RodShownPlace[Rod] = Slot.Place;
        }
        RodBlend[Rod] = FMath::Max(0.0f, RodBlend[Rod] - DeltaSeconds / 0.45f);
        Visual->SetActorHiddenInGame(Slot.Place == ERodPlace::Case);
        if (Slot.Place == ERodPlace::Case || Slot.Place == ERodPlace::Hands) continue;   // (the hands: UpdateVisuals)
        const FTransform Target = Slot.Place == ERodPlace::Stand ? StandHoldAt(Slot.Base, Slot.Yaw) : GroundHoldAt(Slot.Base, Slot.Yaw, Slot.Pitch);
        FTransform Hold = Target;
        if (RodBlend[Rod] > 0) Hold.Blend(Target, RodShownHold[Rod], RodBlend[Rod]);
        RodShownHold[Rod] = Hold;
        FRodInput In;
        In.Hold = Hold;
        In.FishingState = Slot.State;
        In.Bait = Slot.Bait;
        In.FloatTarget = Slot.FloatPosition;
        In.HookDepth = Slot.HookDepth;
        In.Facing = FRotator(0, Slot.Yaw, 0).Vector();
        In.Wind = Wind;
        In.Nibbles = Slot.Nibbles;
        In.BiteMotion = Slot.BiteMotion;
        In.BiteElapsed = Now - Slot.BiteStart;
        In.BiteDuration = Slot.BiteDuration;
        In.BiteAmount = Slot.BiteAmount;
        In.bOnStand = Slot.Place == ERodPlace::Stand;
        In.bOnGround = Slot.Place == ERodPlace::Ground;
        In.StandBase = Slot.Base;
        In.StandYaw = Slot.Yaw;
        In.LineFish = Slot.LineFish;
        In.RodHeft = FMath::Sqrt(RodGrams((ERodKind)Rod) / RodGrams(ERodKind::Medium));
        // On the ground the hook is caught on the handle's ring, the rig drawn along the rod.
        In.HookHold = Slot.Place == ERodPlace::Ground ? 1.0f : 0.0f;
        In.HandPoint = Hold.TransformPosition(FVector(ALakeRod::HandleLength + 4, 0, 2));
        Visual->UpdateRod(In, DeltaSeconds);
    }
}

void ALakeCharacter::UpdateCaseVisual(float DeltaSeconds)
{
    if (!CaseVisual) return;
    TArray<int32> SlotOf;
    for (const FLakeRodSlot& Slot : Rods) SlotOf.Add(Slot.Place == ERodPlace::Case ? Slot.CaseSlot : -1);
    const bool bOnBack = CasePlace == ECasePlace::Back;
    FTransform Where;
    if (bOnBack)
    {
        // Across the back from the right shoulder down to the left hip, the lid outwards; it follows
        // the chest bone as the angler bends and walks.
        const FRotator Body = GetActorRotation();
        const FVector Up = Body.RotateVector(FVector(0, 0.44f, 0.9f).GetSafeNormal());
        const FVector Out = Body.RotateVector(FVector(-1, 0, 0));
        FVector Back = GetActorTransform().TransformPosition(FVector(-17, 0, 36));
        if (USkeletalMeshComponent* Skel = GetMesh(); Skel && Skel->GetBoneIndex(TEXT("spine_03")) != INDEX_NONE)
            Back = Skel->GetBoneLocation(TEXT("spine_03")) + Out * 15;
        Where = FTransform(FRotationMatrix::MakeFromXZ(Up, Out).Rotator(), Back);
    }
    else Where = FTransform(FRotator(0, CaseYaw, 0), CaseBase);
    CaseVisual->Show(Where, bOnBack, bCaseOpen, SlotOf, DeltaSeconds);
}

// --- the owner's interface ------------------------------------------------------------------

bool ALakeCharacter::GroundAt(const UWorld* World, FVector2D At, float NearZ, FVector& OutPoint, FVector* OutNormal)
{
    if (OutNormal) *OutNormal = FVector::UpVector;
    FHitResult Hit;
    const FVector From(At, NearZ + 150), To(At, NearZ - 400);
    const bool bHit = World && World->LineTraceSingleByObjectType(Hit, From, To, FCollisionObjectQueryParams(ECC_WorldStatic));
    OutPoint = bHit ? Hit.ImpactPoint : FVector(At, LakeMap::Ground(At.X, At.Y));
    if (bHit && OutNormal) *OutNormal = Hit.ImpactNormal;
    // Not into the water (the pier's deck is fine).
    return !(LakeMap::IsWater(At.X, At.Y) && !LakeMap::OnPier(OutPoint));
}

bool ALakeCharacter::AimGround(FVector& OutPoint, FVector& OutNormal) const
{
    const APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC) return false;
    FVector Eye;
    FRotator View;
    PC->GetPlayerViewPoint(Eye, View);
    FHitResult Hit;
    FCollisionQueryParams Params(TEXT("LakeAim"), false, this);
    const FVector Dir = View.Vector();
    const bool bHit = GetWorld()->LineTraceSingleByObjectType(Hit, Eye, Eye + Dir * 900, FCollisionObjectQueryParams(ECC_WorldStatic), Params);
    // The water has no collision: a view going under its surface stops on it (and there nothing is laid).
    if (Dir.Z < -0.01f && Eye.Z > LakeFish::SurfaceZ)
    {
        const FVector OnSurface = Eye + Dir * ((LakeFish::SurfaceZ - Eye.Z) / Dir.Z);
        if (LakeMap::IsWater(OnSurface.X, OnSurface.Y) && !LakeMap::OnPier(OnSurface) && (!bHit || Hit.ImpactPoint.Z < LakeFish::SurfaceZ))
        {
            OutPoint = OnSurface;
            OutNormal = FVector::UpVector;
            return FVector::Dist(Eye, OnSurface) < 900;
        }
    }
    if (!bHit) return false;
    OutPoint = Hit.ImpactPoint;
    OutNormal = Hit.ImpactNormal;
    return true;
}

void ALakeCharacter::SetCaseWindowInput(bool bOpen)
{
    if (bWindowInput == bOpen) return;
    bWindowInput = bOpen;
    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC) return;
    // The window takes the mouse: a cursor to drag the rods, the view stands still.
    PC->bShowMouseCursor = bOpen;
    if (bOpen)
    {
        PC->SetIgnoreLookInput(true);
        FInputModeGameAndUI Mode;
        Mode.SetHideCursorDuringCapture(false);
        Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
        PC->SetInputMode(Mode);
        int32 W = 0, H = 0;
        PC->GetViewportSize(W, H);
        PC->SetMouseLocation(W / 2, H / 2);
    }
    else
    {
        PC->ResetIgnoreLookInput();
        PC->SetInputMode(FInputModeGameOnly());
    }
}

void ALakeCharacter::CloseCaseWindow()
{
    bCaseWindow = false;
    if (bCaseOpen) ServerCaseAction(1);
}

void ALakeCharacter::UpdateLocalInterface(float DeltaSeconds)
{
    APlayerController* PC = Cast<APlayerController>(GetController());
    if (!PC) return;
    const float Now = RealNow(GetWorld());
    // The window shows while the server keeps the case open for us (a second to answer the request).
    if (bCaseWindow && !bCaseOpen && Now - CaseWindowAsked > 1.0f) bCaseWindow = false;
    SetCaseWindowInput(IsCaseWindowOpen());

    // R held two seconds: the rod's ghost to lay it on the ground.
    if (LayDown >= 0 && !bLayDone && Now - LayDown >= LayHold)
    {
        bLayDone = true;
        if (CanPutRodAway() && !IsCaseWindowOpen()) Placing = EPlacing::Rod;
        else if (HasRodInHands()) Status = TEXT("Положить удочку на землю можно с оснасткой в руке. ") + WhyRodBusy(*this);
    }
    if (Placing == EPlacing::Case && CasePlace != ECasePlace::Back) Placing = EPlacing::None;
    if (Placing == EPlacing::Rod && !CanPutRodAway()) Placing = EPlacing::None;

    FVector Eye;
    FRotator View;
    PC->GetPlayerViewPoint(Eye, View);
    const FVector Dir = View.Vector();
    if (Placing != EPlacing::None)
    {
        // The ghost goes where the angler looks, on the ground within reach.
        FVector Point, Normal;
        const bool bHit = AimGround(Point, Normal);
        if (!bHit) Point = GetActorLocation() + FRotator(0, View.Yaw, 0).Vector() * 150 - FVector(0, 0, 90);
        PlaceAt = Point;
        // Along the way the angler faces (the rod points out from him; the case lies across).
        const float Facing = PC->GetControlRotation().Yaw;
        PlaceYaw = Placing == EPlacing::Case ? Facing + 90 : Facing;
        bPlaceValid = false;
        if (!bHit) PlaceProblem = TEXT("Смотрите на землю рядом");
        else if (FVector::Dist2D(Point, GetActorLocation()) > PlaceReach) PlaceProblem = TEXT("Слишком далеко");
        else if (LakeMap::IsWater(Point.X, Point.Y) && !LakeMap::OnPier(Point)) PlaceProblem = TEXT("В воду нельзя");
        else if (Normal.Z < 0.8f) PlaceProblem = TEXT("Слишком круто");
        else { bPlaceValid = true; PlaceProblem.Reset(); }
    }
    if (!Ghost) Ghost = GetWorld()->SpawnActor<ALakeGhost>();
    if (Ghost)
    {
        const FTransform Where = Placing == EPlacing::Rod ? GroundHoldAt(PlaceAt, PlaceYaw, 0) : FTransform(FRotator(0, PlaceYaw, 0), PlaceAt);
        Ghost->Show(Placing == EPlacing::None ? 0 : Placing == EPlacing::Case ? 1 : 2, Where, bPlaceValid);
    }

    // What the crosshair is on: the case lying near, or a rod on the ground.
    const bool bWasCase = bAimCase;
    const int32 WasRod = AimRod;
    bAimCase = false;
    AimRod = -1;
    if (Placing == EPlacing::None && !IsCaseWindowOpen())
    {
        float Best = 420;   // cm along the view
        float T = 0;
        if (CasePlace == ECasePlace::Ground && FVector::Dist2D(GetActorLocation(), CaseBase) <= Reach)
        {
            const FVector Along = FRotator(0, CaseYaw, 0).Vector() * (ALakeRodCase::Length / 2 - 5);
            const FVector Mid = FVector(CaseBase) + FVector(0, 0, ALakeRodCase::Height / 2);
            if (RayToSegment(Eye, Dir, Mid - Along, Mid + Along, T) < 24 && T > 0 && T < Best) { Best = T; bAimCase = true; }
        }
        for (int32 Rod = 0; Rod < Rods.Num(); ++Rod)
        {
            const FLakeRodSlot& Slot = Rods[Rod];
            if (Slot.Place != ERodPlace::Ground || DistanceToRod(Rod) > Reach) continue;
            const FVector A = GroundHoldAt(Slot.Base, Slot.Yaw, Slot.Pitch).GetLocation();
            const FVector B = GroundHoldAt(Slot.Base, Slot.Yaw, Slot.Pitch).TransformPosition(FVector(300, 0, 0));
            if (RayToSegment(Eye, Dir, A, B, T) < 18 && T > 0 && T < Best) { Best = T; bAimCase = false; AimRod = Rod; }
        }
    }
    AimOptions.Reset();
    if (bAimCase) { AimOptions.Add(TEXT("Открыть")); AimOptions.Add(TEXT("Надеть на спину")); }
    else if (AimRod >= 0) AimOptions.Add(FString::Printf(TEXT("Взять удочку (%s)"), RodName((ERodKind)AimRod)));
    if (bAimCase != bWasCase || AimRod != WasRod) { AimChoice = 0; InteractDown = -1; }
    AimChoice = AimOptions.Num() ? FMath::Clamp(AimChoice, 0, AimOptions.Num() - 1) : 0;

    // F held half a second: do what the prompt shows.
    if (InteractDown >= 0 && Now - InteractDown >= InteractHold)
    {
        InteractDown = -1;
        if (bAimCase && AimChoice == 0) { bCaseWindow = true; CaseWindowAsked = Now; ServerCaseAction(0); }
        else if (bAimCase && AimChoice == 1) ServerCaseAction(2);
        else if (AimRod >= 0) ServerTakeRod((uint8)AimRod);
    }
    if (!bAimCase && AimRod < 0) InteractDown = -1;
}

// --- keys --------------------------------------------------------------------------------------

void ALakeCharacter::CaseKey()
{
    // B: take the case off the back and choose where to lay it (B again or RMB: changed one's mind).
    if (Placing == EPlacing::Case) { Placing = EPlacing::None; return; }
    if (IsCaseWindowOpen()) return;
    if (CasePlace == ECasePlace::Back) { Placing = EPlacing::Case; return; }
    Status = TEXT("Чехол лежит на земле: наведите на него и выберите «Надеть на спину» (колесо, держите F).");
}

void ALakeCharacter::InteractPressed()
{
    if (IsCaseWindowOpen()) { CloseCaseWindow(); return; }
    if (bAimCase || AimRod >= 0) InteractDown = RealNow(GetWorld());
}

void ALakeCharacter::InteractReleased() { InteractDown = -1; }

void ALakeCharacter::LayPressed()
{
    if (Placing != EPlacing::None || IsCaseWindowOpen()) return;
    LayDown = RealNow(GetWorld());
    bLayDone = false;
}

void ALakeCharacter::LayReleased()
{
    // A short press: the stand (put the rod on it, or take one from it).
    if (LayDown >= 0 && !bLayDone) ServerToggleRod();
    LayDown = -1;
}

void ALakeCharacter::WheelUp()
{
    if (AimOptions.Num() > 1) { AimChoice = (AimChoice + AimOptions.Num() - 1) % AimOptions.Num(); InteractDown = -1; return; }
    if (IsCaseWindowOpen()) return;
    Shallower();
}

void ALakeCharacter::WheelDown()
{
    if (AimOptions.Num() > 1) { AimChoice = (AimChoice + 1) % AimOptions.Num(); InteractDown = -1; return; }
    if (IsCaseWindowOpen()) return;
    Deeper();
}
