import assert from "node:assert/strict";
import test from "node:test";

import {
  identityScore,
  normalizeTableIdentity,
  pickBestVPinPlayItem,
  pickDominantVpsItem,
  selectVPinPlayItem,
  type TableIdentity,
  type VPinPlayItem,
} from "../src/media-match.ts";

const monsterBash: VPinPlayItem = {
  vpsId: "F1Skyh5h",
  filename: "Monster Bash (Williams 1998).vpx",
  vpsdb: { name: "Monster Bash", manufacturer: "Williams", year: 1998 },
};

const catacomb: VPinPlayItem = {
  vpsId: "FxvSafVN",
  name: "Catacomb",
  manufacturer: "Stern",
  year: 1981,
};

function table(overrides: Partial<TableIdentity> = {}): TableIdentity {
  return { rom: "mb_106b", name: "mb", vpxFileName: "mb.vpx", ...overrides };
}

test("regression: a short table name is not matched inside Catacomb", () => {
  assert.equal(identityScore("mb", "catacomb", ""), 0);
  assert.equal(pickBestVPinPlayItem([catacomb], table()), null);
});

test("an exact short title remains a valid match", () => {
  const exact = { vpsId: "short", name: "MB" };
  assert.equal(pickBestVPinPlayItem([exact], table({ rom: "" })), exact);
});

test("ROM lookup wins before a misleading name search", async () => {
  const calls: string[] = [];
  const result = await selectVPinPlayItem(table(), {
    byRom: async () => {
      calls.push("rom");
      return [monsterBash];
    },
    byName: async () => {
      calls.push("name");
      return [catacomb];
    },
  });

  assert.equal(result?.source, "rom");
  assert.equal(result?.item.vpsId, "F1Skyh5h");
  assert.deepEqual(calls, ["rom"]);
});

test("name lookup remains available when an exact ROM has no candidates", async () => {
  const attackFromMars = { vpsId: "afm", name: "Attack from Mars" };
  const calls: string[] = [];
  const result = await selectVPinPlayItem(
    table({ rom: "unknown", name: "Attack from Mars (Bally 1995)", vpxFileName: null }),
    {
      byRom: async () => {
        calls.push("rom");
        return [];
      },
      byName: async () => {
        calls.push("name");
        return [attackFromMars];
      },
    },
  );

  assert.equal(result?.source, "name");
  assert.equal(result?.item.vpsId, "afm");
  assert.deepEqual(calls, ["rom", "name"]);
});

test("an exact VPX filename selects the right candidate for a shared ROM", () => {
  const original = { ...monsterBash, vpsId: "original", filename: "Monster Bash.vpx" };
  const mod = { ...monsterBash, vpsId: "mod", filename: "Monster Bash VPW 1.2.vpx" };
  const selected = pickBestVPinPlayItem(
    [original, mod],
    table({ name: "mb", vpxFileName: "Monster Bash VPW 1.2.vpx" }),
  );

  assert.equal(selected?.vpsId, "mod");
});

test("dominant ROM candidate is stable when names and filenames are inconclusive", () => {
  const items = [
    { vpsId: "base", name: "Monster Bash" },
    { vpsId: "variant", name: "Monster Bash Special" },
    { vpsId: "base", name: "Monster Bash" },
  ];
  assert.equal(pickDominantVpsItem(items)?.vpsId, "base");
});

test("normal title suffixes and punctuation continue to match", () => {
  const cases = [
    ["Attack from Mars Bally 1995", "Attack from Mars"],
    ["The Addams Family Gold", "Addams Family"],
    ["AC/DC LUCI Premium", "AC DC"],
    ["Medieval Madness VPW 1 0", "Medieval Madness"],
  ];

  for (const [rawTarget, rawName] of cases) {
    const target = normalizeTableIdentity(rawTarget);
    const name = normalizeTableIdentity(rawName);
    assert.ok(identityScore(target, name, "") >= 35, `${rawTarget} should match ${rawName}`);
  }
});

test("a broad set of real-world table naming styles keeps selecting the intended title", () => {
  const cases = [
    ["24", "24"],
    ["300", "300"],
    ["AC DC LUCI Premium", "AC/DC"],
    ["Attack from Mars Bally 1995", "Attack from Mars"],
    ["Black Knight 2000 Williams 1989", "Black Knight 2000"],
    ["Creature from the Black Lagoon VPW", "Creature from the Black Lagoon"],
    ["Doctor Who Bally 1992", "Doctor Who"],
    ["Fish Tales Williams 1992", "Fish Tales"],
    ["FunHouse Williams 1990", "FunHouse"],
    ["Guns N Roses Limited Edition", "Guns N' Roses"],
    ["Indiana Jones The Pinball Adventure Williams 1993", "Indiana Jones: The Pinball Adventure"],
    ["IT Pinball Madness", "IT"],
    ["Judge Dredd Bally 1993", "Judge Dredd"],
    ["Medieval Madness VPW 1 0", "Medieval Madness"],
    ["Monster Bash Williams 1998", "Monster Bash"],
    ["No Good Gofers Williams 1997", "No Good Gofers"],
    ["Scared Stiff Bally 1996", "Scared Stiff"],
    ["Tales of the Arabian Nights Williams 1996", "Tales of the Arabian Nights"],
    ["Terminator 2 Judgment Day Chrome", "Terminator 2: Judgment Day"],
    ["The Addams Family Gold", "Addams Family"],
    ["The Machine Bride of Pin Bot Williams 1991", "The Machine: Bride of Pin Bot"],
    ["Theatre of Magic Bally 1995", "Theatre of Magic"],
    ["TRON Legacy Stern 2011", "TRON: Legacy"],
    ["Twilight Zone Bally 1993", "Twilight Zone"],
    ["Who Dunnit Bally 1995", "Who Dunnit"],
  ];

  for (const [directoryName, canonicalName] of cases) {
    const intended = { vpsId: canonicalName, name: canonicalName };
    const selected = pickBestVPinPlayItem(
      [catacomb, { vpsId: "decoy", name: "Star Wars" }, intended],
      table({ rom: "", name: directoryName, vpxFileName: null }),
    );
    assert.equal(selected, intended, `${directoryName} should select ${canonicalName}`);
  }
});

test("unrelated short fragments do not create matches", () => {
  const cases = [
    ["mb", "Catacomb"],
    ["it", "Whirlwind Limited"],
    ["af", "Safe Cracker"],
    ["mm", "Summer Mania"],
    ["t2", "Attack from Mars 2"],
  ];

  for (const [target, name] of cases) {
    assert.equal(identityScore(target, normalizeTableIdentity(name), ""), 0, `${target} must not match ${name}`);
  }
});
