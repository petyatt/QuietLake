#include "LakeFishFight.h"
#include "LakeWaves.h"
#include "LakeFish.h"
#include "LakeGame.h"
#include "LakeLife.h"
#include "LakeMap.h"

void FLakeFishFight::Start()
{
    FightPhase = EFightPhase::Pause;
    FightRemaining = 0;
    FightPulses = 0;
    bFightJump = bShoreTried = bShoreRun = false;
    bFirstRunDone = false;
    bWasSlack = false;
    NextEscape = 0;
    SurfaceRoll = 0;
    HookGrip = 1; HookedWater = 1;
    StartSwimmer(Fish->GetActorForwardVector());
    SwimAt = Fish->GetActorLocation();
    SwimPose = Fish->GetActorQuat();
    SwimVel = Fish->Vel;
    bFightAwake = false;   // it feels the strike as the line pulls, not before
    NextFightJump = Fish->SimTime;
    PendingLineForce = FVector::ZeroVector;
    PendingLineSpring = 0;
}

void FLakeFishFight::LiftOut()
{
    bFightJump = false;
    Fish->bLifted = true;
    HangTime = 0;
    Fish->Vel = FVector::ZeroVector;
    PendingLineForce = FVector::ZeroVector;
    PendingLineSpring = 0;
}

void FLakeFishFight::LowerIntoWater(FVector LineVelocity)
{
    bFightJump = false;
    Fish->bLifted = false;
    Fish->State = ALakeFish::EState::Hooked;
    SurfaceRoll = Fish->Roll = Fish->GetActorRotation().Roll;
    // (Its body goes on as it hung and swung; the line's speed is already in it.)
    Fish->Vel = bSwimming ? Swim.Velocity().GetClampedToMaxSize(200) : LineVelocity.GetClampedToMaxSize(200);
    if (bSwimming) Swim.SetVelocity(Fish->Vel);
    SwimVel = Fish->Vel;
    PendingLineForce = FVector::ZeroVector;
    PendingLineSpring = 0;
    Fish->ForceNetUpdate();
}

FFishImmersion FLakeFishFight::ImmersionAt(FVector Mouth, FQuat Rotation) const
{
    const float Length = LakeFish::LengthCm(Fish->Species, Fish->WeightKg);
    const FVector Forward = Rotation.GetForwardVector();
    const FVector Right = Rotation.GetRightVector(), Up = Rotation.GetUpVector();
    const float Height = Length * (Fish->Species == EFishSpecies::Pike ? 0.075f : Fish->Species == EFishSpecies::Crucian ? 0.17f : 0.12f);
    const float Width = Length * 0.065f;
    FFishImmersion Result;
    float Volume = 0;
    for (int32 I = 0; I < 7; ++I)
    {
        const float Along = (I + 0.5f) / 7.0f;
        const float Section = FMath::Sin(PI * Along);
        const float Weight = Section * Section;
        const FVector Offset = -Forward * (Length * Along * 0.9f);
        const FVector Centre = Mouth + Offset;
        const float RadiusZ = FMath::Max(0.5f, Section * FMath::Sqrt(FMath::Square(Up.Z * Height) + FMath::Square(Right.Z * Width)));
        const float Level = FMath::Clamp(float(ULakeWaves::SurfaceAt(Fish->GetWorld(), FVector2D(Centre)) - Centre.Z) / RadiusZ, -1.0f, 1.0f);
        const float Wet = LakeMap::IsWater(Centre.X, Centre.Y) ? 0.5f + 0.75f * Level - 0.25f * Level * Level * Level : 0.0f;
        Result.Fraction += Weight * Wet;
        Result.BuoyantMoment += FVector::CrossProduct(Offset, FVector::UpVector) * (Weight * Wet);
        Volume += Weight;
    }
    Result.Fraction /= Volume;
    Result.BuoyantMoment /= Volume;
    return Result;
}

void FLakeFishFight::HangOnLine(FVector Mouth, FVector LineUp, float DeltaSeconds)
{
    LastHangFrame = GFrameCounter;
    const float Dt = FMath::Clamp(DeltaSeconds, 0.0001f, 0.05f);
    HangTime += Dt;
    // Out of the water it fights in bouts: hard thrashing, then a moment hanging still.
    const float Want = FMath::Sin(HangTime * 1.7f + Fish->Phase) > 0.82f ? Fish->StaminaFraction() * 0.7f : 0.04f;
    HangEffort = FMath::FInterpTo(HangEffort, Want, Dt, 3.0f);
    // The hook holds its mouth; the rest is its swimmer: its weight out of the water, the water
    // holding up what is still in it. Thrashing, the whole body bends one way and the other.
    const float Thrash = FMath::Clamp(HangEffort * 1.6f, 0.0f, 1.0f);
    Swim.Surface = LakeMap::IsWater(Mouth.X, Mouth.Y) ? ULakeWaves::SurfaceAt(Fish->GetWorld(), FVector2D(Mouth)) : -1e9f;
    Swim.Floor = -1e9f;
    SwimHeldByMouth(Mouth, Thrash, Thrash > 0.25f ? (FMath::Fmod(HangTime * 4, 1.0f) < 0.5f ? 1.0f : -1.0f) : 0.0f, 0, Dt);
    PoseFromSwimmer(0.0f);
}

void FLakeFishFight::ChooseFightTarget(bool bFinalRun)
{
    ALakeCharacter* Angler = Fish->TargetAngler.Get();
    if (!Angler) return;
    const FFishSpeciesInfo& S = LakeFish::Info(Fish->Species);
    const FFishFightStyle& F = S.Fight;
    const FVector P = Fish->GetActorLocation();
    FVector Away = (P - Angler->GetActorLocation()).GetSafeNormal2D();
    if (Away.IsNearlyZero()) Away = FVector(LakeMap::DeeperDirection(P.X, P.Y), 0);
    const FVector Along = FVector::CrossProduct(Away, FVector::UpVector) * (Fish->Random.FRand() < 0.5f ? 1 : -1);
    const float Pick = Fish->Random.FRand() * (F.AlongWeight + F.AwayWeight + F.DiveWeight + F.SurfaceWeight);
    const bool bAlong = Pick < F.AlongWeight;
    const bool bDive = Pick >= F.AlongWeight + F.AwayWeight && Pick < F.AlongWeight + F.AwayWeight + F.DiveWeight;
    const bool bSurface = Pick >= F.AlongWeight + F.AwayWeight + F.DiveWeight;
    const FVector Dir = bFinalRun ? (Away + FVector(LakeMap::DeeperDirection(P.X, P.Y), 0)).GetSafeNormal()
        : bAlong ? (Along + Away * 0.25f).GetSafeNormal() : (Away + Along * Fish->Random.FRandRange(-0.5f, 0.5f)).GetSafeNormal();
    Fish->RoamTarget = P + Dir * Fish->Random.FRandRange(300, 700) * (0.4f + Fish->StaminaFraction());
    // A target on dry land would make the fish push into an invisible wall throughout a bout.
    for (int32 I = 0; I < 8 && (!LakeMap::IsWater(Fish->RoamTarget.X, Fish->RoamTarget.Y) || LakeMap::Depth(Fish->RoamTarget.X, Fish->RoamTarget.Y) < 20); ++I)
        Fish->RoamTarget = (Fish->RoamTarget + P) * 0.5f;
    const float Bottom = ALakeFish::BottomZ(Fish->RoamTarget.X, Fish->RoamTarget.Y) + 8;
    const float Fresh = Fish->StaminaFraction();
    // A fish with strength sits deeper than its usual band and bores down; only a tired one comes up
    // and lets itself be landed. The species' own layer stays in the choice (a rudd keeps higher
    // than a bream even while fresh).
    const float Band = FMath::Clamp((S.DepthMin + S.DepthMax) * 0.5f, 0.0f, 1.0f);
    const float BandZ = FMath::Lerp(LakeFish::SurfaceZ - 12, Bottom, Band);
    Fish->RoamTarget.Z = Fresh < 0.3f || bSurface ? LakeFish::SurfaceZ - 12
        : bDive || bFinalRun ? Bottom + 6
        : FMath::Lerp(BandZ, Bottom + 6, 0.55f * FMath::SmoothStep(0.3f, 0.9f, Fresh));
    Fish->RoamTarget.Z = FMath::Clamp(Fish->RoamTarget.Z, Bottom, FMath::Max(Bottom, LakeFish::SurfaceZ - 8));
}

void FLakeFishFight::ChooseEscapeTarget()
{
    // Where it would rather be, a few metres off: weeds and snags to tangle the line in, deeper water,
    // anywhere but nearer the bank — or now and then straight back at the angler, which gives it slack.
    const ALakeCharacter* Angler = Fish->TargetAngler.Get();
    const FVector P = Fish->GetActorLocation();
    FVector Best = P;
    float BestScore = -1e9f;
    const bool bBack = Angler && Fish->Random.FRand() < 0.2f;
    for (int32 I = 0; I < 10; ++I)
    {
        const float A = 2 * PI * (I + Fish->Random.FRand()) / 10;
        const float R = Fish->Random.FRandRange(250.0f, 550.0f);
        const FVector2D Q = FVector2D(P) + FVector2D(FMath::Cos(A), FMath::Sin(A)) * R;
        if (!LakeMap::IsWater(Q.X, Q.Y) || LakeMap::Depth(Q.X, Q.Y) < 35) continue;
        const LakeMap::FZones Z = LakeMap::ZonesAt(Q.X, Q.Y);
        float Score = 1.2f * LakeMap::WeedAmount(Q) + 1.5f * Z.Cover + 0.5f * FMath::Min(3.0f, LakeMap::Depth(Q.X, Q.Y) / 100.0f)
            + Fish->Random.FRandRange(0.0f, 0.9f);
        if (Angler)
        {
            const float Toward = FVector2D::DotProduct((Q - FVector2D(P)).GetSafeNormal(), (FVector2D(Angler->GetActorLocation()) - FVector2D(P)).GetSafeNormal());
            Score += bBack ? 2.5f * Toward : -0.6f * FMath::Max(0.0f, Toward);
        }
        if (Score > BestScore) { BestScore = Score; Best = FVector(Q, 0); }
    }
    if (BestScore < -1e8f) { ChooseFightTarget(); return; }
    const float Bottom = ALakeFish::BottomZ(Best.X, Best.Y) + 10;
    Best.Z = FMath::Lerp(Bottom, LakeFish::SurfaceZ - 20.0f, Fish->Random.FRandRange(0.0f, 0.4f));
    Fish->RoamTarget = Best;
}

void FLakeFishFight::FightSplash()
{
    if (ALakeLife* Life = ALakeLife::Get(Fish->GetWorld()))
    {
        const FVector P = Fish->GetActorLocation();
        Life->EmitSign(ELakeSign::Splash, FVector(P.X, P.Y, LakeFish::SurfaceZ), FMath::Min(0.35f + Fish->WeightKg * 0.4f, 1.4f), Fish->Species, Fish->WeightKg, 0, true);
    }
    Fish->NextSign = Fish->SimTime + Fish->Random.FRandRange(0.9f, 2.2f);
}

void FLakeFishFight::StartFightBurst()
{
    const FFishSpeciesInfo& S = LakeFish::Info(Fish->Species);
    const FFishFightStyle& F = S.Fight;
    FightPhase = EFightPhase::Burst;
    const float Base = F.BurstMin > 0 ? Fish->Random.FRandRange(F.BurstMin, F.BurstMax) : S.RunBurst;
    FightRemaining = Base * FMath::Lerp(0.8f, 1.4f, Fish->Size01) * FMath::Lerp(0.55f, 1.0f, Fish->StaminaFraction()) / FightIndividual;
    Fish->BurstTime = FightRemaining;
    if (!bShoreRun && !bFightJump && F.JumpChance > 0 && Fish->SimTime >= NextFightJump && Fish->StaminaFraction() > 0.4f
        && Fish->GetActorLocation().Z > LakeFish::SurfaceZ - 35 && Fish->Random.FRand() < F.JumpChance * Fish->StaminaFraction())
    {
        bFightJump = true;
        NextFightJump = Fish->SimTime + F.JumpCooldown;
        Fish->Vel.Z = F.JumpSpeed * FMath::Lerp(0.8f, 1.0f, Fish->StaminaFraction());
        // The launch has a real cost even when the following airborne motion is passive.
        Fish->Stamina = FMath::Max(0.0f, Fish->Stamina - F.EffortCost * 0.4f);
        FightSplash();
    }
}

bool FLakeFishFight::ReadyToLand()
{
    if (Fish->State != ALakeFish::EState::Hooked || Fish->bLifted) return false;
    const FFishFightStyle& F = LakeFish::Info(Fish->Species).Fight;
    const ALakeCharacter* Angler = Fish->TargetAngler.Get();
    const FVector P = Fish->GetActorLocation();
    const float Fresh = Fish->StaminaFraction();
    if (!bShoreTried && !bFightJump && F.ShoreRunChance > 0 && Fish->StateTime > 3 && Fresh > 0.15f && Fresh < 0.6f
        && Angler && FVector::Dist2D(P, Angler->GetActorLocation()) < F.ShoreDistance && LakeMap::ShoreDistance(P.X, P.Y) < 450)
    {
        bShoreTried = true;
        if (Fish->Random.FRand() < F.ShoreRunChance * FightIndividual)
        {
            bShoreRun = true;
            FightPulses = 1;
            ChooseFightTarget(true);
            StartFightBurst();
        }
    }
    return !bFightJump && !bShoreRun;
}

void FLakeFishFight::UpdateFightMotion(float DeltaSeconds, FVector& Desired, float& MaxSpeed)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Fish->Species);
    const FFishFightStyle& F = S.Fight;
    if (!bFightAwake)
    {
        // A prick causes a modest swimming response; full escape bouts require a load.
        MaxSpeed = S.CruiseSpeed * 0.65f;
        Desired = Fish->GetActorForwardVector() * MaxSpeed;
        Fish->BurstTime = 0;
        return;
    }
    const float Fresh = Fish->StaminaFraction();
    FightRemaining -= DeltaSeconds;
    ReadyToLand();
    // Given slack it is free to go where it likes to shake the hook: for the weeds or a snag, into the
    // deep, along the bank, or even back towards the angler for more slack. It picks again now and then.
    const bool bSlack = PendingLineForce.SizeSquared() < 15 * 15;
    if (bSlack && Fresh > 0.12f && !bFightJump && (!bWasSlack || Fish->SimTime >= NextEscape))
    {
        ChooseEscapeTarget();
        NextEscape = Fish->SimTime + Fish->Random.FRandRange(1.2f, 2.4f);
        if (FightPhase == EFightPhase::Pause || FightPhase == EFightPhase::Gap)
        {
            FightPhase = EFightPhase::Pull;
            FightRemaining = Fish->Random.FRandRange(F.PullMin, F.PullMax);
        }
    }
    bWasSlack = bSlack;
    // The moment it feels the hook it rushes off, down and away: the first run of every fish.
    if (!bFirstRunDone)
    {
        bFirstRunDone = true;
        ChooseFightTarget(true);
        FightPulses = FMath::Max(1, F.SeriesMin);
        StartFightBurst();
    }
    // Hauled up to the surface with strength left, it does not wait for its next bout: it sees the
    // bank, bores down and away at once and thrashes — and tires itself the sooner for it.
    if (!bFightJump && !bShoreRun && FightPhase != EFightPhase::Burst && Fresh > 0.3f && PendingLineForce.Z > 30.0f + Fish->WeightKg * 60.0f
        && Fish->MouthLocation().Z > LakeFish::SurfaceZ - LakeFish::LengthCm(Fish->Species, Fish->WeightKg) * 0.35f)
    {
        ChooseFightTarget(true);
        FightPulses = 1;
        StartFightBurst();
        if (Fish->SimTime >= Fish->NextSign) FightSplash();
    }
    if (FightRemaining <= 0 && !bFightJump)
    {
        if (FightPhase == EFightPhase::Burst && --FightPulses > 0)
        {
            FightPhase = EFightPhase::Gap;
            FightRemaining = F.SeriesGap / FightIndividual;
        }
        else if (FightPhase == EFightPhase::Gap) StartFightBurst();
        else if (FightPhase == EFightPhase::Pull || FightPhase == EFightPhase::Burst)
        {
            bShoreRun = false;
            FightPhase = EFightPhase::Pause;
            FightRemaining = Fish->Random.FRandRange(F.PauseMin, F.PauseMax) * FMath::Lerp(1.8f, 1.0f, Fresh) / FightIndividual;
        }
        else
        {
            ChooseFightTarget();
            if (Fish->Random.FRand() < F.BurstChance * Fresh * FightIndividual)
            {
                FightPulses = FMath::Max(1, FMath::RoundToInt(Fish->Random.RandRange(F.SeriesMin, F.SeriesMax) * FMath::Lerp(0.4f, 1.0f, Fresh)));
                StartFightBurst();
            }
            else
            {
                FightPhase = EFightPhase::Pull;
                FightRemaining = Fish->Random.FRandRange(F.PullMin, F.PullMax) * FMath::Lerp(0.8f, 1.3f, Fish->Size01) / FightIndividual;
            }
        }
    }
    Fish->BurstTime = FightPhase == EFightPhase::Burst ? FMath::Max(0.0f, FightRemaining) : 0;
    const bool bRest = FightPhase == EFightPhase::Pause || FightPhase == EFightPhase::Gap;
    const float Pace = bRest ? F.PauseSpeed : FightPhase == EFightPhase::Burst ? 1.0f : F.PullSpeed;
    MaxSpeed = S.BurstSpeed * Pace * FMath::Lerp(0.12f, 1.0f, Fresh) * FightIndividual;
    Desired = (Fish->RoamTarget - Fish->GetActorLocation()).GetSafeNormal() * MaxSpeed;
    // A hooked fish pulls against the line whatever way it meant to go: drawn up it bores down, drawn
    // in it heads out. Its own way (along the bank, for the weeds, up to the top) only bends that.
    if (!bFightJump && PendingLineForce.SizeSquared() > 400)
    {
        FVector Way = Desired.GetSafeNormal() * 0.45f - PendingLineForce.GetSafeNormal() * 0.75f;
        // Above all it wants the depth: a fish with strength left keeps to the lower water and bores
        // for the bottom when the line has it near the top, rather than skimming away along the
        // surface at the line's end. Tiring, it lets itself come up.
        const FVector Here = Fish->GetActorLocation();
        const float Column = FMath::Max(20.0f, LakeFish::SurfaceZ - ALakeFish::BottomZ(Here.X, Here.Y));
        // (Spent, it no longer keeps down: it lets itself be drawn up and rolls at the top.)
        const float Keeps = Column * FMath::Lerp(0.0f, 0.8f, FMath::SmoothStep(0.15f, 0.8f, Fresh));
        const float Shallow = Keeps < 5.0f ? 0.0f : FMath::Clamp((Keeps - (LakeFish::SurfaceZ - (float)Here.Z)) / 50.0f, 0.0f, 1.5f);
        Way.Z -= 1.3f * Shallow;
        Desired = Way.GetSafeNormal() * MaxSpeed;
    }
    if (Fish->Effort > 0.4f && Fish->Vel.Size() > 30 && Fish->SimTime >= NextFightDisturb)
    {
        if (auto* Life = ALakeLife::Get(Fish->GetWorld())) Life->Disturb(Fish->GetActorLocation(), 120 + Fish->WeightKg * 100, FMath::Min(1.0f, Fish->Effort * (float)Fish->Vel.Size() / 100));
        NextFightDisturb = Fish->SimTime + 1;
    }
    if (Fish->GetActorLocation().Z > LakeFish::SurfaceZ - 30 && Fish->Effort > 0.35f && Fish->SimTime >= Fish->NextSign) FightSplash();
}

namespace
{
    // Drawn back through the water: from this speed towards the line's pull (cm/s) the fish starts
    // to lose its head to the line, from this one it has lost it.
    constexpr float DrawnFrom = 5, DrawnFull = 25;
    constexpr float HookCorner = 0.04f;   // how far the hook sits off the mouth's axis, of the body's length
}

void FLakeFishFight::MoveHooked(FVector Desired, float MaxSpeed, float DeltaSeconds)
{
    // On the hook the fish is its swimmer (FLakeSwimmer): six points from the mouth to the tail fin,
    // muscles in the joints, the water, its weight and the line's pull at the mouth. Nothing here
    // moves the fish but those: where it wants to go (Desired, from UpdateFightMotion) only decides
    // how hard it beats its tail and which way it bends.
    const FFishSpeciesInfo& S = LakeFish::Info(Fish->Species);
    Desired = Desired.GetClampedToMaxSize(MaxSpeed);
    const float BodyLength = LakeFish::LengthCm(Fish->Species, Fish->WeightKg);
    // Moved or thrown from outside (a leap, a test, a teleport): lay the body out anew where it is.
    if (!bSwimming || !Fish->GetActorLocation().Equals(SwimAt, 0.01f) || !Fish->Vel.Equals(SwimVel, 0.01f) || !Fish->GetActorQuat().Equals(SwimPose, 0.0001f))
        StartSwimmer(bFightJump && Fish->Vel.SizeSquared() > 400 ? Fish->Vel.GetSafeNormal() : Fish->GetActorForwardVector());
    const float Fresh = Fish->StaminaFraction();
    const FVector Here = Swim.Centre();
    // The water over the fish, wind waves included: near the top it rides them.
    Swim.Surface = ULakeWaves::SurfaceAt(Fish->GetWorld(), FVector2D(Here));
    Swim.Floor = ALakeFish::BottomZ(Here.X, Here.Y);
    Swim.LineLength = 0;
    // A species' strength is how fast it can beat for its length; a tired fish's muscles give way.
    Swim.BeatScale = SwimBeatScale();
    Swim.Strength = FMath::Square(Swim.BeatScale) * FMath::Lerp(0.3f, 1.0f, Fresh);
    const FVector MouthNow = Swim.P[0];
    // The head at the top or out (0 under, 1 clear): the line holds it up there.
    const float HeadOut = bFightJump ? 0.0f : FMath::SmoothStep(-0.12f * BodyLength, 0.06f * BodyLength, (float)(MouthNow.Z - Swim.Surface));
    const bool bBurst = Fish->BurstTime > 0;
    const bool bRest = FightPhase == EFightPhase::Pause || FightPhase == EFightPhase::Gap;

    const FVector Heading = Swim.Heading();
    const float PitchNow = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(float(Heading.Z), -1.0f, 1.0f)));
    // Drawn back through the water by a line stronger than it, tail first, the fish has lost the fight
    // for its head: it no longer holds its body against the line, and the pull at its mouth swings it
    // round to come head first after the hook. As soon as the line eases it turns away again.
    {
        const float Moving = (float)Fish->Vel.Size();
        const float Towards = FVector::DotProduct(Fish->Vel, PendingLineForce.GetSafeNormal());
        const float TailFirst = Moving > 1 ? float(-FVector::DotProduct(Heading, Fish->Vel / Moving)) : 0.0f;
        const float Now = FMath::SmoothStep(DrawnFrom, DrawnFull, Towards) * FMath::SmoothStep(-0.1f, 0.5f, TailFirst);
        Drawn = FMath::FInterpTo(Drawn, Now, DeltaSeconds, Now > Drawn ? 8.0f : 1.5f);
    }

    float Push = 0, Steering = 0, Climb = 0;
    Swim.WantBack = FVector::ZeroVector;
    const FVector PullDir = Desired.GetSafeNormal();
    if (!bFightJump && !PullDir.IsNearlyZero())
    {
        FVector Want = PullDir;
        // Boring down against a line that draws it up it stands nose down, tail beating, but not
        // straight on its head: it keeps way on, the way it was heading if it has no other. Down by
        // the bottom it levels off; near the top it does not make for the air.
        const float AboveBottom = (float)Here.Z - Swim.Floor;
        const float Steepest = FMath::Lerp(-0.1f, -0.87f, FMath::SmoothStep(10.0f, 45.0f, AboveBottom));
        const float Highest = FMath::Lerp(0.0f, 0.5f, FMath::SmoothStep(0.5f * BodyLength, 2 * BodyLength, LakeFish::SurfaceZ - (float)MouthNow.Z));
        const float Dive = FMath::Clamp(Want.Z, Steepest, Highest);
        FVector Level(Want.X, Want.Y, 0);
        const float Sure = FMath::Clamp(float(Level.Size()) * 2, 0.0f, 1.0f);   // how much of a way across the water it has in mind
        Level = Level.GetSafeNormal() * Sure + FVector(Swim.Steady.X, Swim.Steady.Y, 0).GetSafeNormal() * (1 - Sure);
        if (!Level.Normalize()) Level = Fish->GetActorForwardVector().GetSafeNormal2D();
        Want = Level * FMath::Sqrt(FMath::Max(0.0f, 1 - Dive * Dive)) + FVector(0, 0, Dive);
        // It does not swim out onto the bank or into water too shallow to cover it; the line can
        // draw it there.
        const FVector In(LakeMap::DeeperDirection(MouthNow.X, MouthNow.Y), 0);
        const bool bShallow = !LakeMap::IsWater(MouthNow.X, MouthNow.Y) || LakeFish::SurfaceZ - ALakeFish::BottomZ(MouthNow.X, MouthNow.Y) < FMath::Max(10.0f, 0.3f * BodyLength);
        if (bShallow && FVector::DotProduct(Want, In) < 0.3f) Want = (In + FVector(0, 0, -0.1f));
        Want.Normalize();
        // It bends to turn that way (FLakeSwimmer::SteerTo): how fast that turns it is the water's doing.
        Swim.SteerTo(Want, DeltaSeconds, Steering, Climb);
        // A spent fish, or one with its head out of the water, hardly steers any more: it lies over and
        // lets itself be drawn.
        const float Steers = (1 - Drawn) * FMath::Lerp(0.1f, 1.0f, FMath::SmoothStep(0.0f, 0.3f, Fresh)) * (1 - 0.8f * HeadOut);
        Steering *= Steers;
        Climb *= Steers;
        if (Drawn > 0.5f) Swim.WantBack = FVector::ZeroVector;
        // How hard it beats: as hard as it takes to go as fast as it means to, so held back by the
        // line it does not ease off, and even resting it leans against what draws it away. In a rush
        // it gives all it has, pulling steadily about six tenths of that force, resting less; tired, far less.
        // (The pull grows with the square of the beat, so these are roots of the shares of force.)
        float Most = bBurst ? 1.0f : 0.78f;
        if (bRest && bFightAwake) Most *= FMath::Lerp(0.35f, 0.92f, Fresh);
        Most *= FMath::Sqrt(FMath::Lerp(0.08f, 1.0f, Fresh) * FightIndividual * (1 - 0.6f * HeadOut));
        const float Speed = (float)Desired.Size();
        const float Along = FVector::DotProduct(Fish->Vel, PullDir);
        const float Flat = 6.0f * BodyLength * Swim.BeatScale;   // about what a free fish reaches at full beat
        Push = FMath::Clamp(0.9f * Speed / Flat + (Speed - Along) / (0.25f * Speed + 8), 0.0f, FMath::Min(1.0f, Most));
        // (Dragged backwards it is not swimming anywhere: its tail only flails.)
        Push *= FMath::Lerp(1.0f, 0.4f, Drawn);
    }
    // The line's pull at the mouth, easing within this step as the mouth gives way to it.
    Swim.MouthForce = PendingLineForce;
    Swim.MouthSpring = PendingLineSpring;
    Swim.MouthAt = MouthNow;
    Swim.Advance(DeltaSeconds, Push, Steering, Climb);
    {
        // -SwimDebug: what the hooked fish is doing, four times a second.
        static const bool bDebug = FParse::Param(FCommandLine::Get(), TEXT("SwimDebug"));
        if (bDebug && FMath::Fmod(Fish->SimTime, 0.25f) < DeltaSeconds)
            UE_LOG(LogTemp, Display, TEXT("SWIM t %.2f drawn %.2f phase %d depth %.0f pitch %+.0f want %s head %s push %.2f steer %+.2f climb %+.2f line %s (%.0f gf, spring %.1f) vel %s beat %.1f use %.2f water %.2f stamina %.2f"),
                Fish->StateTime, Drawn, (int32)FightPhase, LakeFish::SurfaceZ - Here.Z, PitchNow, *PullDir.ToCompactString(), *Heading.ToCompactString(), Push, Steering, Climb,
                *PendingLineForce.GetSafeNormal().ToCompactString(), PendingLineForce.Size(), PendingLineSpring, *Fish->Vel.ToCompactString(), Swim.Beat, Swim.MuscleUse, Swim.WaterShare(), Fresh);
    }
    if (bFightJump)
    {
        // A leap: the fish flies as it was thrown, its body along its flight.
        const FVector Flight = Swim.Velocity(), Centre = Swim.Centre();
        if (Flight.SizeSquared() > 400)
        {
            const FVector Dir = Flight.GetSafeNormal();
            const float Kept = Swim.Accumulator;
            Swim.Reset(Centre + Dir * (0.412f * BodyLength), BodyLength, Fish->WeightKg * 1000, Dir, BodyHeightShare());
            Swim.Accumulator = Kept;
            Swim.SetVelocity(Flight);
        }
        if (Centre.Z <= LakeFish::SurfaceZ - 4 && Flight.Z <= 0)
        {
            bFightJump = false;
            FightSplash();
        }
    }
    const float Water = Swim.WaterShare();
    HookedWater = Water;
    Fish->Vel = Swim.Velocity();
    Fish->Effort = Swim.Drive;

    // The hook wears loose when the line hauls against all the fish has.
    const float Opposition = PullDir.IsNearlyZero() ? 0.0f : FMath::Max(0.0f, float(-FVector::DotProduct(PendingLineForce, PullDir)))
        / FMath::Max(1.0f, Fish->WeightKg * S.Acceleration * S.Strength);
    HookGrip -= DeltaSeconds * (1 - HookGrip) * 0.06f * FMath::Max(0.0f, Opposition - 0.8f);
    if (HookGrip <= 0) { Fish->Release(6); return; }
    // What tires it: how hard it beats (its rushes above all). Leaning against a steady pull while it
    // rests is slower, steady work.
    const float Work = FMath::Clamp(FMath::Pow(Push, 1.5f) * (bRest ? 0.5f : 1.0f) + (bRest || bFightJump ? 0.0f : Opposition * 0.15f), 0.0f, 1.0f);
    const float Recover = bRest && !bFightJump ? S.Fight.Recovery * FMath::Max(0.0f, 1 - Work / 0.2f) : 0;
    Fish->Stamina = FMath::Clamp(Fish->Stamina + DeltaSeconds * (Recover - S.Fight.EffortCost * Work), 0.0f, Fish->MaxStamina);
    // Its head held out of the water it gulps air and is soon spent: this is how a fish is subdued.
    if (!bFightJump && Swim.P[0].Z > LakeFish::SurfaceZ + 2) Fish->Stamina = FMath::Max(0.0f, Fish->Stamina - Fish->MaxStamina * 0.12f * DeltaSeconds);

    // Keep the rendered pose aligned with the swimmer. A cosmetic roll here rotates the mesh
    // without rotating the simulated body or its water forces.
    SurfaceRoll = Fish->Roll = 0;
    const float Thrash = HeadOut * FMath::Clamp(Fish->Effort * 1.6f + 0.25f * Fresh, 0.0f, 1.0f);
    if (Thrash > 0.3f && Fish->SimTime >= Fish->NextSign)
    {
        FightSplash();
        Fish->NextSign = Fish->SimTime + Fish->Random.FRandRange(0.3f, 0.75f) / FMath::Max(0.5f, Thrash);
    }
    Fish->Wish = Fish->Vel;
    Fish->WishSpeedNow = Fish->Vel.Size();
    const float YawBefore = Fish->Yaw;
    PoseFromSwimmer(0.0f);
    const FRotator Rotation = Fish->GetActorRotation();
    Fish->YawRate = DeltaSeconds > 0 ? FMath::FindDeltaAngleDegrees(YawBefore, Rotation.Yaw) / DeltaSeconds : 0.0f;
    Fish->Yaw = Rotation.Yaw;
    Fish->Pitch = Rotation.Pitch;
}

float FLakeFishFight::BodyHeightShare() const
{
    // (The depth of the body as ImmersionAt has it: a pike is slender, a crucian deep.)
    return Fish->Species == EFishSpecies::Pike ? 0.15f : Fish->Species == EFishSpecies::Crucian ? 0.34f : Fish->Species == EFishSpecies::Perch ? 0.27f : 0.24f;
}

float FLakeFishFight::SwimBeatScale() const
{
    // The passport's pull (acceleration times strength, about nine tenths of a body weight for a
    // perch) is what the fish pulls steadily; in a rush, at its full beat, 1.6 times that. A swimmer
    // of beat scale 1 pulls about 0.42 of its weight on a tight line (QuietLake.Prototype.Swimmer),
    // and the pull grows with the square of the beat.
    const FFishSpeciesInfo& S = LakeFish::Info(Fish->Species);
    return FMath::Clamp(FMath::Sqrt(1.6f * S.Acceleration * S.Strength / 412.0f), 0.8f, 2.6f);
}

void FLakeFishFight::StartSwimmer(FVector Direction)
{
    const FVector Dir = Direction.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
    Swim.Reset(Fish->MouthLocation(), LakeFish::LengthCm(Fish->Species, Fish->WeightKg), Fish->WeightKg * 1000, Dir, BodyHeightShare());
    const FVector Up = Fish->GetActorUpVector() - Dir * FVector::DotProduct(Fish->GetActorUpVector(), Dir);
    if (Up.SizeSquared() > 0.01f) Swim.Back = Up.GetSafeNormal();
    Swim.SetVelocity(Fish->Vel);
    Swim.Surface = LakeFish::SurfaceZ;
    // (The hook in one corner of the jaw or the other.)
    Swim.HookOffset = (FMath::Sin(Fish->Phase) >= 0 ? 1.0f : -1.0f) * HookCorner * Swim.Length;
    bSwimming = true;
}

void FLakeFishFight::PoseFromSwimmer(float RollDegrees)
{
    // The head along the swimmer's head, its back as the swimmer has it, the mouth where it is.
    const FVector Forward = Swim.Forward();
    if (Forward.IsNearlyZero()) return;
    FQuat Q = FRotationMatrix::MakeFromXZ(Forward, Swim.Back).ToQuat();
    if (RollDegrees != 0) Q = Q * FQuat(FVector::ForwardVector, FMath::DegreesToRadians(RollDegrees));
    Fish->SetActorLocationAndRotation(Swim.P[0] - Q.RotateVector(Fish->MouthOffset()), Q);
    SwimAt = Fish->GetActorLocation();
    SwimPose = Fish->GetActorQuat();
    SwimVel = Fish->Vel;
}

void FLakeFishFight::SwimHeldByMouth(FVector Mouth, float TailEffort, float Steering, float Climb, float DeltaSeconds)
{
    // The mouth is where something else has it (the hook of the angler's rod out of the water, the
    // server's fish on a client): a short stiff line holds the swimmer to it, and the body answers.
    if (!bSwimming) StartSwimmer(Fish->GetActorForwardVector());
    Swim.BeatScale = SwimBeatScale();
    Swim.Strength = FMath::Square(Swim.BeatScale) * FMath::Lerp(0.3f, 1.0f, Fish->StaminaFraction());
    Swim.MouthForce = FVector::ZeroVector;
    Swim.MouthSpring = 0;
    Swim.LineTip = Mouth;
    Swim.LineLength = 0.05f;
    Swim.LineStiffness = 2000 * Fish->WeightKg;
    // (Carried far in one step, a teleport: it goes along whole.)
    if (FVector::DistSquared(Swim.P[0], Mouth) > FMath::Square(0.5f * Swim.Length)) Swim.Shift(Mouth - Swim.P[0]);
    // (The stiff hold stands in for the hook's place, not the line's pull: no jaw-corner couple from it.)
    const float Corner = Swim.HookOffset;
    Swim.HookOffset = 0;
    Swim.Advance(DeltaSeconds, TailEffort, Steering, Climb);
    Swim.HookOffset = Corner;
    Swim.Shift(Mouth - Swim.P[0]);
    Swim.LineLength = 0;
}
