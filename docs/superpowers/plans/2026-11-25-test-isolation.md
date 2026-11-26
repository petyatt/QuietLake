# Изоляция и разбиение тестов — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use subagent-driven-development (recommended) or executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Тесты получают детерминированные seed, файл разбит на четыре тематических, одна команда запускает все 49 тестов.

**Architecture:** Общий `Source/QuietLake/LakeTestCommon.h` (include, фикстура, `SpawnFish` с seed из параметров) + четыре `.cpp`, собранные по путям тестов. Документация переводится на `RunTests QuietLake`.

**Tech Stack:** C++ UE 5.8, Unreal automation tests, PowerShell/Python для разового переноса и проверки.

**Spec:** [docs/superpowers/specs/2026-11-25-test-isolation-design.md](../specs/2026-11-25-test-isolation-design.md)

## Global Constraints

- Игровой код не меняется: правки только в `Source/QuietLake/*Test*` и `LakeTestCommon.h`, плюс README/CLAUDE.
- Тесты и их проверки не переписываются; перенос строк дословный.
- Каждый файл обёрнут в `#if WITH_DEV_AUTOMATION_TESTS` … `#endif`.
- Коммит — только по просьбе.

## Review Focus

- Потеря теста при переносе: сверка множества путей до и после (49).
- Seed из параметров должен совпадать при повторном прогоне: две рыбы с одинаковыми параметрами получат одинаковый seed — если тест сравнивает двух одинаковых рыб, поведение совпадёт; проверить такие места.
- Дублирование `SpawnFish`/фикстуры в нескольких TU — только `inline`, иначе ODR.
- Команда `RunTests QuietLake` не должна зацепить чужие тесты движка.

---

### Task 1: `LakeTestCommon.h` и детерминированный seed

**Files:**
- Create: `Source/QuietLake/LakeTestCommon.h`
- Modify: `Source/QuietLake/LakeFishingTest.cpp` (preamble → include)

**Interfaces:**
- Produces: `FLakeFishingFixture` (как было), `inline ALakeFish* SpawnFish(UWorld*, EFishSpecies, float Weight, FVector, int32 School = -1, int32 Seed = -1)`; при `Seed < 0` seed считается из вида, веса, координаты и школы.

- [ ] **Step 1: Написать заголовок** — перенести include-ы и `FLakeFishingFixture`, `SpawnFish` сделать `inline` и детерминированным.
- [ ] **Step 2: Собрать** — `Tools/Build.ps1`; ожидается `Result: Succeeded`.
- [ ] **Step 3: Прогнать тесты** — `RunTests QuietLake.Fishing`, ожидается 35/35.

### Task 2: Разбиение на четыре файла

**Files:**
- Create: `Source/QuietLake/LakeFishTest.cpp`, `LakeWorldTest.cpp`, `LakeEcosystemTest.cpp`
- Modify: `Source/QuietLake/LakeFishingTest.cpp` (остаются 13 тестов)
- Create (разовый скрипт, не коммитить): `Saved/split_tests.py`

**Interfaces:**
- Consumes: заголовок из Task 1.
- Produces: четыре файла; суммарно 42 теста `LakeFishingTest.cpp`-класса.

- [ ] **Step 1: Скрипт переноса** — режет по `IMPLEMENT_…`, забирает комментарии перед тестом, раскладывает по карте «путь → файл», пишет файлы с одним guard-блоком.
- [ ] **Step 2: Сверить состав** — множества путей до/после равны (42), порядок внутри файлов сохранён.
- [ ] **Step 3: Собрать** — `Result: Succeeded`.
- [ ] **Step 4: Прогнать** — `RunTests QuietLake`, ожидается 49/49.

### Task 3: Документация и два прогона

**Files:**
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: Команда и числа** — `Automation RunTests QuietLake`; 49 тестов: `QuietLake.Fishing` 35 (включая HeavyLanding 2 и Landing 3), `QuietLake.Ecosystem` 12, `QuietLake.Prototype` 2; перечислить четыре+три файла.
- [ ] **Step 2: Два полных прогона** — `RunTests QuietLake` дважды, оба 49/49; отдельно `Food`, `Interactions`, `BanksAndDepth` — успешны.
- [ ] **Step 3: Границы** — `git status`: только тестовые файлы, заголовок, README/CLAUDE, docs.
