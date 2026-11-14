#include "LakeSwimmer.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
    constexpr float Easy = 0.15f, Brisk = 0.4f;

    struct FStroke
    {
        float Speed = 0, Stride = 0, HeadSweep = 0, TailSweep = 0, HeadYaw = 0;
    };

    // Swims straight for Seconds and measures the last second and a half: speed, distance per tail
    // beat, how far the snout and the tail fin swing aside of the line of travel (all in body
    // lengths) and how far the head turns off it (degrees).
    FStroke Swim(FLakeSwimmer& Fish, float Effort, float Seconds)
    {
        FStroke Out;
        float SpeedSum = 0, Low[2] = {1e9f, 1e9f}, High[2] = {-1e9f, -1e9f}, YawLow = 1e9f, YawHigh = -1e9f;
        int32 Samples = 0;
        const int32 Steps = FMath::RoundToInt(Seconds * 240);
        for (int32 I = 0; I < Steps; ++I)
        {
            Fish.Advance(1 / 240.0f, Effort, 0);
            if (I < Steps - 360) continue;
            const FVector Going = Fish.Velocity();
            const FVector Dir = Going.GetSafeNormal(), Side(-Dir.Y, Dir.X, 0), Centre = Fish.Centre();
            SpeedSum += Going.Size();
            ++Samples;
            const float Aside[2] = {float(FVector::DotProduct(Fish.P[0] - Centre, Side)), float(FVector::DotProduct(Fish.P[FLakeSwimmer::Count - 1] - Centre, Side))};
            for (int32 K = 0; K < 2; ++K) { Low[K] = FMath::Min(Low[K], Aside[K]); High[K] = FMath::Max(High[K], Aside[K]); }
            const float Yaw = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(float(FVector::DotProduct(Fish.Forward(), Side)), -1.0f, 1.0f)));
            YawLow = FMath::Min(YawLow, Yaw);
            YawHigh = FMath::Max(YawHigh, Yaw);
        }
        Out.Speed = SpeedSum / Samples / Fish.Length;
        Out.Stride = Out.Speed / Fish.Beat;
        Out.HeadSweep = (High[0] - Low[0]) / 2 / Fish.Length;
        Out.TailSweep = (High[1] - Low[1]) / 2 / Fish.Length;
        Out.HeadYaw = (YawHigh - YawLow) / 2;
        return Out;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeSwimmerTest, "QuietLake.Prototype.Swimmer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeSwimmerTest::RunTest(const FString& Parameters)
{
    // The same motion at any frame rate; the body keeps its length; a glide slows down and never
    // speeds up, but carries the fish on for a while.
    FVector Baseline = FVector::ZeroVector;
    for (float FPS : {60.0f, 30.0f, 120.0f})
    {
        FLakeSwimmer Fish;
        const FVector Start = Fish.Centre();
        for (int32 I = 0; I < FPS * 8; ++I) Fish.Advance(1 / FPS, Brisk, 0);
        const FVector Travel = Fish.Centre() - Start;
        AddInfo(FString::Printf(TEXT("%.0f fps: travel %s speed %.1f cm/s"), FPS, *Travel.ToCompactString(), Fish.Velocity().Size()));
        TestTrue(TEXT("Tail strokes propel an unanchored body forward"), Travel.X > 3 * Fish.Length && FMath::Abs(Travel.Y) < Travel.X * 0.3f);
        TestFalse(TEXT("Free swimming remains finite"), Fish.Centre().ContainsNaN());
        for (int32 I = 0; I < FLakeSwimmer::Count - 1; ++I)
            TestTrue(TEXT("The body does not stretch"), FMath::Abs(FVector::Distance(Fish.P[I], Fish.P[I + 1]) - Fish.Length / 5) < 0.1f);
        if (FPS == 60) Baseline = Travel;
        else TestTrue(TEXT("Fixed substeps preserve motion across frame rates"), FVector::Distance(Baseline, Travel) < 2);

        const float SwimmingSpeed = Fish.Velocity().Size();
        const FVector GlideFrom = Fish.Centre();
        float Last = SwimmingSpeed;
        bool bSpedUp = false;
        for (int32 I = 0; I < FPS * 5; ++I)
        {
            Fish.Advance(1 / FPS, 0, 0);
            if (I > FPS / 2 && I % FMath::RoundToInt(FPS / 4) == 0)
            {
                const float Now = Fish.Velocity().Size();
                bSpedUp |= Now > Last * 1.03f + 0.5f;
                Last = Now;
            }
        }
        const float Glide = FVector::Distance(Fish.Centre(), GlideFrom) / Fish.Length;
        AddInfo(FString::Printf(TEXT("%.0f fps: glide %.1f lengths in 5 s, speed %.1f -> %.1f cm/s"), FPS, Glide, SwimmingSpeed, Fish.Velocity().Size()));
        TestFalse(TEXT("A glide never speeds up"), bSpedUp);
        TestTrue(TEXT("Water slows the fish when muscles stop"), Fish.Velocity().Size() < SwimmingSpeed * 0.3f);
        TestTrue(TEXT("The fish glides on, it does not stop dead"), Glide > 1.5f);
    }

    // The stroke looks like a fish's: most of a body length per beat, the head nearly still, the
    // tail fin sweeping.
    {
        FLakeSwimmer Fish;
        const FStroke S = Swim(Fish, Brisk, 5);
        AddInfo(FString::Printf(TEXT("Brisk: %.2f lengths/s, %.2f lengths per beat, head +-%.3f, tail +-%.3f lengths, head yaw +-%.1f deg"), S.Speed, S.Stride, S.HeadSweep, S.TailSweep, S.HeadYaw));
        TestTrue(TEXT("A beat carries the fish about half a body length or more"), S.Stride > 0.45f && S.Stride < 0.8f);
        TestTrue(TEXT("The head hardly swings"), S.HeadSweep < 0.035f && S.HeadYaw < 6);
        TestTrue(TEXT("The tail fin sweeps wide"), S.TailSweep > 0.055f && S.TailSweep > 2 * S.HeadSweep);
    }
    {
        FLakeSwimmer Slow, Fast;
        const FStroke A = Swim(Slow, Easy, 5), B = Swim(Fast, 1, 4);
        AddInfo(FString::Printf(TEXT("Easy %.2f lengths/s at %.1f beats/s; burst %.2f lengths/s at %.1f beats/s, muscles at limit %.0f%%"), A.Speed, Slow.Beat, B.Speed, Fast.Beat, Fast.MuscleUse * 100));
        TestTrue(TEXT("An easy swim is about a body length a second"), A.Speed > 0.6f && A.Speed < 2);
        TestTrue(TEXT("A burst is several body lengths a second"), B.Speed > 4.5f && B.Speed < 9);
        // From rest the fish is off within a few beats.
        FLakeSwimmer Start;
        float Reached = -1;
        for (int32 I = 0; I < 480 && Reached < 0; ++I)
        {
            Start.Advance(1 / 240.0f, 1, 0);
            if (Start.Velocity().Size() > 0.5f * B.Speed * Start.Length) Reached = I / 240.0f;
        }
        AddInfo(FString::Printf(TEXT("From rest to half of the burst speed: %.2f s"), Reached));
        TestTrue(TEXT("A start from rest takes a fraction of a second"), Reached > 0 && Reached < 0.6f);
        // Weak muscles cannot keep the stroke up against the water.
        FLakeSwimmer Tired;
        Tired.Strength = 0.25f;
        const FStroke T = Swim(Tired, 1, 4);
        AddInfo(FString::Printf(TEXT("A quarter of the strength: burst %.2f lengths/s, muscles at limit %.0f%%"), T.Speed, Tired.MuscleUse * 100));
        TestTrue(TEXT("Weaker muscles make a slower fish"), T.Speed < 0.8f * B.Speed);
    }

    // No water, no travel: muscles are internal forces.
    {
        FLakeSwimmer Dry;
        Dry.bWater = false;
        const FVector Start = Dry.Centre();
        for (int32 I = 0; I < 2400; ++I) Dry.Advance(1 / 240.0f, 1, 0.5f);
        TestTrue(TEXT("Internal muscles cannot propel the centre of mass without water"), FVector::Distance(Start, Dry.Centre()) < 0.05f);
    }

    // Steering.
    {
        auto Turn = [](float Steering)
        {
            FLakeSwimmer Probe;
            float Sum = 0, Old = 0;
            for (int32 I = 0; I < 480; ++I)
            {
                Probe.Advance(1 / 240.0f, Brisk, Steering);
                const float Yaw = FMath::Atan2(float(Probe.Forward().Y), float(Probe.Forward().X));
                Sum += FMath::UnwindRadians(Yaw - Old);
                Old = Yaw;
            }
            return Sum;
        };
        const float LeftTurned = Turn(1), RightTurned = Turn(-1);
        AddInfo(FString::Printf(TEXT("Steering for 2 s: %.0f and %.0f degrees"), FMath::RadiansToDegrees(LeftTurned), FMath::RadiansToDegrees(RightTurned)));
        TestTrue(TEXT("Opposite muscle bias turns the body in opposite directions"), LeftTurned * RightTurned < 0);
        TestTrue(TEXT("A swimming fish turns round within a couple of seconds"), FMath::Abs(LeftTurned) > PI * 0.6f && FMath::Abs(RightTurned) > PI * 0.6f);

        // A held bend with no strokes: the fish coasts round and slows, it never gains speed.
        FLakeSwimmer Coast;
        for (int32 I = 0; I < 720; ++I) Coast.Advance(1 / 240.0f, Brisk, 0);
        float Last = Coast.Velocity().Size();
        bool bSpedUp = false;
        for (int32 I = 0; I < 960; ++I)
        {
            Coast.Advance(1 / 240.0f, 0, 1);
            if (I > 120 && I % 60 == 0)
            {
                const float Now = Coast.Velocity().Size();
                bSpedUp |= Now > Last * 1.03f + 0.5f;
                Last = Now;
            }
        }
        TestFalse(TEXT("Coasting in a bend never speeds up"), bSpedUp);
    }

    // A line on the mouth: a light pull is overcome, a pull of the fish's own weight is not.
    {
        FLakeSwimmer Light, Heavy;
        Light.MouthForce = FVector(-0.1f * Light.Grams, 0, 0);
        Heavy.MouthForce = FVector(-1.0f * Heavy.Grams, 0, 0);
        const FVector Start = Light.Centre();
        for (int32 I = 0; I < 720; ++I) { Light.Advance(1 / 240.0f, 1, 0); Heavy.Advance(1 / 240.0f, 1, 0); }
        AddInfo(FString::Printf(TEXT("Pulled back by the mouth for 3 s at full effort: 0.1 of its weight -> %.0f cm, its whole weight -> %.0f cm"), Light.Centre().X - Start.X, Heavy.Centre().X - Start.X));
        TestTrue(TEXT("The fish swims away against a light pull"), Light.Centre().X - Start.X > Light.Length);
        TestTrue(TEXT("A pull of its own weight drags the fish back"), Heavy.Centre().X - Start.X < -Heavy.Length);
        TestFalse(TEXT("A pulled fish remains finite"), Heavy.Centre().ContainsNaN() || Light.Centre().ContainsNaN());
    }
    return true;
}
#endif
