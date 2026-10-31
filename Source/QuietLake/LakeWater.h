#pragma once
#include "CoreMinimal.h"
#include "LakeFish.h"

/**
 * The water by depth ("Live Fish"): temperature and dissolved oxygen in each place, from the
 * weather, the hour, the depth and the weed. Nothing is stored: the same conditions give the same
 * water, so a preview that moves the clock sees a consistent lake.
 *  - Layers: after steady calm warm weather the top warms and a sharp drop in temperature forms at
 *    2-3 m (the thermocline); under it the water is colder and runs short of oxygen. Wind mixes it all.
 *  - Weed: by day the plants give off oxygen, at night they take it; by dawn after a warm calm night
 *    dense lilies are short of it.
 * Fish keep to water they can live in: oxygen above their species' OxygenMin, not hotter than their
 * TempHigh. Short of oxygen they leave and, worst off, gulp air at the surface.
 */
namespace LakeWater
{
    /** How layered the water is, 0 (mixed) .. 1 (a sharp thermocline): steady calm warm weather builds it, wind breaks it. */
    QUIETLAKE_API float Stratification(const FFishConditions& C);
    /** Depth (m) of the temperature drop. */
    QUIETLAKE_API float ThermoclineM(const FFishConditions& C);
    /** Water temperature (C) at a depth (m). */
    QUIETLAKE_API float TempC(const FFishConditions& C, float DepthM);
    /** How short of oxygen the weed is, 0..1: it grows through a warm calm night, the most at dawn, and is gone by mid-morning. */
    QUIETLAKE_API float WeedDeficit(const FFishConditions& C);
    /** Weed at a place, 0..1 (lily beds, the weedy backwater). */
    QUIETLAKE_API float WeedAt(float X, float Y);
    /** Dissolved oxygen (mg/l) at a place and depth (m). */
    QUIETLAKE_API float OxygenMgL(const FFishConditions& C, float X, float Y, float DepthM);
    /** How well a fish of this species can live at this place and depth, 0..1 (oxygen and heat). */
    QUIETLAKE_API float LivableAt(EFishSpecies Species, const FFishConditions& C, float X, float Y, float DepthM);
    /** The best of the water column at a place for the species, 0..1. */
    QUIETLAKE_API float Livable(EFishSpecies Species, const FFishConditions& C, float X, float Y);
    /** Z (cm) moved to the nearest depth the species can live at, or kept if none is better. */
    QUIETLAKE_API float ToLivable(EFishSpecies Species, const FFishConditions& C, float X, float Y, float Z);
    /** Short enough of oxygen to gulp air at the surface. */
    QUIETLAKE_API bool IsGasping(EFishSpecies Species, float OxygenMgL);
}
