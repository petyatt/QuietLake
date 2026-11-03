# Текстуры дна (CC0)

Сканированные текстуры для подводного ложа озера. Обе — **CC0 (общественное достояние)** с
[Poly Haven](https://polyhaven.com), скачаны как 1K PNG и ужаты в 1024/512 для игры:

| Ассет в проекте | Источник | Лицензия |
|---|---|---|
| `T_BedSand_Base/Normal/Rough` | [Aerial Beach 02](https://polyhaven.com/a/aerial_beach_02) — плоский прибрежный песок с рябью | CC0 |
| `T_BedMud_Base/Normal/Rough` | [Brown Mud Rocks 01](https://polyhaven.com/a/brown_mud_rocks_01) — мокрый ил с галькой и растительным мусором | CC0 |

Импортируются и подключаются к материалу дна скриптом `Tools/WireBedTextures.py`
(маска глубины: песок на отмелях, ил глубже; сверху каустика). Исходные PNG — в
`Saved/BedTextures/` (в git не входят).
