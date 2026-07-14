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

export interface NvramField {
  id: string;
  section: "game_state" | "high_scores";
  label: string;
  value: string | number | boolean;
  encoding: string;
  min: number | null;
  max: number | null;
  length: number;
  writable: boolean;
}

export interface MachineHighScore {
  label: string;
  shortLabel: string | null;
  initials: string;
  score: number;
  initialsFieldId: string | null;
  scoreFieldId: string | null;
}

export interface NvramDocument {
  rom: string;
  path: string;
  mapPath: string;
  platform: string;
  writable: boolean;
  writeWarning: string | null;
  checksumsValid: boolean | null;
  highScores: MachineHighScore[];
  fields: NvramField[];
}

export interface SaveResult {
  backupPath: string;
  checksumsValid: boolean | null;
}
