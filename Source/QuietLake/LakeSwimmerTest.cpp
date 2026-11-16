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

    // No water, no travel: muscles are internal forces. Out of the water the fish only falls.
    {
        FLakeSwimmer Dry;
        Dry.bWater = false;
        const FVector Start = Dry.Centre();
        for (int32 I = 0; I < 240; ++I) Dry.Advance(1 / 240.0f, 1, 0.5f, 0.5f);
        const FVector Moved = Dry.Centre() - Start;
        AddInfo(FString::Printf(TEXT("A second in the air at full effort: %s"), *Moved.ToCompactString()));
        TestTrue(TEXT("Internal muscles cannot propel the centre of mass without water"), FVector(Moved.X, Moved.Y, 0).Size() < 0.05f);
        TestTrue(TEXT("Out of the water the fish falls freely"), FMath::Abs(Moved.Z + 0.5f * 981) < 0.12f * 981);
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
    // Depth. Under water the fish weighs nothing; it climbs and dives with its pectoral fins and a
    // bend of the body, and holds its depth when it does neither.
    {
        FLakeSwimmer Still;
        for (int32 I = 0; I < 1200; ++I) Still.Advance(1 / 240.0f, 0, 0);
        TestTrue(TEXT("A still fish under water neither sinks nor floats up"), FMath::Abs(Still.Centre().Z) < 0.5f);
        // (The body's pitch, degrees: the front of the body, not the head alone.)
        auto PitchOf = [](const FLakeSwimmer& Fish) { return FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(float((Fish.P[0] - Fish.P[3]).GetSafeNormal().Z), -1.0f, 1.0f))); };
        auto Depth = [&](float Climb, float Effort)
        {
            FLakeSwimmer Fish;
            for (int32 I = 0; I < 240; ++I) Fish.Advance(1 / 240.0f, Effort, 0, Climb);
            return FVector2D(Fish.Centre().Z, PitchOf(Fish));
        };
        const FVector2D Up = Depth(1, Brisk), Down = Depth(-1, Brisk), Gentle = Depth(0.3f, Brisk), Hover = Depth(1, 0), Level = Depth(0, Brisk);
        AddInfo(FString::Printf(TEXT("1 s of brisk swimming: climbing %+.0f cm (pitch %+.0f), diving %+.0f cm (pitch %+.0f), a light touch %+.0f cm (pitch %+.0f), level %+.1f cm; standing still, fins rowing up: %+.1f cm (pitch %+.0f)"), Up.X, Up.Y, Down.X, Down.Y, Gentle.X, Gentle.Y, Level.X, Hover.X, Hover.Y));
        TestTrue(TEXT("The fish climbs"), Up.X > 4 && Up.Y > 25);
        TestTrue(TEXT("The fish dives"), Down.X < -4 && Down.Y < -25);
        TestTrue(TEXT("A light touch climbs gently"), Gentle.Y > 3 && Gentle.Y < Up.Y);
        TestTrue(TEXT("The fish holds its depth"), FMath::Abs(Level.X) < 1);
        TestTrue(TEXT("A standing fish lifts its nose with its fins"), Hover.Y > 3);
        // Held to a pitch by a simple damped controller, as the fish's brain will do it.
        for (float Effort : {Easy, Brisk})
        {
            FLakeSwimmer Fish;
            float Last = 0, Worst = 0;
            for (int32 I = 0; I < 1440; ++I)
            {
                const float Target = I < 720 ? -40 : 30;
                const float Now = PitchOf(Fish), Rate = (Now - Last) * 240;
                Last = Now;
                Fish.Advance(1 / 240.0f, Effort, 0, FMath::Clamp((Target - Now - 0.4f * Rate) / 30, -1.0f, 1.0f));
                if ((I > 480 && I < 720) || I > 1200) Worst = FMath::Max(Worst, FMath::Abs(PitchOf(Fish) - Target));
            }
            AddInfo(FString::Printf(TEXT("Effort %.2f: pitch asked -40 then +30, off by at most %.0f degrees two seconds on"), Effort, Worst));
            TestTrue(TEXT("The fish holds the pitch it is asked for"), Worst < 15);
        }

        FLakeSwimmer Rolled;
        Rolled.Back = FVector(0, 1, 0);
        for (int32 I = 0; I < 480; ++I) Rolled.Advance(1 / 240.0f, 0, 0);
        TestTrue(TEXT("A fish on its side rights itself in the water"), Rolled.Back.Z > 0.95f);

        FLakeSwimmer Deep;
        Deep.Floor = -15;
        float Lowest = 0;
        for (int32 I = 0; I < 960; ++I)
        {
            Deep.Advance(1 / 240.0f, Brisk, 0, -1);
            for (int32 K = 0; K < FLakeSwimmer::Count; ++K) Lowest = FMath::Min(Lowest, float(Deep.P[K].Z));
        }
        AddInfo(FString::Printf(TEXT("Diving at a bottom 15 cm below: lowest point %.1f cm"), Lowest));
        TestTrue(TEXT("The bottom stops a diving fish"), Lowest > -15.5f && Lowest < -8);
    }

    // The surface and a line. Drawn up by the mouth, the fish comes out of the water head first and
    // hangs; the line then carries its whole weight, and less while the water still carries a part.
    {
        FLakeSwimmer Fish;
        Fish.Reset(FVector(0, 0, -20));
        Fish.Surface = 0;
        Fish.LineTip = FVector(0, 0, 150);
        // (Thrashing: the whole body bends one way and the other, four times a second.)
        auto Hold = [&](float LineLength, float Seconds, float Effort, bool bThrash = false)
        {
            float Sum = 0, Peak = 0;
            int32 Samples = 0;
            const int32 Steps = FMath::RoundToInt(Seconds * 240);
            for (int32 I = 0; I < Steps; ++I)
            {
                // (Reeling in at 40 cm a second.)
                Fish.LineLength = FMath::Max(LineLength, Fish.LineLength - 40.0f / 240);
                Fish.Advance(1 / 240.0f, Effort, bThrash ? ((I / 30) % 2 ? 1.0f : -1.0f) : 0.0f);
                Peak = FMath::Max(Peak, Fish.LineTension);
                if (I >= Steps - 240) { Sum += Fish.LineTension; ++Samples; }
            }
            return FVector2D(Sum / Samples, Peak);
        };
        Fish.LineLength = 175;
        const FVector2D Slack = Hold(175, 1, 0);
        TestTrue(TEXT("A slack line does not pull"), Slack.Y < 1);
        // Mouth a third of a body length above the surface: the head is out, the tail in the water.
        const FVector2D Half = Hold(150 - Fish.Length / 3, 6, 0);
        const float HalfWet = Fish.WaterShare();
        AddInfo(FString::Printf(TEXT("Head held above the water: line %.0f gf of %.0f g, %.0f%% of the fish under water, mouth %.1f cm above, tail %.1f cm"), Half.X, Fish.Grams, HalfWet * 100, Fish.P[0].Z, Fish.P[FLakeSwimmer::Count - 1].Z));
        TestTrue(TEXT("With its tail in the water the fish weighs only its part in the air"), Half.X > 0.05f * Fish.Grams && Half.X < 0.75f * Fish.Grams);
        TestTrue(TEXT("The line carries what the water does not"), FMath::Abs(Half.X - (1 - HalfWet) * Fish.Grams) < 0.15f * Fish.Grams);
        // Mouth 90 cm above the surface: all of it in the air.
        const FVector2D Out = Hold(60, 8, 0);
        AddInfo(FString::Printf(TEXT("Hanging on the line: %.0f gf of %.0f g (peak %.0f gf while lifted), mouth %.1f cm, tail %.1f cm above the water"), Out.X, Fish.Grams, Out.Y, Fish.P[0].Z, Fish.P[FLakeSwimmer::Count - 1].Z));
        TestTrue(TEXT("Out of the water the line carries the whole fish"), FMath::Abs(Out.X - Fish.Grams) < 0.05f * Fish.Grams);
        TestTrue(TEXT("It hangs head up"), Fish.P[0].Z - Fish.P[FLakeSwimmer::Count - 1].Z > 0.9f * Fish.Length && Fish.WaterShare() < 0.01f);
        TestTrue(TEXT("Lifting does not jerk the line beyond a few weights"), Out.Y < 4 * Fish.Grams);
        // Thrashing on the line.
        const FVector2D Thrash = Hold(60, 3, 1, true);
        AddInfo(FString::Printf(TEXT("Thrashing on the line: %.0f gf on average, peak %.0f gf, mouth %.1f cm above the water"), Thrash.X, Thrash.Y, Fish.P[0].Z));
        TestTrue(TEXT("A thrashing fish jerks the line but stays on it"), Thrash.Y > 1.05f * Fish.Grams && Thrash.Y < 6 * Fish.Grams && FMath::Abs(Fish.P[0].Z - (90 - Fish.Grams / Fish.LineStiffness)) < 10);
        TestFalse(TEXT("A hanging fish remains finite"), Fish.Centre().ContainsNaN());
    }

    // Other builds: a slender fish and a deep-bodied one both swim.
    for (float BodyHeight : {0.15f, 0.34f})
    {
        FLakeSwimmer Fish;
        Fish.Reset(FVector::ZeroVector, 30, 350, FVector::ForwardVector, BodyHeight);
        const FStroke S = Swim(Fish, Brisk, 5);
        AddInfo(FString::Printf(TEXT("Body height %.2f of the length: %.2f lengths/s, %.2f per beat, head +-%.3f, tail +-%.3f"), BodyHeight, S.Speed, S.Stride, S.HeadSweep, S.TailSweep));
        TestTrue(TEXT("Slender and deep fish swim too"), S.Stride > 0.25f && S.Stride < 0.9f && S.TailSweep > S.HeadSweep);
    }
    // Held by the mouth on a short line, how hard does it pull? (In body weights; this is what the
    // game sets a species' strength by.)
    {
        auto Pull = [](float BodyHeight, float BeatScale, float Effort)
        {
            FLakeSwimmer Fish;
            Fish.BeatScale = BeatScale;
            Fish.Strength = BeatScale * BeatScale;
            Fish.Reset(FVector::ZeroVector, 30, 350, FVector::ForwardVector, BodyHeight);
            Fish.LineTip = FVector(-1, 0, 0);
            Fish.LineLength = 1;
            Fish.LineStiffness = 350;
            float Sum = 0;
            for (int32 I = 0; I < 960; ++I)
            {
                Fish.Advance(1 / 240.0f, Effort, 0);
                if (I >= 480) Sum += Fish.LineTension;
            }
            return Sum / 480 / Fish.Grams;
        };
        FString Line;
        for (float BodyHeight : {0.15f, 0.24f, 0.27f, 0.34f})
            Line += FString::Printf(TEXT(" height %.2f: %.2f / %.2f / %.2f;"), BodyHeight, Pull(BodyHeight, 1, 1), Pull(BodyHeight, 1.5f, 1), Pull(BodyHeight, 2, 1));
        AddInfo(TEXT("Pull on a tether at full effort, body weights, beat scale 1 / 1.5 / 2:") + Line);
        AddInfo(FString::Printf(TEXT("Perch build, beat scale 1.5: effort 0.25 %.2f, 0.5 %.2f, 0.75 %.2f, 1 %.2f"), Pull(0.27f, 1.5f, 0.25f), Pull(0.27f, 1.5f, 0.5f), Pull(0.27f, 1.5f, 0.75f), Pull(0.27f, 1.5f, 1)));
        TestTrue(TEXT("A faster beat pulls harder"), Pull(0.27f, 1.5f, 1) > 1.5f * Pull(0.27f, 1, 1));
    }
    return true;
}

// A fish on a line from above (a pole's tip over the water), boring down against it as the game's
// hooked fish does: can it keep its nose down and hold its depth?
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeSwimmerLineTest, "QuietLake.Prototype.SwimmerLine", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLakeSwimmerLineTest::RunTest(const FString& Parameters)
{
    for (float Effort : {0.78f, 0.5f})
    {
        FLakeSwimmer Fish;
        Fish.BeatScale = 1.84f;
        Fish.Strength = 1.84f * 1.84f;
        Fish.Reset(FVector(0, 0, -110), 38, 800);
        Fish.Surface = 0;
        Fish.Floor = -180;
        Fish.LineTip = FVector(-230, 0, 440);
        Fish.LineLength = 595;
        Fish.LineStiffness = 7;
        float DepthSum = 0, TensionSum = 0, PitchSum = 0, Shallowest = 1000, BackLow = 1;
        int32 Samples = 0;
        const float Dt = 1 / 50.0f;
        for (int32 I = 0; I < 750; ++I)
        {
            // Against the line, nose down but not on its head, keeping the way it was heading.
            const FVector Away = -(Fish.LineTip - Fish.P[0]).GetSafeNormal();
            const float Dive = FMath::Clamp(float(Away.Z), -0.87f, 0.0f);
            FVector Level(Away.X, Away.Y, 0);
            const float Sure = FMath::Clamp(float(Level.Size()) * 2, 0.0f, 1.0f);
            Level = Level.GetSafeNormal() * Sure + FVector(Fish.Steady.X, Fish.Steady.Y, 0).GetSafeNormal() * (1 - Sure);
            Level.Normalize();
            float Steering = 0, Climb = 0;
            Fish.SteerTo(Level * FMath::Sqrt(FMath::Max(0.0f, 1 - Dive * Dive)) + FVector(0, 0, Dive), Dt, Steering, Climb);
            Fish.Advance(Dt, Effort, Steering, Climb);
            if (I >= 150)
            {
                const float Depth = -float(Fish.Centre().Z);
                DepthSum += Depth; TensionSum += Fish.LineTension; Shallowest = FMath::Min(Shallowest, Depth); BackLow = FMath::Min(BackLow, float(Fish.Back.Z));
                PitchSum += FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(float(Fish.Heading().Z), -1.0f, 1.0f)));
                ++Samples;
            }
        }
        AddInfo(FString::Printf(TEXT("Effort %.2f under a line from above for 12 s: depth %.0f cm on average (shallowest %.0f), pitch %+.0f, line %.0f gf of %.0f g, back never lower than %.1f"),
            Effort, DepthSum / Samples, Shallowest, PitchSum / Samples, TensionSum / Samples, Fish.Grams, BackLow));
        TestTrue(TEXT("Boring down against a line from above, the fish keeps its depth"), Shallowest > 10 && DepthSum / Samples > 80);   // (pulled up now and then, it bores down again)
        TestTrue(TEXT("It keeps its nose down"), PitchSum / Samples < -30);
        TestTrue(TEXT("It does not roll over on its back"), BackLow > -0.5f);
        if (Effort > 0.7f) TestTrue(TEXT("It loads the line with most of its weight"), TensionSum / Samples > 0.6f * Fish.Grams);
    }
    return true;
}
#endif
