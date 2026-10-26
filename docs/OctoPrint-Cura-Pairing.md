# OctoPrint status API and slicer pairing (ESP32, RAW_SERIAL)

Besides `GET /api/version` and `POST /api/files/local` (see
[OctoPrint-BFT-API.ru.md](OctoPrint-BFT-API.ru.md)), ESP3D answers the
OctoPrint calls Cura's *OctoPrint Connection* plugin needs, and OctoPrint's
Application Keys flow behind the plugin's **Request...** button.

## Status endpoints

All need `X-Api-Key` (ESP3D user/admin password or a paired key) or a WebUI
login.

| Endpoint | What it does |
|---|---|
| `GET /api/printer` | `tool0`/`bed` temperatures and state flags. Sends `M105` when the last reading is older than 5 s. |
| `GET /api/job` | `Operational`, `Printing` or `Paused`; SD progress, elapsed time, file name. Sends `M27` / `M27 C` when needed. |
| `POST /api/job` | `pause` (action `pause`/`resume`/`toggle`) → `M25`/`M24`; `cancel` → `M524`. |
| `POST /api/printer/command` | `{"command":...}` or `{"commands":[...]}`, sent to Marlin as is (ESP3D `[ESP...]` commands are refused). |
| `POST /api/files/local/<name>` | `{"command":"select","print":true}` selects (`M23`) and optionally starts the file. |
| `GET /api/settings`, `GET /api/printerprofiles`, `POST /api/login`, `/api/connection` | Fixed answers so Cura connects. |

The state comes from Marlin's replies as they pass through ESP3D (`T:`/`B:`
temperatures, `SD printing byte`, `Not SD printing`, `Done printing file`,
`Current file:`, and the host prompts `Pause` / `Resuming`). The upload answer
now carries `Location: http://<host>/api/files/local/<name>`. ESP3D also
announces `_octoprint._tcp` over mDNS so Cura can discover it.

## Pairing

Off by default: setting **slicer pairing** in Security (`ESP_OCTOPRINT_PAIRING`,
position 1258), e.g. `[ESP401]P=1258 T=B V=1`.

1. `GET /plugin/appkeys/probe` → 204 when pairing is on, else 404.
2. `POST /plugin/appkeys/request` `{"app":"Cura"}` → 201, `Location` to poll,
   `auth_dialog` = the WebUI.
3. `GET /plugin/appkeys/request/<app_token>` → 202 while pending, 200
   `{"api_key":...}` once, 404 when denied or expired (after 2 minutes).

A request is approved in either place:

- **WebUI**: ESP3D sends `PAIRING:<user_token>:<app>` on the WebUI websocket
  (repeated every 5 s while pending, `PAIRING:done:<user_token>` when decided).
  The WebUI posts `{"decision":true|false}` to
  `/plugin/appkeys/decision/<user_token>`; this needs a WebUI login.
  `GET /plugin/appkeys/pending` lists open requests.
- **Printer display**: when no SD job is running, ESP3D sends
  `M0 S30 Pair <app>? Click`. Clicking the knob within 29 s approves; letting
  it time out leaves the request to the WebUI. ESP3D recognises the click by
  Marlin's `//action:prompt_begin Pair ...` line (needs `HOST_PROMPT_SUPPORT`)
  followed by M0's `ok` before the timeout. A decision made in the WebUI while
  the prompt is up ends it with `M108`.

Keys are 32 hex characters, stored in NVS (namespace `octokeys`, up to 4; a new
key for the same app name replaces the old one). They work only as
`X-Api-Key` for the OctoPrint endpoints, with user rights.
`GET /api/plugin/appkeys` lists them (admin login) and
`POST /api/plugin/appkeys` `{"command":"revoke","id":N}` removes one. The WebUI
shows them under Settings → ESP3D → slicer keys.
