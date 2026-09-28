// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The script realm in a Web Worker.
//
// A page's scripts run here, in Ranger's JavaScript interpreter
// (ComponentEngine, compiled into evg_realm.js), on a copy of the page. What
// crosses to the page is text: the job in, the DOM ops out. A worker has no
// DOM, and the interpreter is given no network, so a script that tries to
// reach either finds nothing. The page terminates the worker when a job takes
// too long.

importScripts("./evg_realm.js");

self.onmessage = (e) => {
  const job = e.data;
  const started = Date.now();
  try {
    const realm = new self.EvgRealm.JsRealm();
    for (const s of job.scripts) realm.addScript(s.url, s.text);
    const ops = realm.run(job.html, job.url, job.width, job.height);
    self.postMessage({ job: job.job, ok: true, ops, summary: realm.summary() + " ms=" + (Date.now() - started) });
  } catch (err) {
    self.postMessage({ job: job.job, ok: false, error: String((err && err.message) || err) });
  }
};
