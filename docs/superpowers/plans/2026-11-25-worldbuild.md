# Единый сценарий сборки мира — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use subagent-driven-development (recommended) or executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `Tools/WorldBuild.ps1` одной командой собирает мир по шагам (сборка → уровень → вода → биомы → проверки), ведёт логи и падает при первом несоответствии.

**Architecture:** PowerShell-оркестратор запускает существующие команды (`Build.bat`, `-ExecCmds="py …"` в полном редакторе, `-run=pythonscript` в commandlet), раскладывает логи по шагам, проверяет маркеры и пороги выравнивания. Проверка выравнивания — маленький редакторный скрипт `Tools/WaterAlignment.py`.

**Tech Stack:** Windows PowerShell 5.1, UE 5.8.4 CLI (`UnrealEditor.exe`, `UnrealEditor-Cmd.exe`), Python редактора.

**Spec:** [docs/superpowers/specs/2026-11-25-worldbuild-design.md](../specs/2026-11-25-worldbuild-design.md)

## Global Constraints

- Не менять `Content/`, `Source/`, `Config/`, `QuietLake.uproject`; только `Tools/`, `docs/`.
- Редактор на время полного прогона закрыт; `-CheckOnly` тоже запускает редактор (для выравнивания), поэтому и там он должен быть закрыт.
- PowerShell 5.1: без тернарных операторов и `??`, без `-Parallel`.
- Коммит — только по просьбе пользователя.
- Логи шагов — в `Saved/Logs/WorldBuild_<yyyyMMdd-HHmmss>/`; сводка — `summary.txt`.

## Review Focus

- Шаг, который «прошёл» без маркера `done` в логе, должен валить сборку — иначе оркестратор создаёт ложную уверенность.
- Таймаут убивает процесс и не оставляет висящий `UnrealEditor.exe`.
- Уже заданные `QUIETLAKE_*` не перетираются.
- `-SkipBuild` не должен пытаться собрать C++; `-CheckOnly` — не пересобирать карту.
- Пути с пробелами (проект в `C:\Fish` их не имеет) и `-ExecCmds` кавычки не должны ломать запуск.

---

### Task 1: `Tools/WaterAlignment.py`

**Files:**
- Create: `Tools/WaterAlignment.py`

**Interfaces:**
- Produces: лог-строки `QUIET_LAKE_ALIGN …` в логе редактора; выход из редактора после выполнения.

- [ ] **Step 1: Написать скрипт**

Регистрирует `unreal.register_slate_post_tick_callback`, на 40-м кадре вызывает `unreal.LakeLevelLibrary.water_alignment_log()`, снимает колбэк и вызывает `unreal.SystemLibrary.quit_editor()`.

- [ ] **Step 2: Проверить синтаксис**

Run: `python -m py_compile Tools/WaterAlignment.py`
Expected: без вывода, код 0.

### Task 2: `Tools/WorldBuild.ps1`

**Files:**
- Create: `Tools/WorldBuild.ps1`
- Read: `Tools/BuildLakeLevel.py`, `Tools/BuildLakeBiomes.py`, `Tools/ApplyWaterMaterials.py`, `Tools/CheckVillageWater.py`, `Tools/CreateVillageWater.py`, `Tools/TuneVillageWater.py`

**Interfaces:**
- Consumes: `Tools/WaterAlignment.py` из Task 1.
- Produces: код выхода 0/1, каталог `Saved/Logs/WorldBuild_<stamp>/` с `*.log` и `summary.txt`.

- [ ] **Step 1: Написать оркестратор**

Параметры: `-SkipBuild`, `-NoWater`, `-NoBiomes`, `-RebuildWater`, `-CheckOnly`, `-Align` (по умолчанию включён, `-Align:$false` отключает), `-LogRoot`. Функции: `Assert-EditorClosed`, `Invoke-Step` (запуск процесса с таймаутом и кодом выхода), `Test-Markers` (все маркеры в логе, без строк провала), `Invoke-Build` (`Build.bat` напрямую), `Invoke-Level`, `Invoke-Water`, `Invoke-Biomes`, `Invoke-Alignment`, `Write-Summary`. Env по умолчанию выставляются, только если пусты, и печатаются.

- [ ] **Step 2: Проверить синтаксис**

Run: `powershell -NoProfile -Command "$null = [System.Management.Automation.Language.Parser]::ParseFile('C:\Fish\Tools\WorldBuild.ps1', [ref]$null, [ref]$errs); if ($errs) { $errs; exit 1 }"`
Expected: без ошибок разбора.

- [ ] **Step 3: Прогнать проверочный режим**

Run: `powershell -ExecutionPolicy Bypass -File Tools/WorldBuild.ps1 -CheckOnly`
Expected: шаги воды и выравнивания зелёные, `summary.txt` создан, код 0.

### Task 3: Документация

**Files:**
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: README — команда сборки мира**

В «Сборка и проверки» добавить `powershell -ExecutionPolicy Bypass -File Tools/WorldBuild.ps1` и флаги одной строкой.

- [ ] **Step 2: CLAUDE — операционный раздел**

В «Сборка, тесты, превью» добавить команду и правило: после правки рельефа/воды/биомов — `WorldBuild.ps1`, проверки встроены.

- [ ] **Step 3: Проверить ссылки и границы**

Run: `python Saved/check_docs.py` (если файл ещё есть) или `Select-String` по ссылкам; `git status --short` — только `Tools/`, `docs/`, `README.md`, `CLAUDE.md`.
Expected: 0 битых ссылок, никаких `Content/`/`Source/`.
