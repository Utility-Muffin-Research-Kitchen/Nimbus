# Nimbus on Leaf — port plan

## What this is

**Nimbus** is a weather app, originally built for **NextUI** on TrimUI handhelds
(by Eric Reinsmidt — [ericreinsmidt/nextui-nimbus](https://github.com/ericreinsmidt/nextui-nimbus),
MIT). This repo is the **Leaf / Miniloong Pocket 1 (MLP1)** port: Catastrophe-native,
no PakKit, and moved off the API-key weather backend so it works with **zero setup**.

- **Source stack (NextUI):** Apostrophe UI + PakKit (header-only widget helper) +
  cJSON + libcurl + qrcodegen, weather from **WeatherAPI.com** (needs a per-user API key).
- **Target stack (Leaf):** **Catastrophe** UI + cJSON + libcurl, weather from
  **Open-Meteo** (no key, no account). Shipped as a `.pak`, staged into Leaf the same
  way as Joe's Calibrage and Fugazi.

## Why these changes

### 1. Weather backend: WeatherAPI.com → Open-Meteo (the big win)

WeatherAPI.com requires a per-user API key. The entire QR-code + embedded-web-server +
`api_key.txt` flow in the original exists for one reason: to let a user enter that key
without a keyboard (scan a QR, open a phone browser, paste the key). [Open-Meteo](https://open-meteo.com)
needs **no key and no account**, so the switch lets us:

- **Delete** qrcodegen, the socket HTTP server, `load/save_api_key`, the HTML setup
  page, and the whole `show_api_key_setup` flow.
- Ship an app that works on **first launch with zero setup** — better adoption, less code.
- Use Open-Meteo's free, keyless **geocoding** API for city search; the data model
  already stores `lat`/`lon` per location.
- Stay free-only: Open-Meteo is free for non-commercial use under CC-BY 4.0 (attribution),
  which fits Leaf's no-sale stance.

### 2. Drop PakKit → Catastrophe-native

PakKit is a NextUI-era header helper. Leaf's identity is Catastrophe. Reimplement the
handful of PakKit widgets on Catastrophe, and **add to Catastrophe anything missing**
rather than vendoring PakKit.

### 3. Apostrophe → Catastrophe

Mechanical `ap_*` → `cat_*` swap, the same path that landed Joe's Calibrage.
(Gotcha: `ap_color` → `cat_draw_color`, **not** `cat_color`, which is a `uint32`.)

## Feature set to preserve

Four tabs + locations + cache:

- **Current** — temperature, feels-like, humidity, wind, precipitation, cloud cover, UV index
- **Forecast** — 3-day daily high/low, condition, chance of rain, weather icons
- **Hourly** — hour-by-hour temperature / condition / rain chance
- **Astro** — sunrise, sunset, moon phase
- **Locations** — up to 10 saved; on-screen-keyboard search to add (city → geocode → lat/lon)
- **Offline cache** — per-location JSON, "cached data" indicator, Wi-Fi retry/continue on launch
- **°C / °F** toggle

## Open-Meteo field mapping

| Nimbus data | WeatherAPI.com | Open-Meteo |
|---|---|---|
| temp / feels-like | `current.temp_c` / `feelslike_c` | `current`: `temperature_2m` / `apparent_temperature` |
| humidity / wind / precip / cloud | `current.*` | `current`: `relative_humidity_2m` / `wind_speed_10m` / `precipitation` / `cloud_cover` |
| UV index | `current.uv` | `current` / `daily`: `uv_index` / `uv_index_max` |
| condition icon | `condition.code` (WeatherAPI codes) | `weather_code` (WMO codes) → remap icon table |
| daily hi/lo, rain % | `forecastday.day.*` | `daily`: `temperature_2m_max/min`, `precipitation_probability_max` |
| hourly | `forecastday.hour[]` | `hourly`: `temperature_2m`, `weather_code`, `precipitation_probability` |
| sunrise / sunset | `astro.*` | `daily`: `sunrise` / `sunset` |
| **moon phase** | `astro.moon_phase` | **not provided → compute locally from the date** (lunar-age / synodic-month formula, keyless) |
| geocode (city → lat/lon) | `search.json` (key) | `geocoding-api.open-meteo.com/v1/search` (keyless) |

Two notes:

- **Moon phase** is the only data gap. Open-Meteo doesn't supply it; compute it locally
  from the date (lunar age over the ~29.53-day synodic month). Keeps the Astro tab whole
  with no extra API or key. Accuracy is ±~1 day, fine for a phase icon.
- The **icon remap** is the main translation effort: WeatherAPI condition codes →
  WMO weather codes. Group the ~28 WMO codes into the existing icon buckets
  (clear / cloudy / rain / snow / thunder / fog), with day/night via `is_day`.

## PakKit → Catastrophe widget map

| PakKit | Catastrophe equivalent |
|---|---|
| `pakkit_list` / `list_item` / `list_opts` | Catastrophe list state + row render (browse-page pattern) |
| `pakkit_menu` / `menu_item` | small Catastrophe menu (or a list) |
| `pakkit_scroll_state` / `handle_input` / `animate` / `update` | `cat_scroll_state` + `cat_draw_scroll_view` (already used for the launcher About page) |
| `pakkit_hint` / `draw_hints` | Catastrophe footer hints (`cat_footer_item`) |
| `pakkit_message` | **add** `cat_message` — a simple centered dialog |
| `pakkit_confirm` | **add** `cat_confirm` — a yes/no dialog |
| PakKit keyboard (location search) | Catastrophe on-screen keyboard (already in Leaf for Wi-Fi / search) |

`cat_message` / `cat_confirm` go **into Catastrophe** (the "add what we need" call) so every
app gets them.

## Phases

0. **Repo + scaffold** *(this doc)* — create this repo, MIT / © Eric Reinsmidt, README,
   `.gitignore`, fork the app source in.
1. **De-NextUI the core** — drop PakKit / qrcodegen / web-server; Apostrophe → Catastrophe;
   get a window up on the Mac/desktop build first (stub weather) before touching the device.
2. **Open-Meteo backend** — swap fetch + JSON parse to Open-Meteo (current / daily / hourly),
   WMO → icon remap, local moon-phase calc, keyless geocoding for search. Keep the offline cache.
3. **Catastrophe-native screens** — Current / Forecast / Hourly / Astro tabs + Locations +
   settings, themed in Leaf chrome (theme, fonts, footer). Land `cat_message` / `cat_confirm`
   in Catastrophe.
4. **MLP1 port + Leaf integration** — `ports/mlp1`, the pak build (Fugazi / Calibrage pattern),
   icon, `stage-app` wiring, on-device validation on Puff.
5. **Docs + release** — leaf-docs app-store page, a public release (Disco Boy pattern), and add
   it to the app-store list.

## Build / deploy (target)

- Mirrors Joe's Calibrage / Fugazi: a `ports/mlp1` Makefile building against sibling
  **Catastrophe** through the `mlp1-toolchain` Docker image; `make package-mlp1` →
  `build/mlp1/package/Nimbus.pak`; staged with Leaf `make stage-app APP=Nimbus DEVICE=mlp1`.
- **Network:** libcurl (present on MLP1; the launcher's OTA uses it). cJSON vendored, as
  Jawaka / Catastrophe do.
- **App-store app** — not bundled in the base Leaf install (like Disco Boy); downloaded
  from its own release.

## Risks / edge cases

- **Moon phase** — local lunar-age formula is approximate (±1 day). Fine for a phase icon;
  document the method in the code.
- **Icon remap** — make sure every WMO bucket maps to an existing icon (+ day/night).
- **Open-Meteo availability** — generous free tier; the offline cache (already designed in)
  softens outages. Respect CC-BY attribution somewhere in the app/about.
- **Units** — Open-Meteo can return °F / mph directly via request params, or we convert.
  Keep the °C / °F toggle; default °F (US audience).
- **On-screen keyboard** — reuse Leaf's Catastrophe keyboard; it's used by the launcher, so
  it may need a small Catastrophe API surface to be callable from an app.

## Naming / repo

- Repo: `Utility-Muffin-Research-Kitchen/Nimbus` — public, MIT, © Eric Reinsmidt; sibling to
  DiscoBoy / Fugazi.
- Keep the name **Nimbus** (weather-apt, clean; no Zappa tie needed). `pak.json` `name: "Nimbus"`,
  platform `mlp1`.
- The original stays at `ericreinsmidt/nextui-nimbus` (TrimUI / NextUI line); this is the Leaf line.
