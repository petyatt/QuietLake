#include "LakeSwimmer.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLakeSwimmerTest,"QuietLake.Prototype.Swimmer",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FLakeSwimmerTest::RunTest(const FString& Parameters)
{
    FVector Baseline;
    for(float FPS : {60.0f,30.0f,120.0f})
    {
        FLakeSwimmer Fish; Fish.Reset();
        const FVector Start=Fish.Centre();
        for(int I=0;I<FPS*8;++I) Fish.Advance(1/FPS,0.8f,0);
        const FVector Travel=Fish.Centre()-Start;
        AddInfo(FString::Printf(TEXT("%.0f fps: travel %s speed %.2f"),FPS,*Travel.ToCompactString(),Fish.Velocity().Size()));
        TestTrue(TEXT("Tail strokes propel an unanchored body forward"),Travel.X>20 && FMath::Abs(Travel.Y)<Travel.X*0.5f);
        TestFalse(TEXT("Free swimming remains finite"),Fish.Centre().ContainsNaN());
        for(int I=0;I<5;++I) TestTrue(TEXT("The body does not stretch"),FMath::Abs(FVector::Distance(Fish.P[I],Fish.P[I+1])-Fish.Length/5)<0.1f);
        if(FPS==60) Baseline=Travel; else TestTrue(TEXT("Fixed substeps preserve motion across frame rates"),FVector::Distance(Baseline,Travel)<1);
        const float SwimmingSpeed=Fish.Velocity().Size();
        for(int I=0;I<FPS*5;++I) Fish.Advance(1/FPS,0,0);
        TestTrue(TEXT("Water slows the fish when muscles stop"),Fish.Velocity().Size()<SwimmingSpeed*0.15f);
    }
    FLakeSwimmer Dry; Dry.Reset(); Dry.bWater=false;
    const FVector Start=Dry.Centre();
    for(int I=0;I<2400;++I) Dry.Advance(1/240.0f,1,0);
    TestTrue(TEXT("Internal muscles cannot propel the centre of mass without water"),FVector::Distance(Start,Dry.Centre())<0.05f);
    FLakeSwimmer Left,Right; Left.Reset(); Right.Reset();
    for(int I=0;I<2400;++I) {Left.Advance(1/240.0f,.8f,.6f); Right.Advance(1/240.0f,.8f,-.6f);}
    AddInfo(FString::Printf(TEXT("Steering: left %s right %s"),*Left.Forward().ToCompactString(),*Right.Forward().ToCompactString()));
    TestTrue(TEXT("Opposite muscle bias turns the body in opposite directions"),Left.Forward().Y*Right.Forward().Y<0);
    return true;
}
#endif
