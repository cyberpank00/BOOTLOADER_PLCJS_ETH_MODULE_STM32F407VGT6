# BOOTLOADER_PLCJS_ETH_MODULE_12DI_D4MG_STM32F407VGT6

Ethernet bootloader для модуля PLCJS 12-DI на базе `STM32F407VGT6`.

Бутлоадер предоставляет OTA-протокол обновления по `Modbus TCP`, сохраняет новый
образ во внутреннюю staging-область, проверяет `CRC32`, устанавливает образ в
область приложения и затем передает управление основной прошивке.

## Текущее состояние

Этот бутлоадер интегрирован и протестирован совместно с парным проектом
приложения:

- базовый адрес bootloader: `0x08000000`
- базовый адрес приложения: `0x08040000`
- сектор metadata: `0x08020000`
- staging-область: `0x08080000 .. 0x080BFFFF` (непрерывное окно 256 KB)
- сектор настроек приложения: `0x080C0000`

Проверенные сценарии:

- цикл warm reset `app -> bootloader -> app`
- повторные циклы перехода без выключения питания
- полный OTA update по `Modbus TCP`
- отклонение образа с неверным `CRC`
- восстановление после прерванной OTA-сессии
- длительный тест стабильности ping в bootloader

## Основные возможности

- bare-metal `LwIP` (`NO_SYS = 1`) с `Modbus TCP` сервером на порту `502`
- OTA update в режиме single-client
- staging, проверка `CRC32` и установка в flash-область приложения
- хранение служебного состояния в отдельном flash-секторе metadata
- программный запрос из приложения на вход в bootloader после reset
- recovery path для прерванной установки и прерванной загрузки

## Разметка flash

Определена в `Application/flash/flash_map.h`.

| Область | Адрес | Размер | Назначение |
|---|---:|---:|---|
| Bootloader | `0x08000000` | 128 KB | Сектора 0-4 |
| Metadata | `0x08020000` | 128 KB | Сектор 5, состояние OTA/update |
| Application | `0x08040000` | 256 KB | Сектора 6-7 |
| Staging | `0x08080000` | 256 KB | Сектора 8-9 |
| App settings | `0x080C0000` | 128 KB | Сектор 10, используется main app |

Образ приложения должен содержать структуру `fw_header_t` по смещению `0x200` от
`APP_FLASH_BASE`. Бутлоадер читает `product_id` и `hw_revision` из самого бинарника
(не только из того, что сказал инструмент обновления) в трёх точках:
`FINALIZE_UPDATE`, верификация при установке, `BOOT_VERIFY_APP`.
См. [Заголовок прошивки](#заголовок-прошивки) ниже.

Константы идентификации (дефолты для варианта 12-DI/D4MG, настраиваются через CMake):

| Константа | Значение | CMake-флаг |
|---|---|---|
| `PRODUCT_ID_DEFAULT` | `0x12D1D4A0` | `-DPRODUCT_ID=0x...` |
| `HW_REVISION_DEFAULT` | `1` | `-DHW_REVISION=N` |
| `BOOTLOADER_VERSION` | `1.0.0` | — |
| `FW_MAX_BLOCK_SIZE` | `240` байт | — |

## Сетевая конфигурация

Текущие сетевые настройки bootloader зашиты в `LWIP/App/lwip.c`:

- IP: `192.168.142.99`
- netmask: `255.255.255.0`
- gateway: `192.168.142.1`
- порт `Modbus TCP`: `502`
- `Modbus unit id`: `1`

Текущие ограничения:

- только статический IP
- DHCP в bootloader отсутствует
- изменение IP через `Modbus TCP` не реализовано
- одновременно поддерживается только один TCP-клиент

## Как работает вход в bootloader

Логика решения stay/boot реализована в `Application/boot/boot_entry.c`.

Bootloader остается активным, если выполняется хотя бы одно из условий:

1. приложение запросило вход в bootloader, записав `BOOT_REQUEST_MAGIC`
   в зарезервированную no-init ячейку RAM по адресу `0x2001FFF0`
2. установка была прервана
3. в metadata установлен флаг `install_requested`
4. прием прошивки был прерван и оставался в процессе
5. приложение помечено как невалидное
6. быстрая проверка векторов приложения не проходит

Программный запрос одноразовый: bootloader считывает RAM-флаг и очищает его при
входе.

## Машина состояний bootloader

Реализована в `Application/boot/boot_main.c`.

Основные состояния:

- `BOOT_START`
- `BOOT_CHECK_ENTRY`
- `BOOT_WAIT_COMMAND`
- `BOOT_RECEIVE_FW`
- `BOOT_VERIFY_STAGING`
- `BOOT_INSTALL_FW`
- `BOOT_VERIFY_APP`
- `BOOT_READY_TO_BOOT`
- `BOOT_ERROR`

Нормальный OTA-поток:

1. вход в `BOOT_WAIT_COMMAND`
2. получение `BEGIN_UPDATE`
3. стирание staging и переход в `BOOT_RECEIVE_FW`
4. прием всех блоков прошивки
5. `FINALIZE_UPDATE` проверяет `CRC32` staging-области **и** читает `fw_header_t` из самого бинарника
6. `INSTALL_UPDATE` копирует прошивку в область приложения и повторно проверяет заголовок
7. образ приложения проходит валидацию (CRC32 + заголовок прошивки)
8. bootloader передает управление приложению

## OTA-протокол по Modbus TCP

Обработчики OTA-регистров реализованы в `Application/modbus/fw_update_proto.c`.

Команды, записываемые в holding register `HR[0x0000]`:

- `1` - `BEGIN_UPDATE`
- `2` - `FINALIZE_UPDATE`
- `3` - `INSTALL_UPDATE`
- `4` - `ABORT_UPDATE`
- `5` - `REBOOT`

Часто используемые input-регистры:

- `IR[0x0000..0x0001]` - magic `0xB00710AD`
- `IR[0x0004]` - состояние bootloader
- `IR[0x0005]` - признак валидности приложения
- `IR[0x000B]` - последняя ошибка
- `IR[0x000C..0x000D]` - общее число блоков
- `IR[0x000E..0x000F]` - число принятых блоков
- `IR[0x0010..0x0011]` - размер образа
- `IR[0x0012..0x0013]` - `CRC32` образа
- `IR[0x0014]` - статус команды
- `IR[0x0015]` - признак валидности staging

Значения статуса команды:

- `0` - `IDLE`
- `1` - `BUSY`
- `2` - `OK`
- `3` - `ERROR`

Известные коды ошибок:

- `1` - `PRODUCT_MISMATCH`
- `2` - `HW_REV_MISMATCH`
- `3` - `IMAGE_TOO_LARGE`
- `4` - `BLOCK_CRC`
- `5` - `IMAGE_CRC`
- `6` - `FLASH_ERASE`
- `7` - `FLASH_WRITE`
- `8` - `APP_VALIDATE`
- `9` - `UPDATE_TIMEOUT`
- `10` - `BLOCK_INDEX`
- `11` - `BAD_PARAMS`

## OTA-клиент

Опорный CLI-клиент: `tools/fw_update.mjs`. Легаси-версия на Python всё ещё
доступна как `tools/fw_update.py`.

Базовое использование:

```bash
node tools/fw_update.mjs status
node tools/fw_update.mjs update path/to/app.bin
node tools/fw_update.mjs abort
node tools/fw_update.mjs reboot
node tools/fw_update.mjs app-bootloader
```

Параметры по умолчанию:

- IP bootloader: `192.168.142.99`
- IP приложения: `192.168.142.98`
- порт: `502`
- unit id: `1`

Полезные опции:

- `--boot-ip` / `--ip` — переопределить адрес bootloader
- `--app-ip` — переопределить адрес приложения

## Вход в bootloader из основного приложения

Парное приложение входит в bootloader, записывая `0xB007` в holding register
`118`, после чего делает warm reset с сохранением общего RAM-флага.

Пример с `pymodbus`:

```python
from pymodbus.client import ModbusTcpClient

client = ModbusTcpClient("192.168.142.98", port=502, timeout=5)
client.connect()
client.write_register(address=118, value=0xB007, device_id=1)
client.close()
```

В протестированной конфигурации:

- main app отвечает на `192.168.142.98`
- bootloader отвечает на `192.168.142.99`

## Замечания по восстановлению

Во время тестирования были подтверждены такие рабочие детали:

- после обычной сессии `BOOT_WAIT_COMMAND` команда `REBOOT` возвращает
  устройство в main app
- после прерванной или неудачной OTA-сессии перед возвратом в приложение нужно
  сначала выполнить `ABORT_UPDATE`, а затем `REBOOT`
- неудачный `FINALIZE_UPDATE` из-за ошибки `CRC` оставляет `staging_valid = 0`
- прерванная загрузка оставляет bootloader в безопасном состоянии и не позволяет
  запустить неполный образ приложения

## Исправление Ethernet reset path

Путь warm reset зависит от корректной переинициализации Ethernet-периферии.

В текущей реализации bootloader сначала включает ETH clocks, а уже потом делает
force reset MAC в `LWIP/Target/ethernetif.c`.
Это исключает зависание в stale состоянии `MAC/DMA` после переходов
`bootloader -> app` и `app -> bootloader`.

## Сборка

Система сборки проекта: `CMake + Ninja`.

Типовая локальная сборка:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build
```

Выходные файлы:

- `BOOTLOADER_PLCJS.elf`
- `BOOTLOADER_PLCJS.hex`
- `BOOTLOADER_PLCJS.bin`

## Сборка для вариантов модулей

`PRODUCT_ID_DEFAULT` и `HW_REVISION_DEFAULT` защищены `#ifndef` в `Application/flash/flash_map.h`
и переопределяются через CMake:

```bash
# 12 дискретных входа / 4 выхода (дефолт)
cmake -S . -B build/12di -DPRODUCT_ID=0x12D1D4A0 -DHW_REVISION=1

# 12 дискретных выхода
cmake -S . -B build/12do -DPRODUCT_ID=0x12D00000 -DHW_REVISION=1

# 4 входа RTD
cmake -S . -B build/4rtd -DPRODUCT_ID=0x04D00000 -DHW_REVISION=1

# 8 аналоговых входов
cmake -S . -B build/8ai -DPRODUCT_ID=0x08A10000 -DHW_REVISION=1

# 8 аналоговых выходов
cmake -S . -B build/8ao -DPRODUCT_ID=0x08A00000 -DHW_REVISION=1
```

Каждый результирующий `BOOTLOADER_PLCJS.bin` будет принимать OTA-образы
только с совпадающими `fw_header_t.product_id` и `hw_revision`.
Загрузка прошивки для неподходящего модуля отклоняется
при `FINALIZE_UPDATE` — до записи в область приложения.

## Заголовок прошивки

Начиная с этого релиза, валидные образы приложения должны встраивать `fw_header_t`
по адресу `APP_FLASH_BASE + 0x200` (смещение `0x200` в `app.bin`).

Цепочка валидации (три независимые проверки):

| Точка проверки | Файл | Что проверяется |
|---|---|---|
| `FINALIZE_UPDATE` | `fw_update_proto.c` | заголовок в **staging** flash |
| `INST_VERIFYING` | `fw_installer.c` | заголовок в **app** flash после копирования |
| `BOOT_VERIFY_APP` | `boot_main.c` | заголовок в **app** flash перед передачей |

При ошибке проверки возвращается код ошибки `PRODUCT_MISMATCH` (1),
бутлоадер переходит в состояние `BOOT_ERROR`.

## Известные ограничения

- IP bootloader фиксирован и зашит в код
- watchdog в bootloader не включен
- аутентификация и контроль доступа для OTA-команд отсутствуют
- одновременно может быть подключен только один `Modbus TCP` клиент
- staging-область ограничена `256 KB`

## Связанные репозитории

- bootloader repo: этот репозиторий
- парный repo основного приложения: `PLCJS_ETH_MODULE_12DI_D4MG_STM32F407VGT6`
