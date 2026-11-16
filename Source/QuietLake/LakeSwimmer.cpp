#include "LakeSwimmer.h"

namespace
{
    constexpr float H = 1.0f / 240;
    constexpr float Gravity = 981;
    // A perch's build, mouth to tail fin: share of the mass, height of the body (of the tail fin) as a
    // share of its length, its width as a share of that height, and how much of a segment each point
    // stands for.
    constexpr float MassShare[] = {0.12f, 0.25f, 0.28f, 0.20f, 0.10f, 0.05f};
    constexpr float Height[] = {0.20f, 0.27f, 0.27f, 0.20f, 0.10f, 0.14f};
    constexpr float Width[] = {0.6f, 0.5f, 0.5f, 0.45f, 0.4f, 0.06f};
    constexpr float Slice[] = {0.5f, 1, 1, 1, 1, 0.5f};
    constexpr float PerchHeight = 0.27f;
    // The stroke: how far each joint bends (radians, head joint to tail joint) and the wave's lag
    // from joint to joint. The head hardly moves, the tail fin sweeps.
    constexpr float JointBend[] = {0.04f, 0.08f, 0.18f, 0.70f};
    constexpr float WaveLag = 1.6f;
    // Muscle torque limit per joint, in body weight times body length.
    constexpr float JointTorque[] = {0.8f, 0.8f, 0.7f, 0.6f};
    constexpr float CarriedWater = 1.2f;    // of a cylinder of water of the body's height
    constexpr float DragAcross = 0.8f;      // on the side area (and on the area from above)
    constexpr float DragAlong = 0.02f;      // on the wetted area,
    constexpr float WettedArea = 0.4f;      // which is this share of the length squared
    constexpr float DragBackward = 6;       // a backward slide: fins and scales against the flow
    constexpr float SlowAlong = 0.15f;      // per second, so a glide comes to rest
    constexpr float AirDrag = 0.6f;         // per second, out of the water
    constexpr float WakeShare = 0.3f;
    constexpr float IdleBeat = 0.15f;       // of the fastest beat, at no effort
    constexpr float FullStroke = 0.25f;     // effort at which the stroke reaches its full bend
    constexpr float SteerBend = 0.4f;       // radians per joint at full steering
    // Up and down the body hardly bends, and the water holds it far less than sideways: to climb the
    // fish drops its tail (radians per joint at full climb), which turns the stroke's push under it
    // and its nose up, and lifts with its pectoral fins.
    constexpr float TailTilt[] = {0.04f, 0.10f, 0.22f, 0.40f};
    constexpr float FinArea = 0.014f;       // both pectoral fins spread, of the length squared
    constexpr float FinDrag = 2;
    constexpr float FinRow = 0.02f;         // of the fish's weight, rowing up or down
    constexpr float FinTilt = 0.6f;         // radians at full climb
    constexpr float BankingRate = 16;       // per second: it rolls over to bank into a turn
    constexpr float RightingRate = 5;       // per second: the fish turns its back up in the water
    constexpr float MuscleGive = 0.000002f;
    // Steering (SteerTo): degrees off at which the bend is full, and seconds of turning it allows for.
    constexpr float SteerBand = 40, SteerEase = 0.1f, ClimbBand = 30, ClimbEase = 0.15f;
    constexpr float TurnQuick = 20;                 // per second: how fast the bend of a turn comes on
    constexpr float SteadyQuick = 20;               // per second: smoothing of the heading it steers by
    constexpr float BankFrom = 15, BankFull = 45;   // degrees up or down off: it starts to bank, lies right over   // degrees up or down off: it starts to bank, lies right over
    constexpr float LineLiesBack = 0.7f;    // how much of that pull across the body is at the third point (0.4 of the length), the rest at the second
    constexpr float DrawnFrom = 8, DrawnFull = 30;   // cm/s towards the pull: the fish is being drawn, the line no longer lies along it
    constexpr float LineDamping = 0.02f;    // seconds: the line's pull grows with how fast it stretches
    constexpr float BottomGrip = 6;         // per second: sliding along the bottom
}

void FLakeSwimmer::Reset(FVector Mouth, float BodyLength, float BodyGrams, FVector Forward, float BodyHeight)
{
    Length = FMath::Max(5.0f, BodyLength);
    Grams = FMath::Max(1.0f, BodyGrams);
    Time = Accumulator = Drive = Bend = Lift = Phase = Beat = MuscleUse = LineTension = 0;
    WantBack = FVector::ZeroVector;
    Steady = Forward.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
    MouthForce = FVector::ZeroVector;
    MouthSpring = 0;
    const FVector Dir = Forward.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
    Back = (FVector::UpVector - Dir * FVector::DotProduct(FVector::UpVector, Dir)).GetSafeNormal(UE_SMALL_NUMBER, FVector::RightVector);
    const float Segment = Length / (Count - 1);
    const float Deep = FMath::Max(0.05f, BodyHeight) / PerchHeight;
    float SideSum = 0;
    for (int32 I = 0; I < Count; ++I)
    {
        P[I] = Mouth - Dir * (Segment * I);
        V[I] = FVector::ZeroVector;
        Wet[I] = 1;
        Mass[I] = Grams * MassShare[I];
        const float Long = Segment * Slice[I];
        // (A slender fish is slender in its trunk: its fins and tail are no smaller for it.)
        Tall[I] = Height[I] * (I <= 2 ? Deep : FMath::Max(1.0f, Deep)) * Length;
        Wide[I] = Tall[I] * Width[I];
        CarriedSide[I] = CarriedWater * PI * FMath::Square(Tall[I] / 2) * Long;
        CarriedUp[I] = CarriedWater * PI * FMath::Square(Wide[I] / 2) * Long;
        SideArea[I] = Tall[I] * Long;
        TopArea[I] = Wide[I] * Long;
        SideSum += SideArea[I];
    }
    for (int32 I = 0; I < Count; ++I)
        Along[I] = 0.5f * DragAlong * WettedArea * Length * Length * (SideArea[I] / SideSum) / Mass[I];
    ShedMass = WakeShare * CarriedWater * PI * FMath::Square(Tall[Count - 1] / 2);
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

float FLakeSwimmer::WaterShare() const
{
    float Sum = 0;
    for (int32 I = 0; I < Count; ++I) Sum += Wet[I] * Mass[I];
    return Sum / Grams;
}

void FLakeSwimmer::SteerTo(const FVector& Want, float DeltaSeconds, float& Steering, float& Climb)
{
    Steering = Climb = 0;
    const FVector To = Want.GetSafeNormal();
    const FVector Was = Steady;
    Steady = FMath::VInterpTo(Steady, Heading(), DeltaSeconds, SteadyQuick).GetSafeNormal(UE_SMALL_NUMBER, Heading());
    if (To.IsNearlyZero()) { WantBack = FVector::ZeroVector; return; }
    // How fast it is turning, degrees a second, as a vector across the heading.
    const FVector Turning = DeltaSeconds > 0 ? (Steady - Was) * (57.3f / DeltaSeconds) : FVector::ZeroVector;
    const float Off = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(float(FVector::DotProduct(Steady, To)), -1.0f, 1.0f)));
    FVector Turn = To - Steady * FVector::DotProduct(Steady, To);
    const FVector LevelUp = (FVector::UpVector - Steady * Steady.Z).GetSafeNormal(UE_SMALL_NUMBER, Back);
    if (!Turn.Normalize()) Turn = FVector::CrossProduct(LevelUp, Steady);   // (dead astern: round over its side)
    FVector Banked = FVector::CrossProduct(Turn, Steady);
    if (Banked.Z < 0) Banked = -Banked;
    const float Bank = FMath::SmoothStep(BankFrom, BankFull, Off * FMath::Abs(float(FVector::DotProduct(Turn, LevelUp))));
    WantBack = FMath::Lerp(LevelUp, Banked, Bank).GetSafeNormal(UE_SMALL_NUMBER, LevelUp);
    const FVector Side = FVector::CrossProduct(Back, Steady);
    Steering = FMath::Clamp(-(Off * float(FVector::DotProduct(Turn, Side)) - SteerEase * float(FVector::DotProduct(Turning, Side))) / SteerBand, -1.0f, 1.0f);
    Climb = WaterShare() > 0.5f ? FMath::Clamp((Off * float(FVector::DotProduct(Turn, Back)) - ClimbEase * float(FVector::DotProduct(Turning, Back))) / ClimbBand, -1.0f, 1.0f) : 0.0f;
}

void FLakeSwimmer::Advance(float DeltaSeconds, float Effort, float Steering, float Climb)
{
    Accumulator += FMath::Clamp(DeltaSeconds, 0.0f, 0.1f);
    while (Accumulator >= H) { Step(Effort, Steering, Climb); Accumulator -= H; }
}

void FLakeSwimmer::Step(float Effort, float Steering, float Climb)
{
    Time += H;
    Drive = FMath::FInterpTo(Drive, FMath::Clamp(Effort, 0.0f, 1.0f), H, 10);
    Bend = FMath::FInterpTo(Bend, FMath::Clamp(Steering, -1.0f, 1.0f), H, TurnQuick);
    Lift = FMath::FInterpTo(Lift, FMath::Clamp(Climb, -1.0f, 1.0f), H, TurnQuick);
    // The fish sets its speed by how fast it beats; the stroke itself only fades in from rest.
    Beat = MaxBeat() * FMath::Lerp(IdleBeat, 1.0f, Drive);
    Phase = FMath::Fmod(Phase + 2 * PI * Beat * H, 2 * PI);
    const float Stroke = FMath::SmoothStep(0.0f, FullStroke, Drive);
    const float Segment = Length / (Count - 1);

    // Each point's own directions: towards the head, to the fish's side, to its back.
    FVector Before[Count], Axis[Count], Side[Count], Up[Count];
    float GiveSide[Count], GiveUp[Count];     // share of a push across the body the carried water takes
    auto Upright = [&](const FVector& Dir)
    {
        FVector Out = Back - Dir * FVector::DotProduct(Back, Dir);
        if (!Out.Normalize()) Out = FVector::CrossProduct(Dir, FVector::RightVector).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
        return Out;
    };
    for (int32 I = 0; I < Count; ++I)
    {
        Before[I] = P[I];
        Axis[I] = (P[FMath::Max(0, I - 1)] - P[FMath::Min(I + 1, Count - 1)]).GetSafeNormal();
        const FVector LocalUp = Upright(Axis[I]);
        const FVector LocalSide = FVector::CrossProduct(LocalUp, Axis[I]);
        // A fish lying on its side presents its width to the waterline, not its full height.
        const float VerticalSize = FMath::Sqrt(FMath::Square(Tall[I] * LocalUp.Z) + FMath::Square(Wide[I] * LocalSide.Z));
        Wet[I] = bWater ? FMath::Clamp((Surface - float(P[I].Z)) / FMath::Max(0.5f, VerticalSize) + 0.5f, 0.0f, 1.0f) : 0.0f;
    }
    // The back stays across the head; in the water the fish turns it up.
    Back = Upright(Axis[0]);
    const FVector BackTo = WantBack.IsNearlyZero() ? FVector::UpVector : WantBack;
    const FVector Level = BackTo - Axis[0] * FVector::DotProduct(BackTo, Axis[0]);
    if (Level.SizeSquared() > 0.05f)
        Back = FMath::Lerp(Back, Level.GetSafeNormal(), FMath::Min(1.0f, (WantBack.IsNearlyZero() ? RightingRate : BankingRate) * WaterShare() * H)).GetSafeNormal(UE_SMALL_NUMBER, Back);
    for (int32 I = 0; I < Count; ++I)
    {
        Up[I] = Upright(Axis[I]);
        Side[I] = FVector::CrossProduct(Up[I], Axis[I]);
        GiveSide[I] = CarriedSide[I] * Wet[I] / (Mass[I] + CarriedSide[I] * Wet[I]);
        GiveUp[I] = CarriedUp[I] * Wet[I] / (Mass[I] + CarriedUp[I] * Wet[I]);
    }
    // A push on a point moves it less across the body than along it.
    auto Moved = [&](int32 I, const FVector& Push)
    {
        return (Push - Side[I] * (GiveSide[I] * FVector::DotProduct(Push, Side[I])) - Up[I] * (GiveUp[I] * FVector::DotProduct(Push, Up[I]))) / Mass[I];
    };

    // The line pulls the mouth when taut.
    FVector OnMouth = MouthForce;
    if (MouthSpring > 0)
    {
        const float Pull = OnMouth.Size();
        if (Pull > UE_SMALL_NUMBER)
        {
            const FVector Dir = OnMouth / Pull;
            OnMouth = Dir * FMath::Max(0.0f, Pull - MouthSpring * float(FVector::DotProduct(P[0] - MouthAt, Dir)));
        }
    }
    LineTension = 0;
    if (LineLength > 0)
    {
        const FVector To = LineTip - P[0];
        const float Distance = To.Size();
        if (Distance > LineLength)
        {
            const FVector Dir = To / Distance;
            LineTension = FMath::Max(0.0f, LineStiffness * ((Distance - LineLength) - LineDamping * float(FVector::DotProduct(V[0], Dir))));
            OnMouth += Dir * LineTension;
        }
    }
    // The line is fast in the mouth, but a fish heading away from it has the line lying back along
    // its head and flank: what the line pulls along the body it pulls at the mouth, and what it pulls
    // across the body it pulls where it leaves the body, further back the more it comes from astern.
    {
        const float Pull = OnMouth.Size();
        // (Only while the fish holds its own: drawn back through the water by a pull stronger than it,
        // it has the line straight from its mouth again, and the pull swings its head round after the hook.)
        const float Drawn = Pull > UE_SMALL_NUMBER ? FMath::SmoothStep(DrawnFrom, DrawnFull, float(FVector::DotProduct(Velocity(), OnMouth)) / Pull) : 0.0f;
        const float Astern = Pull > UE_SMALL_NUMBER ? FMath::SmoothStep(0.2f, 0.9f, float(-FVector::DotProduct(OnMouth, Axis[0])) / Pull) * (1 - Drawn) : 0.0f;
        const FVector AlongBody = Axis[0] * FVector::DotProduct(OnMouth, Axis[0]);
        const FVector AcrossBody = OnMouth - AlongBody;
        V[0] += Moved(0, (AlongBody + AcrossBody * (1 - Astern)) * Gravity) * H;
        V[1] += Moved(1, AcrossBody * (Astern * (1 - LineLiesBack)) * Gravity) * H;
        V[2] += Moved(2, AcrossBody * (Astern * LineLiesBack) * Gravity) * H;
        // The hook in the corner of the jaw: the pull also turns the head, as a couple on the head's
        // segment.
        if (HookOffset != 0 && Pull > UE_SMALL_NUMBER)
        {
            const FVector Turn = FVector::CrossProduct(Side[0] * HookOffset, OnMouth);
            const FVector Couple = FVector::CrossProduct(Turn, Axis[0]) / FMath::Max(1.0f, Segment);
            V[0] += Moved(0, Couple * Gravity) * H;
            V[1] -= Moved(1, Couple * Gravity) * H;
        }
    }

    for (int32 I = 0; I < Count; ++I)
    {
        // Its weight, less what the water carries.
        V[I] += Moved(I, FVector(0, 0, Mass[I] * Gravity * (Buoyancy * Wet[I] - 1))) * H;
        float Forward = FVector::DotProduct(V[I], Axis[I]);
        float Sideways = FVector::DotProduct(V[I], Side[I]);
        float Rising = FVector::DotProduct(V[I], Up[I]);
        if (Wet[I] > 0)
        {
            // The tail fin's edge leaves the water it moved sideways behind.
            if (I == Count - 1 && Forward > 0) Sideways /= 1 + ShedMass * Wet[I] / (Mass[I] + CarriedSide[I] * Wet[I]) * Forward * H;
            Sideways /= 1 + 0.5f * DragAcross * SideArea[I] * Wet[I] / (Mass[I] + CarriedSide[I] * Wet[I]) * FMath::Abs(Sideways) * H;
            Rising /= 1 + 0.5f * DragAcross * TopArea[I] * Wet[I] / (Mass[I] + CarriedUp[I] * Wet[I]) * FMath::Abs(Rising) * H;
            Forward *= FMath::Exp(-SlowAlong * Wet[I] * H) / (1 + Along[I] * Wet[I] * (Forward >= 0 ? 1.0f : DragBackward) * FMath::Abs(Forward) * H);
        }
        V[I] = (Axis[I] * Forward + Side[I] * Sideways + Up[I] * Rising) * FMath::Exp(-AirDrag * (1 - Wet[I]) * H);
        // The pectoral fins, behind the head, work as a plane's wings (Tu and Terzopoulos, 1994): tilted
        // leading edge up, the water coming at the fish lifts its front; down, it pushes it under.
        if (I == 1 && Wet[I] > 0)
        {
            const float Angle = Lift * FinTilt;
            const FVector FinNormal = Up[I] * FMath::Cos(Angle) - Axis[I] * FMath::Sin(Angle);
            const float Into = FVector::DotProduct(V[I], FinNormal);
            const float Hold = 0.5f * FinDrag * FinArea * Length * Length * (0.3f + 0.7f * FMath::Abs(Lift)) * Wet[I] / (Mass[I] + CarriedUp[I] * Wet[I]);
            V[I] -= FinNormal * (Into - Into / (1 + Hold * FMath::Abs(Into) * H));
            // And they row: a small push up or down even when the fish stands still.
            V[I] += Moved(I, Up[I] * (Lift * FinRow * Grams * Gravity * Wet[I])) * H;
        }
        P[I] += V[I] * H;
    }

    float Muscle[Count] = {}, MuscleUp[Count] = {};
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
        // Muscles pull each joint towards the stroke's angle, as hard as they can and no harder:
        // sideways for the stroke and for a turn, up and down for a climb.
        for (int32 I = 1; I < Count - 1; ++I)
        {
            const float Most = Limit * JointTorque[I - 1];
            for (int32 Plane = 0; Plane < 2; ++Plane)
            {
                const FVector A = P[I] - P[I - 1], B = P[I + 1] - P[I];
                const float SizeA = A.Size();
                if (SizeA < 0.001f) continue;
                const FVector Dir = A / SizeA;
                const FVector Above = Upright(Dir);
                const FVector Across = Plane == 0 ? FVector::CrossProduct(Above, Dir) : Above;
                const float AlongB = FVector::DotProduct(B, Dir), AcrossB = FVector::DotProduct(B, Across);
                const float Angle = FMath::Atan2(AcrossB, AlongB);
                const float Wanted = Plane == 0 ? Stroke * JointBend[I - 1] * FMath::Sin(Phase - I * WaveLag) + Bend * SteerBend : -Lift * TailTilt[I - 1];
                const float Error = FMath::UnwindRadians(Angle - Wanted);
                const FVector G0 = Across / SizeA;
                const FVector G2 = (Across * AlongB - Dir * AcrossB) / FMath::Max(0.01f, AlongB * AlongB + AcrossB * AcrossB);
                const FVector G1 = -G0 - G2;
                const FVector M0 = Moved(I - 1, G0), M1 = Moved(I, G1), M2 = Moved(I + 1, G2);
                const float Denom = FVector::DotProduct(G0, M0) + FVector::DotProduct(G1, M1) + FVector::DotProduct(G2, M2) + Soft;
                float& Held = Plane == 0 ? Muscle[I] : MuscleUp[I];
                const float Total = FMath::Clamp(Held + (-Error - Soft * Held) / Denom, -Most, Most);
                const float Lambda = Total - Held;
                Held = Total;
                P[I - 1] += M0 * Lambda;
                P[I] += M1 * Lambda;
                P[I + 1] += M2 * Lambda;
            }
        }
        // The bottom holds the fish up.
        for (int32 I = 0; I < Count; ++I)
            if (P[I].Z < Floor + Wide[I] / 2) P[I].Z = Floor + Wide[I] / 2;
    }
    int32 AtLimit = 0;
    for (int32 I = 1; I < Count - 1; ++I)
        if (FMath::Abs(Muscle[I]) >= 0.999f * Limit * JointTorque[I - 1]) ++AtLimit;
    MuscleUse = FMath::FInterpTo(MuscleUse, AtLimit / float(Count - 2), H, 6);
    for (int32 I = 0; I < Count; ++I)
    {
        V[I] = (P[I] - Before[I]) / H;
        // Lying on the bottom, it does not slide far.
        if (P[I].Z <= Floor + Wide[I] / 2 + 0.01f) { V[I].X *= FMath::Exp(-BottomGrip * H); V[I].Y *= FMath::Exp(-BottomGrip * H); }
    }
}
