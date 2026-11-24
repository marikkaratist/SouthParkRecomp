# South Park (N64) — статическая рекомпиляция

Порт South Park (Nintendo 64, USA) на ПК на стеке
[N64Recomp](https://github.com/N64Recomp/N64Recomp) + N64ModernRuntime + RT64 + RecompFrontend.

ROM в репозиторий не входит и никогда не будет входить: нужен собственный дамп.

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

## Установка (готовая сборка)

1. Скачайте `SouthParkRecompiled-v0.1.1-windows-x64.zip` со страницы [Releases](../../releases) и распакуйте в любую папку.
2. Запустите `SouthParkRecompiled.exe`.
3. В лаунчере укажите свой дамп South Park (USA) — `.z64`; если дамп в `.v64`/`.n64`, сначала преобразуйте его через `tools/rom_prepare.py`. ROM проверяется по хешу (SHA1 выше).

Нужны Windows 10/11 x64 и видеокарта с поддержкой DirectX 12 или Vulkan.

## Структура

- `src/`, `include/`, `assets/`, `icons/` — приложение: лаунчер, настройки, игровая часть (`src/game/boot.cpp`)
- `patches/` — MIPS-код, который встраивается в игру (пока заглушка)
- `symbols/` — файл символов для N64Recomp и имена функций
- `southpark.us.toml`, `aspMain.us.toml`, `patches.toml` — конфиги рекомпиляторов
- `n64recomp-patches/` — правки N64Recomp, без которых ROM не проходит
- `runtime-patches/` — правки N64ModernRuntime, rt64 и RecompFrontend, применённые к подмодулям
- `tools/` — скрипты анализа ROM
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
- N64Recomp, собранный в `external/N64Recomp/build` с патчами из `n64recomp-patches/`. Версия должна совпадать с той, что лежит в `lib/N64ModernRuntime/N64Recomp` (сейчас коммит `2b6f056`): более новая пишет таблицы секций в формате, которого этот рантайм не знает;
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
git -C lib/RecompFrontend apply ../../runtime-patches/0005-recompfrontend-mouse-controller-focus-race.patch
```

## Лицензия

GPL-3.0 — см. [LICENSE](LICENSE). South Park и все права на игру принадлежат их владельцам; проект не содержит данных игры.

## Этапы

- [x] 0. Инструменты, проект, проверенный ROM
- [x] 1. Анализ ROM: карта памяти, загрузчик, TLB-подкачка кода — см. [docs/LAYOUT.md](docs/LAYOUT.md)
- [x] 2. Символы: поиск функций, перенос имён из Turok 2, первый прогон N64Recomp — см. [docs/SYMBOLS.md](docs/SYMBOLS.md)
- [x] 3. Рекомпиляция и сборка: программа собирается и открывает лаунчер — см. [docs/RUNTIME.md](docs/RUNTIME.md)
- [x] 4. Первый запуск и отладка: игра проходит вступление с картинкой — см. [docs/RUNTIME.md](docs/RUNTIME.md)
- [ ] 5. Полировка: управление, widescreen, FPS, моды
