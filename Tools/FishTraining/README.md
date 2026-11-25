# Обучение рыбьих политик

Маленькие нейронные сети, которые решают за рыбу в прототипе экосистемы (`-FishEcosystemPrototype`). Здесь они обучаются на синтетических эпизодах и выгружаются в C++ как `constexpr`-таблицы.

## Как это устроено

- Среда — `pike_env.py`: эпизод из нескольких шагов, дистанции в метрах, запасы (энергия, голод) в 0..1. Действия: `wait`, `approach`, `strike`, `retreat`; наблюдения: дистанция, укрытие, угроза, энергия, голод.
- Награда — игровая функция полезности: пойманная добыча против потраченной энергии, риска и промаха. Обучение — эпизодические policy gradients на стандартном Python (`math`, `random`), без torch/numpy и без GPU.
- Модель — два слоя весов (hidden 5–12 нейронов, выход 3–13 действий). Скрипт печатает оценку на удержанных примерах и **перезаписывает** соответствующий `Source/QuietLake/*Weights.h`: веса в игре не читаются из файлов, а компилируются в код.

## Скрипты

| Скрипт | Веса (что генерирует) | Что решает |
|---|---|---|
| `train_species.py` | `FishDecisionWeights.h` | «плыть / кормиться / отдыхать» для щуки, окуня, плотвы и карася |
| `train_pike.py` + `export_pike.py` | `pike_policy.json`, `PikeWeights.h` | сближение и бросок щуки (общая политика) |
| `train_pike_ambush.py` | `PikeAmbushWeights.h` | засадный бросок щуки |
| `train_perch.py` | `PerchHuntWeights.h` | охота окуня на малька рядом с хищником |
| `train_roach.py` | `RoachShoalWeights.h` | поведение плотвы в стае |
| `train_crucian.py` | `CrucianPatchWeights.h` | выбор пятна корма карасём |
| `train_threat.py` | `ThreatPolicyWeights.h` | реакция на угрозу (базовая) |
| `train_crucian_threat.py` | `CrucianThreatWeights.h`, `CrucianBedThreatWeights.h` | реакция карася на угрозу (в толще и у дна) |
| `train_fishing.py` | `FishingPolicyWeights.h` | выбор наживки и подхода к снасти |
| `train_inspection.py` | `InspectionWeights.h` | осмотр наживки |
| `train_ground_patch.py` | `GroundPatchWeights.h` | выбор донного пятна корма |
| `train_patch_morsel.py` | `PatchMorselWeights.h` | выбор куска в пятне корма |

`test_pike.py` — быстрые проверки среды, `pike_policy.json` — веса щуки в JSON (из него стартуют `train_perch.py` и `train_pike_ambush.py`).

## Запуск

```powershell
python Tools/FishTraining/train_species.py
python Tools/FishTraining/train_perch.py
```

Скрипты пишут веса прямо в `Source/QuietLake/`, поэтому после обучения пересоберите `QuietLakeEditor` (`Tools/Build.ps1`) и прогоните автотесты дважды. Веса — сгенерированный код: не правьте `.h` руками, меняйте обучение.

## Ограничения

- Сети крошечные и обучаются на упрощённой модели мира; проверка говорит о совпадении с ручной функцией полезности, а не о полевой точности против настоящих рыб.
- Контракт наблюдений и действий зашит и в C++, и в Python: меняя признаки, меняйте обе стороны и заново обучайте все зависимые политики.
- План развития (популяция, LOD, сохранения, обучение внутри UE) — `docs/FishEcosystem_Architecture.md`.
