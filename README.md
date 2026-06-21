<div align="center">

# Nimbus

A weather app for [Leaf](https://leaf.game) on the Miniloong Pocket 1.

</div>

Nimbus shows current conditions, a forecast, hourly weather, and sun/moon info on
your handheld. This is the **Leaf** port of the original
[NextUI Nimbus](https://github.com/ericreinsmidt/nextui-nimbus): rebuilt
Catastrophe-native, with weather from [Open-Meteo](https://open-meteo.com) so it
needs **no account and no API key** — it just works on first launch.

## Status

**Planning.** The port plan lives in [PLAN.md](PLAN.md). Nothing is built yet.

## Stack

- [Catastrophe](https://github.com/Utility-Muffin-Research-Kitchen) UI toolkit (no PakKit)
- Weather + geocoding via [Open-Meteo](https://open-meteo.com) (keyless), libcurl + cJSON
- Packaged as a Leaf `.pak`, staged like the other Leaf apps

## License

MIT — see [LICENSE](LICENSE). Weather data by [Open-Meteo](https://open-meteo.com) (CC-BY 4.0).
