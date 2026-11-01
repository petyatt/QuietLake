# Природные звуки озера

Все четыре ambience-записи взяты из проекта **radio aporee** (field recordings), выложены на
archive.org под **Public Domain Mark 1.0** — можно использовать без ограничений, атрибуция не
требуется (но она указана ниже, чтобы было видно происхождение).

Исходники лежат в `Tools/Sounds/`, в движок импортированы как `/Game/Audio/A_*` и зациклены
(`looping = true`).

| Ассет в игре | Файл-источник | Запись | Автор / место | Лицензия | Источник |
|---|---|---|---|---|---|
| `A_Wind` | `A_Wind.ogg`, 120,7 с | Ветер в сосновом лесу | radio aporee, запись 41242 (Nyker Strandvej 30, Rønne, Дания) | Public Domain Mark 1.0 | https://archive.org/details/aporee_41242_47058 |
| `A_RainLight` | `A_RainLight.ogg`, 239,0 с | «dynamic rain», тихий дождь на лестнице | radio aporee, запись 29862 (Poznań, Mateckiego, Польша) | Public Domain Mark 1.0 | https://archive.org/details/aporee_29862_34343 |
| `A_RainHeavy` | `A_RainHeavy.wav`, 124,2 с | Сильный дождь у кафе | radio aporee, запись 73657 (Барселона, Испания) | Public Domain Mark 1.0 | https://archive.org/details/aporee_73657_86080 |
| `A_Crickets` | `A_Crickets.ogg`, 365,6 с | Сверчки ночью, «walking with crickets» | radio aporee, запись 31854 (Лондон, Великобритания) | Public Domain Mark 1.0 | https://archive.org/details/aporee_31854_36582 |

## Что не используется

- `A_Birds` — птичий ambience удалён из игры по просьбе пользователя (звук больше не
  проигрывается; ассет в `Content/Audio/A_Birds.uasset` остаётся, но код на него не ссылается).
- Одноразовые звуки рыбы (`/Game/Life/A_Plop`, `A_Bubbles`, `A_Splash`, `A_Scatter`) — всё ещё
  сгенерированы `Tools/GenerateLifeSounds.py`, не записи.

## Импорт

`Tools/Sounds/` → `/Game/Audio` делается питоном редактора (см. `Saved/import_nature_sounds.py`
в истории, повторяемо: `AssetImportTask` + `looping = true`). При замене записи повторить импорт
с тем же именем и включённым зацикливанием.
