#!/usr/bin/env python3
"""Controller-first ScoreTracker viewer for Batocera's Ports menu."""

from __future__ import annotations

import json
import os
import queue
import subprocess
import threading
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any

import pygame


API_URL = os.environ.get("SCORETRACKER_API_URL", "http://127.0.0.1:8080").rstrip("/")
WINDOWED = os.environ.get("SCORETRACKER_WINDOWED") == "1"
VPINBALL_ROOT = Path(os.environ.get("SCORETRACKER_VPINBALL_ROOT", "/userdata/roms/vpinball"))
UPDATER = Path(
    os.environ.get(
        "SCORETRACKER_UPDATER",
        "/userdata/system/scoretracker/update-scoretracker.sh",
    )
)

BG = (18, 20, 25)
PANEL = (31, 34, 42)
PANEL_ALT = (38, 42, 52)
WHITE = (244, 246, 250)
MUTED = (155, 162, 176)
FAINT = (105, 112, 127)
BLUE = (71, 112, 255)
BLUE_DARK = (43, 69, 151)
GREEN = (70, 190, 119)
RED = (231, 94, 88)
LINE = (60, 65, 78)


@dataclass
class ScoreEntry:
    score: int
    date: str
    duration: int | None
    signed: bool


@dataclass
class TableHistory:
    key: str
    name: str
    rom: str
    entries: list[ScoreEntry]
    games: int
    best: int
    latest: ScoreEntry
    total_time: int
    vpx_file_name: str | None
    media_path: Path | None = None


def parse_date(value: str) -> float:
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
    except (TypeError, ValueError):
        return 0


def format_date(value: str, short: bool = False) -> str:
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except (TypeError, ValueError):
        return value or "Unknown date"
    return parsed.strftime("%d %b" if short else "%d %b %Y  %H:%M")


def format_duration(seconds: int | None) -> str:
    if seconds is None:
        return "--"
    hours, remainder = divmod(max(0, seconds), 3600)
    minutes = remainder // 60
    return f"{hours}h {minutes:02d}m" if hours else f"{minutes}m"


def compact_score(score: int) -> str:
    if score >= 1_000_000_000:
        return f"{score / 1_000_000_000:.1f}B"
    if score >= 1_000_000:
        return f"{score / 1_000_000:.1f}M"
    if score >= 1_000:
        return f"{score / 1_000:.0f}K"
    return str(score)


def build_tables(snapshot: dict[str, Any]) -> list[TableHistory]:
    groups: dict[str, list[dict[str, Any]]] = {}
    for game in snapshot.get("games", []):
        scores = [int(score) for score in game.get("scores", []) if int(score) > 0]
        if not scores:
            continue
        key = str(game.get("rom") or game.get("table") or "Unknown")
        game = {**game, "scores": scores}
        groups.setdefault(key, []).append(game)

    result: list[TableHistory] = []
    for key, games in groups.items():
        games.sort(key=lambda game: parse_date(str(game.get("date", ""))))
        entries = [
            ScoreEntry(
                score=score,
                date=str(game.get("date", "")),
                duration=game.get("gameDuration"),
                signed=bool(game.get("signed")),
            )
            for game in games
            for score in game["scores"]
        ]
        latest_game = games[-1]
        result.append(
            TableHistory(
                key=key,
                name=str(latest_game.get("table") or key),
                rom=str(latest_game.get("rom") or key),
                entries=entries,
                games=len(games),
                best=max(entry.score for entry in entries),
                latest=entries[-1],
                total_time=sum(int(game.get("gameDuration") or 0) for game in games),
                vpx_file_name=latest_game.get("vpxFileName"),
            )
        )
    return sorted(result, key=lambda table: parse_date(table.latest.date), reverse=True)


class LocalMediaLibrary:
    """Resolves Batocera-managed wheel-like artwork without network access."""

    MEDIA_TAGS = ("marquee", "thumbnail", "image")
    MEDIA_SUFFIXES = ("wheel", "marquee", "logo")
    EXTENSIONS = ("png", "jpg", "jpeg", "webp")

    def __init__(self, root: Path):
        self.root = root
        self.gamelist_media: dict[str, Path] = {}
        self._load_gamelist()

    @staticmethod
    def _keys(value: str) -> set[str]:
        path = Path(value)
        return {path.name.casefold(), path.stem.casefold()}

    def _resolve_path(self, value: str) -> Path:
        value = os.path.expanduser(value)
        path = Path(value)
        if not path.is_absolute():
            path = self.root / value.removeprefix("./")
        return path

    def _load_gamelist(self) -> None:
        gamelist = self.root / "gamelist.xml"
        if not gamelist.is_file():
            return
        try:
            document = ET.parse(gamelist)
        except (ET.ParseError, OSError):
            return
        for game in document.findall(".//game"):
            game_path = (game.findtext("path") or "").strip()
            if not game_path:
                continue
            media: Path | None = None
            for tag in self.MEDIA_TAGS:
                value = (game.findtext(tag) or "").strip()
                if not value:
                    continue
                candidate = self._resolve_path(value)
                # A marquee is Batocera's logo slot. For image/thumbnail, only
                # accept explicitly wheel-like filenames so screenshots do not
                # become poor substitutes for transparent table logos.
                wheel_like = any(token in candidate.stem.casefold() for token in self.MEDIA_SUFFIXES)
                if candidate.is_file() and (tag == "marquee" or wheel_like):
                    media = candidate
                    break
            if media:
                for key in self._keys(game_path):
                    self.gamelist_media[key] = media

    def resolve(self, table: TableHistory) -> Path | None:
        identities = [value for value in (table.vpx_file_name, table.name, table.key) if value]
        for identity in identities:
            for key in self._keys(identity):
                media = self.gamelist_media.get(key)
                if media:
                    return media

        stems = {Path(identity).stem for identity in identities}
        media_roots = (
            self.root / "images",
            Path("/userdata/system/configs/emulationstation/downloaded_images/vpinball"),
            Path.home() / ".emulationstation/downloaded_images/vpinball",
        )
        for media_root in media_roots:
            for stem in stems:
                for suffix in self.MEDIA_SUFFIXES:
                    for extension in self.EXTENSIONS:
                        candidate = media_root / f"{stem}-{suffix}.{extension}"
                        if candidate.is_file():
                            return candidate
        return None


class Fonts:
    def __init__(self, scale: float):
        regular_path = next(
            (
                path
                for path in (
                    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
                    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                    "/usr/share/fonts/TTF/DejaVuSans.ttf",
                )
                if os.path.isfile(path)
            ),
            None,
        )
        bold_path = next(
            (
                path
                for path in (
                    "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
                    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                )
                if os.path.isfile(path)
            ),
            regular_path,
        )

        def font(size: int, bold: bool = False) -> pygame.font.Font:
            result = pygame.font.Font(bold_path if bold else regular_path, max(12, round(size * scale)))
            if bold and bold_path == regular_path:
                result.set_bold(True)
            return result

        self.hero = font(44, True)
        self.title = font(30, True)
        self.heading = font(22, True)
        self.body = font(18)
        self.body_bold = font(18, True)
        self.small = font(14)
        self.small_bold = font(14, True)
        self.tiny = font(12, True)


class ScoreTrackerApp:
    def __init__(self) -> None:
        pygame.display.init()
        pygame.font.init()
        pygame.joystick.init()
        flags = pygame.RESIZABLE if WINDOWED else pygame.FULLSCREEN
        requested = (1280, 720) if WINDOWED else (0, 0)
        self.screen = pygame.display.set_mode(requested, flags)
        pygame.display.set_caption("VPX ScoreTracker")
        width, _ = self.screen.get_size()
        self.scale = max(0.72, min(1.35, width / 1920))
        self.fonts = Fonts(self.scale)
        self.clock = pygame.time.Clock()
        self.running = True
        self.loading = False
        self.error = ""
        self.snapshot: dict[str, Any] = {}
        self.tables: list[TableHistory] = []
        self.selected = 0
        self.detail: TableHistory | None = None
        self.detail_row = 0
        self.media_library = LocalMediaLibrary(VPINBALL_ROOT)
        self.media_cache: dict[tuple[str, int, int], pygame.Surface | None] = {}
        self.result_queue: queue.Queue[tuple[dict[str, Any] | None, str]] = queue.Queue()
        self.update_queue: queue.Queue[tuple[str, str]] = queue.Queue()
        self.update_loading = False
        self.update_version = ""
        self.update_message = ""
        self.update_confirm = False
        self.updating = False
        self.axis_ready = {0: True, 1: True}
        self.refresh()
        self.check_for_update()

    def px(self, value: int) -> int:
        return round(value * self.scale)

    def refresh(self) -> None:
        if self.loading or self.updating:
            return
        self.loading = True
        self.error = ""

        def load() -> None:
            try:
                request = urllib.request.Request(
                    f"{API_URL}/api/scores", headers={"Accept": "application/json"}
                )
                with urllib.request.urlopen(request, timeout=30) as response:
                    data = json.load(response)
                self.result_queue.put((data, ""))
            except (OSError, ValueError, urllib.error.URLError) as exc:
                self.result_queue.put((None, f"Could not load scores: {exc}"))

        threading.Thread(target=load, daemon=True).start()

    def accept_results(self) -> None:
        try:
            data, error = self.result_queue.get_nowait()
        except queue.Empty:
            return
        self.loading = False
        self.error = error
        if data is not None:
            current_key = self.detail.key if self.detail else None
            self.snapshot = data
            self.tables = build_tables(data)
            for table in self.tables:
                table.media_path = self.media_library.resolve(table)
            self.selected = min(self.selected, max(0, len(self.tables) - 1))
            if current_key:
                self.detail = next((table for table in self.tables if table.key == current_key), None)

    def check_for_update(self) -> None:
        if self.update_loading or self.updating or not UPDATER.is_file():
            return
        self.update_loading = True

        def check() -> None:
            try:
                result = subprocess.run(
                    [str(UPDATER), "--check"],
                    capture_output=True,
                    text=True,
                    timeout=45,
                    check=False,
                )
                if result.returncode != 0:
                    self.update_queue.put(("error", result.stderr.strip()))
                    return
                available = next(
                    (
                        line.partition("=")[2]
                        for line in result.stdout.splitlines()
                        if line.startswith("UPDATE_AVAILABLE=")
                    ),
                    "",
                )
                self.update_queue.put(("available" if available else "current", available))
            except (OSError, subprocess.SubprocessError) as exc:
                self.update_queue.put(("error", str(exc)))

        threading.Thread(target=check, daemon=True).start()

    def request_update(self) -> None:
        if self.detail or self.updating or not self.update_version:
            return
        self.update_confirm = True

    def install_update(self) -> None:
        if self.updating or not self.update_version:
            return
        target_version = self.update_version
        self.update_confirm = False
        self.updating = True
        self.update_message = f"Installing ScoreTracker {target_version}..."

        def install() -> None:
            try:
                result = subprocess.run(
                    [str(UPDATER), "--install"],
                    capture_output=True,
                    text=True,
                    timeout=600,
                    check=False,
                )
                message = (result.stderr if result.returncode else result.stdout).strip()
                self.update_queue.put(("installed" if result.returncode == 0 else "install_error", message))
            except (OSError, subprocess.SubprocessError) as exc:
                self.update_queue.put(("install_error", str(exc)))

        threading.Thread(target=install, daemon=True).start()

    def accept_update_results(self) -> None:
        try:
            state, message = self.update_queue.get_nowait()
        except queue.Empty:
            return
        self.update_loading = False
        if state == "available":
            self.update_version = message
            self.update_message = f"ScoreTracker {message} is available. Press Y or U to update."
        elif state == "installed":
            installed_version = self.update_version
            self.updating = False
            self.update_version = ""
            self.update_message = f"Updated to ScoreTracker {installed_version}. Restart the viewer to load its new UI."
            self.refresh()
        elif state == "install_error":
            self.updating = False
            detail = message.splitlines()[-1] if message else "unknown error"
            self.update_message = f"Update failed: {detail}"
        elif state == "current":
            self.update_version = ""
        # A failed background check should not obscure score or NVRAM warnings.

    def move(self, amount: int) -> None:
        if self.update_confirm or self.updating:
            return
        if self.detail:
            self.detail_row = max(0, min(len(self.detail.entries) - 1, self.detail_row + amount))
        elif self.tables:
            self.selected = max(0, min(len(self.tables) - 1, self.selected + amount))

    def activate(self) -> None:
        if self.updating:
            return
        if self.update_confirm:
            self.install_update()
        elif not self.detail and self.tables:
            self.detail = self.tables[self.selected]
            self.detail_row = 0

    def back(self) -> None:
        if self.updating:
            return
        if self.update_confirm:
            self.update_confirm = False
        elif self.detail:
            self.detail = None
        else:
            self.running = False

    def handle_button(self, button: int) -> None:
        if self.updating:
            return
        if button == 0:
            self.activate()
        elif button in (1, 2, 6):
            self.back()
        elif button == 3:
            self.request_update()
        elif button in (4, 9):
            self.refresh()

    def handle_event(self, event: pygame.event.Event) -> None:
        if event.type == pygame.QUIT:
            self.running = False
        elif event.type == pygame.KEYDOWN:
            if event.key in (pygame.K_DOWN, pygame.K_s):
                self.move(1)
            elif event.key in (pygame.K_UP, pygame.K_w):
                self.move(-1)
            elif event.key in (pygame.K_RETURN, pygame.K_SPACE):
                self.activate()
            elif event.key in (pygame.K_ESCAPE, pygame.K_BACKSPACE):
                self.back()
            elif event.key in (pygame.K_r, pygame.K_F5):
                self.refresh()
            elif event.key == pygame.K_u:
                self.request_update()
        elif event.type == pygame.JOYHATMOTION:
            if event.value[1] > 0:
                self.move(-1)
            elif event.value[1] < 0:
                self.move(1)
        elif event.type == pygame.JOYBUTTONDOWN:
            self.handle_button(event.button)
        elif event.type == pygame.JOYAXISMOTION and event.axis in (0, 1):
            value = event.value
            if abs(value) < 0.45:
                self.axis_ready[event.axis] = True
            elif self.axis_ready[event.axis]:
                if event.axis == 1:
                    self.move(1 if value > 0 else -1)
                self.axis_ready[event.axis] = False

    def text(
        self,
        value: str,
        font: pygame.font.Font,
        color: tuple[int, int, int],
        x: int,
        y: int,
        anchor: str = "topleft",
    ) -> pygame.Rect:
        surface = font.render(value, True, color)
        rect = surface.get_rect(**{anchor: (x, y)})
        self.screen.blit(surface, rect)
        return rect

    def clipped_text(
        self,
        value: str,
        font: pygame.font.Font,
        color: tuple[int, int, int],
        rect: pygame.Rect,
    ) -> None:
        candidate = value
        while candidate and font.size(candidate + ("..." if candidate != value else ""))[0] > rect.width:
            candidate = candidate[:-1]
        if candidate != value:
            candidate = candidate.rstrip() + "..."
        self.text(candidate, font, color, rect.x, rect.y)

    def draw_header(self, subtitle: str) -> int:
        width, _ = self.screen.get_size()
        height = self.px(88)
        pygame.draw.rect(self.screen, (10, 11, 14), (0, 0, width, height))
        pygame.draw.rect(self.screen, BLUE, (0, height - self.px(4), width, self.px(4)))
        left = self.px(54)
        top = self.px(22)
        pygame.draw.rect(self.screen, WHITE, (left, top, self.px(7), self.px(43)))
        pygame.draw.rect(self.screen, BLUE, (left + self.px(13), top, self.px(7), self.px(43)))
        self.text("VPX SCORETRACKER", self.fonts.heading, WHITE, left + self.px(35), top - self.px(1))
        self.text(subtitle, self.fonts.small, MUTED, left + self.px(35), top + self.px(29))
        status = "UPDATING..." if self.updating else ("REFRESHING..." if self.loading else "LOCAL SCORES")
        self.text(status, self.fonts.small_bold, BLUE if self.loading else MUTED, width - left, top + self.px(14), "topright")
        return height

    def draw_footer(self) -> None:
        width, height = self.screen.get_size()
        footer_h = self.px(54)
        y = height - footer_h
        pygame.draw.rect(self.screen, (10, 11, 14), (0, y, width, footer_h))
        pygame.draw.line(self.screen, LINE, (0, y), (width, y), 1)
        left = self.px(54)
        if self.update_confirm:
            hint = "A / ENTER  INSTALL UPDATE     B / ESC  CANCEL"
        else:
            hint = "A / ENTER  OPEN     B / ESC  {}     LB / R  REFRESH".format(
                "BACK" if self.detail else "EXIT"
            )
            if self.update_version and not self.detail:
                hint += "     Y / U  UPDATE"
        self.text(hint, self.fonts.small_bold, MUTED, left, y + self.px(18))
        self.text("SCORETRACKER PYBROWSER", self.fonts.tiny, FAINT, width - left, y + self.px(20), "topright")

    def draw_notice(self, message: str, color: tuple[int, int, int]) -> None:
        width, height = self.screen.get_size()
        rect = pygame.Rect(self.px(54), height - self.px(118), width - self.px(108), self.px(48))
        pygame.draw.rect(self.screen, PANEL_ALT, rect, border_radius=self.px(4))
        pygame.draw.rect(self.screen, color, (rect.x, rect.y, self.px(5), rect.height))
        self.clipped_text(message, self.fonts.small_bold, WHITE, rect.inflate(-self.px(30), -self.px(14)))

    def draw_media(self, table: TableHistory, rect: pygame.Rect) -> bool:
        if not table.media_path:
            return False
        key = (str(table.media_path), rect.width, rect.height)
        if key not in self.media_cache:
            try:
                source = pygame.image.load(str(table.media_path)).convert_alpha()
                ratio = min(rect.width / source.get_width(), rect.height / source.get_height())
                size = (
                    max(1, round(source.get_width() * ratio)),
                    max(1, round(source.get_height() * ratio)),
                )
                self.media_cache[key] = pygame.transform.smoothscale(source, size)
            except (OSError, pygame.error):
                self.media_cache[key] = None
        media = self.media_cache[key]
        if media is None:
            return False
        target = media.get_rect(center=rect.center)
        self.screen.blit(media, target)
        return True

    def draw_overview(self) -> None:
        width, height = self.screen.get_size()
        header_h = self.draw_header("BATOCERA PORTS VIEWER")
        margin = self.px(54)
        content_top = header_h + self.px(34)
        self.text("Recently played", self.fonts.hero, WHITE, margin, content_top)
        games = len(self.snapshot.get("games", []))
        summary = f"{games:,} completed games   /   {len(self.tables):,} played tables"
        self.text(summary, self.fonts.body, MUTED, margin, content_top + self.px(57))

        list_top = content_top + self.px(105)
        list_bottom = height - self.px(78)
        row_h = self.px(92)
        visible = max(1, (list_bottom - list_top) // row_h)
        start = max(0, min(self.selected - visible // 2, len(self.tables) - visible))

        if not self.tables and not self.loading:
            self.text("No recorded scores were found.", self.fonts.title, MUTED, margin, list_top + self.px(40))

        for screen_row, index in enumerate(range(start, min(len(self.tables), start + visible))):
            table = self.tables[index]
            y = list_top + screen_row * row_h
            rect = pygame.Rect(margin, y, width - margin * 2, row_h - self.px(10))
            selected = index == self.selected
            pygame.draw.rect(self.screen, PANEL_ALT if selected else PANEL, rect, border_radius=self.px(4))
            pygame.draw.rect(self.screen, BLUE if selected else LINE, rect, self.px(3) if selected else 1, border_radius=self.px(4))
            if selected:
                pygame.draw.rect(self.screen, BLUE, (rect.x, rect.y, self.px(7), rect.height))
            media_rect = pygame.Rect(rect.x + self.px(18), rect.y + self.px(10), self.px(118), rect.height - self.px(20))
            has_media = self.draw_media(table, media_rect)
            left = rect.x + self.px(153 if has_media else 26)
            self.clipped_text(table.name, self.fonts.heading, WHITE, pygame.Rect(left, rect.y + self.px(14), rect.width * 0.46, self.px(28)))
            self.text(f"ROM / {table.rom}", self.fonts.small, MUTED, left, rect.y + self.px(48))
            latest_x = width - margin - self.px(460)
            self.text("LATEST", self.fonts.tiny, FAINT, latest_x, rect.y + self.px(15))
            self.text(f"{table.latest.score:,}", self.fonts.body_bold, BLUE, latest_x, rect.y + self.px(39))
            best_x = width - margin - self.px(245)
            self.text("PERSONAL BEST", self.fonts.tiny, FAINT, best_x, rect.y + self.px(15))
            self.text(f"{table.best:,}", self.fonts.body_bold, WHITE, best_x, rect.y + self.px(39))
            self.text(format_date(table.latest.date, True), self.fonts.small, MUTED, rect.right - self.px(23), rect.y + self.px(33), "topright")

        warnings = self.snapshot.get("warnings", [])
        if self.update_confirm:
            self.draw_notice(f"Install ScoreTracker {self.update_version}? Press A to confirm or B to cancel.", BLUE)
        elif self.update_message:
            self.draw_notice(self.update_message, RED if self.update_message.startswith("Update failed") else BLUE)
        elif self.error:
            self.draw_notice(self.error, RED)
        elif warnings:
            self.draw_notice(f"{len(warnings)} score source warning(s). Scores shown may be incomplete.", (224, 170, 73))
        self.draw_footer()

    def draw_score_chart(self, table: TableHistory, rect: pygame.Rect) -> None:
        pygame.draw.rect(self.screen, PANEL, rect, border_radius=self.px(4))
        pygame.draw.rect(self.screen, LINE, rect, 1, border_radius=self.px(4))
        self.text("SCORE PROGRESS", self.fonts.small_bold, MUTED, rect.x + self.px(20), rect.y + self.px(17))

        max_points = 20
        selected_chrono = len(table.entries) - 1 - self.detail_row
        start = max(0, min(selected_chrono - max_points // 2, len(table.entries) - max_points))
        entries = table.entries[start : start + max_points]
        selected_local = selected_chrono - start
        plot = pygame.Rect(
            rect.x + self.px(66),
            rect.y + self.px(56),
            rect.width - self.px(88),
            rect.height - self.px(94),
        )
        maximum = max(entry.score for entry in entries)
        ceiling = max(1, round(maximum * 1.12))

        for tick in range(4):
            ratio = tick / 3
            y = round(plot.bottom - ratio * plot.height)
            pygame.draw.line(self.screen, LINE, (plot.x, y), (plot.right, y), 1)
            self.text(
                compact_score(round(ceiling * ratio)),
                self.fonts.tiny,
                FAINT,
                plot.x - self.px(10),
                y,
                "midright",
            )

        if len(entries) == 1:
            points = [(plot.centerx, round(plot.bottom - entries[0].score / ceiling * plot.height))]
        else:
            points = [
                (
                    round(plot.x + index * plot.width / (len(entries) - 1)),
                    round(plot.bottom - entry.score / ceiling * plot.height),
                )
                for index, entry in enumerate(entries)
            ]

        fill_surface = pygame.Surface(rect.size, pygame.SRCALPHA)
        relative = [(x - rect.x, y - rect.y) for x, y in points]
        fill_points = [(relative[0][0], plot.bottom - rect.y), *relative, (relative[-1][0], plot.bottom - rect.y)]
        pygame.draw.polygon(fill_surface, (*BLUE, 30), fill_points)
        self.screen.blit(fill_surface, rect.topleft)
        if len(points) > 1:
            pygame.draw.aalines(self.screen, BLUE, False, points)
            pygame.draw.lines(self.screen, BLUE, False, points, self.px(3))

        for index, point in enumerate(points):
            selected = index == selected_local
            pygame.draw.circle(self.screen, WHITE if selected else PANEL, point, self.px(7 if selected else 5))
            pygame.draw.circle(self.screen, BLUE, point, self.px(7 if selected else 5), self.px(3))

        selected_entry = entries[selected_local]
        selected_point = points[selected_local]
        label = f"{selected_entry.score:,}"
        label_w = self.fonts.small_bold.size(label)[0] + self.px(18)
        label_x = max(plot.x, min(selected_point[0] - label_w // 2, plot.right - label_w))
        label_y = max(plot.y, selected_point[1] - self.px(38))
        label_rect = pygame.Rect(label_x, label_y, label_w, self.px(28))
        pygame.draw.rect(self.screen, BLUE_DARK, label_rect, border_radius=self.px(3))
        self.text(label, self.fonts.small_bold, WHITE, label_rect.centerx, label_rect.y + self.px(6), "midtop")

        self.text(format_date(entries[0].date, True), self.fonts.tiny, FAINT, plot.x, plot.bottom + self.px(12))
        self.text(format_date(entries[-1].date, True), self.fonts.tiny, FAINT, plot.right, plot.bottom + self.px(12), "topright")

    def draw_detail(self) -> None:
        assert self.detail is not None
        table = self.detail
        width, height = self.screen.get_size()
        header_h = self.draw_header(f"TABLE HISTORY  /  {table.rom}")
        margin = self.px(54)
        top = header_h + self.px(30)
        hero_media = pygame.Rect(margin, top - self.px(4), self.px(250), self.px(68))
        has_media = self.draw_media(table, hero_media)
        title_left = margin + (self.px(275) if has_media else 0)
        self.clipped_text(table.name, self.fonts.hero, WHITE, pygame.Rect(title_left, top, width - title_left - margin - self.px(420), self.px(60)))
        self.text("PERSONAL BEST", self.fonts.tiny, FAINT, width - margin, top + self.px(3), "topright")
        self.text(f"{table.best:,}", self.fonts.title, BLUE, width - margin, top + self.px(25), "topright")

        stat_top = top + self.px(76)
        stat_w = (width - margin * 2 - self.px(24)) // 3
        stats = [
            ("RECORDED SCORES", f"{len(table.entries):,}"),
            ("PLAYED GAMES", f"{table.games:,}"),
            ("RECORDED TIME", format_duration(table.total_time)),
        ]
        for index, (label, value) in enumerate(stats):
            rect = pygame.Rect(margin + index * (stat_w + self.px(12)), stat_top, stat_w, self.px(78))
            pygame.draw.rect(self.screen, PANEL, rect, border_radius=self.px(4))
            self.text(label, self.fonts.tiny, FAINT, rect.x + self.px(18), rect.y + self.px(14))
            self.text(value, self.fonts.heading, WHITE, rect.x + self.px(18), rect.y + self.px(38))

        content_top = stat_top + self.px(102)
        content_bottom = height - self.px(72)
        content_width = width - margin * 2
        gap = self.px(16)
        chart_width = round(content_width * 0.62)
        chart_rect = pygame.Rect(margin, content_top, chart_width, content_bottom - content_top)
        self.draw_score_chart(table, chart_rect)

        list_left = chart_rect.right + gap
        list_width = width - margin - list_left
        self.text("SCORE HISTORY", self.fonts.small_bold, MUTED, list_left, content_top)
        list_top = content_top + self.px(32)
        list_bottom = content_bottom
        row_h = self.px(64)
        newest = list(reversed(table.entries))
        selected_newest = self.detail_row
        visible = max(1, (list_bottom - list_top) // row_h)
        start = max(0, min(selected_newest - visible // 2, len(newest) - visible))
        for screen_row, index in enumerate(range(start, min(len(newest), start + visible))):
            entry = newest[index]
            y = list_top + screen_row * row_h
            rect = pygame.Rect(list_left, y, list_width, row_h - self.px(7))
            selected = index == selected_newest
            pygame.draw.rect(self.screen, PANEL_ALT if selected else PANEL, rect, border_radius=self.px(3))
            if selected:
                pygame.draw.rect(self.screen, BLUE, rect, self.px(2), border_radius=self.px(3))
            score_rect = self.text(f"{entry.score:,}", self.fonts.body_bold, WHITE, rect.x + self.px(15), rect.y + self.px(9))
            self.text(format_duration(entry.duration), self.fonts.tiny, FAINT, rect.x + self.px(15), rect.y + self.px(35))
            if entry.signed:
                badge = pygame.Rect(score_rect.right + self.px(10), rect.y + self.px(8), self.px(68), self.px(24))
                pygame.draw.rect(self.screen, (33, 77, 53), badge, border_radius=self.px(14))
                self.text("SIGNED", self.fonts.tiny, GREEN, badge.centerx, badge.y + self.px(5), "midtop")
            self.text(format_date(entry.date, True), self.fonts.small, MUTED, rect.right - self.px(15), rect.y + self.px(20), "topright")

        if self.error:
            self.draw_notice(self.error, RED)
        self.draw_footer()

    def draw(self) -> None:
        self.screen.fill(BG)
        if self.detail:
            self.draw_detail()
        else:
            self.draw_overview()
        pygame.display.flip()

    def run(self) -> None:
        while self.running:
            for event in pygame.event.get():
                self.handle_event(event)
            self.accept_results()
            self.accept_update_results()
            self.draw()
            self.clock.tick(60)
        pygame.quit()


if __name__ == "__main__":
    ScoreTrackerApp().run()
