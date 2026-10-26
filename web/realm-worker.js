// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The script realm in a Web Worker.
//
// A page's scripts run here, in RangerDOM (webapi.js on CEr, compiled into
// evg_realm.js), on a copy of the page that lives as long as the page is
// shown. What crosses to the page is text the browser wrote and reads
// (src/RealmWire.rgr): requests in, answers out. A worker has no DOM, and
// the realm is given no network, so a script that reaches for either
// finds nothing; what it asks for goes to the browser as a message. The
// page terminates the worker when an answer takes too long.

importScripts("./evg_realm.js");

let realm = null;

self.onmessage = (e) => {
  const m = e.data;
  try {
    if (m.start || !realm) realm = new self.EvgRealm.PageRealm();
    const answer = realm.handle(m.request);
    self.postMessage({ job: m.job, ok: true, answer });
  } catch (err) {
    self.postMessage({ job: m.job, ok: false, error: String((err && err.message) || err) });
  }
};
