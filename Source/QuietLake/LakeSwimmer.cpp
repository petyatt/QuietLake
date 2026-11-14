#include "LakeSwimmer.h"

namespace
{
    constexpr float H = 1.0f / 240;
    constexpr float Gravity = 981;
    // A perch's build, mouth to tail fin: share of the mass, height of the body (of the tail fin) as a
    // share of its length, and how much of a segment each point stands for.
    constexpr float MassShare[] = {0.12f, 0.25f, 0.28f, 0.20f, 0.10f, 0.05f};
    constexpr float Height[] = {0.20f, 0.27f, 0.27f, 0.20f, 0.10f, 0.14f};
    constexpr float Slice[] = {0.5f, 1, 1, 1, 1, 0.5f};
    // The stroke: how far each joint bends (radians, head joint to tail joint) and the wave's lag
    // from joint to joint. The head hardly moves, the tail fin sweeps.
    constexpr float JointBend[] = {0.04f, 0.08f, 0.18f, 0.70f};
    constexpr float WaveLag = 1.6f;
    // Muscle torque limit per joint, in body weight times body length.
    constexpr float JointTorque[] = {0.8f, 0.8f, 0.7f, 0.6f};
    constexpr float CarriedWater = 1.2f;    // of a cylinder of water of the body's height
    constexpr float DragAcross = 0.8f;      // on the side area
    constexpr float DragAlong = 0.02f;      // on the wetted area,
    constexpr float WettedArea = 0.4f;      // which is this share of the length squared
    constexpr float DragBackward = 6;       // a backward slide: fins and scales against the flow
    constexpr float SlowAlong = 0.15f;      // per second, so a glide comes to rest
    constexpr float WakeShare = 0.3f;
    constexpr float IdleBeat = 0.15f;       // of the fastest beat, at no effort
    constexpr float FullStroke = 0.25f;     // effort at which the stroke reaches its full bend
    constexpr float SteerBend = 0.4f;       // radians per joint at full steering
    constexpr float MuscleGive = 0.000002f;

    FVector Perp(FVector A) { return FVector(-A.Y, A.X, 0); }
}

void FLakeSwimmer::Reset(FVector Mouth, float BodyLength, float BodyGrams)
{
    Length = FMath::Max(5.0f, BodyLength);
    Grams = FMath::Max(1.0f, BodyGrams);
    Time = Accumulator = Drive = Bend = Phase = Beat = MuscleUse = 0;
    MouthForce = FVector::ZeroVector;
    const float Segment = Length / (Count - 1);
    float Side[Count], SideSum = 0;
    for (int32 I = 0; I < Count; ++I)
    {
        P[I] = Mouth - FVector(Segment * I, 0, 0);
        V[I] = FVector::ZeroVector;
        Normal[I] = FVector(0, 1, 0);
        Mass[I] = Grams * MassShare[I];
        const float Tall = Height[I] * Length, Long = Segment * Slice[I];
        Carried[I] = CarriedWater * PI * FMath::Square(Tall / 2) * Long;
        Side[I] = Tall * Long;
        SideSum += Side[I];
    }
    for (int32 I = 0; I < Count; ++I)
    {
        Across[I] = 0.5f * DragAcross * Side[I] / (Mass[I] + Carried[I]);
        Along[I] = 0.5f * DragAlong * WettedArea * Length * Length * (Side[I] / SideSum) / Mass[I];
    }
    Shed = WakeShare * CarriedWater * PI * FMath::Square(Height[Count - 1] * Length / 2) / (Mass[Count - 1] + Carried[Count - 1]);
}

FVector FLakeSwimmer::Centre() const
{
    FVector Sum = FVector::ZeroVector;
    for (int32 I = 0; I < Count; ++I) Sum += P[I] * Mass[I];
    return Sum / Grams;
}

FVector FLakeSwimmer::Velocity() const
{
    FVector Sum = FVector::ZeroVector;
    for (int32 I = 0; I < Count; ++I) Sum += V[I] * Mass[I];
    return Sum / Grams;
}

void FLakeSwimmer::Advance(float DeltaSeconds, float Effort, float Steering)
{
    Accumulator += FMath::Clamp(DeltaSeconds, 0.0f, 0.1f);
    while (Accumulator >= H) { Step(Effort, Steering); Accumulator -= H; }
}

void FLakeSwimmer::Step(float Effort, float Steering)
{
    Time += H;
    Drive = FMath::FInterpTo(Drive, FMath::Clamp(Effort, 0.0f, 1.0f), H, 10);
    Bend = FMath::FInterpTo(Bend, FMath::Clamp(Steering, -1.0f, 1.0f), H, 8);
    // The fish sets its speed by how fast it beats; the stroke itself only fades in from rest.
    Beat = MaxBeat() * FMath::Lerp(IdleBeat, 1.0f, Drive);
    Phase = FMath::Fmod(Phase + 2 * PI * Beat * H, 2 * PI);
    const float Stroke = FMath::SmoothStep(0.0f, FullStroke, Drive);
    const float Segment = Length / (Count - 1);

    FVector Before[Count], Axis[Count];
    float Give[Count];      // share of a sideways push the carried water takes
    for (int32 I = 0; I < Count; ++I)
    {
        Before[I] = P[I];
        Axis[I] = (P[FMath::Max(0, I - 1)] - P[FMath::Min(I + 1, Count - 1)]).GetSafeNormal();   // towards the head
        Normal[I] = Perp(Axis[I]);
        Give[I] = bWater ? Carried[I] / (Mass[I] + Carried[I]) : 0;
    }
    // A push on a point moves it less across the body than along it.
    auto Moved = [&](int32 I, const FVector& Push)
    {
        return (Push - Normal[I] * (Give[I] * FVector::DotProduct(Push, Normal[I]))) / Mass[I];
    };
    V[0] += Moved(0, MouthForce * Gravity) * H;
    for (int32 I = 0; I < Count; ++I)
    {
        if (bWater)
        {
            float Forward = FVector::DotProduct(V[I], Axis[I]);
            float Sideways = FVector::DotProduct(V[I], Normal[I]);
            // The tail fin's edge leaves the water it moved sideways behind.
            if (I == Count - 1 && Forward > 0) Sideways /= 1 + Shed * Forward * H;
            Sideways /= 1 + Across[I] * FMath::Abs(Sideways) * H;
            Forward *= FMath::Exp(-SlowAlong * H) / (1 + Along[I] * (Forward >= 0 ? 1.0f : DragBackward) * FMath::Abs(Forward) * H);
            V[I] = Axis[I] * Forward + Normal[I] * Sideways;
        }
        P[I] += V[I] * H;
    }

    float Muscle[Count] = {};
    const float Soft = MuscleGive / (Grams * H * H);
    const float Limit = FMath::Max(0.0f, Strength) * Grams * Gravity * Length * H * H;
    for (int32 Pass = 0; Pass < 12; ++Pass)
    {
        // The body does not stretch.
        for (int32 I = 0; I < Count - 1; ++I)
        {
            const FVector D = P[I + 1] - P[I];
            const float Distance = D.Size();
            if (Distance < 0.001f) continue;
            const FVector Dir = D / Distance;
            const FVector A = Moved(I, Dir), B = Moved(I + 1, Dir);
            const float Fix = (Distance - Segment) / (FVector::DotProduct(A, Dir) + FVector::DotProduct(B, Dir));
            P[I] += A * Fix;
            P[I + 1] -= B * Fix;
        }
        // Muscles pull each joint towards the stroke's angle, as hard as they can and no harder.
        for (int32 I = 1; I < Count - 1; ++I)
        {
            const FVector A = P[I] - P[I - 1], B = P[I + 1] - P[I];
            const float Angle = FMath::Atan2(float(FVector::CrossProduct(A, B).Z), float(FVector::DotProduct(A, B)));
            const float Wanted = Stroke * JointBend[I - 1] * FMath::Sin(Phase - I * WaveLag) + Bend * SteerBend;
            const float Error = FMath::UnwindRadians(Angle - Wanted);
            const FVector G0 = Perp(A) / FMath::Max(0.01, A.SizeSquared());
            const FVector G2 = Perp(B) / FMath::Max(0.01, B.SizeSquared());
            const FVector G1 = -G0 - G2;
            const FVector M0 = Moved(I - 1, G0), M1 = Moved(I, G1), M2 = Moved(I + 1, G2);
            const float Denom = FVector::DotProduct(G0, M0) + FVector::DotProduct(G1, M1) + FVector::DotProduct(G2, M2) + Soft;
            const float Most = Limit * JointTorque[I - 1];
            const float Total = FMath::Clamp(Muscle[I] + (-Error - Soft * Muscle[I]) / Denom, -Most, Most);
            const float Lambda = Total - Muscle[I];
            Muscle[I] = Total;
            P[I - 1] += M0 * Lambda;
            P[I] += M1 * Lambda;
            P[I + 1] += M2 * Lambda;
        }
    }
    int32 AtLimit = 0;
    for (int32 I = 1; I < Count - 1; ++I)
        if (FMath::Abs(Muscle[I]) >= 0.999f * Limit * JointTorque[I - 1]) ++AtLimit;
    MuscleUse = FMath::FInterpTo(MuscleUse, AtLimit / float(Count - 2), H, 6);
    for (int32 I = 0; I < Count; ++I) V[I] = (P[I] - Before[I]) / H;
}
