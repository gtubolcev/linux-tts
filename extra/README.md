# Extra modules

Not built by the packaging and not in the PPA. Each directory builds on its own
(`build-deb.sh`, in docker) for those who want to try it.

- `zonos3/` — `sd_zonos3`: [ZONOS3-Mini](https://github.com/Zyphra/ZONOS3-Mini) through
  its server (zonos3.cpp, ggml, Vulkan with CPU fallback). Taken out of the packages on
  2026-11-14: the model invents or drops words in about a quarter of the phrases. A good
  candidate for a heavier voice if that is solved. `Zonos3Gpu 1` in `zonos3.conf` runs it
  on the graphics card (the settings app no longer has a check box for it).
