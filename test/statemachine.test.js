'use strict';

/**
 * Tests for the display state machine.
 *
 * The pure function under test is computeDisplayStateAt() in data/index.html.
 * Both copies must be kept in sync — if you change the logic in either place,
 * update the other and re-run these tests.
 *
 * TODO: run this in GitHub Actions as part of the release workflow.
 *
 * Run locally:
 *   npm install
 *   npm test
 */

// ── Pure state machine (mirrors computeDisplayStateAt in data/index.html) ─────

function t2s(hhmm) {
  const [h, m] = hhmm.split(':').map(Number);
  return h * 3600 + m * 60;
}

function fmt(totalSecs) {
  const h = Math.floor(totalSecs / 3600);
  const m = Math.floor((totalSecs % 3600) / 60);
  const s = totalSecs % 60;
  return `${String(h).padStart(2,'0')}:${String(m).padStart(2,'0')}:${String(s).padStart(2,'0')}`;
}

function computeDisplayStateAt(sess, nowSecs, warnMinutes = 5, override = '') {
  if (override) {
    return { line1: override, line2: '', blinkMs: 0, scroll: true };
  }
  if (!sess || sess.length === 0) {
    return { line1: 'NO SESSION', line2: '', blinkMs: 0, scroll: false };
  }

  const sorted     = [...sess].sort((a, b) => t2s(a.start) - t2s(b.start));
  const firstStart = t2s(sorted[0].start);
  const lastEnd    = t2s(sorted[sorted.length - 1].end);

  if (nowSecs < firstStart)
    return { line1: 'TRACK CLOSED', line2: '', blinkMs: 0, scroll: false };
  if (nowSecs >= lastEnd)
    return { line1: 'TRACK CLOSED', line2: '', blinkMs: 0, scroll: false };

  let active = null;
  for (const s of sorted)
    if (nowSecs >= t2s(s.start) && nowSecs < t2s(s.end)) { active = s; break; }

  if (!active)
    return { line1: 'TRACK CLOSED', line2: '', blinkMs: 0, scroll: false };

  const remaining = t2s(active.lastStart) - nowSecs;
  const warnSecs  = warnMinutes * 60;

  if (remaining <= 0)
    return { line1: active.type, line2: '00:00:00', blinkMs: 1000, scroll: false };

  const timeStr = fmt(remaining);

  if (remaining <= warnSecs)
    return { line1: active.type, line2: timeStr, blinkMs: 750, scroll: false };

  return { line1: active.type, line2: timeStr, blinkMs: 0, scroll: false };
}

// ── Test fixtures ─────────────────────────────────────────────────────────────

const proto = { type: 'Prototype',     start: '09:00', lastStart: '12:00', end: '13:00' };
const urban = { type: 'Urban Concept', start: '14:00', lastStart: '16:30', end: '17:30' };

/** Seconds since midnight helper. */
function at(h, m, s = 0) { return h * 3600 + m * 60 + s; }

// ── Tests ─────────────────────────────────────────────────────────────────────

describe('computeDisplayStateAt — no sessions', () => {
  test('empty array → NO SESSION', () => {
    const r = computeDisplayStateAt([], at(10, 0));
    expect(r.line1).toBe('NO SESSION');
    expect(r.line2).toBe('');
    expect(r.blinkMs).toBe(0);
  });

  test('null → NO SESSION', () => {
    expect(computeDisplayStateAt(null, at(10, 0)).line1).toBe('NO SESSION');
  });
});

describe('computeDisplayStateAt — override', () => {
  test('override text takes priority over any session state', () => {
    const r = computeDisplayStateAt([proto], at(10, 0), 5, 'SAFETY CAR');
    expect(r.line1).toBe('SAFETY CAR');
    expect(r.line2).toBe('');
    expect(r.scroll).toBe(true);
  });

  test('override works even with no sessions', () => {
    const r = computeDisplayStateAt([], at(10, 0), 5, 'PAUSE');
    expect(r.line1).toBe('PAUSE');
  });
});

describe('computeDisplayStateAt — TRACK CLOSED', () => {
  test('one second before first session → TRACK CLOSED, no blink', () => {
    const r = computeDisplayStateAt([proto], at(8, 59, 59));
    expect(r.line1).toBe('TRACK CLOSED');
    expect(r.blinkMs).toBe(0);
  });

  test('after last session ends → TRACK CLOSED, no blink', () => {
    const r = computeDisplayStateAt([proto], at(13, 0, 0));
    expect(r.line1).toBe('TRACK CLOSED');
    expect(r.blinkMs).toBe(0);
  });

  test('gap between two sessions → TRACK CLOSED', () => {
    // proto ends 13:00, urban starts 14:00 — gap at 13:30
    const r = computeDisplayStateAt([proto, urban], at(13, 30));
    expect(r.line1).toBe('TRACK CLOSED');
  });
});

describe('computeDisplayStateAt — COUNTDOWN', () => {
  test('session open, well before last start → COUNTDOWN, no blink', () => {
    // 09:00 start, 12:00 last start; at 10:00 remaining = 2 h
    const r = computeDisplayStateAt([proto], at(10, 0));
    expect(r.line1).toBe('Prototype');
    expect(r.line2).toBe('02:00:00');
    expect(r.blinkMs).toBe(0);
  });

  test('at session open time → COUNTDOWN begins immediately', () => {
    const r = computeDisplayStateAt([proto], at(9, 0, 0));
    expect(r.line1).toBe('Prototype');
    expect(r.blinkMs).toBe(0);
  });

  test('countdown string is formatted correctly', () => {
    // At 11:58:00 → remaining = 2 min
    expect(computeDisplayStateAt([proto], at(11, 58)).line2).toBe('00:02:00');
  });

  test('single second of remaining time formats correctly', () => {
    expect(computeDisplayStateAt([proto], at(11, 59, 59)).line2).toBe('00:00:01');
  });
});

describe('computeDisplayStateAt — WARNING', () => {
  test('within warning window → WARNING (blinkMs 750)', () => {
    // warn = 5 min; at 11:56 remaining = 4 min ≤ 5 min
    const r = computeDisplayStateAt([proto], at(11, 56), 5);
    expect(r.line1).toBe('Prototype');
    expect(r.blinkMs).toBe(750);
    expect(r.line2).toBe('00:04:00');
  });

  test('exactly at warning boundary → WARNING', () => {
    // warn = 5 min; at 11:55:00 remaining = exactly 300 s
    expect(computeDisplayStateAt([proto], at(11, 55, 0), 5).blinkMs).toBe(750);
  });

  test('one second before warning boundary → COUNTDOWN', () => {
    // warn = 5 min; at 11:54:59 remaining = 301 s
    expect(computeDisplayStateAt([proto], at(11, 54, 59), 5).blinkMs).toBe(0);
  });

  test('custom warning threshold is respected', () => {
    // warn = 15 min; at 11:45 remaining = exactly 15 min → WARNING
    expect(computeDisplayStateAt([proto], at(11, 45), 15).blinkMs).toBe(750);
    // at 11:44:59 remaining = 15 min + 1 s → COUNTDOWN
    expect(computeDisplayStateAt([proto], at(11, 44, 59), 15).blinkMs).toBe(0);
  });
});

describe('computeDisplayStateAt — LAST_START', () => {
  test('exactly at last start → LAST_START, 00:00:00, blinkMs 1000', () => {
    const r = computeDisplayStateAt([proto], at(12, 0, 0));
    expect(r.line1).toBe('Prototype');
    expect(r.line2).toBe('00:00:00');
    expect(r.blinkMs).toBe(1000);
  });

  test('after last start, still before session end → LAST_START', () => {
    const r = computeDisplayStateAt([proto], at(12, 30));
    expect(r.line2).toBe('00:00:00');
    expect(r.blinkMs).toBe(1000);
  });

  test('one second before session end → still LAST_START', () => {
    const r = computeDisplayStateAt([proto], at(12, 59, 59));
    expect(r.blinkMs).toBe(1000);
  });
});

describe('computeDisplayStateAt — multi-session', () => {
  test('correct session is active when two sessions exist', () => {
    expect(computeDisplayStateAt([proto, urban], at(15, 0)).line1).toBe('Urban Concept');
    expect(computeDisplayStateAt([proto, urban], at(10, 0)).line1).toBe('Prototype');
  });

  test('unsorted input is handled correctly', () => {
    // Supply Urban first, Prototype second — result should still pick Prototype at 10:00
    const r = computeDisplayStateAt([urban, proto], at(10, 0));
    expect(r.line1).toBe('Prototype');
  });

  test('Urban Concept session type is preserved in line1', () => {
    const r = computeDisplayStateAt([urban], at(15, 0));
    expect(r.line1).toBe('Urban Concept');
  });
});
