#include "LakeRod.h"
#include "LakeGame.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"

namespace
{
    constexpr int32 BlankSegments = 14;
    constexpr int32 MainLineSegments = 24, LeaderSegments = 8;
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
    MakeLine(TEXT("Tail"), 3, Tail, TailSim);
}

void ALakeRod::SimulateLine(FLineSim& L, FVector A, FVector B, float Length, float DeltaSeconds, bool bFloatsOnWater, bool bReset)
{
    // Monofilament weighs almost nothing and air drag dominates it: it falls slowly, never
    // swings like a rope, does not stretch, and resists sharp bends. Simulated as a short verlet
    // chain with strong drag, inextensible segments and a bending term; both ends are pinned.
    const int32 N = L.P.Num() - 1;
    if (bReset)
    {
        for (int32 I = 0; I <= N; ++I) L.P[I] = L.Prev[I] = FMath::Lerp(A, B, float(I) / N);
        L.LastA = A; L.LastB = B;
    }
    const float Segment = FMath::Max(Length, FVector::Dist(A, B)) / N;
    const float Dt = FMath::Clamp(DeltaSeconds, 1 / 240.0f, 1 / 20.0f);
    const int32 Steps = FMath::CeilToInt(Dt / (1 / 240.0f));
    const float H = Dt / Steps;
    for (int32 Step = 0; Step < Steps; ++Step)
    {
        const float S = float(Step + 1) / Steps;
        const FVector EndA = FMath::Lerp(L.LastA, A, S), EndB = FMath::Lerp(L.LastB, B, S);
        for (int32 I = 1; I < N; ++I)
        {
            const bool bWet = ALakeEnvironment::IsWater(L.P[I]) && L.P[I].Z < LakeFish::SurfaceZ;
            const float Drag = bWet ? 30.0f : 7.0f;            // terminal speed ~1.4 m/s in air
            const FVector V = (L.P[I] - L.Prev[I]) * FMath::Exp(-Drag * H);
            L.Prev[I] = L.P[I];
            L.P[I] += V + FVector(0, 0, -Gravity * (bWet ? 0.15f : 1.0f)) * H * H;
        }
        L.P[0] = EndA;
        L.P[N] = EndB;
        for (int32 Pass = 0; Pass < 10; ++Pass)
        {
            // Stiffness: each point is drawn towards the line through its neighbours.
            for (int32 I = 1; I < N; ++I) L.P[I] += ((L.P[I - 1] + L.P[I + 1]) * 0.5f - L.P[I]) * 0.12f;
            // No stretch: a segment may be shorter (slack) but never longer than its share.
            for (int32 I = 0; I < N; ++I)
            {
                const FVector D = L.P[I + 1] - L.P[I];
                const float Len = D.Size();
                if (Len <= Segment) continue;
                const FVector Fix = D * ((Len - Segment) / Len);
                const float WA = I == 0 ? 0.0f : (I + 1 == N ? 1.0f : 0.5f);
                const float WB = I + 1 == N ? 0.0f : (I == 0 ? 1.0f : 0.5f);
                L.P[I] += Fix * WA;
                L.P[I + 1] -= Fix * WB;
            }
        }
        // Slack line lies on the water (the main line) or on the ground and lake bed.
        for (int32 I = 1; I < N; ++I)
        {
            const float Floor = bFloatsOnWater && ALakeEnvironment::IsWater(L.P[I]) ? LakeFish::SurfaceZ + 0.2f
                : ALakeEnvironment::GroundHeight(L.P[I].X, L.P[I].Y) + 0.3f;
            if (L.P[I].Z < Floor)
            {
                L.P[I].Z = Floor;
                L.Prev[I] = FMath::Lerp(L.Prev[I], L.P[I], 0.3f);   // surface friction
                L.Prev[I].Z = Floor;
            }
        }
    }
    L.LastA = A;
    L.LastB = B;
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

void ALakeRod::SimulateBlank(const FTransform& Hold, FVector TipLoad, float DeltaSeconds)
{
    // The blank bends as one elastic cantilever clamped in the hand: a single bending mode
    // (tip deflection D, shape t^2 (3 - t) / 2 along the blank) driven by the line's pull and by
    // the inertia of the hand's motion. Natural frequency ~4 Hz with strong damping, like a real
    // rod: it whips on a fast swing and settles within a fraction of a second.
    const FVector Axis = Hold.GetUnitAxis(EAxis::X);
    const FVector Start = Hold.GetLocation() + Axis * HandleLength;
    const float BlankLength = RodLength - HandleLength;
    const FVector RestTip = Start + Axis * BlankLength;
    const float Dt = FMath::Clamp(DeltaSeconds, 1 / 240.0f, 1 / 20.0f);

    // First frame or a teleport (spawn, respawn): start straight and at rest.
    if (!bInitialised || FVector::Dist(RestTip, RestTipPrev) > 150)
    {
        Deflection = DeflectionVelocity = FVector::ZeroVector;
        RestTipPrev = RestTipPrev2 = RestTip;
    }
    // Acceleration of the unbent tip: swinging the rod forward bends it backwards.
    const FVector HandAccel = (RestTip - 2 * RestTipPrev + RestTipPrev2) / (Dt * Dt);
    RestTipPrev2 = RestTipPrev;
    RestTipPrev = RestTip;
    auto Perpendicular = [&Axis](const FVector& V) { return V - Axis * FVector::DotProduct(V, Axis); };
    const FVector Drive = Perpendicular(TipLoad - HandAccel.GetClampedToMaxSize(60000) * 1.2f);

    constexpr float Omega = 2 * PI * 4.0f, Damping = 0.45f;   // 4 Hz, damping ratio
    constexpr int32 Substeps = 4;
    const float H = Dt / Substeps;
    for (int32 I = 0; I < Substeps; ++I)
    {
        const FVector Accel = Drive - Deflection * (Omega * Omega) - DeflectionVelocity * (2 * Damping * Omega);
        DeflectionVelocity += Accel * H;
        Deflection += DeflectionVelocity * H;
    }
    Deflection = Perpendicular(Deflection).GetClampedToMaxSize(BlankLength * 0.45f);

    // Build the points along the bent shape, keeping every segment its real length.
    const float SegmentLength = BlankLength / BlankSegments;
    Points.SetNum(BlankSegments + 1);
    Points[0] = Start;
    for (int32 I = 1; I <= BlankSegments; ++I)
    {
        const float T = float(I) / BlankSegments;
        const FVector Target = Start + Axis * BlankLength * T + Deflection * (T * T * (3 - T) * 0.5f);
        Points[I] = Points[I - 1] + (Target - Points[I - 1]).GetSafeNormal() * SegmentLength;
    }
}

namespace
{
    // --- the float rig, in grams and centimetres -------------------------------------------------
    // A 4.8 g pole float (balsa body, a thin antenna) loaded by a shot 10 cm above the hook so that
    // about 2.5 cm of antenna shows when everything hangs free. Buoyancy comes from the float's
    // real shape (the model's profile), in slices along its axis.
    constexpr float FloatLength = (3.5f + 7.5f) * BobberScale;   // keel eye to antenna tip
    constexpr float FloatPivot = 3.5f * BobberScale;             // keel to the model's origin
    constexpr float FloatMass = 1.2f;
    constexpr float FloatCentre = 3.8f;                          // its centre of mass, from the keel
    constexpr float BodyCapacity = 4.5f, AntennaCapacity = 0.27f;// water displaced when under
    constexpr float ShotMass = 3.66f, HookMass = 0.15f;      // lead weighs 9% less in water
    constexpr float ShotAboveHook = 10.0f;
    // Loaded, the float's keel eye sits this deep (2.5 cm of antenna showing): the leader is that
    // much shorter than the set depth, so the bait hangs exactly where the angler set it.
    constexpr float KeelDepth = FloatLength - 2.5f;
    constexpr float KeelMass = FloatMass * (1 - FloatCentre / FloatLength), TipMass = FloatMass * FloatCentre / FloatLength;

    struct FFloatSlice { float S, Volume; };
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
                Out.Add({ S, V });
                (Z <= 2.2f ? Body : Antenna) += V;
            }
            // The model is drawn larger than a real float; scale the volumes to the float's capacity.
            for (FFloatSlice& Slice : Out) Slice.Volume *= Slice.S / BobberScale - 3.5f <= 2.2f ? BodyCapacity / Body : AntennaCapacity / Antenna;
            return Out;
        }();
        return Slices;
    }
    /** Pull two points together to at most Length apart, by their inverse masses (0 = held). */
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

void ALakeRod::UpdateBobber(const FRodInput& In, float DeltaSeconds)
{
    // The rig is four bodies tied by line: the float (a rigid stick from the keel eye K to the
    // antenna tip T, with its weight and the buoyancy of its submerged slices), the shot and the
    // hook with the bait; the main line ties the keel to the rod tip. A fish moves the hook; the
    // float only answers with its weight and buoyancy: it dips when the line is pulled, rises when
    // the hook or the shot is lifted off it, lies down with no load, goes under when dragged.
    const float Dt = FMath::Clamp(DeltaSeconds, 1 / 240.0f, 1 / 30.0f);
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
    const bool bFishOut = In.FishingState == 4 && In.FishGrams > 0;
    if (!bFishOut) LandLine = -1;
    const float WholeLine = MainLength + LeaderLength;
    if (In.LineFish && (In.FishingState == 2 || In.FishingState == 3))
    {
        bHookHeld = true;
        HookTarget = In.LineFish->MouthLocation();
    }
    else if (In.FishingState >= 1 && In.BiteMotion != EBiteMotion::None)
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
    if (bHookHeld)
    {
        // Held back along the water, never lifted: the fish stays at its depth.
        const float Reach = MainLength + LeaderLength - 2.0f;
        const float Flat = FMath::Sqrt(FMath::Max(1.0f, FMath::Square(Reach) - FMath::Square(TipPos.Z - HookTarget.Z)));
        const FVector2D Off(HookTarget.X - TipPos.X, HookTarget.Y - TipPos.Y);
        if (Off.Size() > Flat) { const FVector2D Kept = Off.GetSafeNormal() * Flat; HookTarget.X = TipPos.X + Kept.X; HookTarget.Y = TipPos.Y + Kept.Y; }
    }

    // The cast: while LMB is held the lowering rod lays the line out ahead of the tip, the rig
    // following a point that swings forward on the line; once released the float drops along a
    // ballistic arc to the landing point the server chose.
    const bool bFlying = In.SinceCast >= 0 && In.SinceCast < In.CastFlight;
    const float SweepRad = FMath::DegreesToRadians(In.SweepAngle);
    const FVector SweepPoint = TipPos + (In.Facing * FMath::Cos(SweepRad) + FVector::UpVector * FMath::Sin(SweepRad)) * (MainLength - 2);
    if (bFlying && !bCastLaunched)
    {
        bCastLaunched = true;
        FlightStart = FloatK;
        const FVector Land = In.FloatTarget - FVector(0, 0, FloatPivot - 1);
        FlightVelocity = (Land - FlightStart - 0.5f * FVector(0, 0, -Gravity * 0.6f) * FMath::Square(In.CastFlight)) / In.CastFlight;
    }
    if (In.SinceCast < 0) bCastLaunched = false;

    const TArray<FFloatSlice>& Slices = FloatSlices();
    const int32 Steps = FMath::CeilToInt(Dt / (1 / 240.0f));
    const float H = Dt / Steps;
    for (int32 Step = 0; Step < Steps; ++Step)
    {
        // --- the float: weight and the buoyancy of every submerged slice, shared between K and T ---
        const FVector Axis = (FloatT - FloatK).GetSafeNormal();
        const bool bOverWater = ALakeEnvironment::IsWater(FloatK);
        const float Surface = LakeFish::SurfaceZ + FMath::Sin(Time * 2.1f) * 0.3f;
        float LiftK = 0, LiftT = 0, Wet = 0;
        if (bOverWater)
            for (const FFloatSlice& Slice : Slices)
            {
                const float Z = FloatK.Z + Axis.Z * Slice.S;
                const float Thick = FMath::Abs(Axis.Z) * FloatLength / Slices.Num() + 0.3f;
                const float Under = FMath::Clamp((Surface - Z) / Thick + 0.5f, 0.0f, 1.0f);
                const float B = Slice.Volume * Under;
                LiftK += B * (1 - Slice.S / FloatLength);
                LiftT += B * Slice.S / FloatLength;
                Wet += Under / Slices.Num();
            }
        // Water damps a floating float hard; in the air it barely slows.
        const float FloatDamp = FMath::Pow(Wet > 0.02f ? 0.03f : 0.6f, H);
        auto Integrate = [H](FVector& P, FVector& Prev, float Damp, const FVector& Acc)
        {
            const FVector V = (P - Prev) * Damp;
            Prev = P;
            P += V + Acc * H * H;
        };
        Integrate(FloatK, FloatKPrev, FloatDamp, FVector(0, 0, -Gravity + LiftK * Gravity / KeelMass));
        // The ripple rocks the antenna a little, so a float with nothing to hold it upright falls over.
        const FVector Rock = Wet > 0.02f ? FVector(FMath::Sin(Time * 1.3f), FMath::Cos(Time * 1.7f), 0) * 15.0f : FVector::ZeroVector;
        Integrate(FloatT, FloatTPrev, FloatDamp, FVector(0, 0, -Gravity + LiftT * Gravity / TipMass) + Rock);
        if (In.FishingState == 1 || In.FishingState == 2)
            if (!bFlying && Wet > 0.02f)
            {
                // The server keeps where the float is (it moves when the angler walks): drift towards it.
                const FVector Pull = FVector(In.FloatTarget.X - FloatK.X, In.FloatTarget.Y - FloatK.Y, 0) * (bHookHeld && In.FishingState == 2 ? 0.3f : 2.0f) * H * H;
                FloatK += Pull;
                FloatT += Pull;
            }
        // --- the shot and the hook: lead sinks steadily (water drag), the bait follows a fish ---
        const bool bShotWet = ALakeEnvironment::IsWater(ShotPos) && ShotPos.Z < LakeFish::SurfaceZ;
        Integrate(ShotPos, ShotPrev, FMath::Pow(bShotWet ? 0.0005f : 0.5f, H), FVector(0, 0, -Gravity * (bShotWet ? 0.91f : 1.0f)));
        if (bHookHeld)
        {
            HookPrev = HookPos;
            HookPos = FMath::Lerp(HookPos, HookTarget, 0.08f);
        }
        else if (bFishOut)
        {
            // Still in the water the fish is nearly weightless and the water holds it back hard;
            // in the air it swings on the line like a pendulum, the thrashing body slowing the swing.
            const bool bFishWet = ALakeEnvironment::IsWater(HookPos) && HookPos.Z < LakeFish::SurfaceZ - 3;
            Integrate(HookPos, HookPrev, FMath::Pow(bFishWet ? 0.02f : 0.45f, H), FVector(0, 0, -Gravity * (bFishWet ? 0.15f : 1.0f)));
        }
        else
        {
            const bool bHookWet = ALakeEnvironment::IsWater(HookPos) && HookPos.Z < LakeFish::SurfaceZ;
            Integrate(HookPos, HookPrev, FMath::Pow(bHookWet ? 0.0002f : 0.5f, H), FVector(0, 0, -Gravity * (bHookWet ? 0.5f : 1.0f)));
        }
        // The cast moves the float by hand: the rest of the rig hangs from it.
        if (In.bSweeping)
        {
            const FVector Move = (SweepPoint - FloatK) * 0.03f;
            FloatK += Move; FloatT += Move;
        }
        else if (bFlying && bCastLaunched)
        {
            const float T = FMath::Min(In.SinceCast, In.CastFlight);
            const FVector Arc = FlightStart + FlightVelocity * T + 0.5f * FVector(0, 0, -Gravity * 0.6f) * T * T;
            const FVector Move = (Arc - FloatK) * 0.12f * FMath::SmoothStep(0.0f, 0.25f, T);
            FloatK += Move; FloatT += Move;
        }

        // --- line and stick constraints, by mass ---
        const float WK = 1 / KeelMass, WT = 1 / TipMass, WS = 1 / ShotMass, WH = bHookHeld ? 0.0f : bFishOut ? 1 / In.FishGrams : 1 / HookMass;
        for (int32 Pass = 0; Pass < 8; ++Pass)
        {
            Rope(FloatK, WK, FloatT, WT, FloatLength, true);
            Rope(FloatK, WK, ShotPos, WS, ToShot);
            Rope(ShotPos, WS, HookPos, WH, ShotToHook);
            FVector Held = TipPos;
            Rope(Held, 0, FloatK, WK + WT, MainLength);      // the rod tip holds; the whole float follows
        }
        if (bFishOut)
        {
            // The rod holds the fish's whole weight: the hook stays within the line's reach of the tip.
            // A line stretched in the fight (the rod's give) comes back at a walking pace, so the fish
            // is drawn in instead of flung out.
            if (LandLine < 0) LandLine = FMath::Max(WholeLine, FVector::Dist(TipPos, HookPos));
            LandLine = FMath::Max(WholeLine, LandLine - 250.0f * H);
            FVector Held = TipPos;
            Rope(Held, 0, HookPos, 1, LandLine);
            // A thrashing fish on a 6 m line never flies: its speed is limited.
            const FVector Moved = HookPos - HookPrev;
            if (Moved.Size() > 450.0f * H) HookPrev = HookPos - Moved.GetSafeNormal() * 450.0f * H;
            // At the end the free hand takes the fish as it swings within reach.
            const float Reach = FMath::Lerp(70.0f, 260.0f, In.HandAlpha);
            if (In.HandAlpha > 0 && FVector::Dist(HookPos, In.HandPoint) < Reach)
            {
                const float Take = 1 - FMath::Exp(-4.0f * In.HandAlpha * H);
                HookPos = FMath::Lerp(HookPos, In.HandPoint, Take);
                HookPrev = FMath::Lerp(HookPrev, HookPos, Take);
            }
        }
        // In the air the line runs through both of the float's rings: it hangs along the line.
        if (Wet <= 0.02f)
        {
            const FVector Along = (FloatK - ShotPos).GetSafeNormal();
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
        const float MaxStep = (Wet > 0.02f && !bFlying && !In.bSweeping ? 200.0f : 2500.0f) * H;
        for (FVector* P : { &FloatK, &FloatT, &ShotPos, &HookPos })
        {
            FVector& Prev = P == &FloatK ? FloatKPrev : P == &FloatT ? FloatTPrev : P == &ShotPos ? ShotPrev : HookPrev;
            if (FVector::Dist(*P, Prev) > MaxStep) Prev = *P - (*P - Prev).GetSafeNormal() * MaxStep;
            // Ground and lake bed: what lies on the bottom stays put. A fish hanging over the bank
            // rests its body on the ground below the hook.
            const float Floor = ALakeEnvironment::GroundHeight(P->X, P->Y) + (bFishOut && P == &HookPos && !ALakeEnvironment::IsWater(*P) ? In.FishLength * 0.8f : 0.5f);
            if (P->Z < Floor) { P->Z = Floor; Prev = FMath::Lerp(Prev, *P, 0.5f); Prev.Z = Floor; }
        }
    }
    BobberPos = FloatK + (FloatT - FloatK).GetSafeNormal() * FloatPivot;
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
        const float Radius = FMath::Lerp(0.8f, 0.22f, (I + 0.5f) / BlankSegments);   // 16 mm butt section, 4 mm tip
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
    LeaderNow = bInitialised ? FMath::FInterpConstantTo(LeaderNow, TargetLeader, FMath::Min(DeltaSeconds, 1 / 30.0f), 120.0f) : TargetLeader;
    FRodInput In = Input;
    In.HookDepth = LeaderNow;
    // The line pulls the tip: the float rig's weight, the water's drag on it, the fish while fighting.
    FVector TipLoad = FVector::ZeroVector;
    if (Points.Num())
    {
        const FVector ToRig = (BobberPos - Tip()).GetSafeNormal();
        const float MainLength = LineLength - FMath::Clamp(In.HookDepth, 5.0f, 520.0f);
        const bool bTaut = FVector::Dist(BobberPos, Tip()) > MainLength * 0.985f;
        if (In.FishingState == 3) TipLoad = ToRig * (1500.0f + In.Tension * 75000.0f);
        else if (In.FishingState == 4) TipLoad = (HookPos - Tip()).GetSafeNormal() * (2500.0f + In.FishGrams * 30.0f);   // the fish's weight bends the rod
        else if (bTaut) TipLoad = ToRig * (bBobberInWater ? 2500.0f : In.bSweeping ? 6000.0f : 4000.0f);
    }
    SimulateBlank(In.Hold, TipLoad, DeltaSeconds);
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
        BobberPos = BobberPrev = FloatK + FVector(0, 0, 3.5f * BobberScale);
        bInitialised = true;
    }
    LastTip = Tip();
    UpdateBobber(In, DeltaSeconds);
    Layout(In);
    // Lines: main line from the tip to the bobber eye, leader from the eye to the hook.
    const float LeaderLength = FMath::Clamp(In.HookDepth, 5.0f, 520.0f);
    const FVector Eye = BobberEye->GetComponentLocation();
    SimulateLine(MainSim, Tip(), Eye, LineLength - LeaderLength, DeltaSeconds, true, bReset);
    // The leader runs through the shot to the hook.
    const float RigLeader = FMath::Max(LeaderLength - KeelDepth, 4.0f);
    const float ShotGap = FMath::Min(ShotAboveHook, RigLeader - 2.0f);
    SimulateLine(LeaderSim, Eye, ShotPos, RigLeader - ShotGap, DeltaSeconds, false, bReset);
    SimulateLine(TailSim, ShotPos, HookPos, ShotGap, DeltaSeconds, false, bReset);
    LayoutLine(MainSim, MainLine, 0.11f);
    LayoutLine(LeaderSim, Leader, 0.075f);
    LayoutLine(TailSim, Tail, 0.075f);
}
