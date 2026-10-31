#include "LakeWeather.h"
#include "LakeGame.h"
#include "LakeMap.h"
#include "Net/UnrealNetwork.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/AudioComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Sound/SoundWave.h"
#include "DynamicWindParameters.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "TimerManager.h"

namespace
{
    float S(float A, float B, float X) { return FMath::SmoothStep(A, B, X); }

    /** Typical weathers with ranges for each value; the server rolls one and blends towards it. */
    struct FPreset { const TCHAR* Name; float Weight; FVector2f Cloud, Fog, Rain, Wind; bool bFoggy, bRainy; };
    const FPreset GPresets[] = {
        { TEXT("Clear calm"),   1.2f, { 0.0f, 0.25f }, { 0.0f, 0.15f }, { 0, 0 },          { 0.0f, 0.12f }, false, false },
        { TEXT("Clear breezy"), 1.0f, { 0.1f, 0.35f }, { 0.0f, 0.05f }, { 0, 0 },          { 0.25f, 0.5f }, false, false },
        { TEXT("Cloudy"),       1.2f, { 0.45f, 0.7f }, { 0.0f, 0.2f },  { 0, 0 },          { 0.15f, 0.45f }, false, false },
        { TEXT("Overcast"),     0.9f, { 0.8f, 1.0f },  { 0.1f, 0.3f },  { 0, 0 },          { 0.1f, 0.4f }, false, false },
        { TEXT("Windy"),        0.5f, { 0.3f, 0.6f },  { 0.0f, 0.1f },  { 0, 0 },          { 0.6f, 0.9f }, false, false },
        { TEXT("Mist"),         0.8f, { 0.2f, 0.6f },  { 0.55f, 0.8f }, { 0, 0 },          { 0.0f, 0.1f }, true, false },
        { TEXT("Dense fog"),    0.4f, { 0.5f, 0.8f },  { 0.85f, 1.0f }, { 0, 0 },          { 0.0f, 0.06f }, true, false },
        { TEXT("Drizzle"),      0.7f, { 0.85f, 1.0f }, { 0.25f, 0.45f },{ 0.1f, 0.3f },   { 0.05f, 0.3f }, false, true },
        { TEXT("Rain"),         0.6f, { 0.9f, 1.0f },  { 0.2f, 0.35f }, { 0.35f, 0.65f }, { 0.2f, 0.5f }, false, true },
        { TEXT("Downpour"),     0.3f, { 1.0f, 1.0f },  { 0.25f, 0.4f }, { 0.75f, 1.0f },  { 0.5f, 0.9f }, false, true },
    };
}

FLakeWeatherState FLakeWeatherState::Blend(const FLakeWeatherState& A, const FLakeWeatherState& B, float T)
{
    FLakeWeatherState R;
    R.Cloud = FMath::Lerp(A.Cloud, B.Cloud, T);
    R.Fog = FMath::Lerp(A.Fog, B.Fog, T);
    R.Rain = FMath::Lerp(A.Rain, B.Rain, T);
    R.Wind = FMath::Lerp(A.Wind, B.Wind, T);
    R.WindYaw = A.WindYaw + FMath::FindDeltaAngleDegrees(A.WindYaw, B.WindYaw) * T;
    return R;
}

ALakeWeather::ALakeWeather()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;   // after the camera has moved
    bReplicates = true;
    bAlwaysRelevant = true;
    SetNetUpdateFrequency(2);
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void ALakeWeather::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ALakeWeather, ClockOriginServer);
    DOREPLIFETIME(ALakeWeather, ClockOriginHours);
    DOREPLIFETIME(ALakeWeather, From);
    DOREPLIFETIME(ALakeWeather, To);
    DOREPLIFETIME(ALakeWeather, BlendStart);
    DOREPLIFETIME(ALakeWeather, BlendSeconds);
    DOREPLIFETIME(ALakeWeather, WaterTemp);
    DOREPLIFETIME(ALakeWeather, Pressure);
    DOREPLIFETIME(ALakeWeather, Trend);
}

ALakeWeather* ALakeWeather::Get(const UWorld* World)
{
    if (!World) return nullptr;
    for (TActorIterator<ALakeWeather> It(const_cast<UWorld*>(World)); It; ++It) return *It;
    return nullptr;
}

float ALakeWeather::ServerNow() const
{
    const AGameStateBase* State = GetWorld() ? GetWorld()->GetGameState() : nullptr;
    return State ? State->GetServerWorldTimeSeconds() : (GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f);
}

float ALakeWeather::Hours() const
{
    const float H = ClockOriginHours + (ServerNow() - ClockOriginServer) / (DayMinutes * 60.0f) * 24.0f;
    return FMath::Fmod(FMath::Fmod(H, 24.0f) + 24.0f, 24.0f);
}

FLakeWeatherState ALakeWeather::Current() const
{
    const float T = FMath::Clamp((ServerNow() - BlendStart) / FMath::Max(0.01f, BlendSeconds), 0.0f, 1.0f);
    return FLakeWeatherState::Blend(From, To, FMath::SmoothStep(0.0f, 1.0f, T));
}

FVector ALakeWeather::SunDirection(float* OutElevation) const
{
    // Late summer at about 56 degrees north; solar noon at 13:00 local time.
    const float Lat = FMath::DegreesToRadians(56.0f), Dec = FMath::DegreesToRadians(17.0f);
    const float HourAngle = FMath::DegreesToRadians((Hours() - 13.0f) * 15.0f);
    const float SinE = FMath::Sin(Lat) * FMath::Sin(Dec) + FMath::Cos(Lat) * FMath::Cos(Dec) * FMath::Cos(HourAngle);
    const float E = FMath::Asin(FMath::Clamp(SinE, -1.0f, 1.0f));
    // Azimuth from north, clockwise.
    const float A = FMath::Atan2(FMath::Sin(HourAngle), FMath::Cos(HourAngle) * FMath::Sin(Lat) - FMath::Tan(Dec) * FMath::Cos(Lat)) + PI;
    if (OutElevation) *OutElevation = FMath::RadiansToDegrees(E);
    // World: +X east, +Y south (Unreal's yaw turns clockwise seen from above).
    return FVector(FMath::Cos(E) * FMath::Sin(A), -FMath::Cos(E) * FMath::Cos(A), FMath::Sin(E));
}

float ALakeWeather::Daylight() const
{
    float E;
    SunDirection(&E);
    return S(-6.0f, 4.0f, E);
}

float ALakeWeather::EffectiveFog() const
{
    const FLakeWeatherState W = Current();
    const float H = Hours();
    // Mist lies on the water on calm mornings and, less, after sunset.
    const float Morning = S(3.0f, 4.5f, H) * (1 - S(7.5f, 9.5f, H));
    const float Evening = S(20.5f, 22.5f, H) * 0.5f + (H < 3.0f ? 0.5f : 0.0f);
    return FMath::Clamp(W.Fog + (0.45f * Morning + 0.25f * Evening) * (1 - S(0.2f, 0.5f, W.Wind)) * (1 - W.Rain), 0.0f, 1.0f);
}

float ALakeWeather::TemperatureC() const
{
    const FLakeWeatherState W = Current();
    const float Day = Daylight();
    // Coolest before dawn, warmest mid-afternoon; rain and wind chill, clouds keep the night warm.
    float T = 17.0f + 6.0f * FMath::Cos(2 * PI * (Hours() - 15.0f) / 24.0f);
    T += -4.0f * W.Rain - 1.5f * W.Wind - 2.5f * W.Cloud * Day + 1.5f * W.Cloud * (1 - Day);
    return T;
}

FString ALakeWeather::Describe() const
{
    const FLakeWeatherState W = Current();
    const float FogNow = EffectiveFog();
    FString Sky;
    if (W.Rain > 0.75f) Sky = TEXT("Ливень");
    else if (W.Rain > 0.35f) Sky = TEXT("Дождь");
    else if (W.Rain > 0.08f) Sky = TEXT("Морось");
    else if (FogNow > 0.85f) Sky = TEXT("Густой туман");
    else if (FogNow > 0.55f) Sky = TEXT("Туман");
    else if (FogNow > 0.32f) Sky = TEXT("Дымка");
    else if (W.Cloud > 0.8f) Sky = TEXT("Пасмурно");
    else if (W.Cloud > 0.45f) Sky = TEXT("Облачно");
    else Sky = Daylight() > 0.3f ? TEXT("Ясно") : TEXT("Ясная ночь");
    const int32 Ms = FMath::RoundToInt(WindSpeed());
    const FString WindText = Ms < 1 ? TEXT("штиль") : FString::Printf(TEXT("ветер %d м/с"), Ms);
    return Sky + TEXT("  ·  ") + WindText;
}

void ALakeWeather::SetHours(float NewHours)
{
    ClockOriginServer = ServerNow();
    ClockOriginHours = NewHours;
    ForceNetUpdate();
}

float ALakeWeather::Shock() const
{
    // How big the last change was: rain and wind count most.
    const float Size = FMath::Clamp(FMath::Abs(To.Cloud - From.Cloud) * 0.6f + FMath::Abs(To.Rain - From.Rain) * 1.2f
        + FMath::Abs(To.Wind - From.Wind) * 0.8f + FMath::Abs(To.Fog - From.Fog) * 0.3f, 0.0f, 1.0f);
    const float Now = ServerNow();
    if (Now < BlendStart) return 0;
    const float During = FMath::Clamp((Now - BlendStart) / FMath::Max(0.01f, BlendSeconds), 0.0f, 1.0f);
    const float AfterHours = (Now - BlendStart - BlendSeconds) * GameHoursPerSecond();
    return Size * (AfterHours < 0 ? During : 1 - S(0.0f, 3.0f, AfterHours));
}

float ALakeWeather::StableHours() const
{
    return FMath::Max(0.0f, (ServerNow() - BlendStart - BlendSeconds) * GameHoursPerSecond());
}

FFishConditions ALakeWeather::FishConditions() const
{
    const FLakeWeatherState W = Current();
    FFishConditions C;
    SunDirection(&C.SunElevation);
    C.bMorning = Hours() < 13.0f;
    C.WaterTempC = WaterTemp;
    C.Cloud = W.Cloud;
    C.Rain = W.Rain;
    C.Wind = W.Wind;
    C.Fog = EffectiveFog();
    C.PressureTrend = Trend;
    C.Shock = Shock();
    C.StableHours = StableHours();
    const FVector Dir = FRotator(0, W.WindYaw, 0).Vector();
    C.WindDir = FVector2D(Dir.X, Dir.Y);
    return C;
}

FVector ALakeWeather::WindVelocity() const
{
    const float T = ServerNow();
    const float Gust = 1 + 0.3f * FMath::Sin(T * 0.37f) * FMath::Sin(T * 1.13f + 1.0f);
    return FRotator(0, Current().WindYaw, 0).Vector() * WindSpeed() * 100 * Gust;
}

void ALakeWeather::SetWeatherNow(const FLakeWeatherState& State)
{
    From = To = State;
    BlendStart = ServerNow() - 1;
    BlendSeconds = 0.01f;
    NextChange = ServerNow() + Random.FRandRange(240, 480);
    ForceNetUpdate();
}

void ALakeWeather::SetWeather(const FLakeWeatherState& State, float Seconds, float Delay)
{
    From = Current();
    To = State;
    BlendStart = ServerNow() + FMath::Max(0.0f, Delay);
    BlendSeconds = FMath::Max(0.01f, Seconds);
    NextChange = BlendStart + BlendSeconds + Random.FRandRange(240, 480);
    ForceNetUpdate();
}

void ALakeWeather::PickNextWeather()
{
    // Weighted roll: fog is likelier around dawn and at night, rain after a grey sky, and the
    // weather rarely jumps from clear straight into a downpour.
    const float H = Hours();
    const bool bFogHours = H < 8.5f || H > 21.0f;
    const float CloudNow = Current().Cloud;
    float Total = 0;
    float Weights[UE_ARRAY_COUNT(GPresets)];
    for (int32 I = 0; I < UE_ARRAY_COUNT(GPresets); ++I)
    {
        const FPreset& P = GPresets[I];
        float W = P.Weight;
        if (P.bFoggy) W *= bFogHours ? 2.5f : 0.4f;
        if (P.bRainy) W *= CloudNow > 0.7f ? 2.0f : CloudNow < 0.4f ? 0.25f : 1.0f;
        Weights[I] = W;
        Total += W;
    }
    float Roll = Random.FRandRange(0, Total);
    int32 Pick = 0;
    for (; Pick < UE_ARRAY_COUNT(GPresets) - 1 && Roll > Weights[Pick]; ++Pick) Roll -= Weights[Pick];
    const FPreset& P = GPresets[Pick];
    FLakeWeatherState Next;
    Next.Cloud = Random.FRandRange(P.Cloud.X, P.Cloud.Y);
    Next.Fog = Random.FRandRange(P.Fog.X, P.Fog.Y);
    Next.Rain = Random.FRandRange(P.Rain.X, P.Rain.Y);
    Next.Wind = Random.FRandRange(P.Wind.X, P.Wind.Y);
    Next.WindYaw = To.WindYaw + Random.FRandRange(-60, 60);
    // Rain and big changes come as a front: the pressure starts falling one to two game hours ahead.
    const FLakeWeatherState Now = Current();
    const bool bFront = P.bRainy || FMath::Abs(Next.Cloud - Now.Cloud) > 0.5f || FMath::Abs(Next.Wind - Now.Wind) > 0.4f;
    const float Delay = bFront ? Random.FRandRange(150, 300) : Random.FRandRange(10, 40);
    UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_WEATHER next %s cloud=%.2f fog=%.2f rain=%.2f wind=%.2f in %.0f s"), P.Name, Next.Cloud, Next.Fog, Next.Rain, Next.Wind, Delay);
    SetWeather(Next, Random.FRandRange(60, 150), Delay);
}

void ALakeWeather::BeginPlay()
{
    Super::BeginPlay();
    Random.Initialize(FDateTime::Now().GetTicks() & 0x7fffffff);
    if (HasAuthority())
    {
        // The game opens on the misty dawn the lake was designed around.
        ClockOriginServer = ServerNow();
        ClockOriginHours = 5.5f;
        From = To = FLakeWeatherState();
        BlendStart = ClockOriginServer;
        NextChange = ClockOriginServer + Random.FRandRange(300, 480);
    }
#if !UE_BUILD_SHIPPING
    // -WeatherPreview: a fixed set of times and weathers seen from the start spot, HUD on.
    if (HasAuthority() && FParse::Param(FCommandLine::Get(), TEXT("WeatherPreview")))
    {
        struct FShot { float Hours, Cloud, Fog, Rain, Wind; const TCHAR* File; float Turn = -25, Pitch = -4, Back = 250; };
        TArray<FShot> Shots = {
            { 5.8f,  0.3f, 0.7f, 0.0f, 0.05f, TEXT("Weather_Dawn.png") },
            { 12.5f, 0.15f, 0.0f, 0.0f, 0.3f, TEXT("Weather_Noon.png") },
            { 15.0f, 0.95f, 0.15f, 0.0f, 0.75f, TEXT("Weather_OvercastWind.png") },
            { 14.0f, 0.9f, 0.25f, 0.25f, 0.15f, TEXT("Weather_Drizzle.png") },
            { 16.0f, 1.0f, 0.3f, 1.0f, 0.85f, TEXT("Weather_Downpour.png") },
            { 20.6f, 0.25f, 0.05f, 0.0f, 0.1f, TEXT("Weather_Sunset.png") },
            { 23.5f, 0.1f, 0.1f, 0.0f, 0.05f, TEXT("Weather_Night.png") },
            { 1.5f,  0.5f, 0.9f, 0.0f, 0.0f, TEXT("Weather_NightFog.png") },
        };
        if (FParse::Param(FCommandLine::Get(), TEXT("GrassLightPreview")))
            Shots = {
                { 12.5f, 0.15f, 0, 0, 0.1f, TEXT("Grass_NoonAcross.png"), -60, -25, 1000 },
                { 12.5f, 0.15f, 0, 0, 0.1f, TEXT("Grass_NoonAway.png"), 120, -25, 1000 },
                { 18.0f, 0.15f, 0, 0, 0.1f, TEXT("Grass_LowSun.png"), 0, -25, 1000 },
                { 15.0f, 0.95f, 0.15f, 0, 0.1f, TEXT("Grass_Overcast.png"), -60, -25, 1000 },
            };
        for (int32 I = 0; I < Shots.Num(); ++I)
        {
            const FShot Shot = Shots[I];
            const float At = 18.0f + I * 9.0f;
            FTimerHandle Pose, Snap;
            GetWorldTimerManager().SetTimer(Pose, [this, Shot]
            {
                FLakeWeatherState W;
                W.Cloud = Shot.Cloud; W.Fog = Shot.Fog; W.Rain = Shot.Rain; W.Wind = Shot.Wind; W.WindYaw = 60;
                From = To = W;
                BlendStart = ServerNow();
                NextChange = BlendStart + 1e6f;
                SetHours(Shot.Hours);
                Wetness = Shot.Rain > 0 ? 0.8f : 0.0f;
                if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
                    if (APawn* Pawn = PC->GetPawn())
                    {
                        FVector2D Facing;
                        const FVector2D Spot = LakeMap::StartSpot(&Facing) - Facing * Shot.Back;
                        Pawn->SetActorLocation(FVector(Spot, LakeMap::Ground(Spot.X, Spot.Y) + 100));
                        PC->SetControlRotation(FRotator(Shot.Pitch, FMath::RadiansToDegrees(FMath::Atan2(Facing.Y, Facing.X)) + Shot.Turn, 0));
                    }
            }, At - 7.0f, false);
            GetWorldTimerManager().SetTimer(Snap, [this, Shot]
            {
                FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / Shot.File, true, false);
                const FFishConditions C = FishConditions();
                FString Bite;
                for (int32 Sp = 0; Sp < LakeFish::NumSpecies(); ++Sp) Bite += FString::Printf(TEXT(" %s %.2f"), *LakeFish::Info((EFishSpecies)Sp).Id, LakeFish::Activity((EFishSpecies)Sp, C));
                UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_WEATHER_SHOT %s %s %.0f C | bite:%s"), Shot.File, *Describe(), TemperatureC(), *Bite);
            }, At, false);
        }
        FTimerHandle Exit;
        GetWorldTimerManager().SetTimer(Exit, [] { FPlatformMisc::RequestExit(false); }, 18.0f + Shots.Num() * 9.0f + 2, false);
    }
#endif
}

void ALakeWeather::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (HasAuthority() && ServerNow() >= NextChange) PickNextWeather();
    if (HasAuthority())
    {
        // Pressure heads for the level of the weather that is coming (so it falls before the rain),
        // at most 2.5 mm per game hour; the water warms and cools a few hours behind the air.
        const float Hours = DeltaSeconds * GameHoursPerSecond();
        const float Target = 762.0f - 9.0f * To.Cloud - 13.0f * To.Rain - 5.0f * To.Wind + 3.0f * To.Fog * (1 - To.Rain);
        const float Rate = FMath::Clamp((Target - Pressure) / 2.5f, -2.5f, 2.5f);
        Pressure += Rate * Hours;
        Trend = FMath::FInterpTo(Trend, Rate, DeltaSeconds, 0.5f);
        WaterTemp += (0.35f * TemperatureC() + 0.65f * 23.0f - WaterTemp) * Hours / 5.0f;   // a warm, shallow summer pond: about +20..+22
    }
    // Diagnostic for the network test: every machine reports the same clock and weather.
    if (!bLoggedClock && GetWorld()->GetTimeSeconds() > 7)
    {
        bLoggedClock = true;
        UE_LOG(LogTemp, Display, TEXT("QUIET_LAKE_CLOCK %.2f h, %s, %.0f C, water %.1f C, %.0f mm (netmode %d)"), Hours(), *Describe(), TemperatureC(), WaterTemp, Pressure, (int32)GetNetMode());
    }
    if (GetNetMode() != NM_DedicatedServer) ApplyToWorld(DeltaSeconds);
}

void ALakeWeather::ApplyToWorld(float DeltaSeconds)
{
    UWorld* World = GetWorld();
    if (!bBound)
    {
        // The environment spawns the sky on every machine; take it over once it exists.
        for (TActorIterator<ADirectionalLight> It(World); It && !Sun; ++It) Sun = Cast<UDirectionalLightComponent>(It->GetLightComponent());
        if (!Sun) return;
        for (TActorIterator<ASkyAtmosphere> It(World); It; ++It) Atmosphere = It->GetComponent();
        for (TActorIterator<ASkyLight> It(World); It; ++It) SkyLight = It->GetLightComponent();
        for (TActorIterator<AExponentialHeightFog> It(World); It; ++It) Fog = It->GetComponent();
        for (TActorIterator<AVolumetricCloud> It(World); It; ++It) Clouds = It->FindComponentByClass<UVolumetricCloudComponent>();
        for (TActorIterator<APostProcessVolume> It(World); It; ++It) Post = *It;
        for (TActorIterator<ALakeEnvironment> It(World); It; ++It)
        {
            TInlineComponentArray<UAudioComponent*> Audio(*It);
            for (UAudioComponent* A : Audio)
            {
                const FString Name = A->Sound ? A->Sound->GetName() : FString();
                if (Name == TEXT("A_Wind")) WindSound = A;
                if (Name == TEXT("A_Birds")) BirdSound = A;
            }
        }
        // Moonlight: a second, faint, cool directional light that also lights the atmosphere.
        auto* MoonActor = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 3000), FRotator(-30, 0, 0));
        MoonActor->SetMobility(EComponentMobility::Movable);
        Moon = Cast<UDirectionalLightComponent>(MoonActor->GetLightComponent());
        Moon->SetAtmosphereSunLight(true);
        Moon->SetAtmosphereSunLightIndex(1);
        Moon->SetLightColor(FLinearColor(0.62f, 0.72f, 1.0f));
        Moon->SetLightSourceAngle(0.5f);
        Moon->SetCastShadows(true);
        if (Clouds)
        {
            if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst.m_SimpleVolumetricCloud_Inst")))
            {
                CloudMaterial = UMaterialInstanceDynamic::Create(Base, this);
                Clouds->SetMaterial(CloudMaterial);
            }
        }
        WeatherParameters = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/Weather/MPC_Weather.MPC_Weather"));
        RainTiles = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
        RainTiles->SetupAttachment(RootComponent);
        RainTiles->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Game/Weather/SM_RainTile.SM_RainTile")));
        RainTiles->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        RainTiles->SetCastShadow(false);
        RainTiles->SetUsingAbsoluteLocation(true);
        RainTiles->RegisterComponent();
        for (int32 I = 0; I < 9; ++I) RainTiles->AddInstance(FTransform::Identity, true);
        auto Sound = [this](const TCHAR* Name)
        {
            auto* A = NewObject<UAudioComponent>(this);
            A->SetupAttachment(RootComponent);
            A->SetSound(LoadObject<USoundWave>(nullptr, *FString::Printf(TEXT("/Game/Audio/%s.%s"), Name, Name)));
            A->SetVolumeMultiplier(0.001f);
            A->bAutoActivate = false;
            A->RegisterComponent();
            A->Play();
            return A;
        };
        RainLightSound = Sound(TEXT("A_RainLight"));
        RainHeavySound = Sound(TEXT("A_RainHeavy"));
        CricketSound = Sound(TEXT("A_Crickets"));
        // Night needs a darker floor for the auto exposure than the engine default.
        if (Post)
        {
            Post->Settings.bOverride_AutoExposureMinBrightness = true;
            Post->Settings.bOverride_AutoExposureMaxBrightness = true;
            Post->Settings.AutoExposureMaxBrightness = 16.0f;
        }
        bBound = true;
    }

    const FLakeWeatherState W = Current();
    const float FogNow = EffectiveFog();
    float Elevation;
    const FVector ToSun = SunDirection(&Elevation);
    const float Day = S(-6.0f, 4.0f, Elevation);
    const float Night = 1 - S(-12.0f, -2.0f, Elevation);
    const float Overcast = FMath::Max(W.Cloud, W.Rain);

    // Changes are applied in small steps only: every update of a light, the sky or the fog throws
    // away cached shadow pages and sky captures, which is expensive. The sun moves 0.125 degrees a
    // second, so a step of 0.15 degrees is invisible.
    auto Changed = [](float& Last, float Now, float Step) { if (FMath::Abs(Now - Last) < Step) return false; Last = Now; return true; };
    // Sun: rises in the north-east, low and warm at the ends of the day, softened by cloud.
    if (FVector::DotProduct(ToSun, LastSunDir) < FMath::Cos(FMath::DegreesToRadians(0.15f)))
    {
        LastSunDir = ToSun;
        Sun->SetWorldRotation((-ToSun).Rotation());
        // Moon: roughly opposite the sun, always a little above the horizon at night.
        const FVector ToMoon(-ToSun.X, -ToSun.Y, FMath::Max(0.3f, -ToSun.Z * 0.8f + 0.2f));
        Moon->SetWorldRotation((-ToMoon.GetSafeNormal()).Rotation());
    }
    // 6.5 lux: the sky is lit up to match in LakeEnvironment (SkyLuminanceFactor), so only the direct light is softer.
    if (Changed(LastSun, 6.5f * S(-3.0f, 6.0f, Elevation) * FMath::Lerp(1.0f, 0.12f, Overcast * Overcast), 0.05f)) Sun->SetIntensity(LastSun);
    // The atmosphere already reddens a low sun (the light crosses more air); the colour
    // temperature only adds a little warmth on top, or a misty dawn turns blood red.
    if (Changed(LastSunTemp, FMath::Lerp(4800.0f, 5800.0f, S(0.0f, 20.0f, Elevation)), 25.0f)) Sun->SetTemperature(LastSunTemp);
    if (Changed(LastSunAngle, FMath::Lerp(0.8f, 6.0f, Overcast), 0.1f)) Sun->SetLightSourceAngle(LastSunAngle);
    if (Changed(LastMoon, 0.4f * Night * FMath::Lerp(1.0f, 0.15f, Overcast), 0.005f))
    {
        Moon->SetIntensity(LastMoon);
        Moon->SetVisibility(LastMoon > 0.003f);
    }
    // Under a thick cloud the whole sky is dimmer, more so in rain.
    if (SkyLight && Changed(LastSky, FMath::Lerp(1.0f, 0.8f, W.Cloud * W.Cloud) * FMath::Lerp(1.0f, 0.6f, W.Rain), 0.01f)) SkyLight->SetIntensity(LastSky);
    if (Atmosphere)
    {
        // A grey sky under cloud and rain: less blue scattering, more milky haze.
        if (Changed(LastMie, 0.004f + 0.003f * FogNow + 0.02f * W.Rain + 0.006f * W.Cloud, 0.0002f)) Atmosphere->SetMieScatteringScale(LastMie);
        if (Changed(LastRayleigh, FMath::Lerp(0.0331f, 0.012f, Overcast * 0.85f), 0.0003f)) Atmosphere->SetRayleighScatteringScale(LastRayleigh);
    }
    if (Post)
    {
        // By day the eye adapts only part of the way: a grey or rainy day must look darker than a
        // sunny one, not be brightened back to it. Measured with -ExposureProbe, free adaptation
        // settles at EV100 -0.1 at a clear noon, -3.3 against a low sun, and drops with the
        // sunlight the cloud lets through (-2.5 overcast, -3.35 in a downpour) while fog raises it.
        // The exposure is held in a narrow band that follows only half of the loss of sunlight,
        // and a little darker overall than before. At night adaptation stops early, which keeps
        // the night dark but readable.
        const float SunShare = FMath::Lerp(1.0f, 0.12f, Overcast * Overcast);
        const float ClearEV = FMath::Lerp(-3.3f, -0.1f, S(0.0f, 35.0f, Elevation));
        const float DayEV = ClearEV + 0.3f + 0.5f * FMath::Log2(SunShare) + 1.3f * FogNow;
        Post->Settings.AutoExposureMinBrightness = FMath::Lerp(-3.2f, DayEV - 0.25f, Day);
        Post->Settings.AutoExposureMaxBrightness = FMath::Lerp(1.0f, DayEV + 0.25f, Day);
        Post->Settings.AutoExposureBias = FMath::Lerp(-0.4f, 0.0f, Day);
#if !UE_BUILD_SHIPPING
        // -ExposureProbe: free adaptation and the HDR debug view, to read what the eye would choose.
        if (FParse::Param(FCommandLine::Get(), TEXT("ExposureProbe")))
        {
            Post->Settings.AutoExposureMinBrightness = -6;
            Post->Settings.AutoExposureMaxBrightness = 16;
            static bool bShown = false;
            if (!bShown && GetWorld()->GetFirstPlayerController()) { bShown = true; GetWorld()->GetFirstPlayerController()->ConsoleCommand(TEXT("ShowFlag.VisualizeHDR 1")); }
        }
#endif
    }

    // Fog: a thin haze over the valley and a dense layer on the water; mist on calm mornings.
    if (Fog && Changed(LastFog, FogNow + 3.0f * W.Rain, 0.004f))
    {
        Fog->SetFogDensity(0.01f + 0.045f * FogNow + 0.02f * W.Rain);
        FExponentialHeightFogData Water = Fog->SecondFogData;
        Water.FogDensity = 0.02f + 0.34f * FogNow;
        Water.FogHeightFalloff = 2.0f;
        Fog->SetSecondFogData(Water);
        Fog->SetVolumetricFogExtinctionScale(0.5f + 0.8f * FogNow + 0.4f * W.Rain);
    }
    // A misty dawn is milky white, not orange: a low sun lights the fog only weakly (its light is
    // reddened by the long way through the air), the cool light of the sky lights most of it.
    // Only around dawn and dusk: at night the sky's light stays as it was, or the fog glows.
    const float Twilight = S(-6.0f, 2.0f, Elevation) * (1 - S(2.0f, 20.0f, Elevation));
    if (Changed(LastSunInFog, FMath::Lerp(0.3f, 1.0f, S(2.0f, 20.0f, Elevation)) + 2 * Twilight, 0.02f))
    {
        Sun->SetVolumetricScatteringIntensity(FMath::Lerp(0.3f, 1.0f, S(2.0f, 20.0f, Elevation)));
        if (SkyLight) SkyLight->SetVolumetricScatteringIntensity(1.0f + 0.6f * Twilight);
    }
    // The haze colour and the lift of the sky (LakeEnvironment dims the sun and raises the sky to
    // match) are daytime light: the haze's own glow fades out at night, the night sky is not raised.
    if (Changed(LastDayTint, Day, 0.01f))
    {
        if (Fog) Fog->SetFogInscatteringColor(FLinearColor(0.42f, 0.47f, 0.55f) * FMath::Lerp(0.015f, 1.0f, Day * Day));
        if (Atmosphere)
        {
            const float Lift = FMath::Lerp(1.0f, 10.0f / 6.5f, Day);
            Atmosphere->SetSkyLuminanceFactor(FLinearColor(Lift, Lift, Lift));
        }
    }
    if (CloudMaterial && Changed(LastCloud, W.Cloud + 2.0f * W.Rain, 0.004f))
    {
        CloudMaterial->SetScalarParameterValue(TEXT("Cloud_GlobalCoverage"), FMath::Lerp(-0.7f, 1.2f, W.Cloud));
        CloudMaterial->SetScalarParameterValue(TEXT("Cloud_GlobalDensity"), FMath::Lerp(0.004f, 0.035f, W.Cloud));
        CloudMaterial->SetScalarParameterValue(TEXT("StormClouds"), S(0.3f, 1.0f, W.Rain));
    }

    // Wind for the Megaplants trees and reeds, the materials and the rain.
    const FVector WindDir = FRotator(0, W.WindYaw, 0).Vector();
    // The plugin exports neither the subsystem class nor a usable header, so its Blueprint
    // function UpdateWindParameters is called through reflection.
    static UClass* DynamicWindClass = FindObject<UClass>(nullptr, TEXT("/Script/DynamicWind.DynamicWindSubsystem"));
    if (UObject* DynamicWind = DynamicWindClass ? World->GetSubsystemBase(DynamicWindClass) : nullptr)
        if (UFunction* Update = DynamicWind->FindFunction(TEXT("UpdateWindParameters")))
        {
            FDynamicWindParameters P;
            P.WindDirection = WindDir;
            P.WindSpeed = 6.0f + 40.0f * W.Wind;
            P.WindAmplitude = 0.1f + 2.4f * W.Wind;
            DynamicWind->ProcessEvent(Update, &P);
        }
    // The ground gets wet in the rain and dries slowly afterwards.
    Wetness = FMath::Clamp(Wetness + DeltaSeconds * (W.Rain > 0.05f ? 0.02f * W.Rain : -0.004f * (1 - 0.6f * W.Cloud)), 0.0f, 1.0f);
    if (WeatherParameters)
        if (UMaterialParameterCollectionInstance* MPC = World->GetParameterCollectionInstance(WeatherParameters))
        {
            MPC->SetScalarParameterValue(TEXT("Rain"), W.Rain);
            MPC->SetScalarParameterValue(TEXT("Wind"), W.Wind);
            MPC->SetScalarParameterValue(TEXT("WindDirX"), WindDir.X);
            MPC->SetScalarParameterValue(TEXT("WindDirY"), WindDir.Y);
            MPC->SetScalarParameterValue(TEXT("Wetness"), Wetness);
            MPC->SetScalarParameterValue(TEXT("Daylight"), FMath::Max(Day, 0.05f));
        }

    // Rain: nine identical 12 m tiles on a world grid around the camera, so the streaks stay put
    // in the world while the angler walks.
    FVector Camera = FVector::ZeroVector;
    if (APlayerController* PC = World->GetFirstPlayerController())
        if (PC->PlayerCameraManager) Camera = PC->PlayerCameraManager->GetCameraLocation();
    RainTiles->SetVisibility(W.Rain > 0.01f);
    if (W.Rain > 0.01f)
    {
        constexpr float Tile = 1200.0f;
        const float BaseX = FMath::FloorToFloat(Camera.X / Tile) * Tile, BaseY = FMath::FloorToFloat(Camera.Y / Tile) * Tile;
        for (int32 I = 0; I < 9; ++I)
            RainTiles->UpdateInstanceTransform(I, FTransform(FVector(BaseX + (I % 3 - 1) * Tile, BaseY + (I / 3 - 1) * Tile, Camera.Z - 400)), true, I == 8, true);
    }

    // Ambience: rain, wind, birds by day and crickets on warm dry nights.
    // Rain: single drops in a drizzle, giving way to the dense hiss of a downpour.
    if (RainLightSound) RainLightSound->SetVolumeMultiplier(FMath::Max(0.001f, 0.7f * S(0.0f, 0.25f, W.Rain) * (1 - 0.6f * S(0.4f, 1.0f, W.Rain))));
    if (RainHeavySound) RainHeavySound->SetVolumeMultiplier(FMath::Max(0.001f, 0.85f * S(0.2f, 1.0f, W.Rain)));
    if (WindSound) WindSound->SetVolumeMultiplier(0.12f + 0.75f * W.Wind);
    if (BirdSound) BirdSound->SetVolumeMultiplier(FMath::Max(0.001f, 0.3f * Day * (1 - 0.85f * W.Rain)));
    if (CricketSound) CricketSound->SetVolumeMultiplier(FMath::Max(0.001f, 0.45f * Night * (1 - W.Rain) * S(10.0f, 15.0f, TemperatureC())));
}
