"""Microphone list: one entry per physical device, full names, no pseudo-devices."""
from fethr.core.audio import _pick_devices

HOSTAPIS = [{"name": "MME"}, {"name": "Windows DirectSound"}, {"name": "Windows WASAPI"},
            {"name": "Windows WDM-KS"}]


def dev(name, api, inputs=2):
    return {"name": name, "hostapi": api, "max_input_channels": inputs, "max_output_channels": 0}


RAW = [
    dev("Microsoft Sound Mapper - Input", 0),
    dev("Microphone (Yeti Stereo Microph", 0),          # MME truncates at 31 chars
    dev("Headset (Jabra Evolve2 65)", 0),
    dev("Speakers (Realtek)", 0, inputs=0),
    dev("Primary Sound Capture Driver", 1),
    dev("Microphone (Yeti Stereo Microphone)", 1),
    dev("Headset (Jabra Evolve2 65)", 1),
    dev("Microphone (Yeti Stereo Microphone)", 2),
    dev("Headset (Jabra Evolve2 65)", 2),
    dev("Microphone (Yeti Stereo Microphone)", 3),
    dev("Headset Microphone (Jabra Evolve2 65)", 3),
]


def test_one_entry_per_mic_from_wasapi():
    out = _pick_devices(RAW, HOSTAPIS, default_index=1)
    assert [d["name"] for d in out] == [
        "Microphone (Yeti Stereo Microphone)", "Headset (Jabra Evolve2 65)"]
    assert out[0]["index"] == 7 and out[1]["index"] == 8        # the WASAPI rows


def test_default_badge_follows_the_truncated_mme_default():
    out = _pick_devices(RAW, HOSTAPIS, default_index=1)
    assert [d["default"] for d in out] == [True, False]


def test_falls_back_to_next_api_when_wasapi_absent():
    raw = [d for d in RAW if d["hostapi"] != 2]
    out = _pick_devices(raw, HOSTAPIS, default_index=None)
    assert [d["name"] for d in out] == [
        "Microphone (Yeti Stereo Microphone)", "Headset (Jabra Evolve2 65)"]
    assert all(raw[d["index"]]["hostapi"] == 1 for d in out)


def test_pseudo_devices_and_outputs_dropped_and_empty_ok():
    assert _pick_devices([], HOSTAPIS, None) == []
    only_pseudo = [dev("Microsoft Sound Mapper - Input", 0), dev("Speakers", 2, inputs=0)]
    assert _pick_devices(only_pseudo, HOSTAPIS, 0) == []


def test_linux_style_single_api_untouched():
    apis = [{"name": "ALSA"}]
    raw = [dev("default", 0), dev("hw:0,0", 0)]
    assert [d["name"] for d in _pick_devices(raw, apis, 0)] == ["default", "hw:0,0"]
