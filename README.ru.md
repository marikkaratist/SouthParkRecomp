[English](README.md) | **Русский**

# South Park: Recompiled

Порт South Park (Nintendo 64, USA) на ПК методом статической рекомпиляции, на стеке
[N64Recomp](https://github.com/N64Recomp/N64Recomp) + N64ModernRuntime + RT64 + RecompFrontend.

ROM в репозиторий не входит и никогда не будет входить: нужен собственный дамп.

## Возможности

- **Широкий экран** как следует: без чёрных полос, трёхмерная сцена на всё окно, объекты по
  бокам отрисовываются (пирамида видимости игры расширена под форму окна). Небо на всю ширину,
  интерфейс можно прижать к краям окна или оставить в середине, как на консоли
  (General → HUD Placement).
- **60 обновлений в секунду.** Игра читает управление и двигает мир 60 раз в секунду вместо 30,
  со скоростью оригинала (General → Game Update Rate). Сверх этого RT64 рисует промежуточные
  кадры с частотой монитора, с интерполяцией каждого объекта, так что движение и смены плана
  остаются чистыми.
- **Современное управление**, как в нынешних шутерах на геймпаде: левый стик — ходьба и шаг
  вбок, правый стик или мышь — поворот и обзор, взгляд остаётся там, где его оставили; левая
  кнопка мыши — огонь (General → Control Scheme, Invert Look, Mouse Sensitivity). Схема Original
  сохраняет раскладку Nintendo 64.
- **Дальность прорисовки**: Original или Far — мир рисуется вдвое дальше, и туман начинается
  вдвое дальше (General → Draw Distance). Сняты пределы движка на модели, материалы и
  отложенные объекты в кадре, поэтому дальние здания не пропадают.
- Лаунчер с управлением с геймпада и клавиатуры, переназначение кнопок, сохранения на
  Controller Pak в файле рядом с настройками.

Поддержка модов в задачи проекта не входит; раздел Mods в лаунчере достался от фронтенда.

## Установка

1. Скачайте `SouthParkRecompiled-v0.9.2-windows-x64.zip` со страницы [Releases](../../releases)
   и распакуйте в любую папку.
2. Запустите `SouthParkRecompiled.exe`.
3. В лаунчере укажите свой дамп South Park (USA) в формате `.z64`. Если дамп в `.v64` или
   `.n64`, сначала преобразуйте его через `tools/rom_prepare.py`. ROM проверяется по хешу (SHA1 ниже).

Нужны Windows 10/11 x64 и видеокарта с поддержкой DirectX 12 или Vulkan.

## ROM

| | |
|---|---|
| Игра | South Park (USA), код `NDTE`, версия 0 |
| Размер | 16 МБ |
| SHA1 (.z64) | `42d7350d1f9040c5546f481c4d650b99a23aadbc` |
| CRC в заголовке | `7ECBE939 3C331795` |
| CIC | 6102 |
| Точка входа | `0x80000400` |
| libultra | 2.0D |
| Микрокод | F3DEX.NoN fifo 2.05, L3DEX fifo 2.05 |
| Упаковка данных | RNC (~5850 блоков, с `0x9C9E34`) |

Подготовка ROM из любого формата (.z64 / .v64 / .n64), оригинал не меняется:

```
python tools/rom_prepare.py "<путь к дампу>" rom/southpark.us.z64
```

## HD-текстуры (свой пак)

Текстурные паки — это моды `.rtz`, как HD-паки для Zelda 64: Recompiled или Banjo: Recompiled.
`tools/make_texture_pack.py` собирает такой пак из вашей копии игры с помощью апскейлера Real-ESRGAN
([realesrgan-ncnn-vulkan](https://github.com/xinntao/Real-ESRGAN/releases)):

1. Играйте со включённым дампом; каждая показанная текстура сохраняется один раз:
   ```
   SouthParkRecompiled.exe --dump-textures C:\sp\dump
   ```
   Папку можно не указывать: с одним `--dump-textures` (удобно как аргумент запуска в лаунчере,
   например в Playnite) дамп идёт в `%LOCALAPPDATA%\SouthParkRecompiled\texture_dump`.
   Текстуры, которые уже есть в папке, повторно не пишутся, так что дамп можно собирать за несколько заходов.
2. Декодирование, апскейл в 4 раза и упаковка (Python с numpy и Pillow):
   ```
   python tools/make_texture_pack.py all C:\sp\dump C:\sp\work --esrgan <путь к realesrgan-ncnn-vulkan.exe> --packer build\texture_packer.exe
   ```
3. Скопируйте `C:\sp\work\southpark_hd.rtz` в `%LOCALAPPDATA%\SouthParkRecompiled\mods` и включите его в разделе Mods.

Текстуры сопоставляются по хешу RT64, поэтому перед сборкой пройдите все места, которые хотите
охватить; всё, что не попало в дамп, останется оригинальным. Бесшовные текстуры остаются
бесшовными, у прозрачных краёв нет ореола. Мелкие текстуры (до 32 текселов) перед апскейлером
увеличиваются вдвое по ближайшему соседу, чтобы детали в один тексел, например зрачки на портретах,
не стирались. Чтобы поправить текстуру вручную, найдите её в `C:\sp\work\native` (файлы названы по
хешу), положите свою версию под тем же именем в `C:\sp\work\overrides` (размер любой) и снова
запустите шаг `pack`; при пересборке правка сохраняется. Пак делается из вашей копии игры — держите
его для себя.

## Структура

- `src/`, `include/`, `assets/`, `icons/` — приложение: лаунчер, настройки, игровая часть (`src/game/`)
- `patches/` — MIPS-код, который встраивается в игру (пока заглушка)
- `symbols/` — файл символов для N64Recomp и имена функций
- `southpark.us.toml`, `aspMain.us.toml`, `patches.toml` — конфиги рекомпиляторов
- `n64recomp-patches/` — правки N64Recomp, без которых ROM не проходит
- `runtime-patches/` — правки N64ModernRuntime, rt64 и RecompFrontend, применённые к подмодулям
- `tools/` — скрипты анализа ROM, графики и отладки
- `docs/` — карта памяти, символы, устройство рантайма
- `lib/` — подмодули: N64ModernRuntime, rt64, RecompFrontend
- `external/N64Recomp` — рекомпилятор с правками (в .gitignore)
- `rom/`, `references/`, `.venv/` — ROM, чужие проекты для изучения, Python-окружение (в .gitignore)

## Сборка (Windows)

Нужны:

- Visual Studio Build Tools с набором «Разработка классических приложений на C++» и Windows SDK;
- LLVM 20 или новее — `clang-cl` для самой программы (стандартная библиотека MSVC 14.51 старше не принимает);
- LLVM с целью MIPS — `clang` для `patches/` (в сборках LLVM 23 для Windows цели MIPS нет, подходит 18.1.8);
- `make` (например, из w64devkit);
- N64Recomp, собранный в `external/N64Recomp/build` с патчами из `n64recomp-patches/`. Версия
  должна совпадать с той, что лежит в `lib/N64ModernRuntime/N64Recomp` (сейчас коммит `2b6f056`):
  более новая пишет таблицы секций в формате, которого этот рантайм не знает;
- свой ROM: `python tools/rom_prepare.py "<дамп>" rom/southpark.us.z64`.

Пути к инструментам задаются в `build.local.ps1` (образец — параметры в начале `build.ps1`):

```
.\build.ps1              # рекомпиляция при первом запуске, настройка CMake, сборка
.\build.ps1 -Recompile   # заново прогнать N64Recomp и RSPRecomp
```

Подмодули: `git submodule update --init --recursive`, затем правки из `runtime-patches/`:

```
git -C lib/N64ModernRuntime apply ../../runtime-patches/0001-n64modernruntime-fix-thread-queue-remove.patch ../../runtime-patches/0004-n64modernruntime-rsp-done-after-display-list.patch
git -C lib/rt64 apply ../../runtime-patches/0002-rt64-acclaim-f3dex2-as-f3dex2.patch ../../runtime-patches/0003-rt64-msaa-resolve-without-color-target.patch
git -C lib/RecompFrontend apply ../../runtime-patches/0005-recompfrontend-mouse-controller-focus-race.patch ../../runtime-patches/0006-recompfrontend-dump-textures-env.patch
```

## Этапы

- [x] 0. Инструменты, проект, проверенный ROM
- [x] 1. Анализ ROM: карта памяти, загрузчик, TLB-подкачка кода — см. [docs/LAYOUT.ru.md](docs/LAYOUT.ru.md)
- [x] 2. Символы: поиск функций, перенос имён из Turok 2, первый прогон N64Recomp — см. [docs/SYMBOLS.ru.md](docs/SYMBOLS.ru.md)
- [x] 3. Рекомпиляция и сборка: программа собирается и открывает лаунчер — см. [docs/RUNTIME.ru.md](docs/RUNTIME.ru.md)
- [x] 4. Первый запуск и отладка: игра проходит вступление с картинкой — см. [docs/RUNTIME.ru.md](docs/RUNTIME.ru.md)
- [x] 5. Улучшения: широкий экран, 60 обновлений в секунду, современное управление, дальность прорисовки — см. [docs/RUNTIME.ru.md](docs/RUNTIME.ru.md#этап-5-доводка)
- [ ] 6. Доводка: оставшиеся шероховатости перед 1.0

## Лицензия

GPL-3.0 — см. [LICENSE](LICENSE). South Park и все права на игру принадлежат их владельцам;
проект не содержит данных игры.
