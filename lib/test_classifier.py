"""Unit tests for Classifier — window safety classification logic."""

import pytest
from classifier import Classifier


@pytest.fixture
def clf():
    """Classifier with a small known allow/always_mask set."""
    return Classifier(
        allow={"OBS", "Terminal", "Finder"},
        always_mask={"NotificationCenter", "SecurityAgent"},
    )


# --- 1. Default deny: unknown app is unsafe ---

def test_unknown_app_is_unsafe(clf):
    assert clf.is_unsafe(owner="SomeRandomApp") is True


# --- 2. Safe list: app in allow list is safe ---

def test_safe_app_is_not_unsafe(clf):
    assert clf.is_unsafe(owner="OBS") is False
    assert clf.is_unsafe(owner="Terminal") is False
    assert clf.is_unsafe(owner="Finder") is False


# --- 3. Always-mask: app in always_mask is unsafe even if in allow list ---

def test_always_mask_overrides_safe(clf):
    # Pure always_mask entry
    assert clf.is_unsafe(owner="NotificationCenter") is True
    assert clf.is_unsafe(owner="SecurityAgent") is True


def test_always_mask_beats_allow():
    """An app in both allow AND always_mask should be unsafe."""
    clf = Classifier(
        allow={"OBS", "NotificationCenter"},
        always_mask={"NotificationCenter"},
    )
    assert clf.is_unsafe(owner="NotificationCenter") is True


# --- 4. Negative layer: always safe regardless of owner ---

def test_negative_layer_is_safe(clf):
    assert clf.is_unsafe(owner="SomeRandomApp", layer=-1) is False
    assert clf.is_unsafe(owner="NotificationCenter", layer=-1) is False
    assert clf.is_unsafe(owner="", layer=-100) is False


# --- 5. Layer > 0 from unknown app: unsafe ---

def test_overlay_from_unknown_app_is_unsafe(clf):
    assert clf.is_unsafe(owner="UnknownOverlay", layer=1) is True
    assert clf.is_unsafe(owner="UnknownOverlay", layer=5) is True


# --- 6. Layer > 0 from safe app: safe ---

def test_overlay_from_safe_app_is_safe(clf):
    assert clf.is_unsafe(owner="OBS", layer=1) is False
    assert clf.is_unsafe(owner="Terminal", layer=3) is False


# --- 7. Empty owner string: unsafe ---

def test_empty_owner_is_unsafe(clf):
    assert clf.is_unsafe(owner="") is True
    assert clf.is_unsafe(owner="", layer=0) is True
    assert clf.is_unsafe(owner="", layer=1) is True


# --- 8. update_lists: hot-reload changes classification ---

def test_update_lists_changes_classification(clf):
    # Before update: "Slack" is unknown → unsafe
    assert clf.is_unsafe(owner="Slack") is True

    clf.update_lists(
        allow={"OBS", "Terminal", "Finder", "Slack"},
        always_mask={"NotificationCenter", "SecurityAgent"},
    )
    # After update: "Slack" is now safe
    assert clf.is_unsafe(owner="Slack") is False


def test_update_lists_can_revoke_safety(clf):
    # Terminal starts as safe
    assert clf.is_unsafe(owner="Terminal") is False

    clf.update_lists(
        allow={"OBS", "Finder"},
        always_mask={"NotificationCenter", "SecurityAgent"},
    )
    # Terminal is no longer in allow → unsafe
    assert clf.is_unsafe(owner="Terminal") is True


# --- 9. Config-driven init: custom allow/always_mask sets work ---

def test_custom_init_sets():
    clf = Classifier(allow={"MyApp"}, always_mask={"BadApp"})
    assert clf.is_unsafe(owner="MyApp") is False
    assert clf.is_unsafe(owner="BadApp") is True
    assert clf.is_unsafe(owner="OtherApp") is True


def test_empty_init_sets():
    clf = Classifier(allow=set(), always_mask=set())
    # With empty allow, everything is unsafe by default deny
    assert clf.is_unsafe(owner="OBS") is True
    assert clf.is_unsafe(owner="Terminal") is True
    # Negative layer still safe
    assert clf.is_unsafe(owner="OBS", layer=-1) is False
