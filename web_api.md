# ESP3D HTTP API

Документ описывает HTTP-обработчики, зарегистрированные в исходном коде
ESP3D. Это не описание WebSocket, Telnet, FTP или MKS raw-TCP: они используют
другие транспортные протоколы.

## Общие правила

Основной HTTP-сервер запускается, если определён `HTTP_FEATURE`, а настройка
`ESP_HTTP_ON` равна `1`. Его порт — `ESP_HTTP_PORT` (по умолчанию `80`).
Маршруты, отмеченные `HTTP_ANY`, не ограничивают метод на уровне роутера;
семантика определяется обработчиком.

При `AUTHENTICATION_FEATURE` защищённые обработчики используют HTTP-сессию
`ESPSESSIONID` либо, для BFT на ESP8266, также Basic authentication. Ответ
неавторизованного обычного обработчика обычно имеет статус `401` и тело
`Wrong authentication!`.

Основной сервер добавляет к обычным ответам `Cache-Control: no-cache`,
`User-Agent` и `Host`; при `ESP_ACCESS_CONTROL_ALLOW_ORIGIN` добавляется
`Access-Control-Allow-Origin: *`.

| Условие | Значение |
| --- | --- |
| `HTTP_FEATURE` | Включает основной HTTP-сервер и базовые маршруты. |
| `FILESYSTEM_FEATURE` | Добавляет `/files` и раздачу файлов из flash FS. |
| `SD_DEVICE` | Добавляет `/sdfiles` и раздачу `/sd/...`. |
| `WEB_UPDATE_FEATURE` | Добавляет `/updatefw`. |
| `CAMERA_DEVICE` | Добавляет `/snap`. |
| `SSDP_FEATURE` | Добавляет `/description.xml`. |
| `CAPTIVE_PORTAL_FEATURE` + AP setup | Добавляет captive-portal URL. |
| `COMMUNICATION_PROTOCOL == RAW_SERIAL` | Добавляет BFT `/upload` и `/print` на ESP8266/ESP32. |
| `COMMUNICATION_PROTOCOL == MKS_SERIAL` | Вместо BFT-`/upload` добавляет MKS frame upload. |

## Основной сервер

### `GET|POST|… /`

Возвращает web UI. Сначала ищет `${ESP3D_HOST_PATH}/index.html.gz`, затем
`index.html` во flash FS; gzip-вариант сопровождается
`Content-Encoding: gzip`. Если файла нет, выдаётся встроенная сжатая страница.

Наличие параметра `forcefallback` (обычно `forcefallback=yes`) принудительно
выбирает встроенную страницу даже при наличии `index.html`.

### `GET|POST|… /login`

Проверка, создание и удаление HTTP-сессии.

| Параметры | Действие |
| --- | --- |
| отсутствуют | Возвращает текущий уровень аутентификации. |
| `SUBMIT`, `USER`, `PASSWORD` | Вход под `admin` или `user`; при успехе устанавливает cookie `ESPSESSIONID`. |
| `TIMEOUT` | Время жизни новой сессии при входе. |
| `NEWPASSWORD` | Меняет пароль admin после успешной проверки учётных данных. |
| `DISCONNECT=YES` | Удаляет текущую сессию и сбрасывает cookie. |

Ответ — JSON вида `{"status":"ok","authentication_lvl":"admin"}`.
При выключенном `AUTHENTICATION_FEATURE` всегда отвечает HTTP 200 с уровнем
`admin`.

### `GET|POST|… /command`

Передаёт команду контроллеру либо ядру ESP3D.

| Параметры | Действие |
| --- | --- |
| `cmd=<G-code или [ESP…]>` | Непосредственно отправляет G-code целевой прошивке. Команда `[ESP…]` обрабатывается ESP3D и её ответ формируется стандартным диспетчером команд. |
| `ping` | HTTP 200 с пустым телом. |

Для G-code без завершающего LF добавляется LF, кроме realtime-команд GRBL и
grblHAL. Для обычного G-code немедленный ответ: `200 text/plain`,
`ESP3D says: command forwarded`. Отсутствие обоих параметров: `400 Invalid command`.

Маршрут требует аутентификацию, если она включена.

### `GET|POST|… /config`

Запускает внутреннюю команду `[ESP420]addPreTag`. Необязательный параметр
`json` передаётся как `json=<значение>`. Формат и права ответа определяет
обработчик команды ESP420; маршрут передаёт ему текущий уровень HTTP-сессии.

### `GET|POST|… /files`

Доступен при `FILESYSTEM_FEATURE`. Это одновременно список и менеджер файлов
flash FS; требуется не-гостевая сессия.

| Параметры | Значение |
| --- | --- |
| `path` | Каталог для листинга; по умолчанию `/`. |
| `action=delete`, `filename` | Удаляет файл. |
| `action=deletedir`, `filename` | Удаляет каталог. |
| `action=createdir`, `filename` | Создаёт каталог. |
| `quiet=yes` | Возвращает только статус upload (`{"status":"…"}`). |

Без `quiet` ответ — потоковый JSON с `files`, `path`, `occupation`, `status`,
`total` и `used`. Каждый объект `files` содержит как минимум `name` и `size`;
каталог кодируется размером `-1`.

`POST` с multipart body вызывает загрузчик flash FS. Имя файла берётся из
multipart-part. Поддерживаются `rpath` для префикса назначения, `path`,
`createPath=true` для создания промежуточных каталогов и параметр
`<upload_filename>S` для проверки размера/свободного места. При ошибке
частично записанный файл удаляется.

### `GET|POST|… /sdfiles`

Доступен при `SD_DEVICE`; аналогичен `/files`, но работает с SD-картой.
Поддерживает `path`, `action=delete|deletedir|createdir|list`, `filename` и
`quiet=yes`. Полный ответ — JSON со списком, статистикой места и статусом;
если носитель недоступен, возвращается `{"status":"not available"}` или
`{"status":"no SD card"}` с HTTP 200.

Multipart `POST` загружает файл на SD. Параметры те же, что у `/files`:
`rpath`, `path`, `createPath=true` и `<upload_filename>S`. Маршрут требует
не-гостевую сессию.

### `GET|POST|… /updatefw`

Доступен при `WEB_UPDATE_FEATURE`, только admin. Multipart `POST` записывает
прошивку через Arduino `Update`; необязательный параметр
`<upload_filename>S` ограничивает ожидаемый размер.

Основной обработчик возвращает JSON статуса: `{"status":"no file"}`,
`{"status":"canceled"}`, `{"status":"ok"}` или `{"status":"error"}`.
После `ok` устройство ожидает секунду и перезагружается. Любая не-admin
запрос получает HTTP 401.

### `GET /snap`

Доступен при `CAMERA_DEVICE`, требует не-гостевую сессию. Делегируется
`esp3d_camera.handle_snap()`: она формирует снимок камеры и его тип контента.

### `GET /description.xml`

Доступен при `SSDP_FEATURE`; возвращает SSDP device schema с
`Content-Type: text/xml`.

### Captive-portal URL

Только при `CAPTIVE_PORTAL_FEATURE` в режиме AP setup следующие маршруты
вызывают тот же обработчик, что и `/`:

- `GET|POST|… /generate_204`
- `GET|POST|… /gconnectivitycheck.gstatic.com`
- `GET|POST|… /fwlink/`

### Статические файлы и fallback

Все незарегистрированные URL попадают в `handle_not_found`.

1. При `FILESYSTEM_FEATURE` выдаётся файл из flash FS, предпочтительно
   `<path>.gz`, затем `<path>`; `/favicon.ico` отдаётся из встроенного ресурса.
2. При `SD_DEVICE` URL `/sd/<path>` выдаёт файл с SD-карты.
3. Если существует `/404.htm` или `/404.htm.gz` во flash FS, возвращается он.
4. Иначе ответ — HTTP 404 с пустым телом.

Этот fallback требует не-гостевую сессию, когда аутентификация включена.

## Загрузка BFT для RAW_SERIAL

Доступно только при `COMMUNICATION_PROTOCOL == RAW_SERIAL` на ESP8266 или
ESP32. Имя должно быть именем файла в корне SD: без `/`, `\\`, символов
`:*?"<>|` и управляющих символов; лимит 120 байт. Передача выполняется через
Marlin Binary File Transfer, а не в FS ESP3D.

### `POST /upload?X-Filename=<filename>[&print=1]`

Тело — необработанные байты G-code. Нужен `Content-Length`; chunked encoding
не поддержан. Успех: HTTP 200, `{"err":0}`. Ошибка: HTTP 400,
`{"err":1}`; у ESP8266 детализация передаётся заголовком
`X-ESP3D-BFT-Error`.

`print=1` или `print=true` после успешной передачи запускает файл. На ESP8266
запуск выполняет `M21`, `M23 <filename>`, `M24`; ESP32 использует тот же BFT
service. Неудачный автозапуск возвращает ошибку upload.

На ESP8266 загрузчик реализован hook-обработчиком и поддерживает Basic auth
или cookie. На ESP32 используется streaming `HTTPRaw`; поддерживается только
raw body, не multipart.

### `POST /print?X-Filename=<filename>`

Выбирает уже загруженный корневой файл и запускает печать, дожидаясь ответов
Marlin. Успех: `200 application/json`, `{"err":0,"started":true}`.
Ошибки: `400` (`409`, если BFT активен) и `{"err":1}`; детализация передаётся
в `X-ESP3D-Print-Error`. Нужна аутентифицированная сессия, если включён
`AUTHENTICATION_FEATURE`.

## MKS upload

При `COMMUNICATION_PROTOCOL == MKS_SERIAL` `/upload` не является BFT API.
Маршрут принимает обычную multipart-загрузку, разбивает файл на MKS frames и
передаёт их MKS-прошивке принтера. Имя файла берётся из multipart body;
необязательные `rpath` и `s<filename>` задают удалённый путь и размер. Успех:
`200 text/plain`, `{"status":"ok"}`; ошибка загрузки: HTTP 500.

## Отдельный WebDAV сервер

WebDAV — отдельный HTTP-сервер, не набор маршрутов основного сервера. Он
запускается при `WEBDAV_FEATURE` и `ESP_WEBDAV_ON=1` на `ESP_WEBDAV_PORT`
(по умолчанию `8181`). Код WebDAV не использует `AuthenticationService`; не
публикуйте этот порт в недоверенной сети.

Базовый URL — `/`; пространство имён и доступный носитель выбираются
значением `WEBDAV_FEATURE` (`FS_ROOT`, `FS_FLASH` либо `FS_SD`). Корень нельзя
использовать для GET/HEAD/PUT/DELETE/MKCOL/LOCK/UNLOCK.

| Метод | Обработчик и важные заголовки | Типичные статусы |
| --- | --- | --- |
| `OPTIONS` | Возвращает DAV/Allow-заголовки. | 200 |
| `GET` | Скачивает существующий файл; отдаёт `Last-Modified`, `Content-Length`, `Content-Type`. | 200, 400, 404, 500, 503 |
| `HEAD` | Метаданные существующего файла без тела. | 200, 400, 404, 500, 503 |
| `PUT` | Создаёт или перезаписывает файл. Требуется `Content-Length`. | 201, 204, 400, 404, 409, 507 |
| `DELETE` | Удаляет файл или каталог. | 204, 400, 404, 500, 503 |
| `COPY` | Копирует файл; требуются `Destination`, опционален `Overwrite: T`. Источник и назначение должны быть на одном FS. | 201, 204, 400, 404, 409, 412, 413, 500, 507 |
| `MOVE` | Перемещает файл; требуются `Destination`, опционален `Overwrite: T`. | 201, 204, 400, 404, 409, 412, 413, 500, 507 |
| `MKCOL` | Создаёт каталог. | 201, 400, 404, 409, 500, 503 |
| `PROPFIND` | Возвращает WebDAV multistatus; учитывает `Depth`. | 207, 400, 404, 500, 503 |
| `PROPPATCH` | Делегирует `PROPFIND`; изменения свойств не сохраняются. | 207 |
| `LOCK` | Создаёт временный lock-token, возвращает `Lock-Token`. | 200, 400, 404, 423, 500, 503 |
| `UNLOCK` | Удаляет lock-token. | 204, 400, 404, 423, 500, 503 |

Неизвестный WebDAV-метод возвращает `405 Method Not Allowed`. Во все WebDAV
ответы добавляются `DAV: 1`, `Allow`, `Connection: close` и
`Cache-Control: no-cache`.

## Исходники маршрутов

- Регистрация основного сервера: `esp3d/src/modules/http/http_server.cpp`.
- Базовые обработчики: `esp3d/src/modules/http/handlers/`.
- BFT HTTP: `esp3d/src/modules/bft/binary_file_transfer_http.cpp`.
- WebDAV router: `esp3d/src/modules/webdav/webdav_server.cpp` и
  `esp3d/src/modules/webdav/handlers/`.
