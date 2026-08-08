#!/usr/bin/env python3
"""Attach the packaged ScoreTracker icon to its Batocera Ports entry."""

from __future__ import annotations

import os
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def update_gamelist(gamelist: Path) -> None:
    if gamelist.exists():
        tree = ET.parse(gamelist)
        root = tree.getroot()
        if root.tag != "gameList":
            raise ValueError(f"unexpected root element: {root.tag}")
    else:
        root = ET.Element("gameList")
        tree = ET.ElementTree(root)

    game = next(
        (
            candidate
            for candidate in root.findall("game")
            if (candidate.findtext("path") or "").strip().removeprefix("./")
            == "ScoreTracker.sh"
        ),
        None,
    )
    if game is None:
        game = ET.SubElement(root, "game")
        ET.SubElement(game, "path").text = "./ScoreTracker.sh"

    name = game.find("name")
    if name is None:
        name = ET.SubElement(game, "name")
    name.text = "VPX ScoreTracker"

    for tag in ("image", "thumbnail"):
        media = game.find(tag)
        if media is None:
            media = ET.SubElement(game, tag)
        media.text = "./images/ScoreTracker.png"

    released = game.find("releasedate")
    if released is None:
        released = ET.SubElement(game, "releasedate")
    released.text = "20260101T000000"

    ET.indent(tree, space="\t")
    temporary = gamelist.with_suffix(".xml.scoretracker-tmp")
    tree.write(temporary, encoding="utf-8", xml_declaration=True)
    os.replace(temporary, gamelist)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(f"Usage: {sys.argv[0]} GAMELIST_XML")
    update_gamelist(Path(sys.argv[1]))
