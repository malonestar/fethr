"""Pure-logic tests for the live-transcript overlay (no GUI, no display)."""

from __future__ import annotations

import re
from types import SimpleNamespace

import pytest

from fethr.core.settings import Settings
from fethr.ui.overlay import (
    HIDE,
    OVERLAY_HTML,
    OVERLAY_MAX_WIDTH,
    OVERLAY_MIN_HEIGHT,
    SCHEDULE,
    SHOW,
    SMOKE_LONG_EVENTS,
    OverlayLogic,
    OverlayManager,
    overlay_position,
    overlay_size,
    supported_kwargs,
)


def state(name: str) -> dict:
    return {"type": "state", "state": name, "detail": ""}


def live(final: bool = False) -> dict:
    event = {"type": "live", "final": final, "mode": "raw",
             "transcript": {"sentences": [], "pending": "", "tentative": ""}}
    if final:
        event["text"] = "hello"
    return event


# ---------------------------------------------------------------- show/hide --


def test_recording_shows_and_final_state_hides_after_delay():
    logic = OverlayLogic(delay=0.6)
    assert logic.feed(state("recording"), True, 0.0) == SHOW
    assert logic.feed(live(), True, 0.1) is None
    assert logic.feed(state("transcribing"), True, 1.0) is None
    assert logic.feed(state("pasted"), True, 2.0) == SCHEDULE
    assert logic.deadline == pytest.approx(2.6)
    assert not logic.due(2.5)
    assert logic.due(2.6)
    assert not logic.visible
    assert not logic.due(3.0)            # fires once only


def test_hide_waits_for_the_later_of_terminal_state_and_final_live():
    logic = OverlayLogic(delay=0.6)
    logic.feed(state("recording"), True, 0.0)
    logic.feed(state("pasted"), True, 1.0)            # -> 1.6
    logic.feed(live(final=True), True, 1.4)           # -> 2.0, later wins
    assert logic.deadline == pytest.approx(2.0)
    assert not logic.due(1.7)
    assert logic.due(2.0)


def test_an_earlier_event_never_pulls_the_deadline_in():
    logic = OverlayLogic(delay=0.6)
    logic.feed(state("recording"), True, 0.0)
    logic.feed(live(final=True), True, 5.0)           # -> 5.6
    logic.feed(state("idle"), True, 4.0)              # clock skew: stays 5.6
    assert logic.deadline == pytest.approx(5.6)


def test_error_and_idle_also_hide():
    for terminal in ("error", "idle"):
        logic = OverlayLogic(delay=0.6)
        logic.feed(state("recording"), True, 0.0)
        assert logic.feed(state(terminal), True, 1.0) == SCHEDULE
        assert logic.due(1.6)


def test_new_recording_cancels_a_pending_hide():
    logic = OverlayLogic(delay=0.6)
    logic.feed(state("recording"), True, 0.0)
    logic.feed(state("pasted"), True, 1.0)
    assert logic.feed(state("recording"), True, 1.2) is None   # already up
    assert logic.deadline is None
    assert not logic.due(5.0)
    assert logic.visible


def test_events_while_hidden_do_nothing():
    logic = OverlayLogic()
    assert logic.feed(state("idle"), True, 0.0) is None
    assert logic.feed(live(final=True), True, 0.0) is None
    assert logic.feed(live(), True, 0.0) is None
    assert logic.feed(state("pasted"), True, 0.0) is None
    assert not logic.visible


def test_disabled_never_shows_and_disabling_hides_at_once():
    logic = OverlayLogic()
    assert logic.feed(state("recording"), False, 0.0) is None
    assert not logic.visible

    assert logic.feed(state("recording"), True, 1.0) == SHOW
    assert logic.feed(live(), False, 1.1) == HIDE
    assert not logic.visible
    assert logic.disable() is None


def test_manager_respects_the_setting_at_runtime():
    """Toggling ``ui.live_overlay`` takes effect without a restart."""
    settings = Settings()
    assert settings.ui.live_overlay is True
    app = SimpleNamespace(settings=settings)
    manager = OverlayManager(app)          # no window: show/hide are no-ops
    manager.handle(state("recording"))
    assert manager.visible
    settings.update({"ui": {"live_overlay": False}})
    manager.apply_settings()
    assert not manager.visible
    manager.handle(state("recording"))
    assert not manager.visible
    manager.destroy()


def test_live_overlay_setting_round_trips():
    settings = Settings.from_dict({"ui": {"live_overlay": False}})
    assert settings.ui.live_overlay is False
    assert Settings().to_dict()["ui"]["live_overlay"] is True


# ----------------------------------------------------------------- position --


def test_position_is_bottom_centre_above_the_taskbar():
    # 1920x1080 screen, 40 px taskbar at the bottom.
    x, y = overlay_position((0, 0, 1920, 1040), 560, 110, 90)
    assert x == (1920 - 560) // 2
    assert y == 1040 - 90 - 110


def test_position_honours_an_offset_work_area():
    # Taskbar on the left (work area starts at x=60) and a scaled pill.
    x, y = overlay_position((60, 0, 2560, 1440), 840, 165, 135)
    assert x == 60 + (2500 - 840) // 2
    assert y == 1440 - 135 - 165


def test_position_is_clamped_on_a_tiny_work_area():
    # Pill wider than the screen and margin too big: pinned to the top-left
    # of the work area rather than pushed off it.
    assert overlay_position((0, 0, 400, 150), 560, 110, 90) == (0, 0)
    # Margin too big for the height, but the pill fits: stays inside.
    assert overlay_position((0, 0, 1000, 200), 560, 110, 150) == (220, 0)


# --------------------------------------------------------------------- size --

AREA_1080 = (0, 0, 1920, 1040)       # 1920x1080 with a 40 px taskbar


def test_size_width_is_720_capped_by_60_percent_of_the_work_area():
    w, _ = overlay_size(AREA_1080, 100)
    assert w == OVERLAY_MAX_WIDTH == 720
    # A narrow work area: 60% of 1000 = 600 < 720.
    w, _ = overlay_size((0, 0, 1000, 800), 100)
    assert w == 600
    # 150% scale: 720 logical = 1080 physical, still under 60% of 2560.
    w, _ = overlay_size((0, 0, 2560, 1380), 100, 1.5)
    assert w == 1080


def test_size_grows_with_content_between_one_line_and_40_percent():
    _, h = overlay_size(AREA_1080, None)
    assert h == OVERLAY_MIN_HEIGHT                       # nothing reported yet
    _, h = overlay_size(AREA_1080, 10)
    assert h == OVERLAY_MIN_HEIGHT                       # never below one line
    _, h = overlay_size(AREA_1080, 200)
    assert h == 200                                      # grows with the text
    _, h = overlay_size(AREA_1080, 5000)
    assert h == int(1040 * 0.4) == 416                   # then caps; page scrolls


def test_size_scales_content_and_cap_by_dpi():
    area = (0, 0, 2880, 1740)                            # 4K-ish at 150%
    _, h = overlay_size(area, 200, 1.5)
    assert h == 300
    _, h = overlay_size(area, 10_000, 1.5)
    assert h == int(1740 * 0.4)
    _, h = overlay_size(area, None, 1.5)
    assert h == round(OVERLAY_MIN_HEIGHT * 1.5)


def test_growing_keeps_the_bottom_edge_anchored_above_the_taskbar():
    bottoms = set()
    for content in (None, 48, 120, 260, 900):
        w, h = overlay_size(AREA_1080, content)
        x, y = overlay_position(AREA_1080, w, h, 90)
        bottoms.add(y + h)
        assert x == (1920 - w) // 2
        assert y >= 0
    assert bottoms == {1040 - 90}


def test_size_survives_a_tiny_work_area():
    w, h = overlay_size((0, 0, 300, 100), 5000)
    assert w == 180 and h == 40                          # 60% / 40%, no crash
    assert overlay_size((0, 0, 0, 0), 100) == (1, 1)


def test_manager_accepts_only_sane_heights():
    manager = OverlayManager(SimpleNamespace(settings=Settings()))
    assert manager.set_content_height(312.5) is True
    assert manager._content_height == 312.5
    for bad in (None, "tall", float("nan"), -4, 0, 1e9):
        assert manager.set_content_height(bad) is False
    assert manager._content_height == 312.5
    manager.handle(state("recording"))                   # new session: one line
    assert manager._content_height is None
    manager.destroy()


def test_js_api_exposes_only_report_height():
    manager = OverlayManager(SimpleNamespace(settings=Settings()))
    public = [n for n in dir(manager._api) if not n.startswith("_")]
    assert public == ["report_height"]
    assert manager._api.report_height(150) is True
    assert manager._content_height == 150.0
    manager.destroy()


def test_long_smoke_transcript_is_about_200_words():
    transcript = SMOKE_LONG_EVENTS[-1]["transcript"]
    words = sum(len((s["clean"] or s["raw"]).split()) for s in transcript["sentences"])
    words += len(transcript["pending"].split()) + len(transcript["tentative"].split())
    assert 170 <= words <= 240


# -------------------------------------------------------------------- misc --


def test_supported_kwargs_filters_unknown_names():
    def create_window(title, url=None, focus=True, on_top=False):
        return None

    kept = supported_kwargs(create_window, {"focus": False, "focusable": False,
                                            "on_top": True})
    assert kept == {"focus": False, "on_top": True}


def test_overlay_html_defines_fethr_event():
    text = OVERLAY_HTML.read_text(encoding="utf-8")
    assert re.search(r"window\.fethrEvent\s*=\s*function", text)
    assert "<script>" in text and "</script>" in text
    assert text.count("<style>") == 1 and "</style>" in text
    for needle in ("tentative", "pending", "clean", "recording"):
        assert needle in text
    assert text.count("{") == text.count("}")


def test_overlay_html_wraps_grows_and_scrolls():
    text = OVERLAY_HTML.read_text(encoding="utf-8")
    assert "overflow-wrap: anywhere" in text          # URLs break
    assert "overflow-x: hidden" in text               # never scrolls sideways
    assert "report_height" in text                    # asks Python to grow
    assert "ResizeObserver" in text
    assert re.search(r"RESIZE_DEBOUNCE_MS\s*=\s*80\b", text)
    assert "font: 15px/1.45" in text
