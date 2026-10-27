# «Тихое озеро» (QuietLake) — памятка для Claude

Сетевая игра про рыбалку с поплавочной удочкой на UE 5.8. Почти всё мира создаётся из C++ при запуске
(рельеф, растения, предметы, рыбы), ассеты генерируются скриптами из `Tools/`.
Как играть и что проверено — [README.md](README.md); как устроен мир и механики — [WORLD.md](WORLD.md).

## Правила работы

- Общаться с пользователем по-русски, включая промежуточные сообщения и сообщения коммитов.
  Комментарии в коде — по-английски, коротко и по делу.
- Коммитить только по просьбе пользователя.
- Никогда не коммитить `Content/Fab`, `Content/Megaplant_Library`, `Content/Characters/Mannequins`
  (они в `.gitignore`; восстанавливаются через Fab в редакторе и `Tools/CopyMannequin.ps1`).
- Fab: только бесплатные лицензии; не входить в аккаунты и не вводить пароли за пользователя.
- После изменения механики обновлять [WORLD.md](WORLD.md), а после проверки — раздел «Проверено …» в [README.md](README.md).

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

Автотесты (`Source/QuietLake/LakeFishingTest.cpp`: ServerRules, FishBehaviour, WeatherMood, PondLife, BitePatterns);
результат — строки `Test Completed` в `Saved/Logs/QuietLake.log`. Запускать дважды: часть тестов случайна.

```bash
"/f/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" C:/Fish/QuietLake.uproject -ExecCmds="Automation RunTests QuietLake.Fishing;Quit" -unattended -nopause -nosplash -NullRHI
```

Сетевой тест (сервер и два клиента) — через инструмент PowerShell, без `-ExecutionPolicy Bypass`:
`& C:\Fish\Tools\TestNetwork.ps1`.

Превью — сценарии, которые сами снимают кадры в `Saved/*.png`, пишут замеры строками `QUIET_LAKE_*`
в лог и закрывают игру:

```bash
"/f/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe" C:/Fish/QuietLake.uproject /Engine/Maps/Entry -game -windowed -ResX=1280 -ResY=720 -LandPreview -nosound -log=Land.log
```

| Флаг | Что показывает | Где код |
|---|---|---|
| `-LakePreview` | места озера с воздуха и с берега, затем рыбака: заброс, поплавок, рогатка | `LakeEnvironment.cpp`, `LakePreviews.cpp` |
| `-BitePreview` | каждое движение поплавка при поклёвке крупным планом | `LakePreviews.cpp` |
| `-LandPreview` | рыба на леске выходит из воды и попадает в руку | `LakePreviews.cpp` |
| `-LifePreview` | знаки на воде, малёк, панель F3, время кадра | `LakeLife.cpp` |
| `-WeatherPreview` | погода и время суток | `LakeWeather.cpp` |
| `-ExposureProbe` | свободная экспозиция с ShowFlag.VisualizeHDR, для калибровки света | `LakeWeather.cpp` |

Скрипты ассетов в `Tools/`: `Generate*.py` запускаются Blender (`--background --factory-startup --python …`),
`Create*.py` и `Import*.py` — питоном редактора: `UnrealEditor-Cmd.exe C:/Fish/QuietLake.uproject -run=pythonscript -script=C:/Fish/Tools/<имя>.py`.

## Код (`Source/QuietLake`)

- `LakeGame.h/.cpp` — рыбак `ALakeCharacter` (заброс, поклёвка, подсечка, вываживание, вынимание рыбы,
  поза удилища), `ALakeEnvironment` (объявление), `ALakeGameMode`.
- `LakePreviews.cpp` — сценарии `-LakePreview`, `-BitePreview`, `-LandPreview`.
- `LakeHUD.h/.cpp` — экран: карточка статуса, полосы натяжения и заброса, погода, панель F3.
- `LakeRod.h/.cpp` — удочка только для вида (`ALakeRod`): изгиб удилища, леска, физический поплавок,
  дробь, крючок, рыба на леске. Считается на каждой машине из реплицируемого состояния рыбака
  (`FRodInput`), по сети не передаётся.
- `LakeFish.h/.cpp` — рыба: виды, наживки, поклёвки (`MakeBite`), плавание, распорядок, вываживание.
  Здесь же общие перечисления, включая `EFishingState`.
- `LakeLife.h/.cpp` — «Живой пруд»: стаи, малёк, хищники, знаки на воде, панель F3.
- `LakeMap.h/.cpp` — форма озера (SDF), глубины, рельеф, места рыбалки, зоны.
- `LakeEnvironment.cpp` — построение мира: рельеф, лес, трава, камыш, кувшинки, коряги, мостки, свет.
- `LakeWeather.h/.cpp` — время суток, погода, экспозиция, туман, состояние воды, условия клёва.
- `LakeMenu.h/.cpp` — главное меню и `ALakePlayerController`; `FisherAnim.h/.cpp` — анимация рыбака.
- `LakeFishingTest.cpp` — автотесты.

## Устройство

- Сервер решает всё: рыбы, погода, поклёвки, улов. Клиенты получают состояние и анимируют сами
  (удочку, поплавок, рыбу на леске, знаки на воде).
- Цикл ловли — `EFishingState`: `Idle` → `Waiting` (наживка в воде) → `Bite` (рыба держит наживку,
  время на подсечку) → `Fight` → `Landing` (рыба на леске в воздухе, засчитывается в руке).
  `bRetrieving` — снасть поднимается из воды в состоянии `Waiting`.
- Рыба видна игроку в радиусе 150 м (`NetCullDistance`), вдали её симуляция грубее.
- Новые превью: добавить ветку в `ALakeCharacter::RunPreviews` (или `RunPreview` у нужного актора)
  и флаг в список пропуска меню в `LakeMenu.cpp`.
