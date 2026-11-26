# Реструктуризация документации QuietLake — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use subagent-driven-development (recommended) or executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** README снова становится коротким руководством, полная история проверок сохраняется дословно в `docs/CHECKS.md`, WORLD — единственный источник истины по миру (с новым разделом про обученные политики), CLAUDE — операционная памятка.

**Architecture:** Документы разделяются по назначению: руководство (README), мир и механики (WORLD), история проверок (docs/CHECKS.md), операции (CLAUDE), обучение рыб (Tools/FishTraining/README.md). Перенос истории — скриптом с проверкой контрольных сумм, без ручного пересказа.

**Tech Stack:** Markdown (русский), Python 3 из bundled-рантайма для разового скрипта переноса и проверки ссылок.

**Spec:** [docs/superpowers/specs/2026-11-25-docs-structure-design.md](../specs/2026-11-25-docs-structure-design.md)

## Global Constraints

- Не менять `Content/`, `Source/`, `Tools/*.py`, `Config/`, `QuietLake.uproject` — только документы и разовый скрипт в `Saved/`.
- Все 127 разделов «Проверено» переносятся дословно; сверка по SHA-256 каждого блока.
- Язык документов — русский; комментарии в коде — английский.
- Коммит — только по явной просьбе пользователя (правило CLAUDE.md): шаги коммита выполнять лишь при такой просьбе.
- README после правки — не длиннее ~150 строк; `docs/CHECKS.md` содержит все 127 разделов.
- Правило пополнения: новые проверки пишутся в `docs/CHECKS.md`, в README попадают только три-четыре последние.

## Review Focus

- Ссылка на несуществующий раздел WORLD или битый якорь в оглавлении CHECKS — читатель попадает в пустоту.
- Потеря уникального факта при сокращении «Что реализовано»: каждый факт должен оказаться в WORLD или в новом README.
- Расхождение истории: если правку проверки внести в README вместо CHECKS, копии разойдутся.
- Кодировка и переносы строк при скриптовой записи (CRLF/LF) не должны менять содержимое блоков.
- Раздел в оглавлении без даты/темы — по нему нельзя найти проверку.

---

### Task 1: Перенос журнала проверок в `docs/CHECKS.md`

**Files:**
- Create: `docs/CHECKS.md`
- Create (временный, не коммитить): `Saved/extract_checks.py`, `Saved/checks_manifest.json`
- Read: `README.md`

**Interfaces:**
- Produces: `docs/CHECKS.md` с заголовком, оглавлением по месяцам и 127 разделами; `Saved/checks_manifest.json` — массив `{date, title, sha256, bytes}` для сверки в задачах 2 и 6.

- [ ] **Step 1: Написать `Saved/extract_checks.py`**

Скрипт читает `README.md`, режет по строкам, начинающимся с `## `; блок «Проверено» — от заголовка до следующего `## ` (или конца файла). Пишет `docs/CHECKS.md`: шапка (назначение, правило пополнения), оглавление (группы по месяцу, строка `- [ДД.ММ.ГГГГ — тема](#якорь)`), затем блоки дословно в исходном порядке. Якорь — GitHub-slug: нижний регистр, пробелы в `-`, удалить `«».,:;!?()—`; коллизии — суффикс `-1`, `-2`. Скрипт печатает и сохраняет манифест; утверждает, что суммарный SHA-256 склеенных блоков равен SHA-256 склеенных блоков исходного README. Режим `--verify-readme` сверяет четыре последних блока в новом README с манифестом и печатает `latest in readme: N, identical: M`.

- [ ] **Step 2: Запустить и проверить полноту**

Run: `python Saved/extract_checks.py`
Expected: `checks: 127, bytes: <N>, sha256: <hash>`, файл создан, утверждение прошло.

- [ ] **Step 3: Проверить оглавление**

Run: `python -c "..."` (проверка, что число строк оглавления равно 127 и каждый якорь присутствует в тексте CHECKS).
Expected: `toc entries: 127, missing anchors: 0`.

- [ ] **Step 4: Коммит (только если пользователь попросил)**

```bash
git add docs/CHECKS.md
git commit -m "Документация: журнал проверок вынесен в docs/CHECKS.md"
```

### Task 2: README — короткое руководство

**Files:**
- Modify: `README.md` (полная перезапись верхней части, сохранение четырёх последних проверок)
- Read: `Saved/checks_manifest.json`, `WORLD.md`

**Interfaces:**
- Consumes: `docs/CHECKS.md` из Task 1.
- Produces: README с разделами «Что это», «Запуск», «Управление», «Сборка и проверки», «Что реализовано» (кратко, со ссылками в WORLD), «Последние проверки» (4 раздела от 16 ноября, дословно), «Документация» (карта), «План разработки».

- [ ] **Step 1: Собрать новый README**

Верхняя часть — по образцу из дизайна: заголовок, 3–4 предложения о проекте, «Запуск» (Play.cmd, OpenEditor.cmd, сеть), «Управление» (компактный список), «Сборка и проверки» (`Tools/Build.ps1`, автотесты, `Tools/PreviewScene.ps1`, `Tools/TestNetwork.ps1`), «Что реализовано» (8–10 пунктов, каждый — 1–2 строки + ссылка на раздел WORLD), «Последние проверки» (четыре раздела от 16 ноября из манифеста, дословные копии), «Документация» (README/WORLD/docs/CHECKS/docs/FishEcosystem_Architecture/CLAUDE/Tools/FishTraining README), «План разработки» (без изменений).

- [ ] **Step 2: Сверить последние проверки**

Run: `python Saved/extract_checks.py --verify-readme`
Expected: `latest in readme: 4, identical: 4`.

- [ ] **Step 3: Сверить факты «Что реализовано»**

Читать старые пункты (из `git show HEAD:README.md`, строки 11–24) и новый README; для каждого факта указать, где он теперь: в WORLD (ссылкой) или в новом README. Всё, чего нет, добавить в WORLD (Task 3) — список расхождений передать туда.

- [ ] **Step 4: Проверить размер**

Run: `(Get-Content README.md | Measure-Object -Line).Lines`
Expected: ≤ 150 непустых строк (сейчас 896).

- [ ] **Step 5: Коммит (только если пользователь попросил)**

```bash
git add README.md
git commit -m "Документация: README — руководство, история проверок в docs/CHECKS.md"
```

### Task 3: WORLD — экосистема и перенос уникальных фактов

**Files:**
- Modify: `WORLD.md`
- Read: `docs/FishEcosystem_Architecture.md`, `Source/QuietLake/FishDecisionWeights.h`, `Tools/FishTraining/*.py`

**Interfaces:**
- Consumes: список расхождений из Task 2 Step 3.
- Produces: раздел «Обученные политики экосистемы» после `### «Live Fish»`; ссылки на `docs/FishEcosystem_Architecture.md` и `Tools/FishTraining/README.md`.

- [ ] **Step 1: Написать раздел**

Подраздел `### Обученные политики экосистемы (прототип, ноябрь 2026)`: четыре вида прототипа (щука, окунь, плотва, карась), решения «плыть/кормиться/отдыхать» и отдельные политики (охота, засада, угроза, осмотр наживки, пятно корма), формат сети 12×7→3×13, 13 наборов весов в `Source/QuietLake/*Weights.h`, генерация скриптами `Tools/FishTraining/train_*.py`, статус проверки (согласие с ручной функцией полезности; полевая точность не измерена), запуск `-FishEcosystemPrototype`, ссылки на дизайн и README обучения.

- [ ] **Step 2: Добавить недостающие факты**

Из списка расхождений Task 2: каждый факт — коротким предложением в подходящий раздел WORLD (Окружение/Рыбы/Рыбак и снасть/Управление), без дублирования уже написанного.

- [ ] **Step 3: Проверить ссылки**

Run: проверка, что пути `docs/FishEcosystem_Architecture.md` и `Tools/FishTraining/README.md` существуют.
Expected: `links ok: 2`.

### Task 4: CLAUDE.md — только операции

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Обновить списки и добавить карту документации**

Заменить перечень тестов на актуальный: 48 автотестов в четырёх файлах (`LakeFishingTest.cpp`, `LakeSwimmerTest.cpp`, `LakeHeavyLandingTest.cpp`, `LakeFishLandingTest.cpp`), перечислить имена по файлам. Добавить `Tools/FishTraining` (обучение и экспорт весов) и `docs/FishEcosystem_Architecture.md`. Добавить блок «Документация»: README — руководство, WORLD — мир и механики, `docs/CHECKS.md` — история проверок (пополнять там), `Tools/FishTraining/README.md`. Убрать из CLAUDE пересказ механик, если он дублирует WORLD (оставить ссылку).

- [ ] **Step 2: Сверить число тестов с кодом**

Run: `Select-String -Path Source\QuietLake\*Test*.cpp -Pattern 'IMPLEMENT_SIMPLE_AUTOMATION_TEST|IMPLEMENT_AUTOMATION_TEST' | Measure-Object`
Expected: 48; имена в CLAUDE совпадают с файлами.

### Task 5: `Tools/FishTraining/README.md`

**Files:**
- Create: `Tools/FishTraining/README.md`
- Read: `Tools/FishTraining/*.py`, `docs/FishEcosystem_Architecture.md`

- [ ] **Step 1: Написать README обучения**

Назначение; как устроена среда (`pike_env.py`, синтетические эпизоды, награда = игровая полезность); скрипты `train_*.py` и что каждый генерирует (какой `*Weights.h`); запуск (`python Tools/FishTraining/train_perch.py`, без внешних зависимостей); где лежат веса и что их нельзя править руками; ограничения (крошечные сети, проверка на удержанных примерах против функции полезности, полевой точности нет).

- [ ] **Step 2: Проверить, что имена файлов совпадают**

Run: `Get-ChildItem Tools\FishTraining -Filter *.py | Select-Object -ExpandProperty Name`
Expected: все упомянутые скрипты существуют.

### Task 6: Финальная проверка документации

**Files:**
- Read: все документы
- Create (временный, не коммитить): `Saved/check_docs.py`

- [ ] **Step 1: Проверить ссылки и якоря**

Скрипт разбирает относительные ссылки в README, WORLD, CLAUDE, `docs/CHECKS.md`, `Tools/FishTraining/README.md`; проверяет существование путей и наличие якорей (по алгоритму из Task 1). Run: `python Saved/check_docs.py`
Expected: `broken links: 0, broken anchors: 0`.

- [ ] **Step 2: Проверить границы изменений**

Run: `git status --short`
Expected: только `README.md`, `WORLD.md`, `CLAUDE.md`, `docs/CHECKS.md`, `Tools/FishTraining/README.md`, `docs/superpowers/specs/…`, `docs/superpowers/plans/…`; ни одного файла в `Content/`, `Source/`, `Tools/*.py`, `Config/`.

- [ ] **Step 3: Проверить итоговые счётчики**

`docs/CHECKS.md` — 127 разделов; README — 4 последние проверки, ≤ 150 непустых строк; в README нет проверок старше 16 ноября.

- [ ] **Step 4: Коммит (только если пользователь попросил)**

```bash
git add README.md WORLD.md CLAUDE.md docs/CHECKS.md Tools/FishTraining/README.md docs/superpowers
git commit -m "Документация: README-руководство, журнал проверок, раздел политик, карта документов"
```
