#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LakeFish.h"
#include "LakeWeather.generated.h"

/** One weather condition; every value 0..1 except the wind direction. */
USTRUCT()
struct FLakeWeatherState
{
    GENERATED_BODY()
    UPROPERTY() float Cloud = 0.3f;    // clear .. overcast
    UPROPERTY() float Fog = 0.6f;      // none .. dense
    UPROPERTY() float Rain = 0.0f;     // none .. downpour
    UPROPERTY() float Wind = 0.05f;    // calm .. strong (12 m/s)
    UPROPERTY() float WindYaw = 20.0f; // direction the wind blows towards, degrees

    static FLakeWeatherState Blend(const FLakeWeatherState& A, const FLakeWeatherState& B, float T);
};

/**
 * Game clock and weather. The server keeps the clock and picks the weather; both replicate as a
 * few numbers (a clock origin and a blend between two weather states on the server clock), so
 * every machine computes the same time and weather and drives its own sky from them:
 * sun and moon, sky light, height fog and morning mist, volumetric clouds, the Megaplants wind,
 * MPC_Weather for the materials (water waves and rain rings, grass sway, wet ground, rain), the
 * rain streaks around the camera, and the ambience (drizzle and downpour layers, wind, birds by day, crickets at night).
 */
UCLASS()
class QUIETLAKE_API ALakeWeather : public AActor
{
    GENERATED_BODY()
public:
    ALakeWeather();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    /** Real minutes per game day. */
    static constexpr float DayMinutes = 48.0f;
    static ALakeWeather* Get(const UWorld* World);

    /** Hours since midnight, 0..24. */
    float Hours() const;
    FLakeWeatherState Current() const;
    /** Fog including the mist that lies on the water on calm mornings and evenings. */
    float EffectiveFog() const;
    /** 0 at night, 1 in daylight (sun above the horizon). */
    float Daylight() const;
    float TemperatureC() const;
    /** Wind speed in m/s. */
    float WindSpeed() const { return Current().Wind * 12.0f; }
    /** The wind at the water (cm/s, blowing towards WindYaw) with slow gusts: what the line feels. */
    FVector WindVelocity() const;
    /** "Туман · штиль" and the like, for the HUD. */
    FString Describe() const;
    /** Direction towards the sun (world space, X east, -Y north) and its elevation in degrees. */
    FVector SunDirection(float* OutElevation = nullptr) const;

    /** Water temperature: follows the air slowly, a large pond keeps its warmth. */
    float WaterTempC() const { return WaterTemp; }
    /** Barometric pressure (mm Hg) and its change per game hour (falls ahead of rain, rises after). */
    float PressureMmHg() const { return Pressure; }
    float PressureTrend() const { return Trend; }
    /** 0..1 right after a sharp change of the weather, fading over three game hours. */
    float Shock() const;
    /** Game hours since the weather last settled. */
    float StableHours() const;
    /** Previews: the weather now has been steady for GameHours already (the water has had time to layer). */
    void SetSteadyFor(float GameHours) { BlendStart = ServerNow() - BlendSeconds - GameHours / GameHoursPerSecond(); ForceNetUpdate(); }
    /** Everything the fish react to, for LakeFish::Activity. */
    FFishConditions FishConditions() const;

    // --- server ---
    void SetHours(float NewHours);
    /** Blend to State over BlendSeconds, starting after Delay (a front: the pressure falls first). */
    void SetWeather(const FLakeWeatherState& State, float BlendSeconds, float Delay = 0);
    /** This weather at once, with no blend (previews and tests). */
    void SetWeatherNow(const FLakeWeatherState& State);

private:
    float ServerNow() const;
    void PickNextWeather();
    void ApplyToWorld(float DeltaSeconds);

    UPROPERTY(Replicated) float ClockOriginServer = 0;
    UPROPERTY(Replicated) float ClockOriginHours = 5.5f;
    UPROPERTY(Replicated) FLakeWeatherState From;
    UPROPERTY(Replicated) FLakeWeatherState To;
    UPROPERTY(Replicated) float BlendStart = -100;
    UPROPERTY(Replicated) float BlendSeconds = 1;
    UPROPERTY(Replicated) float WaterTemp = 20.5f;
    UPROPERTY(Replicated) float Pressure = 758.0f;
    UPROPERTY(Replicated) float Trend = 0.0f;
    float GameHoursPerSecond() const { return 24.0f / (DayMinutes * 60.0f); }
    float NextChange = 0;
    FRandomStream Random;

    // Per machine (not on a dedicated server).
    bool bBound = false;
    bool bLoggedClock = false;
    // Last values given to the sky, so it is only touched when something visibly changes.
    FVector LastSunDir = FVector::ZeroVector;
    /** Which directional light is the forward shading one: 1 the sun, 0 the moon (-1: not set yet). */
    int32 ForwardMain = -1;
    float LastSun = -1, LastSunTemp = -1, LastSunAngle = -1, LastMoon = -1, LastSky = -1, LastMie = -1, LastRayleigh = -1, LastFog = -1, LastCloud = -1, LastSunInFog = -1, LastDayTint = -1;
    float Wetness = 0;
    UPROPERTY() TObjectPtr<class UDirectionalLightComponent> Sun;
    UPROPERTY() TObjectPtr<class UDirectionalLightComponent> Moon;
    UPROPERTY() TObjectPtr<class UStaticMeshComponent> NightSky;
    UPROPERTY() TObjectPtr<class UMaterialInstanceDynamic> NightSkyMaterial;
    UPROPERTY() TObjectPtr<class USkyAtmosphereComponent> Atmosphere;
    UPROPERTY() TObjectPtr<class USkyLightComponent> SkyLight;
    UPROPERTY() TObjectPtr<class UExponentialHeightFogComponent> Fog;
    UPROPERTY() TObjectPtr<class UVolumetricCloudComponent> Clouds;
    UPROPERTY() TObjectPtr<class UMaterialInstanceDynamic> CloudMaterial;
    UPROPERTY() TObjectPtr<class UMaterialParameterCollection> WeatherParameters;
    UPROPERTY() TObjectPtr<class UHierarchicalInstancedStaticMeshComponent> RainTiles;
    UPROPERTY() TObjectPtr<class UAudioComponent> RainLightSound;
    UPROPERTY() TObjectPtr<class UAudioComponent> RainHeavySound;
    UPROPERTY() TObjectPtr<class UAudioComponent> CricketSound;
    UPROPERTY() TObjectPtr<class UAudioComponent> WindSound;
    UPROPERTY() TObjectPtr<class APostProcessVolume> Post;
};
