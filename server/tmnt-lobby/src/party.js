// Anonymous party snapshots (README.md, Party snapshots): which turtles make up an online party, kept only as counts per
// day. A character is the game's own actor index; nothing here names a player, an account or a party.
import { MAX_PARTY } from './protocol.js';

// Must match ACTOR_LEONARDO..ACTOR_DONATELLO of the game's Turtle_Const.h, and kTurtleCount in src/online/party_report.h.
export const PARTY_CHARACTERS = ['Leonardo', 'Michelangelo', 'Raphael', 'Donatello'];

const FIELDS = new Set(['op', 'seq', 'characters', 'host']);
const isCharacter = (v) => Number.isInteger(v) && v >= 0 && v < PARTY_CHARACTERS.length;

// Every multiset of 1 to MAX_PARTY characters, in canonical order: the whole space a composition can take.
function compositions() {
  const found = [];
  const extend = (names, from) => {
    if (names.length) found.push(names.join('+'));
    if (names.length === MAX_PARTY) return;
    for (let c = from; c < PARTY_CHARACTERS.length; c++) extend([...names, PARTY_CHARACTERS[c]], c);
  };
  extend([], 0);
  return found;
}

export const PARTY_COMPOSITIONS = compositions();

// What each party dimension may hold: a client never supplies a value, only character indexes.
export const PARTY_VALUES = {
  character: new Set(PARTY_CHARACTERS),
  host_character: new Set(PARTY_CHARACTERS),
  party_size: new Set(Array.from({ length: MAX_PARTY }, (_, i) => String(i + 1))),
  party_composition: new Set(PARTY_COMPOSITIONS),
  party_duplicate: new Set(['yes', 'no']),
};

export const PARTY_DIMENSIONS = Object.keys(PARTY_VALUES);
export const PARTY_ROWS_PER_DAY = PARTY_DIMENSIONS.reduce((n, d) => n + PARTY_VALUES[d].size, 0);

// { characters: ascending indexes, host: an index or null } from a party.report request, or null when anything is off: an
// unknown field, a repeated key, a size outside 1 to 4, an index outside the roster, a host that is not in the party.
// `text` is the frame as received: every quoted string of a valid one is a key or the op, so a repeated key adds quotes.
export function parsePartyReport(msg, text) {
  const keys = Object.keys(msg);
  if (keys.some((k) => !FIELDS.has(k)) || !Array.isArray(msg.characters)) return null;
  if (typeof text !== 'string' || text.split('"').length - 1 !== 2 * (keys.length + 1)) return null;
  const { characters, host } = msg;
  if (characters.length < 1 || characters.length > MAX_PARTY || !characters.every(isCharacter)) return null;
  if (host !== undefined && !(isCharacter(host) && characters.includes(host))) return null;
  return { characters: [...characters].sort((a, b) => a - b), host: host ?? null };
}

// The counts one snapshot adds, as [dimension, value, amount]; throws on anything parsePartyReport would refuse.
export function partyCounts({ characters, host }) {
  if (!Array.isArray(characters) || characters.length < 1 || characters.length > MAX_PARTY || !characters.every(isCharacter) ||
      (host !== null && !(isCharacter(host) && characters.includes(host)))) {
    throw new RangeError('invalid party');
  }
  const names = [...characters].sort((a, b) => a - b).map((c) => PARTY_CHARACTERS[c]);
  const rows = [...new Set(names)].map((name) => ['character', name, names.filter((n) => n === name).length]);
  rows.push(['party_size', String(names.length), 1], ['party_composition', names.join('+'), 1],
    ['party_duplicate', new Set(names).size < names.length ? 'yes' : 'no', 1]);
  if (host !== null) rows.push(['host_character', PARTY_CHARACTERS[host], 1]);
  return rows;
}
