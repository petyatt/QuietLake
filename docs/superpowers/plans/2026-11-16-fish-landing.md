# Рывок и подвод к берегу — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** руки рыбака ограничены по силе, уставшую рыбу до ~1 кг выдёргивают коротким рывком, тяжелее — подводят к мели и берут рукой.

**Architecture:** предел рук уже возвращён в `ALakeCharacter::UpdateRodHands`. Добавляется запас рывка рук (`HeaveReserve`), который ограничивает `HandHeaveForce` по времени; подсказка 10 и скриптовая рука стенда переходят на подвод к мели; тесты HeavyLanding переписываются под это.

**Tech Stack:** UE 5.8 C++, автотесты `QuietLake.Fishing.*`, стенд `-FishingBench`.

**Spec:** `docs/superpowers/specs/2026-11-16-fish-landing-design.md`

## Global Constraints

- Удержание ЛКМ не поднимает нагрузку выше предела рук; никакой телепортации и скриптового подъёма.
- `HandHeaveForce` ≈ 1000 гс, полный рывок ≈ 3 с, восстановление ≈ 6 с (подбирается по стенду).
- Тексты подсказок по-русски, комментарии в коде по-английски.
- Стенд — одно окно 1280×720 за раз.
- Коммит — только по просьбе пользователя.

## Review Focus

- Рыбу выдернули рывком наполовину, запас кончился — рыба должна плавно уйти обратно в воду, без рывка удилища и без схода «на ровном месте».
- Свежая рыба во время рывка (вышла из воды на прыжке) — рывок не должен вытаскивать свежую рыбу.
- Отпущенная ЛКМ во время подвода: уставшая рыба не должна сходить с крючка от слабины за секунды (сход на слабине зависит от `HookGrip`, у уставшей рыбы редок).
- Мель рядом с рыбаком: подвод не должен застревать на кромке, когда рыба уже «на мели» — срабатывает подсказка 9 и взятие рукой.
- Сеть: `HeaveReserve` считается только на сервере (как `RodLift`), клиенту не нужен.

---

### Task 1: Запас рывка рук

**Files:**
- Modify: `Source/QuietLake/LakeGame.h` (рядом с `HandHeaveForce`, `HandStrain`, `HandLiftLimit`)
- Modify: `Source/QuietLake/LakeGame.cpp` (`UpdateRodHands`, сброс при начале боя/`FinishFishing`)
- Test: `Source/QuietLake/LakeHeavyLandingTest.cpp` (`HeavyLanding.HeldLift` переписать)

**Interfaces:**
- Produces: `float HeaveReserve = 1` (0..1), `static constexpr float HandHeaveForce = 1000`, `HeaveSeconds = 3`, `HeaveRecoverSeconds = 6`; `HandLiftLimit()` = `Lerp(HandPullForce, HandHeaveForce, HandStrain * HeaveReserve)`.

- [ ] Тест `HeldLift`: (а) `HandStrain=0`, ЛКМ, нагрузка 1500 → `RodLift` падает; (б) нагрузка 100 → растёт; (в) `HandStrain=1`, нагрузка 800 → сначала растёт, через 4 с непрерывной нагрузки запас < 0.1 и `RodLift` падает; (г) после 7 с с нагрузкой 100 запас > 0.9.
- [ ] Реализация: в `UpdateRodHands` при `bLift && Force > HandPullForce && HandStrain > 0` запас тратится `DeltaSeconds / HeaveSeconds * HandStrain`, иначе восстанавливается `DeltaSeconds / HeaveRecoverSeconds`. Сброс в 1 при новой подсечке.
- [ ] Сборка, тест `QuietLake.Fishing.HeavyLanding` проходит.

### Task 2: Подвод к мели — подсказка и стенд

**Files:**
- Modify: `Source/QuietLake/LakeGame.cpp` (`UpdateFight`: условие `bTooHeavy`, текст подсказки 10)
- Modify: `Source/QuietLake/LakeBench.cpp` (скриптовая рука: на подсказке 10 отпустить ЛКМ и отходить, держа отпущенной до мели/улова)

**Interfaces:**
- Consumes: `HeaveReserve` из Task 1.

- [ ] `bTooHeavy`: уставшая рыба у поверхности, и либо нагрузка рук выше предела, либо запас рывка < 0.15. Текст 10: «Рыбу не поднять. Отпустите ЛКМ — удилище опустится — и отходите от воды: рыба на боку пойдёт за вами на мель, там возьмёте рукой.»
- [ ] Стенд: щука 2 кг и лещ 1,2 кг — рыба доходит до мели и берётся рукой; замерить время подвода. Если подвод дольше ~40 с — найти по `-SwimDebug`, что держит рыбу (направление лески, сопротивление тела на боку, дно), и поправить физику.

### Task 3: Физический тест подъёма и подвода

**Files:**
- Modify: `Source/QuietLake/LakeHeavyLandingTest.cpp` (`HeavyLanding.PhysicalLift`)

- [ ] Уставший окунь 1,2 кг при удержании ЛКМ выходит из воды и берётся рукой (рывок).
- [ ] Уставшая щука 4 кг: удержанием ЛКМ не поднимается, снасть не рвётся за 60 с, рыба не в `Landing`. Подвод проверяется стендом (в тестовом мире нет земли под рыбаком).
- [ ] Порог 99/101 % прочности — оставить как есть.

### Task 4: Проверка и документы

- [ ] Сборка; автотесты `QuietLake.Fishing` дважды.
- [ ] Стенд по шести видам: плотва 0.2, окунь 0.3, краснопёрка 0.25, карась 0.5, лещ 1.2, щука 2 — все пойманы, ни одна не вышла из воды свежей, нет таймаутов.
- [ ] WORLD.md («Рыбак и снасть», вываживание), README «Проверено 16 ноября 2026», `docs/heavy-fish-landing.md` — лебёдка заменена рывком и подводом; CLAUDE.md — строка «Устройство» про `UpdateRodHands`, если меняется смысл.
