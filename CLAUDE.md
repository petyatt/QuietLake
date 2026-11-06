# «Тихое озеро» (QuietLake) — памятка для Claude

Сетевая игра про рыбалку с поплавочной удочкой на UE 5.8. Почти всё мира создаётся из C++ при запуске
(рельеф, растения, предметы, рыбы), ассеты генерируются скриптами из `Tools/`.
Как играть и что проверено — [README.md](README.md); как устроен мир и механики — [WORLD.md](WORLD.md).

## Правила работы

- Общаться с пользователем по-русски, включая промежуточные сообщения и сообщения коммитов.
  Комментарии в коде — по-английски, коротко и по делу.
- Коммитить только по просьбе пользователя.
- Изменения проекта и запуск работы в других чатах сначала согласовывать с пользователем; выполнять работу в пределах согласованной задачи.
- Никогда не коммитить `Content/Fab`, `Content/Megaplant_Library`, `Content/Characters/Mannequins`, `Content/Characters/MetaHumans`
  (они в `.gitignore`; восстанавливаются через Fab в редакторе, `Tools/CopyMannequin.ps1` и `Tools/CreateFisherMetaHuman.py`;
  Megaplant в ветке `experiment/landscape-water` больше не используется).
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

Автотесты (`Source/QuietLake/LakeFishingTest.cpp`: ServerRules, FishBehaviour, WeatherMood, PondLife, BitePatterns, Casting, WindDrift, RodControl, FishMotion, Ripples, Stocking, SpeciesFiles, Interactions, Food, Groundbait, Learning, Water, MixedSchools, BanksAndDepth, RodCase, ThreeRods);
результат — строки `Test Completed` в `Saved/Logs/QuietLake.log`. Запускать дважды: часть тестов случайна.

```bash
"/f/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" C:/Fish/QuietLake.uproject -ExecCmds="Automation RunTests QuietLake.Fishing;Quit" -unattended -nopause -nosplash -NullRHI
```

Сетевой тест (сервер и два клиента) — через инструмент PowerShell, без `-ExecutionPolicy Bypass`:
`& C:\Fish\Tools\TestNetwork.ps1`.

Для локальных правок окружения сначала использовать короткий прицельный просмотр: `& Tools/PreviewScene.ps1 -View Shore` (кромка у лагеря), `-View Camp`, `-View Pier`, `-View Air` (всё озеро сверху), `-View Bowl` (Landscape без воды: чаша, прорезанная Water) или имя места ловли. `-StayOpen` оставляет окно открытым. Камера сразу направляется на выбранное место, один кадр через 8 с после постройки мира, затем выход. Полный облёт `-LakePreview` запускать только для общей проверки: пользователь попросил не ждать обзора всего озера ради одного участка.

Превью — сценарии, которые сами снимают кадры в `Saved/*.png`, пишут замеры строками `QUIET_LAKE_*`
в лог и закрывают игру (`MSYS_NO_PATHCONV=1` обязателен, иначе Git Bash испортит путь карты). Карта игры — `/Game/Maps/Lake` (Landscape и движковая Water, ветка `experiment/landscape-water`); со старой `/Engine/Maps/Entry` мир, как прежде, целиком строится кодом:

```bash
MSYS_NO_PATHCONV=1 "/f/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe" C:/Fish/QuietLake.uproject /Game/Maps/Lake -game -windowed -ResX=1280 -ResY=720 -LandPreview -nosound -log=Land.log
```

| Флаг | Что показывает | Где код |
|---|---|---|
| `-LakePreview` | места озера с воздуха и с берега, кусты и опушка вблизи, затем рыбака: заброс, поплавок, рогатка | `LakeEnvironment.cpp`, `LakePreviews.cpp` |
| `-BitePreview` | каждое движение поплавка при поклёвке крупным планом | `LakePreviews.cpp` |
| `-LandPreview` | рыба на леске выходит из воды и попадает в руку | `LakePreviews.cpp` |
| `-FightPreview` | вываживание сбоку под водой: глубина, ход, повороты, натяжение; лог `QUIET_LAKE_FIGHT` | `LakePreviews.cpp` |
| `-MouthPreview` | рот каждого вида рыб крупным планом, кадр `Saved/Mouth_<вид>.png` | `LakePreviews.cpp` |
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
| `-CloudPreview` | объёмные облака: ясно с кучевыми, кучевые, почти сплошные, пасмурно, морось, ливень, закат, рассвет, тени облаков с высоты, молния днём и ночью, ночь с облаками (`Saved/Clouds_*.png`); время кадра в логе `QUIET_LAKE_WEATHER_SHOT`, удары `QUIET_LAKE_LIGHTNING`, гром `QUIET_LAKE_THUNDER` | `LakeWeather.cpp` |
| `-FrontPreview` | приход ливня фронтом: ясный полдень, облака из-за горизонта, край над озером, ливень (`Saved/Front_1…8.png`); лог `QUIET_LAKE_FRONT` | `LakeWeather.cpp` |
| `-NightPreview` | сразу ночь: три кадра — берег, в сторону луны, густой туман; по 10–12 с на кадр (~50 с всего) | `LakeWeather.cpp` |
| `-MapPreview` | миникарта и большая карта озера (M): `Saved/Map_Minimap.png`, `Saved/Map_Full.png` | `LakeHUD.cpp` |
| `-CasePreview` | чехол для удочек: за спиной и от первого лица, призрак после B, на земле, подсказка «Открыть / Надеть», открытая крышка и окно, все удочки в чехле, контур удочки при удержании R и удочка на земле, три снасти на рогатках; лог `QUIET_LAKE_CASE` | `LakePreviews.cpp` |
| `-FisherPreview` | рыбак-MetaHuman в одежде: вид спереди, сбоку, сзади и лицо крупно (`Saved/Fisher_Front/Side/Back/Face.png`) | `LakePreviews.cpp` |
| `-PlantPreview` | растения нового мира в ряд за лагерем в ясный полдень: весь ряд, хвойные, лиственные с кустами, тростник с рогозом и осокой, кора берёзы, ольхи и сосны крупно (`Saved/Plants_*.png`); размеры в логе `QUIET_LAKE_PLANT` | `LakePreviews.cpp` |
| `-ExposureProbe` | свободная экспозиция с ShowFlag.VisualizeHDR, для калибровки света | `LakeWeather.cpp` |

Скрипты ассетов в `Tools/`: `Generate*.py` запускаются Blender (`--background --factory-startup --python …`; кроме `GenerateLifeSounds.py`, `GenerateLakeSounds.py` и `GenerateThunder.py` — это обычный `python`),
`Create*.py` и `Import*.py` — питоном редактора (`GenerateFishModels.py`/`ImportFish.py` с `QUIETLAKE_FISH=Bream,Rudd` в окружении делают только эти виды) (`CreateGrownPlants.py` готовит графы редактора растений, `QUIETLAKE_PLANTS=Pine_B,Alder_A` — только эти; выгрузить меш можно только из окна графа: `ExportGrownPlants.py` в полном редакторе (`-ExecCmds="py …"`) открывает графы, в каждом — «Экспорт» и подтверждения; после выгрузки `GeneratePlantTextures.py` (Blender) и `TunePlantMaterials.py` дают берёзе и ольхе свою кору; `GenerateShorePlants.py` (Blender) и `ImportShorePlants.py` — тростник, рогоз и осока в `Content/Plants/Shore`; `TuneFishMaterials.py` даёт готовым материалам рыб вид под водой из `FishLook.py`): `UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript -script=C:/Fish/Tools/<имя>.py`.
Готовая (купленная) модель рыбы — конвейер с тремя LOD: `PrepareMeshyLODs.py` в Blender переставляет модель (нос в начало координат, тело по +X, координата волны в красном канале вершинных цветов) и облегчает её по бюджету вида; `ImportMeshyLODs.py` запускается **в полном редакторе** (`UnrealEditor.exe … -ExecutePythonScript=`, в commandlet нет `StaticMeshEditorSubsystem`) через `ImportMeshyLODsRun.py`, импортирует LOD0 и подкладывает LOD1/2, собирает материал `M_Meshy_<Вид>`. Список видов — `QUIETLAKE_FISH`, папка источников — `QUIETLAKE_MESHY_SOURCE` (по умолчанию `Saved/AssetReview/Meshy-20261101/<вид>`), длины моделей пишутся в `meshLength` паспорта. Простой вариант без LOD — `ProcessMeshyFish.py` + `ImportTexturedFish.py`. Уровень озера: `CreateTerrainMaterial.py` (commandlet) собирает и `M_LakeLandscape` (материал Landscape со слоями Meadow/Forest/Clay), затем `UnrealEditor.exe C:/Fish/QuietLake.uproject -ExecCmds="py C:/Fish/Tools/BuildLakeLevel.py"` — **полный редактор, именно `-ExecCmds="py …"`** (`-ExecutePythonScript` выходит сразу после скрипта, а кистям Water нужны кадры) — строит `/Game/Maps/Lake` с нуля: Landscape из `LakeMap` (`ULakeLevelLibrary`, `LakeLevel.cpp`), озеро Water по контуру берега и реки Water по ручьям, поднимает мели «островами» Water, ждёт прорезки, запекает высоты в `Content/Maps/LakeGround.bin` (их читает `LakeMap::Ground`; без файла — исходная формула `LakeMap::DesignGround`) и сохраняет; в логе `QUIET_LAKE_LEVEL … deepest stored point`, `… baked … camp bank`. После правки дна в редакторе — пересобрать уровень скриптом, иначе файл высот устареет. Облака: `GenerateCloudNoise.py` (Blender) — бесшовные 3D-шумы в `Tools/GeneratedClouds`, `ImportCloudNoise.py` — объёмные текстуры `Content/Weather/Noise`, `CreateCloudMaterial.py` — материал `M_LakeClouds`. Купленные деревья (ива, дуб): `PrepareTrees.py` в Blender ставит дерево комлем в начало координат и задаёт высоту, `PrepareTreeFoliage.py` в Blender заменяет комки кроны листовыми карточками и рисует текстуры листьев (проверочный рендер `Saved/TreeRender_*.png`), `ImportTrees.py` (commandlet) импортирует их Nanite-мешами в `Content/Plants/Trees`; источники распаковываются в `Saved/TreeAssets`.
Рыбак (MetaHuman + своя одежда), по порядку: `CreateFisherMetaHuman.py` — **в полном редакторе** (`UnrealEditor.exe … -ExecutePythonScript=`; нужны MetaHuman Creator Core Data из лаунчера и вход в Epic для облачного рига и текстур; флаги `QUIETLAKE_MH_PRESET=Bruce`, `QUIETLAKE_MH_CLOUD=1`, `QUIETLAKE_MH_BUILD=1`, `QUIETLAKE_MH_EXPORT=1`) создаёт `MH_Fisher` (лицо из пресета, тело 180 см), собирает `Content/Characters/MetaHumans/Fisher` и выгружает FBX тела и головы в `Saved/FisherBody`; `GenerateFisherClothes.py` (Blender, аргументы — эти два FBX и папка) шьёт по телу ватник со стоячим воротником, свитер с горлом, штаны, сапоги и кепку (`Fisher_Outfit.fbx`, `Fisher_Cap.fbx`, кадры `Saved/FisherClothes_*.png`); `GenerateFisherTextures.py` (Blender) — текстуры ткани в `Tools/GeneratedFisher`; `CreateFisherMaterials.py` — `M_Fisher_Cloth` и `MI_Fisher_*`; `ImportFisherOutfit.py` (commandlet) — `SKM_Fisher`, `SKM_Fisher_Cap`, LOD и материалы. Если в Creator поменять лицо или причёску — пересобрать (`QUIETLAKE_MH_BUILD=1 QUIETLAKE_MH_EXPORT=1`) и повторить три последних шага, чтобы кепка и ворот сели по новой голове.

## Код (`Source/QuietLake`)

- `LakeGame.h/.cpp` — рыбак `ALakeCharacter` (заброс, поклёвка, подсечка, вываживание, вынимание рыбы,
  и его вид: тело — `SKM_Fisher` (кисти и одежда на скелете MetaHuman, совместимом с манекеном, — клипы
  манекена и IK рук `FisherAnim` работают как раньше); `CreateFisherOutfit` берёт лицо и пряди из собранного
  `BP_MH_Fisher` (лицо со своим `ABP_Face` копирует позу тела) и надевает кепку; от первого лица они не
  рисуются для самого рыбака (`SetOwnerNoSee`), на выделенном сервере не создаются; проверка — `-FisherPreview`;
  поза удилища), `ALakeEnvironment` (объявление), `ALakeGameMode`.
- `LakePreviews.cpp` — сценарии `-LakePreview`, `-BitePreview`, `-LandPreview`.
- `LakeRods.cpp` — три удочки рыбака (`ALakeCharacter::Rods`: чехол, руки, рогатка, земля; снасть удочки на рогатке ловит сама, рыба выбирает снасть по `ALakeFish::TargetRig`) и чехол (на спине / на земле, открыт): серверные запросы и локальный интерфейс владельца — призрак (B, R 2 с), подсказка над чехлом (колесо, F), окно чехла. Вид чехла и призрака — `LakeRodCase.h/.cpp` (`ALakeRodCase`, `ALakeGhost`); окно и подсказки рисует `LakeHUD.cpp`. Модели — `GenerateTackleModels.py`/`ImportTackle.py` с `QUIETLAKE_TACKLE=RodCase,RodCaseLid,RodCaseStrap,RodPacked_Light,RodPacked_Medium,RodPacked_Heavy`.
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
- `LakeEnvironment.cpp` — построение мира: рельеф (без Landscape), мостки, лодки, кладки, колья, лагерь, свет, звуки. Растений, камней и коряг больше не ставит — на уровне озера их будет расставлять PCG Biome Core. На уровне с Landscape и Water (`/Game/Maps/Lake`) свой рельеф и плоскость воды не строит.
- `LakeLevel.h/.cpp` — инструмент редактора `ULakeLevelLibrary` для `Tools/BuildLakeLevel.py`: Landscape из `LakeMap`, контур берега, озеро и реки Water, замер прорезки.
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
