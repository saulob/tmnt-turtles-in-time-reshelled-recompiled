// The match rooms of the development server and the tests, in process. Commands between the hub and a room are queued
// and run one at a time, in order, after the call that made them has finished, as two Durable Objects would see them.
import { MatchRoom } from '../src/room.js';
import { LocalHost, sqliteStore } from './host.js';

export class LocalRooms {
  constructor({ now = () => Date.now(), log = () => {}, timers = false } = {}) {
    this.now = now;
    this.log = log;
    this.timers = timers;
    this.rooms = new Map();
    this.queue = [];
    this.draining = false;
    this.hub = null;
  }

  // The hub's host gets `room(id, method, ...args)`; rooms report to `hub` by method name.
  install(hubHost, hub) {
    this.hub = hub;
    hubHost.room = (id, method, ...args) => this.enqueue(() => this.ensure(id).room[method](...args));
  }

  ensure(id) {
    let entry = this.rooms.get(id);
    if (!entry) {
      const host = new LocalHost(sqliteStore(':memory:'), { now: this.now, log: this.log });
      host.hub = (method, ...args) => this.enqueue(() => this.hub[method](...args));
      host.destroy = () => {
        this.rooms.delete(id);
        if (host.timer) clearTimeout(host.timer);
        queueMicrotask(() => host.sql.close());
      };
      entry = { host, room: null };
      if (this.timers) host.onAlarm = () => entry.room.alarm();
      entry.room = new MatchRoom(host, { local: true });
      this.rooms.set(id, entry);
    }
    return entry;
  }

  enqueue(fn) {
    this.queue.push(fn);
    if (this.draining) return;
    this.draining = true;
    try {
      while (this.queue.length) {
        try {
          this.queue.shift()();
        } catch (error) {
          this.log(`local: command failed: ${error?.stack ?? error}`);
        }
      }
    } finally {
      this.draining = false;
    }
  }

  // Runs the alarm of every room whose time has come (for tests that move the clock).
  tick() {
    for (const { host, room } of [...this.rooms.values()]) {
      if (host.alarmAt !== null && host.alarmAt <= this.now()) {
        host.alarmAt = null;
        room.alarm();
      }
    }
  }

  close() {
    for (const { host } of this.rooms.values()) {
      if (host.timer) clearTimeout(host.timer);
      for (const conn of host.conns) conn.close(1001, 'server stopping');
      try {
        host.sql.close();
      } catch {
        // Already closed.
      }
    }
    this.rooms.clear();
  }
}
