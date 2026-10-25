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

void ALakeRod::UpdateBobber(const FRodInput& In, float DeltaSeconds)
{
    // The float rig is two point masses on a line of fixed length: the bobber hangs on the main
    // line (tip -> bobber) and the hook with its shot on the leader (bobber -> hook, = float depth).
    // Everything - the cast, lifting the rig out of the water, a fish dragging the float - comes
    // from gravity, drag, buoyancy and these two length limits; nothing is teleported.
    const float Dt = FMath::Clamp(DeltaSeconds, 1 / 240.0f, 1 / 30.0f);
    const FVector TipPos = Tip();
    const float LeaderLength = FMath::Clamp(In.HookDepth, 5.0f, 520.0f);
    const float MainLength = LineLength - LeaderLength;
    if (In.Nibbles != SeenNibbles)
    {
        SeenNibbles = In.Nibbles;
        NibbleTime = Time;
    }
    // A take draws the float under steadily (about 40 cm/s), letting go lets it bob back up.
    SinkNow = FMath::FInterpConstantTo(SinkNow, In.FishingState == 2 ? 13.0f : 0.0f, Dt, In.FishingState == 2 ? 40.0f : 25.0f);
    // A nibble dips it a couple of centimetres for a moment, with a small sideways twitch.
    const float NibbleT = (Time - NibbleTime) / 0.35f;
    const float Dip = NibbleT >= 0 && NibbleT < 1 ? FMath::Sin(PI * NibbleT) : 0.0f;
    // The cast has two parts. While LMB is held the lowering rod lays the line out ahead of the
    // tip, the rig following a point that swings smoothly forward on the line; once released the
    // float drops along a ballistic arc to the landing point the server chose. The line limit
    // applies throughout.
    const bool bFlying = In.SinceCast >= 0 && In.SinceCast < In.CastFlight;
    const float SweepRad = FMath::DegreesToRadians(In.SweepAngle);
    const FVector SweepPoint = TipPos + (In.Facing * FMath::Cos(SweepRad) + FVector::UpVector * FMath::Sin(SweepRad)) * (MainLength - 2);
    if (bFlying && !bCastLaunched)
    {
        bCastLaunched = true;
        FlightStart = BobberPos;
        // Arc from wherever the sweep left the float to its waterline at the landing point.
        const FVector Land = In.FloatTarget + FVector(0, 0, 1);
        FlightVelocity = (Land - FlightStart - 0.5f * FVector(0, 0, -Gravity * 0.6f) * FMath::Square(In.CastFlight)) / In.CastFlight;
    }
    if (In.SinceCast < 0) bCastLaunched = false;

    const int32 Steps = FMath::CeilToInt(Dt / (1 / 240.0f));
    const float H = Dt / Steps;
    for (int32 Step = 0; Step < Steps; ++Step)
    {
        // --- bobber ---
        const bool bBobberWet = ALakeEnvironment::IsWater(BobberPos) && BobberPos.Z < LakeFish::SurfaceZ + 1;
        const FVector V = (BobberPos - BobberPrev) * FMath::Pow(bBobberWet ? 0.02f : 0.35f, H);
        FVector A(0, 0, -Gravity);
        if (bBobberWet)
        {
            // Buoyancy: the float rides at the waterline; a fish holding the bait pulls it under.
            const float Rest = LakeFish::SurfaceZ + FMath::Sin(Time * 2.1f) * 0.5f - SinkNow - Dip * 2.0f;
            A.Z += (Rest - BobberPos.Z) * 900 + Gravity;
            A.X += Dip * 60 * FMath::Sin(Time * 40);
            // Drift towards where the server keeps the float (it moves when the angler walks).
            if (In.FishingState == 1 || In.FishingState == 2)
                if (!bFlying) A += FVector(In.FloatTarget.X - BobberPos.X, In.FloatTarget.Y - BobberPos.Y, 0) * 2.0f;
        }
        BobberPrev = BobberPos;
        BobberPos += V + A * H * H;
        if (In.bSweeping)
            BobberPos = FMath::Lerp(BobberPos, SweepPoint, 0.03f);
        else if (bFlying && bCastLaunched)
        {
            // Hand over from the sweep to the arc gradually, so the float never jerks.
            const float T = FMath::Min(In.SinceCast, In.CastFlight);
            const FVector Arc = FlightStart + FlightVelocity * T + 0.5f * FVector(0, 0, -Gravity * 0.6f) * T * T;
            BobberPos = FMath::Lerp(BobberPos, Arc, 0.12f * FMath::SmoothStep(0.0f, 0.25f, T));
        }

        // --- hook with the lead shot: a fish holding it moves it, otherwise it falls or sinks ---
        const bool bHookPinned = In.LineFish && In.FishingState >= 2;
        if (bHookPinned)
        {
            // Follow the fish's mouth closely but without jumps (it may be a little away when it takes).
            HookPrev = HookPos;
            HookPos = FMath::Lerp(HookPos, In.LineFish->MouthLocation(), 0.06f);
        }
        else
        {
            const bool bHookWet = ALakeEnvironment::IsWater(HookPos) && HookPos.Z < LakeFish::SurfaceZ;
            const FVector HV = (HookPos - HookPrev) * FMath::Pow(bHookWet ? 0.01f : 0.5f, H);
            HookPrev = HookPos;
            HookPos += HV + FVector(0, 0, -Gravity * (bHookWet ? 0.25f : 1.0f)) * H * H;
        }

        // --- line limits (a rope only limits the maximum length) ---
        for (int32 Pass = 0; Pass < 3; ++Pass)
        {
            const FVector Eye = BobberPos - FVector(0, 0, BobberKeel);
            const FVector Lead = HookPos - Eye;
            if (Lead.Size() > LeaderLength)
            {
                const FVector Fix = Lead.GetSafeNormal() * (Lead.Size() - LeaderLength);
                BobberPos += Fix * (bHookPinned ? 0.6f : 0.35f);       // the shot is heavier than the float
                if (!bHookPinned) HookPos -= Fix * 0.65f;
            }
            const FVector Main = BobberPos - TipPos;
            if (Main.Size() > MainLength) BobberPos = TipPos + Main.GetSafeNormal() * MainLength;
        }
        // A light float rig never moves faster than ~25 m/s, even after a hitch or a sudden change.
        // On the water a float moves at most ~2 m/s; nothing ever throws it out of the water.
        const float MaxStep = (bBobberWet && !bFlying && !In.bSweeping ? 200.0f : 2500.0f) * H;
        if (FVector::Dist(BobberPos, BobberPrev) > MaxStep) BobberPrev = BobberPos - (BobberPos - BobberPrev).GetSafeNormal() * MaxStep;
        if (FVector::Dist(HookPos, HookPrev) > MaxStep) HookPrev = HookPos - (HookPos - HookPrev).GetSafeNormal() * MaxStep;
        // Ground and lake bed.
        BobberPos.Z = FMath::Max(BobberPos.Z, ALakeEnvironment::GroundHeight(BobberPos.X, BobberPos.Y) + 1.5f);
        HookPos.Z = FMath::Max(HookPos.Z, ALakeEnvironment::GroundHeight(HookPos.X, HookPos.Y) + 0.5f);
    }
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
    const FVector LeaderDir = (BobberPos - HookPos).GetSafeNormal();
    const FRotator BobberRot = bBobberInWater
        ? FRotator(FMath::Sin(Time * 1.7f) * 3 + (In.FishingState == 2 ? 25 : 0), 0, FMath::Cos(Time * 1.3f) * 3)
        : FRotationMatrix::MakeFromZ(LeaderDir.IsNearlyZero() ? FVector::UpVector : LeaderDir).Rotator();
    Bobber->SetWorldLocationAndRotation(BobberPos, BobberRot);
    Bobber->SetWorldScale3D(FVector(BobberScale));
    const FVector Eye = BobberEye->GetComponentLocation();
    Hook->SetWorldLocationAndRotation(HookPos, FRotationMatrix::MakeFromZX(Eye - HookPos, In.Hold.GetUnitAxis(EAxis::X)).Rotator());
    Sinker->SetWorldLocation(FMath::Lerp(HookPos, Eye, 0.25f));
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
        else if (bTaut) TipLoad = ToRig * (bBobberInWater ? 2500.0f : In.bSweeping ? 6000.0f : 4000.0f);
    }
    SimulateBlank(In.Hold, TipLoad, DeltaSeconds);
    // First frame or a teleport: hang the rig straight down from the tip.
    const bool bReset = !bInitialised || FVector::Dist(Tip(), LastTip) > 300;
    if (bReset)
    {
        BobberPos = BobberPrev = Tip() - FVector(0, 0, LineLength - FMath::Clamp(In.HookDepth, 5.0f, 520.0f));
        HookPos = HookPrev = BobberPos - FVector(0, 0, In.HookDepth);
        bInitialised = true;
    }
    LastTip = Tip();
    UpdateBobber(In, DeltaSeconds);
    Layout(In);
    // Lines: main line from the tip to the bobber eye, leader from the eye to the hook.
    const float LeaderLength = FMath::Clamp(In.HookDepth, 5.0f, 520.0f);
    const FVector Eye = BobberEye->GetComponentLocation();
    SimulateLine(MainSim, Tip(), Eye, LineLength - LeaderLength, DeltaSeconds, true, bReset);
    SimulateLine(LeaderSim, Eye, HookPos, LeaderLength, DeltaSeconds, false, bReset);
    LayoutLine(MainSim, MainLine, 0.11f);
    LayoutLine(LeaderSim, Leader, 0.075f);
}
