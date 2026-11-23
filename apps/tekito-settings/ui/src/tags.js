// Home's tags: what each setting is about, so a few presses find it. A
// setting has the tags of the modes it works in (a setting for every mode
// has all three), and of what it is about. Every setting in the catalog
// (catalog.jsx) and the other pages Home finds is listed here, by id;
// the tags are named in i18n.js (tags).

// The tags in the order Home shows them.
export const tagOrder = [
  "japanese", "english", "mixed", "convert", "switching", "look", "list", "slips", "predict", "notation", "keys", "dictionary",
  "privacy", "apps", "ai",
];

const J = "japanese";
const E = "english";
const M = "mixed";
const ALL = [J, E, M];
const JM = [J, M];
const EM = [E, M];

export const settingTags = {
  // 一般: switching
  switchKey: [...ALL, "switching", "keys"],
  switchOrder: [...ALL, "switching"],
  altSwitches: [...ALL, "switching", "keys"],
  ctrlShift: [...ALL, "switching", "keys"],
  defaultIme: [...ALL, "switching", "apps"],
  startMode: [...ALL, "switching"],
  modePerApp: [...ALL, "switching", "apps"],
  // 一般: display
  indicator: [...ALL, "look", "switching"],
  underline: [...ALL, "look"],
  mixMarker: [M, "look"],
  mixEnglishUnderline: [M, "look"],
  // 一般: apps and the rest
  excludedApps: [...ALL, "apps"],
  companion: ["look"],
  settingsCode: [],
  language: ["look"],
  // 変換
  live: [...JM, "convert"],
  sentenceCheck: [...JM, "convert"],
  convertFromEnd: [...JM, "convert", "keys"],
  letterDelay: [...JM, "convert", "look"],
  kanjiDelay: [...JM, "convert", "look"],
  singleVowel: [M, "convert"],
  aiModel: [...JM, "convert", "ai"],
  aiGpu: [...JM, "ai"],
  prediction: [...JM, "predict", "list"],
  completion: [E, "predict", "list"],
  nextWord: [E, "predict", "list"],
  casual: [E, "list"],
  phonetic: [E, "list", "convert"],
  // 入力ミスの修正
  slipAutoApply: [...JM, "slips"],
  englishCorrection: [...EM, "slips"],
  spelling: [...EM, "slips", "notation"],
  spaceless: [...EM, "slips"],
  doubled: [E, "slips"],
  recheck: [E, "slips"],
  enterAsShown: [...ALL, "slips", "convert", "keys"],
  // 表記
  punctuation: [...JM, "notation"],
  numbers: [...JM, "notation"],
  widths: [...JM, "notation"],
  kanji: [...JM, "notation", "convert"],
  spacing: [...JM, "notation"],
  kanjiVariants: [...JM, "notation", "list"],
  period: [...EM, "notation", "keys"],
  doubleSpace: [E, "notation", "keys"],
  smart: [E, "notation"],
  // 候補一覧
  windowSize: [...ALL, "list", "look"],
  rows: [...ALL, "list", "look"],
  meanings: [...ALL, "list", "dictionary"],
  candidateWindow: [...ALL, "list", "look"],
  showTypedKeys: [...JM, "list", "look"],
  special: [...ALL, "list", "convert"],
  postal: [...JM, "list", "convert"],
  firstPageSymbol: [...JM, "list"],
  typedForms: [...JM, "list"],
  chunks: [...JM, "list", "convert"],
  // 辞書と学習
  learn: [...ALL, "dictionary"],
  secret: [...ALL, "dictionary", "privacy"],
  private: [...ALL, "dictionary", "privacy", "apps"],
  encryption: [...ALL, "dictionary", "privacy"],
  // キーと入力
  keyboard: [...ALL, "keys"],
  kanaInput: [...JM, "keys"],
  capsLock: [...JM, "keys"],
  numpad: [...JM, "keys", "notation"],
  casedWords: [J, "keys"],
  romaji: [...JM, "keys"],
  brackets: [...ALL, "keys"],
  escStepsBack: [...JM, "keys", "convert"],
  shiftEnter: [...JM, "keys", "convert"],
  punctuationCommit: [...JM, "keys", "convert"],
  keyList: [...ALL, "keys"],
  // Pages Home finds besides the settings.
  japaneseWords: [...JM, "dictionary"],
  englishWords: [...EM, "dictionary"],
  dictionaryFiles: [...ALL, "dictionary"],
  learned: [...ALL, "dictionary", "privacy"],
};
