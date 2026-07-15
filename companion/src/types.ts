export interface GameRecord {
  date: string;
  rom: string;
  scores: number[];
  gameDuration: number | null;
  gameState: Record<string, unknown> | null;
  table: string;
  source: string;
  sourceIndex: number;
  vpxFileName: string | null;
  vpxFileHash: string | null;
}

export interface ScanWarning {
  source: string;
  message: string;
}

export interface ScanSnapshot {
  generatedAt: string;
  tablesRoot: string;
  sourcesScanned: number;
  vpxFilesFound: number;
  games: GameRecord[];
  warnings: ScanWarning[];
}

export interface MachineHighScore {
  label: string;
  shortLabel: string | null;
  initials: string;
  score: number;
}

export interface NvramDocument {
  rom: string;
  highScores: MachineHighScore[];
}
