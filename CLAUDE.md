# «Тихое озеро» (QuietLake) — памятка для Claude

Сетевая игра про рыбалку с поплавочной удочкой на UE 5.8. Почти всё мира создаётся из C++ при запуске
(рельеф, растения, предметы, рыбы), ассеты генерируются скриптами из `Tools/`.
Как играть и что проверено — [README.md](README.md); как устроен мир и механики — [WORLD.md](WORLD.md).

## Правила работы

- Общаться с пользователем по-русски, включая промежуточные сообщения и сообщения коммитов.
  Комментарии в коде — по-английски, коротко и по делу.
- Коммитить только по просьбе пользователя.
- Никогда не коммитить `Content/Fab`, `Content/Megaplant_Library`, `Content/Characters/Mannequins`
  (они в `.gitignore`; восстанавливаются через Fab в редакторе и `Tools/CopyMannequin.ps1`;
  после восстановления Megaplant запустить `Tools/FixPineBark.py` питоном редактора).
- Fab: только бесплатные лицензии; не входить в аккаунты и не вводить пароли за пользователя.
- После изменения механики обновлять [WORLD.md](WORLD.md), а после проверки — раздел «Проверено …» в [README.md](README.md).

## Художественный характер

Деревенское озеро: часть берегов лесная, часть открытая луговая. Пользователь позже сам добавит деревню на открытых участках — сохранять их свободными, не засаживать лесом и не занимать постоянными постройками. У причала — протоптанная земля, старое дерево, лодки и рабочие детали; лесные берега оформлять отдельно.

## Пути

- Проект: `C:\Fish`, движок: `F:\UE_5.8`, Blender: `F:\Programm Files\blender.exe`.
- Shell — Git Bash на Windows: для `Build.bat` нужен `MSYS_NO_PATHCONV=1`.
- Правки, которые трудно сделать через Edit, удобно писать `.py`-скриптом в scratchpad и запускать
  `python file.py` (heredoc с кавычками в Bash часто ломается).

## Сборка, тесты, превью

Сборка (редактор нужно закрыть, иначе DLL занята):

```bash
MSYS_NO_PATHCONV=1 "/f/UE_5.8/Engine/Build/BatchFiles/Build.bat" QuietLakeEditor Win64 Development "-Project=C:/Fish/QuietLake.uproject" -WaitMutex
```

Автотесты (`Source/QuietLake/LakeFishingTest.cpp`: ServerRules, FishBehaviour, WeatherMood, PondLife, BitePatterns, Casting, WindDrift, RodControl, FishMotion, Ripples, Stocking, SpeciesFiles, Interactions, Food, Groundbait, Learning, Water, MixedSchools);
результат — строки `Test Completed` в `Saved/Logs/QuietLake.log`. Запускать дважды: часть тестов случайна.

```bash
"/f/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" C:/Fish/QuietLake.uproject -ExecCmds="Automation RunTests QuietLake.Fishing;Quit" -unattended -nopause -nosplash -NullRHI
```

Сетевой тест (сервер и два клиента) — через инструмент PowerShell, без `-ExecutionPolicy Bypass`:
`& C:\Fish\Tools\TestNetwork.ps1`.

Для локальных правок окружения сначала использовать короткий прицельный просмотр: `& Tools/PreviewScene.ps1 -View Shore` (кромка у лагеря), `-View Camp`, `-View Pier` или имя места ловли. `-StayOpen` оставляет окно открытым. Камера сразу направляется на выбранное место, один кадр через 8 с после постройки мира, затем выход. Полный облёт `-LakePreview` запускать только для общей проверки: пользователь попросил не ждать обзора всего озера ради одного участка.

Превью — сценарии, которые сами снимают кадры в `Saved/*.png`, пишут замеры строками `QUIET_LAKE_*`
в лог и закрывают игру (`MSYS_NO_PATHCONV=1` обязателен, иначе Git Bash испортит путь карты `/Engine/Maps/Entry`):

```bash
MSYS_NO_PATHCONV=1 "/f/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe" C:/Fish/QuietLake.uproject /Engine/Maps/Entry -game -windowed -ResX=1280 -ResY=720 -LandPreview -nosound -log=Land.log
```

| Флаг | Что показывает | Где код |
|---|---|---|
| `-LakePreview` | места озера с воздуха и с берега, кусты и опушка вблизи, затем рыбака: заброс, поплавок, рогатка | `LakeEnvironment.cpp`, `LakePreviews.cpp` |
| `-BitePreview` | каждое движение поплавка при поклёвке крупным планом | `LakePreviews.cpp` |
| `-LandPreview` | рыба на леске выходит из воды и попадает в руку | `LakePreviews.cpp` |
| `-FightPreview` | вываживание сбоку под водой: глубина, ход, повороты, натяжение; лог `QUIET_LAKE_FIGHT` | `LakePreviews.cpp` |
| `-CastPreview` | заброс с руки сбоку и вблизи, ранний и ближний заброс, тяжёлое и лёгкое удилища, вынимание с ловлей рукой, ходьба с оснасткой, глубокий спуск и спуск глубже воды | `LakePreviews.cpp` |
| `-RodPreview` | удилище: изгиб кончика при забросе лёгким, средним и тяжёлым, при вываживании | `LakePreviews.cpp` |
| `-LeaderPreview` | поводок крупным планом: оснастка в руке, замах, полёт, приводнение, лежащая снасть, подъём с остановкой (ЛКМ отпущена и снова зажата), вынимание; лог `QUIET_LAKE_LEADER` (`-Depth=`) | `LakePreviews.cpp` |
| `-RetrievePreview` | вынимание снасти: удилище вверх, снасть подлетает, левая рука берёт грузило — сбоку, из-за плеча, от первого лица с остановкой; `-LongCast` — дальний заброс; лог `QUIET_LAKE_RETRIEVE` | `LakePreviews.cpp` |
| `-LinePreview` | боковой ветер у лагеря сверху: парус лески, снос поплавка, оснастка на дне стоит | `LakePreviews.cpp` |
| `-GroundbaitPreview` | три шара прикормки у лагеря (полёт, круги), 4 минуты замера: рыба у прикормки и у такого же места без неё, панель F3; лог `QUIET_LAKE_GROUNDBAIT` | `LakePreviews.cpp` |
| `-LifePreview` | знаки на воде, малёк, панель F3, время кадра | `LakeLife.cpp` |
| `-FishPreview` | плавание: пути рыб сверху, стаи, карась у дна, засада и бросок щуки под водой, разворот стаи от всплеска, размеры, рыба у поверхности; лог `QUIET_LAKE_FISH_MOTION`, `QUIET_LAKE_FISH_SMOOTH` | `LakeLife.cpp` |
| `-WaterPreview` | вода с берега, с мостков, сверху и из-под воды при солнце, в пасмурно, вечером и ночью, рыбы на 15/40/80 см; `-WaterQuick` — только солнце | `LakeLife.cpp` |
| `-WeatherPreview` | погода и время суток | `LakeWeather.cpp` |
| `-ExposureProbe` | свободная экспозиция с ShowFlag.VisualizeHDR, для калибровки света | `LakeWeather.cpp` |

Скрипты ассетов в `Tools/`: `Generate*.py` запускаются Blender (`--background --factory-startup --python …`),
`Create*.py` и `Import*.py` — питоном редактора (`GenerateFishModels.py`/`ImportFish.py` с `QUIETLAKE_FISH=Bream,Rudd` в окружении делают только эти виды) (`CreateGrownPlants.py` готовит графы редактора растений, выгрузка — Ctrl+E в окне графа): `UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript -script=C:/Fish/Tools/<имя>.py`.

## Код (`Source/QuietLake`)

- `LakeGame.h/.cpp` — рыбак `ALakeCharacter` (заброс, поклёвка, подсечка, вываживание, вынимание рыбы,
  поза удилища), `ALakeEnvironment` (объявление), `ALakeGameMode`.
- `LakePreviews.cpp` — сценарии `-LakePreview`, `-BitePreview`, `-LandPreview`.
- `LakeHUD.h/.cpp` — экран: карточка статуса, полосы натяжения и заброса, погода, панель F3.
- `LakeRod.h/.cpp` — удочка только для вида (`ALakeRod`): физическое удилище (цепочка с упругостью),
  физическая основная леска (ветер, вода), физический поплавок, дробь, крючок, рыба на леске. Считается на каждой машине из реплицируемого состояния рыбака
  (`FRodInput`), по сети не передаётся.
- `LakeFish.h/.cpp` — рыба: паспорта видов (`FFishSpeciesInfo`, читаются из `Content/LiveFish/Species/*.json`, описание полей — README.md там же; новый вид — новый файл, код видов по именам не различает), походка (`FFishGait`), наживки, поклёвки (`NextBiteStep`: рыба решает каждый шаг, возвращается после промаха), плавание (`Move`: повороты дугой, сглаженное желание), стая, распорядок, вываживание, сглаживание на клиентах (`FFishNetSmoothing`).
  Здесь же общие перечисления, включая `EFishingState`.
- `LakeFood.h/.cpp` — естественный корм пруда: планктон, насекомые, мотыль, растения (`FLakeFoodField`: что где есть, выедание и отрастание; глубина планктона по времени суток); рыба выбирает корм по рациону паспорта (`diet`).
- `LakeWater.h/.cpp` — вода по глубине: расслоение и скачок температуры, кислород (застойное дно, трава ночью), где рыба может жить (`oxygenMin` в паспорте).
- `LakeRipples.h/.cpp` — живые волны у игрока: волновое уравнение на сетке вокруг камеры (`FLakeRippleSim`), источники — рыба у поверхности, поплавок, дождь (`ALakeRipples`), высоты уходят в материал воды текстурой.
- `LakeLife.h/.cpp` — «Live Fish»: стаи, малёк, хищники, знаки на воде, испуг от заброса и шагов (`Disturb`), замеры плавания, панель F3.
- `LakeMap.h/.cpp` — форма озера (SDF), глубины, рельеф, места рыбалки, зоны.
- `LakeEnvironment.cpp` — построение мира: рельеф, лес, трава, камыш, кувшинки, коряги, мостки, свет.
- `LakeForest.h/.cpp` — источник точек PCG по рельефу и зонам леса, граф `Content/Forest/PCG_LakeForest.uasset` с двумя штатными Skinned Mesh Spawner. Плотность и опушки редактируются в графе; луг под деревню, подходы и места ловли исключены. `LakeForestTest.cpp` проверяет зоны и повторяемость; запускать весь набор через `Automation RunTests QuietLake`. `-LegacyForest` возвращает старую расстановку; `Tools/PreviewScene.ps1 -View Forest` сразу показывает опушку.
- `LakeWeather.h/.cpp` — время суток, погода, экспозиция, туман, состояние воды, условия клёва.
- `LakeMenu.h/.cpp` — главное меню и `ALakePlayerController`; `FisherAnim.h/.cpp` — анимация рыбака.
- `LakeFishingTest.cpp` — автотесты.

## Устройство

- Сервер решает всё: рыбы, погода, поклёвки, улов. Клиенты получают состояние и анимируют сами
  (удочку, поплавок, рыбу на леске, знаки на воде).
- Цикл ловли — `EFishingState`: `Idle` → `Waiting` (наживка в воде) → `Bite` (рыба держит наживку,
  время на подсечку) → `Fight` → `Landing` (рыба на леске в воздухе, засчитывается в руке).
  `bRetrieving` — снасть поднимается из воды в состоянии `Waiting`. Заброс автоматический: `bCharging` — ЛКМ зажата,
  сила растёт (`CastCharge`, `CastChargeTime`); отпускание (`DoCast`) выбирает точку (`CastTargetFor`: куда
  смотрит рыбак, дальность от силы, не дальше `CastReach`) в `FloatPosition`, `bRigFlying` — поплавок летит
  к ней по дуге за `CastFlight` (`ALakeRod`), остальная снасть по физике; приводнение — по симуляции сервера
  (`OnRigLanded`). ЛКМ после заброса — вытаскивание (`StartRetrieve`, `bRetrieveReturns`), ПКМ — подсечка
  (`Strike`). Мышь ведёт удилище и камеру (`MoveRodAxis`); ловля оснастки рукой — `bRigInHand`. Вес удилища (`ERodKind`, `RodHeft`) замедляет
  и смягчает все движения.
- Рыба видна игроку в радиусе 150 м (`NetCullDistance`), вдали её симуляция грубее.
- Новые превью: добавить ветку в `ALakeCharacter::RunPreviews` (или `RunPreview` у нужного актора)
  и флаг в список пропуска меню в `LakeMenu.cpp`.
