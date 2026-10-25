#include "LakeFish.h"
#include "LakeGame.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

// ---------------------------------------------------------------------------------------------
// Species table. Values are game-tuned but follow the real fish:
//  perch  - schooling mid-water predator, bold, prefers worms, quick head-shaking fight;
//  roach  - schools near reeds in the upper water, maggot and bread, light and cautious;
//  crucian- bottom feeder near weed, slow, very cautious nibbles, bread and maggot, steady pull;
//  pike   - solitary ambush hunter by the reeds, explosive bursts, strong long runs.
// ---------------------------------------------------------------------------------------------
namespace
{
    const FFishSpeciesInfo GSpecies[] = {
        // Name           Mesh                                        Len  K     Wmin  Wmax  Habitat                    Depth      Cruise Burst Turn  Acc  School Sense  Worm Maggot Bread  Nib    Str  Stam  Beat
        { TEXT("Окунь"),  TEXT("/Game/Fish/SM_Fish_Perch.SM_Fish_Perch"),     26, 1.25f, 0.08f, 0.8f, EFishHabitat::OpenWater, 0.3f, 0.75f, 28, 160, 220, 380, 5, 650, {1.0f, 0.55f, 0.08f}, 0, 1, 1.0f, 9,  9 },
        { TEXT("Плотва"), TEXT("/Game/Fish/SM_Fish_Roach.SM_Fish_Roach"),     22, 1.1f,  0.05f, 0.4f, EFishHabitat::Reeds,     0.1f, 0.45f, 24, 130, 200, 320, 7, 450, {0.35f, 1.0f, 0.85f}, 1, 3, 0.7f, 6, 10 },
        { TEXT("Карась"), TEXT("/Game/Fish/SM_Fish_Crucian.SM_Fish_Crucian"), 20, 1.9f,  0.15f, 0.9f, EFishHabitat::Bottom,    0.75f, 1.0f, 12,  80, 110, 160, 1, 350, {0.55f, 0.7f, 1.0f},  3, 6, 0.9f, 14, 6 },
        { TEXT("Щука"),   TEXT("/Game/Fish/SM_Fish_Pike.SM_Fish_Pike"),       62, 0.65f, 0.8f,  3.5f, EFishHabitat::Ambush,    0.3f, 0.7f,  10, 420, 120, 900, 1, 800, {0.18f, 0.05f, 0.0f}, 0, 0, 1.6f, 12, 7 },
    };
    const TCHAR* GBaitNames[] = { TEXT("червь"), TEXT("опарыш"), TEXT("хлеб") };

    float Depth01(float Z, float X, float Y)
    {
        const float Bottom = ALakeFish::BottomZ(X, Y);
        return FMath::Clamp((LakeFish::SurfaceZ - Z) / FMath::Max(1.0f, LakeFish::SurfaceZ - Bottom), 0.0f, 1.0f);
    }
}

const FFishSpeciesInfo& LakeFish::Info(EFishSpecies Species) { return GSpecies[FMath::Clamp((int32)Species, 0, 3)]; }
const TCHAR* LakeFish::BaitName(EBait Bait) { return GBaitNames[FMath::Clamp((int32)Bait, 0, 2)]; }
float LakeFish::LengthCm(EFishSpecies Species, float WeightKg)
{
    return FMath::Pow(WeightKg * 1000.0f * 100.0f / Info(Species).Condition, 1.0f / 3.0f);
}
float LakeFish::BaitInterest(EFishSpecies Species, EBait Bait, float DepthFraction)
{
    const FFishSpeciesInfo& S = Info(Species);
    // Bait outside the species' depth band is found less often (a bottom feeder rarely rises to it).
    const float Mid = (S.DepthMin + S.DepthMax) * 0.5f, Half = (S.DepthMax - S.DepthMin) * 0.5f + 0.15f;
    const float DepthMatch = FMath::Clamp(1.0f - FMath::Max(0.0f, FMath::Abs(DepthFraction - Mid) - Half) * 2.5f, 0.15f, 1.0f);
    return S.BaitLike[FMath::Clamp((int32)Bait, 0, 2)] * DepthMatch;
}
float ALakeFish::BottomZ(float X, float Y) { return ALakeEnvironment::GroundHeight(X, Y); }

ALakeFish::ALakeFish()
{
    PrimaryActorTick.bCanEverTick = true;
    bReplicates = true;
    bAlwaysRelevant = true;   // the lake is small; every angler sees every fish
    SetReplicatingMovement(false);
    SetNetUpdateFrequency(12);
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
    Body->SetupAttachment(RootComponent);
    Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Body->SetCastShadow(true);
}

void ALakeFish::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ALakeFish, Species);
    DOREPLIFETIME(ALakeFish, WeightKg);
    DOREPLIFETIME(ALakeFish, NetLocation);
    DOREPLIFETIME(ALakeFish, NetRotation);
    DOREPLIFETIME(ALakeFish, NetEffort);
}

void ALakeFish::Setup(EFishSpecies InSpecies, float InWeight, FVector Home, int32 InSchool)
{
    Species = InSpecies;
    WeightKg = InWeight;
    HomePoint = Home;
    School = InSchool;
    Random.Initialize(GetUniqueID() * 7919 + (int32)(InWeight * 1000));
    Phase = Random.FRandRange(0, 2 * PI);
    Hunger = Random.FRandRange(0.55f, 1.0f);
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    // Bigger fish of a species hold out longer.
    MaxStamina = S.Stamina * FMath::Lerp(0.7f, 1.4f, FMath::Clamp((WeightKg - S.MinWeight) / (S.MaxWeight - S.MinWeight), 0.0f, 1.0f));
    Stamina = MaxStamina;
    NetLocation = GetActorLocation();
    NetRotation = GetActorRotation();
    ChooseRoamTarget();
    UpdateVisual(0);
}

FVector ALakeFish::MouthLocation() const
{
    return GetActorLocation() + GetActorForwardVector() * LakeFish::LengthCm(Species, WeightKg) * 0.42f;
}

// --- habitat -------------------------------------------------------------------------------

void ALakeFish::ChooseRoamTarget()
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    float RMin = 0.2f, RMax = 0.8f;
    switch (S.Habitat)
    {
    case EFishHabitat::Reeds:  RMin = 0.72f; RMax = 0.9f; break;
    case EFishHabitat::Bottom: RMin = 0.4f;  RMax = 0.88f; break;
    case EFishHabitat::Ambush: RMin = 0.8f;  RMax = 0.9f; break;
    default: break;
    }
    // Schools and ambushers stay around their home area; others wander the whole habitat.
    FVector Target;
    for (int32 Try = 0; Try < 12; ++Try)
    {
        if (S.SchoolSize > 1 || S.Habitat == EFishHabitat::Ambush)
        {
            const float Spread = S.Habitat == EFishHabitat::Ambush ? 250.0f : 900.0f;
            Target = HomePoint + FVector(Random.FRandRange(-Spread, Spread), Random.FRandRange(-Spread, Spread), 0);
        }
        else
        {
            const float A = Random.FRandRange(-PI, PI), R = Random.FRandRange(RMin, RMax);
            Target = FVector(2200 + FMath::Cos(A) * 2200 * R, FMath::Sin(A) * 2800 * R, 0);
        }
        const float R = ALakeEnvironment::LakeRadius(Target.X, Target.Y);
        if (R < RMin * 0.8f || R > 0.92f) continue;
        const float Bottom = BottomZ(Target.X, Target.Y);
        if (LakeFish::SurfaceZ - Bottom < 30) continue;   // too shallow to swim
        Target.Z = FMath::Lerp(LakeFish::SurfaceZ - 10, Bottom + 8, Random.FRandRange(S.DepthMin, S.DepthMax));
        RoamTarget = Target;
        return;
    }
    RoamTarget = FVector(2200, 0, LakeFish::SurfaceZ - 80);
}

FVector ALakeFish::SteerRoam() const
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    const FVector To = RoamTarget - GetActorLocation();
    // Pike hang almost still at their ambush spot; others cruise and slow down on arrival.
    const float Speed = S.Habitat == EFishHabitat::Ambush ? FMath::Min(S.CruiseSpeed, To.Size() * 0.2f)
                                                          : S.CruiseSpeed * FMath::Clamp(To.Size() / 120.0f, 0.3f, 1.0f);
    return To.GetSafeNormal() * Speed;
}

FVector ALakeFish::SteerSchool() const
{
    if (School < 0) return FVector::ZeroVector;
    FVector Centre = FVector::ZeroVector, Heading = FVector::ZeroVector, Separation = FVector::ZeroVector;
    int32 Count = 0;
    for (TActorIterator<ALakeFish> It(GetWorld()); It; ++It)
    {
        const ALakeFish* Other = *It;
        if (Other == this) continue;
        const FVector Offset = GetActorLocation() - Other->GetActorLocation();
        const float Dist = Offset.Size();
        if (Dist < 35 && Dist > 0.1f) Separation += Offset / (Dist * Dist) * 35;   // any species
        if (Other->School != School || Dist > 260) continue;
        Centre += Other->GetActorLocation();
        Heading += Other->Vel;
        ++Count;
    }
    if (Count == 0) return Separation * 20;
    Centre /= Count;
    Heading /= Count;
    return (Centre - GetActorLocation()) * 0.25f + Heading * 0.45f + Separation * 20;
}

FVector ALakeFish::SteerBounds() const
{
    const FVector P = GetActorLocation();
    const float R = ALakeEnvironment::LakeRadius(P.X, P.Y);
    const float Depth = LakeFish::SurfaceZ - BottomZ(P.X, P.Y);
    FVector Push = FVector::ZeroVector;
    if (R > 0.86f || Depth < 35)
    {
        // Back towards open water.
        Push += (FVector(2200, 0, P.Z) - P).GetSafeNormal2D() * 60 * FMath::Max(1.0f, (R - 0.86f) * 30);
    }
    if (P.Z > LakeFish::SurfaceZ - 12) Push.Z -= 40;
    if (P.Z < BottomZ(P.X, P.Y) + 10) Push.Z += 40;
    return Push;
}

// --- reaction to bait ----------------------------------------------------------------------

void ALakeFish::UpdateBait(float DeltaSeconds)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    ALakeCharacter* Angler = TargetAngler.Get();
    // The bait was pulled out of the water, or another fish got there first.
    if ((State == EState::Investigate || State == EState::Nibble) && (!Angler || !Angler->HasBaitInWater()))
    {
        State = EState::Roam;
        TargetAngler = nullptr;
        return;
    }
    if (State == EState::Roam && SimTime >= NextThink)
    {
        NextThink = SimTime + Random.FRandRange(0.6f, 1.4f);
        for (TActorIterator<ALakeCharacter> It(GetWorld()); It; ++It)
        {
            ALakeCharacter* Candidate = *It;
            if (!Candidate->HasBaitInWater()) continue;
            const float* Until = IgnoreBaitUntil.Find(Candidate);
            if (Until && *Until > SimTime) continue;
            const FVector Bait = Candidate->BaitLocation();
            const float Dist = FVector::Dist(Bait, GetActorLocation());
            if (Dist > S.SenseRadius) continue;
            const float Interest = LakeFish::BaitInterest(Species, Candidate->Bait, Depth01(Bait.Z, Bait.X, Bait.Y))
                * Hunger * FMath::Sqrt(1 - Dist / S.SenseRadius);
            if (Random.FRand() < Interest * 0.7f)
            {
                State = EState::Investigate;
                StateTime = 0;
                TargetAngler = Candidate;
                return;
            }
        }
        return;
    }
    if (State == EState::Investigate)
    {
        // Lose interest if it takes too long to get there.
        if (StateTime > 25)
        {
            IgnoreBaitUntil.Add(Angler, SimTime + 20);
            State = EState::Roam;
            return;
        }
        if (FVector::Dist(MouthLocation(), Angler->BaitLocation()) < 14 && Angler->ClaimBait(this))
        {
            State = EState::Nibble;
            StateTime = 0;
            NibblesLeft = Random.RandRange(S.MinNibbles, S.MaxNibbles);
            NextThink = SimTime + Random.FRandRange(0.5f, 1.2f);
        }
        return;
    }
    if (State == EState::Nibble && SimTime >= NextThink)
    {
        NextThink = SimTime + Random.FRandRange(0.7f, 1.6f);
        const float Like = LakeFish::BaitInterest(Species, Angler->Bait, 0.5f);
        if (NibblesLeft > 0)
        {
            Angler->OnFishNibble(this);
            --NibblesLeft;
            // A fish that does not like the bait much often gives up after tasting it.
            if (Random.FRand() > 0.55f + Like * 0.45f)
            {
                Angler->ReleaseBaitClaim(this);
                IgnoreBaitUntil.Add(Angler, SimTime + 30);
                State = EState::Roam;
                ChooseRoamTarget();
            }
            return;
        }
        Angler->OnFishTake(this);
        State = EState::Holding;
        StateTime = 0;
        // Swim off with the bait: away from the angler, slowly.
        const FVector Away = (GetActorLocation() - Angler->GetActorLocation()).GetSafeNormal2D();
        RoamTarget = GetActorLocation() + (Away + FVector(Random.FRandRange(-0.6f, 0.6f), Random.FRandRange(-0.6f, 0.6f), 0)).GetSafeNormal() * 300;
        RoamTarget.Z = GetActorLocation().Z;
    }
}

// --- interaction ---------------------------------------------------------------------------

void ALakeFish::Spook(FVector From, float Seconds)
{
    if (State == EState::Hooked) return;
    if (ALakeCharacter* Angler = TargetAngler.Get()) Angler->ReleaseBaitClaim(this);
    TargetAngler = nullptr;
    State = EState::Flee;
    FleeFrom = From;
    FleeTime = Seconds;
    BurstTime = FMath::Min(Seconds, 1.2f);
}

void ALakeFish::Hook(ALakeCharacter* Angler)
{
    State = EState::Hooked;
    StateTime = 0;
    TargetAngler = Angler;
    BurstTime = 2.0f;   // the first run after the strike
}

void ALakeFish::ApplyLineForce(FVector Force) { PendingLineForce = Force; }

void ALakeFish::Release(float SpookSeconds)
{
    const FVector From = TargetAngler.IsValid() ? TargetAngler->GetActorLocation() : GetActorLocation();
    if (TargetAngler.IsValid()) IgnoreBaitUntil.Add(TargetAngler, SimTime + 60);
    State = EState::Roam;
    PendingLineForce = FVector::ZeroVector;
    Hunger *= 0.5f;
    Spook(From, SpookSeconds);
}

// --- motion --------------------------------------------------------------------------------

void ALakeFish::Move(FVector Desired, float MaxSpeed, float DeltaSeconds)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    Desired = Desired.GetClampedToMaxSize(MaxSpeed);
    FVector Heading = GetActorForwardVector();
    const bool bBurst = BurstTime > 0;
    const float TurnRate = S.TurnRate * (bBurst ? 1.8f : 1.0f) * (State == EState::Hooked ? 0.8f : 1.0f);
    // Turn the body towards the desired direction, limited by the species' turn rate.
    // Fish keep their back up: pitch is limited, roll is not simulated.
    if (Desired.SizeSquared() > 4)
    {
        FVector Want = Desired.GetSafeNormal();
        Want.Z = FMath::Clamp(Want.Z, -0.5f, 0.5f);
        Want.Normalize();
        const FQuat From = Heading.ToOrientationQuat(), To = Want.ToOrientationQuat();
        const float Angle = FMath::RadiansToDegrees(From.AngularDistance(To));
        const float Step = Angle > KINDA_SMALL_NUMBER ? FMath::Min(1.0f, TurnRate * DeltaSeconds / Angle) : 1.0f;
        Heading = FQuat::Slerp(From, To, Step).GetForwardVector();
    }
    // Thrust only along the body axis, like a real tail; reverse thrust is weak.
    float Accel = S.Acceleration * (bBurst ? 2.5f : 1.0f);
    if (State == EState::Hooked) Accel *= S.Strength * FMath::Lerp(0.25f, 1.0f, StaminaFraction());
    const float ForwardSpeed = FVector::DotProduct(Vel, Heading);
    const float TargetSpeed = Desired.Size() * FMath::Max(0.0f, FVector::DotProduct(Heading, Desired.GetSafeNormal()));
    const float Thrust = FMath::Clamp((TargetSpeed - ForwardSpeed) * 3.0f, -Accel * 0.3f, Accel);
    Effort = FMath::Clamp(FMath::Abs(Thrust) / FMath::Max(1.0f, S.Acceleration * 2.5f) + ForwardSpeed / S.BurstSpeed * 0.5f, 0.0f, 1.0f);

    Vel += Heading * Thrust * DeltaSeconds;
    // External pull from the fishing line, divided by the fish's mass.
    Vel += PendingLineForce / FMath::Max(0.03f, WeightKg) * DeltaSeconds;
    PendingLineForce = FVector::ZeroVector;
    // Water drag: low along the streamlined body, high sideways and vertically.
    const FVector Forward = Heading * FVector::DotProduct(Vel, Heading);
    const FVector Side = Vel - Forward;
    const float ForwardDrag = 1.2f / S.BurstSpeed * Forward.Size();
    Vel = Forward / (1 + ForwardDrag * DeltaSeconds) + Side / (1 + 5.0f * DeltaSeconds);

    // Integrate and keep the fish inside the water volume.
    FVector P = GetActorLocation() + Vel * DeltaSeconds;
    const float Bottom = BottomZ(P.X, P.Y);
    const float Top = LakeFish::SurfaceZ - 4;
    if (P.Z > Top) { P.Z = Top; Vel.Z = FMath::Min(0.0, Vel.Z); }
    if (P.Z < Bottom + 4) { P.Z = Bottom + 4; Vel.Z = FMath::Max(0.0, Vel.Z); }
    if (ALakeEnvironment::LakeRadius(P.X, P.Y) > 0.97f || LakeFish::SurfaceZ - Bottom < 10)
    {
        // Hit the shore: stop the outward motion.
        const FVector In = (FVector(2200, 0, P.Z) - P).GetSafeNormal2D();
        P = GetActorLocation();
        Vel -= In * FMath::Min(0.0, FVector::DotProduct(Vel, In)) * 1.0;
    }
    SetActorLocationAndRotation(P, Heading.Rotation());
}

void ALakeFish::Simulate(float DeltaSeconds)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    SimTime += DeltaSeconds;
    StateTime += DeltaSeconds;
    BurstTime = FMath::Max(0.0f, BurstTime - DeltaSeconds);
    UpdateBait(DeltaSeconds);

    FVector Desired = FVector::ZeroVector;
    float MaxSpeed = S.CruiseSpeed;
    ALakeCharacter* Angler = TargetAngler.Get();
    switch (State)
    {
    case EState::Roam:
        if (FVector::Dist(GetActorLocation(), RoamTarget) < 60 || StateTime > (S.Habitat == EFishHabitat::Ambush ? 70 : 20))
        {
            ChooseRoamTarget();
            StateTime = 0;
        }
        Desired = SteerRoam() + SteerSchool();
        Hunger = FMath::Min(1.0f, Hunger + DeltaSeconds * 0.004f);
        break;
    case EState::Investigate:
    {
        // Approach slowly, ending with the mouth at the bait.
        const FVector To = Angler->BaitLocation() - MouthLocation();
        Desired = To.GetSafeNormal() * FMath::Min(S.CruiseSpeed * 0.8f, To.Size() * 0.8f + 4);
        // Pike close the last metre in one strike.
        if (S.Habitat == EFishHabitat::Ambush && To.Size() < 120) { BurstTime = 0.4f; Desired = To.GetSafeNormal() * S.BurstSpeed; MaxSpeed = S.BurstSpeed; }
        break;
    }
    case EState::Nibble:
    {
        // Hover with the mouth at the bait, tiny pecks forward.
        const FVector To = Angler->BaitLocation() - MouthLocation();
        Desired = To * 1.5f + GetActorForwardVector() * 6 * FMath::Sin(SimTime * 9);
        MaxSpeed = 20;
        break;
    }
    case EState::Holding:
        Desired = (RoamTarget - GetActorLocation()).GetSafeNormal() * S.CruiseSpeed * 0.6f;
        break;
    case EState::Hooked:
    {
        if (!Angler) { State = EState::Roam; break; }
        // Run away from the angler, change direction now and then, dive if the species does.
        if (StateTime > 2.5f + Random.FRand() * 2)
        {
            StateTime = 0;
            const FVector Away = (GetActorLocation() - Angler->GetActorLocation()).GetSafeNormal2D();
            const FVector Side = FVector::CrossProduct(Away, FVector::UpVector) * Random.FRandRange(-1.2f, 1.2f);
            RoamTarget = GetActorLocation() + (Away + Side).GetSafeNormal() * 500;
            RoamTarget.Z = FMath::Lerp(LakeFish::SurfaceZ - 10, BottomZ(RoamTarget.X, RoamTarget.Y) + 8, S.DepthMax);
            if (Random.FRand() < 0.35f * StaminaFraction()) BurstTime = Species == EFishSpecies::Pike ? 2.0f : 0.8f;
        }
        Desired = (RoamTarget - GetActorLocation()).GetSafeNormal() * S.BurstSpeed;
        MaxSpeed = FMath::Lerp(S.CruiseSpeed * 0.6f, S.BurstSpeed, StaminaFraction());
        // Pulling against the line costs stamina; resting near the surface gives a little back.
        Stamina = FMath::Clamp(Stamina - DeltaSeconds * (0.3f + Effort * 1.4f), 0.0f, MaxStamina);
        break;
    }
    case EState::Flee:
        FleeTime -= DeltaSeconds;
        Desired = (GetActorLocation() - FleeFrom).GetSafeNormal() * S.BurstSpeed + SteerSchool();
        MaxSpeed = S.BurstSpeed;
        if (FleeTime <= 0) { State = EState::Roam; ChooseRoamTarget(); StateTime = 0; }
        break;
    }
    if (State == EState::Flee || State == EState::Hooked) MaxSpeed = FMath::Max(MaxSpeed, S.CruiseSpeed);
    if (BurstTime > 0) MaxSpeed = S.BurstSpeed;
    Desired += SteerBounds();
    Move(Desired, MaxSpeed, DeltaSeconds);
}

// --- per-frame -----------------------------------------------------------------------------

void ALakeFish::UpdateVisual(float DeltaSeconds)
{
    const FFishSpeciesInfo& S = LakeFish::Info(Species);
    if (!Body->GetStaticMesh())
    {
        Body->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, S.Mesh));
        // The mesh's snout is at X = 0 and the tail towards +X: turn it to face the actor's forward
        // and put the centre of mass (about 40% back from the snout) on the actor's origin.
        Body->SetRelativeRotation(FRotator(0, 180, 0));
        Body->SetRelativeLocation(FVector(S.MeshLength * 0.4f, 0, 0));
        Body->SetCustomPrimitiveDataFloat(2, Phase);
        Body->SetCustomPrimitiveDataFloat(3, S.MeshLength);
    }
    SetActorScale3D(FVector(LakeFish::LengthCm(Species, WeightKg) / S.MeshLength));
    const float E = HasAuthority() ? Effort : NetEffort;
    Body->SetCustomPrimitiveDataFloat(0, S.TailBeat * (0.6f + 2.2f * E));
    Body->SetCustomPrimitiveDataFloat(1, 0.03f + 0.06f * E);
}

void ALakeFish::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (HasAuthority())
    {
        // Fixed sub-steps keep the swimming model stable at low frame rates.
        const int32 Steps = FMath::Clamp(FMath::CeilToInt(DeltaSeconds / 0.02f), 1, 5);
        for (int32 I = 0; I < Steps; ++I) Simulate(DeltaSeconds / Steps);
        NetLocation = GetActorLocation();
        NetRotation = GetActorRotation();
        NetEffort = Effort;
    }
    else
    {
        // Smooth between network updates.
        SetActorLocation(FMath::VInterpTo(GetActorLocation(), FVector(NetLocation), DeltaSeconds, 6));
        SetActorRotation(FMath::RInterpTo(GetActorRotation(), NetRotation, DeltaSeconds, 6));
    }
    if (GetNetMode() != NM_DedicatedServer) UpdateVisual(DeltaSeconds);
}
