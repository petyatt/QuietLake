#include "LakeRod.h"
#include "LakeRipples.h"
#include "LakeGame.h"
#include "LakeMap.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"

namespace
{
    constexpr int32 BlankSegments = 14;
    constexpr int32 MainLineSegments = 40, LeaderSegments = 16, TailSegments = 4;
    // 0.2 mm monofilament: 0.036 g per metre. Air drag across it, per unit of its mass:
    // 0.5 * air density * Cd (1.2) * diameter / mass per length, times 2 for the decay rate.
    constexpr float LineGramsPerCm = 0.00036f;
    constexpr float LineAirDrag = 0.08f;      // 1/s per cm/s of speed across the line
    constexpr float BobberScale = 1.4f;      // a 4-5 g pole float, larger than the generated model
    constexpr float BobberKeel = 3.5f * BobberScale;   // waterline to the line eye
    constexpr float Gravity = 980.0f;

    UStaticMeshComponent* Part(AActor* Owner, const TCHAR* Name, const TCHAR* Mesh, USceneComponent* Parent)
    {
        auto* C = Owner->CreateDefaultSubobject<UStaticMeshComponent>(Name);
        C->SetupAttachment(Parent);
        C->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, Mesh));
        C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        C->SetCastShadow(true);
        C->SetUsingAbsoluteLocation(true);
        C->SetUsingAbsoluteRotation(true);
        return C;
    }

    float WaterOrGround(FVector P)
    {
        return ALakeEnvironment::IsWater(P) ? LakeFish::SurfaceZ : ALakeEnvironment::GroundHeight(P.X, P.Y);
    }
}

ALakeRod::ALakeRod()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostPhysics;
    bReplicates = false;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    Handle = Part(this, TEXT("Handle"), TEXT("/Game/Tackle/SM_RodHandle.SM_RodHandle"), RootComponent);
    Reel = Part(this, TEXT("Reel"), TEXT("/Game/Tackle/SM_Reel.SM_Reel"), RootComponent);
    UMaterialInterface* BlankMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Tackle/M_RodBlank.M_RodBlank"));
    for (int32 I = 0; I < BlankSegments; ++I)
    {
        auto* Segment = Part(this, *FString::Printf(TEXT("Blank%d"), I), TEXT("/Engine/BasicShapes/Cylinder.Cylinder"), RootComponent);
        Segment->SetMaterial(0, BlankMaterial);
        Blank.Add(Segment);
    }
    Bobber = Part(this, TEXT("Bobber"), TEXT("/Game/Tackle/SM_Bobber.SM_Bobber"), RootComponent);
    BobberEye = CreateDefaultSubobject<USceneComponent>(TEXT("BobberEye"));
    BobberEye->SetupAttachment(Bobber);
    BobberEye->SetRelativeLocation(FVector(0, 0, -BobberKeel));
    Sinker = Part(this, TEXT("Sinker"), TEXT("/Game/Tackle/SM_Sinker.SM_Sinker"), RootComponent);
    Hook = Part(this, TEXT("Hook"), TEXT("/Game/Tackle/SM_Hook.SM_Hook"), RootComponent);
    // Baits share the hook's frame (eye at the origin), so they are simply attached to it.
    for (const TCHAR* Name : { TEXT("Worm"), TEXT("Maggot"), TEXT("Bread") })
    {
        auto* B = CreateDefaultSubobject<UStaticMeshComponent>(Name);
        B->SetupAttachment(Hook);
        B->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Game/Tackle/SM_%s.SM_%s"), Name, Name)));
        B->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Baits.Add(B);
    }
    Stand = Part(this, TEXT("Stand"), TEXT("/Game/Tackle/SM_RodStand.SM_RodStand"), RootComponent);

    // Lines are drawn as chains of thin cylinders along our own line simulation.
    UMaterialInterface* LineMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Tackle/M_FishingLine.M_FishingLine"));
    auto MakeLine = [this, LineMaterial](const TCHAR* Name, int32 Segments, TArray<TObjectPtr<UStaticMeshComponent>>& Out, FLineSim& Sim)
    {
        for (int32 I = 0; I < Segments; ++I)
        {
            auto* Segment = Part(this, *FString::Printf(TEXT("%s%d"), Name, I), TEXT("/Engine/BasicShapes/Cylinder.Cylinder"), RootComponent);
            Segment->SetMaterial(0, LineMaterial);
            Segment->SetCastShadow(false);
            Out.Add(Segment);
        }
        Sim.P.SetNumZeroed(Segments + 1);
        Sim.Prev.SetNumZeroed(Segments + 1);
    };
    MakeLine(TEXT("MainLine"), MainLineSegments, MainLine, MainSim);
    MakeLine(TEXT("Leader"), LeaderSegments, Leader, LeaderSim);
    MakeLine(TEXT("Tail"), TailSegments, Tail, TailSim);
}

void ALakeRod::LayoutLine(const FLineSim& L, const TArray<TObjectPtr<UStaticMeshComponent>>& Segments, float Radius)
{
    for (int32 I = 0; I < Segments.Num(); ++I)
    {
        const FVector A = L.P[I], B = L.P[I + 1];
        const FVector D = B - A;
        Segments[I]->SetWorldLocationAndRotation((A + B) * 0.5f, FRotationMatrix::MakeFromZ(D.IsNearlyZero() ? FVector::UpVector : D).ToQuat());
        // Overlap the joints a little so the line looks continuous.
        Segments[I]->SetWorldScale3D(FVector(Radius / 50.0f, Radius / 50.0f, (D.Size() + Radius) / 100.0f));
    }
}

void ALakeRod::Tick(float DeltaSeconds) { Super::Tick(DeltaSeconds); }

namespace
{
    // --- the float rig, in grams and centimetres -------------------------------------------------
    // A 4.8 g pole float (balsa body, a thin antenna) loaded by a shot 10 cm above the hook so that
    // it sits in the water to the widest part of its body: the upper half of the body and the whole
    // antenna show when everything hangs free. Buoyancy comes from the float's real shape (the
    // model's profile), in slices along its axis.
    constexpr float FloatLength = (3.5f + 7.5f) * BobberScale;   // keel eye to antenna tip
    constexpr float FloatPivot = 3.5f * BobberScale;             // keel to the model's origin
    constexpr float FloatMass = 1.2f;
    constexpr float FloatCentre = 3.8f;                          // its centre of mass, from the keel
    constexpr float BodyCapacity = 4.5f, AntennaCapacity = 0.27f;// water displaced when under
    // Water displaced to the widest part of the body (2.35 g) = float + shot (9% lighter in water) + half the hook.
    constexpr float ShotMass = 1.2f, HookMass = 0.15f;
    constexpr float ShotAboveHook = 10.0f;
    // Loaded, the float's keel eye sits this deep (the waterline at the model's origin): the leader
    // is that much shorter than the set depth, so the bait hangs exactly where the angler set it.
    constexpr float KeelDepth = FloatPivot;
    constexpr float KeelMass = FloatMass * (1 - FloatCentre / FloatLength), TipMass = FloatMass * FloatCentre / FloatLength;

    struct FFloatSlice { float S, Volume, Radius; };
    const TArray<FFloatSlice>& FloatSlices()
    {
        static const TArray<FFloatSlice> Slices = []
        {
            // Radius (cm) along the model's axis (cm, waterline 0, keel -3.5): body, then antenna.
            static const float Profile[][2] = { { -3.5f, 0.06f }, { -3.1f, 0.12f }, { -1.8f, 0.4f }, { -0.6f, 0.85f }, { 0.0f, 0.9f }, { 0.6f, 0.85f },
                                                { 1.4f, 0.55f }, { 2.0f, 0.22f }, { 2.2f, 0.15f }, { 7.2f, 0.13f }, { 7.5f, 0.0f } };
            auto Radius = [](float Z)
            {
                for (int32 I = 0; I + 1 < UE_ARRAY_COUNT(Profile); ++I)
                    if (Z <= Profile[I + 1][0]) return FMath::Lerp(Profile[I][1], Profile[I + 1][1], (Z - Profile[I][0]) / (Profile[I + 1][0] - Profile[I][0]));
                return 0.0f;
            };
            constexpr int32 N = 44;
            TArray<FFloatSlice> Out;
            float Body = 0, Antenna = 0;
            for (int32 I = 0; I < N; ++I)
            {
                const float S = (I + 0.5f) / N * FloatLength;
                const float Z = S / BobberScale - 3.5f;
                const float R = Radius(Z) * BobberScale;
                const float V = PI * R * R * FloatLength / N;
                Out.Add({ S, V, R });
                (Z <= 2.2f ? Body : Antenna) += V;
            }
            // The model is drawn larger than a real float; scale the volumes to the float's capacity.
            for (FFloatSlice& Slice : Out) Slice.Volume *= Slice.S / BobberScale - 3.5f <= 2.2f ? BodyCapacity / Body : AntennaCapacity / Antenna;
            return Out;
        }();
        return Slices;
    }
    /** Pull two points together to at most Length apart, by their inverse masses (0 = held). */
    /** Keep A and B at least Length apart (a line's resistance to sharp bends, two links apart). */
    void Apart(FVector& A, float WA, FVector& B, float WB, float Length)
    {
        const FVector D = B - A;
        const float Len = D.Size();
        if (Len >= Length || Len < KINDA_SMALL_NUMBER || WA + WB <= 0) return;
        const FVector Fix = D / Len * (Len - Length) / (WA + WB);
        A += Fix * WA;
        B -= Fix * WB;
    }
    // A joint of the line bends at most ~25 degrees: two links apart, points stay this many links apart.
    constexpr float LineBendSpan = 1.95f;
    void Rope(FVector& A, float WA, FVector& B, float WB, float Length, bool bRigid = false)
    {
        const FVector D = B - A;
        const float Len = D.Size();
        if (Len < KINDA_SMALL_NUMBER || (!bRigid && Len <= Length) || WA + WB <= 0) return;
        const FVector Fix = D / Len * (Len - Length) / (WA + WB);
        A += Fix * WA;
        B -= Fix * WB;
    }
}

namespace
{
    // The blank: a thin-walled tapered tube, 16 mm at the butt to 4 mm at the tip. Its mass per
    // length goes with the radius and its bending stiffness with the radius cubed; the stiffness
    // at the butt is set so the medium 6 m pole's first bending mode is ~3.2 Hz (a light carbon
    // pole, with the same stiffness and less weight, comes out quicker, a heavy one slower).
    constexpr float ButtEI = 1.5e12f;             // dyn cm^2
    float BlankRadius(float T) { return FMath::Lerp(0.8f, 0.22f, T); }
}

void ALakeRod::PrepareBlank(const FTransform& Hold)
{
    const int32 N = BlankSegments;
    const float Seg = (RodLength - HandleLength) / N;
    const float BlankGrams = 0.8f * 320.0f * Heft * Heft;   // the handle is the rest of the rod's weight
    float Sum = 0;
    for (int32 I = 0; I < N; ++I) Sum += BlankRadius((I + 0.5f) / N);
    TArray<float> Mass;
    Mass.Init(0, N + 1);
    for (int32 I = 0; I < N; ++I)
    {
        const float M = BlankGrams * BlankRadius((I + 0.5f) / N) / Sum;
        Mass[I] += M * 0.5f;
        Mass[I + 1] += M * 0.5f;
    }
    BlankW.SetNum(N + 1);
    BlankAlpha.SetNum(N + 1);
    for (int32 I = 0; I <= N; ++I)
    {
        BlankW[I] = I < 2 ? 0.0f : 1 / Mass[I];
        // A joint's bending energy is 2 EI / L^3 |C|^2 with C its offset from the middle of its neighbours.
        const float EI = ButtEI * FMath::Cube(BlankRadius(float(I) / N) / BlankRadius(0));
        BlankAlpha[I] = FMath::Cube(Seg) / (4 * EI);
    }
    // About half of critical damping in the first mode: a rod settles within a couple of swings.
    BlankDamp = 16.0f / Heft;
    const FVector Axis = Hold.GetUnitAxis(EAxis::X);
    const FVector Start = Hold.GetLocation() + Axis * HandleLength;
    if (!bInitialised || Points.Num() != N + 1 || FVector::Dist(Start, Points[0]) > 150)
    {
        // First frame or a teleport: straight and at rest.
        Points.SetNum(N + 1);
        BlankPrev.SetNum(N + 1);
        for (int32 I = 0; I <= N; ++I) Points[I] = BlankPrev[I] = Start + Axis * Seg * I;
        HoldFrom = Hold;
    }
    else HoldFrom = HoldTo;
    HoldTo = Hold;
    // Lying on the ground the blank rests on it (or on the water's surface beyond the bank).
    BlankFloor.Reset();
    if (bLyingDown)
        for (int32 I = 0; I <= N; ++I) BlankFloor.Add(WaterOrGround(Start + Axis * Seg * I) + BlankRadius(float(I) / N) * FMath::Sqrt(Heft));
}

void ALakeRod::StepBlank(float H, float S)
{
    // The butt is clamped in the hands (the first two points follow them); every other point has
    // its weight and is pulled straight by the stiffness of the joints around it (XPBD, a few
    // passes), with fixed link lengths. The tip carries the line, and any load given as TipForce.
    const int32 N = BlankSegments;
    const float Seg = (RodLength - HandleLength) / N;
    const FQuat Rot = FQuat::Slerp(HoldFrom.GetRotation(), HoldTo.GetRotation(), S);
    const FVector Axis = Rot.GetAxisX();
    const FVector Start = FMath::Lerp(HoldFrom.GetLocation(), HoldTo.GetLocation(), S) + Axis * HandleLength;
    Points[0] = BlankPrev[0] = Start;
    Points[1] = BlankPrev[1] = Start + Axis * Seg;
    const float Keep = FMath::Exp(-BlankDamp * H);
    for (int32 I = 2; I <= N; ++I)
    {
        const FVector V = (Points[I] - BlankPrev[I]) * Keep;
        BlankPrev[I] = Points[I];
        FVector Acc(0, 0, -Gravity);
        if (I == N) Acc += TipForce * BlankW[N];
        Points[I] += V + Acc * H * H;
    }
    BlankLambda.Init(FVector::ZeroVector, N + 1);
    // Enough passes for the stiffness to reach from the butt to the tip within the step: with too few
    // the blank comes out much softer than it is.
    for (int32 Pass = 0; Pass < 16; ++Pass)
    {
        for (int32 I = 1; I < N; ++I)
        {
            const FVector C = Points[I] - (Points[I - 1] + Points[I + 1]) * 0.5f;
            const float Wsum = BlankW[I] + 0.25f * (BlankW[I - 1] + BlankW[I + 1]);
            const float At = BlankAlpha[I] / (H * H);
            const FVector D = (-C - BlankLambda[I] * At) / (Wsum + At);
            BlankLambda[I] += D;
            Points[I] += D * BlankW[I];
            Points[I - 1] -= D * (0.5f * BlankW[I - 1]);
            Points[I + 1] -= D * (0.5f * BlankW[I + 1]);
        }
        for (int32 I = 1; I < N; ++I) Rope(Points[I], BlankW[I], Points[I + 1], BlankW[I + 1], Seg, true);
        // The cast draw: the line, held by the hand, keeps the tip within its length of the float.
        if (TipReach > 0)
        {
            FVector Anchor = TipAnchor;
            Rope(Anchor, 0, Points[N], BlankW[N], TipReach);
        }
    }
    if (BlankFloor.Num() == N + 1)
        for (int32 I = 2; I <= N; ++I) Points[I].Z = FMath::Max(Points[I].Z, BlankFloor[I]);
}

void ALakeRod::UpdateBobber(const FRodInput& In, float DeltaSeconds)
{
    // The rig is four bodies tied by line: the float (a rigid stick from the keel eye K to the
    // antenna tip T, with its weight and the buoyancy of its submerged slices), the shot and the
    // hook with the bait; the main line ties the keel to the rod tip. A fish moves the hook; the
    // float only answers with its weight and buoyancy: it dips when the line is pulled, rises when
    // the hook or the shot is lifted off it, lies down with no load, goes under when dragged.
    // (Up to a tenth of a second per frame, in 240 Hz steps: a frame that hitches must not leave the
    // rig behind the rod, whose hands follow the clock — the cast would come out short.)
    const float Dt = FMath::Clamp(DeltaSeconds, 1 / 240.0f, 0.1f);
    const FVector TipPos = Tip();
    const float LeaderLength = FMath::Max(FMath::Clamp(In.HookDepth, 5.0f, 520.0f) - KeelDepth, 4.0f);
    const float MainLength = LineLength - FMath::Clamp(In.HookDepth, 5.0f, 520.0f);
    const float ToShot = FMath::Max(LeaderLength - ShotAboveHook, 2.0f), ShotToHook = FMath::Min(ShotAboveHook, LeaderLength - 2.0f);

    // What the fish does with the hook. Holding the bait in its mouth (a take), the hook goes with
    // the fish; while it only tries the bait, its mouth jiggles, tugs or lifts the hook from where it
    // lay (the anchor). Without a fish (a preview) a take follows the same script.
    const float E = FMath::Max(In.BiteElapsed, 0.0f), D = FMath::Max(In.BiteDuration, 0.05f), Amount = In.BiteAmount;
    const float U = FMath::Clamp(E / D, 0.0f, 1.0f);
    if (In.BiteMotion != LastBiteMotion || E + 0.05f < LastBiteElapsed) { BiteAnchor = HookPos; BiteFloat = FloatK; }
    LastBiteMotion = In.BiteMotion;
    LastBiteElapsed = E;
    const FVector Away = In.Facing.GetSafeNormal2D(), Side(-Away.Y, Away.X, 0);
    bool bHookHeld = false;
    FVector HookTarget = HookPos;
    // Lifting a fish out: the fish is a heavy weight on the end of the line.
    const bool bFishOut = In.FishingState == EFishingState::Landing && In.FishGrams > 0;
    if (!bFishOut) LandLine = -1;
    const float WholeLine = MainLength + LeaderLength;
    if (In.LineFish && (In.FishingState == EFishingState::Bite || In.FishingState == EFishingState::Fight || In.bFishHeld))
    {
        bHookHeld = true;
        HookTarget = In.LineFish->MouthLocation();
    }
    else if (In.FishingState >= EFishingState::Waiting && In.BiteMotion != EBiteMotion::None)
    {
        bHookHeld = true;
        // Pulling means first taking up the slack (bait lying on the bottom), then drawing the line.
        const FVector FromFloat = BiteAnchor - FloatK;
        const FVector PullDir = FromFloat.GetSafeNormal();
        const float Slack = FMath::Max(0.0f, LeaderLength - FromFloat.Size());
        switch (In.BiteMotion)
        {
        case EBiteMotion::Tremble:
        {
            // Nosing the bait: small tugs and lifts, two or three a second.
            const float Envelope = E < D ? FMath::Sin(PI * U) : 0.0f;
            const float Tug = (0.6f * FMath::Sin(E * 14) + 0.4f * FMath::Sin(E * 22 + 1)) * Envelope;
            HookTarget = BiteAnchor + PullDir * FMath::Max(0.0f, Tug) * (Slack + 1.5f * Amount) - FVector(0, 0, FMath::Min(0.0f, Tug) * 2.5f * Amount) + Side * Amount * Envelope * FMath::Sin(E * 17);
            break;
        }
        case EBiteMotion::Peck: HookTarget = BiteAnchor + PullDir * (Slack + 1.0f * Amount) * (E < D ? FMath::Sin(PI * U) : 0.0f); break;
        case EBiteMotion::Lift: HookTarget = BiteAnchor + FVector(0, 0, (8 + 9 * Amount) * FMath::SmoothStep(0.0f, 0.8f, U)); break;
        // The fish stands holding the bait where it led it: the line stays as it is (a float pulled under stays under).
        case EBiteMotion::Hold: HookTarget = BiteAnchor; break;
        case EBiteMotion::Drift:
        case EBiteMotion::Sink:
        {
            // The fish swims off with the bait near the bottom: slowly aside (drift) or briskly away
            // (sink). A float cannot skate across the water that fast (see below), so a quick pull
            // draws it under, a slow one tows it, leaning and settling.
            // Either way it goes back down towards the bottom with the bait, so the float takes its load again.
            const bool bDrift = In.BiteMotion == EBiteMotion::Drift;
            const FVector Dir = (bDrift ? Side + Away * 0.2f : Side * 0.8f + Away * 0.4f).GetSafeNormal2D();
            const float Speed = bDrift ? 12.0f : 0.6f * Amount;
            HookTarget = BiteAnchor + Dir * Speed * E;
            const float Bottom = ALakeEnvironment::GroundHeight(HookTarget.X, HookTarget.Y) + 3.0f;
            HookTarget.Z = FMath::Lerp(BiteAnchor.Z, Bottom, bDrift ? FMath::SmoothStep(0.0f, 0.6f, U) : FMath::SmoothStep(0.0f, 0.4f, E));
            break;
        }
        default: break;
        }
        HookTarget.Z = FMath::Clamp(HookTarget.Z, ALakeEnvironment::GroundHeight(HookTarget.X, HookTarget.Y) + 0.5f, LakeFish::SurfaceZ - 3);
    }
    // The fish cannot take the hook farther than the whole line reaches from the rod tip.
    // The cast is a pendulum: the fingers let the shot go and the rig, on its taut line, swings forward
    // under the rod tip and up beyond it; the rod raised under it carries it higher. Let go, the rod is
    // lowered and the rig comes down wherever its swing has taken it. Nothing throws or steers it.
    const bool bFlying = In.bRigFlying;
    const bool bInAirCast = bFlying;
    const float TipToFloat = MainLength;
    // The rig in the angler's left hand (ready to cast, or drawn back for it): the fingers hold the
    // shot, the hook hangs just below it, and the line to the rod tip is kept taut.
    const bool bShotHeld = In.HookHold >= 0.999f;
    // Held in the hand (also while swinging a cast) the line is just taut, never pulled beyond its
    // length: let go, the rig swings out on it from where the hand held it, like a swing.
    const FVector ShotTarget = In.HandPoint;
    const FVector HandFrom = ShotPos;
    const int32 Steps = FMath::CeilToInt(Dt / (1 / 240.0f));
    const float H = Dt / Steps;
    TipReach = -1;
    if (bShotHeld)
    {
        TipAnchor = ShotTarget;
        TipReach = MainLength + ToShot;
    }
    else if ((bHookHeld && In.LineFish) || bFishOut)
    {
        // (A fish lifted out holds the tip through the line just as it did in the water: the blank
        // bends as far as keeping the tip within the line's length of it takes.)
        TipAnchor = bFishOut ? HookPos : HookTarget;
        TipReach = WholeLine;
    }
    const TArray<FFloatSlice>& Slices = FloatSlices();
    // The water under the float: the live waves where they are simulated (they move it up and down
    // and tilt it with their slope); elsewhere (a dedicated server) a faint stand-in swell.
    if (!Ripples.IsValid() && Time >= NextRipplesLookup)
    {
        Ripples = ALakeRipples::Get(GetWorld());
        NextRipplesLookup = Time + 1.0f;
    }
    const ALakeRipples* Waves = Ripples.Get();
    const float Wave = Waves ? Waves->ShownHeightAt(FVector2D(FloatK)) : FMath::Sin(Time * 2.1f) * 0.3f;
    const FVector2D WaveSlope = Waves ? Waves->ShownSlopeAt(FVector2D(FloatK)) : FVector2D::ZeroVector;
    for (int32 Step = 0; Step < Steps; ++Step)
    {
        StepBlank(H, float(Step + 1) / Steps);
        FVector& RodTip = Points.Last();
        // --- the float: weight and the buoyancy of every submerged slice, shared between K and T ---
        const FVector Axis = (FloatT - FloatK).GetSafeNormal();
        const bool bOverWater = ALakeEnvironment::IsWater(FloatK);
        const float Surface = LakeFish::SurfaceZ + Wave;
        float LiftK = 0, LiftT = 0, Wet = 0;
        if (bOverWater)
            for (const FFloatSlice& Slice : Slices)
            {
                const float Z = FloatK.Z + Axis.Z * Slice.S;
                // The slice's height in the water: its length when the float stands, its diameter when
                // it lies, so a lying float settles smoothly instead of all its slices going under at once.
                const float Thick = FMath::Abs(Axis.Z) * FloatLength / Slices.Num() + 2 * Slice.Radius * FMath::Sqrt(FMath::Max(0.0f, 1 - Axis.Z * Axis.Z)) + 0.3f;
                const float Under = FMath::Clamp((Surface - Z) / Thick + 0.5f, 0.0f, 1.0f);
                const float B = Slice.Volume * Under;
                LiftK += B * (1 - Slice.S / FloatLength);
                LiftT += B * Slice.S / FloatLength;
                Wet += Under / Slices.Num();
            }
        // Water damps a floating float hard; in the air it barely slows.
        // Held in the fingers, the float hangs in the nearly taut line: its swings die out quickly.
        const float FloatDamp = FMath::Pow(Wet > 0.02f ? 0.03f : bShotHeld ? 0.15f : 0.8f, H);
        auto Integrate = [H](FVector& P, FVector& Prev, float Damp, const FVector& Acc)
        {
            const FVector V = (P - Prev) * Damp;
            Prev = P;
            P += V + Acc * H * H;
        };
        // Quadratic air drag of the light, broad float; the compact shot has less area per gram.
        auto AirDrag = [&](const FVector& P, const FVector& Prev, float AreaPerMass)
        {
            const FVector Relative = (P - Prev) / H - In.Wind;
            return -Relative * FMath::Min(AreaPerMass * Relative.Size(), 0.5f / H);
        };
        Integrate(FloatK, FloatKPrev, FloatDamp, FVector(0, 0, -Gravity + LiftK * Gravity / KeelMass)
            + (Wet < 0.02f ? AirDrag(FloatK, FloatKPrev, 0.0018f) : FVector::ZeroVector));
        // The ripple rocks the antenna a little, so a float with nothing to hold it upright falls over.
        // A passing wave tips it too: the water slides downhill under the keel, so the antenna leans
        // the other way.
        const FVector Rock = Wet > 0.02f ? FVector(FMath::Sin(Time * 1.3f), FMath::Cos(Time * 1.7f), 0) * 15.0f
            + FVector(WaveSlope, 0) * 600.0f : FVector::ZeroVector;
        Integrate(FloatT, FloatTPrev, FloatDamp, FVector(0, 0, -Gravity + LiftT * Gravity / TipMass) + Rock
            + (Wet < 0.02f ? AirDrag(FloatT, FloatTPrev, 0.0018f) : FVector::ZeroVector));
        // Drag is relative to the surface current, never a spring to a chosen landing point.
        if (Wet > 0.02f)
        {
            const FVector Current = FVector(In.Wind.X, In.Wind.Y, 0) * 0.03f;
            FloatK += Current * (1 - FloatDamp) * H;
            FloatT += Current * (1 - FloatDamp) * H;
        }
        // --- the shot and the hook: lead sinks steadily (water drag), the bait follows a fish ---
        const bool bShotWet = ALakeEnvironment::IsWater(ShotPos) && ShotPos.Z < LakeFish::SurfaceZ;
        if (bShotHeld)
        {
            ShotPrev = ShotPos;
            // Closed fingers are a kinematic contact, acquired only at the rig's actual position.
            ShotPos = FMath::Lerp(HandFrom, ShotTarget, float(Step + 1) / Steps);
        }
        else Integrate(ShotPos, ShotPrev, FMath::Pow(bShotWet ? 0.0005f : 0.8f, H), FVector(0, 0, -Gravity * (bShotWet ? 0.91f : 1.0f))
            + (bShotWet ? FVector::ZeroVector : AirDrag(ShotPos, ShotPrev, 0.00035f)));
        if (bHookHeld)
        {
            HookPrev = HookPos;
            // A hooked fish is on the line: its mouth is where the hook is. Chasing it softly left the
            // line trailing half a metre behind a running fish, like a rubber band.
            HookPos = In.LineFish ? HookTarget : FMath::Lerp(HookPos, HookTarget, 0.08f);
        }
        else if (bFishOut)
        {
            // Out of the water the fish hangs on the line and swings like a pendulum, the thrashing body
            // slowing the swing. The rod holds it with the same spring as in the water (its bend and the
            // arms, from the tip of the rod as the hands hold it, unbent): leaving the water changes
            // nothing in the pull, so nothing jumps. Dipped back, the water holds it up again.
            const float Support = ALakeEnvironment::IsWater(HookPos) ? FishSupport(HookPos.Z, In.FishLength) : 0;
            const float Grams = FMath::Max(30.0f, In.FishGrams);
            const float Across = In.LineSpring > 0 ? In.LineSpring : 2600.0f / 140.0f, Along = In.LineSpringAlong > 0 ? In.LineSpringAlong : Across;
            const float Sin2 = (float)FVector::CrossProduct(In.Hold.GetUnitAxis(EAxis::X), (HookPos - StraightTip).GetSafeNormal()).SizeSquared();
            const FVector Pull = TetherForce(StraightTip, HookPos, (HookPos - HookPrev) / H - StraightTipVel, LineLength, Grams / 1000,
                1.0f / (Sin2 / Across + (1 - Sin2) / Along));
            Integrate(HookPos, HookPrev, FMath::Pow(FMath::Lerp(0.45f, 0.02f, Support), H),
                FVector(0, 0, -Gravity * (1 - 0.98f * Support)) + Pull * (Gravity / Grams));
        }
        else
        {
            const bool bHookWet = ALakeEnvironment::IsWater(HookPos) && HookPos.Z < LakeFish::SurfaceZ;
            // Hanging from the held shot the hook settles quickly instead of swinging.
            Integrate(HookPos, HookPrev, FMath::Pow(bHookWet ? 0.0002f : bShotHeld ? 0.002f : 0.8f, H), FVector(0, 0, -Gravity * (bHookWet ? 0.5f : 1.0f)));
        }
        // --- the main line: thin and light, carried by the wind in the air, lying on the water ---
        // In the air the drag across the line grows with the square of its speed relative to the
        // wind: a still line hangs, a breeze blows it out in a belly. On the water it lies on the
        // surface and moves with it (the wind drives the surface at ~3% of its speed).
        const int32 Links = MainSim.P.Num() - 1;
        const FVector SurfaceDrift = FVector(In.Wind.X, In.Wind.Y, 0) * 0.03f;
        for (int32 I = 1; I < Links; ++I)
        {
            FVector& P = MainSim.P[I];
            FVector& Prev = MainSim.Prev[I];
            FVector V = (P - Prev) / H;
            if (ALakeEnvironment::IsWater(P) && P.Z <= LakeFish::SurfaceZ + 0.5f)
                V = SurfaceDrift + (V - SurfaceDrift) * FMath::Exp(-30.0f * H);
            else
            {
                const FVector Tangent = (MainSim.P[I + 1] - MainSim.P[I - 1]).GetSafeNormal();
                const FVector Rel = V - In.Wind;
                const FVector Along = Tangent * FVector::DotProduct(Rel, Tangent);
                const FVector Across = Rel - Along;
                const float K = FMath::Min(LineAirDrag * Across.Size(), 400.0f) + 1.0f;
                V = (In.Wind + Across * FMath::Exp(-K * H) + Along * FMath::Exp(-0.05f * K * H)) + FVector(0, 0, -Gravity * H);
            }
            Prev = P;
            P += V * H;
        }
        // --- the leader (float to shot) and the tail (shot to hook): thin line hanging under the
        // float. In the air it trails in the wind like the main line; in the water it sinks slowly
        // (monofilament is barely heavier than water) and the water holds it back hard. ---
        for (FLineSim* L : { &LeaderSim, &TailSim })
        {
            const int32 N = L->P.Num() - 1;
            for (int32 I = 1; I < N; ++I)
            {
                FVector& P = L->P[I];
                FVector& Prev = L->Prev[I];
                FVector V = (P - Prev) / H;
                if (ALakeEnvironment::IsWater(P) && P.Z < LakeFish::SurfaceZ)
                    V = V * FMath::Exp(-25.0f * H) + FVector(0, 0, -Gravity * 0.12f * H);
                else
                {
                    const FVector Tangent = (L->P[I + 1] - L->P[I - 1]).GetSafeNormal();
                    const FVector Rel = V - In.Wind;
                    const FVector Along = Tangent * FVector::DotProduct(Rel, Tangent);
                    const FVector Across = Rel - Along;
                    const float K = FMath::Min(LineAirDrag * Across.Size(), 400.0f) + 1.0f;
                    V = (In.Wind + Across * FMath::Exp(-K * H) + Along * FMath::Exp(-0.05f * K * H)) + FVector(0, 0, -Gravity * H);
                }
                Prev = P;
                P += V * H;
            }
        }
        // --- line and stick constraints, by mass ---
        const float WK = 1 / KeelMass, WT = 1 / TipMass, WS = bShotHeld ? 0.0f : 1 / ShotMass, WH = bHookHeld ? 0.0f : bFishOut ? 1 / In.FishGrams : 1 / HookMass;
        // The rig hanging from the tip, or flying out on the line, pulls it through the line. A fish
        // and the cast draw load it as a force instead (TipForce); a hand-held rig not at all.
        const float WRodTip = In.FishingState >= EFishingState::Fight || bShotHeld ? 0.0f : BlankW.Last();
        const float WRig = WK + WT;
        for (int32 Pass = 0; Pass < 8; ++Pass)
        {
            if (bShotHeld) Rope(RodTip, BlankW.Last(), ShotPos, 0, MainLength + ToShot);
            Rope(FloatK, WK, FloatT, WT, FloatLength, true);
            Rope(FloatK, WK, ShotPos, WS, ToShot);
            Rope(ShotPos, WS, HookPos, WH, ShotToHook);
            Rope(RodTip, WRodTip, FloatK, WRig, TipToFloat);   // the rod tip holds; the whole float follows
            // The main line, link by link from the tip to the float's eye. The float's place is the
            // server's (it drifts the rig in the wind itself), so the line bends to it and does not
            // drag it about. The links keep their length both ways: line not needed between the tip
            // and the float lies on the water in a loop instead of vanishing.
            const float Link = TipToFloat / Links, WL = 1 / FMath::Max(LineGramsPerCm * Link, 1e-4f);
            MainSim.P[0] = RodTip;
            for (int32 I = 0; I < Links; ++I)
                Rope(MainSim.P[I], I == 0 ? 0.0f : WL, I + 1 == Links ? FloatK : MainSim.P[I + 1], I + 1 == Links ? 0.0f : WL, Link, true);
            for (int32 I = 0; I + 1 < Links; ++I)
                Apart(MainSim.P[I], I == 0 ? 0.0f : WL, I + 2 == Links ? FloatK : MainSim.P[I + 2], I + 2 == Links ? 0.0f : WL, Link * LineBendSpan);
            // The leader and the tail hang between the float, the shot and the hook; too light to
            // move them, they only bend to them.
            auto Chain = [](FLineSim& L, FVector& A, FVector& B, float Length)
            {
                const int32 N = L.P.Num() - 1;
                const float Seg = Length / N;
                L.P[0] = A;
                for (int32 I = 0; I < N; ++I)
                    Rope(L.P[I], I == 0 ? 0.0f : 1.0f, I + 1 == N ? B : L.P[I + 1], I + 1 == N ? 0.0f : 1.0f, Seg, true);
                for (int32 I = 0; I + 1 < N; ++I)
                    Apart(L.P[I], I == 0 ? 0.0f : 1.0f, I + 2 == N ? B : L.P[I + 2], I + 2 == N ? 0.0f : 1.0f, Seg * LineBendSpan);
            };
            Chain(LeaderSim, FloatK, ShotPos, ToShot);
            Chain(TailSim, ShotPos, HookPos, ShotToHook);
        }
        // The line lies on the water or the ground.
        for (int32 I = 1; I < Links; ++I)
        {
            FVector& P = MainSim.P[I];
            const float Floor = ALakeEnvironment::IsWater(P) ? LakeFish::SurfaceZ + 0.2f : ALakeEnvironment::GroundHeight(P.X, P.Y) + 0.3f;
            if (P.Z < Floor)
            {
                P.Z = Floor;
                MainSim.Prev[I].Z = FMath::Max(MainSim.Prev[I].Z, Floor);
                if (!ALakeEnvironment::IsWater(P)) MainSim.Prev[I] = FMath::Lerp(MainSim.Prev[I], P, 0.3f);   // ground friction
            }
        }
        if (bFishOut)
        {
            // A thrashing fish on a 6 m line never flies: its speed is limited.
            const FVector Moved = HookPos - HookPrev;
            if (Moved.Size() > 450.0f * H) HookPrev = HookPos - Moved.GetSafeNormal() * 450.0f * H;
        }
        MainSim.P[Links] = FloatK;
        MainSim.P[0] = RodTip;
        LeaderSim.P[0] = FloatK; LeaderSim.P.Last() = ShotPos;
        TailSim.P[0] = ShotPos; TailSim.P.Last() = HookPos;
        for (FLineSim* L : { &LeaderSim, &TailSim })
            for (int32 I = 1; I + 1 < L->P.Num(); ++I)
            {
                FVector& P = L->P[I];
                const float Floor = ALakeEnvironment::GroundHeight(P.X, P.Y) + 0.3f;
                if (P.Z < Floor) { P.Z = Floor; L->Prev[I] = FMath::Lerp(L->Prev[I], P, 0.3f); L->Prev[I].Z = Floor; }
            }
        // In the air the line runs through both of the float's rings: it hangs along the line.
        if (Wet <= 0.02f)
        {
            // Between the leader coming in at the keel and the main line going on from the antenna.
            const FVector Along = ((FloatK - ShotPos).GetSafeNormal() + (MainSim.P[Links - 1] - FloatK).GetSafeNormal()).GetSafeNormal();
            FloatT = FMath::Lerp(FloatT, FloatK + (Along.IsNearlyZero() ? FVector::UpVector : Along) * FloatLength, 0.2f);
        }
        // Water holds a float back: towed by a fish it slides at most ~15 cm/s; pulled faster, the
        // line draws it under instead.
        if (bHookHeld && Wet > 0.02f && FVector::Dist(TipPos, FloatK) < MainLength - 3.0f)
            for (FVector* P : { &FloatK })   // the keel is held back; the antenna trails and leans
            {
                const FVector& Prev = FloatKPrev;
                const FVector2D Slide(P->X - Prev.X, P->Y - Prev.Y);
                const float Most = 15.0f * H;
                if (Slide.Size() > Most) { const FVector2D Kept = Slide.GetSafeNormal() * Most; P->X = Prev.X + Kept.X; P->Y = Prev.Y + Kept.Y; }
            }
        // A light rig never moves faster than ~25 m/s, a float on the water at most ~2 m/s.
        const float MaxStep = (Wet > 0.02f && !bInAirCast ? 200.0f : 2500.0f) * H;
        for (FVector* P : { &FloatK, &FloatT, &ShotPos, &HookPos })
        {
            FVector& Prev = P == &FloatK ? FloatKPrev : P == &FloatT ? FloatTPrev : P == &ShotPos ? ShotPrev : HookPrev;
            if (FVector::Dist(*P, Prev) > MaxStep) Prev = *P - (*P - Prev).GetSafeNormal() * MaxStep;
            // Ground and lake bed: what lies on the bottom stays put. A fish hanging over the bank
            // rests its body on the ground below the hook.
            const float Floor = ALakeEnvironment::GroundHeight(P->X, P->Y) + (bFishOut && P == &HookPos && !ALakeEnvironment::IsWater(*P) ? In.FishLength * 0.8f : 0.5f);
            if (P->Z < Floor) { P->Z = Floor; Prev = FMath::Lerp(Prev, *P, 0.5f); Prev.Z = Floor; }
            // Let down on the bank, the fish does not lie still: it flops in hops towards the water
            // (a fish at the edge gets back in), and on the hook it swims again.
            if (bFishOut && In.bFishLowered && P == &HookPos && !ALakeEnvironment::IsWater(*P) && P->Z <= Floor + 0.5f && Time >= NextFlop)
            {
                const FVector2D Towards = LakeMap::DeeperDirection(P->X, P->Y);
                Prev = *P - FVector(Towards * 140.0f, 230.0f) * H;
                NextFlop = Time + 0.45f;
            }
        }
    }
    BobberPos = FloatK + (FloatT - FloatK).GetSafeNormal() * FloatPivot;
    ShotVel = (ShotPos - ShotPrev) / H;
    HookVel = (HookPos - HookPrev) / H;
    BobberPrev = BobberPos;
    BiteTilt = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp((FloatT - FloatK).GetSafeNormal().Z, -1.0f, 1.0f)));
    bBobberInWater = ALakeEnvironment::IsWater(BobberPos) && BobberPos.Z < LakeFish::SurfaceZ + 3;
}

void ALakeRod::Layout(const FRodInput& In)
{
    // Handle and reel at the butt.
    Handle->SetWorldLocationAndRotation(In.Hold.GetLocation(), In.Hold.GetRotation());
    Reel->SetVisibility(false);   // a pole rod has no reel
    // Tapered blank segments between the simulated points.
    for (int32 I = 0; I < BlankSegments; ++I)
    {
        const FVector A = Points[I], B = Points[I + 1];
        const float Radius = BlankRadius((I + 0.5f) / BlankSegments) * FMath::Sqrt(Heft);   // 16 mm butt section, 4 mm tip (medium rod)
        Blank[I]->SetWorldLocationAndRotation((A + B) * 0.5f, FRotationMatrix::MakeFromZ(B - A).ToQuat());
        Blank[I]->SetWorldScale3D(FVector(Radius / 50.0f, Radius / 50.0f, FVector::Dist(A, B) / 100.0f));
    }
    // Float rig.
    // In the air the float hangs along the line; on the water it stands up, wobbling a little.
    // The float stands, leans or lies along its simulated axis (keel to antenna tip).
    const FVector FloatAxis = (FloatT - FloatK).GetSafeNormal();
    Bobber->SetWorldLocationAndRotation(BobberPos, FRotationMatrix::MakeFromZ(FloatAxis.IsNearlyZero() ? FVector::UpVector : FloatAxis).Rotator());
    Bobber->SetWorldScale3D(FVector(BobberScale));
    const FVector Eye = BobberEye->GetComponentLocation();
    Hook->SetWorldLocationAndRotation(HookPos, FRotationMatrix::MakeFromZX(ShotPos - HookPos, In.Hold.GetUnitAxis(EAxis::X)).Rotator());
    Sinker->SetWorldLocation(ShotPos);
    for (int32 I = 0; I < Baits.Num(); ++I) Baits[I]->SetVisibility(I == (int32)In.Bait);


    Stand->SetVisibility(In.bOnStand);
    Stand->SetWorldLocationAndRotation(In.StandBase, FRotator(0, In.StandYaw + 90, 0));
}

void ALakeRod::UpdateRod(const FRodInput& Input, float DeltaSeconds)
{
    Time += DeltaSeconds;
    // A new depth slides the float along the line instead of teleporting it.
    const float TargetLeader = FMath::Clamp(Input.HookDepth, 5.0f, 520.0f);
    // (Only while the rig is out of the water: once cast the leader is exactly the set depth, so the
    // line never shortens under a float already lying on the water.)
    LeaderNow = bInitialised && Input.FishingState == EFishingState::Idle ? FMath::FInterpConstantTo(LeaderNow, TargetLeader, FMath::Min(DeltaSeconds, 1 / 30.0f), 120.0f) : TargetLeader;
    FRodInput In = Input;
    In.HookDepth = LeaderNow;
    // Loads on the tip given as a force (dyn). A fish, in the water or lifted out, holds the tip
    // through the line itself: the line does not stretch, so the tip stays within the line's length
    // of its mouth and the blank bends as far as that takes (UpdateBobber); so do the rig's weight
    // and the hand drawing it back for the cast.
    TipForce = FVector::ZeroVector;
    if (Points.Num())
    {
        // (A fish lifted out holds the tip through the line as in the water: see UpdateBobber.)
    }
    Heft = FMath::Max(0.5f, In.RodHeft);
    bLyingDown = In.bOnGround;
    {
        const FVector Straight = In.Hold.TransformPosition(FVector(RodLength, 0, 0));
        StraightTipVel = bInitialised && DeltaSeconds > 1e-4f && FVector::Dist(Straight, StraightTip) < 300
            ? (Straight - StraightTip) / DeltaSeconds : FVector::ZeroVector;
        StraightTip = Straight;
    }
    PrepareBlank(In.Hold);
    // First frame or a teleport: hang the rig straight down from the tip.
    const bool bReset = !bInitialised || FVector::Dist(Tip(), LastTip) > 300;
    if (bReset)
    {
        const float Drop = FMath::Clamp(In.HookDepth, 5.0f, 520.0f);
        const float Below = FMath::Max(Drop - KeelDepth, 4.0f);
        FloatK = FloatKPrev = Tip() - FVector(0, 0, LineLength - Drop);
        FloatT = FloatTPrev = FloatK + FVector(0, 0, FloatLength);
        HookPos = HookPrev = FloatK - FVector(0, 0, Below);
        ShotPos = ShotPrev = HookPos + FVector(0, 0, FMath::Min(ShotAboveHook, Below - 2.0f));
        if (In.HookHold >= 0.999f)
        {
            // Initial placement only: assemble the held rig without a first-frame impulse.
            ShotPos = ShotPrev = In.HandPoint;
            HookPos = HookPrev = ShotPos - FVector(0, 0, FMath::Min(ShotAboveHook, Below - 2.0f));
            FloatK = FloatKPrev = ShotPos + (Tip() - ShotPos).GetSafeNormal() * FMath::Max(Below - ShotAboveHook, 2.0f);
            FloatT = FloatTPrev = FloatK + (Tip() - FloatK).GetSafeNormal() * FloatLength;
        }
        BobberPos = BobberPrev = FloatK + FVector(0, 0, 3.5f * BobberScale);
        for (int32 I = 0; I < MainSim.P.Num(); ++I) MainSim.P[I] = MainSim.Prev[I] = FMath::Lerp(Tip(), FloatK, float(I) / (MainSim.P.Num() - 1));
        for (int32 I = 0; I < LeaderSim.P.Num(); ++I) LeaderSim.P[I] = LeaderSim.Prev[I] = FMath::Lerp(FloatK, ShotPos, float(I) / (LeaderSim.P.Num() - 1));
        for (int32 I = 0; I < TailSim.P.Num(); ++I) TailSim.P[I] = TailSim.Prev[I] = FMath::Lerp(ShotPos, HookPos, float(I) / (TailSim.P.Num() - 1));
        bInitialised = true;
    }
    LastTip = Tip();
    UpdateBobber(In, DeltaSeconds);
    Layout(In);
    // Lines, all simulated with the rig: the main line, the leader from the float's eye through the
    // shot, and the tail to the hook.
    LayoutLine(MainSim, MainLine, 0.11f);
    LayoutLine(LeaderSim, Leader, 0.075f);
    LayoutLine(TailSim, Tail, 0.075f);
}

float ALakeRod::LineBelly(int32* OutOnWater) const
{
    if (MainSim.P.Num() < 2) return 0;
    const FVector2D A(MainSim.P[0]), B(MainSim.P.Last());
    const FVector2D Dir = (B - A).GetSafeNormal();
    float Most = 0;
    int32 OnWater = 0;
    for (const FVector& P : MainSim.P)
    {
        const FVector2D D = FVector2D(P) - A;
        Most = FMath::Max(Most, FMath::Abs(D.X * Dir.Y - D.Y * Dir.X));
        if (ALakeEnvironment::IsWater(P) && P.Z <= LakeFish::SurfaceZ + 0.6f) ++OnWater;
    }
    if (OutOnWater) *OutOnWater = OnWater;
    return Most;
}

bool ALakeRod::FloatDown() const
{
    return ALakeEnvironment::IsWater(FloatK) ? FloatK.Z <= LakeFish::SurfaceZ + 1.0f : FloatK.Z <= ALakeEnvironment::GroundHeight(FloatK.X, FloatK.Y) + 2.0f;
}

const ALakeRod::FCastTuning& ALakeRod::CastTuning()
{
    static const FCastTuning Tuning = []
    {
        FCastTuning T;
#if !UE_BUILD_SHIPPING
        FString Value;
        if (FParse::Value(FCommandLine::Get(), TEXT("CastTune="), Value, false))
        {
            TArray<FString> Parts;
            Value.ParseIntoArray(Parts, TEXT(","), true);
            float* Fields[] = { &T.Start, &T.Raise, &T.RaiseTime, &T.Follow, &T.LayAfter, &T.LayTime };
            for (int32 I = 0; I < Parts.Num() && I < UE_ARRAY_COUNT(Fields); ++I) *Fields[I] = FCString::Atof(*Parts[I]);
        }
#endif
        return T;
    }();
    return Tuning;
}

FVector ALakeRod::TetherForce(FVector Anchor, FVector Mouth, FVector RelativeVelocity, float FreeLength, float Mass, float Spring)
{
    const FVector To = Anchor - Mouth;
    const float Stretch = FMath::Max(0.0f, float(To.Size()) - FreeLength);
    if (Stretch <= 0) return FVector::ZeroVector;
    const float K = Spring;
    const float Damping = 1.4f * FMath::Sqrt(K * FMath::Max(0.03f, Mass));
    const float Opening = FMath::Clamp(float(-FVector::DotProduct(RelativeVelocity, To.GetSafeNormal())), -200.0f, 200.0f);
    return To.GetSafeNormal() * FMath::Max(0.0f, K * Stretch + Damping * Opening);
}
FVector ALakeRod::PullOnFish(FVector Mouth, FVector Velocity, float Mass) const
{
    if (!bInitialised) return FVector::ZeroVector;
    // The line does not stretch: the rod's bend is the spring. Its measure is how far the tip of the
    // unbent rod would have to come towards the fish for the line to reach. (The rig's own points are
    // no measure: pulled past the line's length they cannot follow, and the load came out near zero.)
    return TetherForce(StraightTip, Mouth, Velocity - StraightTipVel, LineLength, Mass);
}


float ALakeRod::FishSupport(float MouthZ, float Length)
{
    // As the head clears, a hanging body still has most of its volume in water.
    return 1 - FMath::SmoothStep(0.0f, FMath::Max(8.0f, Length * 0.8f), MouthZ - LakeFish::SurfaceZ);
}
